// Self-test for the Transport seam (fake) + HttpPoster lifecycle. No network.

#include <cstdio>
#include <utility>
#include <vector>

#include "http_poster.h"
#include "transport.h"

using namespace kairos::exec;

static int g_failures = 0;

#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                \
    }                                                              \
  } while (0)

namespace {
class RecordingTransport : public Transport {
 public:
  void Post(HttpRequest req) override { reqs.push_back(std::move(req)); }
  std::vector<HttpRequest> reqs;
};
}  // namespace

int main() {
  // The seam the dispatcher will use is mockable.
  RecordingTransport t;
  Transport& iface = t;
  iface.Post(HttpRequest{"http://example/", "{\"a\":1}", {"Content-Type: application/json"}, 5});
  CHECK(t.reqs.size() == 1);
  CHECK(t.reqs[0].url == "http://example/");
  CHECK(t.reqs[0].body == "{\"a\":1}");
  CHECK(t.reqs[0].headers.size() == 1);

  // HttpPoster constructs + drains + joins cleanly (no network).
  { HttpPoster poster; }

  if (g_failures == 0) {
    std::printf("test_transport: OK\n");
    return 0;
  }
  std::printf("test_transport: FAILED %d check(s)\n", g_failures);
  return 1;
}
