#include "roundtrip_runner.h"

#include <cassert>
#include <string>
#include <utility>

namespace kairos::exec {

namespace {

int WallToMin(std::chrono::system_clock::time_point tp) {
  auto utc8 = tp + std::chrono::hours(8);
  auto dp = std::chrono::floor<std::chrono::days>(utc8);
  std::chrono::hh_mm_ss hms{utc8 - dp};
  return static_cast<int>(hms.hours().count()) * 60 + static_cast<int>(hms.minutes().count());
}

long SteadyMs(std::chrono::steady_clock::duration d) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
}

ExitReason ReasonFor(RtEvent ev) {
  switch (ev) {
    case RtEvent::kSignalExit:
      return ExitReason::kReverseSignal;
    case RtEvent::kStopHit:
      return ExitReason::kStopLoss;
    case RtEvent::kHoldTimeout:
      return ExitReason::kHoldTimeout;
    default:
      return ExitReason::kForcedTime;  // forced-time / quote-stall / signal-lost-exit
  }
}

}  // namespace

RoundTripRunner::RoundTripRunner(Scenario scenario, SignalSource* signal, QuoteSource* hold_quotes,
                                 LegFactory* legs, EventSink* sink, EngineClock clock)
    : s_(std::move(scenario)),
      signal_(signal),
      hold_quotes_(hold_quotes),
      legs_(legs),
      sink_(sink),
      clock_(std::move(clock)),
      arm_start_min_(HhmmToMinutes(s_.roundtrip.arm_start_hhmm)),
      arm_end_min_(HhmmToMinutes(s_.roundtrip.arm_end_hhmm)) {}

RoundTripRunner::~RoundTripRunner() {
  JoinEnterThread();
  JoinExitThread();
}

int RoundTripRunner::NowMin() const { return WallToMin(clock_.wall()); }

void RoundTripRunner::Enqueue(RtEvent ev, const LegResult& leg) {
  {
    std::lock_guard<std::mutex> lk(q_mu_);
    queue_.push_back({ev, leg});
  }
  q_cv_.notify_all();
}

void RoundTripRunner::OnHoldQuote(const std::string& symbol, const TopOfBook& tob) {
  if (symbol != s_.symbol) return;
  {
    std::lock_guard<std::mutex> lk(tob_mu_);
    last_tob_ = tob;
    have_tob_ = true;
  }
  q_cv_.notify_all();
}

FsmInput RoundTripRunner::BuildInput(const RunnerEvent& e) const {
  FsmInput in;
  in.event = e.ev;
  in.now_min = NowMin();
  in.arm_start_min = arm_start_min_;
  in.arm_end_min = arm_end_min_;
  in.on_signal_loss = s_.roundtrip.on_signal_loss;
  if (state_ == RtState::kEnter) {
    in.leg_had_fills = e.leg.filled_shares > 0;
  } else if (state_ == RtState::kExit) {
    in.position_remaining = held_shares_ - e.leg.filled_shares > 0;
  }
  return in;
}

void RoundTripRunner::EmitPhase(EventCategory cat, Severity sev, const char* phase,
                                std::vector<std::pair<std::string, std::string>> fields) {
  Event ev;
  ev.category = cat;
  ev.severity = sev;
  ev.symbol = s_.symbol;
  ev.dedup_key = "rt:" + s_.symbol + ":" + phase;
  ev.fields = std::move(fields);
  sink_->Emit(ev);
}

void RoundTripRunner::StartEnterLeg(int trigger_min) {
  Scenario leg = DeriveEnterLeg(s_, trigger_min);
  leg.name += "-enter";
  enter_leg_ = legs_->Create(leg);
  active_leg_.store(enter_leg_.get());
  enter_thread_ = std::thread([this] {
    LegResult r = enter_leg_->Run();
    active_leg_.store(nullptr);
    Enqueue(r.halted ? RtEvent::kLegHalted : RtEvent::kEnterLegDone, r);
  });
}

void RoundTripRunner::StartExitLeg(int now_min) {
  Scenario leg = DeriveExitLeg(s_, held_shares_, exit_reason_, now_min);
  leg.name += "-exit";
  exit_leg_ = legs_->Create(leg);
  active_leg_.store(exit_leg_.get());
  exit_thread_ = std::thread([this] {
    LegResult r = exit_leg_->Run();
    active_leg_.store(nullptr);
    Enqueue(r.halted ? RtEvent::kLegHalted : RtEvent::kExitLegDone, r);
  });
}

void RoundTripRunner::JoinEnterThread() {
  if (enter_thread_.joinable()) enter_thread_.join();
}

void RoundTripRunner::JoinExitThread() {
  if (exit_thread_.joinable()) exit_thread_.join();
}

