#include <cassert>
#include <cstdint>
#include <iostream>

#include "quote_encode.h"

// Unit test for SeqEpochTracker: per-symbol monotonic seq, epoch bump on rebuild,
// and per-symbol seq reset on rebuild. No SDK.

int main() {
  kairos::concords::SeqEpochTracker tr;

  // Before any session build the epoch is 0 (the legacy/no-epoch sentinel).
  assert(tr.Epoch() == 0);

  // First build -> epoch 1; each symbol's seq starts at 1 and is independent.
  assert(tr.Rebuild() == 1);
  assert(tr.Epoch() == 1);
  assert(tr.NextSeq("2330") == 1);
  assert(tr.NextSeq("2330") == 2);
  assert(tr.NextSeq("2317") == 1);
  assert(tr.NextSeq("2330") == 3);
  assert(tr.NextSeq("2317") == 2);

  // Rebuild bumps the epoch and resets every per-symbol seq.
  assert(tr.Rebuild() == 2);
  assert(tr.Epoch() == 2);
  assert(tr.NextSeq("2330") == 1);
  assert(tr.NextSeq("2317") == 1);

  // feed.cpp allocates a FRESH tracker per feed session (initial build, daily
  // reconnect, staleness rebuild). The epoch is process-global, so a new
  // tracker keeps advancing it instead of restarting at 1 -- a consumer can
  // still tell a benign rebuild (epoch bump) from data loss.
  kairos::concords::SeqEpochTracker session_a;
  std::uint32_t ea = session_a.Rebuild();
  kairos::concords::SeqEpochTracker session_b;
  std::uint32_t eb = session_b.Rebuild();
  assert(eb == ea + 1);
  // Each new session's per-symbol seq starts fresh.
  assert(session_b.NextSeq("2330") == 1);
  // The older tracker keeps its own captured epoch, so a leaked frozen session
  // stays distinguishable from the rebuilt one.
  assert(session_a.Epoch() == ea);

  std::cout << "test_seq_epoch: OK\n";
  return 0;
}
