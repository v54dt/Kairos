// Self-test for the run-state journal: write events -> read fills -> replay into
// accounting. Simulates a mid-session trader restart. No broker, no socket.

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "engine_logic.h"  // Accounting
#include "order_journal.h"
#include "scenario.h"
#include "test_check.h"

using namespace kairos::exec;

static std::vector<std::string> ReadLines(const std::string& path) {
  std::vector<std::string> out;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) out.push_back(line);
  return out;
}

// Minimal structural JSON-object check: an untorn audit line opens with '{',
// closes with '}', carries no raw control char, and has balanced braces/quotes
// with escapes honored. A torn/interleaved line fails at least one of these.
static bool IsWellFormedJsonObject(const std::string& l) {
  if (l.size() < 2 || l.front() != '{' || l.back() != '}') return false;
  bool in_str = false;
  int depth = 0;
  for (std::size_t i = 0; i < l.size(); ++i) {
    char c = l[i];
    if (in_str) {
      if (static_cast<unsigned char>(c) < 0x20) return false;  // raw control char
      if (c == '\\') {
        if (++i >= l.size()) return false;  // dangling escape
        continue;
      }
      if (c == '"') in_str = false;
      continue;
    }
    if (c == '"')
      in_str = true;
    else if (c == '{')
      ++depth;
    else if (c == '}' && --depth < 0)
      return false;
  }
  return !in_str && depth == 0;
}

// The shared TradingDayUtc8 / TradingDayNumUtc8 must equal the OLD inline UTC+8
// journal-day form across a timestamp matrix, incl. the Taipei-midnight (UTC
// 16:00) and UTC-midnight boundaries. Injected time_points make this host-TZ
// independent by construction (no localtime is consulted).
static void TestTradingDayUtc8() {
  using namespace std::chrono;
  auto old_str = [](system_clock::time_point tp) {
    auto utc8 = tp + hours(8);
    year_month_day ymd{floor<days>(utc8)};
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d%02u%02u", static_cast<int>(ymd.year()),
                  static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()));
    return std::string(buf);
  };
  auto utc = [](int y, unsigned m, unsigned d) {
    return system_clock::time_point{sys_days{year{y} / month{m} / day{d}}};
  };
  const std::vector<system_clock::time_point> matrix = {
      utc(2026, 7, 10) + hours(16),               // 2026-07-11 00:00 Taipei
      utc(2026, 7, 10) + hours(16) - seconds(1),  // one tick earlier -> 2026-07-10 Taipei
      utc(2026, 7, 11),                           // UTC midnight -> 2026-07-11 08:00 Taipei
      utc(2025, 12, 31) + hours(16),              // year rollover -> 2026-01-01 Taipei
      utc(2024, 2, 28) + hours(16),               // leap-year Feb -> 2024-02-29 Taipei
      utc(2026, 7, 11) + hours(12) + minutes(34),
  };
  for (const auto& tp : matrix) {
    const std::string s = TradingDayUtc8(tp);
    CHECK(s == old_str(tp));
    CHECK(TradingDayNumUtc8(tp) == std::strtol(s.c_str(), nullptr, 10));
  }
  // Boundary values pinned so a formula regression is caught outright.
  CHECK(TradingDayUtc8(utc(2026, 7, 10) + hours(16)) == "20260711");
  CHECK(TradingDayUtc8(utc(2026, 7, 10) + hours(16) - seconds(1)) == "20260710");
  CHECK(TradingDayNumUtc8(utc(2025, 12, 31) + hours(16)) == 20260101);
  CHECK(TradingDayNumUtc8(utc(2024, 2, 28) + hours(16)) == 20240229);
}

