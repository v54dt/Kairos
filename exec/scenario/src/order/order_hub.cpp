#include "order_hub.h"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <utility>

#include "enum_names.h"  // SideName/BoardName/MarketName
#include "order_codec.h"
#include "order_journal.h"  // AppendFill + trading-day helpers (shared journal format)
#include "time_util.h"      // SystemNowUs / SteadyNowMs
#include "tw_market.h"      // CentsToString

namespace kairos::exec {

namespace {

constexpr std::size_t kDupRingCap = 1024;  // hard bound on the dup ring under a burst

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
      forwarded_send_(send_),
      start_epoch_s_(SystemNowUs() / 1000000),
      current_trading_day_(TradingDayNumUtc8(std::chrono::system_clock::now())),
      risk_(std::move(risk)) {}

void OrderHub::SetForwardedSender(SendFn fn) { forwarded_send_ = std::move(fn); }

OrderHub::~OrderHub() { StopForwarder(); }

long OrderHub::CurrentTradingDay() const {
  return forced_trading_day_ >= 0 ? forced_trading_day_
                                  : TradingDayNumUtc8(std::chrono::system_clock::now());
}

long OrderHub::NowMonoMs() const {
  if (forced_mono_ms_ >= 0) return forced_mono_ms_;
  return SteadyNowMs();
}

void OrderHub::RemoveDupEntry(const std::string& id) {
  for (auto it = dup_ring_.begin(); it != dup_ring_.end(); ++it) {
    if (it->id == id) {
      dup_ring_.erase(it);
      return;
    }
  }
}

void OrderHub::RememberOrder(const std::string& id, const std::string& symbol, Side side,
                             long shares) {
  order_meta_[id] = OrderMeta{symbol, side, shares, 0};
  meta_fifo_.push_back(id);
  while (meta_fifo_.size() > kOrderMetaCap) {
    order_meta_.erase(meta_fifo_.front());
    meta_fifo_.pop_front();
  }
}

void OrderHub::ForgetOrder(const std::string& id) { order_meta_.erase(id); }

void OrderHub::SetTradingDayForTest(long day) {
  std::lock_guard<std::mutex> lock(mu_);
  forced_trading_day_ = day;
}

void OrderHub::SetMonoMsForTest(long ms) {
  std::lock_guard<std::mutex> lock(mu_);
  forced_mono_ms_ = ms;
}

std::size_t OrderHub::OrderMetaCountForTest() const {
  std::lock_guard<std::mutex> lock(mu_);
  return order_meta_.size();
}

void OrderHub::RejectSubmit(int client, const std::string& id, const std::string& reason) {
  std::fprintf(stderr, "kairos-order-hub: reject client=%d id=%s reason=%s\n", client, id.c_str(),
               reason.c_str());
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
  if (!backend_->Connect()) return false;
  StartForwarder();
  return true;
}

void OrderHub::Stop() {
  StopForwarder();  // fail-closed: any still-queued submit is journaled, never sent
  backend_->Disconnect();
}

void OrderHub::StartForwarder() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    stop_forwarder_ = false;
  }
  forwarder_ = std::thread([this] { Forwarder(); });
}

void OrderHub::StopForwarder() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    stop_forwarder_ = true;
  }
  cv_.notify_all();
  if (forwarder_.joinable()) forwarder_.join();
}

void OrderHub::ReleaseQueuedTerminal(const std::string& id) {
  auto it = routes_.find(id);
  if (it != routes_.end()) {
    ReleaseOpen(it->second);  // free reserved notional exactly once (guards on r.closed)
    open_ids_.erase(id);
    routes_.erase(it);
  }
  RemoveDupEntry(id);  // never sent: a legitimate re-order must not read as a dup
  ForgetOrder(id);     // never reaches the broker, so nothing left to journal by id
}

