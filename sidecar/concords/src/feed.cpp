#include "feed.h"

#include <sys/stat.h>
#include <toml++/toml.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <memory>
#include <set>
#include <thread>
#include <vector>

#include "control_sub.h"
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
  std::int64_t day;
};
LocalNow NowUtc8() {
  auto utc8 = std::chrono::system_clock::now() + std::chrono::hours(8);
  auto dp = std::chrono::floor<std::chrono::days>(utc8);
  std::chrono::hh_mm_ss hms{utc8 - dp};
  return {static_cast<int>(hms.hours().count()) * 100 + static_cast<int>(hms.minutes().count()),
          dp.time_since_epoch().count()};
}
bool DailyReconnectDue(int reconnect_hhmm, std::int64_t& last_day) {
  LocalNow n = NowUtc8();
  if (n.hhmm >= reconnect_hhmm && last_day != n.day) {
    last_day = n.day;
    return true;
  }
  return false;
}

long long SteadyMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::int64_t RealtimeUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
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

Quote ToQuote(const concords_sdk::ticker::Quotation& q, const char* pid, std::int64_t recv_ts_us) {
  Quote out;
  out.symbol = pid;
  out.exchange = MapExchange(q.GetExchange());
  out.quote_ts_us =
      std::chrono::duration_cast<std::chrono::microseconds>(q.GetTimestamp().time_since_epoch())
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
  out.source = 0;  // concords
  out.recv_ts_us = recv_ts_us;
  out.board = QuoteBoard::kRoundLot;  // concords equity feed is round-lot
  return out;
}

// One combined Quotation carries a trade when GetTradeSize()>0; split it into an
// authoritative Trade event alongside the depth-bearing Quote.
Trade ToTrade(const concords_sdk::ticker::Quotation& q, const char* pid, std::int64_t recv_ts_us) {
  Trade out;
  out.symbol = pid;
  out.exchange = MapExchange(q.GetExchange());
  out.source = 0;  // concords
  out.trade_ts_us =
      std::chrono::duration_cast<std::chrono::microseconds>(q.GetTimestamp().time_since_epoch())
          .count();
  out.recv_ts_us = recv_ts_us;
  concords_sdk::ticker::PriceVolume tr = q.GetTrade(0);
  out.price_mantissa = static_cast<std::int64_t>(tr.price.digits);
  out.price_scale = static_cast<std::uint8_t>(tr.price.precision);
  out.volume = static_cast<std::int64_t>(tr.volume);
  out.is_trial = q.IsTrial();
  return out;
}

// Per-connection state captured by value into the ticker callback, so a leaked
// frozen ticker writes to its own orphaned session instead of racing the rebuilt
// one's counters / staleness clock. The seq/epoch tracker lives here too so a
// leaked session keeps its own counters.
struct FeedSession {
  Publisher* publisher = nullptr;
  std::atomic<long long> last_quote_ms{0};
  long long offer_dropped = 0;  // touched only by this session's callback thread
  long long offer_warn_ms = 0;
  SeqEpochTracker seq_epoch;  // touched only by this session's callback thread
};

}  // namespace

