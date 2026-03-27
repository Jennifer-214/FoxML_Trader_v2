// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [INTEGRATION TEST]
//======================================================================================================
// end-to-end test of the full pipeline:
//   MockGenerator -> FIX messages -> FIX_Parse -> DataStream -> Regression -> OrderGates -> Portfolio
//
// compile: g++ -std=c++17 -O2 -I.. -o integration_test integration_test.cpp
//======================================================================================================
#include <stdio.h>
#include "../DataStream/MockGenerator.hpp"
#include "../CoreFrameworks/Portfolio.hpp"
#include "../ML_Headers/LinearRegression3X.hpp"
#include "../ML_Headers/ROR_regressor.hpp"

using namespace std;

//======================================================================================================
// [HELPERS]
//======================================================================================================
static int tests_passed = 0;
static int tests_failed = 0;

static void check(const char *name, int condition) {
    if (condition) {
        printf("  [PASS] %s\n", name);
        tests_passed++;
    } else {
        printf("  [FAIL] %s\n", name);
        tests_failed++;
    }
}

// shorthand for FP64 throughout
constexpr unsigned FP = 64;

//======================================================================================================
// [TEST: FIX MESSAGE BUILD + PARSE]
//======================================================================================================
static void test_fix_roundtrip() {
    printf("\n--- FIX Build/Parse Roundtrip ---\n");

    char buf[FIX_MAX_MSG_LEN];
    int len = FIX_BuildMarketDataMsg(buf, sizeof(buf), 1, "AAPL", 2, 187.4500, 1500);

    check("message length > 0", len > 0);

    FIX_ParsedMessage msg = FIX_Parse(buf, len);
    check("parse valid", msg.valid == 1);
    check("msg_type = W", msg.msg_type == 'W');
    check("seq_num = 1", msg.seq_num == 1);
    check("symbol = AAPL", strcmp(msg.symbol, "AAPL") == 0);
    check("entry_type = 2 (trade)", msg.entry_type == 2);

    // price should be within rounding tolerance (4 decimal places)
    double price_err = msg.price - 187.45;
    if (price_err < 0)
        price_err = -price_err;
    check("price ~= 187.45", price_err < 0.01);

    check("volume = 1500", (int)msg.volume == 1500);

    // convert to DataStream
    DataStream<FP> stream = FIX_ToDataStream<FP>(&msg);
    double price_back     = FPN_ToDouble(stream.price);
    double vol_back       = FPN_ToDouble(stream.volume);
    double p_err          = price_back - 187.45;
    if (p_err < 0)
        p_err = -p_err;
    check("DataStream price roundtrip", p_err < 0.01);
    check("DataStream volume roundtrip", (int)vol_back == 1500);
}

//======================================================================================================
// [TEST: MOCK GENERATOR]
//======================================================================================================
static void test_mock_generator() {
    printf("\n--- Mock Generator ---\n");

    MockGeneratorConfig config;
    config.start_price  = 150.0;
    config.volatility   = 0.50;
    config.drift        = 0.01;
    config.base_volume  = 1000.0;
    config.volume_spike = 3.0;
    config.min_price    = 1.0;
    config.symbol       = "TSLA";
    config.seed         = 42;

    MockGenerator gen;
    MockGenerator_Init(&gen, config);

    char buf[FIX_MAX_MSG_LEN];
    FIX_ParsedMessage msgs[20];
    MockGenerator_Batch(&gen, msgs, 20, buf, sizeof(buf));

    check("20 ticks generated", msgs[19].seq_num == 20);
    check("all valid", msgs[0].valid && msgs[9].valid && msgs[19].valid);
    check("symbol preserved", strcmp(msgs[0].symbol, "TSLA") == 0);
    check("prices are positive", msgs[0].price > 0 && msgs[19].price > 0);
    check("volumes are positive", msgs[0].volume > 0 && msgs[19].volume > 0);

    // determinism: same seed should produce same results
    MockGenerator gen2;
    MockGenerator_Init(&gen2, config);
    FIX_ParsedMessage msgs2[20];
    MockGenerator_Batch(&gen2, msgs2, 20, buf, sizeof(buf));
    check("deterministic (same seed = same price)", msgs[19].price == msgs2[19].price);

    printf("  price range: %.2f -> %.2f\n", msgs[0].price, msgs[19].price);
}

