// End-to-end: HubOrderBackend <-> OrderHubServer (paper backend) over a real UDS.
// A submit flows client -> hub -> backend, and the ack + fill route back to the
// client's callbacks. Exercises both ends of the order hub. No broker.

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

#include "hub_order_backend.h"
#include "order_backend.h"
#include "order_hub_server.h"
#include "test_check.h"

using namespace kairos::exec;

int main() {
  std::string path = "/tmp/kairos-test-hubbe-" + std::to_string(::getpid()) + ".sock";
  PaperOrderBackend paper;
  OrderHubServer server(&paper, path);
  CHECK(server.Start());

  std::mutex mu;
  std::string ack_id, fill_id;
  bool ack_ok = false;
  long fill_sh = 0;
  Cents fill_px = 0;

  HubOrderBackend client(path);
  client.SetCallbacks(
      [&](const std::string& id, bool ok, const std::string&) {
        std::lock_guard<std::mutex> lock(mu);
        ack_id = id;
        ack_ok = ok;
      },
      [&](const std::string& id, const Fill& f) {
        std::lock_guard<std::mutex> lock(mu);
        fill_id = id;
        fill_sh = f.shares;
        fill_px = f.price;
      },
      [&](const std::string&, bool) {});
  CHECK(client.Connect());
  CHECK(client.IsConnected());

  client.Submit(
      {"k-1", "2330", Market::kTse, Board::kOddLot, Side::kBuy, "Cash", "ROD", 92500, 1000});

  // wait (up to ~2s) for the async ack + fill to arrive via the reader thread
  for (int i = 0; i < 200; ++i) {
    {
      std::lock_guard<std::mutex> lock(mu);
      if (!ack_id.empty() && !fill_id.empty()) break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  {
    std::lock_guard<std::mutex> lock(mu);
    CHECK(ack_id == "k-1");
    CHECK(ack_ok);
    CHECK(fill_id == "k-1");
    CHECK(fill_sh == 1000);
    CHECK(fill_px == 92500);
  }

  // Hub goes away: (1) the disconnect callback fires (a clean Disconnect() does
  // not), and (2) a subsequent Submit is rejected locally since the write fails.
  std::atomic<bool> disconnected{false};
  std::atomic<bool> got_reject{false};
  HubOrderBackend client2(path);
  client2.SetCallbacks(
      [&](const std::string&, bool ok, const std::string&) {
        if (!ok) got_reject = true;
      },
      [](const std::string&, const Fill&) {}, [](const std::string&, bool) {},
      [&] { disconnected = true; });
  CHECK(client2.Connect());

  client.Disconnect();  // clean: fires no disconnect callback
  server.Stop();        // hub gone: client2's reader hits EOF -> disconnect callback
  for (int i = 0; i < 200 && !disconnected; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  CHECK(disconnected);

  client2.Submit(
      {"k-2", "2330", Market::kTse, Board::kOddLot, Side::kBuy, "Cash", "ROD", 92500, 1000});
  CHECK(got_reject);  // write to the dead hub -> synchronous local reject (ok=false)
  client2.Disconnect();

  if (g_failures == 0) {
    std::printf("test_hub_order_backend: OK\n");
    return 0;
  }
  std::printf("test_hub_order_backend: FAILED %d check(s)\n", g_failures);
  return 1;
}
