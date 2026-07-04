#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace kairos::concords {

enum class Exchange { kTwse, kTpex, kTfx, kOtc };

// Quote-side board marker; mirrors schema `QuoteBoard` (kUnknown @0).
enum class QuoteBoard { kUnknown, kRoundLot, kOddLot };

// Trading session; mirrors schema `Session` (kUnknown @0). Populated by E1/E2.
enum class Session { kUnknown, kDay, kNight };

struct Level {
  std::int64_t price_mantissa;
  std::uint8_t price_scale;
  std::int64_t volume;
};

struct Quote {
  std::string symbol;
  Exchange exchange = Exchange::kTwse;
  std::int64_t quote_ts_us = 0;
  std::vector<Level> bids;
  std::vector<Level> asks;
  // Best-effort last-trade snapshot kept for back-compat; authoritative trades
  // are Trade events.
  std::int64_t last_price = 0;
  std::uint8_t last_scale = 0;
  std::int64_t last_volume = 0;
  bool is_trial = false;
  // A2 fields (default 0/unknown so an unset field matches the capnp default).
  std::uint16_t source = 0;
  std::uint64_t seq = 0;
  std::uint32_t epoch = 0;
  std::int64_t recv_ts_us = 0;
  QuoteBoard board = QuoteBoard::kUnknown;
  Session session = Session::kUnknown;
  std::uint32_t trading_date = 0;
  bool simtrade = false;
  std::int64_t underlying_price = 0;
};

// A standalone TRADE event, separate from the depth-bearing Quote.
struct Trade {
  std::string symbol;
  Exchange exchange = Exchange::kTwse;
  std::uint16_t source = 0;
  std::uint64_t seq = 0;
  std::uint32_t epoch = 0;
  std::int64_t trade_ts_us = 0;
  std::int64_t recv_ts_us = 0;
  std::int64_t price_mantissa = 0;
  std::uint8_t price_scale = 0;
  std::int64_t volume = 0;
  bool is_trial = false;
  Session session = Session::kUnknown;
  std::uint32_t trading_date = 0;
  bool simtrade = false;
  std::int64_t underlying_price = 0;
};

std::vector<std::uint8_t> EncodeQuoteEnvelope(const Quote& q);
std::vector<std::uint8_t> EncodeTradeEnvelope(const Trade& t);

// Assigns per-(symbol) monotonic seq numbers within a feed session and bumps the
// epoch on every session rebuild. The epoch is process-global (shared by every
// FeedSession) and seeded from wall-clock seconds, so a fresh per-session tracker
// -- and even a restarted process appending to the same KQR file -- keeps
// advancing it rather than reusing a prior epoch. seq resets per symbol on a
// rebuild; a gap within one epoch means data loss, a reset accompanied by an
// epoch bump is a benign session rebuild. Not thread-safe: one tracker lives
// inside one FeedSession and its seq map is touched only by that session's
// callback thread.
class SeqEpochTracker {
 public:
  // Advance the process-global feed-session generation (strictly increasing,
  // never below wall-clock seconds) and clear this tracker's per-symbol seq
  // counters. Returns the new epoch.
  std::uint32_t Rebuild();
  // Next seq for `symbol` in the current epoch (first call for a symbol -> 1).
  std::uint64_t NextSeq(const std::string& symbol);
  std::uint32_t Epoch() const { return epoch_; }

 private:
  std::uint32_t epoch_ = 0;
  std::unordered_map<std::string, std::uint64_t> seq_;
};

// Decode a serialized Envelope. If it is a Subscribe, write its (possibly empty)
// symbol list to *out and return true. Returns false for any other variant or
// malformed bytes.
bool DecodeSubscribe(const std::uint8_t* data, std::size_t len, std::vector<std::string>* out);

}  // namespace kairos::concords
