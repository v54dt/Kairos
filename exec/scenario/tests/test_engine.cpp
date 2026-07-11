// Drives ScenarioEngine through its injected seams (QuoteSource + EngineClock)
// with no UDS socket and no real clock, proving the engine is replay-ready.
// FakeQuoteSource delivers synthetic quotes synchronously (production delivers
// on the UDS reader thread); the fake clock makes session gating deterministic.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "engine.h"
#include "event.h"
#include "event_sink.h"
#include "order_backend.h"
#include "queue_sim_backend.h"
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
  void SetTradeCallback(TradeFn on_trade) override { on_trade_ = std::move(on_trade); }
  void Start() override {}
  void Stop() override {}
  void Push(const std::string& symbol, const TopOfBook& tob) {
    if (on_quote_) on_quote_(symbol, tob);
  }
  // Exercises the engine's trade subscription, registered only when the backend
  // asks for trades (WantsMarketTrades()).
  void PushTrade(const std::string& symbol, const Trade& t) {
    if (on_trade_) on_trade_(symbol, t);
  }
  bool HasTradeCallback() const { return static_cast<bool>(on_trade_); }

 private:
  QuoteFn on_quote_;
  TradeFn on_trade_;
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

// Records submits/cancels and NEVER acks: every order ack-times-out, like 7/6.
class NoAckBackend : public OrderBackend {
 public:
  bool Connect() override {
    connected_ = true;
    return true;
  }
  void Disconnect() override { connected_ = false; }
  bool IsConnected() const override { return connected_; }
  void Submit(const OrderSubmitMsg& o) override {
    std::lock_guard<std::mutex> lock(mu_);
    submits.push_back(o.id);
  }
  void Cancel(const std::string& id) override {
    std::lock_guard<std::mutex> lock(mu_);
    cancels.push_back(id);
  }
  std::vector<std::string> Submits() const {
    std::lock_guard<std::mutex> lock(mu_);
    return submits;
  }
  std::vector<std::string> Cancels() const {
    std::lock_guard<std::mutex> lock(mu_);
    return cancels;
  }

 private:
  mutable std::mutex mu_;
  std::vector<std::string> submits;
  std::vector<std::string> cancels;
  bool connected_ = false;
};

// Rejects every submit synchronously (a submit-reject storm).
class RejectingBackend : public OrderBackend {
 public:
  bool Connect() override {
    connected_ = true;
    return true;
  }
  void Disconnect() override { connected_ = false; }
  bool IsConnected() const override { return connected_; }
  void Submit(const OrderSubmitMsg& o) override {
    ++submit_count;
    if (on_ack_) on_ack_(o.id, false, "rejected");
  }
  void Cancel(const std::string&) override {}
  std::atomic<int> submit_count{0};

 private:
  bool connected_ = false;
};

// Ack-times-out the first `fail_first` submits (no callback), then acks+fully
// fills — proves a transient timeout does not halt once acks resume.
class FlakyThenOkBackend : public OrderBackend {
 public:
  explicit FlakyThenOkBackend(int fail_first) : fail_first_(fail_first) {}
  bool Connect() override {
    connected_ = true;
    return true;
  }
  void Disconnect() override { connected_ = false; }
  bool IsConnected() const override { return connected_; }
  void Submit(const OrderSubmitMsg& o) override {
    if (++submit_count <= fail_first_) return;  // no ack -> ack-timeout
    if (on_ack_) on_ack_(o.id, true, "");
    if (on_fill_) on_fill_(o.id, Fill{o.shares, o.price});
  }
  void Cancel(const std::string&) override { ++cancel_count; }
  std::atomic<int> submit_count{0};
  std::atomic<int> cancel_count{0};

 private:
  int fail_first_;
  bool connected_ = false;
};

// Acks and fully fills every submit synchronously, tallying shares — used to prove
// the sell cap and shares-goal completion without a real fill model.
class InstantFillBackend : public OrderBackend {
 public:
  bool Connect() override {
    connected_ = true;
    return true;
  }
  void Disconnect() override { connected_ = false; }
  bool IsConnected() const override { return connected_; }
  void Submit(const OrderSubmitMsg& o) override {
    ++submit_count;
    total_shares += o.shares;
    if (on_ack_) on_ack_(o.id, true, "");
    if (on_fill_) on_fill_(o.id, Fill{o.shares, o.price});
  }
  void Cancel(const std::string&) override {}
  std::atomic<int> submit_count{0};
  std::atomic<long> total_shares{0};

 private:
  bool connected_ = false;
};

