#ifndef KAIROS_EXEC_HTTP_POSTER_H_
#define KAIROS_EXEC_HTTP_POSTER_H_

#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>

#include "transport.h"

namespace kairos::exec {

// Fire-and-forget async HTTP POST: one worker thread drains a queue so the trade
// path never blocks on the network. Failures go to stderr, never to the caller.
class HttpPoster : public Transport {
 public:
  HttpPoster();
  ~HttpPoster() override;
  HttpPoster(const HttpPoster&) = delete;
  HttpPoster& operator=(const HttpPoster&) = delete;

  void Post(HttpRequest req) override;

 private:
  void Run();
  static void DoPost(const HttpRequest& req);

  std::queue<HttpRequest> queue_;
  std::mutex mu_;
  std::condition_variable cv_;
  bool stop_ = false;
  std::thread worker_;
};

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_HTTP_POSTER_H_
