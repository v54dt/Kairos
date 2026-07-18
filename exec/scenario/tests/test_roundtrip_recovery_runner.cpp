// Restart recovery driven through the REAL RoundTripRunner against fakes: seed the
// Buy/Sell/rt journals as a crash would leave them, construct a fresh runner over the
// same journal dir, and assert the resumed state. This is the §7.7 property test:
// a kill in HOLD -> restart -> the stop still fires -> the position exits. Also covers
// degraded (fills without an enter_done line), one-trip-per-day after restart, the
// net<0 refusal, and past-13:25 recovery going straight to the forced-exit fatal.

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "order_journal.h"
#include "roundtrip_journal.h"
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

struct FakeClock {
  std::atomic<long long> wall_min{600};  // 10:00 Taipei
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

 private:
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
  explicit FakeLeg(LegResult r) : result_(r) {}
  LegResult Run() override { return result_; }
  void RequestStop() override {}

 private:
  LegResult result_;
};

class FakeLegFactory : public LegFactory {
 public:
  LegResult exit_result{300, 300 * 56000, 56000, true, false};
  std::unique_ptr<LegRunner> Create(const Scenario& leg) override {
    std::lock_guard<std::mutex> lk(mu_);
    created_sides_.push_back(leg.side);
    if (leg.side == Side::kBuy)
      return std::make_unique<FakeLeg>(LegResult{300, 300 * 58000, 58000, true, false});
    return std::make_unique<FakeLeg>(exit_result);
  }
  int CreateCount() {
    std::lock_guard<std::mutex> lk(mu_);
    return static_cast<int>(created_sides_.size());
  }
  Side SideAt(int i) {
    std::lock_guard<std::mutex> lk(mu_);
    return created_sides_[i];
  }

 private:
  std::mutex mu_;
  std::vector<Side> created_sides_;
};

class RecorderSink : public EventSink {
 public:
  void Emit(const Event& ev) override {
    std::lock_guard<std::mutex> lk(mu_);
    recs_.push_back({ev.dedup_key, ev.fields});
  }
  bool Has(const std::string& key) {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& r : recs_)
      if (r.first == key) return true;
    return false;
  }
  bool HasField(const std::string& key, const std::string& f, const std::string& v) {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& r : recs_) {
      if (r.first != key) continue;
      for (const auto& kv : r.second)
        if (kv.first == f && kv.second == v) return true;
    }
    return false;
  }

 private:
  std::mutex mu_;
  std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>> recs_;
};

Scenario BaseScenario(const std::string& journal_dir) {
  Scenario s;
  s.symbol = "2330";
  s.side = Side::kBuy;
  s.journal_dir = journal_dir;
  s.roundtrip.enabled = true;
  s.roundtrip.signal = "vwap";
  s.roundtrip.stop_loss_pct = 2.0;  // avg 580.00 -> stop 568.40
  s.roundtrip.max_hold_min = 30;
  s.roundtrip.enter_window_min = 10;
  s.roundtrip.on_signal_loss = OnSignalLoss::kHoldWithStops;
  s.roundtrip.arm_start_hhmm = 900;
  s.roundtrip.arm_end_hhmm = 1300;
  s.quote_stall_alert_ms = 0;
  return s;
}

TopOfBook Book(Cents last) {
  TopOfBook t;
  t.bids[0] = {last, 100};
  t.asks[0] = {last, 100};
  t.n_bids = 1;
  t.n_asks = 1;
  t.last_trade = last;
  t.valid = true;
  return t;
}

std::string Day(FakeClock& clk) { return TradingDayUtc8(clk.Make().wall()); }

// Wall-clock epoch microseconds the fake clock reports, so a seeded enter_done ts
// yields ~0 elapsed hold (the max-hold countdown starts fresh, not already expired).
long NowUs(FakeClock& clk) { return (clk.wall_min.load() - 480) * 60 * 1000000L; }

std::string TempDir() {
  static std::atomic<int> ctr{0};
  std::string d =
      (std::filesystem::temp_directory_path() /
       ("kairos-rt-recover-" + std::to_string(::getpid()) + "-" + std::to_string(ctr.fetch_add(1))))
          .string();
  std::filesystem::remove_all(d);
  std::filesystem::create_directories(d);
  return d;
}

