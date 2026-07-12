// Self-test for FailureHalt: halt-at-exactly-N and reset-on-good-ack.

#include <cstdio>

#include "failure_halt.h"
#include "test_check.h"

using namespace kairos::exec;

namespace {

void TestHaltAtExactlyN() {
  FailureHalt h;
  CHECK(!h.halted());
  h.RegisterFailure("rejected", 3);
  CHECK(!h.halted());
  h.RegisterFailure("rejected", 3);
  CHECK(!h.halted());
  h.RegisterFailure("ack timeout", 3);
  CHECK(h.halted());
  CHECK(h.reason() == "halted: 3 consecutive order failures (ack timeout)");
}

void TestResetOnGoodAck() {
  FailureHalt h;
  h.RegisterFailure("rejected", 3);
  h.RegisterFailure("rejected", 3);
  h.Reset();  // a good ack clears the streak
  h.RegisterFailure("rejected", 3);
  h.RegisterFailure("rejected", 3);
  CHECK(!h.halted());
  h.RegisterFailure("rejected", 3);
  CHECK(h.halted());
}

void TestCapDisabled() {
  FailureHalt h;
  for (int i = 0; i < 100; ++i) h.RegisterFailure("rejected", 0);
  CHECK(!h.halted());
  CHECK(h.reason().empty());
}

}  // namespace

int main() {
  TestHaltAtExactlyN();
  TestResetOnGoodAck();
  TestCapDisabled();
  if (g_failures == 0) {
    std::printf("test_failure_halt: OK\n");
    return 0;
  }
  std::printf("test_failure_halt: FAILED %d check(s)\n", g_failures);
  return 1;
}
