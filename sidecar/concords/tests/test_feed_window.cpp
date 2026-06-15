// Self-test for FeedStale: the SDK-frozen detection predicate. No broker, no SDK.

#include <cassert>
#include <iostream>

#include "feed.h"

using kairos::concords::FeedStale;

int main() {
  const int kThr = 30;

  // in window (08:30-13:30), gap >= threshold -> stale
  assert(FeedStale(900, 30, kThr));
  assert(FeedStale(900, 45, kThr));
  assert(FeedStale(1329, 30, kThr));
  assert(FeedStale(830, 31, kThr));  // window opens at 08:30

  // in window but gap below threshold -> not stale
  assert(!FeedStale(900, 29, kThr));
  assert(!FeedStale(900, 0, kThr));

  // outside the window -> never stale (no quotes expected)
  assert(!FeedStale(829, 999, kThr));   // before open
  assert(!FeedStale(1330, 999, kThr));  // at close
  assert(!FeedStale(1500, 999, kThr));  // afternoon

  // threshold 0 disables the watchdog
  assert(!FeedStale(900, 999, 0));

  std::cout << "test_feed_window: OK\n";
  return 0;
}
