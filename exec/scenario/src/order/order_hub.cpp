#include "order_hub.h"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <utility>

#include "order_codec.h"

namespace kairos::exec {

namespace {

long NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// Local calendar date as YYYYMMDD; the day-realized notional resets when it changes.
long LocalTradingDay() {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
  localtime_r(&t, &tm);
  return (tm.tm_year + 1900) * 10000L + (tm.tm_mon + 1) * 100L + tm.tm_mday;
}

// prefix = the "k<pid>" head of a user_defined_id (k<pid>-<seq>); pid its digits.
void ParseId(const std::string& id, std::string* prefix, long* pid) {
  auto dash = id.find('-');
  *prefix = dash == std::string::npos ? id : id.substr(0, dash);
  *pid = (prefix->size() > 1 && (*prefix)[0] == 'k') ? std::strtol(prefix->c_str() + 1, nullptr, 10)
                                                     : 0;
}

}  // namespace

OrderHub::OrderHub(OrderBackend* backend, SendFn send)
    : OrderHub(backend, std::move(send), RiskConfig()) {}

OrderHub::OrderHub(OrderBackend* backend, SendFn send, RiskConfig risk)
    : backend_(backend),
      send_(std::move(send)),
      start_epoch_s_(NowUs() / 1000000),
      current_trading_day_(LocalTradingDay()),
      risk_(std::move(risk)) {}

long OrderHub::CurrentTradingDay() const {
  return forced_trading_day_ >= 0 ? forced_trading_day_ : LocalTradingDay();
}

void OrderHub::SetTradingDayForTest(long day) {
  std::lock_guard<std::mutex> lock(mu_);
  forced_trading_day_ = day;
}

void OrderHub::RejectSubmit(int client, const std::string& id, const std::string& reason) {
  send_(client, EncodeOrderAck({id, false, reason}));
}

void OrderHub::SetAdminHalt(bool halted) { admin_halt_.store(halted); }

bool OrderHub::IsHaltedNow() const {
  if (admin_halt_.load()) return true;
  return !risk_.halt_file_path.empty() && ::access(risk_.halt_file_path.c_str(), F_OK) == 0;
}

bool OrderHub::Start() {
  backend_->SetCallbacks(
      [this](const std::string& id, bool ok, const std::string& e) { OnAck(id, ok, e); },
      [this](const std::string& id, const Fill& f) { OnFill(id, f); },
      [this](const std::string& id, bool ok) { OnCancel(id, ok); });
  return backend_->Connect();
}

void OrderHub::Stop() { backend_->Disconnect(); }

void OrderHub::OnClientConnect(int client) {
  std::lock_guard<std::mutex> lock(mu_);
  ClientStats& cs = clients_[client];
  if (cs.last_activity_us == 0) cs.last_activity_us = NowUs();
}

void OrderHub::OnClientMessage(int client, const std::uint8_t* data, std::size_t len) {
  OrderMessage msg;
  if (!DecodeOrder(data, len, &msg)) return;  // undecodable frame: dropped, never forwarded
  if (msg.kind == OrderMsgKind::kSubmit) {
    const OrderSubmitMsg& o = msg.submit;
    if (IsHaltedNow()) {  // checked before mu_: immediate, no registry work
      RejectSubmit(client, o.id, "hub halted by admin");
      return;
    }
    std::string reject;
    {
      std::lock_guard<std::mutex> lock(mu_);
      long today = CurrentTradingDay();
      if (today != current_trading_day_) {
        current_trading_day_ = today;
        account_day_realized_cents_ = 0;
      }
      // Fail-closed field validation: a doubtful submit is rejected, never forwarded.
      auto live = routes_.find(o.id);
      if (o.id.empty() || o.symbol.empty() || o.shares <= 0 || o.price <= 0 ||
          o.price > kMaxTwStockPriceCents) {
        reject = "invalid order fields";
      } else if (live != routes_.end() && !live->second.closed) {
        reject = "duplicate live order id";
      } else if (std::int64_t n = static_cast<std::int64_t>(o.price) * o.shares;
                 risk_.max_account_notional_cents > 0 &&
                 account_day_realized_cents_ + account_open_notional_cents_ + n >
                     risk_.max_account_notional_cents) {
        reject = "account notional cap exceeded";
      } else if (risk_.max_open_orders_per_client > 0 &&
                 clients_[client].open_orders + 1 > risk_.max_open_orders_per_client) {
        reject = "per-client open-order limit exceeded";
      } else if (risk_.max_open_notional_per_client_cents > 0 &&
                 clients_[client].open_notional + n > risk_.max_open_notional_per_client_cents) {
        reject = "per-client open-notional limit exceeded";
      } else {
        ClientStats& cs = clients_[client];
        routes_[o.id] = Route{client, o.shares, false, false, o.symbol, o.side, o.price};
        account_open_notional_cents_ += n;
        cs.open_notional += n;
        ++cs.open_orders;
        if (cs.prefix.empty()) ParseId(o.id, &cs.prefix, &cs.pid);
        ++cs.submitted;
        cs.last_activity_us = NowUs();
      }
    }
    if (!reject.empty()) {
      RejectSubmit(client, o.id, reject);  // ack ok=false, off the lock; no backend forward
      return;
    }
    backend_->Submit(o);  // gated inside the backend; never hold mu_ across it
  } else if (msg.kind == OrderMsgKind::kCancel) {
    backend_->Cancel(msg.cancel.id);  // cancels are never gated: always allow flattening
  }
}

void OrderHub::OnClientDisconnect(int client) {
  std::lock_guard<std::mutex> lock(mu_);
  for (auto it = routes_.begin(); it != routes_.end();) {
    if (it->second.client == client) {
      ReleaseOpen(it->second);  // free any still-open reserved notional before dropping
      it = routes_.erase(it);
    } else {
      ++it;
    }
  }
  clients_.erase(client);
}

int OrderHub::ClientFor(const std::string& id) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = routes_.find(id);
  return it == routes_.end() ? -1 : it->second.client;
}

