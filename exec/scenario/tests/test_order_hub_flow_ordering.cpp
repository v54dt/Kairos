// Regression: the order-flow journal write must NOT sit on the event-DELIVERY
// path. We probe, from inside send_, whether the audit line for the event is
// already on disk at the moment the hub hands the event to the client. Every
// backend callback delivers to the client first, then journals, so a slow or
// stalled disk cannot delay the ack/fill/cancel (nor, on the shared SDK callback
// thread, the events queued behind it).
//
//   OnAck : hub send_()s the ack,  THEN journals -> at send time NO ack line yet.
//   OnFill: hub send_()s the fill, THEN journals -> at send time NO fill line yet.

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "order_codec.h"
#include "order_hub.h"
#include "order_journal.h"
#include "test_check.h"

using namespace kairos::exec;

namespace {
class Stub : public OrderBackend {
 public:
  bool Connect() override { return true; }
  void Disconnect() override {}
  bool IsConnected() const override { return true; }
  void Submit(const OrderSubmitMsg&) override {}
  void Cancel(const std::string&) override {}
  void FireAck(const std::string& id, bool ok) {
    if (on_ack_) on_ack_(id, ok, "");
  }
  void FireFill(const std::string& id, const Fill& f) {
    if (on_fill_) on_fill_(id, f);
  }
};

// Count "type":"<t>" lines currently on disk for the audit file.
int LinesOfType(const std::string& path, const std::string& t) {
  std::ifstream in(path);
  std::string line;
  int n = 0;
  while (std::getline(in, line))
    if (line.find("\"type\":\"" + t + "\"") != std::string::npos) ++n;
  return n;
}
}  // namespace

int main() {
  const std::string dir = "/tmp/kairos-flow-order-" + std::to_string(::getpid());
  const std::string fpath = JournalPath(dir, "hub-orders-" + JournalDayUtc8());
  std::remove(fpath.c_str());

  Stub b;
  // From inside send_, observe how many ack/fill lines are already on disk.
  int ack_lines_at_ack_send = -1;
  int fill_lines_at_fill_send = -1;
  auto send = [&](int, const std::vector<std::uint8_t>& bytes) {
    OrderMessage m;
    if (!DecodeOrder(bytes.data(), bytes.size(), &m)) return;
    if (m.kind == OrderMsgKind::kAck) ack_lines_at_ack_send = LinesOfType(fpath, "ack");
    if (m.kind == OrderMsgKind::kFill) fill_lines_at_fill_send = LinesOfType(fpath, "fill");
  };

  OrderHub::RiskConfig risk;
  risk.journal_dir = dir;  // order_flow_journal defaults on
  OrderHub hub(&b, send, risk);
  CHECK(hub.Start());

  OrderSubmitMsg os{"kf-1", "2330", Market::kTse, Board::kRoundLot, Side::kBuy, "Cash",
                    "ROD",  92500,  1000};
  hub.OnClientMessage(1, EncodeOrderSubmit(os).data(), EncodeOrderSubmit(os).size());

  b.FireAck("kf-1", true);
  b.FireFill("kf-1", Fill{1000, 92500});

  std::printf("ack line already on disk when ack was delivered : %d\n", ack_lines_at_ack_send);
  std::printf("fill line already on disk when fill was delivered: %d\n", fill_lines_at_fill_send);

  // Both the ACK and the FILL are delivered BEFORE they are journaled, so no
  // disk I/O sits on the delivery path.
  CHECK(ack_lines_at_ack_send == 0);    // ack: send precedes journal
  CHECK(fill_lines_at_fill_send == 0);  // fill: send precedes journal

  std::remove(fpath.c_str());
  ::rmdir(dir.c_str());
  if (g_failures == 0) {
    std::printf("test_order_hub_flow_ordering: OK (send precedes journal)\n");
    return 0;
  }
  std::printf("test_order_hub_flow_ordering: %d check(s) failed\n", g_failures);
  return 1;
}
