// Drives ScenarioEngine through its injected seams (QuoteSource + EngineClock)
// with no UDS socket and no real clock, proving the engine is replay-ready.
// FakeQuoteSource delivers synthetic quotes synchronously (production delivers
// on the UDS reader thread); the fake clock makes session gating deterministic.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <utility>

#include "engine.h"
#include "event_sink.h"
#include "order_backend.h"
#include "quote_book.h"
#include "quote_source.h"
#include "scenario.h"
#include "tw_market.h"

using namespace kairos::exec;

static int g_failures = 0;

#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                \
    }                                                              \
  } while (0)

namespace {

// A fake feed: stores the engine's callback and pushes quotes on demand. Unlike
// UdsQuoteClient, Push() invokes the callback on the caller's thread.
class FakeQuoteSource : public QuoteSource {
 public:
  void SetCallback(QuoteFn on_quote) override { on_quote_ = std::move(on_quote); }
  void Start() override {}
  void Stop() override {}
  void Push(const std::string& symbol, const TopOfBook& tob) {
    if (on_quote_) on_quote_(symbol, tob);
  }

 private:
  QuoteFn on_quote_;
};

// Paper backend that counts Submit calls; fills are still synchronous.
class RecordingPaperBackend : public PaperOrderBackend {
 public:
  void Submit(const OrderSubmitMsg& o) override {
    ++submit_count;
    PaperOrderBackend::Submit(o);
  }
  std::atomic<int> submit_count{0};
};

// A two-sided book that yields a fixed limit price (kCross buy -> best ask).
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

// UTC+8 wall instant for a given local date/time (weekday check + hhmm gating).
std::chrono::system_clock::time_point WallAt(int y, unsigned m, unsigned d, int hh, int mm) {
  auto days = std::chrono::sys_days{std::chrono::year{y} / m / d};
  return days + std::chrono::hours(hh) + std::chrono::minutes(mm) - std::chrono::hours(8);
}

// Fee-optimal sizing is disabled (shares_per_order set) so slice math is exact:
// 10 sh @ 100.00 == NT$ 1000 per slice.
Scenario BaseScenario() {
  Scenario s;
  s.symbol = "2330";
  s.side = Side::kBuy;
  s.board = Board::kOddLot;
  s.price_policy = PricePolicy::kCross;
  s.reference_price = 100.0;
  s.shares_per_order = 10;
  s.require_two_sided = true;
  s.quote_max_age_ms = 0;      // out-of-scope AgeMs clock disabled
  s.quote_stall_alert_ms = 0;  // no stall path
  s.ack_timeout_ms = 0;        // no watchdog
  s.weekdays_only = true;
  s.window_start_hhmm = 900;
  s.window_end_hhmm = 1100;
  return s;
}

bool PollUntil(const std::function<bool()>& pred, int timeout_ms) {
  auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < until) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return pred();
}

// A quote-driven place+fill+complete cycle with no socket and no real clock.
void TestQuoteDrivenComplete() {
  Scenario s = BaseScenario();
  s.pacing = Pacing::kAsap;
  s.budget_twd = 1000;  // exactly one slice

  RecordingPaperBackend backend;
  NullEventSink sink;
  FakeQuoteSource quotes;
  ScenarioEngine engine(std::move(s), &backend, &sink, &quotes);
  engine.set_ignore_window(true);  // wall clock unused; twap not throttled

  quotes.Push("2330", ValidBook(FloatToCents(100.0)));
  engine.Run();

  CHECK(backend.submit_count.load() == 1);
}

// The 13:30 hard stop is driven by the injected wall clock: at close the engine
// breaks before any order; one minute earlier it fills the remainder.
void TestHardStopAtClose() {
  {
    Scenario s = BaseScenario();
    s.pacing = Pacing::kAsap;
    s.budget_twd = 1000;
    RecordingPaperBackend backend;
    NullEventSink sink;
    FakeQuoteSource quotes;
    EngineClock clock;
    clock.wall = [] { return WallAt(2026, 7, 3, 13, 30); };  // Fri 13:30 -> closed
    ScenarioEngine engine(std::move(s), &backend, &sink, &quotes, clock);
    quotes.Push("2330", ValidBook(FloatToCents(100.0)));
    engine.Run();
    CHECK(backend.submit_count.load() == 0);
  }
  {
    Scenario s = BaseScenario();
    s.pacing = Pacing::kAsap;
    s.budget_twd = 1000;
    RecordingPaperBackend backend;
    NullEventSink sink;
    FakeQuoteSource quotes;
    EngineClock clock;
    clock.wall = [] { return WallAt(2026, 7, 3, 13, 29); };  // Fri 13:29 -> fill remainder
    ScenarioEngine engine(std::move(s), &backend, &sink, &quotes, clock);
    quotes.Push("2330", ValidBook(FloatToCents(100.0)));
    engine.Run();
    CHECK(backend.submit_count.load() >= 1);
  }
}

// twap pacing follows the injected wall clock: it waits while ahead of schedule
// and places once the schedule advances past the filled fraction. A stepped mono
// clock keeps the SdkGate from sleeping. Runs on a thread; a 13:30 kill ends it.
void TestTwapPacingSteppedClock() {
  Scenario s = BaseScenario();
  s.pacing = Pacing::kTwap;
  s.budget_twd = 4000;  // ~4 slices across 09:00-11:00

  RecordingPaperBackend backend;
  NullEventSink sink;
  FakeQuoteSource quotes;

  std::atomic<int> fake_hhmm{900};
  std::atomic<long> mono_ns{0};
  EngineClock clock;
  clock.wall = [&fake_hhmm] {
    int hhmm = fake_hhmm.load();
    return WallAt(2026, 7, 3, hhmm / 100, hhmm % 100);
  };
  clock.mono = [&mono_ns] {
    long v = mono_ns.fetch_add(2'000'000'000L);  // +2s per read: never gates
    return std::chrono::steady_clock::time_point(std::chrono::nanoseconds(v));
  };

  ScenarioEngine engine(std::move(s), &backend, &sink, &quotes, clock);
  quotes.Push("2330", ValidBook(FloatToCents(100.0)));
  std::thread runner([&engine] { engine.Run(); });

  // 09:00: schedule anchor + first slice.
  CHECK(PollUntil([&] { return backend.submit_count.load() >= 1; }, 3000));

  // 09:15: scheduled (12.5%) is behind the filled 25% -> engine waits.
  fake_hhmm.store(915);
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  CHECK(backend.submit_count.load() == 1);

  // 10:00: scheduled (50%) overtakes the filled fraction -> more slices place.
  fake_hhmm.store(1000);
  CHECK(PollUntil([&] { return backend.submit_count.load() >= 2; }, 3000));

  fake_hhmm.store(1330);  // hard stop -> Run() returns
  runner.join();
}

}  // namespace

int main() {
  TestQuoteDrivenComplete();
  TestHardStopAtClose();
  TestTwapPacingSteppedClock();
  if (g_failures == 0) {
    std::printf("test_engine: OK\n");
    return 0;
  }
  std::printf("test_engine: FAILED %d check(s)\n", g_failures);
  return 1;
}
