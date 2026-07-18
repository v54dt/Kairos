// RoundTripRunner against fakes (injected clock, scripted signal source, fake HOLD
// quote feed, fake legs) so every path is deterministic with no sockets or real
// time. Covers reverse-signal / stop / max-hold / stall / 13:25 forced exits, the
// ARMED-disarm and HOLD signal-loss policy branches, the exit-after-enter ordering,
// the halt-with-position safety invariants, and both degenerate-window guards.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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

// Taipei minutes -> a system_clock time_point (WallToMin adds +8h back out).
struct FakeClock {
  std::atomic<long long> wall_min{600};  // 10:00 Taipei, inside the arm window
  std::atomic<long long> mono_ms{0};
  EngineClock Make() {
    return EngineClock{
        [this] {
          return std::chrono::system_clock::time_point(std::chrono::minutes(wall_min.load() - 480));
        },
        [this] {
          return std::chrono::steady_clock::time_point(std::chrono::milliseconds(mono_ms.load()));
        }};
  }
  std::chrono::steady_clock::time_point Mono() const {
    return std::chrono::steady_clock::time_point(std::chrono::milliseconds(mono_ms.load()));
  }
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
    Callbacks cb = Get();
    if (cb.on_signal) cb.on_signal(a);
  }
  void Lost() {
    Callbacks cb = Get();
    if (cb.on_lost) cb.on_lost();
  }
  void Restored() {
    Callbacks cb = Get();
    if (cb.on_restored) cb.on_restored();
  }

 private:
  Callbacks Get() {
    std::lock_guard<std::mutex> lk(mu_);
    return cb_;
  }
  std::mutex mu_;
  Callbacks cb_;
};

class FakeQuoteSource : public QuoteSource {
 public:
  void SetCallback(QuoteFn on_quote) override {
    std::lock_guard<std::mutex> lk(mu_);
    on_quote_ = std::move(on_quote);
  }
  void SetTradeCallback(TradeFn) override {}
  void Start() override {}
  void Stop() override {}
  void Push(const std::string& symbol, const TopOfBook& tob) {
    QuoteFn fn;
    {
      std::lock_guard<std::mutex> lk(mu_);
      fn = on_quote_;
    }
    if (fn) fn(symbol, tob);
  }

 private:
  std::mutex mu_;
  QuoteFn on_quote_;
};

class FakeLeg : public LegRunner {
 public:
  FakeLeg(LegResult result, std::atomic<bool>* enter_returned)
      : result_(result), enter_returned_(enter_returned) {}
  LegResult Run() override {
    if (enter_returned_) enter_returned_->store(true);  // enter leg finished
    return result_;
  }
  void RequestStop() override {}

 private:
  LegResult result_;
  std::atomic<bool>* enter_returned_;
};

class FakeLegFactory : public LegFactory {
 public:
  LegResult enter_result{100, 1000000, 10000, true, false};  // 100 sh @ 100.00
  LegResult exit_result{100, 1000000, 10000, true, false};   // fully exited

  std::unique_ptr<LegRunner> Create(const Scenario& leg) override {
    std::lock_guard<std::mutex> lk(mu_);
    created_sides_.push_back(leg.side);
    created_names_.push_back(leg.name);
    if (leg.side == Side::kBuy) return std::make_unique<FakeLeg>(enter_result, &enter_returned_);
    // Ordering invariant: an exit leg is only ever built after the enter Run() returned.
    exit_saw_enter_returned_ = enter_returned_.load();
    return std::make_unique<FakeLeg>(exit_result, nullptr);
  }
  int CreateCount() {
    std::lock_guard<std::mutex> lk(mu_);
    return static_cast<int>(created_sides_.size());
  }
  bool ExitSawEnterReturned() {
    std::lock_guard<std::mutex> lk(mu_);
    return exit_saw_enter_returned_;
  }
  std::string NameAt(int i) {
    std::lock_guard<std::mutex> lk(mu_);
    return created_names_[i];
  }

