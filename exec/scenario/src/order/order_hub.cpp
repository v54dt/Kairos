#include "order_hub.h"

#include <utility>

#include "order_codec.h"

namespace kairos::exec {

OrderHub::OrderHub(OrderBackend* backend, SendFn send)
    : backend_(backend), send_(std::move(send)) {}

bool OrderHub::Start() {
  backend_->SetCallbacks(
      [this](const std::string& id, bool ok, const std::string& e) { OnAck(id, ok, e); },
      [this](const std::string& id, const Fill& f) { OnFill(id, f); },
      [this](const std::string& id, bool ok) { OnCancel(id, ok); });
  return backend_->Connect();
}

void OrderHub::Stop() { backend_->Disconnect(); }

void OrderHub::OnClientMessage(int client, const std::uint8_t* data, std::size_t len) {
  OrderMessage msg;
  if (!DecodeOrder(data, len, &msg)) return;
  if (msg.kind == OrderMsgKind::kSubmit) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      id_to_client_[msg.submit.id] = client;  // route this id's events back here
    }
    backend_->Submit(msg.submit);  // gated inside the backend; never hold mu_ across it
  } else if (msg.kind == OrderMsgKind::kCancel) {
    backend_->Cancel(msg.cancel.id);
  }
}

void OrderHub::OnClientDisconnect(int client) {
  std::lock_guard<std::mutex> lock(mu_);
  for (auto it = id_to_client_.begin(); it != id_to_client_.end();) {
    if (it->second == client) {
      it = id_to_client_.erase(it);
    } else {
      ++it;
    }
  }
}

int OrderHub::ClientFor(const std::string& id) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = id_to_client_.find(id);
  return it == id_to_client_.end() ? -1 : it->second;
}

void OrderHub::OnAck(const std::string& id, bool ok, const std::string& err) {
  int client = ClientFor(id);
  if (client >= 0) send_(client, EncodeOrderAck({id, ok, err}));
}

void OrderHub::OnFill(const std::string& id, const Fill& f) {
  int client = ClientFor(id);
  if (client >= 0) send_(client, EncodeOrderFill({id, f.shares, f.price}));
}

void OrderHub::OnCancel(const std::string& id, bool ok) {
  int client = ClientFor(id);
  if (client >= 0) send_(client, EncodeOrderCancelResult({id, ok}));
}

}  // namespace kairos::exec
