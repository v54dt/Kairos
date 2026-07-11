// End-to-end: a client submits through the real UDS hub server, disconnects,
// then the backend fires a fill on that (now orphaned) order. The hub must
// journal the fill into the per-(symbol,side,day) file the trader replays, so a
// restart accounts it. Prints the journal file and the replayed total.

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "engine_logic.h"  // Accounting
#include "order_backend.h"
#include "order_codec.h"
#include "order_hub_server.h"
#include "order_journal.h"
#include "scenario.h"
#include "uds_frame.h"

using namespace kairos::exec;

static int g_failures = 0;

#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                \
    }                                                              \
  } while (0)

namespace {

// A broker whose fills the test fires by hand, so a fill can arrive AFTER the
// client has disconnected — the window the hub journaling closes.
class DeferredBackend : public OrderBackend {
 public:
  bool Connect() override {
    connected_ = true;
    return true;
  }
  void Disconnect() override { connected_ = false; }
  bool IsConnected() const override { return connected_; }
  void Submit(const OrderSubmitMsg& o) override {
    if (on_ack_) on_ack_(o.id, true, "");  // ack immediately; fill deferred
  }
  void Cancel(const std::string&) override {}
  void FireFill(const std::string& id, const Fill& f) {
    if (on_fill_) on_fill_(id, f);
  }

 private:
  bool connected_ = false;
};

int ConnectClient(const std::string& path) {
  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

}  // namespace

int main() {
  const std::string sock = "/tmp/kairos-e2e-hub-" + std::to_string(::getpid()) + ".sock";
  const std::string dir = "/tmp/kairos-e2e-journal-" + std::to_string(::getpid());
  const std::string name = std::string("2330-Buy-") + JournalDayUtc8();
  const std::string path = JournalPath(dir, name);
  std::remove(path.c_str());

  DeferredBackend backend;
  OrderHub::RiskConfig risk;
  risk.journal_dir = dir;
  OrderHubServer server(&backend, sock, risk);
  CHECK(server.Start());

  int fd = ConnectClient(sock);
  CHECK(fd >= 0);

  OrderSubmitMsg m{"k777-1", "2330", Market::kTse, Board::kRoundLot, Side::kBuy, "Cash",
                   "ROD",    92500,  1000};
  CHECK(WriteFrame(fd, EncodeOrderSubmit(m)));
  std::printf("demo: client submitted %s %s %ld @ %s\n", m.id.c_str(), m.symbol.c_str(), m.shares,
              "925.00");

  // Read the ack so we know the hub registered the route, then disconnect.
  std::vector<std::uint8_t> frame;
  CHECK(ReadFrame(fd, &frame) == 1);
  ::close(fd);
  std::printf("demo: client disconnected (order still open at the broker)\n");

  // Let the server's client loop observe the disconnect and drop the route.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  // The broker now reports the fill on an id no client owns anymore.
  backend.FireFill("k777-1", Fill{1000, 92500});
  std::printf("demo: backend fired a fill on the departed client's id\n");
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // The hub must have written the fill into the trader's own journal file.
  std::printf("demo: journal %s contents:\n", path.c_str());
  {
    std::FILE* f = std::fopen(path.c_str(), "r");
    CHECK(f != nullptr);
    if (f) {
      char line[256];
      while (std::fgets(line, sizeof(line), f)) std::printf("  %s", line);
      std::fclose(f);
    }
  }

  // A restarted trader replays that journal into a fresh Accounting.
  auto fills = ReadJournalFills(path);
  CHECK(fills.size() == 1);
  Scenario s;
  s.symbol = "2330";
  s.side = Side::kBuy;
  s.budget_twd = 10'000'000;
  Accounting replayed;
  for (const auto& fl : fills) replayed.RecordFill(s, fl.price, fl.shares);
  CHECK(replayed.filled_shares == 1000);
  std::printf("demo: restart replay accounted %ld sh (NT$ %ld); remaining budget NT$ %ld\n",
              replayed.filled_shares, replayed.FilledTwd(), replayed.RemainingTwd(s));

  server.Stop();
  std::remove(path.c_str());
  ::rmdir(dir.c_str());
  ::unlink(sock.c_str());

  // Real proof of the env unification: with ONLY the shared KAIROS_JOURNAL_DIR set,
  // the engine and the hub resolve the SAME directory, and an engine fill write plus
  // a hub unroutable fill land in ONE file named for the same trading day.
  {
    const std::string shared = "/tmp/kairos-shared-e2e-" + std::to_string(::getpid());
    ::setenv("KAIROS_JOURNAL_DIR", shared.c_str(), 1);
    ::unsetenv("KAIROS_HUB_JOURNAL_DIR");

    const std::string engine_dir = ResolveJournalDir("", nullptr);  // trader form
    bool used_legacy = false;
    const std::string hub_dir =
        ResolveJournalDir("", "KAIROS_HUB_JOURNAL_DIR", &used_legacy);  // hub form
    CHECK(engine_dir == shared);
    CHECK(hub_dir == shared);
    CHECK(engine_dir == hub_dir);  // both sides resolve one directory
    CHECK(!used_legacy);

    const std::string sname = std::string("2454-Buy-") + JournalDayUtc8();
    const std::string spath = JournalPath(shared, sname);
    std::remove(spath.c_str());

    // Engine side: a live trader appends a fill to <dir>/<symbol-side-day>.jsonl.
    {
      OrderJournal j;
      CHECK(j.Open(engine_dir, sname));
      j.LogFill("k-engine-1", 500, 50000);
    }

    // Hub side: an unroutable fill on the SAME (symbol,side,day) appends to the SAME
    // file the trader replays, resolved purely from the shared env var.
    DeferredBackend hb;
    OrderHub::RiskConfig hrisk;
    hrisk.journal_dir = hub_dir;
    const std::string hsock = "/tmp/kairos-e2e-hub2-" + std::to_string(::getpid()) + ".sock";
    OrderHubServer hserver(&hb, hsock, hrisk);
    CHECK(hserver.Start());
    int hfd = ConnectClient(hsock);
    CHECK(hfd >= 0);
    OrderSubmitMsg hm{"k888-1", "2454", Market::kTse, Board::kRoundLot, Side::kBuy, "Cash",
                      "ROD",    50000,  500};
    CHECK(WriteFrame(hfd, EncodeOrderSubmit(hm)));
    std::vector<std::uint8_t> hframe;
    CHECK(ReadFrame(hfd, &hframe) == 1);  // ack read: route registered
    ::close(hfd);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    hb.FireFill("k888-1", Fill{500, 50000});  // now unroutable
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto both = ReadJournalFills(spath);
    CHECK(both.size() == 2);  // engine fill + hub unroutable fill in one day-named file
    CHECK(sname.find(JournalDayUtc8()) != std::string::npos);
    std::printf("shared-dir proof: engine_dir=%s hub_dir=%s file=%s holds %zu fills\n",
                engine_dir.c_str(), hub_dir.c_str(), spath.c_str(), both.size());

    hserver.Stop();
    std::remove(spath.c_str());
    ::rmdir(shared.c_str());
    ::unlink(hsock.c_str());
    ::unsetenv("KAIROS_JOURNAL_DIR");
  }

  if (g_failures == 0) {
    std::printf("test_hub_unroutable_e2e: OK\n");
    return 0;
  }
  std::printf("test_hub_unroutable_e2e: FAILED %d check(s)\n", g_failures);
  return 1;
}
