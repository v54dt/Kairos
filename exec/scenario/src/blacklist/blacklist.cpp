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
    BlacklistEntry e;
    e.symbol = NormalizeSymbol(fields[i_symbol]);
    if (e.symbol.empty()) {
      throw std::runtime_error(std::format("malformed blacklist row {}: empty symbol", r));
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

  std::vector<BlacklistEntry> entries = bl.Lookup(symbol);
  std::vector<BlacklistEntry> blocking;
  std::vector<BlacklistEntry> warn;
  for (const auto& e : entries) {
    if (BlacklistCategoryBlocks(cfg, e.category)) {
      blocking.push_back(e);
    } else {
      warn.push_back(e);
    }
  }

  if (!blocking.empty()) {
    std::string msg = std::format("symbol {} is blacklisted:", NormalizeSymbol(symbol));
    for (const auto& e : blocking) msg += "\n  - " + DescribeEntry(e);
    return {BlacklistGateResult::kRefuse, msg, false};
  }
  if (!warn.empty()) {
    std::string msg = std::format("symbol {} has non-blocking restrictions (proceeding):",
                                  NormalizeSymbol(symbol));
    for (const auto& e : warn) msg += "\n  - " + DescribeEntry(e);
    return {BlacklistGateResult::kAllow, msg, true};
  }
  return {BlacklistGateResult::kAllow,
          std::format("blacklist OK ({} restrictions loaded; {} clear) from {}", bl.size(),
                      NormalizeSymbol(symbol), path),
          false};
}

}  // namespace kairos::exec