// EngineClock whose mono advances 2s per read so SdkGate never sleeps and the
// ack-timeout watchdog fires immediately (sub-second tests).
EngineClock SteppedMonoClock(std::shared_ptr<std::atomic<long>> mono_ns) {
  EngineClock clock;
  clock.mono = [mono_ns] {
    long v = mono_ns->fetch_add(2'000'000'000L);
    return std::chrono::steady_clock::time_point(std::chrono::nanoseconds(v));
  };
  return clock;
}

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

// Captures fill sizes and the terminal event's fee so the test can assert
// non-instant fills and that the fee model ran on sim fills.
class RecordingSink : public EventSink {
 public:
  void Emit(const Event& ev) override {
    std::lock_guard<std::mutex> lock(mu_);
    if (ev.category == EventCategory::kFill) {
      for (const auto& [k, v] : ev.fields)
        if (k == "shares") fills_.push_back(std::stol(v));
    } else if (ev.category == EventCategory::kComplete || ev.category == EventCategory::kShutdown ||
               ev.category == EventCategory::kIncomplete) {
      for (const auto& [k, v] : ev.fields) {
        if (k == "fee") final_fee_ = std::stol(v);
        if (k == "tax") final_tax_ = std::stol(v);
      }
    } else if (ev.category == EventCategory::kError && ev.dedup_key.rfind("halt:", 0) == 0) {
      for (const auto& [k, v] : ev.fields)
        if (k == "reason") halt_reason_ = v;
    }
  }
  std::string HaltReason() const {
    std::lock_guard<std::mutex> lock(mu_);
    return halt_reason_;
  }
  long TotalFilled() const {
    std::lock_guard<std::mutex> lock(mu_);
    long n = 0;
    for (long f : fills_) n += f;
    return n;
  }
  long MaxSingleFill() const {
    std::lock_guard<std::mutex> lock(mu_);
    long m = 0;
    for (long f : fills_) m = std::max(m, f);
    return m;
  }
  long FinalFee() const {
    std::lock_guard<std::mutex> lock(mu_);
    return final_fee_;
  }
  long FinalTax() const {
    std::lock_guard<std::mutex> lock(mu_);
    return final_tax_;
  }

 private:
  mutable std::mutex mu_;
  std::vector<long> fills_;
  long final_fee_ = 0;
  long final_tax_ = 0;
  std::string halt_reason_;
};

// Queue-sim backend that counts Submit so the test can wait for the engine to
// rest an order before printing trades against it.
class CountingQueueSim : public QueueSimBackend {
 public:
  using QueueSimBackend::QueueSimBackend;
  void Submit(const OrderSubmitMsg& o) override {
    ++submit_count;
    QueueSimBackend::Submit(o);
  }
  std::atomic<int> submit_count{0};
};

// A passive join order fed through the engine + queue-sim backend fills only
// partially against a print that clears the displayed queue ahead, and the fee
// model runs on the sim fill (fee > 0) exactly as it would on paper/live.
void TestQueueSimEngineFills() {
  constexpr std::int64_t kCont = 7200LL * 1000000;  // 10:00 Taipei, continuous
  Scenario s = BaseScenario();
  s.board = Board::kRoundLot;  // the fill model rejects odd-lot submits
  s.price_policy = PricePolicy::kJoin;
  s.pacing = Pacing::kAsap;
  s.shares_per_order = 2000;
  s.budget_twd = 100000000;  // never completes on its own; stopped explicitly

  CountingQueueSim backend(FillMode::kProbQueue, {"2330"});
  RecordingSink sink;
  FakeQuoteSource quotes;
  std::atomic<long> mono_ns{0};
  EngineClock clock;
  clock.mono = [&mono_ns] {
    long v = mono_ns.fetch_add(2'000'000'000L);  // +2s per read: never gates
    return std::chrono::steady_clock::time_point(std::chrono::nanoseconds(v));
  };
  ScenarioEngine engine(std::move(s), &backend, &sink, &quotes, clock);
  engine.set_ignore_window(true);

  // The engine registers the trade subscription only because the queue-sim
  // backend asks for trades.
  CHECK(quotes.HasTradeCallback());

  std::thread runner([&engine] { engine.Run(); });

  // Two-sided book: 1000 displayed at the bid the join order joins; ask a tick
  // above so the join BUY rests behind the 1000 queue.
  TopOfBook book;
  book.bids[0] = {10000, 1000};
  book.asks[0] = {10010, 1000};
  book.n_bids = 1;
  book.n_asks = 1;
  book.quote_ts_us = kCont;
  book.valid = true;
  quotes.Push("2330", book);
  CHECK(PollUntil([&] { return backend.submit_count.load() >= 1; }, 3000));

  // A 1500-share print clears the 1000 ahead and fills only the 500 beyond it.
  quotes.PushTrade("2330", Trade{10000, 1500, kCont + 1000, false});
  CHECK(PollUntil([&] { return sink.TotalFilled() >= 500; }, 3000));
  CHECK(sink.TotalFilled() == 500);  // partial: NOT the 2000 an instant fill gives
  CHECK(sink.MaxSingleFill() < 2000);

  engine.RequestStop();
  runner.join();
  CHECK(sink.FinalFee() > 0);  // the fee model ran on the sim fill
  std::printf("queue-sim engine: filled %ld / 2000 sh, fee NT$ %ld\n", sink.TotalFilled(),
              sink.FinalFee());
}

// A scenario tuned for the fail-closed halt tests: no window gating, no journal
// requirement (paper), a tiny ack-timeout, and a 3-failure halt cap.
Scenario HaltScenario() {
  Scenario s = BaseScenario();
  s.pacing = Pacing::kAsap;
  s.budget_twd = 1000000;  // large: keeps re-placing until the cap halts it
  s.ack_timeout_ms = 1;    // any un-acked order times out on the next stepped read
  s.max_consecutive_order_failures = 3;
  return s;
}

// Ack-timeout: the timed-out (possibly-live) order is cancelled before re-placing,
// and N consecutive timeouts halt the run with a non-empty terminal reason.
void TestAckTimeoutCancelsThenHalts() {
  Scenario s = HaltScenario();
  NoAckBackend backend;
  RecordingSink sink;
  FakeQuoteSource quotes;
  auto mono = std::make_shared<std::atomic<long>>(0);
  ScenarioEngine engine(std::move(s), &backend, &sink, &quotes, SteppedMonoClock(mono));
  engine.set_ignore_window(true);
  quotes.Push("2330", ValidBook(FloatToCents(100.0)));

  int rc = engine.Run();

  CHECK(rc != 0);  // fail-closed exit -> supervisor classifies crashed
  auto submits = backend.Submits();
  auto cancels = backend.Cancels();
  CHECK(submits.size() == 3);  // 3 failures then halt: no 4th submit
  CHECK(cancels.size() == 3);  // each timed-out order was cancelled before forgetting
  for (std::size_t i = 0; i < cancels.size() && i < submits.size(); ++i)
    CHECK(cancels[i] == submits[i]);  // cancel targets the exact timed-out id
  CHECK(!sink.HaltReason().empty());  // ntfy shows a real message, never a bare "triggered"
  std::printf("halt(ack-timeout): submits=%zu cancels=%zu reason=\"%s\" rc=%d\n", submits.size(),
              cancels.size(), sink.HaltReason().c_str(), rc);
}

// A submit-reject storm halts the same way (shared consecutive-failure counter).
void TestRejectStormHalts() {
  Scenario s = HaltScenario();
  RejectingBackend backend;
  RecordingSink sink;
  FakeQuoteSource quotes;
  auto mono = std::make_shared<std::atomic<long>>(0);
  ScenarioEngine engine(std::move(s), &backend, &sink, &quotes, SteppedMonoClock(mono));
  engine.set_ignore_window(true);
  quotes.Push("2330", ValidBook(FloatToCents(100.0)));

  int rc = engine.Run();

  CHECK(rc != 0);
  CHECK(backend.submit_count.load() == 3);  // halted after 3 rejects, no further submits
  CHECK(!sink.HaltReason().empty());
}

// One transient ack-timeout then acks resume: the counter resets, no halt.
void TestTransientTimeoutDoesNotHalt() {
  Scenario s = HaltScenario();
  s.budget_twd = 1000;  // one slice: completes as soon as an ack+fill lands
  FlakyThenOkBackend backend(/*fail_first=*/1);
  RecordingSink sink;
  FakeQuoteSource quotes;
  auto mono = std::make_shared<std::atomic<long>>(0);
  ScenarioEngine engine(std::move(s), &backend, &sink, &quotes, SteppedMonoClock(mono));
  engine.set_ignore_window(true);
  quotes.Push("2330", ValidBook(FloatToCents(100.0)));

  int rc = engine.Run();

  CHECK(rc == 0);  // no halt: the good ack cleared the streak
  CHECK(sink.HaltReason().empty());
  CHECK(backend.cancel_count.load() == 1);  // the one timed-out order was cancelled
}

// FIX B: a live run with an un-openable journal refuses to start (non-zero); the
// same un-openable journal in paper only warns and runs to completion.
void TestLiveRequiresJournal() {
  {
    Scenario s = BaseScenario();
    s.pacing = Pacing::kAsap;
    s.budget_twd = 1000;
    s.live = true;
    s.journal_dir = "/dev/null/nope";  // un-openable (not a directory)
    PaperOrderBackend backend;
    NullEventSink sink;
    FakeQuoteSource quotes;
    ScenarioEngine engine(std::move(s), &backend, &sink, &quotes);
    engine.set_ignore_window(true);
    CHECK(engine.Run() != 0);  // fail-closed: no journal, no live trading
  }
  {
    Scenario s = BaseScenario();
    s.pacing = Pacing::kAsap;
    s.budget_twd = 1000;
    s.live = false;
    s.journal_dir = "/dev/null/nope";
    PaperOrderBackend backend;
    NullEventSink sink;
    FakeQuoteSource quotes;
    ScenarioEngine engine(std::move(s), &backend, &sink, &quotes);
    engine.set_ignore_window(true);
    quotes.Push("2330", ValidBook(FloatToCents(100.0)));
    CHECK(engine.Run() == 0);  // paper: warn and continue
  }
}

// Counts .jsonl files under a directory (0 if the directory does not exist).
static int CountJournalFiles(const std::string& dir) {
  int n = 0;
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
    if (e.path().extension() == ".jsonl") ++n;
  }
  return n;
}

// FIX B real proof, contamination-guarded: a LIVE run with no [journal] defaults to
// $HOME/Kairos/data/journal and writes a fill record there, but a PAPER run with the
// same (empty) config writes NOTHING to that dir, so simulated fills can never land
// in the journal a live run replays from.
void TestJournalDefaultDirWritten() {
  std::string home = "/tmp/kairos-home-" + std::to_string(::getpid());
  const std::string journal_dir = home + "/Kairos/data/journal";
  const char* toml_body = R"(
[scenario]
symbol = "2330"
board = "OddLot"
side = "Buy"
budget_twd = 1000
shares_per_order = 10
pacing = "asap"
[pricing]
policy = "cross"
reference_price = 100.0
[risk]
quote_max_age_ms = 0
)";

  // Paper first: no [journal] must leave the default dir untouched.
  std::filesystem::remove_all(home);
  std::filesystem::create_directories(home);
  ::setenv("HOME", home.c_str(), 1);
  std::string toml = home + "/s.toml";
  std::ofstream(toml) << toml_body;
  {
    Scenario s = LoadScenario(toml);
    CHECK(s.journal_dir.empty());  // parser no longer defaults
    s.live = false;
    RecordingPaperBackend backend;
    NullEventSink sink;
    FakeQuoteSource quotes;
    ScenarioEngine engine(std::move(s), &backend, &sink, &quotes);
    engine.set_ignore_window(true);
    quotes.Push("2330", ValidBook(FloatToCents(100.0)));
    CHECK(engine.Run() == 0);
    CHECK(backend.submit_count.load() > 0);      // paper really filled
    CHECK(CountJournalFiles(journal_dir) == 0);  // yet wrote no journal
  }
  std::printf("paper no-contamination: %s -> %d journal files\n", journal_dir.c_str(),
              CountJournalFiles(journal_dir));

  // Live: the same empty config now defaults the dir and writes a fill record.
  {
    Scenario s = LoadScenario(toml);
    s.live = true;
    RecordingPaperBackend backend;
    NullEventSink sink;
    FakeQuoteSource quotes;
    ScenarioEngine engine(std::move(s), &backend, &sink, &quotes);
    engine.set_ignore_window(true);
    quotes.Push("2330", ValidBook(FloatToCents(100.0)));
    CHECK(engine.Run() == 0);
    CHECK(CountJournalFiles(journal_dir) == 1);
  }
  std::printf("journal default (live): %s -> %d journal files\n", journal_dir.c_str(),
              CountJournalFiles(journal_dir));
  std::filesystem::remove_all(home);
}

