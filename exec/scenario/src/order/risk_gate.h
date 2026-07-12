#ifndef KAIROS_EXEC_RISK_GATE_H_
#define KAIROS_EXEC_RISK_GATE_H_

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "order_codec.h"  // OrderSubmitMsg, Side, Cents
#include "tw_market.h"    // Cents

namespace kairos::exec {

// Account risk-gate limits. All numeric caps are 0 = disabled; notional caps
// are in Cents. self_match_protection defaults on so it is fail-closed by default.
struct RiskConfig {
  long max_account_notional_cents = 0;
  int max_open_orders_per_client = 0;
  long max_open_notional_per_client_cents = 0;
  bool self_match_protection = true;
  long max_order_shares = 0;     // per-submit share hard cap; 0 = disabled
  long dup_order_window_ms = 0;  // reject an identical resubmit within this window; 0 = disabled
  long price_collar_pct = 0;     // reject a price this far from the last fill; 0 = disabled
  std::string halt_file_path;
  // Journal dir shared with the traders; a fill the hub cannot route (client
  // gone) is appended here so a restarted trader replays it. Empty = disabled.
  std::string journal_dir;
  // Also write the per-day hub-orders-<day>.jsonl audit stream (every submit/
  // ack/fill/cancel the hub processes) into journal_dir. On when journal_dir
  // resolves; a best-effort stream separate from the replayed run-state journal.
  bool order_flow_journal = true;
};

// Lifecycle of one live order, derived from the routing the hub already does.
struct Route {
  int client = -1;
  long shares_remaining = 0;
  bool acked = false;
  bool closed = false;  // fully filled or successfully cancelled
  std::string symbol;
  Side side = Side::kBuy;
  Cents price = 0;  // reserved open notional == price * shares_remaining
};

// One admitted-and-still-live submit, kept in a bounded ring so a runaway
// strategy that resubmits the same (symbol, side, shares, price) within the
// window is caught. Removed when the order reaches a terminal state so a
// legitimate sequential re-order after a fill/cancel/reject is not blocked.
struct DupEntry {
  std::string id;
  std::string symbol;
  Side side = Side::kBuy;
  long shares = 0;
  Cents price = 0;
  long mono_ms = 0;
};

// Narrow view of the hub state the gate reads/mutates for one submit decision.
// The hub owns the state under its mutex and builds this per call; const refs
// are read-only, dup_ring is pruned in place, and the aggregates/client counts
// are value snapshots taken under the lock.
struct RiskStateView {
  const std::unordered_map<std::string, Route>& routes;
  const std::unordered_set<std::string>& open_ids;
  const std::unordered_map<std::string, Cents>& last_fill_price_cents;
  std::deque<DupEntry>& dup_ring;
  std::int64_t account_open_notional_cents = 0;
  std::int64_t account_day_realized_cents = 0;
  int client_open_orders = 0;
  std::int64_t client_open_notional = 0;
  long now_ms = 0;  // steady-clock ms for the dup window (0 when the window is off)
};

// The submit-path money-guard: the same first-match else-if chain the hub used
// inline. Stateless and mutex-free; the caller holds the hub lock and passes a
// RiskStateView. Evaluate returns the reject reason, or nullopt to admit. The
// only evaluate-time write is pruning expired dup_ring entries (clause 5); the
// hub keeps the entire admit branch (route/notional/dup-ring push).
class RiskGate {
 public:
  explicit RiskGate(const RiskConfig& risk) : risk_(risk) {}

  std::optional<std::string> Evaluate(const OrderSubmitMsg& o, RiskStateView& view) const;

 private:
  // The account's last fill price for `symbol` if one exists (the collar
  // reference); false at cold start (no fill yet).
  bool CollarReference(const RiskStateView& view, const std::string& symbol, Cents* ref) const;
  // Prune the dup ring to the window and report whether an identical admitted
  // submit is still within it. Caller has checked the window > 0.
  bool DuplicateSubmit(RiskStateView& view, const OrderSubmitMsg& o) const;
  // True if the submit would cross this account's own open opposite-side order on
  // the same symbol (證交法 155-1-5); *other_id is the crossed order.
  bool SelfMatchCross(const RiskStateView& view, const OrderSubmitMsg& o,
                      std::string* other_id) const;

  const RiskConfig& risk_;
};

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_RISK_GATE_H_
