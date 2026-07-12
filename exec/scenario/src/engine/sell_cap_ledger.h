#ifndef KAIROS_EXEC_SELL_CAP_LEDGER_H_
#define KAIROS_EXEC_SELL_CAP_LEDGER_H_

#include <string>
#include <unordered_map>

#include "enum_names.h"

namespace kairos::exec {

// B4 oversell invariant: for a SELL, filled + still-live shares must never exceed
// the held position. This ledger tracks orders dropped from the engine's resting
// slot (ack-timeout, or a re-peg cancel the broker rejected) that may still be
// live at the broker, counting them as committed until a fill or a CONFIRMED
// cancel resolves them. Every method is called under the engine's mu_; the ledger
// carries no mutex of its own.
class SellCapLedger {
 public:
  // Move the still-unfilled part of a resting order into the possibly-live ledger
  // before the engine stops tracking it. Buy side is a no-op (the cap is sell-only).
  void AbandonResting(Side side, bool resting_active, long resting_shares, long resting_filled,
                      const std::string& resting_id);
  // A confirmed cancel means the order can no longer fill: release its shares.
  // Returns true if it was tracked (so the engine can wake the sell-cap waiters).
  bool ReleaseOnCancel(const std::string& id);
  // A late fill of an abandoned (possibly-live) order: decrement its outstanding
  // shares and drop it once fully resolved.
  void OnLateFill(const std::string& id, long shares);
  // Shares still allowed to sell: position - (filled + resting-unfilled + inflight),
  // clamped at zero.
  long SellCapRemaining(long position_shares, long filled_shares, bool resting_active,
                        long resting_shares, long resting_filled) const;

 private:
  // id -> shares not yet filled that may still be live at the broker.
  std::unordered_map<std::string, long> inflight_lost_;
};

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_SELL_CAP_LEDGER_H_
