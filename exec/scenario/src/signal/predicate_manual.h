#ifndef KAIROS_EXEC_PREDICATE_MANUAL_H_
#define KAIROS_EXEC_PREDICATE_MANUAL_H_

#include <cstdint>
#include <string>
#include <vector>

#include "predicate.h"

namespace kairos::exec {

// Operator drill injection. Watches a spool file: each appended JSON line
// {"signal":"<name>","symbol":"<sym>","action":"enter|exit"} whose signal matches
// this predicate's name and whose symbol is in its set is emitted exactly once.
// Offset-tracked (never truncates), so a concurrent appender cannot lose or
// double a line; a shrink resets the offset. The offset starts at end-of-spool,
// so a restart emits only newly appended lines and never replays history.
class ManualPredicate : public Predicate {
 public:
  ManualPredicate(std::string name, std::vector<std::string> symbols, std::string spool_path);

  std::vector<PredicateHit> Poll(std::int64_t ts_us) override;
  bool NeedsQuotes() const override { return false; }

 private:
  std::string spool_path_;
  std::uint64_t offset_ = 0;
};

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_PREDICATE_MANUAL_H_
