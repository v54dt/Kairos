// kairos_quote_tap <symbol>... — subscribe the core quote UDS and print quotes.
// The C++ consumer's live diagnostic (mirror of the Rust kairos-uds-client).

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "tw_market.h"
#include "uds_quote_client.h"

using namespace kairos::exec;

namespace {
std::atomic<bool> g_stop{false};
void OnSig(int) { g_stop = true; }
}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> symbols;
  for (int i = 1; i < argc; ++i) symbols.emplace_back(argv[i]);
  if (symbols.empty()) {
    std::fprintf(stderr, "usage: kairos_quote_tap <symbol>...\n");
    return 1;
  }

  std::string path = QuoteSocketPath();
  UdsQuoteClient client(path, symbols, [](const std::string& sym, const TopOfBook& t) {
    std::printf("%-8s bid=%s ask=%s last=%s%s\n", sym.c_str(), CentsToString(t.best_bid).c_str(),
                CentsToString(t.best_ask).c_str(), CentsToString(t.last_trade).c_str(),
                t.is_trial ? " (試撮)" : "");
    std::fflush(stdout);
  });

  std::signal(SIGINT, OnSig);
  std::signal(SIGTERM, OnSig);
  client.Start();
  std::printf("kairos_quote_tap: tapping %zu symbol(s) on %s\n", symbols.size(), path.c_str());
  std::fflush(stdout);

  while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  client.Stop();
  return 0;
}