// A SELL never sells more than position_shares even when the TWD budget is far
// larger: cumulative fills stop exactly at the held position, and the tax accrues.
void TestSellPositionCap() {
  Scenario s = BaseScenario();
  s.side = Side::kSell;
  s.pacing = Pacing::kAsap;
  s.reference_price = 100.0;
  s.shares_per_order = 10;
  s.position_shares = 30;    // hard cap
  s.budget_twd = 100000000;  // budget could buy far more; the cap must bind first

  InstantFillBackend backend;
  RecordingSink sink;
  FakeQuoteSource quotes;
  auto mono = std::make_shared<std::atomic<long>>(0);
  ScenarioEngine engine(std::move(s), &backend, &sink, &quotes, SteppedMonoClock(mono));
  engine.set_ignore_window(true);
  quotes.Push("2330", ValidBook(FloatToCents(100.0)));

  engine.Run();

  CHECK(backend.submit_count.load() == 3);   // 10 + 10 + 10 = 30, then cap stops it
  CHECK(backend.total_shares.load() == 30);  // never oversells the position
  CHECK(sink.TotalFilled() == 30);
  CHECK(sink.FinalTax() == 9);  // 3 fills of NT$1000 @ 0.3% -> 3 each
  std::printf("sell cap: submits=%d shares=%ld tax=%ld\n", backend.submit_count.load(),
              backend.total_shares.load(), sink.FinalTax());
}