 private:
  std::mutex mu_;
  std::vector<Side> created_sides_;
  std::vector<std::string> created_names_;
  std::atomic<bool> enter_returned_{false};
  bool exit_saw_enter_returned_ = false;
};

struct Rec {
  EventCategory category;
  Severity severity;
  std::string dedup_key;
  std::vector<std::pair<std::string, std::string>> fields;
};

class RecorderSink : public EventSink {
 public:
  void Emit(const Event& ev) override {
    std::lock_guard<std::mutex> lk(mu_);
    recs_.push_back({ev.category, ev.severity, ev.dedup_key, ev.fields});
  }
  bool Has(const std::string& key) {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& r : recs_)
      if (r.dedup_key == key) return true;
    return false;
  }
  bool HasField(const std::string& key, const std::string& f, const std::string& v) {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& r : recs_) {
      if (r.dedup_key != key) continue;
      for (const auto& kv : r.fields)
        if (kv.first == f && kv.second == v) return true;
    }
    return false;
  }
  Severity SeverityOf(const std::string& key) {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& r : recs_)
      if (r.dedup_key == key) return r.severity;
    return Severity::kInfo;
  }

 private:
  std::mutex mu_;
  std::vector<Rec> recs_;
};

Scenario BaseScenario() {
  Scenario s;
  s.symbol = "2330";
  s.side = Side::kBuy;
  s.roundtrip.enabled = true;
  s.roundtrip.signal = "vwap";
  s.roundtrip.stop_loss_pct = 2.0;  // percent, matching max_deviation_pct convention
  s.roundtrip.max_hold_min = 30;
  s.roundtrip.enter_window_min = 10;
  s.roundtrip.on_signal_loss = OnSignalLoss::kHoldWithStops;
  s.roundtrip.arm_start_hhmm = 900;
  s.roundtrip.arm_end_hhmm = 1300;
  s.quote_stall_alert_ms = 0;  // stall watchdog off unless a test enables it
  return s;
}

TopOfBook Book(Cents last, std::chrono::steady_clock::time_point recv) {
  TopOfBook t;
  t.bids[0] = {last, 100};
  t.asks[0] = {last, 100};
  t.n_bids = 1;
  t.n_asks = 1;
  t.last_trade = last;
  t.recv_ts = recv;
  t.valid = true;
  return t;
}

// Reach HOLD: wait for armed, fire an in-window enter, wait for the hold event.
void ArmAndEnter(FakeSignalSource& sig, RecorderSink& sink) {
  CHECK(WaitFor([&] { return sink.Has("rt:2330:armed"); }));
  sig.Signal(SignalAction::kEnter);
  CHECK(WaitFor([&] { return sink.Has("rt:2330:hold"); }));
}

void TestReverseSignalExit() {
  FakeClock clk;
  FakeSignalSource sig;
  FakeQuoteSource quotes;
  FakeLegFactory legs;
  RecorderSink sink;
  RoundTripRunner runner(BaseScenario(), &sig, &quotes, &legs, &sink, clk.Make());
  int rc = -1;
  std::thread th([&] { rc = runner.Run(); });
  ArmAndEnter(sig, sink);
  sig.Signal(SignalAction::kExit);
  th.join();
  CHECK_EQ(rc, 0);
  CHECK(sink.Has("rt:2330:flat"));
  CHECK(sink.HasField("rt:2330:exit", "reason",
                      std::to_string(static_cast<int>(ExitReason::kReverseSignal))));
  CHECK_EQ(legs.CreateCount(), 2);
}

void TestStopTrigger() {
  FakeClock clk;
  FakeSignalSource sig;
  FakeQuoteSource quotes;
  FakeLegFactory legs;
  RecorderSink sink;
  RoundTripRunner runner(BaseScenario(), &sig, &quotes, &legs, &sink, clk.Make());
  int rc = -1;
  std::thread th([&] { rc = runner.Run(); });
  ArmAndEnter(sig, sink);
  // entry avg 100.00, stop 2% => 98.00; a trade at 98.00 crosses the stop.
  quotes.Push("2330", Book(9800, clk.Mono()));
  th.join();
  CHECK_EQ(rc, 0);
  CHECK(sink.Has("rt:2330:flat"));
  CHECK(sink.HasField("rt:2330:exit", "reason",
                      std::to_string(static_cast<int>(ExitReason::kStopLoss))));
}

