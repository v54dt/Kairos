#include "signal_daemon.h"

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <toml++/toml.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

#include "predicate_depth_evap.h"
#include "predicate_manual.h"
#include "uds_quote_client.h"

namespace kairos::exec {

namespace {

void WriteAll(int fd, const std::string& s) {
  std::size_t off = 0;
  while (off < s.size()) {
    ssize_t w = ::write(fd, s.data() + off, s.size() - off);
    if (w <= 0) return;  // closed or timed out: drop, ClientLoop detects EOF
    off += static_cast<std::size_t>(w);
  }
}

void InterruptibleSleep(std::chrono::milliseconds d, const std::atomic<bool>& stop) {
  auto until = std::chrono::steady_clock::now() + d;
  while (!stop && std::chrono::steady_clock::now() < until) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

std::string ReqStr(const toml::table& t, const char* key) {
  auto v = t[key].value<std::string>();
  if (!v) throw std::runtime_error(std::string("signald config missing required key: ") + key);
  return *v;
}

std::vector<std::string> ReqSymbols(const toml::table& t) {
  const auto* arr = t["symbols"].as_array();
  if (arr == nullptr) throw std::runtime_error("signald config: predicate needs a symbols array");
  std::vector<std::string> out;
  for (const auto& node : *arr) {
    auto s = node.value<std::string>();
    if (!s) throw std::runtime_error("signald config: symbols must be strings");
    out.push_back(*s);
  }
  if (out.empty()) throw std::runtime_error("signald config: symbols array is empty");
  return out;
}

DepthSide ParseDepthSide(const std::string& s) {
  if (s == "bid") return DepthSide::kBid;
  if (s == "ask") return DepthSide::kAsk;
  throw std::runtime_error("signald config: invalid depth_evap side (bid|ask): " + s);
}

}  // namespace

void SignalRegistry::Add(std::unique_ptr<Predicate> predicate) {
  predicates_.push_back(std::move(predicate));
}

std::vector<std::string> SignalRegistry::QuoteSymbols() const {
  std::set<std::string> seen;
  std::vector<std::string> out;
  for (const auto& p : predicates_) {
    if (!p->NeedsQuotes()) continue;
    for (const std::string& s : p->Symbols())
      if (seen.insert(s).second) out.push_back(s);
  }
  return out;
}

bool SignalRegistry::Validate(const std::string& signal, const std::string& symbol) const {
  for (const auto& p : predicates_)
    if (p->Name() == signal) return p->WatchesSymbol(symbol);
  return false;
}

std::vector<SignalEmit> SignalRegistry::OnQuote(const std::string& symbol, const TopOfBook& tob,
                                                std::int64_t ts_us) {
  std::vector<SignalEmit> out;
  for (const auto& p : predicates_) {
    if (!p->WatchesSymbol(symbol)) continue;
    if (auto fire = p->OnQuote(symbol, tob, ts_us)) out.push_back({p->Name(), symbol, *fire});
  }
  return out;
}

std::vector<SignalEmit> SignalRegistry::OnTrade(const std::string& symbol, const Trade& trade,
                                                std::int64_t ts_us) {
  std::vector<SignalEmit> out;
  for (const auto& p : predicates_) {
    if (!p->WatchesSymbol(symbol)) continue;
    if (auto fire = p->OnTrade(symbol, trade, ts_us)) out.push_back({p->Name(), symbol, *fire});
  }
  return out;
}

std::vector<SignalEmit> SignalRegistry::Poll(std::int64_t ts_us) {
  std::vector<SignalEmit> out;
  for (const auto& p : predicates_)
    for (auto& hit : p->Poll(ts_us)) out.push_back({p->Name(), hit.symbol, std::move(hit.fire)});
  return out;
}

SignalRegistry BuildSignalRegistry(const std::string& toml_path, const std::string& spool_path) {
  toml::table root = toml::parse_file(toml_path);
  const auto* arr = root["predicate"].as_array();
  if (arr == nullptr || arr->empty())
    throw std::runtime_error("signald config: no [[predicate]] entries");

  SignalRegistry reg;
  std::set<std::string> names;
  for (const auto& node : *arr) {
    const auto* t = node.as_table();
    if (t == nullptr) throw std::runtime_error("signald config: [[predicate]] must be a table");
    std::string kind = ReqStr(*t, "kind");
    std::string name = ReqStr(*t, "name");
    if (!names.insert(name).second)
      throw std::runtime_error("signald config: duplicate predicate name: " + name);
    std::vector<std::string> symbols = ReqSymbols(*t);

    if (kind == "depth_evap") {
      DepthEvapParams p;
      p.side = ParseDepthSide(ReqStr(*t, "side"));
      p.window_s = (*t)["window_s"].value<double>().value_or(p.window_s);
      p.ratio_enter = (*t)["ratio"].value<double>().value_or(p.ratio_enter);
      p.ratio_enter = (*t)["ratio_enter"].value<double>().value_or(p.ratio_enter);
      p.ratio_exit = (*t)["ratio_exit"].value<double>().value_or(p.ratio_exit);
      p.warmup_s = (*t)["warmup_s"].value<double>().value_or(p.warmup_s);
      p.cooldown_us =
          static_cast<std::int64_t>((*t)["cooldown_ms"].value<double>().value_or(0.0) * 1000.0);
      reg.Add(std::make_unique<DepthEvapPredicate>(name, symbols, p));
    } else if (kind == "manual") {
      reg.Add(std::make_unique<ManualPredicate>(name, symbols, spool_path));
    } else {
      throw std::runtime_error("signald config: unknown predicate kind: " + kind);
    }
  }
  return reg;
}

SignalDaemon::SignalDaemon(SignalRegistry registry, Options options, SignalDaemonClock clock,
                           std::unique_ptr<QuoteSource> quote_source)
    : registry_(std::move(registry)),
      opts_(std::move(options)),
      clock_(std::move(clock)),
      quote_source_(std::move(quote_source)) {}

SignalDaemon::~SignalDaemon() { Stop(); }

bool SignalDaemon::Start() {
  listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (listen_fd_ < 0) return false;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, opts_.signal_sock.c_str(), sizeof(addr.sun_path) - 1);
  ::unlink(opts_.signal_sock.c_str());  // clear a stale socket
  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
      ::listen(listen_fd_, 16) != 0) {
    std::fprintf(stderr, "kairos-signald: bind/listen %s failed\n", opts_.signal_sock.c_str());
    ::close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }

