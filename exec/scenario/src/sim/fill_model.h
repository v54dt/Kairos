#ifndef KAIROS_EXEC_SIM_FILL_MODEL_H_
#define KAIROS_EXEC_SIM_FILL_MODEL_H_

#include <cstdint>
#include <string>
#include <vector>

#include "quote_book.h"  // TopOfBook, Level
#include "sim_types.h"

namespace kairos::exec {

// Per-symbol event-driven fill model. Consumes the exec-side 5-level book
// (TopOfBook) and Trade events; drives (partial) fills and acks over callbacks
// shaped exactly like OrderBackend so both the A4 offline hub and the future B8
// live-paper backend reuse it unchanged. ALL time comes from event ts_us
// parameters — no wall clock is read here, so a replayed tape at any speed yields
// identical fills (determinism is an acceptance criterion).
//
// Two selectable modes (FillMode, fixed at construction):
//
// (1) kConservative (穿價才成交). A marketable-on-arrival order walks the
//     displayed book at Submit: a BUY with price >= best_ask consumes ask levels
//     best->limit, one Fill per level at that level's price, capped at order
//     shares; the remainder rests. A resting BUY at limit P fills ONLY when the
//     market trades or quotes STRICTLY through P — a Trade prints at price < P
//     (strictly), OR a book update makes best_ask < P (strictly). At-touch
//     (price == P) NEVER fills in this mode (that is kProbQueue's job). SELL is
//     symmetric: trade price > P / best_bid > P strictly. Trade-through fills at
//     P and are capped by a shared trade-volume budget consumed in time-priority
//     order; quote-through fills at P capped by the crossing displayed volume.
//
// (2) kProbQueue. Ports the queue-position concept from hftbacktest
//     (https://github.com/nkaz001/hftbacktest) ProbQueueModel: an order at a
//     price level tracks queue_ahead (shares that must trade/cancel before it),
//     and advances deterministically (no RNG — the proportional / power-1
//     variant, so determinism holds by construction):
//       On placement: queue_ahead = displayed volume at the order's level
//         (bid volume at P for a resting BUY; 0 if P is not a displayed level).
//       (a) Trade at the order's price P, volume v:
//             filled      = min(remaining, max(0, v - queue_ahead))
//             queue_ahead = max(0, queue_ahead - v)
//             pending_trade_vol += v          // reconciles the next book decrease
//       (b) Book update with displayed decrease D at P (no double-count):
//             D_eff       = max(0, D - pending_trade_vol)
//             pending     = max(0, pending_trade_vol - D)
//             queue_ahead = max(0, queue_ahead - floor(D_eff * queue_ahead / D_prev))
//             D_prev      = D_now
//     An order fills only on a trade (never on a bare book decrease) and only for
//     the portion of trade volume beyond queue_ahead.
//
// Trial events (試撮 indicative auction matches, is_trial=true) never produce a
// continuous fill and never advance the queue; they update the cached book only.
// Per-symbol isolation is the caller's (FillEngine) responsibility: one instance
// owns exactly one symbol's orders.
class SymbolFillModel {
 public:
  SymbolFillModel(std::string symbol, FillMode mode, SimAckFn ack, SimFillFn fill,
                  SimCancelFn cancel);

  void OnBook(const TopOfBook& book, std::int64_t ts_us);
  void OnTrade(Cents price, long vol, std::int64_t ts_us, bool is_trial);

  // Acks (ok=true) then walks the book for the marketable portion; the remainder
  // rests. The caller guarantees a round-lot order for this symbol.
  void Submit(const SimOrder& order);

  // Rest an already-acked order (no ack emitted): walks the current book for the
  // marketable portion, rests the remainder. Used to hand an opening-auction
  // unmatched remainder into the continuous session.
  void PlaceResting(const SimOrder& order);

  // Suspend continuous matching during a call-auction window: while disabled,
  // OnBook only refreshes the cached book and OnTrade is ignored (no fills, no
  // queue advancement). Re-enabling resumes from the current book.
  void SetMatchingEnabled(bool enabled) { matching_ = enabled; }

  // Cancels the resting remainder. Returns true if an order with `id` was found.
  bool Cancel(const std::string& id, std::int64_t ts_us);

  bool HasResting() const { return !resting_.empty(); }

 private:
  struct Resting {
    SimOrder order;
    long queue_ahead = 0;        // kProbQueue only
    long pending_trade_vol = 0;  // kProbQueue only
    long displayed_prev = 0;     // kProbQueue only: displayed vol at the level
  };

  void MarketableWalk(SimOrder* order);  // fills against book_, mutates order.filled
  void EmitFill(Resting* r, long shares, Cents price);
  void DropFilled();

  std::string symbol_;
  FillMode mode_;
  SimAckFn on_ack_;
  SimFillFn on_fill_;
  SimCancelFn on_cancel_;
  TopOfBook book_;
  bool matching_ = true;
  std::vector<Resting> resting_;  // insertion order == time priority
};

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_SIM_FILL_MODEL_H_
