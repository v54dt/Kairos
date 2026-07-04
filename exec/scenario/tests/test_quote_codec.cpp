// Self-test for the wire codec: mantissa/scale -> cents, Subscribe encode,
// Quote -> TopOfBook decode. No socket, no broker.

#include <capnp/message.h>
#include <capnp/serialize.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "kairos.capnp.h"
#include "quote_codec.h"

using namespace kairos::exec;

static int g_failures = 0;

#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                \
    }                                                              \
  } while (0)

#define CHECK_EQ(a, b)                                                                   \
  do {                                                                                   \
    auto _a = (a);                                                                       \
    auto _b = (b);                                                                       \
    if (!(_a == _b)) {                                                                   \
      std::printf("FAIL  %s:%d  %s == %s  (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, \
                  (long long)_a, (long long)_b);                                         \
      ++g_failures;                                                                      \
    }                                                                                    \
  } while (0)

static std::vector<std::uint8_t> BuildQuote() {
  capnp::MallocMessageBuilder msg;
  auto q = msg.initRoot<Envelope>().initQuote();
  q.setSymbol("2330");
  q.setQuoteTsUs(1700000000000000LL);
  auto bids = q.initBids(2);
  bids[0].setPriceMantissa(58000);  // 580.00
  bids[0].setPriceScale(2);
  bids[0].setVolume(100);
  bids[1].setPriceMantissa(57950);
  bids[1].setPriceScale(2);
  bids[1].setVolume(50);
  auto asks = q.initAsks(1);
  asks[0].setPriceMantissa(58100);  // 581.00
  asks[0].setPriceScale(2);
  asks[0].setVolume(80);
  q.setLastPrice(58050);
  q.setLastScale(2);
  q.setLastVolume(10);
  q.setIsTrial(true);
  auto flat = capnp::messageToFlatArray(msg);
  auto bytes = flat.asBytes();
  return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
}

int main() {
  // mantissa/scale -> cents (cents = mantissa * 10^(2-scale))
  CHECK_EQ(MantissaScaleToCents(58000, 2), 58000);   // 580.00
  CHECK_EQ(MantissaScaleToCents(580, 0), 58000);     // 580 -> 58000 cents
  CHECK_EQ(MantissaScaleToCents(5805, 1), 58050);    // 580.5
  CHECK_EQ(MantissaScaleToCents(580500, 3), 58050);  // 580.500 truncates

  // Subscribe encodes a readable Envelope.subscribe
  {
    auto bytes = EncodeSubscribe({"2330", "0050"});
    CHECK(bytes.size() % sizeof(capnp::word) == 0);
    auto words = kj::arrayPtr(reinterpret_cast<const capnp::word*>(bytes.data()),
                              bytes.size() / sizeof(capnp::word));
    capnp::FlatArrayMessageReader reader(words);
    auto env = reader.getRoot<Envelope>();
    CHECK(env.which() == Envelope::SUBSCRIBE);
    auto syms = env.getSubscribe().getSymbols();
    CHECK_EQ(syms.size(), 2u);
    CHECK(std::string(syms[0].cStr()) == "2330");
    CHECK(std::string(syms[1].cStr()) == "0050");
  }

  // Quote decodes into a TopOfBook
  {
    auto bytes = BuildQuote();
    TopOfBook tob;
    std::string symbol;
    CHECK(DecodeQuote(bytes.data(), bytes.size(), &tob, &symbol));
    CHECK(symbol == "2330");
    CHECK_EQ(tob.best_bid(), 58000);
    CHECK_EQ(tob.best_ask(), 58100);
    CHECK_EQ(tob.last_trade, 58050);
    CHECK_EQ(tob.last_vol, 10);
    CHECK(tob.is_trial);
    CHECK(tob.valid);
    CHECK(tob.HasTwoSided());
    CHECK_EQ(tob.n_bids, 2);
    CHECK_EQ(tob.bids[0].price, 58000);
    CHECK_EQ(tob.bids[0].volume, 100);
    CHECK_EQ(tob.bids[1].price, 57950);
    CHECK_EQ(tob.bids[1].volume, 50);
    CHECK_EQ(tob.n_asks, 1);
    CHECK_EQ(tob.asks[0].price, 58100);
  }

  // A subscribe envelope is not a quote
  {
    auto bytes = EncodeSubscribe({"2330"});
    TopOfBook tob;
    std::string symbol;
    CHECK(!DecodeQuote(bytes.data(), bytes.size(), &tob, &symbol));
  }

  // A Trade and a heartbeat are well-formed non-quote variants: DecodeQuote must
  // return false without throwing, and each must bump the unknown-variant count.
  {
    std::uint64_t before = UnknownVariantCount();

    capnp::MallocMessageBuilder trade_msg;
    auto tr = trade_msg.initRoot<Envelope>().initTrade();
    tr.setSymbol("2330");
    tr.setPriceMantissa(58050);
    tr.setPriceScale(2);
    tr.setVolume(3);
    auto tflat = capnp::messageToFlatArray(trade_msg);
    auto tbytes = tflat.asBytes();
    TopOfBook tob;
    std::string symbol;
    CHECK(!DecodeQuote(tbytes.begin(), tbytes.size(), &tob, &symbol));

    capnp::MallocMessageBuilder hb_msg;
    hb_msg.initRoot<Envelope>().setHeartbeat();
    auto hflat = capnp::messageToFlatArray(hb_msg);
    auto hbytes = hflat.asBytes();
    CHECK(!DecodeQuote(hbytes.begin(), hbytes.size(), &tob, &symbol));

    CHECK_EQ(UnknownVariantCount() - before, 2u);
  }

  if (g_failures == 0) {
    std::printf("test_quote_codec: OK\n");
    return 0;
  }
  std::printf("test_quote_codec: FAILED %d check(s)\n", g_failures);
  return 1;
}