void OrderHub::ReleaseOpen(Route& r) {
  if (r.closed) return;  // release reserved open notional exactly once
  std::int64_t n = static_cast<std::int64_t>(r.price) * r.shares_remaining;
  account_open_notional_cents_ -= n;
  auto cs = clients_.find(r.client);
  if (cs != clients_.end()) {
    cs->second.open_notional -= n;
    --cs->second.open_orders;
  }
  r.closed = true;
}

void OrderHub::OnAck(const std::string& id, bool ok, const std::string& err) {
  int client = -1;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = routes_.find(id);
    if (it != routes_.end()) {
      client = it->second.client;
      if (ok) {
        it->second.acked = true;
      } else {
        ReleaseOpen(it->second);  // backend rejected: free its reserved notional
      }
      auto cs = clients_.find(client);
      if (cs != clients_.end()) cs->second.last_activity_us = NowUs();
    }
  }
  if (client >= 0) send_(client, EncodeOrderAck({id, ok, err}));
}

void OrderHub::OnFill(const std::string& id, const Fill& f) {
  int client = -1;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = routes_.find(id);
    if (it != routes_.end()) {
      client = it->second.client;
      auto cs = clients_.find(client);
      if (!it->second.closed) {
        long before = it->second.shares_remaining;
        long acct_sh = std::min<long>(f.shares, before > 0 ? before : 0);
        std::int64_t open_delta = static_cast<std::int64_t>(it->second.price) * acct_sh;
        account_open_notional_cents_ -= open_delta;
        account_day_realized_cents_ += static_cast<std::int64_t>(f.price) * f.shares;
        it->second.shares_remaining -= f.shares;
        if (cs != clients_.end()) cs->second.open_notional -= open_delta;
        if (it->second.shares_remaining <= 0) {
          it->second.closed = true;
          if (cs != clients_.end()) --cs->second.open_orders;
        }
      }
      if (cs != clients_.end()) {
        ++cs->second.filled;
        cs->second.last_activity_us = NowUs();
      }
    }
  }
  if (client >= 0) send_(client, EncodeOrderFill({id, f.shares, f.price}));
}

void OrderHub::OnCancel(const std::string& id, bool ok) {
  int client = -1;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = routes_.find(id);
    if (it != routes_.end()) {
      client = it->second.client;
      auto cs = clients_.find(client);
      if (ok) {
        ReleaseOpen(it->second);  // successful cancel: free reserved notional
        if (cs != clients_.end()) ++cs->second.cancelled;
      }
      if (cs != clients_.end()) cs->second.last_activity_us = NowUs();
    }
  }
  if (client >= 0) send_(client, EncodeOrderCancelResult({id, ok}));
}

HubStatus OrderHub::CaptureStatus() const {
  HubStatus s;
  s.start_epoch_s = start_epoch_s_;
  s.written_epoch_s = NowUs() / 1000000;
  s.halted = IsHaltedNow();  // read-only access() off the hot path
  std::lock_guard<std::mutex> lock(mu_);
  s.account_open_notional_cents = account_open_notional_cents_;
  s.account_day_realized_cents = account_day_realized_cents_;
  s.max_account_notional_cents = risk_.max_account_notional_cents;
  std::unordered_map<int, int> open;
  for (const auto& [id, r] : routes_) {
    if (r.acked && !r.closed) ++open[r.client];
  }
  s.client_count = static_cast<int>(clients_.size());
  for (const auto& [fd, cs] : clients_) {
    ClientStatus c;
    c.prefix = cs.prefix;
    c.pid = cs.pid;
    auto it = open.find(fd);
    c.open = it == open.end() ? 0 : it->second;
    c.submitted = cs.submitted;
    c.filled = cs.filled;
    c.cancelled = cs.cancelled;
    c.last_activity_s = cs.last_activity_us / 1000000;
    s.clients.push_back(std::move(c));
  }
  std::sort(s.clients.begin(), s.clients.end(),
            [](const ClientStatus& a, const ClientStatus& b) { return a.prefix < b.prefix; });
  return s;
}

}  // namespace kairos::exec
