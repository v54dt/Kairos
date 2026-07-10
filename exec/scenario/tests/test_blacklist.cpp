// Self-test for the blacklist parser and the fail-closed startup gate. Uses
// tempfile fixtures only; never reads the real lab current.csv.

#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>

#include "blacklist.h"

using namespace kairos::exec;

static int g_failures = 0;

#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                \
    }                                                              \
  } while (0)

static std::string TempPath(const char* tag) {
  return "/tmp/kairos-blacklist-test-" + std::to_string(getpid()) + "-" + tag + ".csv";
}

static std::string WriteTemp(const char* tag, const std::string& body) {
  std::string path = TempPath(tag);
  std::ofstream(path, std::ios::binary) << body;
  return path;
}

static std::time_t Mtime(const std::string& path) {
  struct stat st;
  ::stat(path.c_str(), &st);
  return st.st_mtime;
}

// A recent mtime so the staleness check passes by default.
static std::time_t Fresh(const std::string& path) { return Mtime(path) + 1; }

static void SetMtime(const std::string& path, std::time_t when) {
  struct timeval tv[2];
  tv[0].tv_sec = when;
  tv[0].tv_usec = 0;
  tv[1].tv_sec = when;
  tv[1].tv_usec = 0;
  ::utimes(path.c_str(), tv);
}

// Epoch for midday (04:00 UTC) of a Taipei calendar date, well clear of the
// UTC-day boundary so the +8h date arithmetic in the gate is unambiguous.
static std::time_t TaipeiMidday(int y, int m, int d) {
  struct tm tm {};
  tm.tm_year = y - 1900;
  tm.tm_mon = m - 1;
  tm.tm_mday = d;
  tm.tm_hour = 4;
  return timegm(&tm);
}

// Wrappers that pin mtime just before `now` so neither the staleness nor the
// future-mtime clock-anomaly check masks the date-window result under test.
static void RefuseAt(const std::string& path, const BlacklistConfig& cfg, const std::string& symbol,
                     std::time_t now) {
  SetMtime(path, now - 1);
  auto out = EvaluateBlacklistGate(path, cfg, symbol, now);
  CHECK(out.result == BlacklistGateResult::kRefuse);
}

static BlacklistGateOutcome AllowAt(const std::string& path, const BlacklistConfig& cfg,
                                    const std::string& symbol, std::time_t now) {
  SetMtime(path, now - 1);
  auto out = EvaluateBlacklistGate(path, cfg, symbol, now);
  CHECK(out.result == BlacklistGateResult::kAllow);
  return out;
}

static const char kHeader[] = "symbol,category,note,start_date,end_date\n";

static void TestParseCorrectness() {
  std::string csv = std::string(kHeader) +
                    "2330,disposal,\"處置股, 注意\",2026-07-01,2026-07-10\n"
                    "0050,attention,\"quote \"\"here\"\"\",2026-07-01,\n";
  Blacklist bl = Blacklist::Parse(csv);
  CHECK(bl.size() == 2);

  auto e2330 = bl.Lookup("2330");
  CHECK(e2330.size() == 1);
  CHECK(e2330[0].note == "處置股, 注意");
  CHECK(e2330[0].category == BlacklistCategory::kDisposal);
  CHECK(e2330[0].start_date == "2026-07-01");
  CHECK(e2330[0].end_date == "2026-07-10");

  auto e0050 = bl.Lookup("0050");
  CHECK(e0050.size() == 1);
  CHECK(e0050[0].note == "quote \"here\"");
  CHECK(e0050[0].end_date == "");

  // Leading zeros preserved: 0050 hits, 50 misses.
  CHECK(!bl.Lookup("0050").empty());
  CHECK(bl.Lookup("50").empty());
}

static void TestHeaderMapping() {
  // Reordered columns + an extra unknown trailing column, mapped by name.
  std::string csv =
      "note,extra,category,end_date,symbol,start_date\n"
      "\"a note\",ignored,suspension,,2317,2026-07-01\n";
  Blacklist bl = Blacklist::Parse(csv);
  auto e = bl.Lookup("2317");
  CHECK(e.size() == 1);
  CHECK(e[0].category == BlacklistCategory::kSuspension);
  CHECK(e[0].note == "a note");
  CHECK(e[0].start_date == "2026-07-01");
}

static void TestNormalization() {
  std::string csv = std::string(kHeader) + " 2330 ,suspension,x,,\n" + "abcd,disposal,y,,\n";
  Blacklist bl = Blacklist::Parse(csv);
  CHECK(!bl.Lookup("2330").empty());  // whitespace trimmed on both sides
  CHECK(!bl.Lookup("ABCD").empty());  // case-insensitive
  CHECK(!bl.Lookup("abcd").empty());
}

