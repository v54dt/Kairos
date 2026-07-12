#include "quote_codec.h"

#include <capnp/message.h>
#include <capnp/serialize.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>

#include "kairos.capnp.h"
#include "time_util.h"  // SteadyNowMs

namespace kairos::exec {
namespace {

std::atomic<std::uint64_t> g_unknown_variants{0};
std::atomic<long long> g_unknown_warn_ms{0};

// A well-formed Envelope this consumer does not handle (Trade/subAck/error/...).
// Count it and warn at most once per 5s, rather than silently dropping it.
void NoteUnknownVariant() {
  std::uint64_t total = g_unknown_variants.fetch_add(1, std::memory_order_relaxed) + 1;
  long long now = SteadyNowMs();
  long long last = g_unknown_warn_ms.load(std::memory_order_relaxed);
  if (now - last > 5000 && g_unknown_warn_ms.compare_exchange_strong(last, now)) {
    std::cerr << "kairos-exec: ignored " << total
              << " non-quote envelopes on the quote stream (trade/control frames)\n";
  }
}

}  // namespace

Cents MantissaScaleToCents(std::int64_t mantissa, std::uint8_t scale) {
  if (scale <= 2) {
    Cents mul = 1;
    for (int i = 0; i < 2 - scale; ++i) mul *= 10;
    return mantissa * mul;
  }
  Cents div = 1;
  for (int i = 0; i < scale - 2; ++i) div *= 10;
  return mantissa / div;
}

std::vector<std::uint8_t> EncodeSubscribe(const std::vector<std::string>& symbols) {
  capnp::MallocMessageBuilder msg;
  auto sub = msg.initRoot<Envelope>().initSubscribe();
  auto list = sub.initSymbols(static_cast<unsigned>(symbols.size()));
  for (unsigned i = 0; i < symbols.size(); ++i) {
    list.set(i, symbols[i].c_str());
  }
  auto flat = capnp::messageToFlatArray(msg);
  auto bytes = flat.asBytes();
  return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
}

bool DecodeQuote(const std::uint8_t* data, std::size_t len, TopOfBook* tob, std::string* symbol) {
  if (data == nullptr || len == 0 || len % sizeof(capnp::word) != 0) {
    return false;
  }
  try {
    std::vector<capnp::word> words(len / sizeof(capnp::word));
    std::memcpy(words.data(), data, len);
    capnp::FlatArrayMessageReader reader(kj::arrayPtr(words.data(), words.size()));
    auto env = reader.getRoot<Envelope>();
    if (env.which() != Envelope::QUOTE) {
      NoteUnknownVariant();
      return false;
    }
    auto q = env.getQuote();

    TopOfBook t;
    for (auto b : q.getBids()) {
      if (t.n_bids >= TopOfBook::kMaxLevels) break;
      t.bids[t.n_bids++] = {MantissaScaleToCents(b.getPriceMantissa(), b.getPriceScale()),
                            static_cast<long>(b.getVolume())};
    }
    for (auto a : q.getAsks()) {
      if (t.n_asks >= TopOfBook::kMaxLevels) break;
      t.asks[t.n_asks++] = {MantissaScaleToCents(a.getPriceMantissa(), a.getPriceScale()),
                            static_cast<long>(a.getVolume())};
    }
    t.last_trade = MantissaScaleToCents(q.getLastPrice(), q.getLastScale());
    t.last_vol = static_cast<long>(q.getLastVolume());
    t.is_trial = q.getIsTrial();
    t.quote_ts_us = q.getQuoteTsUs();
    t.recv_ts = std::chrono::steady_clock::now();
    t.valid = true;

    *tob = t;
    *symbol = q.getSymbol().cStr();
    return true;
  } catch (...) {
    return false;
  }
}

bool DecodeTrade(const std::uint8_t* data, std::size_t len, Trade* trade, std::string* symbol) {
  if (data == nullptr || len == 0 || len % sizeof(capnp::word) != 0) {
    return false;
  }
  try {
    std::vector<capnp::word> words(len / sizeof(capnp::word));
    std::memcpy(words.data(), data, len);
    capnp::FlatArrayMessageReader reader(kj::arrayPtr(words.data(), words.size()));
    auto env = reader.getRoot<Envelope>();
    if (env.which() != Envelope::TRADE) return false;
    auto tr = env.getTrade();

    Trade t;
    t.price = MantissaScaleToCents(tr.getPriceMantissa(), tr.getPriceScale());
    t.volume = static_cast<long>(tr.getVolume());
    t.trade_ts_us = tr.getTradeTsUs();
    t.is_trial = tr.getIsTrial();

    *trade = t;
    *symbol = tr.getSymbol().cStr();
    return true;
  } catch (...) {
    return false;
  }
}

std::uint64_t UnknownVariantCount() { return g_unknown_variants.load(std::memory_order_relaxed); }

}  // namespace kairos::exec
