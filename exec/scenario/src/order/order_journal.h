#ifndef KAIROS_EXEC_ORDER_JOURNAL_H_
#define KAIROS_EXEC_ORDER_JOURNAL_H_

#include <cstdio>
#include <string>
#include <vector>

#include "tw_market.h"  // Cents

namespace kairos::exec {

// One fill recovered from a journal (only what accounting replay needs).
struct JournalFill {
  long shares = 0;
  Cents price = 0;
};

// Append-only JSONL run-state journal. Order lifecycle events are logged (each
// fill fsynced) so a mid-session trader restart can replay fills and restore
// accounting — no double-buy. One file per (symbol, side, day).
class OrderJournal {
 public:
  OrderJournal() = default;
  ~OrderJournal();
  OrderJournal(const OrderJournal&) = delete;
  OrderJournal& operator=(const OrderJournal&) = delete;

  // Opens <dir>/<name>.jsonl for append, creating <dir>. False on error.
  bool Open(const std::string& dir, const std::string& name);
  bool is_open() const { return f_ != nullptr; }

  void LogFill(const std::string& id, long shares, Cents price);
  void LogAck(const std::string& id, bool ok);
  void LogCancel(const std::string& id, bool ok);

  // Open <dir>/<name>.jsonl, append one fill line (fsynced), close. For a writer
  // that keeps no open handle (the hub journaling a rare orphan fill). O_APPEND
  // per line makes it safe to interleave with a trader appending the same file.
  static bool AppendFill(const std::string& dir, const std::string& name, const std::string& id,
                         long shares, Cents price);

 private:
  void Write(const std::string& line);  // append + flush + fsync
  std::FILE* f_ = nullptr;
};

std::string JournalPath(const std::string& dir, const std::string& name);

// Today's YYYYMMDD in fixed UTC+8, matching the engine's journal-file day so the
// hub and the trader name the same per-(symbol,side,day) file.
std::string JournalDayUtc8();

// Replay: fills from a journal file (missing file => empty; bad lines skipped).
std::vector<JournalFill> ReadJournalFills(const std::string& path);

// Value of `"<key>":<integer>` in one of our JSONL lines, or dflt if absent.
long JournalJsonInt(const std::string& line, const std::string& key, long dflt);

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_ORDER_JOURNAL_H_
