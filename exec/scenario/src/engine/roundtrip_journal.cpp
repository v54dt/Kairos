#include "roundtrip_journal.h"

#include <fstream>
#include <string>

#include "order_journal.h"  // AppendJsonlLine, JournalJsonInt, JournalJsonStr

namespace kairos::exec {

namespace {

// Escape the few characters that could break a JSONL string literal. The only
// caller-supplied string is the signal name (config), kept small and identifier-like.
std::string Escape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c == '"' || c == '\\')
      out += '\\', out += c;
    else if (static_cast<unsigned char>(c) >= 0x20)
      out += c;
  }
  return out;
}

}  // namespace

bool RoundTripJournal::Arm(const std::string& dir, const std::string& name) {
  return AppendJsonlLine(dir, name, "{\"e\":\"armed\"}\n", false);
}

bool RoundTripJournal::Trigger(const std::string& dir, const std::string& name,
                               const std::string& signal, long seq, long ts_us) {
  return AppendJsonlLine(dir, name,
                         "{\"e\":\"trigger\",\"signal\":\"" + Escape(signal) + "\",\"seq\":" +
                             std::to_string(seq) + ",\"ts_us\":" + std::to_string(ts_us) + "}\n",
                         true);
}

bool RoundTripJournal::EnterDone(const std::string& dir, const std::string& name, long shares,
                                 long avg_px_cents, long ts_us) {
  return AppendJsonlLine(dir, name,
                         "{\"e\":\"enter_done\",\"sh\":" + std::to_string(shares) +
                             ",\"avg_px_c\":" + std::to_string(avg_px_cents) +
                             ",\"ts_us\":" + std::to_string(ts_us) + "}\n",
                         true);
}

bool RoundTripJournal::ExitTrigger(const std::string& dir, const std::string& name,
                                   const std::string& reason, long ts_us) {
  return AppendJsonlLine(dir, name,
                         "{\"e\":\"exit_trigger\",\"reason\":\"" + Escape(reason) +
                             "\",\"ts_us\":" + std::to_string(ts_us) + "}\n",
                         true);
}

bool RoundTripJournal::Flat(const std::string& dir, const std::string& name, long ts_us) {
  return AppendJsonlLine(dir, name, "{\"e\":\"flat\",\"ts_us\":" + std::to_string(ts_us) + "}\n",
                         true);
}

bool RoundTripJournal::Failed(const std::string& dir, const std::string& name,
                              const std::string& reason, long shares_remaining, long ts_us) {
  return AppendJsonlLine(dir, name,
                         "{\"e\":\"failed\",\"reason\":\"" + Escape(reason) +
                             "\",\"sh_remaining\":" + std::to_string(shares_remaining) +
                             ",\"ts_us\":" + std::to_string(ts_us) + "}\n",
                         true);
}

RtJournalFacts ReadRoundTripJournal(const std::string& path) {
  RtJournalFacts f;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    std::string e = JournalJsonStr(line, "e", "");
    if (e == "armed") {
      f.has_armed = true;
    } else if (e == "trigger") {
      f.has_trigger = true;
    } else if (e == "enter_done") {
      f.has_enter_done = true;
      f.enter_shares = JournalJsonInt(line, "sh", 0);
      f.enter_avg_cents = JournalJsonInt(line, "avg_px_c", 0);
      f.enter_ts_us = JournalJsonInt(line, "ts_us", 0);
    } else if (e == "flat" || e == "failed") {
      f.terminal = true;
    }
    // unknown / malformed lines are skipped (fail-tolerant read)
  }
  return f;
}

}  // namespace kairos::exec
