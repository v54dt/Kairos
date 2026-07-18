#ifndef KAIROS_EXEC_ROUNDTRIP_RUNNER_H_
#define KAIROS_EXEC_ROUNDTRIP_RUNNER_H_

// Orchestrates a same-day long round-trip (PR4). A single loop thread is the only
// writer of FSM state: the signal source, the HOLD quote feed, and each leg thread
// only enqueue events. The HOLD watchdog (stop / max-hold / staleness / 13:25
// forced exit) runs on the loop's 200ms tick from the injected clock, so the
// position stays protected even if the signal source goes silent.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#include "engine.h"  // ScenarioEngine, EngineClock, LegResult
#include "event_sink.h"
#include "order_backend.h"
#include "quote_source.h"
#include "roundtrip_fsm.h"
#include "roundtrip_legs.h"  // ExitReason, kForcedExitMin
#include "scenario.h"
#include "signal_client.h"
#include "signal_proto.h"  // SignalAction

namespace kairos::exec {

// The runner's view of the entry signal, injected so tests script it without a
// socket. The real adapter (SignalClientSource) wraps a PR1 SignalClient.
class SignalSource {
 public:
  struct Callbacks {
    std::function<void(SignalAction)> on_signal;
    std::function<void()> on_lost;
    std::function<void()> on_restored;
  };
  virtual ~SignalSource() = default;
  virtual void SetCallbacks(Callbacks cb) = 0;
  virtual void Start() = 0;
  virtual void Stop() = 0;
};

// One leg's execution. Run() blocks and returns the fill accounting; RequestStop()
// is thread-safe (the runner may stop a running leg from another thread).
class LegRunner {
 public:
  virtual ~LegRunner() = default;
  virtual LegResult Run() = 0;
  virtual void RequestStop() = 0;
};

// Builds a LegRunner for a derived leg scenario. The real factory wires a fresh
// OrderBackend + QuoteSource + ScenarioEngine per leg; tests fake it.
class LegFactory {
 public:
  virtual ~LegFactory() = default;
  virtual std::unique_ptr<LegRunner> Create(const Scenario& leg) = 0;
};

class RoundTripRunner {
 public:
  RoundTripRunner(Scenario scenario, SignalSource* signal, QuoteSource* hold_quotes,
                  LegFactory* legs, EventSink* sink, EngineClock clock = {});
  ~RoundTripRunner();

  RoundTripRunner(const RoundTripRunner&) = delete;
  RoundTripRunner& operator=(const RoundTripRunner&) = delete;

  // Blocks until a terminal state (FLAT / FAILED) or RequestStop(). 0 = flat/clean;
  // non-zero = failed with a possibly-remaining position (fail-closed).
  int Run();
  void RequestStop();

  RtState state() const { return state_; }  // test-only snapshot (loop thread owns it)

 private:
  struct RunnerEvent {
    RtEvent ev;
    LegResult leg;
  };

  void Enqueue(RtEvent ev, const LegResult& leg = {});
  void OnHoldQuote(const std::string& symbol, const TopOfBook& tob);
  FsmInput BuildInput(const RunnerEvent& e) const;
  RtState Execute(RtState from, const RunnerEvent& e, const FsmOutput& out);
  void CheckWatchdog();
  int NowMin() const;

  void StartEnterLeg(int trigger_min);
  void StartExitLeg(int now_min);
  void JoinEnterThread();
  void JoinExitThread();
  void Cleanup();

  void EmitPhase(EventCategory cat, Severity sev, const char* phase,
                 std::vector<std::pair<std::string, std::string>> fields);

  Scenario s_;
  SignalSource* signal_;
  QuoteSource* hold_quotes_;
  LegFactory* legs_;
  EventSink* sink_;
  EngineClock clock_;
  int arm_start_min_;
  int arm_end_min_;

  RtState state_ = RtState::kArmed;
  long held_shares_ = 0;
  long entry_avg_cents_ = 0;
  std::chrono::steady_clock::time_point enter_done_mono_{};
  ExitReason exit_reason_ = ExitReason::kForcedTime;
  bool done_ = false;
  int exit_code_ = 0;

  std::unique_ptr<LegRunner> enter_leg_;
  std::unique_ptr<LegRunner> exit_leg_;
  std::thread enter_thread_;
  std::thread exit_thread_;
  std::atomic<LegRunner*> active_leg_{nullptr};

  std::mutex q_mu_;
  std::condition_variable q_cv_;
  std::deque<RunnerEvent> queue_;
  bool stop_ = false;

  mutable std::mutex tob_mu_;
  TopOfBook last_tob_;
  bool have_tob_ = false;
};

// Real signal adapter: owns a SignalClient and forwards its callbacks. Header-only
// so main and tests build it without a new translation unit.
class SignalClientSource : public SignalSource {
 public:
  SignalClientSource(std::string socket_path, SignalSubscribe sub);
  void SetCallbacks(Callbacks cb) override { cb_ = std::move(cb); }
  void Start() override;
  void Stop() override { client_.Stop(); }

 private:
  Callbacks cb_;
  SignalClient client_;
};

// Real leg factory: each leg gets its own OrderBackend + QuoteSource + engine. The
// backend/quote builders are injected so the mode (paper-instant / queue-sim / live)
// is decided by main, not baked in here.
class EngineLegFactory : public LegFactory {
 public:
  using BackendFn = std::function<std::unique_ptr<OrderBackend>(const Scenario&)>;
  using QuoteFn = std::function<std::unique_ptr<QuoteSource>(const Scenario&)>;

  EngineLegFactory(BackendFn backend, QuoteFn quotes, EventSink* sink, EngineClock clock,
                   bool ignore_window);
  std::unique_ptr<LegRunner> Create(const Scenario& leg) override;

 private:
  BackendFn backend_;
  QuoteFn quotes_;
  EventSink* sink_;
  EngineClock clock_;
  bool ignore_window_;
};

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_ROUNDTRIP_RUNNER_H_
