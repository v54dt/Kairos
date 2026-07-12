#ifndef KAIROS_EXEC_ENUM_NAMES_H_
#define KAIROS_EXEC_ENUM_NAMES_H_

// Order-domain enums shared by the engine, the order hub, and the codecs, plus
// their canonical string names (definitions in scenario.cpp). Split out of
// scenario.h so consumers that only need the enums/names do not pull the whole
// Scenario config surface.

#include "tw_market.h"  // Product

namespace kairos::exec {

enum class Board { kOddLot, kRoundLot };
enum class Side { kBuy, kSell };
enum class Market { kTse, kOtc };
enum class Pacing { kAsap, kTwap };
enum class PricePolicy { kCross, kJoin, kMid, kLast };

const char* BoardName(Board b);
const char* PacingName(Pacing p);
const char* SideName(Side s);
const char* MarketName(Market m);
const char* PricePolicyName(PricePolicy p);
const char* ProductName(Product p);

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_ENUM_NAMES_H_
