#ifndef KAIROS_EXEC_SIM_TYPES_H_
#define KAIROS_EXEC_SIM_TYPES_H_

#include <cstdint>
#include <functional>
#include <string>

#include "order_backend.h"  // Fill
#include "scenario.h"       // Side, Board
#include "tw_market.h"      // Cents

namespace kairos::exec {

// Fill model selected per FillEngine. Conservative = 穿價才成交; ProbQueue ports
// hftbacktest's queue-position concept (see fill_model.h).
enum class FillMode { kConservative, kProbQueue };

// A resting/marketable order tracked by the sim. `shares` is raw shares (no board
// multiply, matching PaperOrderBackend and the live hub's fill quantities).
struct SimOrder {
  std::string id;
  std::string symbol;
  Side side = Side::kBuy;
  Board board = Board::kRoundLot;
  Cents price = 0;
  long shares = 0;
  long filled = 0;
  std::int64_t place_ts_us = 0;

  long remaining() const { return shares - filled; }
};

// Outputs mirror OrderBackend's callback shapes so a SimOrderBackend (A4c) and a
// future live-paper driver (B8) are thin adapters over the same fill logic.
using SimAckFn = std::function<void(const std::string& id, bool ok, const std::string& err)>;
using SimFillFn = std::function<void(const std::string& id, const Fill&)>;
using SimCancelFn = std::function<void(const std::string& id, bool ok)>;

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_SIM_TYPES_H_