int RunFeed(const std::string& config_path) {
  // sidecar.toml holds broker credentials — warn if it is group/other-accessible.
  struct stat cfg_st;
  if (::stat(config_path.c_str(), &cfg_st) == 0 && (cfg_st.st_mode & 0077) != 0) {
    std::cerr << "kairos-sidecar: WARNING: " << config_path
              << " is group/other-accessible; it holds broker credentials — chmod 600 it\n";
  }

  std::string user_id, password, pfx, aeron_dir;
  std::vector<std::string> symbols;
  int reconnect_hhmm = 705;
  int stale_restart_s = 30;
  std::int32_t stream_id = 1001;
  try {
    auto t = toml::parse_file(config_path);
    user_id = t["user"]["user_id"].value_or<std::string>("");
    password = t["user"]["password"].value_or<std::string>("");
    pfx = t["user"]["pfx_filepath"].value_or<std::string>("");
    reconnect_hhmm = ParseHhmm(t["reconnect"]["daily_at"].value_or<std::string>("07:05"), 705);
    stale_restart_s = static_cast<int>(t["feed"]["stale_restart_s"].value_or<std::int64_t>(30));
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

  std::unique_ptr<Publisher> publisher;
  try {
    publisher = std::make_unique<Publisher>(aeron_dir, stream_id);
  } catch (const std::exception& e) {
    std::cerr << "kairos-sidecar: failed to connect to Aeron media driver: " << e.what() << "\n";
    return 1;
  }
  std::unique_ptr<ControlSub> control;
  try {
    control = std::make_unique<ControlSub>(aeron_dir, stream_id + 1);
  } catch (const std::exception& e) {
    std::cerr << "kairos-sidecar: failed to open subscription control stream: " << e.what() << "\n";
    return 1;
  }
  std::unique_ptr<concords_sdk::ticker::Ticker> ticker;

  // warm set: always-subscribed floor that keeps the concords ticker fed past
  // its ~30s idle disconnect. core drives extra on-demand symbols via stream 1002.
  std::set<std::string> warm(symbols.begin(), symbols.end());
  std::set<std::string> subscribed;    // what we currently hold on concords
  std::set<std::string> core_desired;  // latest on-demand set from core
  std::shared_ptr<FeedSession> session;

  auto build = [&]() -> bool {
    TickerGate();
    ticker = concords_sdk::ticker::BuildTicker(user_id.c_str(), password.c_str(), pfx.c_str());
    if (!ticker) {
      std::cerr << "kairos-sidecar: failed to build concords ticker (check credentials/PFX)\n";
      return false;
    }
    ticker->SetErrorCallback([](const std::string& e) {
      std::cerr << "kairos-sidecar: concords ticker error: " << e << "\n";
    });
    auto sess = std::make_shared<FeedSession>();
    sess->publisher = publisher.get();
    sess->seq_epoch.Rebuild();  // new feed session -> next epoch, seq resets per symbol
    ticker->SetQuotationCallback([sess](const concords_sdk::ticker::Quotation& q) {
      const char* pid = q.GetProductId();
      if (!pid) return;
      sess->last_quote_ms.store(
          SteadyMs());  // SDK delivered (alive); update before the Aeron offer
      std::int64_t recv_ts_us = RealtimeUs();
      std::uint32_t epoch = sess->seq_epoch.Epoch();
      // Quote and Trade for one symbol share the per-symbol seq space so a
      // full-stream consumer detects any loss.
      Quote quote = ToQuote(q, pid, recv_ts_us);
      quote.seq = sess->seq_epoch.NextSeq(pid);
      quote.epoch = epoch;
      std::int64_t r = sess->publisher->Offer(EncodeQuoteEnvelope(quote));
      if (q.GetTradeSize() > 0) {
        Trade trade = ToTrade(q, pid, recv_ts_us);
        trade.seq = sess->seq_epoch.NextSeq(pid);
        trade.epoch = epoch;
        sess->publisher->Offer(EncodeTradeEnvelope(trade));
      }
      if (r < 0) {
        ++sess->offer_dropped;
        long long nowms = SteadyMs();
        if (nowms - sess->offer_warn_ms > 5000) {  // throttle: at most once / 5s
          std::cerr << "kairos-sidecar: dropped " << sess->offer_dropped
                    << " quotes (aeron offer=" << r
                    << "; core not connected / publication issue)\n";
          sess->offer_warn_ms = nowms;
        }
      }
    });
    subscribed.clear();
    for (const auto& s : warm) {
      TickerGate();
      if (!ticker->Subscribe(s.c_str())) {
        std::cerr << "kairos-sidecar: warm subscribe failed: " << s << " (limit / bad symbol?)\n";
      }
      subscribed.insert(s);
    }
    sess->last_quote_ms.store(SteadyMs());  // start the staleness clock after the warm burst
    session = sess;  // publish to the main loop only after the ticker is fully wired
    return true;
  };

  // Reconcile concords subscriptions toward (warm ∪ core_desired). SDK calls
  // only fire on an actual diff; warm is never unsubscribed (target ⊇ warm).
  auto reconcile = [&]() {
    std::set<std::string> target = warm;
    target.insert(core_desired.begin(), core_desired.end());
    for (const auto& s : target) {
      if (subscribed.insert(s).second) {
        TickerGate();
        if (ticker->Subscribe(s.c_str())) {
          std::cout << "kairos-sidecar: subscribe " << s << " (on demand)\n";
        } else {
          std::cerr << "kairos-sidecar: subscribe failed: " << s << " (limit / bad symbol?)\n";
        }
      }
    }
    for (auto it = subscribed.begin(); it != subscribed.end();) {
      if (target.count(*it) == 0) {
        TickerGate();
        ticker->Unsubscribe(it->c_str());
        std::cout << "kairos-sidecar: unsubscribe " << *it << " (no subscribers)\n";
        it = subscribed.erase(it);
      } else {
        ++it;
      }
    }
  };

  // Tear down the old ticker and rebuild, retrying with backoff. drop_stuck=true
  // (staleness-triggered) means the SDK is frozen: calling Unsubscribe or running
  // its destructor would deadlock, so we release (leak) it and never touch it again
  // — its connection drops on the broker's idle timeout, freeing the account.
  auto reconnect = [&](bool drop_stuck) {
    if (drop_stuck) {
      (void)ticker.release();
    } else if (ticker) {
      ticker->ClearQuotationCallback();
      for (const auto& s : subscribed) {
        TickerGate();
        ticker->Unsubscribe(s.c_str());
      }
    }
    auto backoff = std::chrono::seconds(1);
    while (!build() && !g_stop) {
      std::cerr << "kairos-sidecar: concords reconnect failed; retrying in " << backoff.count()
                << "s\n";
      auto until = std::chrono::steady_clock::now() + backoff;
      while (std::chrono::steady_clock::now() < until && !g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      backoff = std::min(backoff * 2, std::chrono::seconds(30));
    }
  };

  if (!build()) return 1;
  std::signal(SIGINT, OnSig);
  std::signal(SIGTERM, OnSig);
  std::cout << "kairos-sidecar: concords feed live, " << symbols.size()
            << " symbols -> aeron:ipc stream " << stream_id << "\n";

  LocalNow now = NowUtc8();
  std::int64_t last_reconnect_day = now.hhmm >= reconnect_hhmm ? now.day : now.day - 1;
  while (!g_stop) {
    if (DailyReconnectDue(reconnect_hhmm, last_reconnect_day)) {
      std::cout << "kairos-sidecar: daily reconnect\n";
      reconnect(false);
    }
    LocalNow watch = NowUtc8();
    long long stale_s = (SteadyMs() - session->last_quote_ms.load()) / 1000;
    if (FeedStale(watch.hhmm, stale_s, stale_restart_s)) {
      std::cout << "kairos-sidecar: quote feed stale " << stale_s << "s, reconnecting\n";
      reconnect(true);
    } else if (watch.hhmm < 830 || watch.hhmm >= 1330) {
      session->last_quote_ms.store(SteadyMs());  // off-hours: no quotes expected, keep clock fresh
    }
    std::vector<std::string> latest;
    if (control->Poll(&latest)) {
      core_desired = std::set<std::string>(latest.begin(), latest.end());
    }
    reconcile();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  std::cout << "kairos-sidecar: shutting down\n";
  return 0;
}

}  // namespace kairos::concords
