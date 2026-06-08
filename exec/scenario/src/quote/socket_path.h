#ifndef KAIROS_EXEC_SOCKET_PATH_H_
#define KAIROS_EXEC_SOCKET_PATH_H_

#include <cstdlib>
#include <string>

namespace kairos::exec {

inline std::string ResolveSock(const char* explicit_env, const char* xdg, const char* base) {
  if (explicit_env != nullptr && explicit_env[0] != '\0') return explicit_env;
  if (xdg != nullptr && xdg[0] != '\0') return std::string(xdg) + "/" + base;
  return std::string("/tmp/") + base;
}

// Must match core's quote_socket_path() (Rust): $KAIROS_QUOTE_SOCK, else
// $XDG_RUNTIME_DIR/kairos-quotes.sock, else /tmp/kairos-quotes.sock.
inline std::string ResolveSocketPath(const char* explicit_env, const char* xdg) {
  return ResolveSock(explicit_env, xdg, "kairos-quotes.sock");
}

inline std::string QuoteSocketPath() {
  return ResolveSocketPath(std::getenv("KAIROS_QUOTE_SOCK"), std::getenv("XDG_RUNTIME_DIR"));
}

// Order hub socket (C++-only; hub <-> scenarios, not core): $KAIROS_ORDER_SOCK,
// else $XDG_RUNTIME_DIR/kairos-orders.sock, else /tmp/kairos-orders.sock.
inline std::string ResolveOrderSocketPath(const char* explicit_env, const char* xdg) {
  return ResolveSock(explicit_env, xdg, "kairos-orders.sock");
}

inline std::string OrderSocketPath() {
  return ResolveOrderSocketPath(std::getenv("KAIROS_ORDER_SOCK"), std::getenv("XDG_RUNTIME_DIR"));
}

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_SOCKET_PATH_H_
