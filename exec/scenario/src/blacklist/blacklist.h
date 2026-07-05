#ifndef KAIROS_EXEC_BLACKLIST_H_
#define KAIROS_EXEC_BLACKLIST_H_

// Fail-closed startup gate: loads the F1 restricted-symbol blacklist contract
// (data/blacklist/current.csv) and refuses to trade a restricted symbol. Any
// doubt (missing/unreadable/malformed/stale file) is a REFUSE, never a pass.

#include <ctime>
#include <string>
#include <unordered_map>
#include <vector>

namespace kairos::exec {

enum class BlacklistCategory {
  kDisposal,
  kAttention,
  kSuspension,
  kMarginSuspension,
  kSellFirst,
};

const char* BlacklistCategoryName(BlacklistCategory c);
BlacklistCategory ParseBlacklistCategory(const std::string& s);  // throws on unknown

struct BlacklistEntry {
  std::string symbol;
  BlacklistCategory category;
  std::string note;
  std::string start_date;
  std::string end_date;
};

// RFC4180 reader: quoted fields, embedded commas, doubled-quote escaping,
// CRLF/LF, UTF-8 passthrough, leading BOM stripped. Throws on unterminated quote.
std::vector<std::vector<std::string>> ParseCsv(const std::string& text);

// ASCII-trim + uppercase; leading zeros preserved (no numeric coercion).
std::string NormalizeSymbol(const std::string& s);

class Blacklist {
 public:
  // Header-name-mapped parse. Missing a required column, a row with the wrong
  // field count, or a bad category value are all HARD errors (throw).
  static Blacklist Parse(const std::string& csv_text);

  std::vector<BlacklistEntry> Lookup(const std::string& symbol) const;
  size_t size() const { return count_; }
  bool empty() const { return count_ == 0; }

 private:
  std::unordered_map<std::string, std::vector<BlacklistEntry>> by_symbol_;
  size_t count_ = 0;
};

// suspension has no field: it ALWAYS blocks and cannot be disabled.
struct BlacklistConfig {
  std::string path;
  int max_stale_days = 4;
  bool block_disposal = true;
  bool block_attention = false;
  bool block_margin_suspension = true;
  bool block_sell_first = true;
};

// Precedence: config_path (if non-empty) > env KAIROS_BLACKLIST_CSV > lab default.
std::string ResolveBlacklistPath(const std::string& config_path);

bool BlacklistCategoryBlocks(const BlacklistConfig& cfg, BlacklistCategory c);

// The only bypass: deliberate double opt-in (--ignore-blacklist AND --yes).
bool BlacklistOverride(bool ignore_blacklist, bool assume_yes);

enum class BlacklistGateResult { kAllow, kRefuse };

struct BlacklistGateOutcome {
  BlacklistGateResult result = BlacklistGateResult::kRefuse;
  std::string message;
  bool has_warning = false;
};

// Fail-closed evaluation: any doubt (missing/unreadable/stale/malformed file, or
// the symbol under a blocking category) yields kRefuse with a clear reason.
BlacklistGateOutcome EvaluateBlacklistGate(const std::string& path, const BlacklistConfig& cfg,
                                           const std::string& symbol, std::time_t now);

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_BLACKLIST_H_
