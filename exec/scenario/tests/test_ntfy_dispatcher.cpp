// Self-test for NtfyDispatcher routing + policy + render. No network (fake
// Transport + injected clock).

#include <chrono>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "event.h"
#include "ntfy_dispatcher.h"
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

bool Contains(const std::string& s, const std::string& sub) {
  return s.find(sub) != std::string::npos;
}

NotifyConfig BaseCfg() {
  NotifyConfig c;
  c.enabled = true;
  c.base_url = "https://notify.test";
  c.topic = "kairos";
  c.token = "tok";
  return c;
}

Event Fill() {
  return Event{EventCategory::kFill, Severity::kInfo, "2330", "", {{"px", "2360.00"}}};
}
}  // namespace

int main() {
  using namespace std::chrono;
  steady_clock::time_point t{};
  auto clock = [&t] { return t; };

  // disabled -> nothing
  {
    RecordingTransport tr;
    NotifyConfig c = BaseCfg();
    c.enabled = false;
    NtfyDispatcher d(c, &tr, clock);
    d.Emit(Fill());
    CHECK(tr.reqs.empty());
  }

  // fill -> one post with topic/title/priority + auth header (capitalized title, no tags)
  {
    RecordingTransport tr;
    NtfyDispatcher d(BaseCfg(), &tr, clock);
    d.Emit(Fill());
    CHECK(tr.reqs.size() == 1);
    CHECK(tr.reqs[0].url == "https://notify.test");
    CHECK(Contains(tr.reqs[0].body, "\"topic\":\"kairos\""));
    CHECK(Contains(tr.reqs[0].body, "2330 Fill"));
    CHECK(Contains(tr.reqs[0].body, "\"priority\":3"));
    CHECK(Contains(tr.reqs[0].body, "\"tags\":[]"));
    bool auth = false;
    for (const auto& h : tr.reqs[0].headers)
      if (h == "Authorization: Bearer tok") auth = true;
    CHECK(auth);
  }

  // min_severity = warning -> info fill dropped
  {
    RecordingTransport tr;
    NotifyConfig c = BaseCfg();
    c.min_severity = Severity::kWarning;
    NtfyDispatcher d(c, &tr, clock);
    d.Emit(Fill());
    CHECK(tr.reqs.empty());
  }

  // dedupe: error (window 300s) twice same key -> 1; after window -> 2
  {
    RecordingTransport tr;
    NtfyDispatcher d(BaseCfg(), &tr, clock);
    Event e{EventCategory::kError, Severity::kError, "2330", "reject:1", {}};
    d.Emit(e);
    d.Emit(e);
    CHECK(tr.reqs.size() == 1);
    t += seconds(301);
    d.Emit(e);
    CHECK(tr.reqs.size() == 2);
  }

  // token bucket: capacity 2, no refill -> 3 fills -> 2 posts
  {
    RecordingTransport tr;
    NotifyConfig c = BaseCfg();
    c.rate_capacity = 2;
    c.rate_refill_per_h = 0.0;
    NtfyDispatcher d(c, &tr, clock);
    d.Emit(Fill());
    d.Emit(Fill());
    d.Emit(Fill());
    CHECK(tr.reqs.size() == 2);
  }

  // terminal events bypass the bucket: empty bucket still sends complete
  {
    RecordingTransport tr;
    NotifyConfig c = BaseCfg();
    c.rate_capacity = 1;
    c.rate_refill_per_h = 0.0;
    NtfyDispatcher d(c, &tr, clock);
    d.Emit(Fill());  // consumes the only token
    d.Emit(Fill());  // dropped: bucket empty
    CHECK(tr.reqs.size() == 1);
    d.Emit(Event{EventCategory::kComplete, Severity::kInfo, "2330", "", {}});
    CHECK(tr.reqs.size() == 2);
  }

  if (g_failures == 0) {
    std::printf("test_ntfy_dispatcher: OK\n");
    return 0;
  }
  std::printf("test_ntfy_dispatcher: FAILED %d check(s)\n", g_failures);
  return 1;
}
