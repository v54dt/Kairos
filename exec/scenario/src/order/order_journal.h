#ifndef KAIROS_EXEC_ORDER_JOURNAL_H_
#define KAIROS_EXEC_ORDER_JOURNAL_H_

#include <chrono>
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

// Best-effort per-day audit stream of every order event the hub processes:
// <dir>/hub-orders-<day>.jsonl, one JSONL line per submit/ack/fill/cancel. Kept
// SEPARATE from the run-state fill journal the engine replays (those files stay
// replay-clean). Handle-less (open-per-line) so day rollover is automatic.
// Concurrent appends from callback threads never tear: each line is emitted in a
// single ::write() on an O_APPEND fd, atomic at end-of-file for a regular file
// regardless of line length; the only unbounded field (the broker ack `err`) is
// additionally capped so no field approaches the page size. Every method returns
// false on failure; the caller logs — it never blocks or crashes the hub. Money
// lines (ack/fill/cancel-ack) fsync; submit/cancel-request flush only.
class OrderFlowJournal {
 public:
  static bool AppendSubmit(const std::string& dir, const std::string& id, const std::string& prefix,
                           const std::string& symbol, const char* side, const char* board,
                           const char* market, const std::string& funding_type,
                           const std::string& time_in_force, long shares, Cents price);
  static bool AppendAck(const std::string& dir, const std::string& id, bool ok,
                        const std::string& err);
  static bool AppendFill(const std::string& dir, const std::string& id, long shares, Cents price,
                         bool unroutable);
  static bool AppendCancelReq(const std::string& dir, const std::string& id);
  static bool AppendCancelAck(const std::string& dir, const std::string& id, bool ok);

 private:
  // Append `line` to <dir>/hub-orders-<day>.jsonl, fsyncing when do_fsync.
  static bool Emit(const std::string& dir, const std::string& line, bool do_fsync);
};

std::string JournalPath(const std::string& dir, const std::string& name);

// The trading day of `tp` in fixed UTC+8 (Taipei, no DST) — the single source
// every trading-day derivation shares so both journal writers name the same file
// and roll on the same boundary regardless of host TZ. String YYYYMMDD form and
// its numeric YYYYMMDD counterpart; both are time_point-parameterized for tests.
std::string TradingDayUtc8(std::chrono::system_clock::time_point tp);
long TradingDayNumUtc8(std::chrono::system_clock::time_point tp);

// Today's YYYYMMDD in fixed UTC+8, matching the engine's journal-file day so the
// hub and the trader name the same per-(symbol,side,day) file.
std::string JournalDayUtc8();

// Resolve the run-state journal directory shared by the trader and the hub, so
// both sides append to and replay one file. Precedence: an explicit per-side toml
// dir > the shared $KAIROS_JOURNAL_DIR > a deprecated per-side env fallback named
// by legacy_env_name (skipped when null) > $HOME/Kairos/data/journal. Empty only
// when none resolve (HOME unset with no config/env) — journaling then disabled.
// *used_legacy, when non-null, is set true iff the legacy fallback supplied the
// result, so the caller can log a one-time deprecation note.
std::string ResolveJournalDir(const std::string& toml_dir, const char* legacy_env_name,
                              bool* used_legacy = nullptr);

// Replay: fills from a journal file (missing file => empty; bad lines skipped).
std::vector<JournalFill> ReadJournalFills(const std::string& path);

// Value of `"<key>":<integer>` in one of our JSONL lines, or dflt if absent.
long JournalJsonInt(const std::string& line, const std::string& key, long dflt);

// Value of `"<key>":"<string>"` in one of our JSONL lines, unescaped, or dflt if
// absent. For reading the string fields of the hub audit stream back in tests.
std::string JournalJsonStr(const std::string& line, const std::string& key,
                           const std::string& dflt);

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_ORDER_JOURNAL_H_
