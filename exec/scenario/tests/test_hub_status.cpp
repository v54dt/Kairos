// Self-test for the read-only hub status snapshot: JSON serialization, the
// atomic file write, and CaptureStatus counting over OrderHub routing. No SDK,
// no sockets: a stub backend fires the ack/fill/cancel callbacks by hand.

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "hub_status.h"
#include "order_codec.h"
#include "order_hub.h"

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

class StubBackend : public OrderBackend {
 public:
  bool Connect() override {
    connected = true;
    return true;
  }
  void Disconnect() override { connected = false; }
  bool IsConnected() const override { return connected; }
  void Submit(const OrderSubmitMsg&) override {}
  void Cancel(const std::string&) override {}
  void FireAck(const std::string& id, bool ok) {
    if (on_ack_) on_ack_(id, ok, "");
  }
  void FireFill(const std::string& id, const Fill& f) {
    if (on_fill_) on_fill_(id, f);
  }
  void FireCancel(const std::string& id, bool ok) {
    if (on_cancel_) on_cancel_(id, ok);
  }
  bool connected = false;
};

void Submit(OrderHub& hub, int client, const std::string& id, long shares) {
  OrderSubmitMsg s{id,     "2330", Market::kTse, Board::kOddLot, Side::kBuy,
                   "Cash", "ROD",  92500,        shares};
  auto bytes = EncodeOrderSubmit(s);
  hub.OnClientMessage(client, bytes.data(), bytes.size());
}

const ClientStatus* Find(const HubStatus& s, const std::string& prefix) {
  for (const auto& c : s.clients)
    if (c.prefix == prefix) return &c;
  return nullptr;
}

}  // namespace

int main() {
  // (a) serialize a synthetic two-client snapshot
  HubStatus st;
  st.start_epoch_s = 1000;
  st.written_epoch_s = 1042;
  st.client_count = 2;
  st.clients.push_back(ClientStatus{"k100", 100, 2, 5, 3, 1, 1040});
  st.clients.push_back(ClientStatus{"k200", 200, 0, 1, 1, 0, 1041});
  const std::string json = SerializeHubStatus(st);
  CHECK(json.find("\"start_epoch_s\":1000") != std::string::npos);
  CHECK(json.find("\"written_epoch_s\":1042") != std::string::npos);
  CHECK(json.find("\"client_count\":2") != std::string::npos);
  CHECK(json.find("\"prefix\":\"k100\"") != std::string::npos);
  CHECK(json.find("\"pid\":100") != std::string::npos);
  CHECK(json.find("\"open\":2") != std::string::npos);
  CHECK(json.find("\"submitted\":5") != std::string::npos);
  CHECK(json.find("\"filled\":3") != std::string::npos);
  CHECK(json.find("\"cancelled\":1") != std::string::npos);
  CHECK(json.find("\"last_activity_s\":1040") != std::string::npos);
  CHECK(json.back() == '\n');

  // empty registry -> client_count 0, clients:[]
  HubStatus empty;
  const std::string ejson = SerializeHubStatus(empty);
  CHECK(ejson.find("\"client_count\":0") != std::string::npos);
  CHECK(ejson.find("\"clients\":[]") != std::string::npos);

  // A prefix carrying control bytes is fully escaped: the status file stays one
  // JSON line (no raw newline/tab) so the TUI hub_status parser can read it.
  HubStatus hostile;
  hostile.client_count = 1;
  hostile.clients.push_back(ClientStatus{"k\n\t\"\\\x01 台積電", 7, 0, 0, 0, 0, 0});
  const std::string hjson = SerializeHubStatus(hostile);
  CHECK(hjson.find("\\n") != std::string::npos);
  CHECK(hjson.find("\\t") != std::string::npos);
  CHECK(hjson.find("\\u0001") != std::string::npos);
  CHECK(hjson.find("台積電") != std::string::npos);
  CHECK(hjson.find('\n') == hjson.size() - 1);  // only the trailing terminator
  CHECK(hjson.find('\t') == std::string::npos);

  // (b) atomic write: content lands, temp file is gone (rename consumed it)
  const std::string dir = "/tmp/kairos-hub-status-test-" + std::to_string(::getpid());
  ::mkdir(dir.c_str(), 0755);
  const std::string path = dir + "/kairos-hub-status.json";
  CHECK(AtomicWriteFile(path, json));
  {
    std::ifstream in(path);
    std::string got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(got == json);
  }
  const std::string tmp = path + ".tmp." + std::to_string(::getpid());
  CHECK(::access(tmp.c_str(), F_OK) != 0);
  ::unlink(path.c_str());
  ::rmdir(dir.c_str());

  // (c) CaptureStatus counts open/filled/cancelled from the hub's own routing
  StubBackend backend;
  OrderHub hub(&backend, [](int, const std::vector<std::uint8_t>&) {});
  CHECK(hub.Start());

  Submit(hub, 7, "k100-1", 1000);
  Submit(hub, 7, "k100-2", 500);
  backend.FireAck("k100-1", true);
  backend.FireAck("k100-2", true);
  backend.FireFill("k100-1", Fill{300, 92500});  // partial: k100-1 still open

  HubStatus s1 = hub.CaptureStatus();
  CHECK(s1.client_count == 1);
  const ClientStatus* c1 = Find(s1, "k100");
  CHECK(c1 != nullptr);
  CHECK(c1->pid == 100);
  CHECK(c1->submitted == 2);
  CHECK(c1->open == 2);  // both acked, neither fully filled/cancelled
  CHECK(c1->filled == 1);
  CHECK(s1.start_epoch_s > 0);
  CHECK(s1.written_epoch_s >= s1.start_epoch_s);

  backend.FireFill("k100-1", Fill{700, 92500});  // now fully filled -> closed
  backend.FireCancel("k100-2", true);            // cancelled -> closed

  HubStatus s2 = hub.CaptureStatus();
  const ClientStatus* c2 = Find(s2, "k100");
  CHECK(c2 != nullptr);
  CHECK(c2->open == 0);
  CHECK(c2->filled == 2);
  CHECK(c2->cancelled == 1);

  // a second client, then the first disconnects -> only the second remains
  Submit(hub, 9, "k200-1", 300);
  hub.OnClientDisconnect(7);
  HubStatus s3 = hub.CaptureStatus();
  CHECK(s3.client_count == 1);
  CHECK(Find(s3, "k100") == nullptr);
  CHECK(Find(s3, "k200") != nullptr);

  // (d) a connected-but-idle client (no submit) is counted from connect time
  hub.OnClientConnect(13);
  HubStatus s4 = hub.CaptureStatus();
  CHECK(s4.client_count == 2);  // k200 + the idle client 13
  const ClientStatus* idle = Find(s4, "");
  CHECK(idle != nullptr);
  CHECK(idle->submitted == 0);
  CHECK(idle->open == 0);
  CHECK(idle->last_activity_s > 0);  // stamped at connect, not epoch 0

  hub.Stop();

  if (g_failures == 0) {
    std::printf("test_hub_status: OK\n");
    return 0;
  }
  std::printf("test_hub_status: FAILED %d check(s)\n", g_failures);
  return 1;
}
