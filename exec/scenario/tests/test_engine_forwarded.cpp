// Pins the trader-side ack-clock restart (Kairos review.md 2026-07-18). A hub
// `forwarded` event rebases the ack-timeout clock to the moment the order actually
// reached the broker, so hub queue delay stops reading as broker silence — while
// REAL broker silence still times out relative to that moment. Three timings are
// pinned: restart-at-forwarded, fail-closed-without-forwarded (old behavior), and
// no-double-extend. A settable mono clock + a non-acking backend that exposes the
// forwarded/ack callbacks make each threshold deterministic.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "engine.h"
#include "event.h"
#include "event_sink.h"
#include "order_backend.h"
#include "quote_book.h"
#include "quote_source.h"
#include "scenario.h"
#include "test_check.h"
#include "tw_market.h"

using namespace kairos::exec;

namespace {

class FakeQuoteSource : public QuoteSource {
 public:
  void SetCallback(QuoteFn on_quote) override { on_quote_ = std::move(on_quote); }
  void SetTradeCallback(TradeFn on_trade) override { on_trade_ = std::move(on_trade); }
  void Start() override {}
  void Stop() override {}
  void Push(const std::string& symbol, const TopOfBook& tob) {
    if (on_quote_) on_quote_(symbol, tob);
  }

 private:
  QuoteFn on_quote_;
  TradeFn on_trade_;
};

// Places are recorded but never acked, so the ack-timeout watchdog governs. The
// test drives forwarded/ack explicitly through the captured backend callbacks.
class ManualBackend : public OrderBackend {
 public:
  bool Connect() override {
    connected_ = true;
    return true;
  }
  void Disconnect() override { connected_ = false; }
  bool IsConnected() const override { return connected_; }
  void Submit(const OrderSubmitMsg& o) override {
    std::lock_guard<std::mutex> lk(mu_);
    last_id_ = o.id;
    ++submit_count_;
  }
  void Cancel(const std::string& id) override {
    std::lock_guard<std::mutex> lk(mu_);
    ++cancel_count_;
    (void)id;
  }
  int SubmitCount() {
    std::lock_guard<std::mutex> lk(mu_);
    return submit_count_;
  }
  std::string LastId() {
    std::lock_guard<std::mutex> lk(mu_);
    return last_id_;
  }
  void FireForwarded(const std::string& id) {
    if (on_forwarded_) on_forwarded_(id);
  }
  void FireAck(const std::string& id) {
    if (on_ack_) on_ack_(id, true, "");
  }

 private:
  bool connected_ = false;
  std::mutex mu_;
  std::string last_id_;
  int submit_count_ = 0;
  int cancel_count_ = 0;
};

// Counts ack-timeout watchdog events (dedup_key "ack_timeout:<id>").
class TimeoutSink : public EventSink {
 public:
  void Emit(const Event& ev) override {
    if (ev.category == EventCategory::kError && ev.dedup_key.rfind("ack_timeout:", 0) == 0)
      ++timeouts_;
  }
  int Timeouts() const { return timeouts_.load(); }

 private:
  std::atomic<int> timeouts_{0};
};

EngineClock SettableClock(std::shared_ptr<std::atomic<long>> mono_ns) {
  EngineClock clock;
  clock.mono = [mono_ns] {
    return std::chrono::steady_clock::time_point(std::chrono::nanoseconds(mono_ns->load()));
  };
  clock.wall = [] {
    return std::chrono::system_clock::time_point{};
  };  // wall unused (ignore_window)
  return clock;
}

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

Scenario RestingScenario() {
  Scenario s;
  s.symbol = "2330";
  s.side = Side::kBuy;
  s.board = Board::kOddLot;
  s.price_policy = PricePolicy::kCross;
  s.reference_price = 100.0;
  s.shares_per_order = 10;
  s.require_two_sided = true;
  s.pacing = Pacing::kAsap;
  s.budget_twd = 100000000;  // large: keeps a resting order alive across timeouts
  s.quote_max_age_ms = 0;
  s.quote_stall_alert_ms = 0;
  s.ack_timeout_ms = 3000;
  s.max_consecutive_order_failures = 1000;  // never halt: we observe timeouts directly
  s.weekdays_only = true;
  s.window_start_hhmm = 900;
  s.window_end_hhmm = 1100;
  return s;
}

void SetMono(std::atomic<long>& mono_ns, long ms) { mono_ns.store(ms * 1'000'000L); }

bool PollUntil(const std::function<bool()>& pred, int timeout_ms) {
  auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < until) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return pred();
}

// Wait a bounded window asserting the watchdog does NOT fire (mono held stable
// below the deadline, so no tick can time out).
bool StaysZero(const TimeoutSink& sink, int ms) {
  auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < until) {
    if (sink.Timeouts() != 0) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return sink.Timeouts() == 0;
}

// Places one resting, un-acked order and returns its id (via out params so the
// caller keeps ownership of the long-lived objects).
std::string PlaceResting(ManualBackend& backend, FakeQuoteSource& quotes) {
  CHECK(PollUntil([&] { return backend.SubmitCount() >= 1; }, 3000));
  return backend.LastId();
}

