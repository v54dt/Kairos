// depth_evap predicate on synthetic books: proves a single enter on a depth
// collapse, a single exit only after recovery past the upper threshold, that a
// flickering ratio while fired never re-fires (hysteresis), that a low ratio
// during warmup is suppressed, and that cooldown gates re-entry after an exit.
// Pure: time is the ts_us argument, no clock or socket.

#include <cstdint>
#include <optional>
#include <vector>

#include "predicate_depth_evap.h"
#include "test_check.h"

using namespace kairos::exec;

namespace {

std::int64_t Sec(double s) { return static_cast<std::int64_t>(s * 1e6); }

TopOfBook BidBook(long vol) {
  TopOfBook tob;
  tob.valid = true;
  tob.n_bids = 1;
  tob.bids[0].price = 10000;
  tob.bids[0].volume = vol;
  tob.n_asks = 1;
  tob.asks[0].price = 10010;
  tob.asks[0].volume = 5000;
  return tob;
}

// window_s large so the frozen/decaying baseline stays near 1000 across a test.
DepthEvapParams Params() {
  DepthEvapParams p;
  p.side = DepthSide::kBid;
  p.window_s = 100.0;
  p.ratio_enter = 0.4;
  p.ratio_exit = 0.7;
  p.warmup_s = 5.0;
  p.cooldown_us = Sec(5.0);
  return p;
}

int CountAction(DepthEvapPredicate& p, const char* symbol, long vol, double t, SignalAction want,
                int* other = nullptr) {
  auto fire = p.OnQuote(symbol, BidBook(vol), Sec(t));
  if (!fire) return 0;
  if (fire->action == want) return 1;
  if (other != nullptr) ++*other;
  return 0;
}

void TestEnterExitOnce() {
  DepthEvapPredicate p("de", {"2330"}, Params());
  p.OnQuote("2330", BidBook(1000), Sec(0));  // baseline
  for (double t = 1; t <= 6; ++t) p.OnQuote("2330", BidBook(1000), Sec(t));

  int enters = 0, exits = 0;
  enters += CountAction(p, "2330", 300, 7, SignalAction::kEnter);  // collapse -> enter
  enters += CountAction(p, "2330", 300, 8, SignalAction::kEnter);  // still low -> no re-fire
  exits += CountAction(p, "2330", 1000, 9, SignalAction::kExit);   // recover -> exit
  CHECK(enters == 1);
  CHECK(exits == 1);
}

void TestFlickerNoStorm() {
  DepthEvapPredicate p("de", {"2330"}, Params());
  p.OnQuote("2330", BidBook(1000), Sec(0));
  for (double t = 1; t <= 6; ++t) p.OnQuote("2330", BidBook(1000), Sec(t));

  int enters = 0;
  enters += CountAction(p, "2330", 390, 7.0, SignalAction::kEnter);  // 0.39 -> enter
  enters += CountAction(p, "2330", 410, 7.1, SignalAction::kEnter);  // 0.41, fired -> none
  enters += CountAction(p, "2330", 390, 7.2, SignalAction::kEnter);  // dips again -> none
  enters += CountAction(p, "2330", 420, 7.3, SignalAction::kEnter);  // in band -> none
  CHECK(enters == 1);  // one enter despite the flicker around the threshold
}

void TestWarmupSuppressesEarly() {
  DepthEvapPredicate p("de", {"2330"}, Params());
  p.OnQuote("2330", BidBook(1000), Sec(0));
  auto early = p.OnQuote("2330", BidBook(200), Sec(1));  // ratio 0.2 but pre-warmup
  CHECK(!early.has_value());
  auto warm = p.OnQuote("2330", BidBook(200), Sec(6));  // now past warmup -> enter
  CHECK(warm.has_value());
  CHECK(warm->action == SignalAction::kEnter);
}

void TestCooldownGatesReentry() {
  DepthEvapPredicate p("de", {"2330"}, Params());
  p.OnQuote("2330", BidBook(1000), Sec(0));
  for (double t = 1; t <= 6; ++t) p.OnQuote("2330", BidBook(1000), Sec(t));

  int enters = 0;
  enters += CountAction(p, "2330", 300, 7, SignalAction::kEnter);  // enter @7s
  CountAction(p, "2330", 1000, 8, SignalAction::kExit);            // exit @8s
  enters += CountAction(p, "2330", 300, 9, SignalAction::kEnter);  // 2s later -> cooldown blocks
  CHECK(enters == 1);
  enters += CountAction(p, "2330", 300, 15, SignalAction::kEnter);  // 8s after enter -> allowed
  CHECK(enters == 2);
}

void TestAskSide() {
  DepthEvapParams params = Params();
  params.side = DepthSide::kAsk;
  DepthEvapPredicate p("de", {"2330"}, params);
  auto make = [](long ask_vol) {
    TopOfBook tob = BidBook(1000);  // bid steady; only ask collapses
    tob.asks[0].volume = ask_vol;
    return tob;
  };
  p.OnQuote("2330", make(1000), Sec(0));
  for (double t = 1; t <= 6; ++t) p.OnQuote("2330", make(1000), Sec(t));
  auto fire = p.OnQuote("2330", make(200), Sec(7));
  CHECK(fire.has_value());
  CHECK(fire->action == SignalAction::kEnter);
}

}  // namespace

int main() {
  TestEnterExitOnce();
  TestFlickerNoStorm();
  TestWarmupSuppressesEarly();
  TestCooldownGatesReentry();
  TestAskSide();
  if (g_failures == 0) {
    std::printf("test_predicate_depth_evap: OK\n");
    return 0;
  }
  std::printf("test_predicate_depth_evap: FAILED %d check(s)\n", g_failures);
  return 1;
}
