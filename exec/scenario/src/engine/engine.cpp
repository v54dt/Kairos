#include "engine.h"

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <optional>
#include <thread>
#include <utility>

#include "order_codec.h"
#include "socket_path.h"
#include "tw_market.h"

namespace kairos::exec {

namespace {

double Millis(std::chrono::steady_clock::duration d) {
  return std::chrono::duration_cast<std::chrono::microseconds>(d).count() / 1000.0;
}

struct LocalNow {
  int hhmm;
  bool weekday;
};

LocalNow NowUtc8() {
  auto utc8 = std::chrono::system_clock::now() + std::chrono::hours(8);
  auto dp = std::chrono::floor<std::chrono::days>(utc8);
  std::chrono::weekday wd{dp};
  std::chrono::hh_mm_ss hms{utc8 - dp};
  int hhmm = static_cast<int>(hms.hours().count()) * 100 + static_cast<int>(hms.minutes().count());
  bool weekday = wd != std::chrono::Saturday && wd != std::chrono::Sunday;
  return {hhmm, weekday};
}

int HhmmToMin(int hhmm) { return (hhmm / 100) * 60 + hhmm % 100; }

constexpr int kMarketCloseHhmm = 1330;  // TWSE regular session close; hard stop

}  // namespace

ScenarioEngine::ScenarioEngine(Scenario scenario, OrderBackend* backend, EventSink* sink)
    : s_(std::move(scenario)), backend_(backend), sink_(sink) {
  oid_prefix_ = "k" + std::to_string(::getpid());
  std::string sym = s_.symbol;
  quotes_ =
      std::make_unique<UdsQuoteClient>(QuoteSocketPath(), std::vector<std::string>{sym},
                                       [this, sym](const std::string& s, const TopOfBook& tob) {
                                         if (s == sym) {
                                           book_.Update(tob);
                                           cv_.notify_all();
                                         }
                                       });
}

std::string ScenarioEngine::NextOrderId() {
  return oid_prefix_ + "-" + std::to_string(++order_seq_);
}

void ScenarioEngine::SdkGate() {
  std::lock_guard<std::mutex> lock(sdk_mu_);
  auto now = std::chrono::steady_clock::now();
  if (last_sdk_.time_since_epoch().count() != 0) {
    auto since = now - last_sdk_;
    if (since < std::chrono::seconds(1))
      std::this_thread::sleep_for(std::chrono::seconds(1) - since);
  }
  last_sdk_ = std::chrono::steady_clock::now();
}

void ScenarioEngine::ClearResting() {
  resting_ = RestingOrder{};
  resting_id_.clear();
  resting_filled_ = 0;
  resting_acked_ = false;
  cancelling_ = false;
}

void ScenarioEngine::OnAck(const std::string& id, bool ok, const std::string& err) {
  std::lock_guard<std::mutex> lock(mu_);
  if (id != resting_id_) return;
  if (ok) {
    resting_acked_ = true;
    if (dashboard_ && s_.live) {
      auto now = std::chrono::steady_clock::now();
      dashboard_->ReportOrder(resting_seq_, "success", "", Millis(now - resting_t_start_),
                              Millis(resting_t_submit_ - resting_t_start_),
                              Millis(now - resting_t_submit_));
    }
  } else {
    std::fprintf(stderr, "kairos-exec: order %s rejected: %s\n", id.c_str(), err.c_str());
    sink_->Emit(
        {EventCategory::kError, Severity::kError, s_.symbol, "reject:" + id, {{"reason", err}}});
    if (dashboard_ && s_.live) {
      dashboard_->ReportOrder(resting_seq_, "submit_error", err, std::nullopt,
                              Millis(resting_t_submit_ - resting_t_start_), std::nullopt);
    }
    ClearResting();  // rejected -> free the working slot, retry next tick
    cv_.notify_all();
  }
}

void ScenarioEngine::OnCancel(const std::string& id, bool /*ok*/) {
  std::lock_guard<std::mutex> lock(mu_);
  if (id != resting_id_) return;
  ClearResting();  // working order gone; next tick re-places at the current peg
  cv_.notify_all();
}

void ScenarioEngine::OnFill(const std::string& id, const Fill& f) {
  std::lock_guard<std::mutex> lock(mu_);
  // Fills are routed to us by id, so always count one — even a late fill that lands
  // after a re-peg cancel cleared the resting order — else the budget overshoots.
  acct_.RecordFill(s_, f.price, f.shares);
  if (id == resting_id_) {
    resting_filled_ += f.shares;
    if (resting_filled_ >= resting_.shares) ClearResting();
  }
  std::printf("kairos-exec: fill %s %ld @ %s  (cum %ld sh / NT$ %ld, fee %ld)\n", id.c_str(),
              f.shares, CentsToString(f.price).c_str(), acct_.filled_shares, acct_.FilledTwd(),
              acct_.total_fee_twd);
  sink_->Emit({EventCategory::kFill,
               Severity::kInfo,
               s_.symbol,
               "",
               {{"shares", std::to_string(f.shares)},
                {"price", CentsToString(f.price)},
                {"cum_twd", std::to_string(acct_.FilledTwd())},
                {"budget", std::to_string(s_.budget_twd)}}});
  int pct = s_.budget_twd > 0 ? static_cast<int>(acct_.FilledTwd() * 100 / s_.budget_twd) : 0;
  if (pct >= last_milestone_pct_ + 25) {
    last_milestone_pct_ = pct - (pct % 25);
    std::printf("kairos-exec: progress %d%%\n", last_milestone_pct_);
    sink_->Emit({EventCategory::kMilestone,
                 Severity::kInfo,
                 s_.symbol,
                 "",
                 {{"pct", std::to_string(last_milestone_pct_)}}});
  }
  if (acct_.BudgetReached(s_)) complete_ = true;
  std::fflush(stdout);
  cv_.notify_all();
}

void ScenarioEngine::Run() {
  // Wire callbacks before Connect: a backend may start its reader thread inside
  // Connect (HubOrderBackend), so the callbacks must already be in place.
  backend_->SetCallbacks(
      [this](const std::string& id, bool ok, const std::string& e) { OnAck(id, ok, e); },
      [this](const std::string& id, const Fill& f) { OnFill(id, f); },
      [this](const std::string& id, bool ok) { OnCancel(id, ok); });
  if (!backend_->Connect()) {
    std::fprintf(stderr, "kairos-exec: order backend connect failed\n");
    return;
  }
  quotes_->Start();
  std::printf("kairos-exec: %s %s NT$ %ld, %s, %s\n", SideName(s_.side), s_.symbol.c_str(),
              s_.budget_twd, PricePolicyName(s_.price_policy), s_.live ? "*** LIVE ***" : "PAPER");
  std::fflush(stdout);
  sink_->Emit({EventCategory::kStart,
               Severity::kInfo,
               s_.symbol,
               "",
               {{"side", SideName(s_.side)}, {"budget", std::to_string(s_.budget_twd)}}});

  while (!stop_) {
    double window_progress = 1.0;  // ignore-window => no twap throttle
    if (!ignore_window_) {
      LocalNow n = NowUtc8();
      WindowPhase phase =
          ClassifyWindow(n.hhmm, !s_.weekdays_only || n.weekday, s_.window_start_hhmm,
                         s_.window_end_hhmm, kMarketCloseHhmm);
      if (phase == WindowPhase::kClosed) break;  // hard stop at close (budget may be unfilled)
      if (phase == WindowPhase::kWaitForOpen) {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait_for(lock, std::chrono::seconds(1));
        continue;
      }
      // kInWindow: twap-pace by progress. kFillRemainder (past end_time, pre-close):
      // leave window_progress at 1.0 so the remainder fills at full pace.
      if (phase == WindowPhase::kInWindow) {
        int now_min = HhmmToMin(n.hhmm);
        if (schedule_start_min_ < 0)
          schedule_start_min_ = now_min;  // spread from first in-window tick
        int end_min = HhmmToMin(s_.window_end_hhmm);
        window_progress = end_min > schedule_start_min_
                              ? static_cast<double>(now_min - schedule_start_min_) /
                                    (end_min - schedule_start_min_)
                              : 1.0;
        if (window_progress < 0.0) window_progress = 0.0;
        if (window_progress > 1.0) window_progress = 1.0;
      }
    }

    TopOfBook tob = book_.Snapshot();
    long age = book_.AgeMs();
    bool stale = !tob.valid || (s_.quote_max_age_ms > 0 && age >= 0 && age > s_.quote_max_age_ms);

    long remaining;
    RestingOrder resting;
    std::string rid;
    bool acked, cancelling;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (complete_) break;
      remaining = acct_.RemainingTwd(s_);
      resting = resting_;
      rid = resting_id_;
      acked = resting_acked_;
      cancelling = cancelling_;
    }
    if (remaining <= 0) break;

    if (!stale) {
      Action act = DecideAction(s_, tob, resting, remaining, window_progress);
      if (act.done && !resting.active) {
        std::lock_guard<std::mutex> lock(mu_);
        complete_ = true;
        break;
      }
      if (act.kind == ActionKind::kPlace) {
        std::string id = NextOrderId();
        int seq = order_seq_;
        {
          std::lock_guard<std::mutex> lock(mu_);
          resting_ = RestingOrder{true, act.price, act.shares};
          resting_id_ = id;
          resting_filled_ = 0;
          resting_acked_ = false;
          cancelling_ = false;
          resting_seq_ = seq;
        }
        OrderSubmitMsg om{id,        s_.symbol,       s_.market,        s_.board,
                          s_.side,   s_.funding_type, s_.time_in_force, act.price,
                          act.shares};
        SdkGate();  // before t_start so the rate-limit sleep isn't counted as RTT
        auto t0 = std::chrono::steady_clock::now();
        backend_->Submit(om);
        auto t1 = std::chrono::steady_clock::now();
        {
          std::lock_guard<std::mutex> lock(mu_);
          if (resting_id_ == id) {
            resting_t_start_ = t0;
            resting_t_submit_ = t1;
          }
        }
      } else if (act.kind == ActionKind::kRepeg && acked && !cancelling) {
        // Re-peg = cancel the (acked) working order; next tick re-places at the
        // new peg. Clearing waits for OnCancel/full-fill so racing fills count.
        SdkGate();
        backend_->Cancel(rid);
        std::lock_guard<std::mutex> lock(mu_);
        if (resting_id_ == rid) cancelling_ = true;
      }
    }

    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait_for(lock, std::chrono::milliseconds(200),
                 [this] { return stop_.load() || complete_; });
  }

