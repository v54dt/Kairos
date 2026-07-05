#ifndef KAIROS_EXEC_SIM_ORDER_BACKEND_H_
#define KAIROS_EXEC_SIM_ORDER_BACKEND_H_

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "fill_engine.h"
#include "order_backend.h"
#include "quote_book.h"

namespace kairos::exec {

// An OrderBackend backed by the offline fill-model library, for the mock order
// hub (kairos_sim_hubd). It speaks the exact OrderBackend surface the live hub
// uses, so OrderHub/OrderHubServer route it verbatim and a scenario trader
// connects unmodified. It has NO broker SDK and NO real-order path — a real order
// is structurally impossible.
//
// Order and market-event ingestion are serialized under one mutex so a cancel can
// never race a fill. Order arrival time is taken from the latest market-event
// timestamp (event time, never a wall clock), keeping fills deterministic on a
// replayed tape.
class SimOrderBackend : public OrderBackend {
 public:
  SimOrderBackend(FillMode mode, const std::vector<std::string>& symbols);

  bool Connect() override;
  void Disconnect() override;
  bool IsConnected() const override;
  void Submit(const OrderSubmitMsg& order) override;
  void Cancel(const std::string& id) override;

  // Market-event ingestion (driven by the quote UDS in the daemon, or directly in
  // tests). Both advance the sim's event clock.
  void OnBook(const std::string& symbol, const TopOfBook& book);
  void OnTrade(const std::string& symbol, const Trade& trade);

  // OrderBackend market-event hooks (in-process paper): forward the engine's
  // stream into the fill model. WantsMarketTrades() opts the engine's trade feed
  // in so passive fills advance on real trades.
  void OnMarketBook(const std::string& symbol, const TopOfBook& book, std::int64_t ts_us) override;
  void OnMarketTrade(const std::string& symbol, const Trade& trade, std::int64_t ts_us) override;
  bool WantsMarketTrades() const override { return true; }

  // Flush pending closing-auction orders at end of tape / shutdown (see
  // FillEngine::Finalize). Safe to call once the market-event stream has ended.
  void Finalize();

 private:
  void ApplyBookLocked(const std::string& symbol, const TopOfBook& book);
  void ApplyTradeLocked(const std::string& symbol, const Trade& trade);
  void FlushPendingBookLocked();

  std::mutex mu_;
  FillEngine engine_;
  std::int64_t last_ts_us_ = 0;
  bool connected_ = false;
  // In-process paper defers each post-trade depth Quote (the concords feed emits it
  // BEFORE the paired Trade) until after that Trade, so the queue model sees the
  // trade-then-book order its no-double-count reconciliation requires. Only the
  // OnMarket* hooks buffer; offline replay via OnBook/OnTrade applies immediately.
  TopOfBook pending_book_;
  std::string pending_book_symbol_;
  bool has_pending_book_ = false;
};

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_SIM_ORDER_BACKEND_H_
