#ifndef KAIROS_EXEC_SIM_AUCTION_H_
#define KAIROS_EXEC_SIM_AUCTION_H_

#include <cstdint>
#include <string>
#include <vector>

#include "scenario.h"   // Side
#include "sim_types.h"  // SimOrder
#include "tw_market.h"  // Cents

namespace kairos::exec {

// Single-symbol call-auction (集合競價) accumulator + single-price match, used
// for the opening (09:00) and closing (13:25-13:30) auctions. Pure logic driven
// by event time; no wall clock, no continuous fills while accumulating.
//
// ============================ Assumptions ============================
// TWSE micro-rules that the spec does not pin down are resolved here as the
// simplest faithful reading. Every such choice is listed in ONE place:
//
//  A1. Round lot only (第一版僅整股). Odd-lot auction is out of scope; callers
//      reject odd-lot before an order reaches this engine.
//  A2. Limit orders only. Market orders in the auction are not modelled in the
//      first version (the scenario engine places limit orders); an order carries
//      a positive limit price.
//  A3. Crossing price = the candidate price that maximizes executable volume,
//      where candidates are the union of all accumulated limit prices. Ties are
//      broken in order: (i) minimize |demand-supply| at the price; (ii) price
//      closest to the reference (last pre-window trade); (iii) higher price.
//      (TWSE's actual rule also references the last price for imbalance; this is
//      the simplest faithful reduction.)
//  A4. Allocation is price-then-time priority: buys with limit > cross and sells
//      with limit < cross fill fully; marginal orders exactly at the cross fill
//      by time priority (place_ts_us, then accumulation order). All fills execute
//      at the single crossing price.
//  A5. Reference price for the 3.5% band = the last continuous-session trade
//      observed before the closing window opened (0 => band check disabled, the
//      match is never delayed).
//  A6. 延緩收盤 (stabilization delay): if the indicative closing cross deviates
//      more than 3.5% from the reference, the match is delayed ONCE by 180s
//      (kDelaySeconds); accumulation continues in the extension and the match at
//      the extended close is unconditional. The delay is decided per symbol.
//  A7. Opening-auction unmatched remainder carries into the continuous session as
//      a resting limit order; closing-auction unmatched remainder expires at the
//      close (the session is over).
//  A8. Resting continuous orders are NOT migrated into the closing call auction
//      (real TWSE migrates unfilled continuous orders into 集合競價收盤). They are
//      frozen at 13:25 and expired with a terminal cancel at the close, so an acked
//      order always ends the session with a terminal event; a strategy targeting
//      the closing auction must submit explicit closing-window orders. Related:
//      an order submitted before any market event has place_ts_us anchored at 0,
//      so HhmmFromUs(0) (08:00 Taipei) routes it to the opening auction — accepted,
//      the sim is tape-driven.
// ====================================================================
class AuctionEngine {
 public:
  struct Cross {
    bool crossed = false;
    Cents price = 0;
    long volume = 0;  // executable volume at `price`
  };

  struct Alloc {
    std::string id;
    long shares = 0;
    Cents price = 0;
  };

  void SetReference(Cents ref) { reference_ = ref; }
  Cents reference() const { return reference_; }

  // Accumulate one order (no fill). `order.remaining()` shares participate.
  void Add(const SimOrder& order);

  bool empty() const { return orders_.empty(); }
  const std::vector<SimOrder>& orders() const { return orders_; }

  // Remove an accumulated order before the match (a cancel during the window).
  bool Remove(const std::string& id);

  // Crossing price + executable volume per A3 (crossed=false if no positive cross).
  Cross ComputeCross() const;

  // True if `ComputeCross()` deviates more than 3.5% from a non-zero reference.
  bool DeviatesBeyondBand() const;

  // Fills allocated at the single crossing price per A4. Empty if no cross.
  std::vector<Alloc> Match() const;

  void Clear() { orders_.clear(); }

 private:
  std::vector<SimOrder> orders_;  // accumulation order == arrival order
  Cents reference_ = 0;
};

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_SIM_AUCTION_H_
