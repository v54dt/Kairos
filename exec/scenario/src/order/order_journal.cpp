#include "order_journal.h"

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

namespace kairos::exec {

namespace {
long NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// mkdir -p for a (typically shallow) path.
void MakeDirs(const std::string& dir) {
  std::string acc;
  for (std::size_t i = 0; i < dir.size(); ++i) {
    acc += dir[i];
    if (dir[i] == '/' && acc.size() > 1) ::mkdir(acc.c_str(), 0755);
  }
  ::mkdir(dir.c_str(), 0755);
}

// Escape every character that would break a JSON string literal, including the
// C0 control chars (broker reject text is untrusted and may carry newlines/tabs).
std::string JsonEscape(const std::string& s) {
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    unsigned char u = static_cast<unsigned char>(c);
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (u < 0x20) {
          out += "\\u00";
          out += kHex[u >> 4];
          out += kHex[u & 0xF];
        } else {
          out += c;
        }
    }
  }
  return out;
}

// Append one whole line to <dir>/<name>.jsonl, creating <dir>; fsync when asked.
// One fwrite per line under O_APPEND so interleaved appends never tear.
bool AppendJsonlLine(const std::string& dir, const std::string& name, const std::string& line,
                     bool do_fsync) {
  MakeDirs(dir);
  std::FILE* f = std::fopen(JournalPath(dir, name).c_str(), "ae");  // append, close-on-exec
  if (f == nullptr) return false;
  std::fwrite(line.data(), 1, line.size(), f);
  std::fflush(f);
  if (do_fsync) ::fsync(::fileno(f));
  std::fclose(f);
  return true;
}
}  // namespace

std::string JournalPath(const std::string& dir, const std::string& name) {
  return dir + "/" + name + ".jsonl";
}

std::string JournalDayUtc8() {
  auto utc8 = std::chrono::system_clock::now() + std::chrono::hours(8);
  std::chrono::year_month_day ymd{std::chrono::floor<std::chrono::days>(utc8)};
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%04d%02u%02u", static_cast<int>(ymd.year()),
                static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()));
  return buf;
}

bool OrderJournal::AppendFill(const std::string& dir, const std::string& name,
                              const std::string& id, long shares, Cents price) {
  OrderJournal j;
  if (!j.Open(dir, name)) return false;
  j.LogFill(id, shares, price);  // dtor fcloses
  return true;
}

bool OrderFlowJournal::Emit(const std::string& dir, const std::string& line, bool do_fsync) {
  return AppendJsonlLine(dir, "hub-orders-" + JournalDayUtc8(), line, do_fsync);
}

bool OrderFlowJournal::AppendSubmit(const std::string& dir, const std::string& id,
                                    const std::string& prefix, const std::string& symbol,
                                    const char* side, const char* board, const char* market,
                                    const std::string& funding_type,
                                    const std::string& time_in_force, long shares, Cents price) {
  return Emit(dir,
              "{\"t\":" + std::to_string(NowUs()) + ",\"type\":\"submit\",\"id\":\"" +
                  JsonEscape(id) + "\",\"prefix\":\"" + JsonEscape(prefix) + "\",\"symbol\":\"" +
                  JsonEscape(symbol) + "\",\"side\":\"" + side + "\",\"board\":\"" + board +
                  "\",\"market\":\"" + market + "\",\"fund\":\"" + JsonEscape(funding_type) +
                  "\",\"tif\":\"" + JsonEscape(time_in_force) + "\",\"shares\":" +
                  std::to_string(shares) + ",\"price\":" + std::to_string(price) + "}\n",
              false);
}

bool OrderFlowJournal::AppendAck(const std::string& dir, const std::string& id, bool ok,
                                 const std::string& err) {
  return Emit(dir,
              "{\"t\":" + std::to_string(NowUs()) + ",\"type\":\"ack\",\"id\":\"" + JsonEscape(id) +
                  "\",\"ok\":" + (ok ? "1" : "0") + ",\"err\":\"" + JsonEscape(err) + "\"}\n",
              true);
}