// §7.7: a kill in HOLD leaves Buy fills + an rt enter_done. On restart the runner
// resumes HOLD, the stop watchdog re-arms immediately, a sub-stop quote fires it,
// and the position exits FLAT. The enter leg is NEVER re-run (no double-buy).
void TestResumeHoldStopFires() {
  const std::string dir = TempDir();
  FakeClock clk;
  const std::string day = Day(clk);
  CHECK(OrderJournal::AppendFill(dir, "2330-Buy-" + day, "k-1", 300, 58000));
  CHECK(RoundTripJournal::Arm(dir, "2330-rt-" + day));
  CHECK(RoundTripJournal::Trigger(dir, "2330-rt-" + day, "vwap", 1, NowUs(clk)));
  CHECK(RoundTripJournal::EnterDone(dir, "2330-rt-" + day, 300, 58000, NowUs(clk)));

  FakeSignalSource sig;
  FakeQuoteSource quotes;
  FakeLegFactory legs;
  RecorderSink sink;
  RoundTripRunner runner(BaseScenario(dir), &sig, &quotes, &legs, &sink, clk.Make());
  int rc = -1;
  std::thread th([&] { rc = runner.Run(); });

  CHECK(WaitFor([&] { return sink.Has("rt:2330:recover-hold"); }));
  CHECK(sink.HasField("rt:2330:recover-hold", "held_shares", "300"));
  CHECK(!sink.Has("rt:2330:armed"));  // resumed, not freshly armed
  quotes.Push("2330", Book(56000));   // below the 568.40 stop
  th.join();

  CHECK_EQ(rc, 0);
  CHECK(sink.Has("rt:2330:flat"));
  CHECK(sink.HasField("rt:2330:exit", "reason",
                      std::to_string(static_cast<int>(ExitReason::kStopLoss))));
  CHECK_EQ(legs.CreateCount(), 1);  // only the exit leg — enter never re-run
  CHECK(legs.SideAt(0) == Side::kSell);
  std::filesystem::remove_all(dir);
}

// Fills exist but the rt enter_done line never landed (crash in the gap): still
// resume HOLD, degraded, entry avg derived from the Buy fills; the stop still fires.
void TestDegradedResumeStopFires() {
  const std::string dir = TempDir();
  FakeClock clk;
  const std::string day = Day(clk);
  CHECK(OrderJournal::AppendFill(dir, "2330-Buy-" + day, "k-1", 300, 58000));
  CHECK(RoundTripJournal::Arm(dir, "2330-rt-" + day));
  CHECK(RoundTripJournal::Trigger(dir, "2330-rt-" + day, "vwap", 1, 1));  // no enter_done

  FakeSignalSource sig;
  FakeQuoteSource quotes;
  FakeLegFactory legs;
  RecorderSink sink;
  RoundTripRunner runner(BaseScenario(dir), &sig, &quotes, &legs, &sink, clk.Make());
  int rc = -1;
  std::thread th([&] { rc = runner.Run(); });

  CHECK(WaitFor([&] { return sink.Has("rt:2330:recover-degraded"); }));
  CHECK(sink.HasField("rt:2330:recover-degraded", "entry_avg_cents", "58000"));
  quotes.Push("2330", Book(56000));
  th.join();
  CHECK_EQ(rc, 0);
  CHECK(sink.HasField("rt:2330:exit", "reason",
                      std::to_string(static_cast<int>(ExitReason::kStopLoss))));
  std::filesystem::remove_all(dir);
}

// One trip per day survives a restart: net==0 with a recorded trigger => terminal,
// go straight to done without re-entering.
void TestTerminalAfterRestart() {
  const std::string dir = TempDir();
  FakeClock clk;
  const std::string day = Day(clk);
  CHECK(OrderJournal::AppendFill(dir, "2330-Buy-" + day, "k-1", 300, 58000));
  CHECK(OrderJournal::AppendFill(dir, "2330-Sell-" + day, "k-2", 300, 56000));
  CHECK(RoundTripJournal::Arm(dir, "2330-rt-" + day));
  CHECK(RoundTripJournal::Trigger(dir, "2330-rt-" + day, "vwap", 1, 1));
  CHECK(RoundTripJournal::EnterDone(dir, "2330-rt-" + day, 300, 58000, 1));
  CHECK(RoundTripJournal::Flat(dir, "2330-rt-" + day, 2));

  FakeSignalSource sig;
  FakeQuoteSource quotes;
  FakeLegFactory legs;
  RecorderSink sink;
  RoundTripRunner runner(BaseScenario(dir), &sig, &quotes, &legs, &sink, clk.Make());
  int rc = runner.Run();  // terminal recovery returns without entering the loop
  CHECK_EQ(rc, 0);
  CHECK(sink.Has("rt:2330:recover-done"));
  CHECK(!sink.Has("rt:2330:armed"));
  CHECK_EQ(legs.CreateCount(), 0);  // never re-entered
  std::filesystem::remove_all(dir);
}

