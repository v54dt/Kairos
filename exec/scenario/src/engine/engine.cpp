#include "engine.h"

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <optional>
#include <thread>
#include <utility>

#include "order_codec.h"
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

LocalNow LocalFromUtc(std::chrono::system_clock::time_point tp) {
  auto utc8 = tp + std::chrono::hours(8);
  auto dp = std::chrono::floor<std::chrono::days>(utc8);
  std::chrono::weekday wd{dp};
  std::chrono::hh_mm_ss hms{utc8 - dp};
  int hhmm = static_cast<int>(hms.hours().count()) * 100 + static_cast<int>(hms.minutes().count());
  bool weekday = wd != std::chrono::Saturday && wd != std::chrono::Sunday;
  return {hhmm, weekday};
}

std::string DateFromUtc(std::chrono::system_clock::time_point tp) {
  auto utc8 = tp + std::chrono::hours(8);
  std::chrono::year_month_day ymd{std::chrono::floor<std::chrono::days>(utc8)};
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%04d%02u%02u", static_cast<int>(ymd.year()),
                static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()));
  return buf;
}

int HhmmToMin(int hhmm) { return (hhmm / 100) * 60 + hhmm % 100; }

constexpr int kMarketCloseHhmm = 1330;  // TWSE regular session close; hard stop

}  // namespace

ScenarioEngine::ScenarioEngine(Scenario scenario, OrderBackend* backend, EventSink* sink,
                               QuoteSource* quotes, EngineClock clock)
    : s_(std::move(scenario)),
      backend_(backend),
      sink_(sink),
      quotes_(quotes),
      clock_(std::move(clock)) {
  oid_prefix_ = "k" + std::to_string(::getpid());
  std::string sym = s_.symbol;
  quotes_->SetCallback([this, sym](const std::string& s, const TopOfBook& tob) {
    if (s == sym) {
      book_.Update(tob);
      cv_.notify_all();
    }
  });
  // Restart-safe accounting: replay today's fills so the budget isn't re-bought,
  // then append to the same journal.
  if (!s_.journal_dir.empty()) {
    std::string name = s_.symbol + "-" + SideName(s_.side) + "-" + DateFromUtc(clock_.wall());
    long restored = 0;
    for (const auto& fl : ReadJournalFills(JournalPath(s_.journal_dir, name))) {
      acct_.RecordFill(s_, fl.price, fl.shares);
      restored += fl.shares;
    }
    if (restored > 0)
      std::fprintf(stderr, "kairos-exec: journal replay restored %ld sh (NT$ %ld)\n", restored,
                   acct_.FilledTwd());
    journal_.Open(s_.journal_dir, name);
  }
}

std::string ScenarioEngine::NextOrderId() {
  return oid_prefix_ + "-" + std::to_string(++order_seq_);
}

void ScenarioEngine::SdkGate() {
  std::lock_guard<std::mutex> lock(sdk_mu_);
  auto now = clock_.mono();
  if (last_sdk_.time_since_epoch().count() != 0) {
    auto since = now - last_sdk_;
    if (since < std::chrono::seconds(1))
      std::this_thread::sleep_for(std::chrono::seconds(1) - since);
  }
  last_sdk_ = clock_.mono();
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
  journal_.LogAck(id, ok);
  if (id != resting_id_) return;
  if (ok) {
    resting_acked_ = true;
    if (dashboard_ && s_.live) {
      auto now = clock_.mono();
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

void ScenarioEngine::OnCancel(const std::string& id, bool ok) {
  std::lock_guard<std::mutex> lock(mu_);
  journal_.LogCancel(id, ok);
  if (id != resting_id_) return;
  ClearResting();  // working order gone; next tick re-places at the current peg
  cv_.notify_all();
}

void ScenarioEngine::OnDisconnect() {
  sink_->Emit(
      {EventCategory::kDisconnect, Severity::kError, s_.symbol, "disconnect:" + s_.symbol, {}});
  if (s_.stop_on_disconnect) {
    std::fprintf(stderr, "kairos-exec: order backend disconnected; stopping\n");
    stop_ = true;
    cv_.notify_all();
  }
}

void ScenarioEngine::OnFill(const std::string& id, const Fill& f) {
  std::lock_guard<std::mutex> lock(mu_);
  journal_.LogFill(id, f.shares, f.price);  // journal (fsync) before we count it
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
      [this](const std::string& id, bool ok) { OnCancel(id, ok); }, [this] { OnDisconnect(); });
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
      LocalNow n = LocalFromUtc(clock_.wall());
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
    // quote-stall alert: emit once when quotes go silent past the threshold in the
    // active window, and re-arm when a fresh quote arrives.
    if (s_.quote_stall_alert_ms > 0 && age > s_.quote_stall_alert_ms) {
      if (!quote_stalled_) {
        quote_stalled_ = true;
        sink_->Emit({EventCategory::kQuoteStall,
                     Severity::kWarning,
                     s_.symbol,
                     "quote_stall:" + s_.symbol,
                     {{"age_ms", std::to_string(age)}}});
      }
    } else if (age >= 0) {
      quote_stalled_ = false;
    }

    long remaining;
    RestingOrder resting;
    std::string rid;
    bool acked, cancelling;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (complete_) break;
      // ack-timeout watchdog: an un-acked order that never got a response is dead
      // (hub dropped it / silent write failure) — free the slot to re-place.
      long since_submit =
          std::chrono::duration_cast<std::chrono::milliseconds>(clock_.mono() - resting_t_submit_)
              .count();
      if (AckTimedOut(resting_.active, resting_acked_, since_submit, s_.ack_timeout_ms)) {
        std::fprintf(stderr, "kairos-exec: order %s ack timeout (%ldms); local reject\n",
                     resting_id_.c_str(), since_submit);
        sink_->Emit({EventCategory::kError,
                     Severity::kWarning,
                     s_.symbol,
                     "ack_timeout:" + resting_id_,
                     {}});
        ClearResting();
      }
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
        auto t0 = clock_.mono();
        backend_->Submit(om);
        auto t1 = clock_.mono();
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
