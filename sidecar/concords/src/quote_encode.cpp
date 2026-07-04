#include "quote_encode.h"

#include <capnp/message.h>
#include <capnp/serialize.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>

#include "kairos.capnp.h"

namespace kairos::concords {
namespace {

// Feed-session generation, shared by every FeedSession in the process: a fresh
// per-session tracker must not restart the epoch (that would make a benign
// rebuild indistinguishable from data loss). See schema/NORMALIZATION.md §3.
std::atomic<std::uint32_t> g_epoch{0};

// Advance to the next epoch, never below wall-clock seconds. recordd appends to
// one dated KQR file across a sidecar restart, so a restarted process must not
// reuse the previous process's epoch space; seeding from the clock keeps each
// session's epoch strictly greater than any a prior process could have used.
std::uint32_t NextEpoch() {
  auto now = static_cast<std::uint32_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  std::uint32_t prev = g_epoch.load(std::memory_order_relaxed);
  std::uint32_t next;
  do {
    next = std::max<std::uint32_t>(prev + 1, now);
  } while (!g_epoch.compare_exchange_weak(prev, next, std::memory_order_relaxed));
  return next;
}

::Exchange CapExchange(Exchange e) {
  switch (e) {
    case Exchange::kTwse:
      return ::Exchange::TWSE;
    case Exchange::kTpex:
      return ::Exchange::TPEX;
    case Exchange::kTfx:
      return ::Exchange::TFX;
    case Exchange::kOtc:
      return ::Exchange::OTC;
  }
  return ::Exchange::TWSE;
}

::QuoteBoard CapBoard(QuoteBoard b) {
  switch (b) {
    case QuoteBoard::kUnknown:
      return ::QuoteBoard::UNKNOWN;
    case QuoteBoard::kRoundLot:
      return ::QuoteBoard::ROUND_LOT;
    case QuoteBoard::kOddLot:
      return ::QuoteBoard::ODD_LOT;
  }
  return ::QuoteBoard::UNKNOWN;
}

::Session CapSession(Session s) {
  switch (s) {
    case Session::kUnknown:
      return ::Session::UNKNOWN;
    case Session::kDay:
      return ::Session::DAY;
    case Session::kNight:
      return ::Session::NIGHT;
  }
  return ::Session::UNKNOWN;
}

}  // namespace

std::vector<std::uint8_t> EncodeQuoteEnvelope(const Quote& q) {
  capnp::MallocMessageBuilder msg;
  auto quote = msg.initRoot<Envelope>().initQuote();
  quote.setSymbol(q.symbol.c_str());
  quote.setExchange(CapExchange(q.exchange));
  quote.setQuoteTsUs(q.quote_ts_us);

  auto bids = quote.initBids(static_cast<unsigned>(q.bids.size()));
  for (unsigned i = 0; i < q.bids.size(); ++i) {
    bids[i].setPriceMantissa(q.bids[i].price_mantissa);
    bids[i].setPriceScale(q.bids[i].price_scale);
    bids[i].setVolume(q.bids[i].volume);
  }
  auto asks = quote.initAsks(static_cast<unsigned>(q.asks.size()));
  for (unsigned i = 0; i < q.asks.size(); ++i) {
    asks[i].setPriceMantissa(q.asks[i].price_mantissa);
    asks[i].setPriceScale(q.asks[i].price_scale);
    asks[i].setVolume(q.asks[i].volume);
  }

  quote.setLastPrice(q.last_price);
  quote.setLastScale(q.last_scale);
  quote.setLastVolume(q.last_volume);
  quote.setIsTrial(q.is_trial);
  quote.setSource(q.source);
  quote.setSeq(q.seq);
  quote.setEpoch(q.epoch);
  quote.setRecvTsUs(q.recv_ts_us);
  quote.setBoard(CapBoard(q.board));
  quote.setSession(CapSession(q.session));
  quote.setTradingDate(q.trading_date);
  quote.setSimtrade(q.simtrade);
  quote.setUnderlyingPrice(q.underlying_price);

  auto flat = capnp::messageToFlatArray(msg);
  auto bytes = flat.asBytes();
  return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
}

std::vector<std::uint8_t> EncodeTradeEnvelope(const Trade& t) {
  capnp::MallocMessageBuilder msg;
  auto trade = msg.initRoot<Envelope>().initTrade();
  trade.setSymbol(t.symbol.c_str());
  trade.setExchange(CapExchange(t.exchange));
  trade.setSource(t.source);
  trade.setSeq(t.seq);
  trade.setEpoch(t.epoch);
  trade.setTradeTsUs(t.trade_ts_us);
  trade.setRecvTsUs(t.recv_ts_us);
  trade.setPriceMantissa(t.price_mantissa);
  trade.setPriceScale(t.price_scale);
  trade.setVolume(t.volume);
  trade.setIsTrial(t.is_trial);
  trade.setSession(CapSession(t.session));
  trade.setTradingDate(t.trading_date);
  trade.setSimtrade(t.simtrade);
  trade.setUnderlyingPrice(t.underlying_price);

  auto flat = capnp::messageToFlatArray(msg);
  auto bytes = flat.asBytes();
  return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
}

std::uint32_t SeqEpochTracker::Rebuild() {
  epoch_ = NextEpoch();
  seq_.clear();
  return epoch_;
}

std::uint64_t SeqEpochTracker::NextSeq(const std::string& symbol) { return ++seq_[symbol]; }

bool DecodeSubscribe(const std::uint8_t* data, std::size_t len, std::vector<std::string>* out) {
  if (data == nullptr || len == 0 || len % sizeof(capnp::word) != 0) {
    return false;
  }
  try {
    // Aeron fragments aren't guaranteed word-aligned; copy into aligned storage.
    std::vector<capnp::word> words(len / sizeof(capnp::word));
    std::memcpy(words.data(), data, len);
    capnp::FlatArrayMessageReader reader(kj::arrayPtr(words.data(), words.size()));
    auto env = reader.getRoot<Envelope>();
    if (env.which() != Envelope::SUBSCRIBE) {
      return false;
    }
    auto symbols = env.getSubscribe().getSymbols();
    out->clear();
    out->reserve(symbols.size());
    for (auto s : symbols) {
      out->emplace_back(s.cStr());
    }
    return true;
  } catch (...) {
    return false;
  }
}

}  // namespace kairos::concords
