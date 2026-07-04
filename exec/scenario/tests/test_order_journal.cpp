// Self-test for the run-state journal: write events -> read fills -> replay into
// accounting. Simulates a mid-session trader restart. No broker, no socket.

#include <unistd.h>

#include <cstdio>
#include <string>

#include "engine_logic.h"  // Accounting
#include "order_journal.h"
#include "scenario.h"

using namespace kairos::exec;

static int g_failures = 0;

#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                \
    }                                                              \
  } while (0)

int main() {
  // field extraction from our JSONL lines
  CHECK(JournalJsonInt(R"({"type":"fill","shares":300,"price":58000})", "shares", -1) == 300);
  CHECK(JournalJsonInt(R"({"type":"fill","shares":300,"price":58000})", "price", -1) == 58000);
  CHECK(JournalJsonInt(R"({"shares":300})", "price", -7) == -7);  // absent -> default

  const std::string dir = "/tmp/kairos-journal-test-" + std::to_string(::getpid());
  const std::string name = "0050-Buy-20260707";
  const std::string path = JournalPath(dir, name);

  // A run logs its order lifecycle, then the process "crashes" (scope ends).
  {
    OrderJournal j;
    CHECK(j.Open(dir, name));
    j.LogAck("k-1", true);
    j.LogFill("k-1", 1000, 5800);
    j.LogFill("k-1", 500, 5800);
    j.LogCancel("k-2", true);
    j.LogFill("k-3", 2000, 5810);
  }

  // Restart: replay only the fills (acks/cancels ignored).
  auto fills = ReadJournalFills(path);
  CHECK(fills.size() == 3);
  long total = 0;
  for (const auto& f : fills) total += f.shares;
  CHECK(total == 3500);

  // Replaying into a fresh Accounting restores the same totals a live run had,
  // so RemainingTwd is correct and the budget is not re-bought.
  Scenario s;
  s.symbol = "0050";
  s.budget_twd = 1'000'000;
  Accounting replayed;
  for (const auto& f : fills) replayed.RecordFill(s, f.price, f.shares);
  CHECK(replayed.filled_shares == 3500);
  CHECK(replayed.RemainingTwd(s) == s.budget_twd - replayed.FilledTwd());

  // A restart appends to the same file (does not clobber the earlier fills).
  {
    OrderJournal j;
    CHECK(j.Open(dir, name));
    j.LogFill("k-4", 1000, 5820);
  }
  CHECK(ReadJournalFills(path).size() == 4);

  std::remove(path.c_str());
  ::rmdir(dir.c_str());

  if (g_failures == 0) {
    std::printf("test_order_journal: OK\n");
    return 0;
  }
  std::printf("test_order_journal: FAILED %d check(s)\n", g_failures);
  return 1;
}
