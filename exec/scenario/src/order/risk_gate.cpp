#include "risk_gate.h"

#include <cstdlib>

#include "tw_market.h"  // CentsToString, kMax* bounds

namespace kairos::exec {

bool RiskGate::CollarReference(const RiskStateView& view, const std::string& symbol,
                               Cents* ref) const {
  auto it = view.last_fill_price_cents.find(symbol);
  if (it == view.last_fill_price_cents.end()) return false;
  *ref = it->second;
  return true;
}

bool RiskGate::DuplicateSubmit(RiskStateView& view, const OrderSubmitMsg& o) const {
  while (!view.dup_ring.empty() &&
         view.now_ms - view.dup_ring.front().mono_ms > risk_.dup_order_window_ms) {
    view.dup_ring.pop_front();
  }
  for (const DupEntry& e : view.dup_ring) {
    if (e.symbol == o.symbol && e.side == o.side && e.shares == o.shares && e.price == o.price) {
      return true;
    }
  }
  return false;
}

bool RiskGate::SelfMatchCross(const RiskStateView& view, const OrderSubmitMsg& o,
                              std::string* other_id) const {
  for (const auto& rid : view.open_ids) {
    auto it = view.routes.find(rid);
    if (it == view.routes.end()) continue;
    const Route& r = it->second;
    if (r.closed || r.side == o.side || r.symbol != o.symbol) continue;
    bool cross = o.side == Side::kBuy ? o.price >= r.price : o.price <= r.price;
    if (cross) {
      *other_id = rid;
      return true;
    }
  }
  return false;
}

std::optional<std::string> RiskGate::Evaluate(const OrderSubmitMsg& o, RiskStateView& view) const {
  std::string reject;
  // Fail-closed field validation: a doubtful submit is rejected, never forwarded.
  auto live = view.routes.find(o.id);
  if (o.id.empty() || o.symbol.empty() || o.shares <= 0 || o.shares > kMaxTwStockShares ||
      o.price <= 0 || o.price > kMaxTwStockPriceCents) {
    reject = "invalid order fields";
  } else if (live != view.routes.end() && !live->second.closed) {
    reject = "duplicate live order id";
  } else if (risk_.max_order_shares > 0 && o.shares > risk_.max_order_shares) {
    reject = "order size " + std::to_string(o.shares) + " exceeds max " +
             std::to_string(risk_.max_order_shares);
  } else if (Cents ref = 0; risk_.price_collar_pct > 0 && CollarReference(view, o.symbol, &ref) &&
                            std::llabs(o.price - ref) * 100 > risk_.price_collar_pct * ref) {
    reject = "price " + CentsToString(o.price) + " deviates >" +
             std::to_string(risk_.price_collar_pct) + "% from last fill " + CentsToString(ref) +
             " (fat-finger?)";
  } else if (risk_.dup_order_window_ms > 0 && DuplicateSubmit(view, o)) {
    reject = "duplicate order within " + std::to_string(risk_.dup_order_window_ms) +
             "ms (suspected runaway loop)";
  } else if (std::int64_t n = static_cast<std::int64_t>(o.price) * o.shares;
             risk_.max_account_notional_cents > 0 &&
             view.account_day_realized_cents + view.account_open_notional_cents + n >
                 risk_.max_account_notional_cents) {
    reject = "account notional cap exceeded";
  } else if (risk_.max_open_orders_per_client > 0 &&
             view.client_open_orders + 1 > risk_.max_open_orders_per_client) {
    reject = "per-client open-order limit exceeded";
  } else if (risk_.max_open_notional_per_client_cents > 0 &&
             view.client_open_notional + n > risk_.max_open_notional_per_client_cents) {
    reject = "per-client open-notional limit exceeded";
  } else if (std::string other; risk_.self_match_protection && SelfMatchCross(view, o, &other)) {
    reject = "self-match risk vs open order " + other;
  }
  if (reject.empty()) return std::nullopt;
  return reject;
}

}  // namespace kairos::exec
