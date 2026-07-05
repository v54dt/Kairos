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

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_BLACKLIST_H_
