#include "engine.h"

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <thread>
#include <utility>

#include "order_codec.h"
#include "tw_market.h"

namespace kairos::exec {

namespace {

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

int HhmmToMin(int hhmm) { return (hhmm / 100) * 60 + hhmm % 100; }

constexpr int kMarketCloseHhmm = 1330;  // TWSE regular session close; hard stop

// Distinct non-zero exits so the supervisor classifies the run crashed (and its
// restart-on-crash backoff/cap applies); the stderr fatal line names the reason.
constexpr int kNoJournalExit = 2;
constexpr int kConnectFailExit = 3;
constexpr int kHaltExit = 17;

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
      backend_->OnMarketBook(s, tob, tob.quote_ts_us);  // no-op unless queue-sim paper
      cv_.notify_all();
    }
  });
  // A trade subscription flips UdsQuoteClient into decoding Trade frames, so it is
  // registered only for a backend that consumes them (queue-sim paper) — the live
  // and instant-paper quote paths stay bit-identical.
  if (backend_->WantsMarketTrades()) {
    quotes_->SetTradeCallback([this, sym](const std::string& s, const Trade& t) {
      if (s == sym) backend_->OnMarketTrade(s, t, t.trade_ts_us);
    });
  }
  // A live run with no configured journal resolves the shared journal dir
  // ($KAIROS_JOURNAL_DIR, else the TUI's <data-dir>/journal) so it always has a
  // fill record on the same file the hub uses; paper never inherits this path, so
  // simulated fills can't contaminate the journal a live run replays from.
  if (s_.live) s_.journal_dir = ResolveJournalDir(s_.journal_dir, nullptr);
  // Restart-safe accounting: replay today's fills so the budget isn't re-bought,
  // then append to the same journal.
  if (!s_.journal_dir.empty()) {
    std::string name = s_.symbol + "-" + SideName(s_.side) + "-" + TradingDayUtc8(clock_.wall());
    long restored = 0;
    for (const auto& fl : ReadJournalFills(JournalPath(s_.journal_dir, name))) {
      acct_.RecordFill(s_, fl.price, fl.shares);
      restored += fl.shares;
    }
    if (restored > 0)
      std::fprintf(stderr, "kairos-exec: journal replay restored %ld sh (NT$ %ld)\n", restored,
                   acct_.FilledTwd());
    journal_ok_ = journal_.Open(s_.journal_dir, name);
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

void ScenarioEngine::AbandonResting() {
  sell_cap_.AbandonResting(s_.side, resting_.active, resting_.shares, resting_filled_, resting_id_);
}

void ScenarioEngine::RegisterFailure(const std::string& reason) {
  failure_halt_.RegisterFailure(reason, s_.max_consecutive_order_failures);
}

void ScenarioEngine::StampCancel(const std::string& id, int seq) {
  std::lock_guard<std::mutex> lock(mu_);
  dash_.StampCancel(id, seq, clock_.mono());
}

void ScenarioEngine::OnAck(const std::string& id, bool ok, const std::string& err) {
  std::lock_guard<std::mutex> lock(mu_);
  journal_.LogAck(id, ok);
  if (id != resting_id_) return;
  if (ok) {
    resting_acked_ = true;
    failure_halt_.Reset();  // a good ack clears the fail-closed streak
    dash_.ReportAckSuccess(resting_seq_, resting_t_start_, resting_t_submit_, clock_.mono());
  } else {
    std::fprintf(stderr, "kairos-exec: order %s rejected: %s\n", id.c_str(), err.c_str());
    sink_->Emit(
        {EventCategory::kError, Severity::kError, s_.symbol, "reject:" + id, {{"reason", err}}});
    dash_.ReportReject(resting_seq_, err, resting_t_start_, resting_t_submit_);
    ClearResting();               // rejected -> free the working slot
    RegisterFailure("rejected");  // a reject storm halts fail-closed, not re-tries forever
    cv_.notify_all();
  }
}

void ScenarioEngine::OnCancel(const std::string& id, bool ok) {
  std::lock_guard<std::mutex> lock(mu_);
  journal_.LogCancel(id, ok);
  // Report the cancel round-trip (issue -> OnCancel) against the pending cancel,
  // which may be an ack-abandoned id no longer equal to resting_id_.
  dash_.ReportCancel(id, ok, clock_.mono());
  if (ok) {
    // A confirmed cancel means the order can no longer fill: release any shares
    // it was still holding against the sell cap.
    if (sell_cap_.ReleaseOnCancel(id)) cv_.notify_all();
  }
  if (id != resting_id_) return;
  if (!ok) AbandonResting();  // cancel rejected -> the order may still be live at broker
  ClearResting();             // stop tracking it; next tick re-places at the current peg
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
  } else {
    sell_cap_.OnLateFill(id, f.shares);  // late fill of an abandoned (possibly-live) order
  }
  std::printf("kairos-exec: fill %s %ld @ %s  (cum %ld sh / NT$ %ld, fee %ld, tax %ld)\n",
              id.c_str(), f.shares, CentsToString(f.price).c_str(), acct_.filled_shares,
              acct_.FilledTwd(), acct_.total_fee_twd, acct_.total_tax_twd);
  sink_->Emit(
      {EventCategory::kFill,
       Severity::kInfo,
       s_.symbol,
       "",
       {{"shares", std::to_string(f.shares)},
        {"price", CentsToString(f.price)},
        {"cum_twd", std::to_string(acct_.FilledTwd())},
        {"budget", std::to_string(s_.budget_shares > 0 ? s_.budget_shares : s_.budget_twd)}}});
  int pct =
      s_.budget_shares > 0
          ? static_cast<int>(acct_.filled_shares * 100 / s_.budget_shares)
          : (s_.budget_twd > 0 ? static_cast<int>(acct_.FilledTwd() * 100 / s_.budget_twd) : 0);
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

ScenarioEngine::PaceDecision ScenarioEngine::PaceWindow(double& window_progress) {
  window_progress = 1.0;  // ignore-window => no twap throttle
  if (!ignore_window_) {
    LocalNow n = LocalFromUtc(clock_.wall());
    WindowPhase phase = ClassifyWindow(n.hhmm, !s_.weekdays_only || n.weekday, s_.window_start_hhmm,
                                       s_.window_end_hhmm, kMarketCloseHhmm);
    if (phase == WindowPhase::kClosed)
      return PaceDecision::kBreak;  // hard stop at close (budget may be unfilled)
    if (phase == WindowPhase::kWaitForOpen) {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait_for(lock, std::chrono::seconds(1));
      return PaceDecision::kContinue;
    }
    // kInWindow: twap-pace by progress. kFillRemainder (past end_time, pre-close):
    // leave window_progress at 1.0 so the remainder fills at full pace.
    if (phase == WindowPhase::kInWindow) {
      int now_min = HhmmToMin(n.hhmm);
      if (schedule_start_min_ < 0)
        schedule_start_min_ = now_min;  // spread from first in-window tick
      int end_min = HhmmToMin(s_.window_end_hhmm);
      window_progress =
          end_min > schedule_start_min_
              ? static_cast<double>(now_min - schedule_start_min_) / (end_min - schedule_start_min_)
              : 1.0;
      if (window_progress < 0.0) window_progress = 0.0;
      if (window_progress > 1.0) window_progress = 1.0;
    }
  }
  return PaceDecision::kProceed;
}

bool ScenarioEngine::CheckStaleAndStall(const TopOfBook& tob, long age) {
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
  return stale;
}

void ScenarioEngine::RunAckWatchdog(std::string& timed_out_id, int& timed_out_seq) {
  // ack-timeout watchdog: an un-acked order that never got a response is dead
  // to us — but it may be LIVE at the broker (7/6 proved every timed-out order
  // was accepted), so cancel it before forgetting and count the failure.
  long since_submit =
      std::chrono::duration_cast<std::chrono::milliseconds>(clock_.mono() - resting_t_submit_)
          .count();
  if (AckTimedOut(resting_.active, resting_acked_, since_submit, s_.ack_timeout_ms)) {
    timed_out_id = resting_id_;
    timed_out_seq = resting_seq_;
    std::fprintf(stderr, "kairos-exec: order %s ack timeout (%ldms); local reject\n",
                 resting_id_.c_str(), since_submit);
    sink_->Emit({EventCategory::kError,
                 Severity::kWarning,
                 s_.symbol,
                 "ack_timeout:" + resting_id_,
                 {{"reason", "ack timeout after " + std::to_string(since_submit) +
                                 "ms (order may be live at broker)"}}});
    dash_.ReportAckTimeout(resting_seq_, since_submit, resting_t_start_, resting_t_submit_,
                           clock_.mono());
    AbandonResting();  // keep the possibly-live order counted against the sell cap
    ClearResting();
    RegisterFailure("ack timeout");
  }
}

bool ScenarioEngine::DispatchAction(const TopOfBook& tob, const RestingOrder& resting,
                                    long remaining, double window_progress, long sell_cap_remaining,
                                    const std::string& rid, int rid_seq, bool acked,
                                    bool cancelling) {
  Action act = DecideAction(s_, tob, resting, remaining, window_progress, sell_cap_remaining);
  if (act.done && !resting.active) {
    std::lock_guard<std::mutex> lock(mu_);
    complete_ = true;
    return true;
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
    StampCancel(rid, rid_seq);
    backend_->Cancel(rid);
    std::lock_guard<std::mutex> lock(mu_);
    if (resting_id_ == rid) cancelling_ = true;
  }
  return false;
}

void ScenarioEngine::WindDown() {
  // Wind down: cancel the working order only if the broker acked it.
  std::string rid;
  int rid_seq = 0;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (resting_.active && resting_acked_) {
      rid = resting_id_;
      rid_seq = resting_seq_;
    }
  }
  if (!rid.empty()) {
    SdkGate();
    StampCancel(rid, rid_seq);
    backend_->Cancel(rid);
  }
  quotes_->Stop();
  backend_->Disconnect();
}