//======================================================================================================
// [TEST: FIX -> REGRESSION PIPELINE]
//======================================================================================================
static void test_fix_to_regression() {
    printf("\n--- FIX -> Regression Pipeline ---\n");

    MockGeneratorConfig config;
    config.start_price  = 100.0;
    config.volatility   = 0.25;
    config.drift        = 0.10; // clear uptrend so regression slope should be positive
    config.base_volume  = 500.0;
    config.volume_spike = 2.0;
    config.min_price    = 1.0;
    config.symbol       = "MSFT";
    config.seed         = 12345;

    MockGenerator gen;
    MockGenerator_Init(&gen, config);

    RegressionFeederX<FP> feeder = RegressionFeederX_Init<FP>();
    RORRegressor<FP> ror         = RORRegressor_Init<FP>();
    char buf[FIX_MAX_MSG_LEN];

    // feed 3 windows worth of ticks through FIX -> parse -> regression
    for (int window = 0; window < 3; window++) {
        // fill one regression window
        for (int i = 0; i < MAX_WINDOW; i++) {
            FIX_ParsedMessage msg;
            MockGenerator_NextTick(&gen, buf, sizeof(buf), &msg);
            check("tick valid", msg.valid == 1);

            DataStream<FP> stream = FIX_ToDataStream<FP>(&msg);
            RegressionFeederX_Push(&feeder, stream.price);
        }

        // compute regression
        LinearRegression3XResult<FP> result = RegressionFeederX_Compute(&feeder);
        double slope                        = FPN_ToDouble(result.model.slope);
        double r2                           = FPN_ToDouble(result.r_squared);

        printf("  window %d: slope=%.6f  r2=%.6f\n", window, slope, r2);

        // push to ROR
        RORRegressor_Push(&ror, result);
    }

    // ROR should have 3 slope samples now
    check("ROR has 3 samples", ror.count == 3);

    LinearRegression3XResult<FP> ror_result = RORRegressor_Compute(&ror);
    double slope_of_slopes                  = FPN_ToDouble(ror_result.model.slope);
    printf("  slope-of-slopes: %.6f\n", slope_of_slopes);
    // with a positive drift, the inner slopes should be mostly positive
    check("slope-of-slopes computed (not NaN)", slope_of_slopes == slope_of_slopes);
}

