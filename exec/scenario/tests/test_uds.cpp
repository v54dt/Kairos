// Self-test for socket-path resolution + UDS framing. No broker; framing is
// exercised over an in-process socketpair.

#include <sys/socket.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "socket_path.h"
#include "uds_frame.h"

using namespace kairos::exec;

static int g_failures = 0;

#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                \
    }                                                              \
  } while (0)

static void TestSocketPath() {
  const char* q = "kairos-quotes.sock";
  // explicit env wins
  CHECK(ResolveSock("/run/k.sock", "/run/user/1001", "/run/user/1001", q) == "/run/k.sock");
  // XDG preferred over /run/user
  CHECK(ResolveSock(nullptr, "/run/user/1001", "/run/user/1001", q) ==
        "/run/user/1001/kairos-quotes.sock");
  // /run/user used when XDG is unset
  CHECK(ResolveSock(nullptr, nullptr, "/run/user/1001", q) == "/run/user/1001/kairos-quotes.sock");
  // nothing usable -> empty (never /tmp)
  CHECK(ResolveSock(nullptr, nullptr, "", q).empty());
  CHECK(ResolveSock("", "", "", q).empty());
  // empty values skip to the next candidate
  CHECK(ResolveSock("", nullptr, "/run/user/1001", q) == "/run/user/1001/kairos-quotes.sock");
}

static void TestFraming() {
  int sv[2];
  CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

  std::vector<std::uint8_t> payload = {1, 2, 3, 4, 5, 0xff, 0x00, 0x42};
  CHECK(WriteFrame(sv[0], payload));
  std::vector<std::uint8_t> empty;
  CHECK(WriteFrame(sv[0], empty));

  std::vector<std::uint8_t> out;
  CHECK(ReadFrame(sv[1], &out) == 1);
  CHECK(out == payload);
  CHECK(ReadFrame(sv[1], &out) == 1);
  CHECK(out.empty());

  ::close(sv[0]);  // writer gone -> clean EOF on the reader
  CHECK(ReadFrame(sv[1], &out) == 0);
  ::close(sv[1]);
}

int main() {
  TestSocketPath();
  TestFraming();
  if (g_failures == 0) {
    std::printf("test_uds: OK\n");
    return 0;
  }
  std::printf("test_uds: FAILED %d check(s)\n", g_failures);
  return 1;
}