void OrderHub::Forwarder() {
  std::unique_lock<std::mutex> lock(mu_);
  for (;;) {
    cv_.wait(lock, [this] {
      return stop_forwarder_ || !pending_cancels_.empty() || !pending_submits_.empty();
    });
    if (stop_forwarder_) break;
    if (!pending_cancels_.empty()) {  // flatten priority: cancels drain before submits
      std::string id = std::move(pending_cancels_.front());
      pending_cancels_.pop_front();
      forwarding_ = true;
      lock.unlock();
      backend_->Cancel(id);  // gated inside the backend; only this thread blocks there
      lock.lock();
      forwarding_ = false;
      drain_cv_.notify_all();
      continue;
    }
    PendingSubmit p = std::move(pending_submits_.front());
    pending_submits_.pop_front();
    forwarding_ = true;
    lock.unlock();
    if (IsHaltedNow()) {  // halt set after accept: reject at dequeue, never forward
      lock.lock();
      ReleaseQueuedTerminal(p.order.id);
      bool connected = clients_.count(p.client) > 0;
      lock.unlock();
      if (connected) RejectSubmit(p.client, p.order.id, "hub halted by admin");
      if (FlowJournalOn())
        OrderFlowJournal::AppendAck(risk_.journal_dir, p.order.id, false, "hub halted by admin");
    } else {
      // Tell the owning client its order reached the broker so it can rebase its
      // ack-timeout clock; sent BEFORE Submit so it precedes any synchronous ack.
      // Best-effort and non-blocking (server-injected): a stalled client drops the
      // hint rather than blocking this shared forwarder ahead of other scenarios.
      lock.lock();
      bool connected = clients_.count(p.client) > 0;
      lock.unlock();
      if (connected) forwarded_send_(p.client, EncodeOrderForwarded({p.order.id}));
      if (FlowJournalOn()) OrderFlowJournal::AppendForwarded(risk_.journal_dir, p.order.id);
      backend_->Submit(p.order);  // gated inside the backend; only this thread blocks there
    }
    lock.lock();
    forwarding_ = false;
    drain_cv_.notify_all();
  }
  // Shutdown: fail-closed. A submit accepted but not yet forwarded is dropped
  // (its reservation released) and journaled, never sent to the broker.
  while (!pending_submits_.empty()) {
    PendingSubmit p = std::move(pending_submits_.front());
    pending_submits_.pop_front();
    ReleaseQueuedTerminal(p.order.id);
    bool connected = clients_.count(p.client) > 0;
    lock.unlock();
    if (connected) RejectSubmit(p.client, p.order.id, "hub shutting down");
    if (FlowJournalOn())
      OrderFlowJournal::AppendAck(risk_.journal_dir, p.order.id, false, "hub shutting down");
    lock.lock();
  }
  // A queued broker-bound cancel is a flatten of a live order: forward it before
  // the backend disconnects rather than dropping it, so no working order is left
  // un-cancelled at the exchange after a mid-session hub restart.
  while (!pending_cancels_.empty()) {
    std::string id = std::move(pending_cancels_.front());
    pending_cancels_.pop_front();
    lock.unlock();
    backend_->Cancel(id);
    lock.lock();
  }
}

void OrderHub::DrainForwardedForTest() {
  std::unique_lock<std::mutex> lock(mu_);
  drain_cv_.wait(lock, [this] {
    return pending_submits_.empty() && pending_cancels_.empty() && !forwarding_;
  });
}

