#include "engine.h"

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <thread>
#include <utility>

#include "socket_path.h"
#include "tw_market.h"

namespace kairos::exec {

namespace {

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

}  // namespace

ScenarioEngine::ScenarioEngine(Scenario scenario, OrderBackend* backend)
    : s_(std::move(scenario)), backend_(backend) {
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
  } else {
    std::fprintf(stderr, "kairos-exec: order %s rejected: %s\n", id.c_str(), err.c_str());
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
  if (id != resting_id_) return;
  resting_filled_ += f.shares;
  acct_.RecordFill(s_, f.price, f.shares);
  std::printf("kairos-exec: fill %s %ld @ %s  (cum %ld sh / NT$ %ld, fee %ld)\n", id.c_str(),
              f.shares, CentsToString(f.price).c_str(), acct_.filled_shares, acct_.FilledTwd(),
              acct_.total_fee_twd);
  if (resting_filled_ >= resting_.shares) ClearResting();
  int pct = s_.budget_twd > 0 ? static_cast<int>(acct_.FilledTwd() * 100 / s_.budget_twd) : 0;
  if (pct >= last_milestone_pct_ + 25) {
    last_milestone_pct_ = pct - (pct % 25);
    std::printf("kairos-exec: progress %d%%\n", last_milestone_pct_);
  }
  if (acct_.BudgetReached(s_)) complete_ = true;
  std::fflush(stdout);
  cv_.notify_all();
}

void ScenarioEngine::Run() {
  if (!backend_->Connect()) {
    std::fprintf(stderr, "kairos-exec: order backend connect failed\n");
    return;
  }
  backend_->SetCallbacks(
      [this](const std::string& id, bool ok, const std::string& e) { OnAck(id, ok, e); },
      [this](const std::string& id, const Fill& f) { OnFill(id, f); },
      [this](const std::string& id, bool ok) { OnCancel(id, ok); });
  quotes_->Start();
  std::printf("kairos-exec: %s %s NT$ %ld, %s, %s\n", SideName(s_.side), s_.symbol.c_str(),
              s_.budget_twd, PricePolicyName(s_.price_policy), s_.live ? "*** LIVE ***" : "PAPER");
  std::fflush(stdout);

  while (!stop_) {
    if (!ignore_window_) {
      LocalNow n = NowUtc8();
      bool past_end = n.hhmm >= s_.window_end_hhmm;
      bool in_window =
          n.hhmm >= s_.window_start_hhmm && !past_end && (!s_.weekdays_only || n.weekday);
      if (past_end) break;
      if (!in_window) {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait_for(lock, std::chrono::seconds(1));
        continue;
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
      Action act = DecideAction(s_, tob, resting, remaining);
      if (act.done && !resting.active) {
        std::lock_guard<std::mutex> lock(mu_);
        complete_ = true;
        break;
      }
      if (act.kind == ActionKind::kPlace) {
        std::string id = NextOrderId();
        {
          std::lock_guard<std::mutex> lock(mu_);
          resting_ = RestingOrder{true, act.price, act.shares};
          resting_id_ = id;
          resting_filled_ = 0;
          resting_acked_ = false;
          cancelling_ = false;
        }
        SdkGate();
        backend_->Submit(id, s_.side, act.price, act.shares);
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

  // Wind down: cancel the working order only if the broker acked it (ArbX-aligned).
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
}

void ScenarioEngine::RequestStop() {
  stop_ = true;
  cv_.notify_all();
}

}  // namespace kairos::exec
