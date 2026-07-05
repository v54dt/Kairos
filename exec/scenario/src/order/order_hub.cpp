#include "order_hub.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <utility>

#include "order_codec.h"

namespace kairos::exec {

namespace {

long NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
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
    : backend_(backend), send_(std::move(send)), start_epoch_s_(NowUs() / 1000000) {}

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
  if (!DecodeOrder(data, len, &msg)) return;
  if (msg.kind == OrderMsgKind::kSubmit) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      routes_[msg.submit.id] = Route{client, msg.submit.shares, false, false};
      ClientStats& cs = clients_[client];
      if (cs.prefix.empty()) ParseId(msg.submit.id, &cs.prefix, &cs.pid);
      ++cs.submitted;
      cs.last_activity_us = NowUs();
    }
    backend_->Submit(msg.submit);  // gated inside the backend; never hold mu_ across it
  } else if (msg.kind == OrderMsgKind::kCancel) {
    backend_->Cancel(msg.cancel.id);
  }
}

void OrderHub::OnClientDisconnect(int client) {
  std::lock_guard<std::mutex> lock(mu_);
  for (auto it = routes_.begin(); it != routes_.end();) {
    if (it->second.client == client) {
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

void OrderHub::OnAck(const std::string& id, bool ok, const std::string& err) {
  int client = -1;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = routes_.find(id);
    if (it != routes_.end()) {
      client = it->second.client;
      if (ok) it->second.acked = true;
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
      it->second.shares_remaining -= f.shares;
      if (it->second.shares_remaining <= 0) it->second.closed = true;
      auto cs = clients_.find(client);
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
      if (ok) it->second.closed = true;
      auto cs = clients_.find(client);
      if (cs != clients_.end()) {
        if (ok) ++cs->second.cancelled;
        cs->second.last_activity_us = NowUs();
      }
    }
  }
  if (client >= 0) send_(client, EncodeOrderCancelResult({id, ok}));
}

HubStatus OrderHub::CaptureStatus() const {
  HubStatus s;
  s.start_epoch_s = start_epoch_s_;
  s.written_epoch_s = NowUs() / 1000000;
  std::lock_guard<std::mutex> lock(mu_);
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