static void TestPolicy() {
  BlacklistConfig cfg;
  CHECK(BlacklistCategoryBlocks(cfg, BlacklistCategory::kSuspension));
  CHECK(BlacklistCategoryBlocks(cfg, BlacklistCategory::kDisposal));
  CHECK(BlacklistCategoryBlocks(cfg, BlacklistCategory::kMarginSuspension));
  CHECK(BlacklistCategoryBlocks(cfg, BlacklistCategory::kSellFirst));
  CHECK(!BlacklistCategoryBlocks(cfg, BlacklistCategory::kAttention));

  // suspension always blocks even when other categories are disabled.
  BlacklistConfig off;
  off.block_disposal = false;
  off.block_margin_suspension = false;
  off.block_sell_first = false;
  CHECK(BlacklistCategoryBlocks(off, BlacklistCategory::kSuspension));
  CHECK(!BlacklistCategoryBlocks(off, BlacklistCategory::kDisposal));
}

static void TestOverrideGuard() {
  CHECK(BlacklistOverride(true, true));
  CHECK(!BlacklistOverride(true, false));
  CHECK(!BlacklistOverride(false, true));
  CHECK(!BlacklistOverride(false, false));
}

static void GateRefuses(const std::string& path, const BlacklistConfig& cfg,
                        const std::string& symbol, std::time_t now) {
  auto out = EvaluateBlacklistGate(path, cfg, symbol, now);
  CHECK(out.result == BlacklistGateResult::kRefuse);
}

static void GateAllows(const std::string& path, const BlacklistConfig& cfg,
                       const std::string& symbol, std::time_t now) {
  auto out = EvaluateBlacklistGate(path, cfg, symbol, now);
  CHECK(out.result == BlacklistGateResult::kAllow);
}

