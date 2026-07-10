#include "blacklist.h"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <format>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace kairos::exec {

namespace {
constexpr char kDefaultBlacklistPath[] = "/home/coder/kairos-lab/data/blacklist/current.csv";
constexpr long kSecondsPerDay = 86400;
}  // namespace

const char* BlacklistCategoryName(BlacklistCategory c) {
  switch (c) {
    case BlacklistCategory::kDisposal:
      return "disposal";
    case BlacklistCategory::kAttention:
      return "attention";
    case BlacklistCategory::kSuspension:
      return "suspension";
    case BlacklistCategory::kMarginSuspension:
      return "margin_suspension";
    case BlacklistCategory::kSellFirst:
      return "sell_first";
  }
  return "?";
}

BlacklistCategory ParseBlacklistCategory(const std::string& s) {
  if (s == "disposal") return BlacklistCategory::kDisposal;
  if (s == "attention") return BlacklistCategory::kAttention;
  if (s == "suspension") return BlacklistCategory::kSuspension;
  if (s == "margin_suspension") return BlacklistCategory::kMarginSuspension;
  if (s == "sell_first") return BlacklistCategory::kSellFirst;
  throw std::runtime_error(
      std::format("unknown blacklist category '{}' "
                  "(disposal|attention|suspension|margin_suspension|sell_first)",
                  s));
}

std::vector<std::vector<std::string>> ParseCsv(const std::string& text) {
  std::vector<std::vector<std::string>> rows;
  std::vector<std::string> row;
  std::string field;
  bool in_quotes = false;
  bool field_started = false;
  bool row_started = false;

  size_t i = 0;
  const size_t n = text.size();
  // Strip a leading UTF-8 BOM so the first header cell matches by name.
  if (n >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
      static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
    i = 3;
  }

  auto end_field = [&]() {
    row.push_back(field);
    field.clear();
    field_started = false;
  };
  auto end_row = [&]() {
    end_field();
    rows.push_back(row);
    row.clear();
    row_started = false;
  };

  for (; i < n; ++i) {
    char c = text[i];
    if (in_quotes) {
      if (c == '"') {
        if (i + 1 < n && text[i + 1] == '"') {
          field.push_back('"');
          ++i;
        } else {
          in_quotes = false;
        }
      } else {
        field.push_back(c);
      }
      continue;
    }
    if (c == '"' && !field_started) {
      in_quotes = true;
      field_started = true;
      row_started = true;
      continue;
    }
    if (c == ',') {
      row_started = true;
      end_field();
      continue;
    }
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      if (row_started || !field.empty()) end_row();
      continue;
    }
    field.push_back(c);
    field_started = true;
    row_started = true;
  }
  if (in_quotes) throw std::runtime_error("malformed CSV: unterminated quoted field");
  if (row_started || !field.empty()) end_row();
  return rows;
}

std::string NormalizeSymbol(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  std::string out = s.substr(b, e - b);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return out;
}

Blacklist Blacklist::Parse(const std::string& csv_text) {
  auto rows = ParseCsv(csv_text);
  if (rows.empty()) throw std::runtime_error("malformed blacklist: no header row");

  const std::vector<std::string>& header = rows[0];
  auto col = [&](const char* name) -> int {
    for (size_t i = 0; i < header.size(); ++i) {
      if (header[i] == name) return static_cast<int>(i);
    }
    return -1;
  };
  int i_symbol = col("symbol");
  int i_category = col("category");
  int i_note = col("note");
  int i_start = col("start_date");
  int i_end = col("end_date");
  if (i_symbol < 0 || i_category < 0 || i_note < 0 || i_start < 0 || i_end < 0) {
    throw std::runtime_error(
        "malformed blacklist header: need symbol,category,note,start_date,end_date");
  }

  Blacklist bl;
  for (size_t r = 1; r < rows.size(); ++r) {
    const std::vector<std::string>& fields = rows[r];
    if (fields.size() != header.size()) {
      throw std::runtime_error(
          std::format("malformed blacklist row {}: got {} fields, header has {}", r, fields.size(),
                      header.size()));
    }
    for (const std::string& f : fields) {
      if (f.find('\n') != std::string::npos || f.find('\r') != std::string::npos) {
        throw std::runtime_error(std::format(
            "malformed blacklist row {}: embedded newline in field (corrupt or truncated file)",
            r));
      }
    }
    BlacklistEntry e;
    e.symbol = NormalizeSymbol(fields[i_symbol]);
    if (e.symbol.empty()) {
      throw std::runtime_error(std::format("malformed blacklist row {}: empty symbol", r));
    }
    // TWSE codes are ASCII alphanumeric; any other byte (NBSP, BOM, NUL,
    // full-width digit) would store a key the scenario symbol never matches,
    // silently un-blocking the row — refuse instead (fail-closed).
    for (unsigned char c : e.symbol) {
      if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'))) {
        throw std::runtime_error(std::format(
            "malformed blacklist row {}: non-alphanumeric byte in symbol (corrupt file)", r));
      }
    }
    e.category = ParseBlacklistCategory(fields[i_category]);
    e.note = fields[i_note];
    e.start_date = fields[i_start];
    e.end_date = fields[i_end];
    bl.by_symbol_[e.symbol].push_back(std::move(e));
    ++bl.count_;
  }
  return bl;
}

