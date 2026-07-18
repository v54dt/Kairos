#include "order_journal.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include "time_util.h"

namespace kairos::exec {

namespace {
// Calendar date of `tp` shifted into fixed UTC+8 (Taipei has no DST), the common
// basis for every trading-day form below.
std::chrono::year_month_day Utc8Date(std::chrono::system_clock::time_point tp) {
  return std::chrono::year_month_day{
      std::chrono::floor<std::chrono::days>(tp + std::chrono::hours(8))};
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

// Append `c` to `out` as one JSON escape unit, escaping every character that
// would break a JSON string literal, including the C0 control chars (broker
// reject text is untrusted and may carry newlines/tabs).
void AppendJsonEscaped(std::string& out, char c) {
  static const char* kHex = "0123456789abcdef";
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

std::string JsonEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) AppendJsonEscaped(out, c);
  return out;
}

// JsonEscape but the escaped output is capped at `cap` bytes, cut only at a whole
// escape-unit boundary (never mid-escape) so the result stays valid JSON; a
// "...(truncated)" marker is appended when any input was dropped.
std::string JsonEscapeCap(const std::string& s, std::size_t cap) {
  std::string out;
  out.reserve(s.size() < cap ? s.size() : cap);
  for (char c : s) {
    std::size_t before = out.size();
    AppendJsonEscaped(out, c);
    if (out.size() > cap) {
      out.resize(before);
      out += "...(truncated)";
      return out;
    }
  }
  return out;
}

// Cap for the broker-supplied ack `err` field, measured on the ESCAPED string, so
// no single field ever approaches the page size and stresses the write below.
constexpr std::size_t kErrEscapedCap = 512;

// Append one whole line to <dir>/<name>.jsonl, creating <dir>; fsync when asked.
// The whole line goes out in a single ::write() on an O_APPEND fd: for a regular
// file that write is atomic at end-of-file, so concurrent appends never interleave
// regardless of line length. A short write is treated as a failure (return false),
// never looped — a second write() would not be contiguous and would tear the line.
bool AppendJsonlLine(const std::string& dir, const std::string& name, const std::string& line,
                     bool do_fsync) {
  MakeDirs(dir);
  int fd = ::open(JournalPath(dir, name).c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0) return false;
  ssize_t n = ::write(fd, line.data(), line.size());
  bool ok = n == static_cast<ssize_t>(line.size());
  if (ok && do_fsync) ::fsync(fd);
  ::close(fd);
  return ok;
}
}  // namespace

std::string JournalPath(const std::string& dir, const std::string& name) {
  return dir + "/" + name + ".jsonl";
}

std::string ResolveJournalDir(const std::string& toml_dir, const char* legacy_env_name,
                              bool* used_legacy) {
  if (used_legacy != nullptr) *used_legacy = false;
  if (!toml_dir.empty()) return toml_dir;
  if (const char* env = std::getenv("KAIROS_JOURNAL_DIR"); env != nullptr && env[0] != '\0')
    return env;
  if (legacy_env_name != nullptr) {
    if (const char* env = std::getenv(legacy_env_name); env != nullptr && env[0] != '\0') {
      if (used_legacy != nullptr) *used_legacy = true;
      return env;
    }
  }
  if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0')
    return std::string(home) + "/Kairos/data/journal";
  return "";
}

std::string TradingDayUtc8(std::chrono::system_clock::time_point tp) {
  std::chrono::year_month_day ymd = Utc8Date(tp);
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%04d%02u%02u", static_cast<int>(ymd.year()),
                static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()));
  return buf;
}

long TradingDayNumUtc8(std::chrono::system_clock::time_point tp) {
  std::chrono::year_month_day ymd = Utc8Date(tp);
  return static_cast<int>(ymd.year()) * 10000L + static_cast<unsigned>(ymd.month()) * 100L +
         static_cast<unsigned>(ymd.day());
}

std::string JournalDayUtc8() { return TradingDayUtc8(std::chrono::system_clock::now()); }

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
              "{\"t\":" + std::to_string(SystemNowUs()) + ",\"type\":\"submit\",\"id\":\"" +
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
              "{\"t\":" + std::to_string(SystemNowUs()) + ",\"type\":\"ack\",\"id\":\"" +
                  JsonEscape(id) + "\",\"ok\":" + (ok ? "1" : "0") + ",\"err\":\"" +
                  JsonEscapeCap(err, kErrEscapedCap) + "\"}\n",
              true);
}

bool OrderFlowJournal::AppendFill(const std::string& dir, const std::string& id, long shares,
                                  Cents price, bool unroutable) {
  return Emit(dir,
              "{\"t\":" + std::to_string(SystemNowUs()) + ",\"type\":\"fill\",\"id\":\"" +
                  JsonEscape(id) + "\",\"shares\":" + std::to_string(shares) + ",\"price\":" +
                  std::to_string(price) + ",\"unroutable\":" + (unroutable ? "1" : "0") + "}\n",
              true);
}

bool OrderFlowJournal::AppendCancelReq(const std::string& dir, const std::string& id) {
  return Emit(dir,
              "{\"t\":" + std::to_string(SystemNowUs()) + ",\"type\":\"cancel_req\",\"id\":\"" +
                  JsonEscape(id) + "\"}\n",
              false);
}

bool OrderFlowJournal::AppendForwarded(const std::string& dir, const std::string& id) {
  return Emit(dir,
              "{\"t\":" + std::to_string(SystemNowUs()) + ",\"type\":\"forwarded\",\"id\":\"" +
                  JsonEscape(id) + "\"}\n",
              false);
}

bool OrderFlowJournal::AppendCancelAck(const std::string& dir, const std::string& id, bool ok,
                                       bool withdrawn) {
  return Emit(dir,
              "{\"t\":" + std::to_string(SystemNowUs()) + ",\"type\":\"cancel_ack\",\"id\":\"" +
                  JsonEscape(id) + "\",\"ok\":" + (ok ? "1" : "0") +
                  (withdrawn ? ",\"withdrawn\":1" : "") + "}\n",
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
  Write("{\"t\":" + std::to_string(SystemNowUs()) + ",\"type\":\"fill\",\"id\":\"" +
        JsonEscape(id) + "\",\"shares\":" + std::to_string(shares) +
        ",\"price\":" + std::to_string(price) + "}\n");
}

void OrderJournal::LogAck(const std::string& id, bool ok) {
  Write("{\"t\":" + std::to_string(SystemNowUs()) + ",\"type\":\"ack\",\"id\":\"" + JsonEscape(id) +
        "\",\"ok\":" + (ok ? "1" : "0") + "}\n");
}

void OrderJournal::LogCancel(const std::string& id, bool ok) {
  Write("{\"t\":" + std::to_string(SystemNowUs()) + ",\"type\":\"cancel\",\"id\":\"" +
        JsonEscape(id) + "\",\"ok\":" + (ok ? "1" : "0") + "}\n");
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
