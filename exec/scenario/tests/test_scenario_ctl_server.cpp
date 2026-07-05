// Integration test: the ScenarioCtlServer over a real UDS socket. A client sends
// list/start/stop plus malformed/oversized/unknown-mode lines; the server answers
// each (pipelined lines all answered), rejects the bad ones fail-closed, and never
// crashes. No trader is actually run to completion here (missing binary just makes
// the child exit fast); the focus is the protocol + serve loop.

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "scenario_ctl_server.h"

using namespace kairos::exec;

static int g_failures = 0;

#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                \
    }                                                              \
  } while (0)

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
  timeval tv{3, 0};  // a missing reply fails instead of hanging
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  return fd;
}

static void Send(int fd, const std::string& s) {
  std::size_t off = 0;
  while (off < s.size()) {
    ssize_t w = ::write(fd, s.data() + off, s.size() - off);
    if (w <= 0) return;
    off += static_cast<std::size_t>(w);
  }
}

// Read one '\n'-terminated response line.
static std::string ReadLine(int fd) {
  std::string line;
  char c;
  while (line.size() < 65536) {
    ssize_t n = ::recv(fd, &c, 1, 0);
    if (n <= 0) break;
    if (c == '\n') break;
    line.push_back(c);
  }
  return line;
}

int main() {
  std::filesystem::path dir =
      std::filesystem::temp_directory_path() / ("kairos-ctlsrv-" + std::to_string(::getpid()));
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  {
    std::ofstream f(dir / "2330.toml");
    f << "[scenario]\nname=\"a\"\nsymbol=\"2330\"\n";
  }
  std::string sock = (dir / "ctl.sock").string();
  // A trader bin that exits immediately: a start "succeeds" to spawn, then the
  // child exits fast and the monitor reaps it -- exercising the full path safely.
  std::string trader = "/bin/true";

  Supervisor sup(dir.string(), trader);
  ScenarioCtlServer server(&sup, sock);
  CHECK(server.Start());

  int fd = ConnectClient(sock);
  CHECK(fd >= 0);
  if (fd < 0) {
    server.Stop();
    std::printf("test_scenario_ctl_server: FAILED (connect)\n");
    return 1;
  }

  // list -> a snapshot that includes the enumerated 2330 as stopped.
  {
    std::string req = "{\"cmd\":\"list\"}\n";
    Send(fd, req);
    std::string resp = ReadLine(fd);
    CHECK(resp.find("\"ok\":true") != std::string::npos);
    CHECK(resp.find("\"name\":\"2330\"") != std::string::npos);
    CHECK(resp.find("\"state\":\"stopped\"") != std::string::npos);
  }

  // Pipelined: two lines in one write must both be answered before blocking.
  {
    std::string req = "{\"cmd\":\"list\"}\n{\"cmd\":\"list\"}\n";
    Send(fd, req);
    std::string r1 = ReadLine(fd);
    std::string r2 = ReadLine(fd);
    CHECK(r1.find("\"ok\":true") != std::string::npos);
    CHECK(r2.find("\"ok\":true") != std::string::npos);
  }

  // Fail-closed rejections keep the server serving.
  for (const char* bad : {"{\"cmd\":\"start\",\"name\":\"2330\",\"mode\":\"LIVE\"}\n",
                          "{\"cmd\":\"start\",\"name\":\"2330\"}\n",
                          "{\"cmd\":\"start\",\"name\":\"nope\",\"mode\":\"paper\"}\n",
                          "{\"cmd\":\"frobnicate\"}\n", "not json at all\n"}) {
    std::string req = bad;
    Send(fd, req);
    std::string resp = ReadLine(fd);
    CHECK(resp.find("\"ok\":false") != std::string::npos);
  }

  // An oversized line without a newline is rejected, and the server stays up.
  {
    std::string big(kMaxCtlLineLen + 100, 'x');
    big.push_back('\n');
    Send(fd, big);
    std::string resp = ReadLine(fd);
    CHECK(resp.find("\"ok\":false") != std::string::npos);
    CHECK(resp.find("line too long") != std::string::npos);
    // Still serving.
    std::string req = "{\"cmd\":\"list\"}\n";
    Send(fd, req);
    std::string ok = ReadLine(fd);
    CHECK(ok.find("\"ok\":true") != std::string::npos);
  }

  // A valid test-mode start is accepted (the child is /bin/true, reaped fast).
  {
    std::string req = "{\"cmd\":\"start\",\"name\":\"2330\",\"mode\":\"test\"}\n";
    Send(fd, req);
    std::string resp = ReadLine(fd);
    CHECK(resp.find("\"ok\":true") != std::string::npos);
    CHECK(resp.find("\"name\":\"2330\"") != std::string::npos);
  }
  // stop is always accepted (no-op if not running).
  {
    std::string req = "{\"cmd\":\"stop\",\"name\":\"2330\"}\n";
    Send(fd, req);
    std::string resp = ReadLine(fd);
    CHECK(resp.find("\"ok\":true") != std::string::npos);
  }

  ::close(fd);
  server.Stop();
  sup.StopAll();
  std::filesystem::remove_all(dir);

  if (g_failures == 0) {
    std::printf("test_scenario_ctl_server: OK\n");
    return 0;
  }
  std::printf("test_scenario_ctl_server: FAILED %d check(s)\n", g_failures);
  return 1;
}