void OrderHub::OnClientConnect(int client) {
  std::lock_guard<std::mutex> lock(mu_);
  ClientStats& cs = clients_[client];
  if (cs.last_activity_us == 0) cs.last_activity_us = SystemNowUs();
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
    std::string submit_prefix;  // captured under mu_ for the off-lock audit line
    {
      std::lock_guard<std::mutex> lock(mu_);
      long today = CurrentTradingDay();
      if (today != current_trading_day_) {
        current_trading_day_ = today;
        account_day_realized_cents_ = 0;
        last_fill_price_cents_.clear();  // yesterday's fill is not a valid collar reference today
        order_meta_.clear();             // yesterday's ids are not journaled under today's file
        meta_fifo_.clear();
      }
      long now_ms = risk_.dup_order_window_ms > 0 ? NowMonoMs() : 0;
      auto cs_it =
          clients_.find(client);  // non-inserting snapshot; the gate never mutates clients_
      RiskStateView view{routes_,
                         open_ids_,
                         last_fill_price_cents_,
                         dup_ring_,
                         account_open_notional_cents_,
                         account_day_realized_cents_,
                         cs_it != clients_.end() ? cs_it->second.open_orders : 0,
                         cs_it != clients_.end() ? cs_it->second.open_notional : 0,
                         now_ms};
      std::optional<std::string> gate_reject = gate_.Evaluate(o, view);
      if (gate_reject) {
        reject = *gate_reject;
      } else {
        std::int64_t n = static_cast<std::int64_t>(o.price) * o.shares;
        ClientStats& cs = clients_[client];
        routes_[o.id] = Route{client, o.shares, false, false, o.symbol, o.side, o.price};
        open_ids_.insert(o.id);
        RememberOrder(o.id, o.symbol, o.side, o.shares);
        account_open_notional_cents_ += n;
        cs.open_notional += n;
        ++cs.open_orders;
        if (cs.prefix.empty()) ParseId(o.id, &cs.prefix, &cs.pid);
        submit_prefix = cs.prefix;
        ++cs.submitted;
        cs.last_activity_us = SystemNowUs();
        if (risk_.dup_order_window_ms > 0) {
          dup_ring_.push_back({o.id, o.symbol, o.side, o.shares, o.price, now_ms});
          if (dup_ring_.size() > kDupRingCap) dup_ring_.pop_front();
        }
        // Enqueue under the same lock as the reservation so a cancel racing in
        // always finds the still-queued submit (it never sees a reserved-but-
        // unqueued order). The forwarder is the only thread that hits the gate.
        pending_submits_.push_back({client, o});
      }
    }
    if (!reject.empty()) {
      RejectSubmit(client, o.id, reject);  // ack ok=false, off the lock; no backend forward
      return;
    }
    std::fprintf(stderr, "kairos-order-hub: submit client=%d id=%s %s %s %ld @ %s\n", client,
                 o.id.c_str(), o.symbol.c_str(), o.side == Side::kBuy ? "Buy" : "Sell", o.shares,
                 CentsToString(o.price).c_str());
    if (FlowJournalOn() &&
        !OrderFlowJournal::AppendSubmit(risk_.journal_dir, o.id, submit_prefix, o.symbol,
                                        SideName(o.side), BoardName(o.board), MarketName(o.market),
                                        o.funding_type, o.time_in_force, o.shares, o.price)) {
      std::fprintf(stderr, "kairos-order-hub: FAILED to journal order-flow submit id=%s\n",
                   o.id.c_str());
    }
    cv_.notify_one();  // hand the queued submit to the forwarder; never forward here
  } else if (msg.kind == OrderMsgKind::kCancel) {
    const std::string& id = msg.cancel.id;
    std::fprintf(stderr, "kairos-order-hub: cancel-request client=%d id=%s\n", client, id.c_str());
    if (FlowJournalOn() && !OrderFlowJournal::AppendCancelReq(risk_.journal_dir, id)) {
      std::fprintf(stderr, "kairos-order-hub: FAILED to journal order-flow cancel_req id=%s\n",
                   id.c_str());
    }
    bool withdrawn = false;
    int wclient = -1;
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto pit = pending_submits_.end();
      for (auto it = pending_submits_.begin(); it != pending_submits_.end(); ++it) {
        if (it->order.id == id) {
          pit = it;
          break;
        }
      }
      if (pit != pending_submits_.end()) {
        // Still queued: withdraw locally. Release its reservation and drop it; the
        // broker is never contacted. Whoever holds mu_ first (this or the forwarder
        // popping) wins; if already popped, we fall through to the broker cancel.
        wclient = pit->client;
        pending_submits_.erase(pit);
        ReleaseQueuedTerminal(id);
        if (auto cs = clients_.find(wclient); cs != clients_.end()) {
          ++cs->second.cancelled;
          cs->second.last_activity_us = SystemNowUs();
        }
        withdrawn = true;
      } else {
        pending_cancels_.push_back(id);  // let the forwarder issue the broker cancel
      }
    }
    if (withdrawn) {
      std::fprintf(stderr, "kairos-order-hub: cancel-withdrew queued id=%s -> client=%d\n",
                   id.c_str(), wclient);
      send_(wclient, EncodeOrderCancelResult({id, true}));
      if (FlowJournalOn() &&
          !OrderFlowJournal::AppendCancelAck(risk_.journal_dir, id, true, /*withdrawn=*/true)) {
        std::fprintf(stderr, "kairos-order-hub: FAILED to journal order-flow cancel_ack id=%s\n",
                     id.c_str());
      }
      return;
    }
    cv_.notify_one();  // cancels are never gated: always allow flattening
  }
}