int ScenarioEngine::EmitTerminal() {
  std::lock_guard<std::mutex> lock(mu_);
  if (s_.budget_shares > 0) {
    std::printf("kairos-exec: end - filled %ld sh of %ld, NT$ %ld, fee NT$ %ld, tax NT$ %ld\n",
                acct_.filled_shares, s_.budget_shares, acct_.FilledTwd(), acct_.total_fee_twd,
                acct_.total_tax_twd);
  } else {
    std::printf("kairos-exec: end - filled %ld sh / NT$ %ld of %ld, fee NT$ %ld, tax NT$ %ld\n",
                acct_.filled_shares, acct_.FilledTwd(), s_.budget_twd, acct_.total_fee_twd,
                acct_.total_tax_twd);
  }
  std::fflush(stdout);
  // Fail-closed halt: emit a terminal alert with a real reason (never an empty-field
  // event that ntfy renders as a bare "triggered") and exit non-zero so the
  // supervisor treats it as a crash and its restart backoff/cap takes over.
  if (failure_halt_.halted()) {
    std::fprintf(stderr, "kairos-exec: FATAL %s\n", failure_halt_.reason().c_str());
    sink_->Emit(
        {EventCategory::kError,
         Severity::kError,
         s_.symbol,
         "halt:" + s_.symbol,
         {{"reason", failure_halt_.reason()},
          {"filled_sh", std::to_string(acct_.filled_shares)},
          {"filled_twd", std::to_string(acct_.FilledTwd())},
          {"budget", std::to_string(s_.budget_shares > 0 ? s_.budget_shares : s_.budget_twd)},
          {"tax", std::to_string(acct_.total_tax_twd)}}});
    return kHaltExit;
  }
  // Outcome: Ctrl+C -> shutdown; done -> complete; reached market close still
  // running -> incomplete (don't silently "complete" an unfinished run). complete_
  // also covers a done state the TWD budget can't see, e.g. a SELL whose position
  // cap fully liquidated before the budget bound.
  bool interrupted = stop_.load();
  bool filled = complete_ || acct_.BudgetReached(s_);
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
                {"budget", std::to_string(s_.budget_shares > 0 ? s_.budget_shares : s_.budget_twd)},
                {"fee", std::to_string(acct_.total_fee_twd)},
                {"tax", std::to_string(acct_.total_tax_twd)}}});
  return 0;
}

