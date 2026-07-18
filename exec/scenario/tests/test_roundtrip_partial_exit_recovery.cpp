// Restart during a PARTIAL EXIT, driven through the REAL engine leg + a sim backend.
// Pre-crash: bought 300, the exit leg sold 100 (the Sell journal holds 100), then the
// process was killed. On restart the runner resumes HOLD of net 200. When the stop
// fires it starts the exit leg, whose ScenarioEngine replays the 100 already-sold
// shares. Because the exit leg's budget is the LIFETIME shares bought (300), it sells
// the remaining 200 and the day flattens with zero residual — never a silent flat that
// abandons an unprotected long.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "order_journal.h"
#include "queue_sim_backend.h"
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

EngineClock FixedClock() {
  return EngineClock{
      [] { return std::chrono::system_clock::time_point(std::chrono::minutes(600 - 480)); },
      [] { return std::chrono::steady_clock::time_point(std::chrono::milliseconds(0)); }};
}

TopOfBook Book(Cents px) {
  TopOfBook t;
  t.bids[0] = {px, 1000};
  t.asks[0] = {px, 1000};
  t.n_bids = 1;
  t.n_asks = 1;
  t.last_trade = px;
  t.valid = true;
  return t;
}

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

Scenario BaseScenario(const std::string& dir) {
  Scenario s;
  s.name = "rt";
  s.symbol = "2330";
  s.side = Side::kBuy;
  s.board = Board::kOddLot;
  s.price_policy = PricePolicy::kCross;
  s.pacing = Pacing::kAsap;
  s.journal_dir = dir;
  s.budget_shares = 300;
  s.shares_per_order = 300;  // one slice covers the whole remaining
  s.quote_max_age_ms = 0;
  s.quote_stall_alert_ms = 0;  // isolate: no stall watchdog
  s.window_start_hhmm = 900;
  s.window_end_hhmm = 1325;
  s.roundtrip.enabled = true;
  s.roundtrip.signal = "vwap";
  s.roundtrip.stop_loss_pct = 2.0;  // entry 100.00 -> stop 98.00
  s.roundtrip.max_hold_min = 30;
  s.roundtrip.enter_window_min = 10;
  s.roundtrip.arm_start_hhmm = 900;
  s.roundtrip.arm_end_hhmm = 1300;
  return s;
}

long SumFills(const std::string& path) {
  long total = 0;
  for (const auto& f : ReadJournalFills(path)) total += f.shares;
  return total;
}

std::string TempDir() {
  static std::atomic<int> ctr{0};
  std::string d = (std::filesystem::temp_directory_path() /
                   ("kairos-rt-partial-exit-" + std::to_string(ctr.fetch_add(1))))
                      .string();
  std::filesystem::remove_all(d);
  std::filesystem::create_directories(d);
  return d;
}

}  // namespace

int main() {
  const std::string dir = TempDir();
  const std::string day = TradingDayUtc8(FixedClock().wall());
  const long now_us = (600 - 480) * 60 * 1000000L;

  // Pre-crash state: bought 300, exit leg partially SOLD 100, then killed mid-exit.
  CHECK(OrderJournal::AppendFill(dir, "2330-Buy-" + day, "b-1", 300, 10000));
  CHECK(OrderJournal::AppendFill(dir, "2330-Sell-" + day, "s-1", 100, 10000));
  CHECK(RoundTripJournal::Arm(dir, "2330-rt-" + day));
  CHECK(RoundTripJournal::Trigger(dir, "2330-rt-" + day, "vwap", 1, now_us));
  CHECK(RoundTripJournal::EnterDone(dir, "2330-rt-" + day, 300, 10000, now_us));

  FakeSignalSource sig;
  FakeQuoteSource hold_quotes;
  RecorderSink sink;

  EngineLegFactory::BackendFn backend_fn =
      [](const Scenario& leg) -> std::unique_ptr<OrderBackend> {
    return std::make_unique<QueueSimBackend>(FillMode::kProbQueue,
                                             std::vector<std::string>{leg.symbol});
  };
  EngineLegFactory::QuoteFn quote_fn = [](const Scenario& leg) -> std::unique_ptr<QuoteSource> {
    return std::make_unique<BookQuoteSource>(leg.symbol, Book(9000));  // sub-stop, crosses to sell
  };
  EngineLegFactory legs(std::move(backend_fn), std::move(quote_fn), &sink, FixedClock(),
                        /*ignore_window=*/true);

  RoundTripRunner runner(BaseScenario(dir), &sig, &hold_quotes, &legs, &sink, FixedClock());
  int rc = -1;
  std::thread th([&] { rc = runner.Run(); });

  CHECK(WaitFor([&] { return sink.Has("rt:2330:recover-hold"); }));
  hold_quotes.Push("2330", Book(9000));  // below the 98.00 stop -> stop fires -> exit leg
  th.join();

  const long buy_total = SumFills(JournalPath(dir, "2330-Buy-" + day));
  const long sell_total = SumFills(JournalPath(dir, "2330-Sell-" + day));
  const long residual = buy_total - sell_total;

  CHECK_EQ(buy_total, 300);
  CHECK_EQ(sell_total, 300);  // 100 pre-crash + 200 sold on resume, not 100 + 100
  CHECK_EQ(residual, 0);      // no unprotected long left behind
  CHECK_EQ(rc, 0);
  CHECK(sink.Has("rt:2330:flat"));  // a real flat, backed by a zero residual

  std::filesystem::remove_all(dir);
  if (g_failures == 0) {
    std::printf("test_roundtrip_partial_exit_recovery: OK\n");
    return 0;
  }
  std::printf("test_roundtrip_partial_exit_recovery: FAILED %d check(s)\n", g_failures);
  return 1;
}
