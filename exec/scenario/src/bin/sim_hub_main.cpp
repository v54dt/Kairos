// kairos_sim_hubd <symbol> [<symbol>...] [--prob] [--order-sock PATH] [--quote-sock PATH]
//   Mock order hub for offline/paper simulation. Speaks the SAME UDS
//   OrderEnvelope protocol as kairos_order_hubd (a scenario trader connects
//   unmodified) but fills orders from the offline fill-model library, driven by
//   the core quote+trade UDS stream. There is NO broker SDK and NO real-order
//   path: run EITHER the live hub OR this on a given order socket, never both.

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "order_hub_server.h"
#include "sim_order_backend.h"
#include "socket_path.h"
#include "uds_quote_client.h"

using namespace kairos::exec;

namespace {
std::atomic<bool> g_stop{false};
void OnSig(int) { g_stop = true; }

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

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--prob") {
      mode = FillMode::kProbQueue;
    } else if (a == "--order-sock" && i + 1 < argc) {
      order_sock = argv[++i];
    } else if (a == "--quote-sock" && i + 1 < argc) {
      quote_sock = argv[++i];
    } else if (!a.empty() && a[0] != '-') {
      symbols.push_back(a);
    }
  }
  if (symbols.empty()) {
    std::fprintf(stderr,
                 "usage: kairos_sim_hubd <symbol> [<symbol>...] [--prob] "
                 "[--order-sock PATH] [--quote-sock PATH]\n");
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

  SimOrderBackend backend(mode, symbols);
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
  std::fflush(stdout);

  while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(200));
  std::printf("kairos-sim-hub: shutting down\n");
  quotes.Stop();
  backend.Finalize();  // resolve any pending closing-auction orders (end of tape)
  server.Stop();
  return 0;
}
