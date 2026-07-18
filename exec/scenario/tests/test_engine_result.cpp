// Covers ScenarioEngine::Result(): a fresh engine reports zeros; after a paper
// fill it reflects the filled shares/notional/average and the complete flag.

#include <cstdio>
#include <string>
#include <utility>

#include "engine.h"
#include "event_sink.h"
#include "order_backend.h"
#include "quote_book.h"
#include "quote_source.h"
#include "scenario.h"
#include "test_check.h"
#include "tw_market.h"

using namespace kairos::exec;

namespace {

// Pushes quotes synchronously on the caller's thread (no socket, no real clock).
class FakeQuoteSource : public QuoteSource {
 public:
  void SetCallback(QuoteFn on_quote) override { on_quote_ = std::move(on_quote); }
  void SetTradeCallback(TradeFn) override {}
  void Start() override {}
  void Stop() override {}
  void Push(const std::string& symbol, const TopOfBook& tob) {
    if (on_quote_) on_quote_(symbol, tob);
  }

 private:
  QuoteFn on_quote_;
};

TopOfBook ValidBook(Cents price) {
  TopOfBook tob;
  tob.bids[0] = {price, 100};
  tob.asks[0] = {price, 100};
  tob.n_bids = 1;
  tob.n_asks = 1;
  tob.last_trade = price;
  tob.valid = true;
  return tob;
}

Scenario BaseScenario() {
  Scenario s;
  s.symbol = "2330";
  s.side = Side::kBuy;
  s.board = Board::kOddLot;
  s.price_policy = PricePolicy::kCross;
  s.reference_price = 100.0;
  s.shares_per_order = 10;
  s.pacing = Pacing::kAsap;
  s.quote_max_age_ms = 0;
  s.quote_stall_alert_ms = 0;
  s.ack_timeout_ms = 0;
  s.window_start_hhmm = 900;
  s.window_end_hhmm = 1100;
  return s;
}

// A fresh engine snapshots zeros with no divide-by-zero on the average.
void TestResultBeforeRun() {
  Scenario s = BaseScenario();
  s.budget_twd = 1000;
  PaperOrderBackend backend;
  NullEventSink sink;
  FakeQuoteSource quotes;
  ScenarioEngine engine(std::move(s), &backend, &sink, &quotes);

  LegResult r = engine.Result();
  CHECK_EQ(r.filled_shares, 0);
  CHECK_EQ(r.filled_notional_cents, 0);
  CHECK_EQ(r.avg_price_cents, 0);
  CHECK(!r.complete);
  CHECK(!r.halted);
}

// After a paper fill Result() reflects the accounting and the complete flag.
void TestResultAfterFills() {
  Scenario s = BaseScenario();
  s.budget_twd = 3000;  // 3 slices of 10 sh @ 100.00
  PaperOrderBackend backend;
  NullEventSink sink;
  FakeQuoteSource quotes;
  ScenarioEngine engine(std::move(s), &backend, &sink, &quotes);
  engine.set_ignore_window(true);
  quotes.Push("2330", ValidBook(FloatToCents(100.0)));
  engine.Run();

  LegResult r = engine.Result();
  CHECK_EQ(r.filled_shares, 30);
  CHECK_EQ(r.filled_notional_cents, 300000);  // 30 sh * 10000 cents
  CHECK_EQ(r.avg_price_cents, 10000);         // 100.00
  CHECK(r.complete);
  CHECK(!r.halted);
}

}  // namespace

int main() {
  TestResultBeforeRun();
  TestResultAfterFills();
  if (g_failures == 0) {
    std::printf("test_engine_result: OK\n");
    return 0;
  }
  std::printf("test_engine_result: FAILED %d check(s)\n", g_failures);
  return 1;
}
