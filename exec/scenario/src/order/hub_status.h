#ifndef KAIROS_EXEC_HUB_STATUS_H_
#define KAIROS_EXEC_HUB_STATUS_H_

#include <string>
#include <vector>

namespace kairos::exec {

// Read-only view of one scenario client the hub is serving. `prefix`/`pid` come
// from the user_defined_id (k<pid>-<seq>); `open` is acked-but-not-yet-filled-or-
// cancelled orders; the counters are lifetime totals for this connection.
struct ClientStatus {
  std::string prefix;
  long pid = 0;
  int open = 0;
  long submitted = 0;
  long filled = 0;
  long cancelled = 0;
  long last_activity_s = 0;
};

// Whole-hub snapshot serialized to a status file for the TUI to read.
struct HubStatus {
  long start_epoch_s = 0;
  long written_epoch_s = 0;
  int client_count = 0;
  std::vector<ClientStatus> clients;
};

// Single-line JSON (no external JSON lib; matches the hand-rolled JSONL style).
std::string SerializeHubStatus(const HubStatus& s);

// Write `content` durably: temp file + rename, atomic on the same filesystem.
bool AtomicWriteFile(const std::string& path, const std::string& content);

// $KAIROS_HUB_STATUS, else $XDG_RUNTIME_DIR, else /run/user/<uid>; "" if none.
// Best-effort (never /tmp, never exits): a missing runtime dir just skips writes.
std::string HubStatusPath();

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_HUB_STATUS_H_
