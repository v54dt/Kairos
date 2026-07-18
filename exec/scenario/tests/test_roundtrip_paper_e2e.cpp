// Full-chain paper smoke: the real EngineLegFactory drives ScenarioEngine legs
// against QueueSimBackend on a synthetic book, with a scripted signal source and an
// injected clock. Enter fills, a reverse signal exits, the position goes FLAT.
// Deterministic: odd-lot fills instantly and the clock never advances.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "queue_sim_backend.h"
#include "roundtrip_runner.h"
#include "test_check.h"

using namespace kairos::exec;
using namespace std::chrono_literals;

namespace {

template <class Pred>
bool WaitFor(Pred p, std::chrono::milliseconds timeout = 5s) {
  auto end = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < end) {
    if (p()) return true;
    std::this_thread::sleep_for(1ms);
  }
  return p();
}

EngineClock FixedClock() {
  return EngineClock{
      [] {
        return std::chrono::system_clock::time_point(std::chrono::minutes(600 - 480));
      },  // 10:00
      [] { return std::chrono::steady_clock::time_point(std::chrono::milliseconds(0)); }};
}

TopOfBook Book(Cents px) {
  TopOfBook t;
  t.bids[0] = {px, 100};
  t.asks[0] = {px, 100};
  t.n_bids = 1;
  t.n_asks = 1;
  t.last_trade = px;
  t.valid = true;
  return t;
}

// Delivers one synthetic book on Start() so the leg engine can price and fill.
class BookQuoteSource : public QuoteSource {
 public:
  BookQuoteSource(std::string symbol, TopOfBook book) : symbol_(std::move(symbol)), book_(book) {}
  void SetCallback(QuoteFn f) override { cb_ = std::move(f); }
  void SetTradeCallback(TradeFn) override {}
  void Start() override {
    if (cb_) cb_(symbol_, book_);
  }
  void Stop() override {}

 private:
  std::string symbol_;
  TopOfBook book_;
  QuoteFn cb_;
};

class FakeSignalSource : public SignalSource {
 public:
  void SetCallbacks(Callbacks cb) override {
    std::lock_guard<std::mutex> lk(mu_);
    cb_ = std::move(cb);
  }
  void Start() override {}
  void Stop() override {}
  void Signal(SignalAction a) {
    Callbacks cb;
    {
      std::lock_guard<std::mutex> lk(mu_);
      cb = cb_;
    }
    if (cb.on_signal) cb.on_signal(a);
  }

 private:
  std::mutex mu_;
  Callbacks cb_;
};

class FakeQuoteSource : public QuoteSource {
 public:
  void SetCallback(QuoteFn f) override {
    std::lock_guard<std::mutex> lk(mu_);
    cb_ = std::move(f);
  }
  void SetTradeCallback(TradeFn) override {}
  void Start() override {}
  void Stop() override {}
  void Push(const std::string& s, const TopOfBook& t) {
    QuoteFn cb;
    {
      std::lock_guard<std::mutex> lk(mu_);
      cb = cb_;
    }
    if (cb) cb(s, t);
  }

 private:
  std::mutex mu_;
  QuoteFn cb_;
};

class RecorderSink : public EventSink {
 public:
  void Emit(const Event& ev) override {
    std::lock_guard<std::mutex> lk(mu_);
    keys_.push_back(ev.dedup_key);
  }
  bool Has(const std::string& key) {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& k : keys_)
      if (k == key) return true;
    return false;
  }

 private:
  std::mutex mu_;
  std::vector<std::string> keys_;
};

Scenario BaseScenario() {
  Scenario s;
  s.name = "rt";
  s.symbol = "2330";
  s.side = Side::kBuy;
  s.board = Board::kOddLot;
  s.price_policy = PricePolicy::kCross;
  s.pacing = Pacing::kAsap;
  s.budget_shares = 100;
  s.shares_per_order = 100;  // one slice per leg
  s.quote_max_age_ms = 0;
  s.quote_stall_alert_ms = 0;
  s.window_start_hhmm = 900;
  s.window_end_hhmm = 1325;
  s.roundtrip.enabled = true;
  s.roundtrip.signal = "vwap";
  s.roundtrip.stop_loss_pct = 5.0;  // well below the flat market so no stop fires
  s.roundtrip.max_hold_min = 30;
  s.roundtrip.enter_window_min = 10;
  s.roundtrip.arm_start_hhmm = 900;
  s.roundtrip.arm_end_hhmm = 1300;
  return s;
}

void TestPaperRoundTrip() {
  FakeSignalSource sig;
  FakeQuoteSource hold_quotes;
  RecorderSink sink;

  EngineLegFactory::BackendFn backend_fn =
      [](const Scenario& leg) -> std::unique_ptr<OrderBackend> {
    return std::make_unique<QueueSimBackend>(FillMode::kProbQueue,
                                             std::vector<std::string>{leg.symbol});
  };
  EngineLegFactory::QuoteFn quote_fn = [](const Scenario& leg) -> std::unique_ptr<QuoteSource> {
    return std::make_unique<BookQuoteSource>(leg.symbol, Book(10000));
  };
  EngineLegFactory legs(std::move(backend_fn), std::move(quote_fn), &sink, FixedClock(),
                        /*ignore_window=*/true);

  RoundTripRunner runner(BaseScenario(), &sig, &hold_quotes, &legs, &sink, FixedClock());
  int rc = -1;
  std::thread th([&] { rc = runner.Run(); });

  CHECK(WaitFor([&] { return sink.Has("rt:2330:armed"); }));
  sig.Signal(SignalAction::kEnter);
  CHECK(WaitFor([&] { return sink.Has("rt:2330:hold"); }));
  hold_quotes.Push("2330", Book(10000));  // flat market: no stop
  sig.Signal(SignalAction::kExit);
  th.join();

  CHECK_EQ(rc, 0);
  CHECK(sink.Has("rt:2330:flat"));
}

}  // namespace

int main() {
  TestPaperRoundTrip();
  if (g_failures == 0) {
    std::printf("test_roundtrip_paper_e2e: OK\n");
    return 0;
  }
  std::printf("test_roundtrip_paper_e2e: FAILED %d check(s)\n", g_failures);
  return 1;
}