  // Wind down: cancel the working order only if the broker acked it.
  std::string rid;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (resting_.active && resting_acked_) rid = resting_id_;
  }
  if (!rid.empty()) {
    SdkGate();
    backend_->Cancel(rid);
  }
  quotes_->Stop();
  backend_->Disconnect();

  std::lock_guard<std::mutex> lock(mu_);
  std::printf("kairos-exec: end - filled %ld sh / NT$ %ld of %ld, fee NT$ %ld\n",
              acct_.filled_shares, acct_.FilledTwd(), s_.budget_twd, acct_.total_fee_twd);
  std::fflush(stdout);
  // Outcome: Ctrl+C -> shutdown; budget filled -> complete; reached market close with
  // budget unfilled -> incomplete (don't silently "complete" an unfinished run).
  bool interrupted = stop_.load();
  bool filled = acct_.BudgetReached(s_);
  EventCategory cat = interrupted ? EventCategory::kShutdown
                      : filled    ? EventCategory::kComplete
                                  : EventCategory::kIncomplete;
  Severity sev = (!interrupted && !filled) ? Severity::kWarning : Severity::kInfo;
  sink_->Emit({cat,
               sev,
               s_.symbol,
               "",
               {{"filled_sh", std::to_string(acct_.filled_shares)},
                {"filled_twd", std::to_string(acct_.FilledTwd())},
                {"budget", std::to_string(s_.budget_twd)},
                {"fee", std::to_string(acct_.total_fee_twd)}}});
}

void ScenarioEngine::RequestStop() {
  stop_ = true;
  cv_.notify_all();
}

}  // namespace kairos::exec