int main() {
  TestTradingDayUtc8();

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

  // Hostile id: OrderJournal::LogAck must JSON-escape the id so a quote in it can
  // never break the JSONL framing. The emitted line stays a well-formed object and
  // the id round-trips back through the same parser the TUI mirrors.
  {
    const std::string hdir = "/tmp/kairos-hostile-id-" + std::to_string(::getpid());
    const std::string hname = "2330-Buy-20260707";
    const std::string hpath = JournalPath(hdir, hname);
    std::remove(hpath.c_str());
    {
      OrderJournal j;
      CHECK(j.Open(hdir, hname));
      j.LogAck("bad\"id", false);
    }
    auto hl = ReadLines(hpath);
    CHECK(hl.size() == 1);
    CHECK(IsWellFormedJsonObject(hl[0]));
    CHECK(hl[0].find("bad\\\"id") != std::string::npos);  // escaped, not raw
    CHECK(JournalJsonStr(hl[0], "id", "") == "bad\"id");  // round-trips
    CHECK(JournalJsonInt(hl[0], "ok", -1) == 0);
    std::remove(hpath.c_str());
    ::rmdir(hdir.c_str());
  }

  // OrderJournal::AppendFill: the handle-less writer the hub uses for an orphan
  // fill round-trips through the same ReadJournalFills a trader replays with.
  {
    const std::string adir = "/tmp/kairos-append-test-" + std::to_string(::getpid());
    const std::string aname = "2330-Buy-20260707";
    const std::string apath = JournalPath(adir, aname);
    std::remove(apath.c_str());
    CHECK(OrderJournal::AppendFill(adir, aname, "k-9", 300, 58000));
    CHECK(OrderJournal::AppendFill(adir, aname, "k-9", 200, 58000));
    auto af = ReadJournalFills(apath);
    CHECK(af.size() == 2);
    CHECK(af[0].shares == 300 && af[0].price == 58000);
    CHECK(af[1].shares == 200);
    std::remove(apath.c_str());

    // Concurrent-append safety: two writers (a restarted trader + the hub) append
    // to the SAME file at once. O_APPEND + per-line fsync must produce exactly
    // 2N whole, parseable fill lines — no torn or interleaved line.
    constexpr int kN = 500;
    auto writer = [&](long price) {
      for (int i = 0; i < kN; ++i) OrderJournal::AppendFill(adir, aname, "kc", 100, price);
    };
    std::thread t1(writer, 11);
    std::thread t2(writer, 22);
    t1.join();
    t2.join();
    auto cf = ReadJournalFills(apath);
    CHECK(cf.size() == static_cast<std::size_t>(2 * kN));
    for (const auto& f : cf) CHECK(f.shares == 100 && (f.price == 11 || f.price == 22));
    std::remove(apath.c_str());
    ::rmdir(adir.c_str());
  }

  // OrderFlowJournal: the hub's per-day audit stream. Each event is exactly one
  // well-formed line in hub-orders-<day>.jsonl, parseable back to its fields.
  {
    const std::string fdir = "/tmp/kairos-flow-test-" + std::to_string(::getpid());
    const std::string fpath = JournalPath(fdir, "hub-orders-" + JournalDayUtc8());
    std::remove(fpath.c_str());

    CHECK(OrderFlowJournal::AppendSubmit(fdir, "k1-7", "k1", "2330", "Buy", "RoundLot", "TSE",
                                         "Cash", "ROD", 1000, 92500));
    CHECK(OrderFlowJournal::AppendAck(fdir, "k1-7", true, ""));
    CHECK(OrderFlowJournal::AppendFill(fdir, "k1-7", 1000, 92500, false));
    CHECK(OrderFlowJournal::AppendCancelReq(fdir, "k1-7"));
    CHECK(OrderFlowJournal::AppendCancelAck(fdir, "k1-7", false));
    CHECK(OrderFlowJournal::AppendFill(fdir, "k9-9", 300, 58000, true));

    auto lines = ReadLines(fpath);
    CHECK(lines.size() == 6);
    // submit line: every carried field round-trips
    CHECK(JournalJsonStr(lines[0], "type", "") == "submit");
    CHECK(JournalJsonStr(lines[0], "id", "") == "k1-7");
    CHECK(JournalJsonStr(lines[0], "prefix", "") == "k1");
    CHECK(JournalJsonStr(lines[0], "symbol", "") == "2330");
    CHECK(JournalJsonStr(lines[0], "side", "") == "Buy");
    CHECK(JournalJsonStr(lines[0], "board", "") == "RoundLot");
    CHECK(JournalJsonStr(lines[0], "market", "") == "TSE");
    CHECK(JournalJsonStr(lines[0], "fund", "") == "Cash");
    CHECK(JournalJsonStr(lines[0], "tif", "") == "ROD");
    CHECK(JournalJsonInt(lines[0], "shares", -1) == 1000);
    CHECK(JournalJsonInt(lines[0], "price", -1) == 92500);
    CHECK(JournalJsonInt(lines[0], "t", -1) > 0);
    // ack line
    CHECK(JournalJsonStr(lines[1], "type", "") == "ack");
    CHECK(JournalJsonInt(lines[1], "ok", -1) == 1);
    CHECK(JournalJsonStr(lines[1], "err", "x") == "");
    // routed fill: unroutable flag is 0
    CHECK(JournalJsonStr(lines[2], "type", "") == "fill");
    CHECK(JournalJsonInt(lines[2], "shares", -1) == 1000);
    CHECK(JournalJsonInt(lines[2], "unroutable", -1) == 0);
    // cancel request + cancel ack
    CHECK(JournalJsonStr(lines[3], "type", "") == "cancel_req");
    CHECK(JournalJsonStr(lines[3], "id", "") == "k1-7");
    CHECK(JournalJsonStr(lines[4], "type", "") == "cancel_ack");
    CHECK(JournalJsonInt(lines[4], "ok", -1) == 0);
    // unroutable fill is flagged 1
    CHECK(JournalJsonStr(lines[5], "type", "") == "fill");
    CHECK(JournalJsonInt(lines[5], "unroutable", -1) == 1);
    CHECK(JournalJsonInt(lines[5], "shares", -1) == 300);

    // These audit lines are NOT the run-state fill journal: they coexist under a
    // DIFFERENT file name, so a trader's ReadJournalFills never sees them.
    CHECK(fpath.find("hub-orders-") != std::string::npos);

    // Concurrent appends from two callback threads: O_APPEND + one fwrite per line
    // must yield exactly 2N whole, parseable lines with no torn/interleaved line.
    std::remove(fpath.c_str());
    constexpr int kN = 500;
    auto writer = [&](bool unroutable) {
      for (int i = 0; i < kN; ++i) OrderFlowJournal::AppendFill(fdir, "kc", 100, 11, unroutable);
    };
    std::thread t1(writer, false);
    std::thread t2(writer, true);
    t1.join();
    t2.join();
    auto cl = ReadLines(fpath);
    CHECK(cl.size() == static_cast<std::size_t>(2 * kN));
    for (const auto& l : cl) {
      CHECK(JournalJsonStr(l, "type", "") == "fill");
      CHECK(JournalJsonInt(l, "shares", -1) == 100);
      long u = JournalJsonInt(l, "unroutable", -1);
      CHECK(u == 0 || u == 1);
    }
    std::remove(fpath.c_str());
    ::rmdir(fdir.c_str());
  }

  // Oversize-line concurrency: lines longer than the 4096-byte stdio buffer used
  // to tear because fflush issued several write()s and O_APPEND is only per-write
  // atomic. A single ::write() per line is atomic at end-of-file for any length.
  // The ack `err` field is now capped, so drive the oversize length through an
  // uncapped field (the submit symbol) to stress the writer directly: 16 threads,
  // >=1000 lines each, every escaped line well past 4096 bytes; ZERO may tear.
  {
    const std::string bdir = "/tmp/kairos-flow-big-" + std::to_string(::getpid());
    const std::string bpath = JournalPath(bdir, "hub-orders-" + JournalDayUtc8());
    std::remove(bpath.c_str());

    const std::string big_symbol(5000, 'Z');  // escaped line length ~5000 > 4096
    constexpr int kThreads = 16;
    constexpr int kLines = 1000;
    auto writer = [&](int t) {
      const std::string id = "kb-" + std::to_string(t);
      for (int i = 0; i < kLines; ++i)
        OrderFlowJournal::AppendSubmit(bdir, id, "kb", big_symbol, "Buy", "RoundLot", "TSE", "Cash",
                                       "ROD", 1000, 92500);
    };
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) ts.emplace_back(writer, t);
    for (auto& th : ts) th.join();

    auto bl = ReadLines(bpath);
    CHECK(bl.size() == static_cast<std::size_t>(kThreads * kLines));
    int torn = 0;
    for (const auto& l : bl) {
      if (!IsWellFormedJsonObject(l) || JournalJsonStr(l, "symbol", "") != big_symbol ||
          l.size() <= 4096)
        ++torn;
    }
    CHECK(torn == 0);
    std::printf("oversize-concurrency: %zu lines, torn=%d\n", bl.size(), torn);
    std::remove(bpath.c_str());
    ::rmdir(bdir.c_str());
  }

  if (g_failures == 0) {
    std::printf("test_order_journal: OK\n");
    return 0;
  }
  std::printf("test_order_journal: FAILED %d check(s)\n", g_failures);
  return 1;
}
