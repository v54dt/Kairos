#include "http_poster.h"

#include <curl/curl.h>

#include <cstdio>
#include <mutex>
#include <utility>

namespace kairos::exec {

namespace {
std::once_flag g_curl_once;
void EnsureCurlGlobal() {
  std::call_once(g_curl_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}
}  // namespace

HttpPoster::HttpPoster() {
  EnsureCurlGlobal();
  worker_ = std::thread([this] { Run(); });
}

HttpPoster::~HttpPoster() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    stop_ = true;
  }
  cv_.notify_all();
  if (worker_.joinable()) worker_.join();
}

void HttpPoster::Post(HttpRequest req) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    queue_.push(std::move(req));
  }
  cv_.notify_one();
}

void HttpPoster::Run() {
  while (true) {
    HttpRequest item;
    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
      if (queue_.empty()) return;  // shutdown with drained queue
      item = std::move(queue_.front());
      queue_.pop();
    }
    DoPost(item);
  }
}

void HttpPoster::DoPost(const HttpRequest& req) {
  CURL* curl = curl_easy_init();
  if (!curl) {
    std::fprintf(stderr, "kairos-exec: http init failed for %s\n", req.url.c_str());
    return;
  }

  curl_slist* headers = nullptr;
  for (const auto& h : req.headers) {
    headers = curl_slist_append(headers, h.c_str());
  }

  curl_easy_setopt(curl, CURLOPT_URL, req.url.c_str());
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(req.body.size()));
  if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, req.timeout_sec);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(
      curl, CURLOPT_WRITEFUNCTION,
      +[](char*, size_t s, size_t n, void*) -> size_t { return s * n; });

  CURLcode res = curl_easy_perform(curl);
  long status_code = 0;
  if (res == CURLE_OK) {
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
  }

  if (headers) curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK || status_code < 200 || status_code >= 300) {
    std::fprintf(stderr, "kairos-exec: http POST %s failed: %s (HTTP %ld)\n", req.url.c_str(),
                 curl_easy_strerror(res), status_code);
  }
}

}  // namespace kairos::exec
