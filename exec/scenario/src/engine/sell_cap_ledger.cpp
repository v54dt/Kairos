#include "sell_cap_ledger.h"

namespace kairos::exec {

void SellCapLedger::AbandonResting(Side side, bool resting_active, long resting_shares,
                                   long resting_filled, const std::string& resting_id) {
  if (side != Side::kSell) return;  // the ledger only feeds the sell position cap
  long unfilled = resting_active ? resting_shares - resting_filled : 0;
  if (unfilled > 0 && !resting_id.empty()) inflight_lost_[resting_id] += unfilled;
}

bool SellCapLedger::ReleaseOnCancel(const std::string& id) { return inflight_lost_.erase(id) > 0; }

void SellCapLedger::OnLateFill(const std::string& id, long shares) {
  auto it = inflight_lost_.find(id);
  if (it != inflight_lost_.end()) {
    it->second -= shares;
    if (it->second <= 0) inflight_lost_.erase(it);
  }
}

long SellCapLedger::SellCapRemaining(long position_shares, long filled_shares, bool resting_active,
                                     long resting_shares, long resting_filled) const {
  long inflight = resting_active ? resting_shares - resting_filled : 0;
  for (const auto& entry : inflight_lost_) inflight += entry.second;
  long committed = filled_shares + inflight;
  long remaining = position_shares - committed;
  if (remaining < 0) remaining = 0;
  return remaining;
}

}  // namespace kairos::exec
