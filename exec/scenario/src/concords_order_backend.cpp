#include "concords_order_backend.h"

#include <cstdio>
#include <thread>
#include <utility>

#include "order_parsers.h"  // parseFundingType / parseTimeInForce
#include "tw_market.h"      // CentsToString / FloatToCents

namespace kairos::exec {

namespace {

concords_sdk::stock::Market ToMarket(Market m) {
  return m == Market::kTse ? concords_sdk::stock::Market::TSE : concords_sdk::stock::Market::OTC;
}
concords_sdk::stock::OrderBoard ToBoard(Board b) {
  return b == Board::kOddLot ? concords_sdk::stock::OrderBoard::OddLot
                             : concords_sdk::stock::OrderBoard::RoundLot;
}
concords_sdk::stock::Side ToSide(Side s) {
  return s == Side::kBuy ? concords_sdk::stock::Side::Buy : concords_sdk::stock::Side::Sell;
}

}  // namespace

ConcordsOrderBackend::ConcordsOrderBackend(Scenario s) : s_(std::move(s)) {}

void ConcordsOrderBackend::Gate() {
  std::lock_guard<std::mutex> lock(gate_mu_);
  auto now = std::chrono::steady_clock::now();
  if (last_req_.time_since_epoch().count() != 0) {
    auto since = now - last_req_;
    if (since < std::chrono::seconds(1))
      std::this_thread::sleep_for(std::chrono::seconds(1) - since);
  }
  last_req_ = std::chrono::steady_clock::now();
}

bool ConcordsOrderBackend::Connect() {
  Gate();
  stock_ = concords_sdk::stock::BuildStockClient(
      s_.creds.user_id.c_str(), s_.creds.password.c_str(), s_.creds.account.c_str(),
      s_.creds.pfx_filepath.c_str(), s_.creds.pfx_password.c_str());
  if (!stock_) {
    std::fprintf(stderr, "kairos-exec: BuildStockClient failed (credentials/PFX?)\n");
    return false;
  }
  stock_->SetConnectionStateCallback(
      [](concords_sdk::stock::ConnectionState, const std::string& err) {
        if (!err.empty()) std::fprintf(stderr, "kairos-exec: order link: %s\n", err.c_str());
      });
  stock_->SetOrderSubmitCallback([this](const concords_sdk::stock::OrderSubmitResult& r) {
    if (on_ack_) on_ack_(r.user_defined_id, r.success, r.error_message);
  });
  stock_->SetOrderFillCallback([this](const concords_sdk::stock::OrderFillResult& r) {
    long shares = 0;
    double price = 0.0;
    try {
      shares = std::stol(r.quantity);
      price = std::stod(r.price);
    } catch (...) {
      return;
    }
    if (shares > 0 && price > 0.0 && on_fill_)
      on_fill_(r.user_defined_id, Fill{shares, FloatToCents(price)});
  });
  stock_->SetOrderCancelCallback([this](const concords_sdk::stock::OrderCancelResult& r) {
    if (on_cancel_) on_cancel_(r.target_id, r.success);
  });
  Gate();
  return stock_->Connect();
}

void ConcordsOrderBackend::Disconnect() {
  if (stock_ && stock_->IsConnected()) {
    Gate();
    stock_->Disconnect();
  }
}

bool ConcordsOrderBackend::IsConnected() const { return stock_ && stock_->IsConnected(); }

void ConcordsOrderBackend::Submit(const std::string& id, Side side, Cents price, long shares) {
  std::string qty =
      std::to_string(s_.IsOddLot() ? shares : shares / 1000);  // odd=shares, round=lots
  concords_sdk::stock::OrderInfo order(
      ToMarket(s_.market), ToBoard(s_.board),
      concords_sdk::stock::parseFundingType(s_.funding_type), s_.symbol, ToSide(side),
      concords_sdk::stock::OrderType::Limit,
      concords_sdk::stock::parseTimeInForce(s_.time_in_force), qty, CentsToString(price),
      concords_sdk::stock::DaytradeShortSell::False, id);
  Gate();
  stock_->SubmitOrder(order);
}

void ConcordsOrderBackend::UpdatePrice(const std::string& id, Cents new_price) {
  Gate();
  stock_->UpdateOrderPrice(id, id, CentsToString(new_price));
}

void ConcordsOrderBackend::Cancel(const std::string& id) {
  Gate();
  stock_->CancelOrder(id);
}

std::unique_ptr<OrderBackend> MakeLiveBackend(const Scenario& s) {
  return std::make_unique<ConcordsOrderBackend>(s);
}

}  // namespace kairos::exec
