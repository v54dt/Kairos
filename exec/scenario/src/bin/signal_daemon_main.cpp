// kairos_signald [--config PATH] [--signal-sock PATH] [--quote-sock PATH]
//   [--spool PATH] [--hb-ms N]
//   The signal daemon: server side of the signal protocol. It loads a predicate
//   config (signald.toml), subscribes to the core quote UDS for the union of the
//   predicates' symbols, and pushes signals plus a 1s heartbeat to subscribed
//   traders over the signal UDS. A manual predicate reads an operator spool file
//   for drill injection. SIGINT/SIGTERM shut it down cleanly (join threads, unlink
//   the socket).

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <thread>

#include "signal_client.h"  // SignalSocketPath
#include "signal_daemon.h"
#include "socket_path.h"

using namespace kairos::exec;

namespace {
std::atomic<bool> g_stop{false};
void OnSig(int) { g_stop = true; }

std::string DefaultConfig() {
  const char* home = std::getenv("HOME");
  std::string base = (home != nullptr && home[0] != '\0') ? home : ".";
  return base + "/Kairos/exec/scenario/signald.toml";
}

// Operator drill spool: same runtime-dir resolution as the sockets, never /tmp.
std::string SignalSpoolPath() {
  std::string p = ResolveSock(std::getenv("KAIROS_SIGNAL_SPOOL"), std::getenv("XDG_RUNTIME_DIR"),
                              RunUserDir(), "kairos-signals.spool");
  if (p.empty()) NoRuntimeDir("KAIROS_SIGNAL_SPOOL");
  return p;
}
}  // namespace

int main(int argc, char** argv) {
  std::string config;
  std::string signal_sock;
  std::string quote_sock;
  std::string spool;
  long hb_ms = 1000;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--config" && i + 1 < argc) {
      config = argv[++i];
    } else if (a == "--signal-sock" && i + 1 < argc) {
      signal_sock = argv[++i];
    } else if (a == "--quote-sock" && i + 1 < argc) {
      quote_sock = argv[++i];
    } else if (a == "--spool" && i + 1 < argc) {
      spool = argv[++i];
    } else if (a == "--hb-ms" && i + 1 < argc) {
      hb_ms = std::atol(argv[++i]);
    } else {
      std::fprintf(stderr,
                   "usage: kairos_signald [--config PATH] [--signal-sock PATH] [--quote-sock PATH] "
                   "[--spool PATH] [--hb-ms N]\n");
      return 1;
    }
  }
  if (config.empty()) config = DefaultConfig();
  if (signal_sock.empty()) signal_sock = SignalSocketPath();
  if (quote_sock.empty()) quote_sock = QuoteSocketPath();
  if (spool.empty()) spool = SignalSpoolPath();
  if (hb_ms <= 0) hb_ms = 1000;

  std::signal(SIGINT, OnSig);
  std::signal(SIGTERM, OnSig);
  std::signal(SIGPIPE, SIG_IGN);  // a dead client must not kill the daemon on write

  SignalRegistry registry;
  try {
    registry = BuildSignalRegistry(config, spool);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "kairos-signald: config error: %s\n", e.what());
    return 1;
  }

  SignalDaemon::Options opts;
  opts.signal_sock = signal_sock;
  opts.quote_sock = quote_sock;
  opts.hb_interval = std::chrono::milliseconds(hb_ms);
  SignalDaemon daemon(std::move(registry), std::move(opts));
  std::printf("kairos-signald: config %s, spool %s\n", config.c_str(), spool.c_str());
  std::fflush(stdout);
  if (!daemon.Start()) return 1;
  while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(200));
  std::printf("kairos-signald: shutting down\n");
  std::fflush(stdout);
  daemon.Stop();
  return 0;
}
