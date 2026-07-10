#ifndef KAIROS_EXEC_SIM_FAULT_INJECTOR_H_
#define KAIROS_EXEC_SIM_FAULT_INJECTOR_H_

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "fault_config.h"
#include "order_backend.h"  // Fill

namespace kairos::exec {

// Seeded LCG (Knuth MMIX constants), the repo's convention (see core/src/tapegen):
// deterministic pseudo-randomness with no external dependency.
class Lcg {
 public:
  explicit Lcg(std::uint64_t seed) : state_(seed) {}
  std::uint64_t NextU64() {
    state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
    return state_;
  }
  long InRange(long lo, long hi) {
    if (hi <= lo) return lo;
    std::uint64_t span = static_cast<std::uint64_t>(hi - lo) + 1;
    return lo + static_cast<long>((NextU64() >> 33) % span);
  }
  double Unit() {  // uniform double in [0, 1)
    return static_cast<double>(NextU64() >> 11) * (1.0 / 9007199254740992.0);
  }

 private:
  std::uint64_t state_;
};

// Owns the fault config, the seeded RNG, and a single delayed-delivery worker
// thread. Off (config not Enabled) it is a pure inline passthrough: no draws, no
// thread, no allocation. All draws are guarded by the owning knob being active so
// the per-submit draw order (reject, then drop, then jitter) is fixed and a given
// seed replays the same sequence. Not owned by the fill logic; SimOrderBackend
// wires it into its ack/fill paths.
class FaultInjector {
 public:
  using AckDeliverFn = std::function<void(const std::string& id, bool ok, const std::string& err)>;
  using FillDeliverFn = std::function<void(const std::string& id, const Fill& fill)>;

  explicit FaultInjector(FaultConfig cfg);
  ~FaultInjector();

  bool Enabled() const { return enabled_; }

  // Count a submit (for disconnect_after_n) and report whether it should be
  // rejected. DrawReject only consumes an RNG draw when reject_rate is active.
  void NoteSubmit();
  bool DrawReject();

  // Route an engine ack. ok==false acks (validation rejects) always pass through
  // inline. An ok ack may be dropped (never delivered) or delayed on the worker
  // thread; otherwise it is delivered inline.
  void OnAck(const std::string& id, bool ok, const std::string& err, const AckDeliverFn& deliver);

  // Route an engine fill: split into partial_fill parts summing to the original
  // shares (last part carries the remainder), else deliver whole.
  void OnFill(const std::string& id, const Fill& fill, const FillDeliverFn& deliver);

  // True once when submit count first reaches disconnect_after_n.
  bool ConsumeDisconnectAfterN();
  long DisconnectEveryMs() const { return cfg_.disconnect_every_ms; }

  // Stop the worker thread, dropping any still-pending delayed acks.
  void Stop();

 private:
  void Worker();
  void Schedule(std::chrono::steady_clock::time_point when, std::function<void()> fn);

  const FaultConfig cfg_;
  const bool enabled_;
  Lcg lcg_;

  std::mutex submit_mu_;
  long submit_count_ = 0;
  bool after_n_fired_ = false;

  std::mutex q_mu_;
  std::condition_variable q_cv_;
  std::multimap<std::chrono::steady_clock::time_point, std::function<void()>> queue_;
  bool stop_ = false;
  std::thread worker_;
};

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_SIM_FAULT_INJECTOR_H_