//======================================================================================================
// [TEST: FIX -> ORDER GATES -> PORTFOLIO]
//======================================================================================================
static void test_fix_to_gates_to_portfolio() {
    printf("\n--- FIX -> Gates -> Portfolio ---\n");

    // set up order pool
    OrderPool<FP> pool;
    OrderPool_init(&pool, 64);

    // set up buy conditions: buy when price <= 100, volume >= 400
    BuySideGateConditions<FP> buy_conds;
    buy_conds.price  = FPN_FromDouble<FP>(100.0);
    buy_conds.volume = FPN_FromDouble<FP>(400.0);

    // set up sell conditions: sell when price >= 102, volume <= 2000
    SellSideGateConditions<FP> sell_conds;
    sell_conds.price  = FPN_FromDouble<FP>(102.0);
    sell_conds.volume = FPN_FromDouble<FP>(2000.0);

    ProfitTarget<FP> target;
    target.profit_target = FPN_FromDouble<FP>(1.50);

    // generate ticks starting at 98 with upward drift (should trigger buys then sells)
    MockGeneratorConfig config;
    config.start_price  = 98.0;
    config.volatility   = 0.30;
    config.drift        = 0.15;
    config.base_volume  = 600.0;
    config.volume_spike = 2.0;
    config.min_price    = 1.0;
    config.symbol       = "TEST";
    config.seed         = 99999;

    MockGenerator gen;
    MockGenerator_Init(&gen, config);
    char buf[FIX_MAX_MSG_LEN];

    int buy_triggers  = 0;
    int sell_attempts = 0;

    // run 50 ticks through the gates
    for (int i = 0; i < 50; i++) {
        FIX_ParsedMessage msg;
        MockGenerator_NextTick(&gen, buf, sizeof(buf), &msg);

        DataStream<FP> stream = FIX_ToDataStream<FP>(&msg);

        uint64_t bitmap_before = pool.bitmap;
        BuyGate(&buy_conds, &stream, &pool);
        if (pool.bitmap != bitmap_before)
            buy_triggers++;

        bitmap_before = pool.bitmap;
        SellGate(&sell_conds, &stream, &pool, &target);
        if (pool.bitmap != bitmap_before)
            sell_attempts++;
    }

    printf("  buy triggers:  %d\n", buy_triggers);
    printf("  sell clears:   %d\n", sell_attempts);
    printf("  active orders: %d\n", OrderPool_CountActive(&pool));

    check("some buys triggered", buy_triggers > 0);

    // move filled orders into portfolio
    Portfolio<FP> portfolio;
    Portfolio_Init(&portfolio);

    uint64_t active     = pool.bitmap;
    int positions_added = 0;
    while (active) {
        uint32_t idx = __builtin_ctzll(active);
        Portfolio_AddPosition(&portfolio, pool.slots[idx].quantity, pool.slots[idx].price);
        positions_added++;
        active &= active - 1;
    }

    printf("  positions added to portfolio: %d\n", positions_added);
    check("portfolio count matches", (int)portfolio.position_count == positions_added);

    if (portfolio.position_count > 0) {
        double first_price = FPN_ToDouble(portfolio.positions[0].entry_price);
        printf("  first position entry price: %.4f\n", first_price);
        check("entry price reasonable", first_price > 90.0 && first_price < 110.0);

        // test remove
        int count_before = (int)portfolio.position_count;
        Portfolio_RemovePosition(&portfolio, 0);
        check("remove decrements count", (int)portfolio.position_count == count_before - 1);
    }

    // test clear
    Portfolio_ClearPositions(&portfolio);
    check("clear zeros count", portfolio.position_count == 0);

    free(pool.slots);
}

//======================================================================================================
// [TEST: PORTFOLIO TEMPLATED AT DIFFERENT WIDTHS]
//======================================================================================================
static void test_portfolio_templated() {
    printf("\n--- Portfolio Template Widths ---\n");

    // FP64
    Portfolio<64> p64;
    Portfolio_Init(&p64);
    Portfolio_AddPosition(&p64, FPN_FromDouble<64>(10.0), FPN_FromDouble<64>(150.0));
    check("FP64 portfolio add", p64.position_count == 1);
    double q64 = FPN_ToDouble(p64.positions[0].quantity);
    check("FP64 quantity correct", q64 > 9.99 && q64 < 10.01);

    // FP128
    Portfolio<128> p128;
    Portfolio_Init(&p128);
    Portfolio_AddPosition(&p128, FPN_FromDouble<128>(25.0), FPN_FromDouble<128>(3000.0));
    check("FP128 portfolio add", p128.position_count == 1);
    double q128 = FPN_ToDouble(p128.positions[0].quantity);
    check("FP128 quantity correct", q128 > 24.99 && q128 < 25.01);

    // FP256
    Portfolio<256> p256;
    Portfolio_Init(&p256);
    Portfolio_AddPosition(&p256, FPN_FromDouble<256>(5.0), FPN_FromDouble<256>(42000.0));
    check("FP256 portfolio add", p256.position_count == 1);

    Portfolio_UpdatePosition(&p256, 0, FPN_FromDouble<256>(10.0), FPN_FromDouble<256>(42000.0));
    double q256 = FPN_ToDouble(p256.positions[0].quantity);
    check("FP256 update works", q256 > 9.99 && q256 < 10.01);
}

//======================================================================================================
// [MAIN]
//======================================================================================================
int main() {
    printf("======================================\n");
    printf("  LIBRARY INTEGRATION TEST\n");
    printf("======================================\n");

    test_fix_roundtrip();
    test_mock_generator();
    test_fix_to_regression();
    test_fix_to_gates_to_portfolio();
    test_portfolio_templated();

    printf("\n======================================\n");
    printf("  RESULTS: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("======================================\n");

    return tests_failed > 0 ? 1 : 0;
}