  std::vector<std::string> symbols = registry_.QuoteSymbols();
  if (!symbols.empty()) {
    if (quote_source_ == nullptr)
      quote_source_ = std::make_unique<UdsQuoteClient>(opts_.quote_sock, symbols);
    quote_source_->SetCallback([this](const std::string& symbol, const TopOfBook& tob) {
      std::vector<SignalEmit> emits;
      {
        std::lock_guard<std::mutex> lock(eval_mu_);
        emits = registry_.OnQuote(symbol, tob, clock_.MonoUs());
      }
      Dispatch(emits);
    });
    quote_source_->SetTradeCallback([this](const std::string& symbol, const Trade& trade) {
      std::vector<SignalEmit> emits;
      {
        std::lock_guard<std::mutex> lock(eval_mu_);
        emits = registry_.OnTrade(symbol, trade, clock_.MonoUs());
      }
      Dispatch(emits);
    });
    quote_source_->Start();
  }

  accept_thread_ = std::thread([this] { AcceptLoop(); });
  hb_thread_ = std::thread([this] { HeartbeatLoop(); });
  poll_thread_ = std::thread([this] { PollLoop(); });
  std::printf("kairos-signald: listening on %s\n", opts_.signal_sock.c_str());
  std::fflush(stdout);
  return true;
}

void SignalDaemon::Stop() {
  if (stop_.exchange(true)) return;  // idempotent
  if (listen_fd_ >= 0) {
    ::shutdown(listen_fd_, SHUT_RDWR);
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
  if (accept_thread_.joinable()) accept_thread_.join();

  std::vector<int> fds;
  {
    std::lock_guard<std::mutex> lock(clients_mu_);
    for (const auto& [fd, conn] : conns_) fds.push_back(fd);
  }
  for (int fd : fds) ::shutdown(fd, SHUT_RDWR);  // unblock pending reads
  while (active_clients_.load() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));

  if (quote_source_ != nullptr) quote_source_->Stop();
  if (hb_thread_.joinable()) hb_thread_.join();
  if (poll_thread_.joinable()) poll_thread_.join();
  ::unlink(opts_.signal_sock.c_str());
}

std::size_t SignalDaemon::SubscriberCount(const std::string& signal, const std::string& symbol) {
  std::lock_guard<std::mutex> lock(clients_mu_);
  auto it = subscribers_.find({signal, symbol});
  return it == subscribers_.end() ? 0 : it->second.size();
}

void SignalDaemon::AcceptLoop() {
  while (!stop_) {
    int fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) {
      if (stop_) break;
      continue;
    }
    timeval tv{5, 0};  // bound writes: a stuck reader can't wedge the daemon
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    auto conn = std::make_shared<Conn>();
    conn->fd = fd;
    {
      std::lock_guard<std::mutex> lock(clients_mu_);
      conns_[fd] = conn;
    }
    ++active_clients_;
    std::thread([this, conn] {
      ClientLoop(conn);
      --active_clients_;
    }).detach();
  }
}