int ScenarioEngine::Run() {
  // Fail-closed: a live trader with no run-state journal has no fill record and no
  // restart no-double-buy protection (7/6 ran 39 scenarios this way). Refuse.
  if (!journal_ok_) {
    if (s_.live) {
      std::fprintf(stderr,
                   "kairos-exec: FATAL live run requires a journal but '%s' could not be opened "
                   "for append\n",
                   s_.journal_dir.empty() ? "<unset>" : s_.journal_dir.c_str());
      return kNoJournalExit;
    }
    std::fprintf(stderr, "kairos-exec: WARNING no journal (%s); no restart-safe fill record\n",
                 s_.journal_dir.empty() ? "<unset>" : s_.journal_dir.c_str());
  }
  // Wire callbacks before Connect: a backend may start its reader thread inside
  // Connect (HubOrderBackend), so the callbacks must already be in place.
  backend_->SetCallbacks(
      [this](const std::string& id, bool ok, const std::string& e) { OnAck(id, ok, e); },
      [this](const std::string& id, const Fill& f) { OnFill(id, f); },
      [this](const std::string& id, bool ok) { OnCancel(id, ok); }, [this] { OnDisconnect(); });
  if (!backend_->Connect()) {
    std::fprintf(stderr, "kairos-exec: order backend connect failed\n");
    return kConnectFailExit;
  }
  quotes_->Start();
  if (s_.budget_shares > 0) {
    std::printf("kairos-exec: %s %s %ld shares, %s, %s\n", SideName(s_.side), s_.symbol.c_str(),
                s_.budget_shares, PricePolicyName(s_.price_policy),
                s_.live ? "*** LIVE ***" : "PAPER");
  } else {
    std::printf("kairos-exec: %s %s NT$ %ld, %s, %s\n", SideName(s_.side), s_.symbol.c_str(),
                s_.budget_twd, PricePolicyName(s_.price_policy),
                s_.live ? "*** LIVE ***" : "PAPER");
  }
  std::fflush(stdout);
  sink_->Emit(
      {EventCategory::kStart,
       Severity::kInfo,
       s_.symbol,
       "",
       {{"side", SideName(s_.side)},
        {"budget", std::to_string(s_.budget_shares > 0 ? s_.budget_shares : s_.budget_twd)}}});

  while (!stop_) {
    double window_progress = 1.0;
    PaceDecision pace = PaceWindow(window_progress);
    if (pace == PaceDecision::kBreak) break;
    if (pace == PaceDecision::kContinue) continue;

    TopOfBook tob = book_.Snapshot();
    long age = book_.AgeMs();
    bool stale = CheckStaleAndStall(tob, age);

    long remaining;
    long sell_cap_remaining = -1;  // -1 = no cap (buy)
    RestingOrder resting;
    std::string rid;
    bool acked, cancelling, halted_now = false;
    std::string timed_out_id;  // possibly-live order to cancel outside the lock
    int timed_out_seq = 0;
    int rid_seq = 0;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (complete_ || failure_halt_.halted()) break;
      RunAckWatchdog(timed_out_id, timed_out_seq);
      remaining = acct_.Remaining(s_);
      if (s_.side == Side::kSell) {
        // Count in-flight (resting minus its partial fills) plus any abandoned
        // orders still live at the broker as already committed, so a resting order
        // plus a new one can never oversell the held position.
        sell_cap_remaining =
            sell_cap_.SellCapRemaining(s_.position_shares, acct_.filled_shares, resting_.active,
                                       resting_.shares, resting_filled_);
      }
      resting = resting_;
      rid = resting_id_;
      rid_seq = resting_seq_;
      acked = resting_acked_;
      cancelling = cancelling_;
      halted_now = failure_halt_.halted();
    }
    // Best-effort cancel of the timed-out (possibly-live) order before re-placing.
    if (!timed_out_id.empty()) {
      SdkGate();
      StampCancel(timed_out_id, timed_out_seq);
      backend_->Cancel(timed_out_id);
    }
    if (halted_now) break;
    if (remaining <= 0) break;

    if (!stale) {
      if (DispatchAction(tob, resting, remaining, window_progress, sell_cap_remaining, rid, rid_seq,
                         acked, cancelling))
        break;
    }

    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait_for(lock, std::chrono::milliseconds(200),
                 [this] { return stop_.load() || complete_ || failure_halt_.halted(); });
  }

  WindDown();
  return EmitTerminal();
}

void ScenarioEngine::RequestStop() {
  stop_ = true;
  cv_.notify_all();
}

}  // namespace kairos::exec
