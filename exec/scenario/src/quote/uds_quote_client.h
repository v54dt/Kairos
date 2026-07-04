#ifndef KAIROS_EXEC_UDS_QUOTE_CLIENT_H_
#define KAIROS_EXEC_UDS_QUOTE_CLIENT_H_

#include <atomic>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "quote_book.h"
#include "quote_source.h"
#include "socket_path.h"

namespace kairos::exec {

// Connects to the core quote UDS, subscribes the given symbols, and delivers
// each decoded quote via the callback. Reconnects with backoff on drop.
class UdsQuoteClient : public QuoteSource {
 public:
  UdsQuoteClient(std::string socket_path, std::vector<std::string> symbols, QuoteFn on_quote = {});
  ~UdsQuoteClient() override;

  void SetCallback(QuoteFn on_quote) override { on_quote_ = std::move(on_quote); }
  void SetTradeCallback(TradeFn on_trade) override { on_trade_ = std::move(on_trade); }
  void Start() override;
  void Stop() override;

 private:
  void Run();
  int ConnectAndSubscribe();

  std::string socket_path_;
  std::vector<std::string> symbols_;
  QuoteFn on_quote_;
  TradeFn on_trade_;
  std::atomic<bool> stop_{false};
  std::atomic<int> fd_{-1};
  std::thread thread_;
};

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_UDS_QUOTE_CLIENT_H_
