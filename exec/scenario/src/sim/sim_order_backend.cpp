#include "sim_order_backend.h"

#include <utility>

namespace kairos::exec {

SimOrderBackend::SimOrderBackend(FillMode mode, const std::vector<std::string>& symbols)
    : engine_(
          mode,
          [this](const std::string& id, bool ok, const std::string& e) {
            if (on_ack_) on_ack_(id, ok, e);
          },
          [this](const std::string& id, const Fill& f) {
            if (on_fill_) on_fill_(id, f);
          },
          [this](const std::string& id, bool ok) {
            if (on_cancel_) on_cancel_(id, ok);
          }) {
  for (const auto& s : symbols) engine_.AddSymbol(s);
}

bool SimOrderBackend::Connect() {
  connected_ = true;
  return true;
}

void SimOrderBackend::Disconnect() {
  std::lock_guard<std::mutex> lock(mu_);
  FlushPendingBookLocked();
  connected_ = false;
}

bool SimOrderBackend::IsConnected() const { return connected_; }

void SimOrderBackend::ApplyBookLocked(const std::string& symbol, const TopOfBook& book) {
  if (book.quote_ts_us > last_ts_us_) last_ts_us_ = book.quote_ts_us;
  engine_.OnBook(symbol, book, book.quote_ts_us);
}

void SimOrderBackend::ApplyTradeLocked(const std::string& symbol, const Trade& trade) {
  if (trade.trade_ts_us > last_ts_us_) last_ts_us_ = trade.trade_ts_us;
  engine_.OnTrade(symbol, trade.price, trade.volume, trade.trade_ts_us, trade.is_trial);
}

void SimOrderBackend::FlushPendingBookLocked() {
  if (!has_pending_book_) return;
  has_pending_book_ = false;
  ApplyBookLocked(pending_book_symbol_, pending_book_);
}

void SimOrderBackend::Submit(const OrderSubmitMsg& m) {
  std::lock_guard<std::mutex> lock(mu_);
  FlushPendingBookLocked();  // queue init needs the latest post-trade depth
  SimOrder o;
  o.id = m.id;
  o.symbol = m.symbol;
  o.side = m.side;
  o.board = m.board;
  o.price = m.price;
  o.shares = m.shares;
  o.place_ts_us = last_ts_us_;  // event time of the latest market event
  engine_.Submit(o);
}

void SimOrderBackend::Cancel(const std::string& id) {
  std::lock_guard<std::mutex> lock(mu_);
  FlushPendingBookLocked();
  engine_.Cancel(id, last_ts_us_);
}

void SimOrderBackend::OnBook(const std::string& symbol, const TopOfBook& book) {
  std::lock_guard<std::mutex> lock(mu_);
  ApplyBookLocked(symbol, book);
}

void SimOrderBackend::OnTrade(const std::string& symbol, const Trade& trade) {
  std::lock_guard<std::mutex> lock(mu_);
  ApplyTradeLocked(symbol, trade);
}

void SimOrderBackend::OnMarketBook(const std::string& symbol, const TopOfBook& book,
                                   std::int64_t /*ts_us*/) {
  std::lock_guard<std::mutex> lock(mu_);
  FlushPendingBookLocked();  // a genuine prior book (no trade followed) applies in order
  if (book.quote_ts_us > last_ts_us_) last_ts_us_ = book.quote_ts_us;
  pending_book_ = book;
  pending_book_symbol_ = symbol;
  has_pending_book_ = true;
}

void SimOrderBackend::OnMarketTrade(const std::string& symbol, const Trade& trade,
                                    std::int64_t /*ts_us*/) {
  std::lock_guard<std::mutex> lock(mu_);
  ApplyTradeLocked(symbol, trade);  // trade first, then its post-trade depth
  FlushPendingBookLocked();
}

void SimOrderBackend::Finalize() {
  std::lock_guard<std::mutex> lock(mu_);
  FlushPendingBookLocked();
  engine_.Finalize();
}

}  // namespace kairos::exec