void SignalDaemon::ClientLoop(std::shared_ptr<Conn> conn) {
  int fd = conn->fd;
  std::string buf;
  bool dropping = false;  // discarding an over-length line until its newline
  char chunk[4096];
  while (!stop_) {
    ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
    if (n <= 0) break;  // EOF or error
    for (ssize_t i = 0; i < n; ++i) {
      char ch = chunk[i];
      if (ch == '\n') {
        if (dropping) {
          SignalAck ack;
          ack.err = "line too long";
          SendAck(*conn, ack);
          dropping = false;
        } else {
          HandleSubscribe(conn, buf);
        }
        buf.clear();
        continue;
      }
      if (dropping) continue;
      buf.push_back(ch);
      if (buf.size() > kMaxSignalLineLen) {  // never buffer a runaway line unbounded
        buf.clear();
        dropping = true;
      }
    }
  }
  {
    std::lock_guard<std::mutex> lock(clients_mu_);
    conns_.erase(fd);
    for (const auto& key : conn->subs) {
      auto it = subscribers_.find(key);
      if (it != subscribers_.end()) {
        it->second.erase(conn);
        if (it->second.empty()) subscribers_.erase(it);
      }
    }
  }
  ::close(fd);
}

void SignalDaemon::HandleSubscribe(const std::shared_ptr<Conn>& conn, const std::string& line) {
  SignalSubscribe sub;
  std::string err;
  if (!ParseSubscribe(line, &sub, &err)) {
    SignalAck ack;
    ack.err = err;
    SendAck(*conn, ack);
    return;
  }
  if (!registry_.Validate(sub.signal, sub.symbol)) {
    SignalAck ack;
    ack.err = "unknown signal or symbol";
    SendAck(*conn, ack);
    return;
  }
  {
    std::lock_guard<std::mutex> lock(clients_mu_);
    std::pair<std::string, std::string> key{sub.signal, sub.symbol};
    conn->subs.insert(key);
    subscribers_[key].insert(conn);
  }
  SignalAck ack;
  ack.ok = true;
  SendAck(*conn, ack);
}

std::vector<std::shared_ptr<SignalDaemon::Conn>> SignalDaemon::ConnSnapshot() {
  std::vector<std::shared_ptr<Conn>> out;
  std::lock_guard<std::mutex> lock(clients_mu_);
  for (const auto& [fd, conn] : conns_) out.push_back(conn);
  return out;
}

void SignalDaemon::Dispatch(const std::vector<SignalEmit>& emits) {
  for (const SignalEmit& emit : emits) {
    std::vector<std::shared_ptr<Conn>> targets;
    {
      std::lock_guard<std::mutex> lock(clients_mu_);
      auto it = subscribers_.find({emit.signal, emit.symbol});
      if (it != subscribers_.end()) targets.assign(it->second.begin(), it->second.end());
    }
    for (const auto& conn : targets) SendSignal(*conn, emit);
  }
}

void SignalDaemon::SendHeartbeat(Conn& conn) {
  std::lock_guard<std::mutex> lock(conn.write_mu);
  SignalHeartbeat hb;
  hb.seq = ++conn.seq;
  hb.ts_us = clock_.wall_us();
  WriteAll(conn.fd, SerializeHeartbeat(hb));
}

void SignalDaemon::SendSignal(Conn& conn, const SignalEmit& emit) {
  std::lock_guard<std::mutex> lock(conn.write_mu);
  SignalPush push;
  push.signal = emit.signal;
  push.symbol = emit.symbol;
  push.action = emit.fire.action;
  push.seq = ++conn.seq;
  push.ts_us = clock_.wall_us();
  push.fields = emit.fire.fields;
  WriteAll(conn.fd, SerializeSignal(push));
}

void SignalDaemon::SendAck(Conn& conn, const SignalAck& ack) {
  std::lock_guard<std::mutex> lock(conn.write_mu);
  WriteAll(conn.fd, SerializeAck(ack));  // out-of-band: no seq
}

void SignalDaemon::HeartbeatLoop() {
  auto last = clock_.mono();
  while (!stop_) {
    InterruptibleSleep(std::chrono::milliseconds(10), stop_);
    if (stop_) break;
    if (clock_.mono() - last < opts_.hb_interval) continue;
    last = clock_.mono();
    for (const auto& conn : ConnSnapshot()) SendHeartbeat(*conn);
  }
}

void SignalDaemon::PollLoop() {
  while (!stop_) {
    InterruptibleSleep(opts_.poll_interval, stop_);
    if (stop_) break;
    std::vector<SignalEmit> emits;
    {
      std::lock_guard<std::mutex> lock(eval_mu_);
      emits = registry_.Poll(clock_.MonoUs());
    }
    Dispatch(emits);
  }
}

}  // namespace kairos::exec