std::vector<BlacklistEntry> Blacklist::Lookup(const std::string& symbol) const {
  auto it = by_symbol_.find(NormalizeSymbol(symbol));
  if (it == by_symbol_.end()) return {};
  return it->second;
}

std::string ResolveBlacklistPath(const std::string& config_path) {
  if (!config_path.empty()) return config_path;
  const char* env = std::getenv("KAIROS_BLACKLIST_CSV");
  if (env != nullptr && env[0] != '\0') return env;
  return kDefaultBlacklistPath;
}

bool BlacklistCategoryBlocks(const BlacklistConfig& cfg, BlacklistCategory c) {
  switch (c) {
    case BlacklistCategory::kSuspension:
      return true;
    case BlacklistCategory::kDisposal:
      return cfg.block_disposal;
    case BlacklistCategory::kAttention:
      return cfg.block_attention;
    case BlacklistCategory::kMarginSuspension:
      return cfg.block_margin_suspension;
    case BlacklistCategory::kSellFirst:
      return cfg.block_sell_first;
  }
  return true;
}

bool BlacklistOverride(bool ignore_blacklist, bool assume_yes) {
  return ignore_blacklist && assume_yes;
}

namespace {
std::string DescribeEntry(const BlacklistEntry& e) {
  std::string dates;
  if (!e.start_date.empty() || !e.end_date.empty()) {
    dates = std::format(" [{} .. {}]", e.start_date.empty() ? "?" : e.start_date,
                        e.end_date.empty() ? "?" : e.end_date);
  }
  return std::format("{} ({}){}: {}", e.symbol, BlacklistCategoryName(e.category), dates, e.note);
}

// Strict YYYY-MM-DD; rejects slashes, ROC dates, leading space and short/long widths.
bool ParseIsoDate(const std::string& s) {
  if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
  for (int i : {0, 1, 2, 3, 5, 6, 8, 9}) {
    if (s[i] < '0' || s[i] > '9') return false;
  }
  int year = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0');
  int month = (s[5] - '0') * 10 + (s[6] - '0');
  int day = (s[8] - '0') * 10 + (s[9] - '0');
  // year >= 2000 rejects a zero-padded ROC year (0115-..) that would sort as expired
  return year >= 2000 && month >= 1 && month <= 12 && day >= 1 && day <= 31;
}

// Asia/Taipei is UTC+8 year-round (no DST); the trading day is its local date.
// Mirrors sim/session_schedule.h HhmmFromUs: shift by +8h, never read localtime.
std::string TaipeiToday(std::time_t now) {
  std::time_t shifted = now + 8 * 3600;
  struct tm tm_utc;
  gmtime_r(&shifted, &tm_utc);
  return std::format("{:04}-{:02}-{:02}", tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday);
}

enum class EntryActivity { kActive, kFuture, kExpired, kMalformed };

// Active iff (start empty OR start<=today) AND (end empty OR today<=end); an empty
// side is open-ended. Mirrors the lab fetcher covers() for populated sides, while
// treating a both-empty window as always active. Unparseable or inverted dates are
// malformed => the caller must fail closed (a garbled window may not open the gate).
EntryActivity ClassifyActivity(const BlacklistEntry& e, const std::string& today) {
  bool has_start = !e.start_date.empty();
  bool has_end = !e.end_date.empty();
  if (has_start && !ParseIsoDate(e.start_date)) return EntryActivity::kMalformed;
  if (has_end && !ParseIsoDate(e.end_date)) return EntryActivity::kMalformed;
  if (has_start && has_end && e.start_date > e.end_date) return EntryActivity::kMalformed;
  if (has_start && today < e.start_date) return EntryActivity::kFuture;
  if (has_end && today > e.end_date) return EntryActivity::kExpired;
  return EntryActivity::kActive;
}
}  // namespace

