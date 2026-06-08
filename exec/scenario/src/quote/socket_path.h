#ifndef KAIROS_EXEC_SOCKET_PATH_H_
#define KAIROS_EXEC_SOCKET_PATH_H_

#include <cstdlib>
#include <string>

namespace kairos::exec {

// Must match core's quote_socket_path() (Rust): $KAIROS_QUOTE_SOCK, else
// $XDG_RUNTIME_DIR/kairos-quotes.sock, else /tmp/kairos-quotes.sock.
inline std::string ResolveSocketPath(const char* explicit_env, const char* xdg) {
  if (explicit_env != nullptr && explicit_env[0] != '\0') return explicit_env;
  if (xdg != nullptr && xdg[0] != '\0') return std::string(xdg) + "/kairos-quotes.sock";
  return "/tmp/kairos-quotes.sock";
}

inline std::string QuoteSocketPath() {
  return ResolveSocketPath(std::getenv("KAIROS_QUOTE_SOCK"), std::getenv("XDG_RUNTIME_DIR"));
}

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_SOCKET_PATH_H_
