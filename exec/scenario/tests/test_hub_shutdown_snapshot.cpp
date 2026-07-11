// Regression: the on-shutdown status snapshot must preserve the clients that
// were connected/open when Stop() ran. It used to capture after draining the
// client fds, by which point OnClientDisconnect had erased per-client state,
// clobbering the last good file with {"client_count":0,"clients":[]}.

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

#include "order_backend.h"
#include "order_codec.h"
#include "order_hub_server.h"
#include "test_check.h"
#include "uds_frame.h"

using namespace kairos::exec;

static std::string ReadFile(const std::string& p) {
  std::ifstream in(p);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static int ConnectClient(const std::string& path) {
  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

int main() {
  const std::string statusfile = "/tmp/kairos-hub-shutdown-" + std::to_string(::getpid()) + ".json";
  ::setenv("KAIROS_HUB_STATUS", statusfile.c_str(), 1);
  ::unlink(statusfile.c_str());
  const std::string sock = "/tmp/kairos-shutdown-hub-" + std::to_string(::getpid()) + ".sock";

  PaperOrderBackend backend;
  OrderHubServer server(&backend, sock);
  CHECK(server.Start());

  int fd = ConnectClient(sock);
  CHECK(fd >= 0);
  if (fd < 0) {
    server.Stop();
    std::printf("test_hub_shutdown_snapshot: FAILED (connect)\n");
    return 1;
  }

  OrderSubmitMsg m{"k4242-1", "2330", Market::kTse, Board::kOddLot, Side::kBuy, "Cash",
                   "ROD",     92500,  1000};
  CHECK(WriteFrame(fd, EncodeOrderSubmit(m)));
  std::this_thread::sleep_for(std::chrono::milliseconds(300));  // let the hub process the submit

  // Shut down with the client still connected: the final snapshot must reflect
  // that client, not an empty registry.
  server.Stop();
  const std::string after_stop = ReadFile(statusfile);
  std::printf("after shutdown: %s", after_stop.c_str());
  CHECK(after_stop.find("\"prefix\":\"k4242\"") != std::string::npos);
  CHECK(after_stop.find("\"client_count\":0") == std::string::npos);
  CHECK(after_stop.find("\"clients\":[]") == std::string::npos);

  ::close(fd);
  ::unlink(statusfile.c_str());

  if (g_failures == 0) {
    std::printf("test_hub_shutdown_snapshot: OK\n");
    return 0;
  }
  std::printf("test_hub_shutdown_snapshot: FAILED %d check(s)\n", g_failures);
  return 1;
}
