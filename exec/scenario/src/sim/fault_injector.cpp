#include "fault_injector.h"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace kairos::exec {

FaultInjector::FaultInjector(FaultConfig cfg) : cfg_(cfg), enabled_(cfg.Enabled()), lcg_(cfg.seed) {
  if (enabled_ && (cfg_.ack_delay_ms > 0 || cfg_.ack_jitter_ms > 0)) {
    worker_ = std::thread([this] { Worker(); });
  }
}

FaultInjector::~FaultInjector() { Stop(); }

void FaultInjector::Stop() {
  {
    std::lock_guard<std::mutex> lock(q_mu_);
    if (stop_) return;
    stop_ = true;
  }
  q_cv_.notify_all();
  if (worker_.joinable()) worker_.join();
}

void FaultInjector::NoteSubmit() {
  if (!enabled_) return;
  std::lock_guard<std::mutex> lock(submit_mu_);
  ++submit_count_;
}

bool FaultInjector::DrawReject() {
  if (!enabled_ || cfg_.reject_rate <= 0.0) return false;
  bool reject = lcg_.Unit() < cfg_.reject_rate;
  return reject;
}

void FaultInjector::OnAck(const std::string& id, bool ok, const std::string& err,
                          const AckDeliverFn& deliver) {
  if (!enabled_ || !ok) {
    deliver(id, ok, err);
    return;
  }
  if (cfg_.ack_drop_rate > 0.0 && lcg_.Unit() < cfg_.ack_drop_rate) {
    std::fprintf(stderr, "kairos-sim-hub: fault drop-ack id=%s\n", id.c_str());
    return;
  }
  long delay = 0;
  if (cfg_.ack_delay_ms > 0 || cfg_.ack_jitter_ms > 0) {
    delay = cfg_.ack_delay_ms;
    if (cfg_.ack_jitter_ms > 0) delay += lcg_.InRange(0, cfg_.ack_jitter_ms);
  }
  if (delay <= 0) {
    deliver(id, ok, err);
    return;
  }
  std::fprintf(stderr, "kairos-sim-hub: fault delay-ack id=%s %ldms\n", id.c_str(), delay);
  auto when = std::chrono::steady_clock::now() + std::chrono::milliseconds(delay);
  Schedule(when, [deliver, id, err] { deliver(id, true, err); });
}

void FaultInjector::OnFill(const std::string& id, const Fill& fill, const FillDeliverFn& deliver) {
  if (!enabled_ || cfg_.partial_fill <= 1 || fill.shares <= 1) {
    deliver(id, fill);
    return;
  }
  long n = std::min<long>(cfg_.partial_fill, fill.shares);
  long base = fill.shares / n;
  for (long i = 0; i < n; ++i) {
    long shares = (i == n - 1) ? fill.shares - base * (n - 1) : base;
    deliver(id, Fill{shares, fill.price});
  }
}

bool FaultInjector::ConsumeDisconnectAfterN() {
  if (!enabled_ || cfg_.disconnect_after_n <= 0) return false;
  std::lock_guard<std::mutex> lock(submit_mu_);
  if (after_n_fired_ || submit_count_ < cfg_.disconnect_after_n) return false;
  after_n_fired_ = true;
  return true;
}

void FaultInjector::Schedule(std::chrono::steady_clock::time_point when, std::function<void()> fn) {
  {
    std::lock_guard<std::mutex> lock(q_mu_);
    if (stop_) return;
    queue_.emplace(when, std::move(fn));
  }
  q_cv_.notify_all();
}

void FaultInjector::Worker() {
  std::unique_lock<std::mutex> lock(q_mu_);
  while (!stop_) {
    if (queue_.empty()) {
      q_cv_.wait(lock);
      continue;
    }
    auto next = queue_.begin()->first;
    if (q_cv_.wait_until(lock, next) == std::cv_status::timeout) {
      auto it = queue_.begin();
      if (it == queue_.end() || it->first > std::chrono::steady_clock::now()) continue;
      std::function<void()> fn = std::move(it->second);
      queue_.erase(it);
      lock.unlock();
      fn();
      lock.lock();
    }
  }
}

}  // namespace kairos::exec