static void TestGate() {
  BlacklistConfig cfg;

  // Each blocking category refuses.
  std::string susp = WriteTemp("susp", std::string(kHeader) + "2330,suspension,halt,,\n");
  GateRefuses(susp, cfg, "2330", Fresh(susp));
  std::string disp = WriteTemp("disp", std::string(kHeader) + "2330,disposal,d,,\n");
  GateRefuses(disp, cfg, "2330", Fresh(disp));
  std::string marg = WriteTemp("marg", std::string(kHeader) + "2330,margin_suspension,m,,\n");
  GateRefuses(marg, cfg, "2330", Fresh(marg));
  std::string sell = WriteTemp("sell", std::string(kHeader) + "2330,sell_first,s,,\n");
  GateRefuses(sell, cfg, "2330", Fresh(sell));

  // attention-only allows with a warning.
  std::string att = WriteTemp("att", std::string(kHeader) + "2330,attention,a,,\n");
  auto att_out = EvaluateBlacklistGate(att, cfg, "2330", Fresh(att));
  CHECK(att_out.result == BlacklistGateResult::kAllow);
  CHECK(att_out.has_warning);

  // symbol absent allows.
  GateAllows(susp, cfg, "1101", Fresh(susp));

  // config disables disposal => disposal allows, but suspension still refuses.
  BlacklistConfig relaxed;
  relaxed.block_disposal = false;
  GateAllows(disp, relaxed, "2330", Fresh(disp));
  GateRefuses(susp, relaxed, "2330", Fresh(susp));

  // FAIL-CLOSED: missing file.
  GateRefuses("/tmp/kairos-blacklist-does-not-exist-" + std::to_string(getpid()) + ".csv", cfg,
              "2330", 0);

  // FAIL-CLOSED: path is a directory (not a regular file).
  std::string dir = "/tmp/kairos-blacklist-dir-" + std::to_string(getpid());
  std::filesystem::create_directories(dir);
  GateRefuses(dir, cfg, "2330", 0);

  // FAIL-CLOSED: unterminated quote.
  std::string bad_quote = WriteTemp("badq", std::string(kHeader) + "2330,disposal,\"oops,,\n");
  GateRefuses(bad_quote, cfg, "2330", Fresh(bad_quote));

  // FAIL-CLOSED: header missing the category column.
  std::string bad_hdr = WriteTemp("badh", "symbol,note,start_date,end_date\n2330,x,,\n");
  GateRefuses(bad_hdr, cfg, "2330", Fresh(bad_hdr));

  // FAIL-CLOSED: a row with too few fields.
  std::string short_row = WriteTemp("short", std::string(kHeader) + "2330,disposal,x\n");
  GateRefuses(short_row, cfg, "2330", Fresh(short_row));

  // FAIL-CLOSED: an unknown category value.
  std::string bad_cat = WriteTemp("badc", std::string(kHeader) + "2330,frozen,x,,\n");
  GateRefuses(bad_cat, cfg, "2330", Fresh(bad_cat));

  // FAIL-CLOSED: a non-ASCII byte (NBSP 0xA0) in the symbol cell would store a
  // key the scenario's ASCII "2330" never matches — refuse rather than allow.
  std::string nbsp_sym = WriteTemp("nbsp", std::string(kHeader) +
                                               "\xA0"
                                               "2330,suspension,x,,\n");
  GateRefuses(nbsp_sym, cfg, "2330", Fresh(nbsp_sym));

  // FAIL-CLOSED: an unbalanced quote that re-balances swallows the next row into
  // a note field; the field count still looks right, so refuse on the embedded
  // newline rather than silently dropping the 2330 suspension row.
  std::string swallow = WriteTemp(
      "swallow", std::string(kHeader) + "1101,disposal,\"oops,2026,2026\n" +
                     "2330,suspension,halt,2026-07-01,2026-07-10\n" + "9999,disposal,\"end\",,\n");
  GateRefuses(swallow, cfg, "2330", Fresh(swallow));

  // FAIL-CLOSED: stale file (mtime older than the threshold).
  std::string stale = WriteTemp("stale", std::string(kHeader) + "1101,disposal,x,,\n");
  std::time_t stale_now = Mtime(stale) + (cfg.max_stale_days + 1) * 86400 + 1;
  GateRefuses(stale, cfg, "2330", stale_now);
  // ...and the same file within the window allows.
  GateAllows(stale, cfg, "2330", Mtime(stale) + cfg.max_stale_days * 86400);
  // FAIL-CLOSED: no sub-day slack past the threshold (age just over the limit refuses).
  GateRefuses(stale, cfg, "2330", Mtime(stale) + cfg.max_stale_days * 86400 + 1);
  GateRefuses(stale, cfg, "2330", Mtime(stale) + (cfg.max_stale_days * 86400 + 86400 / 2));
  // FAIL-CLOSED: mtime in the future (clock set back / future-dated file) refuses.
  GateRefuses(stale, cfg, "2330", Mtime(stale) - 86400);

  // empty-but-valid (header only) allows (distinct from missing).
  std::string empty = WriteTemp("empty", kHeader);
  auto empty_out = EvaluateBlacklistGate(empty, cfg, "2330", Fresh(empty));
  CHECK(empty_out.result == BlacklistGateResult::kAllow);
  CHECK(!empty_out.has_warning);

  std::filesystem::remove_all(dir);
  for (const char* t : {"susp", "disp", "marg", "sell", "att", "badq", "badh", "short", "badc",
                        "swallow", "stale", "empty"}) {
    std::remove(TempPath(t).c_str());
  }
}