void TestMaxHoldExpiry() {
  FakeClock clk;
  FakeSignalSource sig;
  FakeQuoteSource quotes;
  FakeLegFactory legs;
  RecorderSink sink;
  Scenario s = BaseScenario();
  s.roundtrip.max_hold_min = 1;
  RoundTripRunner runner(std::move(s), &sig, &quotes, &legs, &sink, clk.Make());
  int rc = -1;
  std::thread th([&] { rc = runner.Run(); });
  ArmAndEnter(sig, sink);
  clk.mono_ms.store(60000);  // one minute of hold elapsed
  th.join();
  CHECK_EQ(rc, 0);
  CHECK(sink.HasField("rt:2330:exit", "reason",
                      std::to_string(static_cast<int>(ExitReason::kHoldTimeout))));
}

void TestQuoteStallForcedExit() {
  FakeClock clk;
  FakeSignalSource sig;
  FakeQuoteSource quotes;
  FakeLegFactory legs;
  RecorderSink sink;
  Scenario s = BaseScenario();
  s.quote_stall_alert_ms = 1000;
  RoundTripRunner runner(std::move(s), &sig, &quotes, &legs, &sink, clk.Make());
  int rc = -1;
  std::thread th([&] { rc = runner.Run(); });
  ArmAndEnter(sig, sink);
  quotes.Push("2330", Book(10000, clk.Mono()));  // recv at mono=0
  clk.mono_ms.store(3000);                       // 3s of silence > 1s threshold
  th.join();
  CHECK_EQ(rc, 0);
  CHECK(sink.HasField("rt:2330:exit", "reason",
                      std::to_string(static_cast<int>(ExitReason::kForcedTime))));
}

void TestForcedExit1325() {
  FakeClock clk;
  FakeSignalSource sig;
  FakeQuoteSource quotes;
  FakeLegFactory legs;
  RecorderSink sink;
  RoundTripRunner runner(BaseScenario(), &sig, &quotes, &legs, &sink, clk.Make());
  int rc = -1;
  std::thread th([&] { rc = runner.Run(); });
  ArmAndEnter(sig, sink);
  clk.wall_min.store(13 * 60 + 24);  // 13:24: watchdog forces the exit
  th.join();
  CHECK_EQ(rc, 0);
  CHECK(sink.Has("rt:2330:flat"));
  CHECK(sink.HasField("rt:2330:exit", "reason",
                      std::to_string(static_cast<int>(ExitReason::kForcedTime))));
}

void TestArmedSignalLostDisarm() {
  FakeClock clk;
  FakeSignalSource sig;
  FakeQuoteSource quotes;
  FakeLegFactory legs;
  RecorderSink sink;
  RoundTripRunner runner(BaseScenario(), &sig, &quotes, &legs, &sink, clk.Make());
  std::thread th([&] { runner.Run(); });
  CHECK(WaitFor([&] { return sink.Has("rt:2330:armed"); }));
  sig.Lost();
  CHECK(WaitFor([&] { return sink.Has("rt:2330:disarm"); }));
  sig.Restored();
  CHECK(WaitFor([&] { return sink.Has("rt:2330:rearm"); }));
  runner.RequestStop();
  th.join();
  CHECK_EQ(legs.CreateCount(), 0);  // never entered
  CHECK_EQ(static_cast<int>(sink.SeverityOf("rt:2330:disarm")),
           static_cast<int>(Severity::kWarning));
}

