// Pure-logic self-tests for the TW market + fee modules. No broker, no network.
// Exits non-zero on first failure.

#include <cstdio>

#include "test_check.h"
#include "tw_fees.h"
#include "tw_market.h"

using namespace kairos::exec;

static void TestTickTable() {
  CHECK_EQ(TickSizeCents(5'00), 1);  // 5.00 -> 0.01
  CHECK_EQ(TickSizeCents(9'99), 1);
  CHECK_EQ(TickSizeCents(10'00), 5);  // boundary 10 -> 0.05
  CHECK_EQ(TickSizeCents(25'00), 5);
  CHECK_EQ(TickSizeCents(50'00), 10);  // boundary 50 -> 0.10
  CHECK_EQ(TickSizeCents(99'90), 10);
  CHECK_EQ(TickSizeCents(100'00), 50);  // boundary 100 -> 0.50
  CHECK_EQ(TickSizeCents(499'50), 50);
  CHECK_EQ(TickSizeCents(500'00), 100);  // boundary 500 -> 1.00
  CHECK_EQ(TickSizeCents(999'00), 100);
  CHECK_EQ(TickSizeCents(1000'00), 500);  // boundary 1000 -> 5.00
  CHECK_EQ(TickSizeCents(1500'00), 500);

  CHECK(TickAligned(5'01));     // 0.01 grid
  CHECK(TickAligned(25'05));    // 0.05 grid
  CHECK(TickAligned(75'10));    // 0.10 grid
  CHECK(TickAligned(250'50));   // 0.50 grid
  CHECK(TickAligned(750'00));   // 1.00 grid
  CHECK(TickAligned(1500'00));  // 5.00 grid
  CHECK(!TickAligned(25'01));
  CHECK(!TickAligned(75'05));
  CHECK(!TickAligned(1501'00));
}

static void TestProductTicks() {
  // ETF: only two tiers (<50 -> 0.01, else 0.05). 0050 @~190 is the gotcha:
  // ETF tick 0.05 vs the stock table's 0.50.
  CHECK_EQ(TickSizeCents(9'99, Product::kEtf), 1);
  CHECK_EQ(TickSizeCents(49'95, Product::kEtf), 1);
  CHECK_EQ(TickSizeCents(50'00, Product::kEtf), 5);
  CHECK_EQ(TickSizeCents(190'00, Product::kEtf), 5);     // 0050 -> 0.05
  CHECK_EQ(TickSizeCents(190'00, Product::kStock), 50);  // same price, stock -> 0.50
  CHECK_EQ(TickSizeCents(5000'00, Product::kEtf), 5);

  // Warrant: <5:0.01, 5-10:0.05, 10-50:0.10, 50-100:0.50, 100-500:1.00, >=500:5.00
  CHECK_EQ(TickSizeCents(4'99, Product::kWarrant), 1);
  CHECK_EQ(TickSizeCents(5'00, Product::kWarrant), 5);
  CHECK_EQ(TickSizeCents(10'00, Product::kWarrant), 10);
  CHECK_EQ(TickSizeCents(50'00, Product::kWarrant), 50);
  CHECK_EQ(TickSizeCents(100'00, Product::kWarrant), 100);
  CHECK_EQ(TickSizeCents(499'00, Product::kWarrant), 100);
  CHECK_EQ(TickSizeCents(500'00, Product::kWarrant), 500);

  // Convertible bond: <150:0.05, 150-1000:1.00, >=1000:5.00
  CHECK_EQ(TickSizeCents(149'95, Product::kConvertibleBond), 5);
  CHECK_EQ(TickSizeCents(150'00, Product::kConvertibleBond), 100);
  CHECK_EQ(TickSizeCents(999'00, Product::kConvertibleBond), 100);
  CHECK_EQ(TickSizeCents(1000'00, Product::kConvertibleBond), 500);

  // Rounding honours the product table: 190.03 on an ETF rounds to the 0.05 grid.
  CHECK_EQ(RoundNearestTick(190'03, Product::kEtf), 190'05);
  CHECK_EQ(RoundDownToTick(190'03, Product::kEtf), 190'00);
  CHECK(TickAligned(190'05, Product::kEtf));
  CHECK(!TickAligned(190'05, Product::kStock));  // off the 0.50 stock grid
}

static void TestTickStep() {
  // 100.00 is asymmetric: down enters the 0.10 tier (50-100), up uses 0.50.
  CHECK_EQ(TickStep(100'00, -1), 99'90);
  CHECK_EQ(TickStep(100'00, -2), 99'80);
  CHECK_EQ(TickStep(99'90, 1), 100'00);
  CHECK_EQ(TickStep(100'00, 1), 100'50);
  // other boundaries
  CHECK_EQ(TickStep(500'00, -1), 499'50);
  CHECK_EQ(TickStep(500'00, 1), 501'00);
  CHECK_EQ(TickStep(50'00, -1), 49'95);
  CHECK_EQ(TickStep(50'00, 1), 50'10);
  // within one tier, multi-step
  CHECK_EQ(TickStep(200'00, -2), 199'00);
  CHECK_EQ(TickStep(200'00, 2), 201'00);
  CHECK_EQ(TickStep(100'00, 0), 100'00);
  // ETF tick is constant 0.05 at >=50 (no asymmetry at 100)
  CHECK_EQ(TickStep(100'00, -1, Product::kEtf), 99'95);
  CHECK_EQ(TickStep(100'00, 1, Product::kEtf), 100'05);
}

static void TestRounding() {
  // 2330 ~ 1005.0 sits on the 5.00 grid.
  CHECK_EQ(RoundNearestTick(1003'00), 1005'00);
  CHECK_EQ(RoundDownToTick(1003'00), 1000'00);
  CHECK_EQ(RoundUpToTick(1001'00), 1005'00);
  // 25.03 on the 0.05 grid -> down 25.00, up 25.05, nearest 25.05.
  CHECK_EQ(RoundDownToTick(25'03), 25'00);
  CHECK_EQ(RoundUpToTick(25'03), 25'05);
  CHECK_EQ(RoundNearestTick(25'03), 25'05);
  // Idempotence on aligned values.
  CHECK_EQ(RoundNearestTick(1005'00), 1005'00);
  CHECK_EQ(RoundDownToTick(99'90), 99'90);
}

static void TestFormat() {
  CHECK(CentsToString(1005'00) == "1005.00");
  CHECK(CentsToString(11'80) == "11.80");
  CHECK(CentsToString(9'40) == "9.40");
  CHECK(CentsToString(100'05) == "100.05");
  CHECK_EQ(FloatToCents(11.8), 11'80);
  CHECK_EQ(FloatToCents(1005.0), 1005'00);
}

static void TestFees() {
  FeeParams p;  // defaults: rate 0.1425%, oddlot min 1

  // The 1402/1403 ceiling: fee pinned at NT$1 up to 1403, tips at 1404.
  CHECK_EQ(OptimalMaxOrderValueTwd(p, /*is_oddlot=*/true), 1403);
  CHECK_EQ(BrokerageFee(1402'00, p, true), 1);
  CHECK_EQ(BrokerageFee(1403'00, p, true), 1);
  CHECK_EQ(BrokerageFee(1404'00, p, true), 2);

  // 1 share of 2330 @1005 -> floor(1005*0.001425)=1 -> min 1.
  CHECK_EQ(BrokerageFee(1005'00, p, true), 1);
  // A single share above the optimal cap unavoidably costs 2.
  CHECK_EQ(BrokerageFee(1500'00, p, true), 2);

  // Round-lot min defaults to 20.
  CHECK_EQ(BrokerageFee(1000'00, p, /*is_oddlot=*/false), 20);
  CHECK_EQ(BrokerageFee(100000'00, p, false), 142);  // floor(100000*0.001425)

  // Sell tax (normal 0.3%, day-trade 0.15%).
  CHECK_EQ(SellTax(100000'00, p, /*daytrade=*/false), 300);
  CHECK_EQ(SellTax(100000'00, p, /*daytrade=*/true), 150);

  // Discount lowers the rate and raises the optimal cap.
  FeeParams d = p;
  d.discount = 0.5;  // 5 折
  // rate 0.0007125 -> N_opt = ceil(2/0.0007125)-1 = 2807, fee@2807=1, @2808=2.
  CHECK_EQ(OptimalMaxOrderValueTwd(d, true), 2807);
  CHECK_EQ(BrokerageFee(2807'00, d, true), 1);
  CHECK_EQ(BrokerageFee(2808'00, d, true), 2);

  // Explicit override wins over the auto cap.
  FeeParams o = p;
  o.max_order_value_twd = 1402;
  CHECK_EQ(ResolvedMaxOrderValueTwd(o, true), 1402);
}

static void TestSizing() {
  FeeParams p;  // cap auto = 1403
  // 2330 @1005 -> floor(1403/1005)=1 share/order.
  CHECK_EQ(OptimalSharesPerOrder(1005'00, p, true), 1);
  // A 50.00 name -> floor(1403/50)=28 shares/order stay at min fee.
  CHECK_EQ(OptimalSharesPerOrder(50'00, p, true), 28);
  CHECK_EQ(BrokerageFee(28 * 50'00, p, true), 1);  // 1400 -> fee 1
  CHECK(BrokerageFee(29 * 50'00, p, true) >= 2);   // 1450 -> fee 2 (would exceed cap)
  // A 10.00 name -> floor(1403/10)=140 shares/order.
  CHECK_EQ(OptimalSharesPerOrder(10'00, p, true), 140);
  // Above-cap single share still yields 1.
  CHECK_EQ(OptimalSharesPerOrder(2000'00, p, true), 1);
}

int main() {
  TestTickTable();
  TestProductTicks();
  TestTickStep();
  TestRounding();
  TestFormat();
  TestFees();
  TestSizing();
  if (g_failures == 0) {
    std::printf("test_market_fees: OK\n");
    return 0;
  }
  std::printf("test_market_fees: FAILED %d check(s)\n", g_failures);
  return 1;
}