void OrderHub::OnClientDisconnect(int client) {
  std::vector<std::string> purged;  // still-queued submits: never sent, dropped whole
  {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto it = pending_submits_.begin(); it != pending_submits_.end();) {
      if (it->client == client) {
        purged.push_back(it->order.id);
        it = pending_submits_.erase(it);
      } else {
        ++it;
      }
    }
    for (auto it = routes_.begin(); it != routes_.end();) {
      if (it->second.client == client) {
        ReleaseOpen(it->second);  // free any still-open reserved notional before dropping
        open_ids_.erase(it->first);
        it = routes_.erase(it);
      } else {
        ++it;
      }
    }
    // The purged submits never reached the broker, so no fill will ever arrive:
    // drop their dup/meta too (their reservation was freed by the routes_ loop).
    for (const auto& id : purged) {
      RemoveDupEntry(id);
      ForgetOrder(id);
    }
    clients_.erase(client);
  }
  if (FlowJournalOn()) {
    for (const auto& id : purged)
      OrderFlowJournal::AppendAck(risk_.journal_dir, id, false, "purged: client disconnect");
  }
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
        open_ids_.erase(it->first);
        RemoveDupEntry(id);  // terminal: a legitimate retry must not read as a dup
        ForgetOrder(id);     // rejected: it will never fill, no journaling needed
      }
      auto cs = clients_.find(client);
      if (cs != clients_.end()) cs->second.last_activity_us = SystemNowUs();
    }
  }
  if (client >= 0) {
    std::fprintf(stderr, "kairos-order-hub: ack id=%s ok=%d err=%s -> client=%d\n", id.c_str(),
                 ok ? 1 : 0, err.c_str(), client);
    send_(client, EncodeOrderAck({id, ok, err}));
  } else {
    std::fprintf(stderr, "kairos-order-hub: UNROUTABLE ack id=%s ok=%d err=%s (no client route)\n",
                 id.c_str(), ok ? 1 : 0, err.c_str());
  }
  if (FlowJournalOn() && !OrderFlowJournal::AppendAck(risk_.journal_dir, id, ok, err)) {
    std::fprintf(stderr, "kairos-order-hub: FAILED to journal order-flow ack id=%s\n", id.c_str());
  }
}

