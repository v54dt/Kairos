// Self-test for the blacklist parser and the fail-closed startup gate. Uses
// tempfile fixtures only; never reads the real lab current.csv.

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
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

int main() {
  TestParseCorrectness();
  TestHeaderMapping();
  TestNormalization();
  TestPolicy();
  TestOverrideGuard();
  TestGate();

  if (g_failures == 0) {
    std::printf("test_blacklist: OK\n");
    return 0;
  }
  std::printf("test_blacklist: FAILED %d check(s)\n", g_failures);
  return 1;
}