RtState RoundTripRunner::Execute(RtState from, const RunnerEvent& e, const FsmOutput& out) {
  RtState next = out.state;

  // Enter leg finished with a position: capture the entry and open the watchdog.
  if (from == RtState::kEnter && next == RtState::kHold) {
    JoinEnterThread();
    held_shares_ = e.leg.filled_shares;
    entry_avg_cents_ = e.leg.avg_price_cents;
    enter_done_mono_ = clock_.mono();
    if (e.ev == RtEvent::kLegHalted) {
      EmitPhase(EventCategory::kPartialFill, Severity::kWarning, "enter-halt",
                {{"held_shares", std::to_string(held_shares_)}});
    }
    EmitPhase(EventCategory::kMilestone, Severity::kInfo, "hold",
              {{"held_shares", std::to_string(held_shares_)},
               {"entry_avg_cents", std::to_string(entry_avg_cents_)}});
    // A loss that landed during the enter leg is edge-triggered and never re-arrives;
    // re-apply the exit-on-loss policy now that the position is in HOLD.
    if (signal_lost_ && s_.roundtrip.on_signal_loss == OnSignalLoss::kExit)
      Enqueue(RtEvent::kSignalLost);
    return next;
  }
  if (from == RtState::kEnter && next != RtState::kEnter) JoinEnterThread();

  switch (out.action) {
    case RtAction::kNone:
      if (e.ev == RtEvent::kSignalRestored && next == RtState::kArmed)
        EmitPhase(EventCategory::kReconnect, Severity::kInfo, "rearm", {});
      break;
    case RtAction::kStartEnterLeg: {
      int now = NowMin();
      if (now >= kForcedExitMin) {
        EmitPhase(EventCategory::kError, Severity::kWarning, "enter-degenerate",
                  {{"now_min", std::to_string(now)}});
        next = RtState::kArmed;  // window would be [13:25,13:25]: drop, stay armed
      } else {
        EmitPhase(EventCategory::kMilestone, Severity::kInfo, "enter",
                  {{"trigger_min", std::to_string(now)}});
        StartEnterLeg(now);
      }
      break;
    }
    case RtAction::kStartExitLeg: {
      assert(!enter_thread_.joinable());  // enter leg already joined on HOLD entry
      exit_reason_ = ReasonFor(e.ev);
      int now = NowMin();
      if (now >= kForcedExitMin) {
        EmitPhase(EventCategory::kError, Severity::kError, "exit-degenerate",
                  {{"remaining_shares", std::to_string(held_shares_)}});
        next = RtState::kFailed;  // past 13:25: cannot cross to close; never silent flat
        done_ = true;
        exit_code_ = 1;
      } else {
        EmitPhase(EventCategory::kMilestone, Severity::kInfo, "exit",
                  {{"reason", std::to_string(static_cast<int>(exit_reason_))}});
        StartExitLeg(now);
      }
      break;
    }
    case RtAction::kDisarm:
      EmitPhase(EventCategory::kDisconnect, Severity::kWarning, "disarm", {});
      break;
    case RtAction::kAlertOnly:
      EmitPhase(EventCategory::kError, Severity::kWarning, "alert",
                {{"state", RtStateName(from)}, {"event", std::to_string(static_cast<int>(e.ev))}});
      break;
    case RtAction::kTerminalFlat:
      if (from == RtState::kExit) JoinExitThread();
      EmitPhase(EventCategory::kComplete, Severity::kInfo, "flat", {});
      done_ = true;
      exit_code_ = 0;
      break;
    case RtAction::kTerminalFailed:
      if (from == RtState::kExit) JoinExitThread();
      EmitPhase(EventCategory::kError, Severity::kError, "failed",
                {{"remaining_shares", std::to_string(held_shares_ - e.leg.filled_shares)}});
      done_ = true;
      exit_code_ = 1;
      break;
  }
  return next;
}

void RoundTripRunner::CheckWatchdog() {
  auto mono = clock_.mono();
  if (NowMin() >= kForcedExitMin - 1) {  // fire at 13:24 so the exit window is non-degenerate
    Enqueue(RtEvent::kForcedExitTime);
    return;
  }
  if (SteadyMs(mono - enter_done_mono_) >= static_cast<long>(s_.roundtrip.max_hold_min) * 60000) {
    Enqueue(RtEvent::kHoldTimeout);
    return;
  }
  TopOfBook tob;
  bool have = false;
  {
    std::lock_guard<std::mutex> lk(tob_mu_);
    tob = last_tob_;
    have = have_tob_;
  }
  if (!have || !tob.valid) return;
  if (s_.quote_stall_alert_ms > 0 && SteadyMs(mono - tob.recv_ts) > s_.quote_stall_alert_ms) {
    Enqueue(RtEvent::kQuoteStallHard);
    return;
  }
  long stop_cents =
      static_cast<long>(entry_avg_cents_ * (1.0 - s_.roundtrip.stop_loss_pct / 100.0));
  Cents ref = tob.last_trade > 0 ? tob.last_trade : tob.best_bid();
  if (ref > 0 && ref <= stop_cents) Enqueue(RtEvent::kStopHit);
}