void OrderHub::OnFill(const std::string& id, const Fill& f) {
  int client = -1;
  bool named = false;       // the id is still in order_meta_ (needed only when unroutable)
  long journal_shares = 0;  // unroutable shares to persist, capped by the order size
  std::string symbol;
  Side side = Side::kBuy;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = routes_.find(id);
    if (it != routes_.end()) {
      client = it->second.client;
      auto cs = clients_.find(client);
      if (risk_.price_collar_pct > 0) last_fill_price_cents_[it->second.symbol] = f.price;
      // A routed fill is journaled by the owning trader; record it against the
      // order size so a redelivery after that trader is gone is not journaled again.
      if (auto m = order_meta_.find(id); m != order_meta_.end()) {
        m->second.shares_accounted =
            std::min<long>(m->second.shares_total, m->second.shares_accounted + f.shares);
      }
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
          open_ids_.erase(it->first);
          RemoveDupEntry(id);  // terminal: a legitimate re-order must not read as a dup
          ForgetOrder(id);     // fully filled: no further fill to journal
          if (cs != clients_.end()) --cs->second.open_orders;
        }
      }
      if (cs != clients_.end()) {
        ++cs->second.filled;
        cs->second.last_activity_us = SystemNowUs();
      }
    } else {
      auto m = order_meta_.find(id);
      if (m != order_meta_.end()) {
        long room = m->second.shares_total - m->second.shares_accounted;
        journal_shares = std::min<long>(f.shares, room > 0 ? room : 0);
        if (journal_shares > 0) {
          named = true;
          symbol = m->second.symbol;
          side = m->second.side;
          m->second.shares_accounted += journal_shares;
          if (m->second.shares_accounted >= m->second.shares_total) ForgetOrder(id);
        }
      }
    }
  }
  if (client >= 0) {
    std::fprintf(stderr, "kairos-order-hub: fill id=%s %ld @ %s -> client=%d\n", id.c_str(),
                 f.shares, CentsToString(f.price).c_str(), client);
    send_(client, EncodeOrderFill({id, f.shares, f.price}));
    if (FlowJournalOn() &&
        !OrderFlowJournal::AppendFill(risk_.journal_dir, id, f.shares, f.price, false)) {
      std::fprintf(stderr, "kairos-order-hub: FAILED to journal order-flow fill id=%s\n",
                   id.c_str());
    }
    return;
  }
  // No client route: the trader that placed this order has exited. Persist the
  // fill into the SAME per-(symbol,side,day) journal it replays, so a restart
  // accounts it and does not re-buy the budget. Only unroutable fills are
  // journaled here; a routed fill is the trader's own to record. journal_shares
  // is capped at the order's still-unaccounted quantity, so a redelivered fill
  // on a fully-accounted id (named=false, journal_shares=0) is not written twice.
  std::fprintf(stderr, "kairos-order-hub: UNROUTABLE fill id=%s %ld @ %s (no client route)\n",
               id.c_str(), f.shares, CentsToString(f.price).c_str());
  if (FlowJournalOn() &&
      !OrderFlowJournal::AppendFill(risk_.journal_dir, id, f.shares, f.price, true)) {
    std::fprintf(stderr, "kairos-order-hub: FAILED to journal order-flow unroutable fill id=%s\n",
                 id.c_str());
  }
  if (risk_.journal_dir.empty()) return;
  if (!named) {
    std::fprintf(
        stderr,
        "kairos-order-hub: cannot journal unroutable fill id=%s (untracked or already accounted)\n",
        id.c_str());
    return;
  }
  std::string name = symbol + "-" + SideName(side) + "-" + JournalDayUtc8();
  std::string path = JournalPath(risk_.journal_dir, name);
  if (OrderJournal::AppendFill(risk_.journal_dir, name, id, journal_shares, f.price)) {
    std::fprintf(stderr, "kairos-order-hub: journaled unroutable fill id=%s %ld @ %s -> %s\n",
                 id.c_str(), journal_shares, CentsToString(f.price).c_str(), path.c_str());
  } else {
    std::fprintf(stderr, "kairos-order-hub: FAILED to journal unroutable fill id=%s to %s\n",
                 id.c_str(), path.c_str());
  }
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
        open_ids_.erase(it->first);
        RemoveDupEntry(id);  // terminal for the gate: a re-peg at the same price is not a dup
        // Meta is kept: a fill can cross the cancel at the exchange, and that
        // post-cancel fill must stay nameable for the journal if the trader exits.
        if (cs != clients_.end()) ++cs->second.cancelled;
      }
      if (cs != clients_.end()) cs->second.last_activity_us = SystemNowUs();
    }
  }
  if (client >= 0) {
    std::fprintf(stderr, "kairos-order-hub: cancel id=%s ok=%d -> client=%d\n", id.c_str(),
                 ok ? 1 : 0, client);
    send_(client, EncodeOrderCancelResult({id, ok}));
  } else {
    std::fprintf(stderr, "kairos-order-hub: UNROUTABLE cancel id=%s ok=%d (no client route)\n",
                 id.c_str(), ok ? 1 : 0);
  }
  if (FlowJournalOn() && !OrderFlowJournal::AppendCancelAck(risk_.journal_dir, id, ok)) {
    std::fprintf(stderr, "kairos-order-hub: FAILED to journal order-flow cancel_ack id=%s\n",
                 id.c_str());
  }
}

HubStatus OrderHub::CaptureStatus() const {
  HubStatus s;
  s.start_epoch_s = start_epoch_s_;
  s.written_epoch_s = SystemNowUs() / 1000000;
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