BlacklistGateOutcome EvaluateBlacklistGate(const std::string& path, const BlacklistConfig& cfg,
                                           const std::string& symbol, std::time_t now) {
  struct stat st;
  if (::stat(path.c_str(), &st) != 0) {
    return {BlacklistGateResult::kRefuse,
            std::format("blacklist file not found or inaccessible: {}", path), false};
  }
  if (!S_ISREG(st.st_mode)) {
    return {BlacklistGateResult::kRefuse,
            std::format("blacklist path is not a regular file: {}", path), false};
  }

  long age_seconds = static_cast<long>(now) - static_cast<long>(st.st_mtime);
  if (age_seconds < 0) {
    return {BlacklistGateResult::kRefuse,
            std::format("blacklist mtime is {} s in the future (clock anomaly): {}", -age_seconds,
                        path),
            false};
  }
  if (age_seconds > static_cast<long>(cfg.max_stale_days) * kSecondsPerDay) {
    long age_days = (age_seconds + kSecondsPerDay - 1) / kSecondsPerDay;
    return {BlacklistGateResult::kRefuse,
            std::format("blacklist is stale: {} days old > {} day threshold ({})", age_days,
                        cfg.max_stale_days, path),
            false};
  }

  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return {BlacklistGateResult::kRefuse, std::format("cannot read blacklist file: {}", path),
            false};
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  if (in.bad()) {
    return {BlacklistGateResult::kRefuse, std::format("error reading blacklist file: {}", path),
            false};
  }

  Blacklist bl;
  try {
    bl = Blacklist::Parse(ss.str());
  } catch (const std::exception& e) {
    return {BlacklistGateResult::kRefuse,
            std::format("blacklist parse failed ({}): {}", path, e.what()), false};
  }

  const std::string today = TaipeiToday(now);
  std::vector<std::string> refuse_lines;
  std::vector<std::string> warn_lines;
  for (const auto& e : bl.Lookup(symbol)) {
    if (!BlacklistCategoryBlocks(cfg, e.category)) {
      warn_lines.push_back(DescribeEntry(e));
      continue;
    }
    switch (ClassifyActivity(e, today)) {
      case EntryActivity::kActive:
        refuse_lines.push_back(DescribeEntry(e));
        break;
      case EntryActivity::kMalformed:
        refuse_lines.push_back(DescribeEntry(e) + " [malformed date window; refusing fail-closed]");
        break;
      case EntryActivity::kFuture:
        warn_lines.push_back(std::format("{} restriction starts {} ({}) - not yet active", e.symbol,
                                         e.start_date, BlacklistCategoryName(e.category)));
        break;
      case EntryActivity::kExpired:
        break;
    }
  }

  if (!refuse_lines.empty()) {
    std::string msg = std::format("symbol {} is blacklisted:", NormalizeSymbol(symbol));
    for (const auto& l : refuse_lines) msg += "\n  - " + l;
    return {BlacklistGateResult::kRefuse, msg, false};
  }
  if (!warn_lines.empty()) {
    std::string msg = std::format("symbol {} has non-blocking restrictions (proceeding):",
                                  NormalizeSymbol(symbol));
    for (const auto& l : warn_lines) msg += "\n  - " + l;
    return {BlacklistGateResult::kAllow, msg, true};
  }
  return {BlacklistGateResult::kAllow,
          std::format("blacklist OK ({} restrictions loaded; {} clear) from {}", bl.size(),
                      NormalizeSymbol(symbol), path),
          false};
}

}  // namespace kairos::exec
