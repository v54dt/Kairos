// kairos_order_hubd <hub.toml> [--paper]
//   Shared concords order backend: one connection + one 1 req/s gate. Scenarios
//   submit/cancel over UDS (default $XDG_RUNTIME_DIR or /tmp/kairos-orders.sock)
//   and get ack/fill/cancel routed back. --paper uses a simulated backend.

#include <sys/stat.h>
#include <toml++/toml.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#include "live_backend.h"
#include "order_backend.h"
#include "order_hub_server.h"
#include "scenario.h"  // UserCreds
#include "socket_path.h"

using namespace kairos::exec;

namespace {
std::atomic<bool> g_stop{false};
void OnSig(int) { g_stop = true; }
}  // namespace

int main(int argc, char** argv) {
  std::string path;
  bool paper = false;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--paper") {
      paper = true;
    } else if (!a.empty() && a[0] != '-') {
      path = a;
    }
  }
  if (path.empty()) {
    std::fprintf(stderr, "usage: kairos_order_hubd <hub.toml> [--paper]\n");
    return 1;
  }

  // hub.toml holds broker credentials — warn if it is group/other-accessible.
  struct stat st;
  if (::stat(path.c_str(), &st) == 0 && (st.st_mode & 0077) != 0) {
    std::fprintf(stderr,
                 "kairos-order-hub: WARNING: %s is group/other-accessible (mode %03o); it holds "
                 "broker credentials — chmod 600 it\n",
                 path.c_str(), st.st_mode & 07777);
  }

  UserCreds creds;
  std::string sock = OrderSocketPath();
  OrderHub::RiskConfig risk;
  try {
    auto t = toml::parse_file(path);
    auto user = [&](const char* k) { return t["user"][k].value<std::string>().value_or(""); };
    creds.user_id = user("user_id");
    creds.password = user("password");
    creds.account = user("account");
    creds.pfx_filepath = user("pfx_filepath");
    creds.pfx_password = user("pfx_password");
    if (auto s = t["hub"]["socket_path"].value<std::string>()) sock = *s;
    // [risk]: TWD notional caps are stored as Cents; 0/absent = disabled.
    risk.max_account_notional_cents =
        t["risk"]["max_account_notional_twd"].value<long>().value_or(0) * 100;
    risk.max_open_orders_per_client =
        t["risk"]["max_open_orders_per_client"].value<int>().value_or(0);
    risk.max_open_notional_per_client_cents =
        t["risk"]["max_open_notional_per_client_twd"].value<long>().value_or(0) * 100;
    risk.self_match_protection = t["risk"]["self_match_protection"].value<bool>().value_or(true);
    // Fat-finger guards: counts/ms/percent, not TWD, so parsed without the *100.
    risk.max_order_shares = t["risk"]["max_order_shares"].value<long>().value_or(0);
    risk.halt_file_path = HubHaltPath();  // sentinel a TUI creates/unlinks to halt/resume
  } catch (const toml::parse_error& e) {
    std::fprintf(stderr, "kairos-order-hub: bad config %s: %s\n", path.c_str(),
                 std::string(e.description()).c_str());
    return 1;
  }

  std::unique_ptr<OrderBackend> backend;
  if (paper) {
    backend = std::make_unique<PaperOrderBackend>();
    std::printf("kairos-order-hub: PAPER backend\n");
  } else {
    backend = MakeLiveBackend(creds);
    if (!backend) {
      std::fprintf(stderr, "kairos-order-hub: this build has no broker SDK (use --paper)\n");
      return 1;
    }
  }

  std::signal(SIGINT, OnSig);
  std::signal(SIGTERM, OnSig);

  OrderHubServer server(backend.get(), sock, risk);
  if (!server.Start()) return 1;
  while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(200));
  std::printf("kairos-order-hub: shutting down\n");
  server.Stop();
  return 0;
}
