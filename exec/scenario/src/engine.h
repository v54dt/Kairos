#ifndef KAIROS_EXEC_ENGINE_H_
#define KAIROS_EXEC_ENGINE_H_

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>

#include "engine_logic.h"
#include "order_backend.h"
#include "quote_book.h"
#include "scenario.h"
#include "uds_quote_client.h"

namespace kairos::exec {

// Drives a scenario: consumes the core quote feed into a QuoteBook, and on each
// tick decides place / re-peg / nothing (engine_logic), routing orders through
// the OrderBackend behind a 1 req/s gate. Run() blocks until the budget is met,
// the window closes, or RequestStop().
class ScenarioEngine {
 public:
  ScenarioEngine(Scenario scenario, OrderBackend* backend);

  void Run();
  void RequestStop();
  void set_ignore_window(bool v) { ignore_window_ = v; }

 private:
  void OnAck(const std::string& id, bool ok, const std::string& err);
  void OnFill(const std::string& id, const Fill& f);
  void SdkGate();
  std::string NextOrderId();

  Scenario s_;
  OrderBackend* backend_;
  QuoteBook book_;
  std::unique_ptr<UdsQuoteClient> quotes_;

  std::mutex mu_;
  std::condition_variable cv_;
  Accounting acct_;
  RestingOrder resting_;
  std::string resting_id_;
  long resting_filled_ = 0;
  bool complete_ = false;
  std::atomic<bool> stop_{false};
  bool ignore_window_ = false;

  std::mutex sdk_mu_;
  std::chrono::steady_clock::time_point last_sdk_{};
  long order_seq_ = 0;
  std::string oid_prefix_;
  int last_milestone_pct_ = 0;
};

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_ENGINE_H_