// budget_shares completes at exactly the share goal (buy side), the last slice
// clamps to the remainder, and a buy accrues no tax.
void TestBuyBudgetShares() {
  Scenario s = BaseScenario();
  s.side = Side::kBuy;
  s.pacing = Pacing::kAsap;
  s.reference_price = 100.0;
  s.shares_per_order = 10;
  s.budget_twd = 0;
  s.budget_shares = 25;  // 10 + 10 + 5

  InstantFillBackend backend;
  RecordingSink sink;
  FakeQuoteSource quotes;
  auto mono = std::make_shared<std::atomic<long>>(0);
  ScenarioEngine engine(std::move(s), &backend, &sink, &quotes, SteppedMonoClock(mono));
  engine.set_ignore_window(true);
  quotes.Push("2330", ValidBook(FloatToCents(100.0)));

  engine.Run();

  CHECK(backend.total_shares.load() == 25);  // exactly the share goal, no overshoot
  CHECK(sink.TotalFilled() == 25);
  CHECK(sink.FinalTax() == 0);  // buys pay no tax
  std::printf("buy budget_shares: submits=%d shares=%ld\n", backend.submit_count.load(),
              backend.total_shares.load());
}

// Journal replay restores the sold tally so a restart never re-sells: a first run
// sells the whole position, a second run over the same journal submits nothing.
void TestSellCapReplayResume() {
  std::string dir = "/tmp/kairos-sellcap-" + std::to_string(::getpid());
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  EngineClock clock;
  clock.wall = [] { return WallAt(2026, 7, 3, 10, 0); };  // fixed date -> stable journal name
  auto mono = std::make_shared<std::atomic<long>>(0);
  clock.mono = SteppedMonoClock(mono).mono;

  auto make = [&](Scenario s) {
    s.side = Side::kSell;
    s.pacing = Pacing::kAsap;
    s.reference_price = 100.0;
    s.shares_per_order = 10;
    s.position_shares = 30;
    s.budget_twd = 100000000;
    s.journal_dir = dir;
    return s;
  };

  {
    InstantFillBackend backend;
    RecordingSink sink;
    FakeQuoteSource quotes;
    ScenarioEngine engine(make(BaseScenario()), &backend, &sink, &quotes, clock);
    engine.set_ignore_window(true);
    quotes.Push("2330", ValidBook(FloatToCents(100.0)));
    engine.Run();
    CHECK(backend.total_shares.load() == 30);
  }
  {
    InstantFillBackend backend;
    RecordingSink sink;
    FakeQuoteSource quotes;
    ScenarioEngine engine(make(BaseScenario()), &backend, &sink, &quotes, clock);
    engine.set_ignore_window(true);
    quotes.Push("2330", ValidBook(FloatToCents(100.0)));
    engine.Run();
    CHECK(backend.submit_count.load() == 0);  // replay saw 30 already sold: cap == 0
    CHECK(backend.total_shares.load() == 0);  // no re-sell of the closed position
  }
  std::printf("sell cap replay: no re-sell after restart\n");
  std::filesystem::remove_all(dir);
}

}  // namespace

int main() {
  TestQuoteDrivenComplete();
  TestSellPositionCap();
  TestBuyBudgetShares();
  TestSellCapReplayResume();
  TestHardStopAtClose();
  TestTwapPacingSteppedClock();
  TestQueueSimEngineFills();
  TestAckTimeoutCancelsThenHalts();
  TestRejectStormHalts();
  TestTransientTimeoutDoesNotHalt();
  TestLiveRequiresJournal();
  TestJournalDefaultDirWritten();
  if (g_failures == 0) {
    std::printf("test_engine: OK\n");
    return 0;
  }
  std::printf("test_engine: FAILED %d check(s)\n", g_failures);
  return 1;
}
