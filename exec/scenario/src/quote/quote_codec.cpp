#include "quote_codec.h"

#include <capnp/message.h>
#include <capnp/serialize.h>

#include <chrono>
#include <cstring>

#include "kairos.capnp.h"

namespace kairos::exec {

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
    t.recv_ts = std::chrono::steady_clock::now();
    t.valid = true;

    *tob = t;
    *symbol = q.getSymbol().cStr();
    return true;
  } catch (...) {
    return false;
  }
}

}  // namespace kairos::exec
