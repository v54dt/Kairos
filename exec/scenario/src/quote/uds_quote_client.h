#ifndef KAIROS_EXEC_UDS_QUOTE_CLIENT_H_
#define KAIROS_EXEC_UDS_QUOTE_CLIENT_H_

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "quote_book.h"
#include "socket_path.h"

namespace kairos::exec {

// Connects to the core quote UDS, subscribes the given symbols, and delivers
// each decoded quote via the callback. Reconnects with backoff on drop.
class UdsQuoteClient {
 public:
  using QuoteFn = std::function<void(const std::string& symbol, const TopOfBook&)>;

  UdsQuoteClient(std::string socket_path, std::vector<std::string> symbols, QuoteFn on_quote);
  ~UdsQuoteClient();

  void Start();
  void Stop();

 private:
  void Run();
  int ConnectAndSubscribe();

  std::string socket_path_;
  std::vector<std::string> symbols_;
  QuoteFn on_quote_;
  std::atomic<bool> stop_{false};
  std::atomic<int> fd_{-1};
  std::thread thread_;
};

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_UDS_QUOTE_CLIENT_H_
