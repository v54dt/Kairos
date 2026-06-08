// Self-test for the PaperOrderBackend contract. No broker.

#include <cstdio>
#include <string>

#include "order_backend.h"

using namespace kairos::exec;

static int g_failures = 0;

#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                \
    }                                                              \
  } while (0)

#define CHECK_EQ(a, b)                                                                   \
  do {                                                                                   \
    auto _a = (a);                                                                       \
    auto _b = (b);                                                                       \
    if (!(_a == _b)) {                                                                   \
      std::printf("FAIL  %s:%d  %s == %s  (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, \
                  (long long)_a, (long long)_b);                                         \
      ++g_failures;                                                                      \
    }                                                                                    \
  } while (0)

int main() {
  PaperOrderBackend backend;

  std::string acked_id;
  bool ack_ok = false;
  std::string filled_id;
  Fill got{};
  std::string cancelled_id;

  backend.SetCallbacks(
      [&](const std::string& id, bool ok, const std::string&) {
        acked_id = id;
        ack_ok = ok;
      },
      [&](const std::string& id, const Fill& f) {
        filled_id = id;
        got = f;
      },
      [&](const std::string& id, bool) { cancelled_id = id; });

  CHECK(backend.Connect());
  CHECK(backend.IsConnected());

  backend.Submit(
      {"ord-1", "2330", Market::kTse, Board::kOddLot, Side::kBuy, "Cash", "ROD", 58000, 100});
  CHECK(acked_id == "ord-1");
  CHECK(ack_ok);
  CHECK(filled_id == "ord-1");  // paper fills immediately at the limit
  CHECK_EQ(got.shares, 100);
  CHECK_EQ(got.price, 58000);

  backend.Submit(
      {"ord-2", "2330", Market::kTse, Board::kOddLot, Side::kSell, "Cash", "ROD", 58100, 50});
  CHECK(filled_id == "ord-2");
  CHECK_EQ(got.shares, 50);
  CHECK_EQ(got.price, 58100);

  backend.Cancel("ord-1");
  CHECK(cancelled_id == "ord-1");

  backend.Disconnect();
  CHECK(!backend.IsConnected());

  if (g_failures == 0) {
    std::printf("test_order_backend: OK\n");
    return 0;
  }
  std::printf("test_order_backend: FAILED %d check(s)\n", g_failures);
  return 1;
}
