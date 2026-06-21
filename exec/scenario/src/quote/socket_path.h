#ifndef KAIROS_EXEC_SOCKET_PATH_H_
#define KAIROS_EXEC_SOCKET_PATH_H_

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace kairos::exec {

// Resolve a UDS path: explicit env, else $XDG_RUNTIME_DIR, else /run/user/<uid>
// (the per-user 0700 runtime dir). Empty if none usable -- never /tmp, which is
// world-writable (any local user could squat the path or connect to the order
// hub). `run_user` is /run/user/<uid> if it exists, else "". `base` is the name.
inline std::string ResolveSock(const char* explicit_env, const char* xdg,
                               const std::string& run_user, const char* base) {
  if (explicit_env != nullptr && explicit_env[0] != '\0') return explicit_env;
  if (xdg != nullptr && xdg[0] != '\0') return std::string(xdg) + "/" + base;
  if (!run_user.empty()) return run_user + "/" + base;
  return "";
}

inline std::string RunUserDir() {
  std::string d = "/run/user/" + std::to_string(::getuid());
  struct stat st;
  return (::stat(d.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) ? d : "";
}

[[noreturn]] inline void NoRuntimeDir(const char* env_name) {
  std::fprintf(stderr, "kairos: no runtime dir for the socket; set $%s or $XDG_RUNTIME_DIR\n",
               env_name);
  std::exit(1);
}

// Quote UDS (core <-> consumers). Must match core's quote_socket_path() (Rust).
inline std::string QuoteSocketPath() {
  std::string p = ResolveSock(std::getenv("KAIROS_QUOTE_SOCK"), std::getenv("XDG_RUNTIME_DIR"),
                              RunUserDir(), "kairos-quotes.sock");
  if (p.empty()) NoRuntimeDir("KAIROS_QUOTE_SOCK");
  return p;
}

// Order hub UDS (hub <-> scenarios; not core).
inline std::string OrderSocketPath() {
  std::string p = ResolveSock(std::getenv("KAIROS_ORDER_SOCK"), std::getenv("XDG_RUNTIME_DIR"),
                              RunUserDir(), "kairos-orders.sock");
  if (p.empty()) NoRuntimeDir("KAIROS_ORDER_SOCK");
  return p;
}

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_SOCKET_PATH_H_
