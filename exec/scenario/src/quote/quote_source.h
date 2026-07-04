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

  virtual ~QuoteSource() = default;

  virtual void SetCallback(QuoteFn on_quote) = 0;
  virtual void Start() = 0;
  virtual void Stop() = 0;
};

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_QUOTE_SOURCE_H_
