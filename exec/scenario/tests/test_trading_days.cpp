#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "order_journal.h"
#include "session_schedule.h"
#include "test_check.h"

// Cross-language golden: schema/testdata/trading_days.txt is generated FROM the
// C++ canonical helpers (TradingDayUtc8 / TradingDayNumUtc8 / HhmmFromUs) and then
// asserted by the core (yyyymmdd_utc8 + tapegen::hhmm_from_us) and tui
// (civil_from_days) date code. Regenerate: KAIROS_REGEN=1 ctest -R test_trading_days.
//
// Columns: ts_us|yyyymmdd|hhmm  (epoch-microsecond ts -> UTC+8 date + Taipei hhmm).

namespace {

using kairos::exec::HhmmFromUs;
using kairos::exec::TradingDayNumUtc8;
using kairos::exec::TradingDayUtc8;

// The chosen coverage points. The expected columns are DERIVED here (never
// hand-typed), so the fixture always reflects what the C++ helper produces today.
const std::int64_t kCases[] = {
    0,                 // epoch (08:00 Taipei 1970-01-01)
    -1000000,          // 1s before epoch (floor toward -inf)
    1783123200000000,  // 2026-07-04 00:00 UTC
    1783094400000000,  // 2026-07-03 16:00 UTC == 2026-07-04 00:00 Taipei
    1783094399999999,  // one microsecond before Taipei midnight
    1767196800000000,  // 2025-12-31 16:00 UTC -> 2026 year rollover (Taipei)
    1709208000000000,  // leap day 2024-02-29 12:00 UTC
    4102358400000000,  // far future 2099-12-31
    1783299600000000,  // 09:00 Taipei
    1783315500000000,  // 13:25 Taipei
    1783315800000000,  // 13:30 Taipei
};

std::chrono::system_clock::time_point TpOf(std::int64_t ts_us) {
  return std::chrono::system_clock::time_point(std::chrono::microseconds(ts_us));
}

std::string Row(std::int64_t ts_us) {
  std::ostringstream os;
  os << ts_us << '|' << TradingDayUtc8(TpOf(ts_us)) << '|' << HhmmFromUs(ts_us);
  return os.str();
}

std::vector<std::string> Split(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == sep) {
      out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  out.push_back(cur);
  return out;
}

const char kHeader[] =
    "# trading_days.txt -- UTC+8 trading-date + Taipei hhmm golden (Track RF1 B).\n"
    "#\n"
    "# Contract: from an epoch-microsecond timestamp, the UTC+8 date (yyyymmdd) and\n"
    "# Taipei local hhmm. Writers/readers pinned: exec TradingDayUtc8 /\n"
    "# TradingDayNumUtc8 / HhmmFromUs (canonical), core yyyymmdd_utc8 +\n"
    "# tapegen::hhmm_from_us, tui civil_from_days. Generated FROM the exec helper.\n"
    "#\n"
    "# Regeneration: KAIROS_REGEN=1 ctest -R test_trading_days (writes this file).\n"
    "# Columns: ts_us|yyyymmdd|hhmm\n";

}  // namespace

int main() {
  const char* path = KAIROS_TRADING_DAYS_PATH;

  if (const char* regen = std::getenv("KAIROS_REGEN"); regen != nullptr && regen[0] != '\0') {
    std::ofstream out(path, std::ios::trunc);
    CHECK(out.good());
    out << kHeader;
    for (std::int64_t ts : kCases) out << Row(ts) << '\n';
    std::printf("regenerated %s\n", path);
    return 0;
  }

  std::ifstream in(path);
  CHECK(in.good());
  std::string line;
  int rows = 0;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> f = Split(line, '|');
    CHECK_EQ(static_cast<int>(f.size()), 3);
    if (f.size() != 3) continue;
    std::int64_t ts = std::stoll(f[0]);
    std::string want_day = f[1];
    int want_hhmm = std::stoi(f[2]);
    CHECK(TradingDayUtc8(TpOf(ts)) == want_day);
    CHECK_EQ(TradingDayNumUtc8(TpOf(ts)), std::stol(want_day));
    CHECK_EQ(HhmmFromUs(ts), want_hhmm);
    ++rows;
  }
  CHECK(rows >= 10);
  if (g_failures != 0) std::printf("%d failures\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