static void TestGateDateWindow() {
  BlacklistConfig cfg;

  // The live 0050 case: two future-dated mid-July income-distribution mechanics.
  std::string live = WriteTemp(
      "live", std::string(kHeader) + "0050,margin_suspension,margin halt,2026-07-15,2026-07-20\n" +
                  "0050,sell_first,sell first,2026-07-16,2026-07-22\n");
  // 2026-07-06: entirely in the future => allow with a warning naming the start.
  auto pre = AllowAt(live, cfg, "0050", TaipeiMidday(2026, 7, 6));
  CHECK(pre.has_warning);
  CHECK(pre.message.find("not yet active") != std::string::npos);
  CHECK(pre.message.find("2026-07-15") != std::string::npos);
  // 2026-07-15: margin_suspension is active => refuse.
  RefuseAt(live, cfg, "0050", TaipeiMidday(2026, 7, 15));
  // 2026-07-21: margin expired but sell_first still active => refuse.
  RefuseAt(live, cfg, "0050", TaipeiMidday(2026, 7, 21));
  // 2026-07-25: both windows expired => allow, no warning.
  auto post = AllowAt(live, cfg, "0050", TaipeiMidday(2026, 7, 25));
  CHECK(!post.has_warning);

  // Single bounded window 2026-07-01..2026-07-10, boundaries inclusive.
  std::string win =
      WriteTemp("win", std::string(kHeader) + "2330,disposal,d,2026-07-01,2026-07-10\n");
  RefuseAt(win, cfg, "2330", TaipeiMidday(2026, 7, 1));                     // today == start
  RefuseAt(win, cfg, "2330", TaipeiMidday(2026, 7, 5));                     // mid window
  RefuseAt(win, cfg, "2330", TaipeiMidday(2026, 7, 10));                    // today == end
  AllowAt(win, cfg, "2330", TaipeiMidday(2026, 7, 11));                     // expired
  CHECK(AllowAt(win, cfg, "2330", TaipeiMidday(2026, 6, 30)).has_warning);  // future warns

  // Open-ended end: start in the past, empty end => active indefinitely.
  std::string oend = WriteTemp("oend", std::string(kHeader) + "2330,disposal,d,2026-07-01,\n");
  RefuseAt(oend, cfg, "2330", TaipeiMidday(2026, 7, 5));
  CHECK(AllowAt(oend, cfg, "2330", TaipeiMidday(2026, 6, 30)).has_warning);  // before start

  // Open-ended start: empty start, end in the future => active from the epoch.
  std::string ostart = WriteTemp("ostart", std::string(kHeader) + "2330,disposal,d,,2026-07-10\n");
  RefuseAt(ostart, cfg, "2330", TaipeiMidday(2026, 7, 5));
  AllowAt(ostart, cfg, "2330", TaipeiMidday(2026, 7, 11));  // past end => expired

  // Both empty: always-active suspension keeps refusing (preserves prior behavior).
  std::string blank = WriteTemp("blank", std::string(kHeader) + "2330,suspension,halt,,\n");
  RefuseAt(blank, cfg, "2330", TaipeiMidday(2026, 7, 5));
  RefuseAt(blank, cfg, "2330", TaipeiMidday(2030, 1, 1));

  // Malformed dates on a blocking category => fail-closed refuse at any date.
  for (const char* d :
       {"2026/07/15,2026-07-20", "garbage,2026-07-20", " 2026-07-15,2026-07-20",
        "2026-13-01,2026-07-20", "2026-07-15,2026-07-40", "0115-07-01,0115-07-20"}) {
    std::string body = std::string(kHeader) + "2330,disposal,d," + d + "\n";
    std::string mal = WriteTemp("mal", body);
    RefuseAt(mal, cfg, "2330", TaipeiMidday(2026, 7, 5));
    RefuseAt(mal, cfg, "2330", TaipeiMidday(2030, 1, 1));
  }

  // Inverted window start>end => malformed => fail-closed refuse.
  std::string inv =
      WriteTemp("inv", std::string(kHeader) + "2330,disposal,d,2026-07-20,2026-07-10\n");
  RefuseAt(inv, cfg, "2330", TaipeiMidday(2026, 7, 15));

  // Timezone boundary: end 2026-07-15. 15:00Z is Taipei 07-15 (refuse); 16:01Z is
  // Taipei 07-16 (allow, expired). No wall clock is read; date comes from `now`.
  std::string tz =
      WriteTemp("tz", std::string(kHeader) + "2330,disposal,d,2026-07-01,2026-07-15\n");
  struct tm t15 {};
  t15.tm_year = 2026 - 1900;
  t15.tm_mon = 6;
  t15.tm_mday = 15;
  t15.tm_hour = 15;
  RefuseAt(tz, cfg, "2330", timegm(&t15));
  struct tm t16 {};
  t16.tm_year = 2026 - 1900;
  t16.tm_mon = 6;
  t16.tm_mday = 15;
  t16.tm_hour = 16;
  t16.tm_min = 1;
  AllowAt(tz, cfg, "2330", timegm(&t16));

  // Non-blocking category (attention off) still warns regardless of window.
  BlacklistConfig att_off;  // block_attention defaults false
  std::string att =
      WriteTemp("attw", std::string(kHeader) + "2330,attention,a,2026-07-15,2026-07-20\n");
  CHECK(AllowAt(att, att_off, "2330", TaipeiMidday(2026, 7, 6)).has_warning);

  // The override guard is independent of the window: still true while in-window.
  {
    SetMtime(win, TaipeiMidday(2026, 7, 5) - 1);
    auto out = EvaluateBlacklistGate(win, cfg, "2330", TaipeiMidday(2026, 7, 5));
    CHECK(out.result == BlacklistGateResult::kRefuse);
    CHECK(BlacklistOverride(true, true));
  }

  for (const char* t : {"live", "win", "oend", "ostart", "blank", "mal", "inv", "tz", "attw"}) {
    std::remove(TempPath(t).c_str());
  }
}

int main() {
  TestParseCorrectness();
  TestHeaderMapping();
  TestNormalization();
  TestPolicy();
  TestOverrideGuard();
  TestGate();
  TestGateDateWindow();

  if (g_failures == 0) {
    std::printf("test_blacklist: OK\n");
    return 0;
  }
  std::printf("test_blacklist: FAILED %d check(s)\n", g_failures);
  return 1;
}