// --- forwarded rebases the clock: no timeout before forwarded+ack_timeout -------
void TestRestartAtForwarded() {
  auto mono = std::make_shared<std::atomic<long>>(0);
  ManualBackend backend;
  TimeoutSink sink;
  FakeQuoteSource quotes;
  ScenarioEngine engine(RestingScenario(), &backend, &sink, &quotes, SettableClock(mono));
  engine.set_ignore_window(true);
  std::thread runner([&] { engine.Run(); });
  quotes.Push("2330", ValidBook(FloatToCents(100.0)));
  std::string id = PlaceResting(backend, quotes);

  SetMono(*mono, 1000);
  backend.FireForwarded(id);  // resting_t_submit_ -> 1000, deadline 4000
  SetMono(*mono, 3500);       // 3500-1000=2500 < 3000: no timeout (would fire at 3500 without it)
  CHECK(StaysZero(sink, 500));
  SetMono(*mono, 4200);  // 4200-1000=3200 >= 3000: fires now
  CHECK(PollUntil([&] { return sink.Timeouts() >= 1; }, 2000));

  engine.RequestStop();
  runner.join();
  std::printf("restart-at-forwarded: timeouts=%d\n", sink.Timeouts());
}

// --- fail-closed: no forwarded -> timeout at the original submit + ack_timeout ---
void TestFailClosedWithoutForwarded() {
  auto mono = std::make_shared<std::atomic<long>>(0);
  ManualBackend backend;
  TimeoutSink sink;
  FakeQuoteSource quotes;
  ScenarioEngine engine(RestingScenario(), &backend, &sink, &quotes, SettableClock(mono));
  engine.set_ignore_window(true);
  std::thread runner([&] { engine.Run(); });
  quotes.Push("2330", ValidBook(FloatToCents(100.0)));
  std::string id = PlaceResting(backend, quotes);
  (void)id;

  SetMono(*mono, 2000);  // 2000 < 3000: no timeout
  CHECK(StaysZero(sink, 500));
  SetMono(*mono, 3100);  // 3100 >= 3000: fires from the ORIGINAL submit time
  CHECK(PollUntil([&] { return sink.Timeouts() >= 1; }, 2000));

  engine.RequestStop();
  runner.join();
  std::printf("fail-closed-without-forwarded: timeouts=%d\n", sink.Timeouts());
}

// --- a second forwarded must not extend again (once per resting order) ----------
void TestNoDoubleExtend() {
  auto mono = std::make_shared<std::atomic<long>>(0);
  ManualBackend backend;
  TimeoutSink sink;
  FakeQuoteSource quotes;
  ScenarioEngine engine(RestingScenario(), &backend, &sink, &quotes, SettableClock(mono));
  engine.set_ignore_window(true);
  std::thread runner([&] { engine.Run(); });
  quotes.Push("2330", ValidBook(FloatToCents(100.0)));
  std::string id = PlaceResting(backend, quotes);

  SetMono(*mono, 1000);
  backend.FireForwarded(id);  // deadline 4000
  SetMono(*mono, 1500);
  backend.FireForwarded(id);  // ignored: would move deadline to 4500 if it extended
  SetMono(*mono, 3500);       // < 4000: no timeout yet
  CHECK(StaysZero(sink, 500));
  SetMono(*mono, 4200);  // >= 4000 (first forwarded) but < 4500: fires -> not double-extended
  CHECK(PollUntil([&] { return sink.Timeouts() >= 1; }, 2000));

  engine.RequestStop();
  runner.join();
  std::printf("no-double-extend: timeouts=%d\n", sink.Timeouts());
}

// --- a forwarded for a stale/abandoned id is ignored (clock not rebased) --------
void TestStaleForwardedIgnored() {
  auto mono = std::make_shared<std::atomic<long>>(0);
  ManualBackend backend;
  TimeoutSink sink;
  FakeQuoteSource quotes;
  ScenarioEngine engine(RestingScenario(), &backend, &sink, &quotes, SettableClock(mono));
  engine.set_ignore_window(true);
  std::thread runner([&] { engine.Run(); });
  quotes.Push("2330", ValidBook(FloatToCents(100.0)));
  std::string id = PlaceResting(backend, quotes);
  (void)id;

  SetMono(*mono, 1000);
  backend.FireForwarded("no-such-id");  // not the resting id: ignored
  SetMono(*mono, 3100);                 // still fires at the original submit + 3000
  CHECK(PollUntil([&] { return sink.Timeouts() >= 1; }, 2000));

  engine.RequestStop();
  runner.join();
  std::printf("stale-forwarded-ignored: timeouts=%d\n", sink.Timeouts());
}

// --- forwarded then a real ack: no timeout ever fires (clean normal path) --------
void TestForwardedThenAckClean() {
  auto mono = std::make_shared<std::atomic<long>>(0);
  ManualBackend backend;
  TimeoutSink sink;
  FakeQuoteSource quotes;
  ScenarioEngine engine(RestingScenario(), &backend, &sink, &quotes, SettableClock(mono));
  engine.set_ignore_window(true);
  std::thread runner([&] { engine.Run(); });
  quotes.Push("2330", ValidBook(FloatToCents(100.0)));
  std::string id = PlaceResting(backend, quotes);

  SetMono(*mono, 1000);
  backend.FireForwarded(id);
  backend.FireAck(id);   // acked: watchdog disarmed regardless of the clock
  SetMono(*mono, 9000);  // well past any deadline
  CHECK(StaysZero(sink, 500));

  engine.RequestStop();
  runner.join();
  std::printf("forwarded-then-ack-clean: timeouts=%d\n", sink.Timeouts());
}

}  // namespace

int main() {
  TestRestartAtForwarded();
  TestFailClosedWithoutForwarded();
  TestNoDoubleExtend();
  TestStaleForwardedIgnored();
  TestForwardedThenAckClean();

  if (g_failures == 0) {
    std::printf("test_engine_forwarded: OK\n");
    return 0;
  }
  std::printf("test_engine_forwarded: FAILED %d check(s)\n", g_failures);
  return 1;
}