// Signal loss under hold_with_stops keeps the position; the independent watchdog
// still fires the stop. Proves protection does not ride on signal liveness.
void TestHoldSignalLostStopStillFires() {
  FakeClock clk;
  FakeSignalSource sig;
  FakeQuoteSource quotes;
  FakeLegFactory legs;
  RecorderSink sink;
  RoundTripRunner runner(BaseScenario(), &sig, &quotes, &legs, &sink, clk.Make());
  int rc = -1;
  std::thread th([&] { rc = runner.Run(); });
  ArmAndEnter(sig, sink);
  sig.Lost();  // hold_with_stops: stay in HOLD
  quotes.Push("2330", Book(9800, clk.Mono()));
  th.join();
  CHECK_EQ(rc, 0);
  CHECK(sink.HasField("rt:2330:exit", "reason",
                      std::to_string(static_cast<int>(ExitReason::kStopLoss))));
}

void TestHoldSignalLostExitPolicy() {
  FakeClock clk;
  FakeSignalSource sig;
  FakeQuoteSource quotes;
  FakeLegFactory legs;
  RecorderSink sink;
  Scenario s = BaseScenario();
  s.roundtrip.on_signal_loss = OnSignalLoss::kExit;
  RoundTripRunner runner(std::move(s), &sig, &quotes, &legs, &sink, clk.Make());
  int rc = -1;
  std::thread th([&] { rc = runner.Run(); });
  ArmAndEnter(sig, sink);
  sig.Lost();  // kExit policy: exit immediately
  th.join();
  CHECK_EQ(rc, 0);
  CHECK(sink.Has("rt:2330:flat"));
  CHECK(sink.HasField("rt:2330:exit", "reason",
                      std::to_string(static_cast<int>(ExitReason::kForcedTime))));
}

void TestExitAfterEnterOrdering() {
  FakeClock clk;
  FakeSignalSource sig;
  FakeQuoteSource quotes;
  FakeLegFactory legs;
  RecorderSink sink;
  RoundTripRunner runner(BaseScenario(), &sig, &quotes, &legs, &sink, clk.Make());
  int rc = -1;
  std::thread th([&] { rc = runner.Run(); });
  ArmAndEnter(sig, sink);
  sig.Signal(SignalAction::kExit);
  th.join();
  CHECK_EQ(rc, 0);
  CHECK(legs.ExitSawEnterReturned());  // exit leg built only after enter Run() returned
  CHECK(legs.NameAt(0).find("-enter") != std::string::npos);
  CHECK(legs.NameAt(1).find("-exit") != std::string::npos);
}

// Enter leg halts WITH fills: a position exists, so HOLD (protected), never FLAT.
void TestEnterHaltWithFillsHolds() {
  FakeClock clk;
  FakeSignalSource sig;
  FakeQuoteSource quotes;
  FakeLegFactory legs;
  legs.enter_result = {100, 1000000, 10000, false, true};  // halted, but 100 sh held
  RecorderSink sink;
  RoundTripRunner runner(BaseScenario(), &sig, &quotes, &legs, &sink, clk.Make());
  int rc = -1;
  std::thread th([&] { rc = runner.Run(); });
  CHECK(WaitFor([&] { return sink.Has("rt:2330:armed"); }));
  sig.Signal(SignalAction::kEnter);
  CHECK(WaitFor([&] { return sink.Has("rt:2330:hold"); }));  // protected, not flat
  CHECK(sink.Has("rt:2330:enter-halt"));
  CHECK(sink.HasField("rt:2330:hold", "held_shares", "100"));
  sig.Signal(SignalAction::kExit);
  th.join();
  CHECK_EQ(rc, 0);
}

// Exit leg halts with a position remaining: loud FATAL, never a silent FLAT.
void TestExitHaltLeavesPositionFatal() {
  FakeClock clk;
  FakeSignalSource sig;
  FakeQuoteSource quotes;
  FakeLegFactory legs;
  legs.exit_result = {40, 400000, 10000, false, true};  // sold 40 of 100, halted
  RecorderSink sink;
  RoundTripRunner runner(BaseScenario(), &sig, &quotes, &legs, &sink, clk.Make());
  int rc = -1;
  std::thread th([&] { rc = runner.Run(); });
  ArmAndEnter(sig, sink);
  sig.Signal(SignalAction::kExit);
  th.join();
  CHECK_EQ(rc, 1);
  CHECK(!sink.Has("rt:2330:flat"));
  CHECK(sink.HasField("rt:2330:failed", "remaining_shares", "60"));
  CHECK_EQ(static_cast<int>(sink.SeverityOf("rt:2330:failed")), static_cast<int>(Severity::kError));
}

