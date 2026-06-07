#include "feed.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include <toml++/toml.h>

#include "publisher.h"
#include "quote_encode.h"
#include "ticker.h"

namespace kairos::concords {
namespace {

std::atomic<bool> g_stop{false};
void OnSig(int) { g_stop = true; }

std::chrono::steady_clock::time_point g_last_req{};
void TickerGate() {
  auto now = std::chrono::steady_clock::now();
  if (g_last_req.time_since_epoch().count() != 0) {
    auto since = now - g_last_req;
    if (since < std::chrono::seconds(1)) {
      std::this_thread::sleep_for(std::chrono::seconds(1) - since);
    }
  }
  g_last_req = std::chrono::steady_clock::now();
}

int ParseHhmm(const std::string& s, int fallback) {
  int h = -1, m = -1;
  if (std::sscanf(s.c_str(), "%d:%d", &h, &m) == 2 && h >= 0 && h < 24 && m >= 0 && m < 60) {
    return h * 100 + m;
  }
  return fallback;
}

struct LocalNow {
  int hhmm;
  long day;
};
LocalNow NowUtc8() {
  auto utc8 = std::chrono::system_clock::now() + std::chrono::hours(8);
  auto dp = std::chrono::floor<std::chrono::days>(utc8);
  std::chrono::hh_mm_ss hms{utc8 - dp};
  return {static_cast<int>(hms.hours().count()) * 100 + static_cast<int>(hms.minutes().count()),
          dp.time_since_epoch().count()};
}
bool DailyReconnectDue(int reconnect_hhmm, long& last_day) {
  LocalNow n = NowUtc8();
  if (n.hhmm >= reconnect_hhmm && last_day != n.day) {
    last_day = n.day;
    return true;
  }
  return false;
}

Exchange MapExchange(concords_sdk::ticker::Exchange e) {
  switch (e) {
    case concords_sdk::ticker::Exchange::kTWSE:
      return Exchange::kTwse;
    case concords_sdk::ticker::Exchange::kTPEX:
      return Exchange::kTpex;
    case concords_sdk::ticker::Exchange::kTFX:
      return Exchange::kTfx;
    case concords_sdk::ticker::Exchange::kOTC:
      return Exchange::kOtc;
  }
  return Exchange::kTwse;
}

void FillLevels(std::vector<Level>& out, const concords_sdk::ticker::Quotation& q, bool bid) {
  std::size_t n = bid ? q.GetBidSize() : q.GetAskSize();
  if (n > 5) n = 5;
  for (std::size_t i = 0; i < n; ++i) {
    concords_sdk::ticker::PriceVolume pv = bid ? q.GetBid(i) : q.GetAsk(i);
    out.push_back({static_cast<std::int64_t>(pv.price.digits),
                   static_cast<std::uint8_t>(pv.price.precision),
                   static_cast<std::int64_t>(pv.volume)});
  }
}

Quote ToQuote(const concords_sdk::ticker::Quotation& q, const char* pid) {
  Quote out;
  out.symbol = pid;
  out.exchange = MapExchange(q.GetExchange());
  out.quote_ts_us = std::chrono::duration_cast<std::chrono::microseconds>(
                        q.GetTimestamp().time_since_epoch())
                        .count();
  FillLevels(out.bids, q, true);
  FillLevels(out.asks, q, false);
  if (q.GetTradeSize() > 0) {
    concords_sdk::ticker::PriceVolume tr = q.GetTrade(0);
    out.last_price = static_cast<std::int64_t>(tr.price.digits);
    out.last_scale = static_cast<std::uint8_t>(tr.price.precision);
    out.last_volume = static_cast<std::int64_t>(tr.volume);
  } else {
    out.last_price = 0;
    out.last_scale = 0;
    out.last_volume = 0;
  }
  out.is_trial = q.IsTrial();
  return out;
}

}  // namespace

int RunFeed(const std::string& config_path) {
  std::string user_id, password, pfx, aeron_dir;
  std::vector<std::string> symbols;
  int reconnect_hhmm = 705;
  std::int32_t stream_id = 1001;
  try {
    auto t = toml::parse_file(config_path);
    user_id = t["user"]["user_id"].value_or<std::string>("");
    password = t["user"]["password"].value_or<std::string>("");
    pfx = t["user"]["pfx_filepath"].value_or<std::string>("");
    reconnect_hhmm = ParseHhmm(t["reconnect"]["daily_at"].value_or<std::string>("07:05"), 705);
    stream_id = static_cast<std::int32_t>(t["aeron"]["stream_id"].value_or<std::int64_t>(1001));
    aeron_dir = t["aeron"]["dir"].value_or<std::string>("");
    if (auto arr = t["feed"]["symbols"].as_array()) {
      for (auto&& e : *arr) {
        if (auto s = e.value<std::string>()) symbols.push_back(*s);
      }
    }
  } catch (const std::exception& e) {
    std::cerr << "failed to parse " << config_path << ": " << e.what() << "\n";
    return 1;
  }
  if (user_id.empty() || pfx.empty() || symbols.empty()) {
    std::cerr << "config needs [user] user_id/pfx_filepath and [feed] symbols\n";
    return 1;
  }

  Publisher publisher(aeron_dir, stream_id);
  std::unique_ptr<concords_sdk::ticker::Ticker> ticker;

  auto build = [&]() -> bool {
    TickerGate();
    ticker = concords_sdk::ticker::BuildTicker(user_id.c_str(), password.c_str(), pfx.c_str());
    if (!ticker) {
      std::cerr << "BuildTicker failed (credentials?)\n";
      return false;
    }
    ticker->SetErrorCallback(
        [](const std::string& e) { std::cerr << "ticker error: " << e << "\n"; });
    ticker->SetQuotationCallback([&](const concords_sdk::ticker::Quotation& q) {
      const char* pid = q.GetProductId();
      if (!pid) return;
      publisher.Offer(EncodeQuoteEnvelope(ToQuote(q, pid)));
    });
    for (const auto& s : symbols) {
      TickerGate();
      ticker->Subscribe(s.c_str());
    }
    return true;
  };

  if (!build()) return 1;
  std::signal(SIGINT, OnSig);
  std::signal(SIGTERM, OnSig);
  std::cout << "kairos-sidecar: concords feed live, " << symbols.size()
            << " symbols -> aeron:ipc stream " << stream_id << "\n";

  long last_reconnect_day = NowUtc8().hhmm >= reconnect_hhmm ? NowUtc8().day : NowUtc8().day - 1;
  while (!g_stop) {
    if (DailyReconnectDue(reconnect_hhmm, last_reconnect_day)) {
      std::cout << "kairos-sidecar: daily reconnect\n";
      if (ticker) {
        ticker->ClearQuotationCallback();
        for (const auto& s : symbols) {
          TickerGate();
          ticker->Unsubscribe(s.c_str());
        }
      }
      build();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  std::cout << "kairos-sidecar: shutting down\n";
  return 0;
}

}  // namespace kairos::concords
