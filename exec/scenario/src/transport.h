#ifndef KAIROS_EXEC_TRANSPORT_H_
#define KAIROS_EXEC_TRANSPORT_H_

#include <string>
#include <vector>

namespace kairos::exec {

struct HttpRequest {
  std::string url;
  std::string body;
  std::vector<std::string> headers;  // e.g. {"Content-Type: application/json"}
  long timeout_sec = 5;
};

// Async best-effort HTTP transport. The dispatcher depends on this seam so it
// can be unit-tested with a fake (no network).
class Transport {
 public:
  virtual ~Transport() = default;
  virtual void Post(HttpRequest req) = 0;
};

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_TRANSPORT_H_