// Operator stop (SIGINT/SIGTERM) while a position is held: fail-closed with a loud
// FATAL and non-zero exit, never a silent rc=0 that reads as a clean flat.
void TestStopDuringHoldFailsClosed() {
  FakeClock clk;
  FakeSignalSource sig;
  FakeQuoteSource quotes;
  FakeLegFactory legs;
  RecorderSink sink;
  RoundTripRunner runner(BaseScenario(), &sig, &quotes, &legs, &sink, clk.Make());
  int rc = -1;
  std::thread th([&] { rc = runner.Run(); });
  ArmAndEnter(sig, sink);  // 100 sh held
  runner.RequestStop();
  th.join();
  CHECK_EQ(rc, 1);
  CHECK(!sink.Has("rt:2330:flat"));
  CHECK(sink.HasField("rt:2330:stopped", "remaining_shares", "100"));
  CHECK_EQ(static_cast<int>(sink.SeverityOf("rt:2330:stopped")),
           static_cast<int>(Severity::kError));
}

void TestDegenerateEnterGuard() {
  FakeClock clk;
  clk.wall_min.store(13 * 60 + 26);  // 13:26 >= 13:25: derive window would be degenerate
  FakeSignalSource sig;
  FakeQuoteSource quotes;
  FakeLegFactory legs;
  RecorderSink sink;
  Scenario s = BaseScenario();
  s.roundtrip.arm_end_hhmm = 1400;  // widen the window so the FSM accepts the enter
  RoundTripRunner runner(std::move(s), &sig, &quotes, &legs, &sink, clk.Make());
  std::thread th([&] { runner.Run(); });
  CHECK(WaitFor([&] { return sink.Has("rt:2330:armed"); }));
  sig.Signal(SignalAction::kEnter);
  CHECK(WaitFor([&] { return sink.Has("rt:2330:enter-degenerate"); }));
  runner.RequestStop();
  th.join();
  CHECK_EQ(legs.CreateCount(), 0);  // no leg derived past 13:25
}

void TestDegenerateExitGuard() {
  FakeClock clk;
  FakeSignalSource sig;
  FakeQuoteSource quotes;
  FakeLegFactory legs;
  RecorderSink sink;
  RoundTripRunner runner(BaseScenario(), &sig, &quotes, &legs, &sink, clk.Make());
  int rc = -1;
  std::thread th([&] { rc = runner.Run(); });
  ArmAndEnter(sig, sink);
  clk.wall_min.store(13 * 60 + 26);  // 13:26: exit cannot cross to close
  th.join();
  CHECK_EQ(rc, 1);
  CHECK(!sink.Has("rt:2330:flat"));
  CHECK(sink.HasField("rt:2330:exit-degenerate", "remaining_shares", "100"));
  CHECK_EQ(legs.CreateCount(), 1);  // only the enter leg was ever built
}

}  // namespace

int main() {
  TestReverseSignalExit();
  TestStopTrigger();
  TestMaxHoldExpiry();
  TestQuoteStallForcedExit();
  TestForcedExit1325();
  TestArmedSignalLostDisarm();
  TestHoldSignalLostStopStillFires();
  TestHoldSignalLostExitPolicy();
  TestExitAfterEnterOrdering();
  TestEnterHaltWithFillsHolds();
  TestExitHaltLeavesPositionFatal();
  TestStopDuringHoldFailsClosed();
  TestDegenerateEnterGuard();
  TestDegenerateExitGuard();
  if (g_failures == 0) {
    std::printf("test_roundtrip_runner: OK\n");
    return 0;
  }
  std::printf("test_roundtrip_runner: FAILED %d check(s)\n", g_failures);
  return 1;
}
