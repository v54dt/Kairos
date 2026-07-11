// Regression: order-flow audit lines are JSONL, so every event must be exactly
// one physical line of well-formed JSON. Broker reject text is untrusted and can
// carry newlines/tabs/other C0 control chars; JsonEscape must escape them so a
// value never tears an event across lines nor emits a raw control char that a
// strict JSON parser rejects.

#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "order_journal.h"

using namespace kairos::exec;

static int g_fail = 0;
#define CHECK(c)                                              \
  do {                                                        \
    if (!(c)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); \
      ++g_fail;                                               \
    }                                                         \
  } while (0)

namespace {
std::vector<std::string> ReadLines(const std::string& p) {
  std::vector<std::string> out;
  std::ifstream in(p);
  std::string l;
  while (std::getline(in, l)) out.push_back(l);
  return out;
}

// Recover a JSON string value for key, decoding the escapes JsonEscape emits.
// Returns false if the value is not a well-formed JSON string.
bool DecodeJsonStr(const std::string& line, const std::string& key, std::string* out) {
  std::string needle = "\"" + key + "\":\"";
  auto p = line.find(needle);
  if (p == std::string::npos) return false;
  p += needle.size();
  out->clear();
  for (; p < line.size(); ++p) {
    char c = line[p];
    if (static_cast<unsigned char>(c) < 0x20) return false;  // raw control char = invalid
    if (c == '"') return true;                               // closing quote
    if (c != '\\') {
      *out += c;
      continue;
    }
    if (++p >= line.size()) return false;
    switch (line[p]) {
      case '"':
        *out += '"';
        break;
      case '\\':
        *out += '\\';
        break;
      case 'b':
        *out += '\b';
        break;
      case 'f':
        *out += '\f';
        break;
      case 'n':
        *out += '\n';
        break;
      case 'r':
        *out += '\r';
        break;
      case 't':
        *out += '\t';
        break;
      case 'u': {
        if (p + 4 >= line.size()) return false;
        int v = 0;
        for (int k = 0; k < 4; ++k) {
          char h = line[++p];
          v <<= 4;
          if (h >= '0' && h <= '9')
            v += h - '0';
          else if (h >= 'a' && h <= 'f')
            v += h - 'a' + 10;
          else if (h >= 'A' && h <= 'F')
            v += h - 'A' + 10;
          else
            return false;
        }
        *out += static_cast<char>(v);
        break;
      }
      default:
        return false;
    }
  }
  return false;  // unterminated string
}
}  // namespace

int main() {
  const std::string dir = "/tmp/kairos-flow-escape-" + std::to_string(::getpid());
  const std::string path = JournalPath(dir, "hub-orders-" + JournalDayUtc8());
  std::remove(path.c_str());

  const std::string err_nl = "REJECT: risk\nline2 detail";  // newline: must not tear the line
  const std::string err_tab = "code\t42";                   // raw TAB: invalid in strict JSON
  const std::string sym_q = "23\"30";                       // embedded quote

  OrderFlowJournal::AppendAck(dir, "k1-1", false, err_nl);
  OrderFlowJournal::AppendAck(dir, "k1-2", false, err_tab);
  OrderFlowJournal::AppendSubmit(dir, "k1-3", "k1", sym_q, "Buy", "RoundLot", "TSE", "Cash", "ROD",
                                 1000, 92500);

  auto lines = ReadLines(path);
  CHECK(lines.size() == 3);  // one physical line per event

  for (const auto& l : lines) {
    for (char c : l) CHECK(static_cast<unsigned char>(c) >= 0x20);  // no raw control char
    CHECK(!l.empty() && l.front() == '{' && l.back() == '}');
  }

  if (lines.size() == 3) {
    std::string v;
    CHECK(DecodeJsonStr(lines[0], "err", &v) && v == err_nl);
    CHECK(DecodeJsonStr(lines[1], "err", &v) && v == err_tab);
    CHECK(DecodeJsonStr(lines[2], "symbol", &v) && v == sym_q);
  }

  std::remove(path.c_str());
  ::rmdir(dir.c_str());
  if (g_fail == 0) {
    std::printf("test_order_flow_escape: OK (one well-formed line per event)\n");
    return 0;
  }
  std::printf("test_order_flow_escape: %d check(s) failed\n", g_fail);
  return 1;
}