int RoundTripRunner::Run() {
  signal_->SetCallbacks(
      {[this](SignalAction a) {
         Enqueue(a == SignalAction::kEnter ? RtEvent::kSignalEnter : RtEvent::kSignalExit);
       },
       [this] { Enqueue(RtEvent::kSignalLost); }, [this] { Enqueue(RtEvent::kSignalRestored); }});
  hold_quotes_->SetCallback(
      [this](const std::string& sym, const TopOfBook& t) { OnHoldQuote(sym, t); });
  hold_quotes_->Start();
  signal_->Start();
  EmitPhase(EventCategory::kStart, Severity::kInfo, "armed",
            {{"signal", s_.roundtrip.signal}, {"symbol", s_.symbol}});

  while (true) {
    std::deque<RunnerEvent> batch;
    {
      std::unique_lock<std::mutex> lk(q_mu_);
      q_cv_.wait_for(lk, std::chrono::milliseconds(200),
                     [this] { return stop_ || !queue_.empty(); });
      batch.swap(queue_);
    }
    for (auto& e : batch) {
      if (e.ev == RtEvent::kSignalLost) {
        signal_lost_ = true;
      } else if (e.ev == RtEvent::kSignalRestored) {
        signal_lost_ = false;
      }
      FsmOutput out = Step(state_, BuildInput(e));
      state_ = Execute(state_, e, out);
      if (done_) break;
    }
    if (done_) break;
    if (stop_) {
      // fail-closed: a stop with a position still open is a failed run, not a clean flat.
      if (state_ == RtState::kHold || state_ == RtState::kExit) {
        EmitPhase(EventCategory::kError, Severity::kError, "stopped",
                  {{"remaining_shares", std::to_string(held_shares_)}});
        state_ = RtState::kFailed;
        exit_code_ = 1;
      }
      break;
    }
    if (state_ == RtState::kHold) CheckWatchdog();
  }

  Cleanup();
  return exit_code_;
}

void RoundTripRunner::RequestStop() {
  {
    std::lock_guard<std::mutex> lk(q_mu_);
    stop_ = true;
  }
  if (LegRunner* leg = active_leg_.load()) leg->RequestStop();
  q_cv_.notify_all();
}

void RoundTripRunner::Cleanup() {
  signal_->Stop();
  hold_quotes_->Stop();
  if (LegRunner* leg = active_leg_.load()) leg->RequestStop();
  JoinEnterThread();
  JoinExitThread();
}

SignalClientSource::SignalClientSource(std::string socket_path, SignalSubscribe sub)
    : client_(std::move(socket_path), std::move(sub),
              SignalCallbacks{[this](const SignalPush& p) {
                                if (cb_.on_signal) cb_.on_signal(p.action);
                              },
                              [this](const std::string&) {
                                if (cb_.on_lost) cb_.on_lost();
                              },
                              [this] {
                                if (cb_.on_restored) cb_.on_restored();
                              },
                              {}}) {}

void SignalClientSource::Start() { client_.Start(); }

EngineLegFactory::EngineLegFactory(BackendFn backend, QuoteFn quotes, EventSink* sink,
                                   EngineClock clock, bool ignore_window)
    : backend_(std::move(backend)),
      quotes_(std::move(quotes)),
      sink_(sink),
      clock_(std::move(clock)),
      ignore_window_(ignore_window) {}

namespace {

// Wraps a ScenarioEngine + its own backend/quote source as one LegRunner. Members
// are ordered so the engine (holding raw pointers) outlives neither dependency.
class EngineLegRunner : public LegRunner {
 public:
  EngineLegRunner(Scenario leg, std::unique_ptr<OrderBackend> backend,
                  std::unique_ptr<QuoteSource> quotes, EventSink* sink, EngineClock clock,
                  bool ignore_window)
      : backend_(std::move(backend)), quotes_(std::move(quotes)) {
    engine_ = std::make_unique<ScenarioEngine>(std::move(leg), backend_.get(), sink, quotes_.get(),
                                               std::move(clock));
    if (ignore_window) engine_->set_ignore_window(true);
  }

  LegResult Run() override {
    engine_->Run();
    return engine_->Result();
  }
  void RequestStop() override { engine_->RequestStop(); }

 private:
  std::unique_ptr<OrderBackend> backend_;
  std::unique_ptr<QuoteSource> quotes_;
  std::unique_ptr<ScenarioEngine> engine_;
};

}  // namespace

std::unique_ptr<LegRunner> EngineLegFactory::Create(const Scenario& leg) {
  return std::make_unique<EngineLegRunner>(leg, backend_(leg), quotes_(leg), sink_, clock_,
                                           ignore_window_);
}

}  // namespace kairos::exec