bool OrderFlowJournal::AppendFill(const std::string& dir, const std::string& id, long shares,
                                  Cents price, bool unroutable) {
  return Emit(dir,
              "{\"t\":" + std::to_string(NowUs()) + ",\"type\":\"fill\",\"id\":\"" +
                  JsonEscape(id) + "\",\"shares\":" + std::to_string(shares) + ",\"price\":" +
                  std::to_string(price) + ",\"unroutable\":" + (unroutable ? "1" : "0") + "}\n",
              true);
}

bool OrderFlowJournal::AppendCancelReq(const std::string& dir, const std::string& id) {
  return Emit(dir,
              "{\"t\":" + std::to_string(NowUs()) + ",\"type\":\"cancel_req\",\"id\":\"" +
                  JsonEscape(id) + "\"}\n",
              false);
}

bool OrderFlowJournal::AppendCancelAck(const std::string& dir, const std::string& id, bool ok) {
  return Emit(dir,
              "{\"t\":" + std::to_string(NowUs()) + ",\"type\":\"cancel_ack\",\"id\":\"" +
                  JsonEscape(id) + "\",\"ok\":" + (ok ? "1" : "0") + "}\n",
              true);
}

OrderJournal::~OrderJournal() {
  if (f_) std::fclose(f_);
}

bool OrderJournal::Open(const std::string& dir, const std::string& name) {
  MakeDirs(dir);
  f_ = std::fopen(JournalPath(dir, name).c_str(), "ae");  // append, close-on-exec
  return f_ != nullptr;
}

void OrderJournal::Write(const std::string& line) {
  if (!f_) return;
  std::fwrite(line.data(), 1, line.size(), f_);
  std::fflush(f_);
  ::fsync(::fileno(f_));  // a fill must survive the crash that triggers the restart
}

void OrderJournal::LogFill(const std::string& id, long shares, Cents price) {
  Write("{\"t\":" + std::to_string(NowUs()) + ",\"type\":\"fill\",\"id\":\"" + id +
        "\",\"shares\":" + std::to_string(shares) + ",\"price\":" + std::to_string(price) + "}\n");
}

void OrderJournal::LogAck(const std::string& id, bool ok) {
  Write("{\"t\":" + std::to_string(NowUs()) + ",\"type\":\"ack\",\"id\":\"" + id +
        "\",\"ok\":" + (ok ? "1" : "0") + "}\n");
}

void OrderJournal::LogCancel(const std::string& id, bool ok) {
  Write("{\"t\":" + std::to_string(NowUs()) + ",\"type\":\"cancel\",\"id\":\"" + id +
        "\",\"ok\":" + (ok ? "1" : "0") + "}\n");
}

long JournalJsonInt(const std::string& line, const std::string& key, long dflt) {
  std::string needle = "\"" + key + "\":";
  auto p = line.find(needle);
  if (p == std::string::npos) return dflt;
  p += needle.size();
  const char* start = line.c_str() + p;
  char* end = nullptr;
  long v = std::strtol(start, &end, 10);
  return end == start ? dflt : v;
}

std::string JournalJsonStr(const std::string& line, const std::string& key,
                           const std::string& dflt) {
  std::string needle = "\"" + key + "\":\"";
  auto p = line.find(needle);
  if (p == std::string::npos) return dflt;
  p += needle.size();
  std::string out;
  for (; p < line.size(); ++p) {
    char c = line[p];
    if (c == '\\' && p + 1 < line.size()) {
      out += line[++p];
    } else if (c == '"') {
      return out;
    } else {
      out += c;
    }
  }
  return dflt;  // unterminated string literal
}

std::vector<JournalFill> ReadJournalFills(const std::string& path) {
  std::vector<JournalFill> out;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    if (line.find("\"type\":\"fill\"") == std::string::npos) continue;
    JournalFill f;
    f.shares = JournalJsonInt(line, "shares", 0);
    f.price = JournalJsonInt(line, "price", 0);
    if (f.shares != 0) out.push_back(f);
  }
  return out;
}

}  // namespace kairos::exec
