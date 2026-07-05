#include "hub_status.h"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "socket_path.h"

namespace kairos::exec {

namespace {

// Escape the few characters that would break a JSON string literal.
std::string JsonEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out;
}

}  // namespace

std::string SerializeHubStatus(const HubStatus& s) {
  std::string out = "{\"start_epoch_s\":" + std::to_string(s.start_epoch_s) +
                    ",\"written_epoch_s\":" + std::to_string(s.written_epoch_s) +
                    ",\"client_count\":" + std::to_string(s.client_count) + ",\"clients\":[";
  for (std::size_t i = 0; i < s.clients.size(); ++i) {
    const ClientStatus& c = s.clients[i];
    if (i != 0) out += ',';
    out += "{\"prefix\":\"" + JsonEscape(c.prefix) + "\",\"pid\":" + std::to_string(c.pid) +
           ",\"open\":" + std::to_string(c.open) + ",\"submitted\":" + std::to_string(c.submitted) +
           ",\"filled\":" + std::to_string(c.filled) +
           ",\"cancelled\":" + std::to_string(c.cancelled) +
           ",\"last_activity_s\":" + std::to_string(c.last_activity_s) + "}";
  }
  out += "],\"account_open_notional_cents\":" + std::to_string(s.account_open_notional_cents) +
         ",\"account_day_realized_cents\":" + std::to_string(s.account_day_realized_cents) +
         ",\"max_account_notional_cents\":" + std::to_string(s.max_account_notional_cents) +
         ",\"halted\":" + (s.halted ? "true" : "false") + "}\n";
  return out;
}

bool AtomicWriteFile(const std::string& path, const std::string& content) {
  std::string tmp = path + ".tmp." + std::to_string(::getpid());
  std::FILE* f = std::fopen(tmp.c_str(), "wbe");
  if (f == nullptr) return false;
  bool ok = std::fwrite(content.data(), 1, content.size(), f) == content.size();
  ok = (std::fflush(f) == 0) && ok;
  std::fclose(f);
  if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) {
    ::unlink(tmp.c_str());
    return false;
  }
  return true;
}

std::string HubStatusPath() {
  return ResolveSock(std::getenv("KAIROS_HUB_STATUS"), std::getenv("XDG_RUNTIME_DIR"), RunUserDir(),
                     "kairos-hub-status.json");
}

std::string HubHaltPath() {
  return ResolveSock(std::getenv("KAIROS_HUB_HALT"), std::getenv("XDG_RUNTIME_DIR"), RunUserDir(),
                     "kairos-hub-halt");
}

}  // namespace kairos::exec
