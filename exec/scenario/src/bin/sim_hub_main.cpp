// kairos_sim_hubd <symbol> [<symbol>...] [--prob] [--order-sock PATH] [--quote-sock PATH]
//   [--fault-seed N] [--fault-ack-delay-ms N] [--fault-ack-jitter-ms N]
//   [--fault-ack-drop-rate R] [--fault-reject-rate R] [--fault-partial N]
//   [--fault-disconnect-after-n N] [--fault-disconnect-every-ms N]
//   Mock order hub for offline/paper simulation. Speaks the SAME UDS
//   OrderEnvelope protocol as kairos_order_hubd (a scenario trader connects
//   unmodified) but fills orders from the offline fill-model library, driven by
//   the core quote+trade UDS stream. There is NO broker SDK and NO real-order
//   path: run EITHER the live hub OR this on a given order socket, never both.
//   The --fault-* knobs (all default off) inject timeouts/rejects/partials/
//   disconnects so engine failure paths can be rehearsed off-hours; they can also
//   be supplied via the KAIROS_SIM_HUBD_ARGS env var (prepended to argv), which is
//   how a drill passes them through the untouched kairos-sim pipeline.

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "fault_config.h"
#include "order_hub_server.h"
#include "sim_order_backend.h"
#include "socket_path.h"
#include "uds_quote_client.h"

using namespace kairos::exec;

namespace {
std::atomic<bool> g_stop{false};
void OnSig(int) { g_stop = true; }

// Whitespace-split the KAIROS_SIM_HUBD_ARGS env var into tokens prepended to argv.
std::vector<std::string> EnvArgs() {
  std::vector<std::string> out;
  const char* env = std::getenv("KAIROS_SIM_HUBD_ARGS");
  if (env == nullptr) return out;
  std::istringstream iss(env);
  std::string tok;
  while (iss >> tok) out.push_back(tok);
  return out;
}

// True if a process is already accepting on the order socket. The hub unlinks and
// rebinds any existing path, so without this check a sim hub started on the live
// hub's default socket would silently hijack live order flow.
bool OrderSocketHasListener(const std::string& path) {
  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return false;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  bool listening = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
  ::close(fd);
  return listening;
}
}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> symbols;
  std::string order_sock;
  std::string quote_sock;
  FillMode mode = FillMode::kConservative;
  FaultConfig faults;

  // Env args first so an explicit CLI flag on the same knob wins (last write).
  std::vector<std::string> args = EnvArgs();
  for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);

  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    bool has_next = i + 1 < args.size();
    if (a == "--prob") {
      mode = FillMode::kProbQueue;
    } else if (a == "--order-sock" && has_next) {
      order_sock = args[++i];
    } else if (a == "--quote-sock" && has_next) {
      quote_sock = args[++i];
    } else if (a == "--fault-seed" && has_next) {
      faults.seed = std::strtoull(args[++i].c_str(), nullptr, 10);
    } else if (a == "--fault-ack-delay-ms" && has_next) {
      faults.ack_delay_ms = std::atol(args[++i].c_str());
    } else if (a == "--fault-ack-jitter-ms" && has_next) {
      faults.ack_jitter_ms = std::atol(args[++i].c_str());
    } else if (a == "--fault-ack-drop-rate" && has_next) {
      faults.ack_drop_rate = std::atof(args[++i].c_str());
    } else if (a == "--fault-reject-rate" && has_next) {
      faults.reject_rate = std::atof(args[++i].c_str());
    } else if (a == "--fault-partial" && has_next) {
      faults.partial_fill = std::atol(args[++i].c_str());
    } else if (a == "--fault-disconnect-after-n" && has_next) {
      faults.disconnect_after_n = std::atol(args[++i].c_str());
    } else if (a == "--fault-disconnect-every-ms" && has_next) {
      faults.disconnect_every_ms = std::atol(args[++i].c_str());
    } else if (!a.empty() && a[0] != '-') {
      symbols.push_back(a);
    }
  }
  if (symbols.empty()) {
    std::fprintf(stderr,
                 "usage: kairos_sim_hubd <symbol> [<symbol>...] [--prob] "
                 "[--order-sock PATH] [--quote-sock PATH] [--fault-seed N] "
                 "[--fault-ack-delay-ms N] [--fault-ack-jitter-ms N] [--fault-ack-drop-rate R] "
                 "[--fault-reject-rate R] [--fault-partial N] [--fault-disconnect-after-n N] "
                 "[--fault-disconnect-every-ms N]\n");
    return 1;
  }
  if (order_sock.empty()) order_sock = OrderSocketPath();
  if (quote_sock.empty()) quote_sock = QuoteSocketPath();

  if (OrderSocketHasListener(order_sock)) {
    std::fprintf(stderr,
                 "kairos-sim-hub: refusing to start: a hub is already listening on %s "
                 "(the live hub?). Stop it or pass --order-sock for a separate paper socket.\n",
                 order_sock.c_str());
    return 1;
  }

  SimOrderBackend backend(mode, symbols, faults);
  UdsQuoteClient quotes(quote_sock, symbols);
  quotes.SetCallback(
      [&backend](const std::string& s, const TopOfBook& t) { backend.OnBook(s, t); });
  quotes.SetTradeCallback(
      [&backend](const std::string& s, const Trade& t) { backend.OnTrade(s, t); });

  std::signal(SIGINT, OnSig);
  std::signal(SIGTERM, OnSig);

  OrderHubServer server(&backend, order_sock);
  if (!server.Start()) return 1;
  quotes.Start();
  std::printf("kairos-sim-hub: %s fill model, %zu symbol(s), quotes %s\n",
              mode == FillMode::kProbQueue ? "ProbQueue" : "conservative", symbols.size(),
              quote_sock.c_str());
  if (faults.Enabled())
    std::printf(
        "kairos-sim-hub: FAULTS ON seed=%llu ack_delay=%ldms jitter=%ldms drop=%.2f reject=%.2f "
        "partial=%ld disc_after=%ld disc_every=%ldms\n",
        static_cast<unsigned long long>(faults.seed), faults.ack_delay_ms, faults.ack_jitter_ms,
        faults.ack_drop_rate, faults.reject_rate, faults.partial_fill, faults.disconnect_after_n,
        faults.disconnect_every_ms);
  std::fflush(stdout);

  auto last_disc = std::chrono::steady_clock::now();
  while (!g_stop) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    bool disconnect = backend.FaultDisconnectAfterN();
    long every = backend.FaultDisconnectEveryMs();
    if (every > 0) {
      auto now = std::chrono::steady_clock::now();
      if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_disc).count() >= every) {
        last_disc = now;
        disconnect = true;
      }
    }
    if (disconnect) {
      std::printf("kairos-sim-hub: fault disconnect clients\n");
      std::fflush(stdout);
      server.DisconnectAllClients();
    }
  }
  std::printf("kairos-sim-hub: shutting down\n");
  quotes.Stop();
  backend.Finalize();  // resolve any pending closing-auction orders (end of tape)
  server.Stop();
  return 0;
}
