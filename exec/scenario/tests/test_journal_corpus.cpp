#include <dirent.h>

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "order_journal.h"
#include "test_check.h"

// Cross-language golden: schema/testdata/journal_corpus.jsonl is the exact output
// of the C++ order-journal writers (OrderFlowJournal::Append* and OrderJournal::Log*),
// covering every event type, hostile-string escaping, and the err truncation cap.
// The only non-deterministic field, the clock-derived "t", is masked to 0 before
// commit and before compare. This test REGENERATES the corpus into a temp dir and
// byte-compares against the committed file (catches writer drift, the tapegen-golden
// pattern). The tui parser asserts the decoded values on the same file.
//
// journal_expected.txt is a parallel per-line table of the semantic fields the tui
// parser must reproduce (type|shares|price|ok), derived from the known event params.
//
// Regeneration: KAIROS_REGEN=1 ctest -R test_journal_corpus.

namespace {

using kairos::exec::OrderFlowJournal;
using kairos::exec::OrderJournal;

// The hostile ack `err`: newline, tab, quote, backslash, a C0 control (0x1c), CJK.
const std::string kHostileErr =
    "reject:\nline2\ttab \"q\" \\slash \x1c sep \xe5\x8f\xb0\xe7\xa9\x8d";

std::string MaskT(const std::string& line) {
  std::string out = line;
  std::size_t p = out.find("\"t\":");
  if (p == std::string::npos) return out;
  std::size_t b = p + 4;
  std::size_t e = b;
  while (e < out.size() && (std::isdigit(static_cast<unsigned char>(out[e])) || out[e] == '-')) ++e;
  out.replace(b, e - b, "0");
  return out;
}

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string FindByPrefix(const std::string& dir, const std::string& prefix) {
  DIR* d = ::opendir(dir.c_str());
  std::string found;
  for (dirent* e = ::readdir(d); e != nullptr; e = ::readdir(d)) {
    std::string n = e->d_name;
    if (n.rfind(prefix, 0) == 0) found = dir + "/" + n;
  }
  ::closedir(d);
  return found;
}

// Drive every writer in a fixed order into a fresh temp dir and return the masked,
// concatenated lines: all OrderFlowJournal events first, then OrderJournal events.
std::string Generate() {
  char tmpl[] = "/tmp/kairos-jc-XXXXXX";
  const char* dir = ::mkdtemp(tmpl);
  std::string d = dir;

  OrderFlowJournal::AppendSubmit(d, "k1-1", "PFX", "2330", "B", "0", "0", "cash", "ROD", 2000,
                                 58050);
  OrderFlowJournal::AppendAck(d, "k1-1", true, "");
  OrderFlowJournal::AppendFill(d, "k1-1", 2000, 58050, false);
  OrderFlowJournal::AppendAck(d, "k1-2", false, kHostileErr);
  OrderFlowJournal::AppendAck(d, "k1-3", false,
                              std::string(600, 'x'));  // > 512 escaped -> truncated
  OrderFlowJournal::AppendCancelReq(d, "k1-1");
  OrderFlowJournal::AppendCancelAck(d, "k1-1", true);

  {
    OrderJournal j;
    CHECK(j.Open(d, "2330-Buy-20260101"));
    j.LogAck("k2-1", true);
    j.LogFill("k2-1", 1000, 58100);
    j.LogCancel("k2-1", true);
    j.LogAck("bad\"id", false);  // hostile id -> JsonEscaped by OrderJournal (regression guard)
  }

  std::string out;
  std::ifstream flow(FindByPrefix(d, "hub-orders-"));
  std::string line;
  while (std::getline(flow, line)) out += MaskT(line) + "\n";
  std::ifstream fills(d + "/2330-Buy-20260101.jsonl");
  while (std::getline(fills, line)) out += MaskT(line) + "\n";
  return out;
}

// Every corpus line must be single-line and free of raw control bytes (all such
// bytes are escaped by the writers), which is what makes the JSONL self-framing.
void CheckNoRawControls(const std::string& corpus) {
  bool at_line_start = true;
  for (std::size_t i = 0; i < corpus.size(); ++i) {
    unsigned char c = static_cast<unsigned char>(corpus[i]);
    if (c == '\n') {
      at_line_start = true;
      continue;
    }
    CHECK(c >= 0x20);
    at_line_start = false;
  }
  (void)at_line_start;
}

}  // namespace

int main() {
  const char* path = KAIROS_JOURNAL_CORPUS_PATH;
  std::string corpus = Generate();
  CheckNoRawControls(corpus);

  if (const char* regen = std::getenv("KAIROS_REGEN"); regen != nullptr && regen[0] != '\0') {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    CHECK(out.good());
    out << corpus;
    std::printf("regenerated %s (%zu bytes)\n", path, corpus.size());
    return 0;
  }

  std::string committed = ReadFile(path);
  if (committed != corpus) {
    std::printf("FAIL  corpus drift; regenerate with KAIROS_REGEN=1\n");
    std::printf("--- committed ---\n%s\n--- regenerated ---\n%s\n", committed.c_str(),
                corpus.c_str());
    return 1;
  }
  if (g_failures != 0) std::printf("%d failures\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