// Inconsistent books (more sold than bought): refuse to start, loud fail-closed.
void TestNetShortRefuses() {
  const std::string dir = TempDir();
  FakeClock clk;
  const std::string day = Day(clk);
  CHECK(OrderJournal::AppendFill(dir, "2330-Buy-" + day, "k-1", 100, 58000));
  CHECK(OrderJournal::AppendFill(dir, "2330-Sell-" + day, "k-2", 300, 56000));

  FakeSignalSource sig;
  FakeQuoteSource quotes;
  FakeLegFactory legs;
  RecorderSink sink;
  RoundTripRunner runner(BaseScenario(dir), &sig, &quotes, &legs, &sink, clk.Make());
  int rc = runner.Run();
  CHECK_EQ(rc, 1);
  CHECK(sink.Has("rt:2330:recover-refuse"));
  CHECK(!sink.Has("rt:2330:flat"));
  CHECK_EQ(legs.CreateCount(), 0);
  std::filesystem::remove_all(dir);
}

// Recovery past 13:25: resume HOLD, but the forced-exit watchdog fires and the exit
// window is degenerate, so the run ends in a loud FAILED naming the held shares.
void TestPastCloseRecoveryFatal() {
  const std::string dir = TempDir();
  FakeClock clk;
  clk.wall_min.store(13 * 60 + 26);  // 13:26
  const std::string day = Day(clk);
  CHECK(OrderJournal::AppendFill(dir, "2330-Buy-" + day, "k-1", 300, 58000));
  CHECK(RoundTripJournal::EnterDone(dir, "2330-rt-" + day, 300, 58000, 1));

  Scenario s = BaseScenario(dir);
  s.roundtrip.arm_end_hhmm = 1300;  // arm window irrelevant on resume
  FakeSignalSource sig;
  FakeQuoteSource quotes;
  FakeLegFactory legs;
  RecorderSink sink;
  RoundTripRunner runner(std::move(s), &sig, &quotes, &legs, &sink, clk.Make());
  int rc = runner.Run();
  CHECK_EQ(rc, 1);
  CHECK(sink.HasField("rt:2330:exit-degenerate", "remaining_shares", "300"));
  CHECK(!sink.Has("rt:2330:flat"));
  CHECK_EQ(legs.CreateCount(), 0);  // could not cross to close
  std::filesystem::remove_all(dir);
}

// No journals at all: recovery is a no-op and the runner arms fresh.
void TestFreshWhenEmpty() {
  const std::string dir = TempDir();
  FakeSignalSource sig;
  FakeQuoteSource quotes;
  FakeLegFactory legs;
  RecorderSink sink;
  FakeClock clk;
  RoundTripRunner runner(BaseScenario(dir), &sig, &quotes, &legs, &sink, clk.Make());
  std::thread th([&] { runner.Run(); });
  CHECK(WaitFor([&] { return sink.Has("rt:2330:armed"); }));
  CHECK(!sink.Has("rt:2330:recover-hold"));
  runner.RequestStop();
  th.join();
  std::filesystem::remove_all(dir);
}

}  // namespace

int main() {
  TestResumeHoldStopFires();
  TestDegradedResumeStopFires();
  TestTerminalAfterRestart();
  TestNetShortRefuses();
  TestPastCloseRecoveryFatal();
  TestFreshWhenEmpty();
  if (g_failures == 0) {
    std::printf("test_roundtrip_recovery_runner: OK\n");
    return 0;
  }
  std::printf("test_roundtrip_recovery_runner: FAILED %d check(s)\n", g_failures);
  return 1;
}
