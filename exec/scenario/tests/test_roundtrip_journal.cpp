// Self-test for the round-trip lifecycle journal: write each event -> read the
// recovery facts back; unknown/malformed lines are skipped; terminal detection;
// concurrent O_APPEND safety. No broker, no socket.

#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "order_journal.h"  // JournalPath, AppendJsonlLine
#include "roundtrip_journal.h"
#include "test_check.h"

using namespace kairos::exec;

namespace {

std::vector<std::string> ReadLines(const std::string& path) {
  std::vector<std::string> out;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) out.push_back(line);
  return out;
}

}  // namespace

int main() {
  const std::string dir = "/tmp/kairos-rt-journal-" + std::to_string(::getpid());
  const std::string name = "2330-rt-20260707";
  const std::string path = JournalPath(dir, name);
  std::remove(path.c_str());

  // A full happy-path trip: armed -> trigger -> enter_done -> exit_trigger -> flat.
  CHECK(RoundTripJournal::Arm(dir, name));
  CHECK(RoundTripJournal::Trigger(dir, name, "vwap", 1, 111));
  CHECK(RoundTripJournal::EnterDone(dir, name, 300, 58000, 222));
  CHECK(RoundTripJournal::ExitTrigger(dir, name, "stop", 333));
  CHECK(RoundTripJournal::Flat(dir, name, 444));

  auto lines = ReadLines(path);
  CHECK(lines.size() == 5);
  for (const auto& l : lines) CHECK(!l.empty() && l.front() == '{' && l.back() == '}');

  RtJournalFacts f = ReadRoundTripJournal(path);
  CHECK(f.has_armed);
  CHECK(f.has_trigger);
  CHECK(f.has_enter_done);
  CHECK(f.enter_shares == 300);
  CHECK(f.enter_avg_cents == 58000);
  CHECK(f.enter_ts_us == 222);
  CHECK(f.terminal);  // flat seen
  std::remove(path.c_str());

  // Crash before the enter_done result line: only armed + trigger persisted. The
  // reader reports has_trigger but NOT has_enter_done and NOT terminal.
  CHECK(RoundTripJournal::Arm(dir, name));
  CHECK(RoundTripJournal::Trigger(dir, name, "vwap", 1, 10));
  RtJournalFacts g = ReadRoundTripJournal(path);
  CHECK(g.has_trigger && !g.has_enter_done && !g.terminal);
  std::remove(path.c_str());

  // A failed terminal line is also terminal and carries the remaining shares.
  CHECK(RoundTripJournal::EnterDone(dir, name, 300, 58000, 5));
  CHECK(RoundTripJournal::Failed(dir, name, "exit_incomplete", 60, 6));
  RtJournalFacts h = ReadRoundTripJournal(path);
  CHECK(h.has_enter_done && h.terminal);
  std::remove(path.c_str());

  // Fail-tolerant read: a malformed line and an unknown-event line are both skipped,
  // while the well-formed enter_done between them still parses.
  CHECK(AppendJsonlLine(dir, name, "{not valid json at all\n", false));
  CHECK(AppendJsonlLine(dir, name, "{\"e\":\"heartbeat\",\"x\":1}\n", false));  // unknown
  CHECK(RoundTripJournal::EnterDone(dir, name, 500, 60000, 9));
  CHECK(AppendJsonlLine(dir, name, "\n", false));  // blank line
  RtJournalFacts m = ReadRoundTripJournal(path);
  CHECK(m.has_enter_done && m.enter_shares == 500 && m.enter_avg_cents == 60000);
  CHECK(!m.has_trigger && !m.terminal);  // the skipped lines contributed nothing
  std::remove(path.c_str());

  // Concurrent appends from two writers (a trader + a hub sharing the dir): O_APPEND
  // per line must yield exactly 2N whole, brace-balanced lines with no torn line.
  constexpr int kN = 500;
  auto writer = [&](long ts) {
    for (int i = 0; i < kN; ++i) RoundTripJournal::EnterDone(dir, name, 100, 100, ts);
  };
  std::thread t1(writer, 1);
  std::thread t2(writer, 2);
  t1.join();
  t2.join();
  auto cl = ReadLines(path);
  CHECK(cl.size() == static_cast<std::size_t>(2 * kN));
  for (const auto& l : cl) CHECK(!l.empty() && l.front() == '{' && l.back() == '}');
  std::remove(path.c_str());
  ::rmdir(dir.c_str());

  if (g_failures == 0) {
    std::printf("test_roundtrip_journal: OK\n");
    return 0;
  }
  std::printf("test_roundtrip_journal: FAILED %d check(s)\n", g_failures);
  return 1;
}
