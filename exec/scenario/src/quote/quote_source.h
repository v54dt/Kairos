#ifndef KAIROS_EXEC_QUOTE_SOURCE_H_
#define KAIROS_EXEC_QUOTE_SOURCE_H_

#include <functional>
#include <string>

#include "quote_book.h"

namespace kairos::exec {

// Quote feed seam the engine consumes: delivers each decoded TopOfBook with its
// symbol to a callback. Set the callback before Start() (not thread-safe against
// a running feed). UdsQuoteClient is the production implementation.
class QuoteSource {
 public:
  using QuoteFn = std::function<void(const std::string& symbol, const TopOfBook&)>;
  using TradeFn = std::function<void(const std::string& symbol, const Trade&)>;

  virtual ~QuoteSource() = default;

  virtual void SetCallback(QuoteFn on_quote) = 0;
  // Optional trade stream (A4 sim hub). Default no-op so existing consumers stay
  // bit-for-bit unchanged: without a trade callback the client never decodes Trade
  // frames, leaving the quote path and its unknown-variant counter untouched.
  virtual void SetTradeCallback(TradeFn /*on_trade*/) {}
  virtual void Start() = 0;
  virtual void Stop() = 0;
};

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_QUOTE_SOURCE_H_
