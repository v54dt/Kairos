#include "fill_engine.h"

#include <utility>

namespace kairos::exec {

FillEngine::FillEngine(FillMode mode, SimAckFn ack, SimFillFn fill, SimCancelFn cancel)
    : mode_(mode),
      on_ack_(std::move(ack)),
      on_fill_(std::move(fill)),
      on_cancel_(std::move(cancel)) {}

void FillEngine::AddSymbol(const std::string& symbol) {
  if (models_.count(symbol)) return;
  models_[symbol] = std::make_unique<SymbolFillModel>(
      symbol, mode_,
      [this](const std::string& id, bool ok, const std::string& e) {
        if (on_ack_) on_ack_(id, ok, e);
      },
      [this](const std::string& id, const Fill& f) { OnModelFill(id, f); },
      [this](const std::string& id, bool ok) {
        if (ok) {
          id_symbol_.erase(id);
          id_remaining_.erase(id);
        }
        if (on_cancel_) on_cancel_(id, ok);
      });
}

void FillEngine::OnModelFill(const std::string& id, const Fill& f) {
  auto it = id_remaining_.find(id);
  if (it != id_remaining_.end()) {
    it->second -= f.shares;
    if (it->second <= 0) {
      id_remaining_.erase(it);
      id_symbol_.erase(id);
    }
  }
  if (on_fill_) on_fill_(id, f);
}

void FillEngine::OnBook(const std::string& symbol, const TopOfBook& book, std::int64_t ts_us) {
  auto it = models_.find(symbol);
  if (it != models_.end()) it->second->OnBook(book, ts_us);
}

void FillEngine::OnTrade(const std::string& symbol, Cents price, long vol, std::int64_t ts_us,
                         bool is_trial) {
  auto it = models_.find(symbol);
  if (it != models_.end()) it->second->OnTrade(price, vol, ts_us, is_trial);
}

void FillEngine::Submit(const SimOrder& order) {
  if (order.board == Board::kOddLot) {
    if (on_ack_) on_ack_(order.id, false, "odd-lot board not supported");
    return;
  }
  auto it = models_.find(order.symbol);
  if (it == models_.end()) {
    if (on_ack_) on_ack_(order.id, false, "unknown symbol");
    return;
  }
  id_symbol_[order.id] = order.symbol;
  id_remaining_[order.id] = order.shares;
  it->second->Submit(order);
}

void FillEngine::Cancel(const std::string& id, std::int64_t ts_us) {
  auto it = id_symbol_.find(id);
  if (it == id_symbol_.end()) {
    if (on_cancel_) on_cancel_(id, false);  // unknown / already terminal
    return;
  }
  auto m = models_.find(it->second);
  if (m == models_.end() || !m->second->Cancel(id, ts_us)) {
    if (on_cancel_) on_cancel_(id, false);
  }
}

}  // namespace kairos::exec
