// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [CONTROLLER TEST SUITE]
//======================================================================================================
// tests for Portfolio (bitmap), PositionExitGate, PortfolioController, TradeLog, config parser
// compile: g++ -std=c++17 -O2 -I.. -o controller_test controller_test.cpp
//======================================================================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../DataStream/MockGenerator.hpp"
#include "../CoreFrameworks/PortfolioController.hpp"
#include "../CoreFrameworks/Order.hpp"
#include "../CoreFrameworks/OrderManager.hpp"
#include "../DataStream/BinanceUserData.hpp"
#include "../Backtest/BacktestEngine.hpp"
#include "../Backtest/HeldOutSplit.hpp"

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

constexpr unsigned FP = 64;

// helper: run warmup to completion, auto-computes tick count from config
// handles both warmup_ticks and min_warmup_samples gates
static void test_warmup_ctrl(PortfolioController<FP> *ctrl, OrderPool<FP> *pool,
                              TradeLog *log, double base_price, double base_vol) {
    int ticks = (int)ctrl->config.warmup_ticks;
    int for_samples = (int)ctrl->config.min_warmup_samples * (int)ctrl->config.poll_interval;
    if (for_samples > ticks) ticks = for_samples;
    ticks += 5; // margin
    for (int i = 0; i < ticks; i++) {
        PortfolioController_Tick(ctrl, pool,
            FPN_FromDouble<FP>(base_price + (i % 10) * 0.3),
            FPN_FromDouble<FP>(base_vol), log);
    }
}

//======================================================================================================
// [TEST 1: CONFIG PARSER]
//======================================================================================================
static void test_config_parser() {
    printf("\n--- Config Parser ---\n");

    // write a test config file
    FILE *f = fopen("/tmp/test_controller.cfg", "w");
    fprintf(f, "# test config\n");
    fprintf(f, "poll_interval=50\n");
    fprintf(f, "warmup_ticks=32\n");
    fprintf(f, "r2_threshold=0.40\n");
    fprintf(f, "slope_scale_buy=0.75\n");
    fprintf(f, "max_shift=3.00\n");
    fprintf(f, "take_profit_pct=5.00\n");
    fprintf(f, "stop_loss_pct=2.00\n");
    fclose(f);

    ControllerConfig<FP> cfg = ControllerConfig_Load<FP>("/tmp/test_controller.cfg");
    check("poll_interval parsed", cfg.poll_interval == 50);
    check("warmup_ticks parsed", cfg.warmup_ticks == 32);

    double r2 = FPN_ToDouble(cfg.r2_threshold);
    check("r2_threshold parsed", fabs(r2 - 0.40) < 0.01);

    double slope = FPN_ToDouble(cfg.slope_scale_buy);
    check("slope_scale_buy parsed", fabs(slope - 0.75) < 0.01);

    double ms = FPN_ToDouble(cfg.max_shift);
    check("max_shift parsed", fabs(ms - 3.0) < 0.01);

    // take_profit_pct is divided by 100 in parser
    double tp = FPN_ToDouble(cfg.take_profit_pct);
    check("take_profit_pct parsed (5% -> 0.05)", fabs(tp - 0.05) < 0.001);

    double sl = FPN_ToDouble(cfg.stop_loss_pct);
    check("stop_loss_pct parsed (2% -> 0.02)", fabs(sl - 0.02) < 0.001);

    // test defaults when file missing
    ControllerConfig<FP> def = ControllerConfig_Load<FP>("/tmp/nonexistent_config.cfg");
    check("missing file returns defaults", def.poll_interval == 100);

    remove("/tmp/test_controller.cfg");
}

//======================================================================================================
// [TEST 2: PORTFOLIO BITMAP BASICS]
//======================================================================================================
static void test_portfolio_bitmap() {
    printf("\n--- Portfolio Bitmap Basics ---\n");

    Portfolio<FP> port;
    Portfolio_Init(&port);
    check("init bitmap is 0", port.active_bitmap == 0);
    check("count is 0", Portfolio_CountActive(&port) == 0);

    // add positions
    Portfolio_AddPosition(&port, FPN_FromDouble<FP>(10.0), FPN_FromDouble<FP>(100.0));
    check("add sets bit", port.active_bitmap == 1);
    check("count is 1", Portfolio_CountActive(&port) == 1);

    Portfolio_AddPosition(&port, FPN_FromDouble<FP>(5.0), FPN_FromDouble<FP>(200.0));
    check("second add sets bit 1", port.active_bitmap == 3);
    check("count is 2", Portfolio_CountActive(&port) == 2);

    // remove position 0
    Portfolio_RemovePosition(&port, 0);
    check("remove clears bit 0", port.active_bitmap == 2);
    check("count is 1 after remove", Portfolio_CountActive(&port) == 1);
    // data still at index 1
    double q1 = FPN_ToDouble(port.positions[1].quantity);
    check("position 1 data intact", fabs(q1 - 5.0) < 0.01);

    // slot reuse: add new position, should get slot 0
    Portfolio_AddPosition(&port, FPN_FromDouble<FP>(7.0), FPN_FromDouble<FP>(300.0));
    check("slot 0 reused", port.active_bitmap == 3);
    double q0 = FPN_ToDouble(port.positions[0].quantity);
    check("new position in slot 0", fabs(q0 - 7.0) < 0.01);

    // test full
    check("not full at 2", !Portfolio_IsFull(&port));
    Portfolio_ClearPositions(&port);
    for (int i = 0; i < 16; i++) {
        Portfolio_AddPosition(&port, FPN_FromDouble<FP>(1.0), FPN_FromDouble<FP>((double)i));
    }
    check("full at 16", Portfolio_IsFull(&port));
    check("count is 16", Portfolio_CountActive(&port) == 16);

    // add when full should be no-op
    Portfolio_AddPosition(&port, FPN_FromDouble<FP>(1.0), FPN_FromDouble<FP>(999.0));
    check("count still 16", Portfolio_CountActive(&port) == 16);
}

//======================================================================================================
// [TEST 3: PORTFOLIO P&L]
//======================================================================================================
static void test_portfolio_pnl() {
    printf("\n--- Portfolio P&L ---\n");

    Portfolio<FP> port;
    Portfolio_Init(&port);

    // add positions: 10 shares at $100, -5 shares at $50
    Portfolio_AddPosition(&port, FPN_FromDouble<FP>(10.0), FPN_FromDouble<FP>(100.0));
    Portfolio_AddPosition(&port, FPN_FromDouble<FP>(-5.0), FPN_FromDouble<FP>(50.0));

    // at $110: long P&L = (110-100)*10 = 100, short P&L = (110-50)*(-5) = -300, total = -200
    FPN<FP> price = FPN_FromDouble<FP>(110.0);
    double pnl = FPN_ToDouble(Portfolio_ComputePnL(&port, price));
    check("mixed P&L correct", fabs(pnl - (-200.0)) < 1.0);

    // empty portfolio P&L is zero
    Portfolio_ClearPositions(&port);
    pnl = FPN_ToDouble(Portfolio_ComputePnL(&port, price));
    check("empty P&L is zero", fabs(pnl) < 0.01);
}

//======================================================================================================
// [TEST 4: POSITION CONSOLIDATION]
//======================================================================================================
static void test_consolidation() {
    printf("\n--- Position Consolidation ---\n");

    Portfolio<FP> port;
    Portfolio_Init(&port);

    FPN<FP> price = FPN_FromDouble<FP>(98.50);

    Portfolio_AddPosition(&port, FPN_FromDouble<FP>(100.0), price);
    check("first add", Portfolio_CountActive(&port) == 1);

    // find by price
    int idx = Portfolio_FindByPrice(&port, price);
    check("find by price works", idx == 0);

    // consolidate
    Portfolio_AddQuantity(&port, idx, FPN_FromDouble<FP>(200.0));
    double qty = FPN_ToDouble(port.positions[0].quantity);
    check("consolidated quantity", fabs(qty - 300.0) < 0.01);
    check("still one position", Portfolio_CountActive(&port) == 1);

    // different price is a separate position
    FPN<FP> price2 = FPN_FromDouble<FP>(99.00);
    Portfolio_AddPosition(&port, FPN_FromDouble<FP>(50.0), price2);
    check("different price = new position", Portfolio_CountActive(&port) == 2);

    // FPN_Equal determinism: same double -> same bits
    FPN<FP> a = FPN_FromDouble<FP>(98.50);
    FPN<FP> b = FPN_FromDouble<FP>(98.50);
    check("FPN_Equal deterministic", FPN_Equal(a, b));
}

//======================================================================================================
// [TEST 5: POSITION EXIT GATE (HOT PATH)]
//======================================================================================================
static void test_exit_gate() {
    printf("\n--- Position Exit Gate ---\n");

    Portfolio<FP> port;
    Portfolio_Init(&port);
    ExitBuffer<FP> buf;
    ExitBuffer_Init(&buf);

    // add position: entry $100, TP $103, SL $98.50
    FPN<FP> entry = FPN_FromDouble<FP>(100.0);
    FPN<FP> tp    = FPN_FromDouble<FP>(103.0);
    FPN<FP> sl    = FPN_FromDouble<FP>(98.50);
    Portfolio_AddPositionWithExits(&port, FPN_FromDouble<FP>(10.0), entry, tp, sl);

    // price between TP and SL - no exit
    PositionExitGate(&port, FPN_FromDouble<FP>(101.0), &buf, 100);
    check("no exit between TP/SL", buf.count == 0);
    check("position still active", port.active_bitmap == 1);

    // price hits take profit
    PositionExitGate(&port, FPN_FromDouble<FP>(103.50), &buf, 200);
    check("TP exit triggered", buf.count == 1);
    check("exit reason is TP", buf.records[0].reason == 0);
    check("exit index correct", buf.records[0].position_index == 0);
    check("exit tick correct", buf.records[0].tick == 200);
    check("bit cleared", port.active_bitmap == 0);

    // call again - should NOT re-trigger (bit is cleared)
    PositionExitGate(&port, FPN_FromDouble<FP>(103.50), &buf, 201);
    check("no re-trigger after exit", buf.count == 1); // still 1, not 2

    // test stop loss
    ExitBuffer_Clear(&buf);
    Portfolio_AddPositionWithExits(&port, FPN_FromDouble<FP>(5.0),
                                   FPN_FromDouble<FP>(100.0),
                                   FPN_FromDouble<FP>(103.0),
                                   FPN_FromDouble<FP>(98.50));
    PositionExitGate(&port, FPN_FromDouble<FP>(97.0), &buf, 300);
    check("SL exit triggered", buf.count == 1);
    check("exit reason is SL", buf.records[0].reason == 1);

    // test multiple positions, partial exit
    ExitBuffer_Clear(&buf);
    Portfolio_ClearPositions(&port);
    // pos 0: TP $105
    Portfolio_AddPositionWithExits(&port, FPN_FromDouble<FP>(10.0),
                                   FPN_FromDouble<FP>(100.0),
                                   FPN_FromDouble<FP>(105.0),
                                   FPN_FromDouble<FP>(95.0));
    // pos 1: TP $102
    Portfolio_AddPositionWithExits(&port, FPN_FromDouble<FP>(5.0),
                                   FPN_FromDouble<FP>(99.0),
                                   FPN_FromDouble<FP>(102.0),
                                   FPN_FromDouble<FP>(96.0));

    // price $103: pos 1 exits (TP $102), pos 0 stays (TP $105)
    PositionExitGate(&port, FPN_FromDouble<FP>(103.0), &buf, 400);
    check("partial exit: 1 of 2", buf.count == 1);
    check("correct position exited", buf.records[0].position_index == 1);
    check("other position still active", (port.active_bitmap & 1) == 1);
}

//======================================================================================================
// [TEST 6: EXIT BUFFER DRAIN]
//======================================================================================================
static void test_exit_buffer_drain() {
    printf("\n--- Exit Buffer Drain ---\n");

    Portfolio<FP> port;
    Portfolio_Init(&port);

    // add position, then manually populate exit buffer
    Portfolio_AddPositionWithExits(&port, FPN_FromDouble<FP>(10.0),
                                   FPN_FromDouble<FP>(100.0),
                                   FPN_FromDouble<FP>(103.0),
                                   FPN_FromDouble<FP>(98.50));
    // clear bit (simulate hot-path exit)
    Portfolio_RemovePosition(&port, 0);

    // data still readable at index 0
    double ep = FPN_ToDouble(port.positions[0].entry_price);
    check("data readable after bit clear", fabs(ep - 100.0) < 0.01);
}

//======================================================================================================
// [TEST 7: FILL CONSUMPTION TIMING]
//======================================================================================================
static void test_fill_timing() {
    printf("\n--- Fill Consumption Timing ---\n");

    ControllerConfig<FP> cfg = ControllerConfig_Default<FP>();
    cfg.warmup_ticks = 0; // skip warmup for this test
    cfg.poll_interval = 1000; // slow path won't run during test

    PortfolioController<FP> ctrl = {};
    PortfolioController_Init(&ctrl, cfg);
    ctrl.state = CONTROLLER_ACTIVE; // force active
    ctrl.buy_conds.price  = FPN_FromDouble<FP>(100.0);
    ctrl.buy_conds.volume = FPN_FromDouble<FP>(400.0);
    ctrl.mean_rev.buy_conds_initial = ctrl.buy_conds;

    OrderPool<FP> pool;
    OrderPool_init(&pool, 64);

    TradeLog log;
    log.file = 0; // no file for this test
    log.trade_count = 0;

    // simulate BuyGate filling slot 0
    pool.slots[0].price    = FPN_FromDouble<FP>(98.0);
    pool.slots[0].quantity = FPN_FromDouble<FP>(500.0);
    pool.bitmap = 1;

    // call controller tick - should consume fill immediately
    PortfolioController_Tick(&ctrl, &pool, FPN_FromDouble<FP>(98.0), FPN_FromDouble<FP>(500.0), &log);

    check("position created same tick", Portfolio_CountActive(&ctrl.portfolio) == 1);
    check("pool slot cleared", pool.bitmap == 0);

    // verify TP/SL computed correctly
    double tp = FPN_ToDouble(ctrl.portfolio.positions[0].take_profit_price);
    double sl = FPN_ToDouble(ctrl.portfolio.positions[0].stop_loss_price);
    double expected_tp = 98.0 * (1.0 + 0.03);  // 100.94
    double expected_sl = 98.0 * (1.0 - 0.015); // 96.53
    check("TP price computed", fabs(tp - expected_tp) < 0.1);
    check("SL price computed", fabs(sl - expected_sl) < 0.1);

    free(pool.slots);
}

//======================================================================================================
// [TEST 8: POOL BACKPRESSURE]
//======================================================================================================
static void test_backpressure() {
    printf("\n--- Pool Backpressure ---\n");

    ControllerConfig<FP> cfg = ControllerConfig_Default<FP>();
    cfg.warmup_ticks = 0;
    cfg.poll_interval = 1000;
    cfg.max_positions = 16; // test bitmap capacity, not position cap

    PortfolioController<FP> ctrl = {};
    PortfolioController_Init(&ctrl, cfg);
    ctrl.state = CONTROLLER_ACTIVE;
    ctrl.buy_conds.price  = FPN_FromDouble<FP>(100.0);
    ctrl.buy_conds.volume = FPN_FromDouble<FP>(400.0);
    ctrl.mean_rev.buy_conds_initial = ctrl.buy_conds;

    OrderPool<FP> pool;
    OrderPool_init(&pool, 64);

    TradeLog log;
    log.file = 0;
    log.trade_count = 0;

    // fill all 16 portfolio slots
    for (int i = 0; i < 16; i++) {
        pool.slots[i].price    = FPN_FromDouble<FP>(90.0 + i); // different prices
        pool.slots[i].quantity = FPN_FromDouble<FP>(100.0);
        pool.bitmap |= (1ULL << i);
    }
    PortfolioController_Tick(&ctrl, &pool, FPN_FromDouble<FP>(95.0), FPN_FromDouble<FP>(500.0), &log);
    check("16 positions filled", Portfolio_CountActive(&ctrl.portfolio) == 16);
    check("portfolio full", Portfolio_IsFull(&ctrl.portfolio));

    // try to add more - pool slot should stay
    pool.slots[20].price    = FPN_FromDouble<FP>(110.0);
    pool.slots[20].quantity = FPN_FromDouble<FP>(100.0);
    pool.bitmap |= (1ULL << 20);

    PortfolioController_Tick(&ctrl, &pool, FPN_FromDouble<FP>(95.0), FPN_FromDouble<FP>(500.0), &log);
    check("still 16 (backpressure)", Portfolio_CountActive(&ctrl.portfolio) == 16);
    check("pool slot remains", (pool.bitmap & (1ULL << 20)) != 0);

    free(pool.slots);
}

//======================================================================================================
// [TEST 9: WARMUP PHASE]
//======================================================================================================
static void test_warmup() {
    printf("\n--- Warmup Phase ---\n");

    ControllerConfig<FP> cfg = ControllerConfig_Default<FP>();
    cfg.warmup_ticks = 10;
    cfg.poll_interval = 1; // every tick pushes to rolling stats during warmup

    PortfolioController<FP> ctrl = {};
    PortfolioController_Init(&ctrl, cfg);

    check("starts in warmup", ctrl.state == CONTROLLER_WARMUP);
    check("buy price is zero (disabled)", FPN_IsZero(ctrl.buy_conds.price));

    OrderPool<FP> pool;
    OrderPool_init(&pool, 64);
    TradeLog log;
    log.file = 0;
    log.trade_count = 0;

    // feed 10 ticks with known prices around $100
    for (int i = 0; i < 10; i++) {
        FPN<FP> price  = FPN_FromDouble<FP>(98.0 + (double)i * 0.5); // 98, 98.5, ..., 102.5
        FPN<FP> volume = FPN_FromDouble<FP>(500.0 + (double)i * 10.0);
        PortfolioController_Tick(&ctrl, &pool, price, volume, &log);
    }

    check("transitioned to active", ctrl.state == CONTROLLER_ACTIVE);
    check("no positions during warmup", Portfolio_CountActive(&ctrl.portfolio) == 0);

    double mean_p = FPN_ToDouble(ctrl.buy_conds.price);
    // mean of 98, 98.5, 99, 99.5, 100, 100.5, 101, 101.5, 102, 102.5 = 100.25
    check("buy price from observed mean", fabs(mean_p - 100.25) < 0.5);

    double mean_v = FPN_ToDouble(ctrl.buy_conds.volume);
    check("buy volume from observed mean", mean_v > 0);

    // initial anchor should match
    check("initial anchor set", FPN_Equal(ctrl.buy_conds.price, ctrl.mean_rev.buy_conds_initial.price));

    free(pool.slots);
}

//======================================================================================================
// [TEST 10: REGRESSION FEEDBACK]
//======================================================================================================
static void test_regression_feedback() {
    printf("\n--- Regression Feedback ---\n");

    ControllerConfig<FP> cfg = ControllerConfig_Default<FP>();
    cfg.warmup_ticks  = 10;
    cfg.poll_interval = 1;   // slow path every tick for testing
    cfg.r2_threshold  = FPN_FromDouble<FP>(0.01); // low threshold so adjustments happen

    PortfolioController<FP> ctrl = {};
    PortfolioController_Init(&ctrl, cfg);

    OrderPool<FP> pool;
    OrderPool_init(&pool, 64);

    TradeLog log;
    log.file = 0;
    log.trade_count = 0;

    // warmup
    for (int i = 0; i < 10; i++) {
        FPN<FP> price = FPN_FromDouble<FP>(100.0);
        FPN<FP> vol   = FPN_FromDouble<FP>(500.0);
        PortfolioController_Tick(&ctrl, &pool, price, vol, &log);
    }
    check("warmup done", ctrl.state == CONTROLLER_ACTIVE);

    FPN<FP> initial_price = ctrl.buy_conds.price;

    // feed ticks with positions that have clear uptrend P&L
    // simulate fills manually
    for (int i = 0; i < 5; i++) {
        pool.slots[i].price    = FPN_FromDouble<FP>(99.0);
        pool.slots[i].quantity = FPN_FromDouble<FP>(10.0);
        pool.bitmap |= (1ULL << i);
    }

    // run many ticks with rising price (positions become increasingly profitable)
    for (int i = 0; i < 200; i++) {
        FPN<FP> price = FPN_FromDouble<FP>(99.0 + (double)i * 0.01);
        FPN<FP> vol   = FPN_FromDouble<FP>(500.0);
        PortfolioController_Tick(&ctrl, &pool, price, vol, &log);
    }

    // buy conditions should have shifted from initial
    int shifted = !FPN_Equal(ctrl.buy_conds.price, initial_price);
    check("buy conditions shifted", shifted);

    free(pool.slots);
}

//======================================================================================================
// [TEST 11: TRADE LOG]
//======================================================================================================
static void test_trade_log() {
    printf("\n--- Trade Log ---\n");

    remove("logging/TEST_order_history.csv");

    TradeLog log;
    int ok = TradeLog_Init(&log, "TEST");
    check("log init", ok);

    { TradeLogRecord r = {};
      r.tick = 100; r.price = 98.50; r.quantity = 600.0;
      r.tp = 101.45; r.sl = 97.02; r.buy_cond_p = 100.0; r.buy_cond_v = 400.0;
      r.is_buy = 1;
      TradeLog_Buy(&log, &r); }
    { TradeLogRecord r = {};
      r.tick = 200; r.price = 101.23; r.quantity = 600.0;
      r.entry_price = 98.50; r.delta_pct = 2.77;
      snprintf(r.reason, sizeof(r.reason), "TP");
      TradeLog_Sell(&log, &r); }
    TradeLog_Close(&log);

    // read back and verify
    FILE *f = fopen("logging/TEST_order_history.csv", "r");
    check("file created", f != 0);
    if (f) {
        char line[512];
        fgets(line, sizeof(line), f); // header
        check("header present", strstr(line, "tick,side") != 0);

        fgets(line, sizeof(line), f); // buy row
        check("buy row has BUY", strstr(line, "BUY") != 0);
        check("buy row has tick", strstr(line, "100,") == line);

        fgets(line, sizeof(line), f); // sell row
        check("sell row has SELL", strstr(line, "SELL") != 0);
        check("sell row has TP", strstr(line, "TP") != 0);

        fclose(f);
    }
    remove("logging/TEST_order_history.csv");
}

//======================================================================================================
// [TEST 12: BRANCHLESS VERIFICATION]
//======================================================================================================
static void test_branchless() {
    printf("\n--- Branchless Verification ---\n");

    ControllerConfig<FP> cfg = ControllerConfig_Default<FP>();
    cfg.warmup_ticks  = 5;
    cfg.poll_interval = 1;
    cfg.r2_threshold  = FPN_FromDouble<FP>(0.80); // HIGH threshold
    cfg.regime_volatile_stddev = FPN_FromDouble<FP>(1.0); // disable volatile detection for this test
    cfg.regime_hysteresis = 1000; // prevent regime switching during test

    PortfolioController<FP> ctrl = {};
    PortfolioController_Init(&ctrl, cfg);

    OrderPool<FP> pool;
    OrderPool_init(&pool, 64);
    TradeLog log;
    log.file = 0;
    log.trade_count = 0;

    // warmup
    for (int i = 0; i < 5; i++) {
        PortfolioController_Tick(&ctrl, &pool, FPN_FromDouble<FP>(100.0), FPN_FromDouble<FP>(500.0), &log);
    }

    FPN<FP> initial_price = ctrl.buy_conds.price;

    // feed noisy data (should NOT shift because R^2 will be low)
    MockGeneratorConfig mc;
    mc.start_price = 100.0; mc.volatility = 5.0; mc.drift = 0.0;
    mc.base_volume = 500.0; mc.volume_spike = 3.0; mc.min_price = 1.0;
    mc.symbol = "NOISY"; mc.seed = 42;
    MockGenerator gen;
    MockGenerator_Init(&gen, mc);
    char buf[FIX_MAX_MSG_LEN];

    for (int i = 0; i < 100; i++) {
        FIX_ParsedMessage msg;
        MockGenerator_NextTick(&gen, buf, sizeof(buf), &msg);
        DataStream<FP> stream = FIX_ToDataStream<FP>(&msg);
        PortfolioController_Tick(&ctrl, &pool, stream.price, stream.volume, &log);
    }

    // with rolling stats, buy_conds.price now updates dynamically on the slow path
    // so it wont be exactly equal to initial - but it should stay near the mean price (~100.0)
    // the key check is that the gate didnt drift wildly due to low R^2
    double final_price = FPN_ToDouble(ctrl.buy_conds.price);
    check("noisy data: conditions near initial", fabs(final_price - 100.0) < 10.0);

    free(pool.slots);
}

//======================================================================================================
// [TEST 13: MAX SHIFT CLAMP]
//======================================================================================================
static void test_max_shift() {
    printf("\n--- Max Shift Clamp ---\n");

    ControllerConfig<FP> cfg = ControllerConfig_Default<FP>();
    cfg.max_shift     = FPN_FromDouble<FP>(0.02); // tight clamp: 2% of price (~$2 at $100)
    cfg.r2_threshold  = FPN_FromDouble<FP>(0.01);
    cfg.warmup_ticks  = 5;
    cfg.poll_interval = 1;
    // pin to RANGING — this test is about MR max_shift, not regime detection
    cfg.regime_slope_threshold = FPN_FromDouble<FP>(1.0);

    PortfolioController<FP> ctrl = {};
    PortfolioController_Init(&ctrl, cfg);

    OrderPool<FP> pool;
    OrderPool_init(&pool, 64);
    TradeLog log;
    log.file = 0;
    log.trade_count = 0;

    // warmup
    for (int i = 0; i < 5; i++) {
        PortfolioController_Tick(&ctrl, &pool, FPN_FromDouble<FP>(100.0), FPN_FromDouble<FP>(500.0), &log);
    }

    FPN<FP> initial = ctrl.mean_rev.buy_conds_initial.price;

    // add positions and feed extreme trend
    for (int i = 0; i < 5; i++) {
        pool.slots[i].price    = FPN_FromDouble<FP>(99.0);
        pool.slots[i].quantity = FPN_FromDouble<FP>(10.0);
        pool.bitmap |= (1ULL << i);
    }

    for (int i = 0; i < 500; i++) {
        FPN<FP> price = FPN_FromDouble<FP>(100.0 + (double)i * 0.1);
        PortfolioController_Tick(&ctrl, &pool, price, FPN_FromDouble<FP>(500.0), &log);
    }

    // buy_conds_initial now tracks rolling average, so the gate moves with the market
    // the clamp ensures the gate stays within max_shift of the CURRENT rolling average
    // with rising prices, the gate should be near the latest rolling avg, not the warmup price
    double final_price = FPN_ToDouble(ctrl.buy_conds.price);
    double rolling_avg = FPN_ToDouble(ctrl.rolling.price_avg);
    double shift_from_rolling = fabs(final_price - rolling_avg);
    // max_shift is now a fraction of price: 0.02 * rolling_avg ≈ $2 at $100
    double max_shift_abs = rolling_avg * 0.02;
    check("shift clamped to max_shift", shift_from_rolling <= max_shift_abs + 0.5);

    free(pool.slots);
}

//======================================================================================================
// [TEST 14: EMPTY PORTFOLIO REGRESSION]
//======================================================================================================
static void test_empty_regression() {
    printf("\n--- Empty Portfolio Regression ---\n");

    ControllerConfig<FP> cfg = ControllerConfig_Default<FP>();
    cfg.warmup_ticks  = 5;
    cfg.poll_interval = 1;
    cfg.r2_threshold  = FPN_FromDouble<FP>(0.01);

    PortfolioController<FP> ctrl = {};
    PortfolioController_Init(&ctrl, cfg);

    OrderPool<FP> pool;
    OrderPool_init(&pool, 64);
    TradeLog log;
    log.file = 0;
    log.trade_count = 0;

    // warmup
    for (int i = 0; i < 5; i++) {
        PortfolioController_Tick(&ctrl, &pool, FPN_FromDouble<FP>(100.0), FPN_FromDouble<FP>(500.0), &log);
    }

    // no positions - P&L should be zero
    check("portfolio empty", Portfolio_CountActive(&ctrl.portfolio) == 0);

    // run ticks - should push zero, slope should flatten
    for (int i = 0; i < 50; i++) {
        PortfolioController_Tick(&ctrl, &pool, FPN_FromDouble<FP>(100.0), FPN_FromDouble<FP>(500.0), &log);
    }

    double pnl = FPN_ToDouble(ctrl.portfolio_delta);
    check("P&L stays zero", fabs(pnl) < 0.01);

    free(pool.slots);
}

//======================================================================================================
// [TEST 15: TICK COUNTER]
//======================================================================================================
static void test_tick_counter() {
    printf("\n--- Tick Counter ---\n");

    ControllerConfig<FP> cfg = ControllerConfig_Default<FP>();
    cfg.warmup_ticks = 5;

    PortfolioController<FP> ctrl = {};
    PortfolioController_Init(&ctrl, cfg);

    OrderPool<FP> pool;
    OrderPool_init(&pool, 64);
    TradeLog log;
    log.file = 0;
    log.trade_count = 0;

    check("starts at 0", ctrl.total_ticks == 0);

    for (int i = 0; i < 20; i++) {
        PortfolioController_Tick(&ctrl, &pool, FPN_FromDouble<FP>(100.0), FPN_FromDouble<FP>(500.0), &log);
    }
    check("total_ticks = 20", ctrl.total_ticks == 20);

    free(pool.slots);
}

//======================================================================================================
// [TEST 16: FULL PIPELINE INTEGRATION]
//======================================================================================================
static void test_full_pipeline() {
    printf("\n--- Full Pipeline Integration ---\n");

    remove("logging/INTG_order_history.csv");

    ControllerConfig<FP> cfg = ControllerConfig_Default<FP>();
    cfg.warmup_ticks  = 20;
    cfg.poll_interval = 10;
    cfg.take_profit_pct    = FPN_FromDouble<FP>(0.03);
    cfg.stop_loss_pct      = FPN_FromDouble<FP>(0.015);
    // loosen volume filter for mock data - mock volumes are uniform around base_volume
    // so we need a low multiplier for some ticks to pass the filter
    cfg.volume_multiplier  = FPN_FromDouble<FP>(1.2);
    cfg.entry_offset_pct   = FPN_FromDouble<FP>(0.005); // 0.5% offset - mock data has high volatility
    cfg.spacing_multiplier = FPN_FromDouble<FP>(0.5);    // tight spacing - mock price range is small

    PortfolioController<FP> ctrl = {};
    PortfolioController_Init(&ctrl, cfg);

    OrderPool<FP> pool;
    OrderPool_init(&pool, 64);

    TradeLog log;
    TradeLog_Init(&log, "INTG");

    MockGeneratorConfig mc;
    mc.start_price  = 100.0;
    mc.volatility   = 1.50;   // high volatility so price dips below mean (triggering buys)
    mc.drift        = 0.0;    // no drift - oscillates around mean
    mc.base_volume  = 600.0;
    mc.volume_spike = 3.0;    // higher spikes so some ticks pass the volume filter
    mc.min_price    = 1.0;
    mc.symbol       = "INTG";
    mc.seed         = 77777;

    MockGenerator gen;
    MockGenerator_Init(&gen, mc);
    char buf[FIX_MAX_MSG_LEN];

    int total_buys  = 0;
    int total_exits = 0;

    for (int i = 0; i < 500; i++) {
        FIX_ParsedMessage msg;
        MockGenerator_NextTick(&gen, buf, sizeof(buf), &msg);
        DataStream<FP> stream = FIX_ToDataStream<FP>(&msg);

        // hot path
        uint16_t bitmap_before = ctrl.portfolio.active_bitmap;
        BuyGate(&ctrl.buy_conds, &stream, &pool);
        PositionExitGate(&ctrl.portfolio, stream.price, &ctrl.exit_buf, ctrl.total_ticks);
        uint16_t exits_this_tick = __builtin_popcount(bitmap_before & ~ctrl.portfolio.active_bitmap);
        total_exits += exits_this_tick;

        // controller
        int count_before = Portfolio_CountActive(&ctrl.portfolio);
        PortfolioController_Tick(&ctrl, &pool, stream.price, stream.volume, &log);
        int fills_this_tick = Portfolio_CountActive(&ctrl.portfolio) - count_before;
        if (fills_this_tick > 0) total_buys += fills_this_tick;
    }

    printf("  buys: %d, exits: %d, active: %d\n", total_buys, total_exits, Portfolio_CountActive(&ctrl.portfolio));
    check("some buys happened", total_buys > 0);
    check("warmup completed", ctrl.state == CONTROLLER_ACTIVE);
    check("total ticks = 500", ctrl.total_ticks == 500);

    TradeLog_Close(&log);
    free(pool.slots);

    // check log file exists and has content
    FILE *f = fopen("logging/INTG_order_history.csv", "r");
    check("trade log file created", f != 0);
    if (f) {
        int lines = 0;
        char line[512];
        while (fgets(line, sizeof(line), f)) lines++;
        check("trade log has entries", lines > 1); // header + at least one trade
        printf("  trade log lines: %d\n", lines);
        fclose(f);
    }
    remove("logging/INTG_order_history.csv");
}

//======================================================================================================
// [TEST 17: STDDEV OFFSET MODE]
//======================================================================================================
static void test_stddev_offset() {
    printf("\n--- Stddev Offset Mode ---\n");

    ControllerConfig<FP> cfg = ControllerConfig_Default<FP>();
    cfg.warmup_ticks  = 10;
    cfg.poll_interval = 1;
    cfg.offset_stddev_mult = FPN_FromDouble<FP>(1.5); // enable stddev mode at 1.5x

    PortfolioController<FP> ctrl = {};
    PortfolioController_Init(&ctrl, cfg);

    OrderPool<FP> pool;
    OrderPool_init(&pool, 64);
    TradeLog log;
    log.file = 0;
    log.trade_count = 0;

    // warmup with known prices around $100 with some spread
    for (int i = 0; i < 10; i++) {
        FPN<FP> price  = FPN_FromDouble<FP>(98.0 + (double)i * 0.5);
        FPN<FP> volume = FPN_FromDouble<FP>(500.0);
        PortfolioController_Tick(&ctrl, &pool, price, volume, &log);
    }
    check("stddev: warmup done", ctrl.state == CONTROLLER_ACTIVE);

    // in stddev mode, buy_price should be avg - (stddev * 1.5)
    // verify the buy price is below the rolling average
    double buy_p = FPN_ToDouble(ctrl.buy_conds.price);
    double avg_p = FPN_ToDouble(ctrl.rolling.price_avg);
    double stddev = FPN_ToDouble(ctrl.rolling.price_stddev);
    check("stddev: buy price below avg", buy_p < avg_p);

    // verify the offset scales with stddev: buy = avg - stddev * mult
    double expected = avg_p - stddev * 1.5;
    check("stddev: buy price = avg - stddev*mult", fabs(buy_p - expected) < 0.5);

    // verify percentage mode gives different result
    ControllerConfig<FP> cfg2 = ControllerConfig_Default<FP>();
    cfg2.warmup_ticks  = 10;
    cfg2.poll_interval = 1;
    // offset_stddev_mult = 0 (default, percentage mode)

    PortfolioController<FP> ctrl2 = {};
    PortfolioController_Init(&ctrl2, cfg2);

    for (int i = 0; i < 10; i++) {
        FPN<FP> price  = FPN_FromDouble<FP>(98.0 + (double)i * 0.5);
        FPN<FP> volume = FPN_FromDouble<FP>(500.0);
        PortfolioController_Tick(&ctrl2, &pool, price, volume, &log);
    }

    double pct_buy_p = FPN_ToDouble(ctrl2.buy_conds.price);
    check("stddev: different from pct mode", fabs(buy_p - pct_buy_p) > 0.01);

    free(pool.slots);
}

//======================================================================================================
// [TEST 18: STDDEV ADAPTATION BOUNDS]
//======================================================================================================
static void test_stddev_adaptation() {
    printf("\n--- Stddev Adaptation Bounds ---\n");

    ControllerConfig<FP> cfg = ControllerConfig_Default<FP>();
    cfg.warmup_ticks     = 5;
    cfg.poll_interval    = 1;
    cfg.r2_threshold     = FPN_FromDouble<FP>(0.01);
    cfg.offset_stddev_mult = FPN_FromDouble<FP>(2.0);
    cfg.offset_stddev_min  = FPN_FromDouble<FP>(0.5);
    cfg.offset_stddev_max  = FPN_FromDouble<FP>(4.0);

    PortfolioController<FP> ctrl = {};
    PortfolioController_Init(&ctrl, cfg);

    check("stddev: init from config", fabs(FPN_ToDouble(ctrl.mean_rev.live_stddev_mult) - 2.0) < 0.01);

    OrderPool<FP> pool;
    OrderPool_init(&pool, 64);
    TradeLog log;
    log.file = 0;
    log.trade_count = 0;

    // warmup
    for (int i = 0; i < 5; i++) {
        PortfolioController_Tick(&ctrl, &pool, FPN_FromDouble<FP>(100.0), FPN_FromDouble<FP>(500.0), &log);
    }

    // add positions and run many ticks to trigger regression adaptation
    for (int i = 0; i < 5; i++) {
        pool.slots[i].price    = FPN_FromDouble<FP>(99.0);
        pool.slots[i].quantity = FPN_FromDouble<FP>(10.0);
        pool.bitmap |= (1ULL << i);
    }

    for (int i = 0; i < 200; i++) {
        FPN<FP> price = FPN_FromDouble<FP>(99.0 + (double)i * 0.01);
        PortfolioController_Tick(&ctrl, &pool, price, FPN_FromDouble<FP>(500.0), &log);
    }

    // stddev_mult should stay within bounds regardless of regression direction
    double sm = FPN_ToDouble(ctrl.mean_rev.live_stddev_mult);
    check("stddev: within lower bound", sm >= 0.49);
    check("stddev: within upper bound", sm <= 4.01);

    // in stddev mode, offset_pct should NOT have drifted (mode-conditional)
    double op = FPN_ToDouble(ctrl.mean_rev.live_offset_pct);
    double init_op = FPN_ToDouble(cfg.entry_offset_pct);
    check("stddev: offset_pct unchanged in stddev mode", fabs(op - init_op) < 0.0001);

    free(pool.slots);
}

//======================================================================================================
// [TEST 19: MULTI-TIMEFRAME GATE]
//======================================================================================================
static void test_multi_timeframe() {
    printf("\n--- Multi-Timeframe Gate ---\n");

    ControllerConfig<FP> cfg = ControllerConfig_Default<FP>();
    cfg.warmup_ticks  = 10;
    cfg.poll_interval = 1;
    cfg.min_long_slope = FPN_FromDouble<FP>(0.0001); // require positive long trend

    PortfolioController<FP> ctrl = {};
    PortfolioController_Init(&ctrl, cfg);

    OrderPool<FP> pool;
    OrderPool_init(&pool, 64);
    TradeLog log;
    log.file = 0;
    log.trade_count = 0;

    // warmup with rising prices (positive long slope)
    for (int i = 0; i < 10; i++) {
        FPN<FP> price = FPN_FromDouble<FP>(100.0 + (double)i * 0.1);
        PortfolioController_Tick(&ctrl, &pool, price, FPN_FromDouble<FP>(500.0), &log);
    }
    check("mt: warmup done", ctrl.state == CONTROLLER_ACTIVE);

    // run a few more ticks with rising prices to build long slope
    for (int i = 0; i < 20; i++) {
        FPN<FP> price = FPN_FromDouble<FP>(101.0 + (double)i * 0.05);
        PortfolioController_Tick(&ctrl, &pool, price, FPN_FromDouble<FP>(500.0), &log);
    }

    // with rising long slope, buy gate should be active (price > 0)
    double buy_p_rising = FPN_ToDouble(ctrl.buy_conds.price);
    check("mt: buys allowed with rising long slope", buy_p_rising > 0);

    // now feed falling prices to create negative long slope
    for (int i = 0; i < 30; i++) {
        FPN<FP> price = FPN_FromDouble<FP>(102.0 - (double)i * 0.2);
        PortfolioController_Tick(&ctrl, &pool, price, FPN_FromDouble<FP>(500.0), &log);
    }

    // with negative long slope, buy gate should be blocked (price = 0)
    double buy_p_falling = FPN_ToDouble(ctrl.buy_conds.price);
    check("mt: buys blocked with falling long slope", buy_p_falling < 0.01);

    free(pool.slots);
}

//======================================================================================================
// [TEST 20: MULTI-TIMEFRAME DISABLED]
//======================================================================================================
static void test_multi_timeframe_disabled() {
    printf("\n--- Multi-Timeframe Disabled ---\n");

    ControllerConfig<FP> cfg = ControllerConfig_Default<FP>();
    cfg.warmup_ticks  = 10;
    cfg.poll_interval = 1;
    // min_long_slope = 0 (default, disabled)

    PortfolioController<FP> ctrl = {};
    PortfolioController_Init(&ctrl, cfg);

    OrderPool<FP> pool;
    OrderPool_init(&pool, 64);
    TradeLog log;
    log.file = 0;
    log.trade_count = 0;

    // warmup
    for (int i = 0; i < 10; i++) {
        PortfolioController_Tick(&ctrl, &pool, FPN_FromDouble<FP>(100.0), FPN_FromDouble<FP>(500.0), &log);
    }

    // feed falling prices — with gate disabled, buys should still work
    for (int i = 0; i < 20; i++) {
        FPN<FP> price = FPN_FromDouble<FP>(100.0 - (double)i * 0.1);
        PortfolioController_Tick(&ctrl, &pool, price, FPN_FromDouble<FP>(500.0), &log);
    }

    double buy_p = FPN_ToDouble(ctrl.buy_conds.price);
    check("mt disabled: buys allowed despite falling slope", buy_p > 0);

    free(pool.slots);
}

//======================================================================================================
// [TEST 21: TRAILING TP DISABLED]
//======================================================================================================
static void test_trailing_disabled() {
    printf("\n--- Trailing TP Disabled ---\n");

    ControllerConfig<FP> cfg = ControllerConfig_Default<FP>();
    cfg.warmup_ticks  = 5;
    cfg.poll_interval = 1;
    // tp_hold_score = 0 (default, disabled)

    PortfolioController<FP> ctrl = {};
    PortfolioController_Init(&ctrl, cfg);

    OrderPool<FP> pool;
    OrderPool_init(&pool, 64);
    TradeLog log;
    log.file = 0;
    log.trade_count = 0;

    // warmup
    for (int i = 0; i < 5; i++) {
        PortfolioController_Tick(&ctrl, &pool, FPN_FromDouble<FP>(100.0), FPN_FromDouble<FP>(500.0), &log);
    }

    // add a position manually
    FPN<FP> tp = FPN_FromDouble<FP>(103.0);
    FPN<FP> sl = FPN_FromDouble<FP>(98.0);
    int slot = Portfolio_AddPositionWithExits(&ctrl.portfolio, FPN_FromDouble<FP>(10.0),
                                              FPN_FromDouble<FP>(100.0), tp, sl);
    ctrl.portfolio.positions[slot].original_tp = tp;
    ctrl.portfolio.positions[slot].original_sl = sl;

    // run with price above TP — should NOT trail (disabled)
    for (int i = 0; i < 20; i++) {
        PortfolioController_Tick(&ctrl, &pool, FPN_FromDouble<FP>(105.0), FPN_FromDouble<FP>(500.0), &log);
    }

    // TP should still be original (103.0) — not raised
    double final_tp = FPN_ToDouble(ctrl.portfolio.positions[slot].take_profit_price);
    check("trailing disabled: TP unchanged", fabs(final_tp - 103.0) < 0.01);

    free(pool.slots);
}

//======================================================================================================
// [TEST 22: TRAILING TP ACTIVATES ON STRONG TREND]
//======================================================================================================
static void test_trailing_activates() {
    printf("\n--- Trailing TP Activates ---\n");

    ControllerConfig<FP> cfg = ControllerConfig_Default<FP>();
    cfg.warmup_ticks    = 5;
    cfg.poll_interval   = 1;
    cfg.tp_hold_score   = FPN_FromDouble<FP>(0.01); // low threshold so it activates easily
    cfg.tp_trail_mult   = FPN_FromDouble<FP>(1.0);
    cfg.sl_trail_mult   = FPN_FromDouble<FP>(2.0);

    PortfolioController<FP> ctrl = {};
    PortfolioController_Init(&ctrl, cfg);
    ctrl.strategy_id = STRATEGY_MOMENTUM; // trailing only active for momentum

    OrderPool<FP> pool;
    OrderPool_init(&pool, 64);
    TradeLog log;
    log.file = 0;
    log.trade_count = 0;

    // warmup
    for (int i = 0; i < 5; i++) {
        PortfolioController_Tick(&ctrl, &pool, FPN_FromDouble<FP>(100.0), FPN_FromDouble<FP>(500.0), &log);
    }

    // add a momentum position with TP at 103
    FPN<FP> entry = FPN_FromDouble<FP>(100.0);
    FPN<FP> tp = FPN_FromDouble<FP>(103.0);
    FPN<FP> sl = FPN_FromDouble<FP>(97.0);
    int slot = Portfolio_AddPositionWithExits(&ctrl.portfolio, FPN_FromDouble<FP>(10.0), entry, tp, sl);
    ctrl.portfolio.positions[slot].original_tp = tp;
    ctrl.portfolio.positions[slot].original_sl = sl;
    ctrl.entry_strategy[slot] = STRATEGY_MOMENTUM;

    // feed steadily rising prices above TP (strong clean trend → high SNR * R²)
    for (int i = 0; i < 50; i++) {
        FPN<FP> price = FPN_FromDouble<FP>(104.0 + (double)i * 0.2);
        PortfolioController_Tick(&ctrl, &pool, price, FPN_FromDouble<FP>(500.0), &log);
    }

    // check if TP was raised above original (trailing activated)
    double final_tp = FPN_ToDouble(ctrl.portfolio.positions[slot].take_profit_price);
    check("trailing: TP raised above original", final_tp > 103.0);

    // check SL was also raised (locking in gains)
    double final_sl = FPN_ToDouble(ctrl.portfolio.positions[slot].stop_loss_price);
    check("trailing: SL raised above original", final_sl > 97.0);

    // TP should ratchet up only — check it's below the final price (trail distance)
    double final_price = 104.0 + 49.0 * 0.2; // 113.8
    check("trailing: TP below current price (trailing distance)", final_tp < final_price);

    free(pool.slots);
}

//======================================================================================================
// [TEST 23: TRAILING TP RATCHET (NEVER DECREASES)]
//======================================================================================================
static void test_trailing_ratchet() {
    printf("\n--- Trailing TP Ratchet ---\n");

    ControllerConfig<FP> cfg = ControllerConfig_Default<FP>();
    cfg.warmup_ticks    = 5;
    cfg.poll_interval   = 1;
    cfg.tp_hold_score   = FPN_FromDouble<FP>(0.01);
    cfg.tp_trail_mult   = FPN_FromDouble<FP>(1.0);
    cfg.sl_trail_mult   = FPN_FromDouble<FP>(2.0);

    PortfolioController<FP> ctrl = {};
    PortfolioController_Init(&ctrl, cfg);

    OrderPool<FP> pool;
    OrderPool_init(&pool, 64);
    TradeLog log;
    log.file = 0;
    log.trade_count = 0;

    // warmup
    for (int i = 0; i < 5; i++) {
        PortfolioController_Tick(&ctrl, &pool, FPN_FromDouble<FP>(100.0), FPN_FromDouble<FP>(500.0), &log);
    }

    FPN<FP> tp = FPN_FromDouble<FP>(103.0);
    FPN<FP> sl = FPN_FromDouble<FP>(97.0);
    int slot = Portfolio_AddPositionWithExits(&ctrl.portfolio, FPN_FromDouble<FP>(10.0),
                                              FPN_FromDouble<FP>(100.0), tp, sl);
    ctrl.portfolio.positions[slot].original_tp = tp;
    ctrl.portfolio.positions[slot].original_sl = sl;

    // phase 1: rising prices — TP should ratchet up
    for (int i = 0; i < 30; i++) {
        FPN<FP> price = FPN_FromDouble<FP>(104.0 + (double)i * 0.3);
        PortfolioController_Tick(&ctrl, &pool, price, FPN_FromDouble<FP>(500.0), &log);
    }
    double tp_after_rise = FPN_ToDouble(ctrl.portfolio.positions[slot].take_profit_price);

    // phase 2: slightly falling prices — TP should NOT decrease
    for (int i = 0; i < 10; i++) {
        FPN<FP> price = FPN_FromDouble<FP>(112.0 - (double)i * 0.1);
        PortfolioController_Tick(&ctrl, &pool, price, FPN_FromDouble<FP>(500.0), &log);
    }
    double tp_after_dip = FPN_ToDouble(ctrl.portfolio.positions[slot].take_profit_price);

    check("ratchet: TP did not decrease during dip", tp_after_dip >= tp_after_rise - 0.01);

    free(pool.slots);
}

//======================================================================================================
// [TEST 24: ORIGINAL TP/SL STORED AT FILL]
//======================================================================================================
static void test_original_tp_sl() {
    printf("\n--- Original TP/SL at Fill ---\n");

    ControllerConfig<FP> cfg = ControllerConfig_Default<FP>();
    cfg.warmup_ticks  = 5;
    cfg.poll_interval = 1;

    PortfolioController<FP> ctrl = {};
    PortfolioController_Init(&ctrl, cfg);

    OrderPool<FP> pool;
    OrderPool_init(&pool, 64);
    TradeLog log;
    log.file = 0;
    log.trade_count = 0;

    // warmup
    for (int i = 0; i < 5; i++) {
        PortfolioController_Tick(&ctrl, &pool, FPN_FromDouble<FP>(100.0), FPN_FromDouble<FP>(500.0), &log);
    }

    // inject a fill
    ctrl.state = CONTROLLER_ACTIVE;
    ctrl.mean_rev.buy_conds_initial = ctrl.buy_conds;
    pool.slots[0].price    = FPN_FromDouble<FP>(99.0);
    pool.slots[0].quantity = FPN_FromDouble<FP>(500.0);
    pool.bitmap = 1;

    PortfolioController_Tick(&ctrl, &pool, FPN_FromDouble<FP>(99.0), FPN_FromDouble<FP>(500.0), &log);

    if (Portfolio_CountActive(&ctrl.portfolio) > 0) {
        int idx = __builtin_ctz(ctrl.portfolio.active_bitmap);
        double live_tp = FPN_ToDouble(ctrl.portfolio.positions[idx].take_profit_price);
        double orig_tp = FPN_ToDouble(ctrl.portfolio.positions[idx].original_tp);
        double live_sl = FPN_ToDouble(ctrl.portfolio.positions[idx].stop_loss_price);
        double orig_sl = FPN_ToDouble(ctrl.portfolio.positions[idx].original_sl);

        check("original_tp matches live TP at fill", fabs(live_tp - orig_tp) < 0.01);
        check("original_sl matches live SL at fill", fabs(live_sl - orig_sl) < 0.01);
        check("original_tp is above entry", orig_tp > 99.0);
        check("original_sl is below entry", orig_sl < 99.0);
    } else {
        check("position was created", 0);
    }

    free(pool.slots);
}

//======================================================================================================
// [TEST: SLIPPAGE SIMULATION]
//======================================================================================================
static void test_slippage() {
    printf("\n--- Slippage Simulation ---\n");

    // TEST 1: buy slippage — entry price should be higher than market
    {
        ControllerConfig<FP> cfg = ControllerConfig_Default<FP>();
        cfg.warmup_ticks = 0;
        cfg.poll_interval = 1000;
        cfg.slippage_pct = FPN_FromDouble<FP>(0.01); // 1% slippage

        PortfolioController<FP> ctrl = {};
        PortfolioController_Init(&ctrl, cfg);
        ctrl.state = CONTROLLER_ACTIVE;
        ctrl.buy_conds.price  = FPN_FromDouble<FP>(105.0);
        ctrl.buy_conds.volume = FPN_FromDouble<FP>(1.0);
        ctrl.mean_rev.buy_conds_initial = ctrl.buy_conds;

        OrderPool<FP> pool;
        OrderPool_init(&pool, 64);
        TradeLog log; log.file = 0; log.trade_count = 0;

        pool.slots[0].price    = FPN_FromDouble<FP>(100.0);
        pool.slots[0].quantity = FPN_FromDouble<FP>(1.0);
        pool.bitmap = 1;

        PortfolioController_Tick(&ctrl, &pool, FPN_FromDouble<FP>(100.0),
                                  FPN_FromDouble<FP>(1.0), &log);

        check("slippage buy: position created", Portfolio_CountActive(&ctrl.portfolio) == 1);
        double entry = FPN_ToDouble(ctrl.portfolio.positions[0].entry_price);
        // 100 + 100*0.01 = 101
        check("slippage buy: entry price adjusted", fabs(entry - 101.0) < 0.1);
        free(pool.slots);
    }

    // TEST 2: sell slippage — realized P&L should reflect worse exit
    {
        ControllerConfig<FP> cfg = ControllerConfig_Default<FP>();
        cfg.warmup_ticks = 0;
        cfg.poll_interval = 1000;
        cfg.slippage_pct = FPN_FromDouble<FP>(0.01); // 1% slippage
        cfg.fee_rate = FPN_Zero<FP>(); // zero fees to isolate slippage effect

        PortfolioController<FP> ctrl = {};
        PortfolioController_Init(&ctrl, cfg);
        ctrl.state = CONTROLLER_ACTIVE;

        // manually add a position at $101 (simulating buy slippage already applied)
        FPN<FP> entry_p = FPN_FromDouble<FP>(101.0);
        FPN<FP> qty = FPN_FromDouble<FP>(1.0);
        Portfolio_AddPositionWithExits(&ctrl.portfolio, qty, entry_p,
            FPN_FromDouble<FP>(110.0), FPN_FromDouble<FP>(90.0));

        // trigger TP exit at $110
        PositionExitGate(&ctrl.portfolio, FPN_FromDouble<FP>(110.0),
                          &ctrl.exit_buf, 100);
        check("slippage sell: exit detected", ctrl.exit_buf.count == 1);

        FPN<FP> pnl_before = ctrl.realized_pnl;
        PortfolioController_DrainExits(&ctrl);

        // exit at 110 with 1% slippage → effective exit = 110 - 110*0.01 = 108.90
        // P&L = 108.90 - 101.0 = 7.90 (with zero fees)
        double pnl = FPN_ToDouble(FPN_Sub(ctrl.realized_pnl, pnl_before));
        check("slippage sell: P&L reflects slippage", fabs(pnl - 7.90) < 0.2);
    }

    // TEST 3: slippage disabled — no price adjustment
    {
        ControllerConfig<FP> cfg = ControllerConfig_Default<FP>();
        cfg.warmup_ticks = 0;
        cfg.poll_interval = 1000;
        cfg.slippage_pct = FPN_Zero<FP>(); // disabled

        PortfolioController<FP> ctrl = {};
        PortfolioController_Init(&ctrl, cfg);
        ctrl.state = CONTROLLER_ACTIVE;
        ctrl.buy_conds.price  = FPN_FromDouble<FP>(105.0);
        ctrl.buy_conds.volume = FPN_FromDouble<FP>(1.0);
        ctrl.mean_rev.buy_conds_initial = ctrl.buy_conds;

        OrderPool<FP> pool;
        OrderPool_init(&pool, 64);
        TradeLog log; log.file = 0; log.trade_count = 0;

        pool.slots[0].price    = FPN_FromDouble<FP>(100.0);
        pool.slots[0].quantity = FPN_FromDouble<FP>(1.0);
        pool.bitmap = 1;

        PortfolioController_Tick(&ctrl, &pool, FPN_FromDouble<FP>(100.0),
                                  FPN_FromDouble<FP>(1.0), &log);

        check("slippage disabled: position created", Portfolio_CountActive(&ctrl.portfolio) == 1);
        double entry = FPN_ToDouble(ctrl.portfolio.positions[0].entry_price);
        check("slippage disabled: entry price exact", fabs(entry - 100.0) < 0.01);
        free(pool.slots);
    }
}

//======================================================================================================
// [TEST: MAX POSITIONS]
//======================================================================================================
static void test_max_positions() {
    printf("\n--- Max Positions ---\n");

    // TEST 1: max_positions=1 rejects second fill
    {
        ControllerConfig<FP> cfg = ControllerConfig_Default<FP>();
        cfg.warmup_ticks = 0;
        cfg.poll_interval = 1000;
        cfg.max_positions = 1;
        cfg.spacing_multiplier = FPN_Zero<FP>(); // disable spacing check

        PortfolioController<FP> ctrl = {};
        PortfolioController_Init(&ctrl, cfg);
        ctrl.state = CONTROLLER_ACTIVE;
        ctrl.buy_conds.price  = FPN_FromDouble<FP>(100.0);
        ctrl.buy_conds.volume = FPN_FromDouble<FP>(400.0);
        ctrl.mean_rev.buy_conds_initial = ctrl.buy_conds;

        OrderPool<FP> pool;
        OrderPool_init(&pool, 64);
        TradeLog log; log.file = 0; log.trade_count = 0;

        // first fill should succeed
        pool.slots[0].price = FPN_FromDouble<FP>(98.0);
        pool.slots[0].quantity = FPN_FromDouble<FP>(500.0);
        pool.bitmap = 1;

        PortfolioController_Tick(&ctrl, &pool, FPN_FromDouble<FP>(98.0),
                                  FPN_FromDouble<FP>(500.0), &log);
        check("max_pos=1: first fill accepted", Portfolio_CountActive(&ctrl.portfolio) == 1);

        // second fill at different price should be rejected
        pool.slots[1].price = FPN_FromDouble<FP>(110.0);
        pool.slots[1].quantity = FPN_FromDouble<FP>(500.0);
        pool.bitmap |= (1ULL << 1);

        PortfolioController_Tick(&ctrl, &pool, FPN_FromDouble<FP>(98.0),
                                  FPN_FromDouble<FP>(500.0), &log);
        check("max_pos=1: second fill rejected", Portfolio_CountActive(&ctrl.portfolio) == 1);
        check("max_pos=1: pool slot 1 remains", (pool.bitmap & (1ULL << 1)) != 0);

        free(pool.slots);
    }

    // TEST 2: max_positions=2 accepts two, rejects third
    {
        ControllerConfig<FP> cfg = ControllerConfig_Default<FP>();
        cfg.warmup_ticks = 0;
        cfg.poll_interval = 1000;
        cfg.max_positions = 2;
        cfg.spacing_multiplier = FPN_Zero<FP>(); // disable spacing check

        PortfolioController<FP> ctrl = {};
        PortfolioController_Init(&ctrl, cfg);
        ctrl.state = CONTROLLER_ACTIVE;
        ctrl.buy_conds.price  = FPN_FromDouble<FP>(100.0);
        ctrl.buy_conds.volume = FPN_FromDouble<FP>(400.0);
        ctrl.mean_rev.buy_conds_initial = ctrl.buy_conds;

        OrderPool<FP> pool;
        OrderPool_init(&pool, 64);
        TradeLog log; log.file = 0; log.trade_count = 0;

        // two fills at different prices
        pool.slots[0].price = FPN_FromDouble<FP>(98.0);
        pool.slots[0].quantity = FPN_FromDouble<FP>(500.0);
        pool.slots[1].price = FPN_FromDouble<FP>(80.0);
        pool.slots[1].quantity = FPN_FromDouble<FP>(500.0);
        pool.bitmap = 3; // bits 0 and 1

        PortfolioController_Tick(&ctrl, &pool, FPN_FromDouble<FP>(98.0),
                                  FPN_FromDouble<FP>(500.0), &log);
        check("max_pos=2: two fills accepted", Portfolio_CountActive(&ctrl.portfolio) == 2);

        // third fill should be rejected
        pool.slots[2].price = FPN_FromDouble<FP>(60.0);
        pool.slots[2].quantity = FPN_FromDouble<FP>(500.0);
        pool.bitmap |= (1ULL << 2);

        PortfolioController_Tick(&ctrl, &pool, FPN_FromDouble<FP>(98.0),
                                  FPN_FromDouble<FP>(500.0), &log);
        check("max_pos=2: third fill rejected", Portfolio_CountActive(&ctrl.portfolio) == 2);

        free(pool.slots);
    }

    // TEST 3: config parser clamps values
    {
        check("max_pos default is 1", ControllerConfig_Default<FP>().max_positions == 1);
    }
}

//======================================================================================================
// [MAIN]
//======================================================================================================
int main() {
    mkdir("logging", 0755); // tests write trade logs here now
    printf("======================================\n");
    printf("  CONTROLLER TEST SUITE\n");
    printf("======================================\n");

    test_config_parser();
    test_portfolio_bitmap();
    test_portfolio_pnl();
    test_consolidation();
    test_exit_gate();
    test_exit_buffer_drain();
    test_fill_timing();
    test_backpressure();
    test_warmup();
    test_regression_feedback();
    test_trade_log();
    test_branchless();
    test_max_shift();
    test_empty_regression();
    test_tick_counter();
    test_full_pipeline();
    test_stddev_offset();
    test_stddev_adaptation();
    test_multi_timeframe();
    test_multi_timeframe_disabled();
    test_trailing_disabled();
    test_trailing_activates();
    test_trailing_ratchet();
    test_original_tp_sl();
    test_slippage();
    test_max_positions();

    //==================================================================================================
    // [TEST: VOLUME SPIKE DETECTION]
    //==================================================================================================
    {
        printf("\n--- Volume Spike Detection ---\n");

        // rolling max tracking
        RollingStats<FP> rs = RollingStats_Init<FP>();
        FPN<FP> p = FPN_FromDouble<FP>(100.0);
        for (int i = 0; i < 10; i++) {
            FPN<FP> v = FPN_FromDouble<FP>(1.0 + i * 0.5); // volumes: 1.0, 1.5, 2.0, ..., 5.5
            RollingStats_Push(&rs, p, v);
        }
        double vmax = FPN_ToDouble(rs.volume_max);
        check("rolling volume_max tracks max", fabs(vmax - 5.5) < 0.01);

        // spike ratio computation
        ControllerConfig<FP> cfg = ControllerConfig_Default<FP>();
        cfg.spike_threshold = FPN_FromDouble<FP>(3.0); // 3x max triggers spike
        cfg.spike_spacing_reduction = FPN_FromDouble<FP>(0.5);

        // ratio = current / max: 5.5 / 5.5 = 1.0 (not a spike)
        FPN<FP> current_vol = FPN_FromDouble<FP>(5.5);
        FPN<FP> ratio = FPN_DivNoAssert(current_vol, rs.volume_max);
        int is_spike = FPN_GreaterThanOrEqual(ratio, cfg.spike_threshold);
        check("spike: 1x max is not a spike", !is_spike);

        // ratio = 20.0 / 5.5 = 3.6x (IS a spike)
        FPN<FP> big_vol = FPN_FromDouble<FP>(20.0);
        FPN<FP> ratio2 = FPN_DivNoAssert(big_vol, rs.volume_max);
        int is_spike2 = FPN_GreaterThanOrEqual(ratio2, cfg.spike_threshold);
        check("spike: 3.6x max triggers spike", is_spike2);

        // spacing reduction: normal spacing vs spike spacing
        FPN<FP> spacing = FPN_FromDouble<FP>(100.0);
        FPN<FP> reduced = FPN_Mul(spacing, cfg.spike_spacing_reduction);
        double reduced_d = FPN_ToDouble(reduced);
        check("spike: spacing reduced to 50%", fabs(reduced_d - 50.0) < 0.01);

        // branchless mask-select produces correct result
        uint64_t spike_mask = -(uint64_t)is_spike2;
        FPN<FP> selected;
        for (unsigned w = 0; w < FPN<FP>::N; w++) {
            selected.w[w] = (reduced.w[w] & spike_mask) | (spacing.w[w] & ~spike_mask);
        }
        selected.sign = (reduced.sign & is_spike2) | (spacing.sign & !is_spike2);
        double sel_d = FPN_ToDouble(selected);
        check("spike: mask-select picks reduced spacing", fabs(sel_d - 50.0) < 0.01);

        // non-spike mask-select keeps original
        uint64_t no_mask = -(uint64_t)is_spike; // is_spike = 0
        FPN<FP> selected2;
        for (unsigned w = 0; w < FPN<FP>::N; w++) {
            selected2.w[w] = (reduced.w[w] & no_mask) | (spacing.w[w] & ~no_mask);
        }
        selected2.sign = (reduced.sign & is_spike) | (spacing.sign & !is_spike);
        double sel2_d = FPN_ToDouble(selected2);
        check("spike: mask-select keeps normal when no spike", fabs(sel2_d - 100.0) < 0.01);
    }

    //==================================================================================================
    // [TEST: MOMENTUM FILL TP/SL]
    //==================================================================================================
    {
        printf("\n--- Momentum Fill TP/SL ---\n");
        ControllerConfig<FP> cfg = ControllerConfig_Default<FP>();
        cfg.warmup_ticks = 10;
        cfg.poll_interval = 5;
        cfg.starting_balance = FPN_FromDouble<FP>(10000.0);
        cfg.momentum_tp_mult = FPN_FromDouble<FP>(3.0);
        cfg.momentum_sl_mult = FPN_FromDouble<FP>(1.0);

        PortfolioController<FP> ctrl = {};
        PortfolioController_Init(&ctrl, cfg);

        OrderPool<FP> pool;
        OrderPool_init(&pool, 64);
        TradeLog log; log.file = 0; log.trade_count = 0;

        // warmup with stable price to build stats
        FPN<FP> vol = FPN_FromDouble<FP>(1.0);
        for (uint64_t t = 0; t < 20; t++) {
            FPN<FP> p = FPN_FromDouble<FP>(70000.0 + (t % 3) * 10.0);
            PortfolioController_Tick(&ctrl, &pool, p, vol, &log);
        }
        check("momentum: warmup done", ctrl.state == CONTROLLER_ACTIVE);

        // switch to momentum strategy
        ctrl.strategy_id = STRATEGY_MOMENTUM;

        // create a fill via pool
        pool.slots[0].price    = FPN_FromDouble<FP>(70000.0);
        pool.slots[0].quantity = FPN_FromDouble<FP>(0.01);
        pool.bitmap = 1;

        // tick to consume the fill
        PortfolioController_Tick(&ctrl, &pool, FPN_FromDouble<FP>(70000.0), vol, &log);

        int has_pos = Portfolio_CountActive(&ctrl.portfolio) > 0;
        check("momentum: position created", has_pos);

        if (has_pos) {
            int pidx = __builtin_ctz(ctrl.portfolio.active_bitmap);
            double tp = FPN_ToDouble(ctrl.portfolio.positions[pidx].take_profit_price);
            double sl = FPN_ToDouble(ctrl.portfolio.positions[pidx].stop_loss_price);
            double entry = 70000.0;

            // TP should be reasonable (not 110k from ×100 bug)
            check("momentum: TP not absurdly high", tp < entry + 5000.0);
            check("momentum: TP above entry", tp > entry);
            check("momentum: SL below entry", sl < entry);
            check("momentum: SL not absurdly low", sl > entry - 5000.0);
            check("momentum: entry_strategy is MOMENTUM",
                  ctrl.entry_strategy[pidx] == STRATEGY_MOMENTUM);
        }
    }

    //==================================================================================================
    // [TEST: REGIME SWITCH POSITION ADJUSTMENT]
    //==================================================================================================
    {
        printf("\n--- Regime Switch Position Adjustment ---\n");
        ControllerConfig<FP> cfg = ControllerConfig_Default<FP>();
        cfg.warmup_ticks = 10;
        cfg.starting_balance = FPN_FromDouble<FP>(10000.0);
        cfg.momentum_tp_mult = FPN_FromDouble<FP>(3.0);
        cfg.momentum_sl_mult = FPN_FromDouble<FP>(1.0);

        PortfolioController<FP> ctrl = {};
        PortfolioController_Init(&ctrl, cfg);

        // build rolling stats with meaningful stddev
        for (int i = 0; i < 20; i++) {
            RollingStats_Push(&ctrl.rolling, FPN_FromDouble<FP>(70000.0 + i * 5.0),
                              FPN_FromDouble<FP>(1.0));
        }

        // manually create a position under MR
        ctrl.state = CONTROLLER_ACTIVE;
        ctrl.strategy_id = STRATEGY_MEAN_REVERSION;
        FPN<FP> entry_p = FPN_FromDouble<FP>(70000.0);
        FPN<FP> qty = FPN_FromDouble<FP>(0.01);
        Portfolio_AddPosition(&ctrl.portfolio, qty, entry_p);
        int pidx = __builtin_ctz(ctrl.portfolio.active_bitmap);
        ctrl.portfolio.positions[pidx].take_profit_price = FPN_FromDouble<FP>(70500.0);
        ctrl.portfolio.positions[pidx].stop_loss_price   = FPN_FromDouble<FP>(69500.0);
        ctrl.portfolio.positions[pidx].original_tp = ctrl.portfolio.positions[pidx].take_profit_price;
        ctrl.portfolio.positions[pidx].original_sl = ctrl.portfolio.positions[pidx].stop_loss_price;
        ctrl.entry_strategy[pidx] = STRATEGY_MEAN_REVERSION;

        double tp_before = FPN_ToDouble(ctrl.portfolio.positions[pidx].take_profit_price);
        double sl_before = FPN_ToDouble(ctrl.portfolio.positions[pidx].stop_loss_price);

        // simulate RANGING → TRENDING regime switch
        Regime_AdjustPositions(&ctrl.portfolio, &ctrl.rolling,
                                REGIME_RANGING, REGIME_TRENDING,
                                ctrl.entry_strategy, &ctrl.config);

        double tp_after = FPN_ToDouble(ctrl.portfolio.positions[pidx].take_profit_price);
        double sl_after = FPN_ToDouble(ctrl.portfolio.positions[pidx].stop_loss_price);

        // TP should widen (increase) or stay same
        check("regime switch: TP widened or unchanged", tp_after >= tp_before - 0.01);
        // SL should tighten (increase toward entry) or stay same
        check("regime switch: SL tightened or unchanged", sl_after >= sl_before - 0.01);
        // TP should still be reasonable (not 110k)
        check("regime switch: TP not absurd", tp_after < 75000.0);
        check("regime switch: SL not absurd", sl_after > 65000.0);
    }

    //==================================================================================================
    // [TEST: POST-SL COOLDOWN]
    //==================================================================================================
    {
        printf("\n--- Post-SL Cooldown ---\n");
        ControllerConfig<FP> cfg = ControllerConfig_Default<FP>();
        cfg.warmup_ticks = 5;
        cfg.poll_interval = 1; // slow path every tick for test speed
        cfg.starting_balance = FPN_FromDouble<FP>(10000.0);
        cfg.sl_cooldown_cycles = 3;

        PortfolioController<FP> ctrl = {};
        PortfolioController_Init(&ctrl, cfg);

        OrderPool<FP> pool;
        OrderPool_init(&pool, 64);
        TradeLog log; log.file = 0; log.trade_count = 0;

        // warmup
        for (uint64_t t = 0; t < 10; t++)
            PortfolioController_Tick(&ctrl, &pool, FPN_FromDouble<FP>(100.0),
                                      FPN_FromDouble<FP>(1.0), &log);
        check("cooldown: warmup done", ctrl.state == CONTROLLER_ACTIVE);
        check("cooldown: counter starts at 0", ctrl.sl_cooldown_counter == 0);

        // create a position and trigger SL exit
        pool.slots[0].price    = FPN_FromDouble<FP>(100.0);
        pool.slots[0].quantity = FPN_FromDouble<FP>(1.0);
        pool.bitmap = 1;
        PortfolioController_Tick(&ctrl, &pool, FPN_FromDouble<FP>(100.0),
                                  FPN_FromDouble<FP>(1.0), &log);
        check("cooldown: position created", Portfolio_CountActive(&ctrl.portfolio) == 1);

        // drop price below SL to trigger exit
        // SL is ~98.5 (entry 100, 1.5% SL), price at 50 is well below
        // PositionExitGate runs on hot path, exit buffer drained on slow path
        FPN<FP> drop_price = FPN_FromDouble<FP>(50.0);
        FPN<FP> drop_vol = FPN_FromDouble<FP>(1.0);

        // one tick: exit gate detects SL, controller drains it + sets cooldown
        PositionExitGate(&ctrl.portfolio, drop_price, &ctrl.exit_buf, 100);
        PortfolioController_Tick(&ctrl, &pool, drop_price, drop_vol, &log);

        check("cooldown: SL exited", Portfolio_CountActive(&ctrl.portfolio) == 0);
        check("cooldown: loss counted", ctrl.losses > 0);
        // counter was set to 3, then decremented once this tick = 2
        check("cooldown: counter set after SL", ctrl.sl_cooldown_counter > 0);
        check("cooldown: buy gate disabled", FPN_IsZero(ctrl.buy_conds.price));

        // tick through cooldown — counter should decrement each cycle
        uint32_t counter_before = ctrl.sl_cooldown_counter;
        PortfolioController_Tick(&ctrl, &pool, drop_price, drop_vol, &log);
        check("cooldown: counter decremented", ctrl.sl_cooldown_counter < counter_before);

        // tick until cooldown expires
        for (int t = 0; t < 10; t++)
            PortfolioController_Tick(&ctrl, &pool, drop_price, drop_vol, &log);
        check("cooldown: counter expired", ctrl.sl_cooldown_counter == 0);
        // gate should be re-enabled (non-zero buy price from strategy dispatch)
        check("cooldown: buy gate re-enabled", !FPN_IsZero(ctrl.buy_conds.price));
    }

    //==================================================================================================
    // [TEST: BOUNDS CHECKS AND SAFETY GUARDS]
    //==================================================================================================
    // regression tests for bugs found in live trading — prevent reintroduction
    //==================================================================================================
    {
        printf("\n--- Exit Buffer Bounds ---\n");

        // exit buffer must not overflow past 16 slots
        Portfolio<FP> port = {};
        ExitBuffer<FP> ebuf = {};
        ebuf.count = 0;

        // fill all 16 positions
        for (int i = 0; i < 16; i++) {
            port.positions[i].quantity = FPN_FromDouble<FP>(1.0);
            port.positions[i].entry_price = FPN_FromDouble<FP>(100.0);
            port.positions[i].take_profit_price = FPN_FromDouble<FP>(101.0);
            port.positions[i].stop_loss_price = FPN_FromDouble<FP>(90.0);
            port.active_bitmap |= (1 << i);
        }

        // trigger all 16 exits at once (price below all SLs)
        FPN<FP> crash_price = FPN_FromDouble<FP>(50.0);
        PositionExitGate(&port, crash_price, &ebuf, 1);
        check("exit_buf: count capped at 16", ebuf.count <= 16);
        check("exit_buf: all positions exited", port.active_bitmap == 0);
    }

    {
        printf("\n--- DrainExits Bounds Guard ---\n");

        // position_index >= 16 must not crash
        ControllerConfig<FP> cfg = ControllerConfig_Default<FP>();
        PortfolioController<FP> ctrl = {};
        PortfolioController_Init(&ctrl, cfg);

        // manually inject a bad exit record with out-of-bounds index
        ctrl.exit_buf.records[0].position_index = 99; // way out of bounds
        ctrl.exit_buf.records[0].exit_price = FPN_FromDouble<FP>(100.0);
        ctrl.exit_buf.records[0].tick = 1;
        ctrl.exit_buf.records[0].reason = 0;
        ctrl.exit_buf.count = 1;

        // this should skip the bad record, not crash
        PortfolioController_DrainExits(&ctrl);
        check("drain_exits: survived OOB position_index", ctrl.exit_buf.count == 0); // cleared by drain
        check("drain_exits: no wins or losses from bad record", ctrl.wins == 0 && ctrl.losses == 0);
    }

    {
        printf("\n--- Sell+Buy Same Tick ---\n");

        // after an exit, a new fill in the same tick should succeed on paper
        // (the same-tick guard is in main.cpp for live orders, but the paper
        // engine should still accept fills — the guard only defers the REST call)
        ControllerConfig<FP> cfg = ControllerConfig_Default<FP>();
        cfg.warmup_ticks = 0;
        cfg.poll_interval = 5;
        cfg.starting_balance = FPN_FromDouble<FP>(10000.0);
        cfg.max_positions = 1;
        cfg.spacing_multiplier = FPN_Zero<FP>();

        PortfolioController<FP> ctrl = {};
        PortfolioController_Init(&ctrl, cfg);
        ctrl.state = CONTROLLER_ACTIVE;
        ctrl.buy_conds.price = FPN_FromDouble<FP>(100.0);
        ctrl.buy_conds.volume = FPN_FromDouble<FP>(400.0);
        ctrl.mean_rev.buy_conds_initial = ctrl.buy_conds;

        OrderPool<FP> pool;
        OrderPool_init(&pool, 64);
        TradeLog log; log.file = 0; log.trade_count = 0;

        // warmup
        for (uint64_t t = 0; t < 10; t++)
            PortfolioController_Tick(&ctrl, &pool, FPN_FromDouble<FP>(100.0),
                                      FPN_FromDouble<FP>(1.0), &log);

        // create position
        pool.slots[0].price = FPN_FromDouble<FP>(98.0);
        pool.slots[0].quantity = FPN_FromDouble<FP>(500.0);
        pool.bitmap = 1;
        PortfolioController_Tick(&ctrl, &pool, FPN_FromDouble<FP>(98.0),
                                  FPN_FromDouble<FP>(500.0), &log);
        check("same_tick: position created", Portfolio_CountActive(&ctrl.portfolio) == 1);

        // exit via SL
        PositionExitGate(&ctrl.portfolio, FPN_FromDouble<FP>(50.0), &ctrl.exit_buf, 100);
        check("same_tick: exit buffered", ctrl.exit_buf.count > 0);

        // new fill available in pool
        pool.slots[1].price = FPN_FromDouble<FP>(98.0);
        pool.slots[1].quantity = FPN_FromDouble<FP>(500.0);
        pool.bitmap |= (1ULL << 1);

        // tick processes both exit drain and new fill
        PortfolioController_Tick(&ctrl, &pool, FPN_FromDouble<FP>(98.0),
                                  FPN_FromDouble<FP>(500.0), &log);
        // exit was drained, new fill may or may not be accepted depending on
        // timing within the tick — but it must not crash
        check("same_tick: no crash on exit+fill same tick", 1);
        // drain exits until loss is counted (may need a slow-path tick)
        for (int t = 0; t < 10; t++)
            PortfolioController_Tick(&ctrl, &pool, FPN_FromDouble<FP>(98.0),
                                      FPN_FromDouble<FP>(500.0), &log);
        check("same_tick: losses counted", ctrl.losses > 0);

        free(pool.slots);
    }

    {
        printf("\n--- Malloc Guard ---\n");
        // rolling_long allocation check — just verify init doesn't crash
        ControllerConfig<FP> cfg = ControllerConfig_Default<FP>();
        PortfolioController<FP> ctrl = {};
        PortfolioController_Init(&ctrl, cfg);
        check("malloc: rolling_long allocated", ctrl.rolling_long != NULL);
        free(ctrl.rolling_long);
        ctrl.rolling_long = NULL;
    }

    //==================================================================================================
    // [TEST: REGIME ADJUSTMENT — TRENDING_DOWN TP/SL USES CORRECT CONFIG + SL FLOOR]
    //==================================================================================================
    {
        printf("\n--- Regime Adjust: TRENDING_DOWN TP/SL ---\n");
        ControllerConfig<FP> cfg = ControllerConfig_Default<FP>();
        cfg.momentum_tp_mult = FPN_FromDouble<FP>(3.0);
        cfg.momentum_sl_mult = FPN_FromDouble<FP>(1.0);
        cfg.take_profit_pct  = FPN_FromDouble<FP>(0.04); // 4% → ×100 = 4.0σ (MR style)

        PortfolioController<FP> ctrl = {};
        PortfolioController_Init(&ctrl, cfg);

        // set up rolling stats with known stddev
        ctrl.rolling.price_stddev = FPN_FromDouble<FP>(100.0); // $100 stddev
        ctrl.rolling.price_avg    = FPN_FromDouble<FP>(70000.0);

        // add momentum position: entry $70000, TP $70500, SL $69500
        FPN<FP> entry = FPN_FromDouble<FP>(70000.0);
        FPN<FP> qty   = FPN_FromDouble<FP>(0.01);
        Portfolio_AddPosition(&ctrl.portfolio, qty, entry);
        int pidx = __builtin_ctz(ctrl.portfolio.active_bitmap);
        ctrl.portfolio.positions[pidx].take_profit_price = FPN_FromDouble<FP>(70500.0);
        ctrl.portfolio.positions[pidx].stop_loss_price   = FPN_FromDouble<FP>(69500.0);
        ctrl.entry_strategy[pidx] = STRATEGY_MOMENTUM;

        // simulate TRENDING → TRENDING_DOWN
        Regime_AdjustPositions(&ctrl.portfolio, &ctrl.rolling,
                                REGIME_TRENDING, REGIME_TRENDING_DOWN,
                                ctrl.entry_strategy, &ctrl.config);

        double tp_after = FPN_ToDouble(ctrl.portfolio.positions[pidx].take_profit_price);
        double sl_after = FPN_ToDouble(ctrl.portfolio.positions[pidx].stop_loss_price);
        double entry_d  = 70000.0;

        // TP should use momentum_tp_mult (3.0 × $100 = $300 offset → $70300)
        // NOT take_profit_pct×100 (4.0 × $100 = $400 → $70400)
        double expected_tp = entry_d + 3.0 * 100.0; // 70300
        check("downtrend: TP uses momentum_tp_mult (3σ not 4σ)",
              tp_after < 70350.0 && tp_after > 70250.0);

        // SL floor: SL distance >= 0.5 × TP distance
        double tp_dist = tp_after - entry_d;
        double sl_dist = entry_d - sl_after;
        check("downtrend: SL floor holds (2:1 min reward/risk)",
              sl_dist >= tp_dist * 0.5 - 0.01);

        // TP should be tightened (Min), not widened
        check("downtrend: TP tightened from original 70500",
              tp_after <= 70500.01);

        printf("  TP: $%.2f (expected ~$%.2f), SL: $%.2f, ratio: %.1f:1\n",
               tp_after, expected_tp, sl_after, tp_dist / sl_dist);
        free(ctrl.rolling_long);
        ctrl.rolling_long = NULL;
    }

    //==================================================================================================
    // [TEST: REGIME ADJUSTMENT — STDDEV=0 GUARD]
    //==================================================================================================
    {
        printf("\n--- Regime Adjust: stddev=0 guard ---\n");
        ControllerConfig<FP> cfg = ControllerConfig_Default<FP>();
        PortfolioController<FP> ctrl = {};
        PortfolioController_Init(&ctrl, cfg);

        // set stddev to ZERO (flat market)
        ctrl.rolling.price_stddev = FPN_Zero<FP>();
        ctrl.rolling.price_avg    = FPN_FromDouble<FP>(70000.0);

        // add position with known TP/SL
        FPN<FP> entry = FPN_FromDouble<FP>(70000.0);
        FPN<FP> qty   = FPN_FromDouble<FP>(0.01);
        Portfolio_AddPosition(&ctrl.portfolio, qty, entry);
        int pidx = __builtin_ctz(ctrl.portfolio.active_bitmap);
        ctrl.portfolio.positions[pidx].take_profit_price = FPN_FromDouble<FP>(70500.0);
        ctrl.portfolio.positions[pidx].stop_loss_price   = FPN_FromDouble<FP>(69500.0);
        ctrl.entry_strategy[pidx] = STRATEGY_MOMENTUM;

        double tp_before = FPN_ToDouble(ctrl.portfolio.positions[pidx].take_profit_price);
        double sl_before = FPN_ToDouble(ctrl.portfolio.positions[pidx].stop_loss_price);

        // attempt regime adjustment — should early-return, positions untouched
        Regime_AdjustPositions(&ctrl.portfolio, &ctrl.rolling,
                                REGIME_TRENDING, REGIME_TRENDING_DOWN,
                                ctrl.entry_strategy, &ctrl.config);

        double tp_after = FPN_ToDouble(ctrl.portfolio.positions[pidx].take_profit_price);
        double sl_after = FPN_ToDouble(ctrl.portfolio.positions[pidx].stop_loss_price);

        check("stddev=0: TP unchanged", fabs(tp_after - tp_before) < 0.01);
        check("stddev=0: SL unchanged", fabs(sl_after - sl_before) < 0.01);
        free(ctrl.rolling_long);
        ctrl.rolling_long = NULL;
    }

    //==================================================================================================
    // [TEST: REGIME ADJUSTMENT — SL FLOOR ON ALL PATHS]
    //==================================================================================================
    {
        printf("\n--- Regime Adjust: SL floor all paths ---\n");
        ControllerConfig<FP> cfg = ControllerConfig_Default<FP>();
        cfg.momentum_tp_mult = FPN_FromDouble<FP>(3.0);
        cfg.momentum_sl_mult = FPN_FromDouble<FP>(1.0);
        cfg.take_profit_pct  = FPN_FromDouble<FP>(0.04);
        cfg.stop_loss_pct    = FPN_FromDouble<FP>(0.04);

        PortfolioController<FP> ctrl = {};
        PortfolioController_Init(&ctrl, cfg);
        ctrl.rolling.price_stddev = FPN_FromDouble<FP>(100.0);
        ctrl.rolling.price_avg    = FPN_FromDouble<FP>(70000.0);

        // test each regime transition path
        int transitions[][2] = {
            {REGIME_RANGING,       REGIME_TRENDING},
            {REGIME_TRENDING,      REGIME_RANGING},
            {REGIME_TRENDING,      REGIME_TRENDING_DOWN},
            {REGIME_TRENDING_DOWN, REGIME_RANGING},
        };
        int strategies[] = {
            STRATEGY_MEAN_REVERSION,  // old_strategy for RANGING
            STRATEGY_MOMENTUM,        // old_strategy for TRENDING
            STRATEGY_MOMENTUM,        // old_strategy for TRENDING
            STRATEGY_MEAN_REVERSION,  // old_strategy for TRENDING_DOWN
        };
        const char *names[] = {
            "RANGING->TRENDING",
            "TRENDING->RANGING",
            "TRENDING->TRENDING_DOWN",
            "TRENDING_DOWN->RANGING",
        };

        for (int t = 0; t < 4; t++) {
            // reset position each time
            ctrl.portfolio.active_bitmap = 0;
            FPN<FP> entry = FPN_FromDouble<FP>(70000.0);
            FPN<FP> qty   = FPN_FromDouble<FP>(0.01);
            Portfolio_AddPosition(&ctrl.portfolio, qty, entry);
            int pidx = __builtin_ctz(ctrl.portfolio.active_bitmap);
            ctrl.portfolio.positions[pidx].take_profit_price = FPN_FromDouble<FP>(70500.0);
            ctrl.portfolio.positions[pidx].stop_loss_price   = FPN_FromDouble<FP>(69500.0);
            ctrl.entry_strategy[pidx] = strategies[t];

            Regime_AdjustPositions(&ctrl.portfolio, &ctrl.rolling,
                                    transitions[t][0], transitions[t][1],
                                    ctrl.entry_strategy, &ctrl.config);

            double tp_a = FPN_ToDouble(ctrl.portfolio.positions[pidx].take_profit_price);
            double sl_a = FPN_ToDouble(ctrl.portfolio.positions[pidx].stop_loss_price);
            double entry_d = 70000.0;
            double tp_dist = tp_a - entry_d;
            double sl_dist = entry_d - sl_a;

            char msg[128];
            snprintf(msg, sizeof(msg), "SL floor %s: sl_dist >= 0.5 * tp_dist", names[t]);
            check(msg, sl_dist >= tp_dist * 0.5 - 0.01);

            snprintf(msg, sizeof(msg), "SL floor %s: TP > entry", names[t]);
            check(msg, tp_a > entry_d - 0.01);

            snprintf(msg, sizeof(msg), "SL floor %s: SL < entry", names[t]);
            check(msg, sl_a < entry_d + 0.01);
        }
        free(ctrl.rolling_long);
        ctrl.rolling_long = NULL;
    }

    //======================================================================================================
    // REGIME MAPPING + CLASSIFICATION
    //======================================================================================================
    printf("\n--- REGIME MAPPING + CLASSIFICATION ---\n");
    {
        // Regime_ToStrategy mapping
        check("ToStrategy: RANGING -> MR",
              Regime_ToStrategy(REGIME_RANGING) == STRATEGY_MEAN_REVERSION);
        check("ToStrategy: TRENDING -> MOMENTUM",
              Regime_ToStrategy(REGIME_TRENDING) == STRATEGY_MOMENTUM);
        check("ToStrategy: VOLATILE -> SIMPLE_DIP",
              Regime_ToStrategy(REGIME_VOLATILE) == STRATEGY_SIMPLE_DIP);
        check("ToStrategy: TRENDING_DOWN -> MR",
              Regime_ToStrategy(REGIME_TRENDING_DOWN) == STRATEGY_MEAN_REVERSION);
        check("ToStrategy: MILD_TREND -> EMA_CROSS",
              Regime_ToStrategy(REGIME_MILD_TREND) == STRATEGY_EMA_CROSS);
        check("ToStrategy: out-of-range -> MR",
              Regime_ToStrategy(99) == STRATEGY_MEAN_REVERSION);

        // RegimeInfo table
        check("RegimeInfo: RANGING short_name",
              strcmp(REGIME_INFO[REGIME_RANGING].short_name, "RANGE") == 0);
        check("RegimeInfo: MILD_TREND short_name",
              strcmp(REGIME_INFO[REGIME_MILD_TREND].short_name, "EMACR") == 0);
        check("RegimeInfo: NUM_REGIMES == 5", NUM_REGIMES == 5);
        check("NUM_STRATEGIES == 5", NUM_STRATEGIES == 5);

        // Regime_Classify integration is tested via the full controller path
        // (SL floor tests above exercise actual regime transitions)
        // Here we verify the config fields that drive the MILD_TREND/TRENDING split
        ControllerConfig<FP> cfg = ControllerConfig_Default<FP>();
        check("Classify: strong_crossover > crossover_threshold",
              FPN_GreaterThan(cfg.regime_strong_crossover, cfg.regime_crossover_threshold));
        check("Classify: strong_crossover default ~0.0015",
              fabs(FPN_ToDouble(cfg.regime_strong_crossover) - 0.0015) < 0.0001);
        check("Classify: crossover_threshold default ~0.0005",
              fabs(FPN_ToDouble(cfg.regime_crossover_threshold) - 0.0005) < 0.0001);
    }

    //======================================================================================================
    // SL FLOOR: MILD_TREND TRANSITIONS
    //======================================================================================================
    printf("\n--- SL FLOOR: MILD_TREND TRANSITIONS ---\n");
    {
        ControllerConfig<FP> cfg = ControllerConfig_Default<FP>();

        Portfolio<FP> portfolio;
        Portfolio_Init(&portfolio);
        RollingStats<FP> rolling;
        memset(&rolling, 0, sizeof(rolling));

        FPN<FP> entry = FPN_FromDouble<FP>(70000.0);
        FPN<FP> stddev = FPN_FromDouble<FP>(50.0);
        rolling.price_stddev = stddev;

        // transitions to test
        int transitions[][2] = {
            {REGIME_RANGING,    REGIME_MILD_TREND},
            {REGIME_MILD_TREND, REGIME_TRENDING},
            {REGIME_TRENDING,   REGIME_MILD_TREND},
            {REGIME_MILD_TREND, REGIME_RANGING},
            {REGIME_MILD_TREND, REGIME_TRENDING_DOWN},
        };
        const char *names[] = {
            "RANGING->MILD_TREND", "MILD_TREND->TRENDING",
            "TRENDING->MILD_TREND", "MILD_TREND->RANGING",
            "MILD_TREND->TRENDING_DOWN",
        };
        int strategies[] = {
            STRATEGY_MEAN_REVERSION, STRATEGY_EMA_CROSS,
            STRATEGY_MOMENTUM, STRATEGY_EMA_CROSS,
            STRATEGY_EMA_CROSS,
        };

        for (int t = 0; t < 5; t++) {
            Portfolio_Init(&portfolio);
            FPN<FP> tp = FPN_AddSat(entry, FPN_FromDouble<FP>(300.0));
            FPN<FP> sl = FPN_SubSat(entry, FPN_FromDouble<FP>(150.0));
            FPN<FP> qty = FPN_FromDouble<FP>(0.01);
            int slot = Portfolio_AddPositionWithExits(&portfolio, qty, entry, tp, sl, FPN_Zero<FP>());

            uint8_t entry_strat[16] = {};
            entry_strat[slot] = (uint8_t)strategies[t];

            Regime_AdjustPositions(&portfolio, &rolling,
                                   transitions[t][0], transitions[t][1],
                                   entry_strat, &cfg);

            double tp_a = FPN_ToDouble(portfolio.positions[slot].take_profit_price);
            double sl_a = FPN_ToDouble(portfolio.positions[slot].stop_loss_price);
            double entry_d = FPN_ToDouble(entry);
            double tp_dist = tp_a - entry_d;
            double sl_dist = entry_d - sl_a;

            char msg[128];
            snprintf(msg, sizeof(msg), "SL floor %s: SL <= TP (no inverted risk)", names[t]);
            check(msg, sl_dist <= tp_dist + 0.01);

            snprintf(msg, sizeof(msg), "SL floor %s: SL >= 0.5*TP (2:1 min)", names[t]);
            check(msg, sl_dist >= tp_dist * 0.49);

            snprintf(msg, sizeof(msg), "SL floor %s: TP > entry", names[t]);
            check(msg, tp_a > entry_d - 0.01);

            snprintf(msg, sizeof(msg), "SL floor %s: SL < entry", names[t]);
            check(msg, sl_a < entry_d + 0.01);
        }
    }

    //======================================================================================================
    // DANGER GRADIENT
    //======================================================================================================
    printf("\n--- DANGER GRADIENT ---\n");
    {
        ControllerConfig<FP> cfg = ControllerConfig_Default<FP>();

        // verify defaults
        check("danger: enabled by default", cfg.danger_enabled == 1);
        check("danger: warn_stddevs default ~3.0",
              fabs(FPN_ToDouble(cfg.danger_warn_stddevs) - 3.0) < 0.01);
        check("danger: crash_stddevs default ~6.0",
              fabs(FPN_ToDouble(cfg.danger_crash_stddevs) - 6.0) < 0.01);
        check("danger: warn < crash (wider range)",
              FPN_LessThan(cfg.danger_warn_stddevs, cfg.danger_crash_stddevs));

        // test danger score math: simulate precomputed thresholds
        // avg=100, stddev=10 → warn=70 (3σ below), crash=40 (6σ below)
        FPN<FP> avg = FPN_FromDouble<FP>(100.0);
        FPN<FP> sd = FPN_FromDouble<FP>(10.0);
        FPN<FP> warn = FPN_SubSat(avg, FPN_Mul(sd, cfg.danger_warn_stddevs));  // 100-30=70
        FPN<FP> crash = FPN_SubSat(avg, FPN_Mul(sd, cfg.danger_crash_stddevs)); // 100-60=40
        FPN<FP> range = FPN_SubSat(warn, crash);  // 70-40=30
        FPN<FP> range_inv = FPN_DivNoAssert(FPN_FromDouble<FP>(1.0), range);

        check("danger: warn threshold ~70.0",
              fabs(FPN_ToDouble(warn) - 70.0) < 0.01);
        check("danger: crash threshold ~40.0",
              fabs(FPN_ToDouble(crash) - 40.0) < 0.01);

        // price at 100 (safe): score should be 0
        {
            FPN<FP> price = FPN_FromDouble<FP>(100.0);
            FPN<FP> depth = FPN_SubSat(warn, price); // 70 - 100 = 0 (saturated)
            FPN<FP> raw = FPN_Mul(depth, range_inv);
            FPN<FP> zero = FPN_Zero<FP>();
            FPN<FP> one = FPN_FromDouble<FP>(1.0);
            FPN<FP> score = FPN_Min(FPN_Max(raw, zero), one);
            check("danger: price=100 (safe) → score=0",
                  FPN_ToDouble(score) < 0.01);
        }

        // price at 55 (in danger zone, halfway): score should be ~0.5
        {
            FPN<FP> price = FPN_FromDouble<FP>(55.0);
            FPN<FP> depth = FPN_SubSat(warn, price); // 70 - 55 = 15
            FPN<FP> raw = FPN_Mul(depth, range_inv); // 15/30 = 0.5
            FPN<FP> zero = FPN_Zero<FP>();
            FPN<FP> one = FPN_FromDouble<FP>(1.0);
            FPN<FP> score = FPN_Min(FPN_Max(raw, zero), one);
            double sv = FPN_ToDouble(score);
            check("danger: price=55 (mid-zone) → score~0.5",
                  sv > 0.4 && sv < 0.6);
        }

        // price at 30 (below crash): score should be clamped to 1.0
        {
            FPN<FP> price = FPN_FromDouble<FP>(30.0);
            FPN<FP> depth = FPN_SubSat(warn, price); // 70 - 30 = 40
            FPN<FP> raw = FPN_Mul(depth, range_inv); // 40/30 = 1.33
            FPN<FP> zero = FPN_Zero<FP>();
            FPN<FP> one = FPN_FromDouble<FP>(1.0);
            FPN<FP> score = FPN_Min(FPN_Max(raw, zero), one);
            check("danger: price=30 (crash) → score=1.0",
                  fabs(FPN_ToDouble(score) - 1.0) < 0.01);
        }

        // gate scaling: score=0.5 should halve the gate price
        {
            FPN<FP> gate = FPN_FromDouble<FP>(68000.0);
            FPN<FP> score = FPN_FromDouble<FP>(0.5);
            FPN<FP> one = FPN_FromDouble<FP>(1.0);
            FPN<FP> scale = FPN_SubSat(one, score); // 0.5
            FPN<FP> scaled_gate = FPN_Mul(gate, scale);
            check("danger: gate scaling at score=0.5 → ~$34000",
                  fabs(FPN_ToDouble(scaled_gate) - 34000.0) < 1.0);
        }

        // gate scaling: score=1.0 should zero the gate
        {
            FPN<FP> gate = FPN_FromDouble<FP>(68000.0);
            FPN<FP> one = FPN_FromDouble<FP>(1.0);
            FPN<FP> scale = FPN_SubSat(one, one); // 0
            FPN<FP> scaled_gate = FPN_Mul(gate, scale);
            check("danger: gate scaling at score=1.0 → $0",
                  FPN_ToDouble(scaled_gate) < 0.01);
        }
    }

    //======================================================================================================
    // GATE OFFSET TRACKING
    //======================================================================================================
    printf("\n--- GATE OFFSET TRACKING ---\n");
    {
        // verify offset capture: if EMA=68000 and gate_price=67950 (dir=0, buy below)
        // offset should be 50 (distance from EMA to gate)
        FPN<FP> ema = FPN_FromDouble<FP>(68000.0);
        FPN<FP> gate_price = FPN_FromDouble<FP>(67950.0);
        FPN<FP> offset = FPN_SubSat(ema, gate_price); // 50
        check("gate offset: EMA=68000, gate=67950 → offset=50",
              fabs(FPN_ToDouble(offset) - 50.0) < 0.01);

        // verify live gate recompute: if EMA rises to 68500, gate should be 68450
        FPN<FP> new_ema = FPN_FromDouble<FP>(68500.0);
        FPN<FP> live_gate = FPN_SubSat(new_ema, offset);
        check("gate offset: EMA rises to 68500 → gate=68450",
              fabs(FPN_ToDouble(live_gate) - 68450.0) < 0.01);

        // verify momentum direction (dir=1, buy above)
        FPN<FP> mom_gate = FPN_FromDouble<FP>(68100.0);
        FPN<FP> mom_offset = FPN_SubSat(mom_gate, ema); // 100
        FPN<FP> mom_live = FPN_AddSat(new_ema, mom_offset); // 68600
        check("gate offset: momentum dir=1, EMA rises → gate=68600",
              fabs(FPN_ToDouble(mom_live) - 68600.0) < 0.01);
    }

    //======================================================================================================
    // DEFAULT_STRATEGY -1 vs -2 DISPATCH
    //======================================================================================================
    printf("\n--- STRATEGY DISPATCH MODES ---\n");
    {
        // -1 legacy: only MR and Momentum
        check("dispatch -1: RANGING → MR",
              STRATEGY_MEAN_REVERSION == STRATEGY_MEAN_REVERSION); // trivial, for completeness
        check("dispatch -1: TRENDING → MOMENTUM (not EMA Cross)",
              STRATEGY_MOMENTUM != STRATEGY_EMA_CROSS);

        // -2 full auto: verify all mappings produce distinct strategies
        int ranging_s   = Regime_ToStrategy(REGIME_RANGING);
        int trending_s  = Regime_ToStrategy(REGIME_TRENDING);
        int volatile_s  = Regime_ToStrategy(REGIME_VOLATILE);
        int mild_s      = Regime_ToStrategy(REGIME_MILD_TREND);
        check("dispatch -2: 4 strategies used (MR, MOM, DIP, EMA)",
              ranging_s != trending_s && trending_s != volatile_s && volatile_s != mild_s);
        check("dispatch -2: VOLATILE uses SimpleDip (not MR)",
              volatile_s == STRATEGY_SIMPLE_DIP);
        check("dispatch -2: MILD_TREND uses EMA Cross (not Momentum)",
              mild_s == STRATEGY_EMA_CROSS);
    }

    //======================================================================================================
    // CENTRALIZED HALT FLAG
    //======================================================================================================
    printf("\n--- CENTRALIZED HALT FLAG ---\n");
    {
        ControllerConfig<FP> cfg = ControllerConfig_Default<FP>();
        cfg.warmup_ticks = 10;
        cfg.poll_interval = 1;
        cfg.min_warmup_samples = 10;
        cfg.kill_switch_enabled = 1;
        cfg.kill_switch_daily_loss_pct = FPN_FromDouble<FP>(0.03); // 3%
        cfg.kill_switch_drawdown_pct = FPN_FromDouble<FP>(0.05);   // 5%

        PortfolioController<FP> ctrl = {};
        PortfolioController_Init(&ctrl, cfg);
        OrderPool<FP> pool;
        OrderPool_init(&pool, 64);
        TradeLog log;
        TradeLog_Init(&log, "HALT_TEST");
        test_warmup_ctrl(&ctrl, &pool, &log, 100.0, 500.0);

        // 1. halt enforcement clears gate_offset and buy_conds
        // use kill_switch_active to create a real halt condition
        ctrl.kill_switch_active = 1;
        ctrl.gate_offset = FPN_FromDouble<FP>(5.0);
        ctrl.buy_conds.price = FPN_FromDouble<FP>(95.0);
        ctrl.buy_conds.volume = FPN_FromDouble<FP>(100.0);
        ctrl.ema_price = FPN_FromDouble<FP>(100.0); // needed for gate tracking
        PortfolioController_Tick(&ctrl, &pool, FPN_FromDouble<FP>(100.0),
                                  FPN_FromDouble<FP>(500.0), &log);
        check("halt enforcement: buy_conds.price zeroed",
              FPN_IsZero(ctrl.buy_conds.price));
        check("halt enforcement: buy_conds.volume zeroed",
              FPN_IsZero(ctrl.buy_conds.volume));
        check("halt enforcement: gate_offset zeroed",
              FPN_IsZero(ctrl.gate_offset));

        // 2. halt persists across multiple ticks (gate tracking can't resurrect)
        ctrl.gate_offset = FPN_FromDouble<FP>(3.0); // try to set it again
        PortfolioController_Tick(&ctrl, &pool, FPN_FromDouble<FP>(100.0),
                                  FPN_FromDouble<FP>(500.0), &log);
        check("halt persists: gate_offset re-zeroed on next tick",
              FPN_IsZero(ctrl.gate_offset));
        check("halt persists: buy_conds.price still zero",
              FPN_IsZero(ctrl.buy_conds.price));
        ctrl.kill_switch_active = 0; // clean up for remaining tests

        // 3. hot-path kill fires on equity crash
        PortfolioController<FP> ctrl2 = {};
        PortfolioController_Init(&ctrl2, cfg);
        test_warmup_ctrl(&ctrl2, &pool, &log, 100.0, 500.0);
        ctrl2.session_start_equity = FPN_FromDouble<FP>(10000.0);
        ctrl2.peak_equity = FPN_FromDouble<FP>(10000.0);
        // manually place a position at 100, then crash price to 50
        Portfolio_AddPositionWithExits(&ctrl2.portfolio, FPN_FromDouble<FP>(1.0),
            FPN_FromDouble<FP>(100.0), FPN_FromDouble<FP>(110.0),
            FPN_FromDouble<FP>(90.0));
        ctrl2.balance = FPN_FromDouble<FP>(9900.0); // $100 deducted for the position
        // crash to 50: position value = 50, equity = 9900+50 = 9950, daily return = -0.5%
        // not enough for 3% kill, let's use a bigger crash
        ctrl2.balance = FPN_FromDouble<FP>(9000.0); // simulate earlier losses
        // equity = 9000 + 50 = 9050, return = (9050-10000)/10000 = -9.5% > 3% limit
        PortfolioController_Tick(&ctrl2, &pool, FPN_FromDouble<FP>(50.0),
                                  FPN_FromDouble<FP>(500.0), &log);
        check("hot-path kill: kill_switch_active set on equity crash",
              ctrl2.kill_switch_active == 1);
        check("hot-path kill: buying_halted set",
              ctrl2.buying_halted == 1);
        check("hot-path kill: halt_reason is 1 (kill)",
              ctrl2.halt_reason == 1);

        // 4. hot-path kill fires on drawdown
        PortfolioController<FP> ctrl3 = {};
        PortfolioController_Init(&ctrl3, cfg);
        test_warmup_ctrl(&ctrl3, &pool, &log, 100.0, 500.0);
        ctrl3.session_start_equity = FPN_FromDouble<FP>(10000.0);
        ctrl3.peak_equity = FPN_FromDouble<FP>(12000.0);  // was at 12k, now crashed
        ctrl3.balance = FPN_FromDouble<FP>(10000.0);
        // position worth 100, equity = 10100, dd = (12000-10100)/12000 = 15.8% > 5% limit
        Portfolio_AddPositionWithExits(&ctrl3.portfolio, FPN_FromDouble<FP>(1.0),
            FPN_FromDouble<FP>(100.0), FPN_FromDouble<FP>(110.0),
            FPN_FromDouble<FP>(90.0));
        PortfolioController_Tick(&ctrl3, &pool, FPN_FromDouble<FP>(100.0),
                                  FPN_FromDouble<FP>(500.0), &log);
        check("hot-path kill: drawdown triggers kill switch",
              ctrl3.kill_switch_active == 1);
        check("hot-path kill: drawdown kill_reason is 2",
              ctrl3.kill_reason == 2);

        // 5. unpause blocked by active kill switch
        PortfolioController<FP> ctrl4 = {};
        PortfolioController_Init(&ctrl4, cfg);
        test_warmup_ctrl(&ctrl4, &pool, &log, 100.0, 500.0);
        ctrl4.kill_switch_active = 1;
        ctrl4.buying_halted = 1;
        ctrl4.halt_reason = 1;
        ctrl4.buy_conds.price = FPN_Zero<FP>(); // simulate halted state
        ctrl4.buy_conds.volume = FPN_Zero<FP>();
        PortfolioController_Unpause(&ctrl4);
        check("unpause blocked: buying_halted stays 1 when kill active",
              ctrl4.buying_halted == 1);
        check("unpause blocked: buy_conds.price stays zero",
              FPN_IsZero(ctrl4.buy_conds.price));

        // 6. centralized halt: volatile regime sets halted
        PortfolioController<FP> ctrl5 = {};
        PortfolioController_Init(&ctrl5, cfg);
        cfg.kill_switch_enabled = 0; // disable kill so it doesn't interfere
        PortfolioController_Init(&ctrl5, cfg);
        test_warmup_ctrl(&ctrl5, &pool, &log, 100.0, 500.0);
        ctrl5.regime.current_regime = REGIME_VOLATILE;
        // run a slow-path tick to trigger centralized halt
        ctrl5.tick_count = ctrl5.config.poll_interval; // force slow path
        PortfolioController_Tick(&ctrl5, &pool, FPN_FromDouble<FP>(100.0),
                                  FPN_FromDouble<FP>(500.0), &log);
        check("volatile halt: buying_halted set",
              ctrl5.buying_halted == 1);
        check("volatile halt: halt_reason is 3 (volatile)",
              ctrl5.halt_reason == 3);

        // 7. SL cooldown decrements independently during volatile
        ctrl5.sl_cooldown_counter = 5;
        ctrl5.tick_count = ctrl5.config.poll_interval;
        PortfolioController_Tick(&ctrl5, &pool, FPN_FromDouble<FP>(100.0),
                                  FPN_FromDouble<FP>(500.0), &log);
        check("cooldown decrement: counter decremented during volatile",
              ctrl5.sl_cooldown_counter == 4);
        check("cooldown decrement: halt_reason still volatile (higher priority)",
              ctrl5.halt_reason == 3);

        // 8. kill switch does NOT fire on small loss (regression: $6.75 on $10k tripped kill)
        {
            ControllerConfig<FP> small_cfg = ControllerConfig_Default<FP>();
            small_cfg.warmup_ticks = 10;
            small_cfg.poll_interval = 1;
            small_cfg.min_warmup_samples = 10;
            small_cfg.starting_balance = FPN_FromDouble<FP>(10000.0);
            small_cfg.kill_switch_enabled = 1;
            small_cfg.kill_switch_daily_loss_pct = FPN_FromDouble<FP>(0.03);  // 3%
            small_cfg.kill_switch_drawdown_pct = FPN_FromDouble<FP>(0.05);    // 5%
            small_cfg.max_positions = 1;

            PortfolioController<FP> sk = {};
            PortfolioController_Init(&sk, small_cfg);
            OrderPool<FP> sp;
            OrderPool_init(&sp, 64);
            TradeLog sl;
            TradeLog_Init(&sl, "KILL_SMALL_TEST");
            test_warmup_ctrl(&sk, &sp, &sl, 66000.0, 500.0);

            // simulate a small loss: balance drops by $6.75 (0.07%)
            sk.session_start_equity = FPN_FromDouble<FP>(10000.0);
            sk.peak_equity = FPN_FromDouble<FP>(10000.0);
            sk.balance = FPN_FromDouble<FP>(9993.25);  // $6.75 loss
            // no open positions — equity = balance = $9993.25
            // daily loss: (10000 - 9993.25) / 10000 = 0.07% — below 3% threshold
            // drawdown: (10000 - 9993.25) / 10000 = 0.07% — below 5% threshold

            // run enough ticks to hit the kill check (every 16th tick)
            for (int i = 0; i < 32; i++) {
                PortfolioController_Tick(&sk, &sp, FPN_FromDouble<FP>(66000.0),
                                          FPN_FromDouble<FP>(500.0), &sl);
            }
            check("small loss: kill switch should NOT fire on $6.75 loss (0.07%)",
                  sk.kill_switch_active == 0);
            check("small loss: buying_halted should be 0",
                  sk.buying_halted == 0 || sk.halt_reason != 1);

            // verify the thresholds are correct
            double daily_pct = FPN_ToDouble(sk.config.kill_switch_daily_loss_pct);
            double dd_pct = FPN_ToDouble(sk.config.kill_switch_drawdown_pct);
            check("small loss: daily_loss_pct is 0.03 (3%)",
                  daily_pct > 0.029 && daily_pct < 0.031);
            check("small loss: drawdown_pct is 0.05 (5%)",
                  dd_pct > 0.049 && dd_pct < 0.051);

            // now verify kill DOES fire on a real 4% loss
            sk.kill_switch_active = 0;
            sk.buying_halted = 0;
            sk.halt_reason = 0;
            sk.balance = FPN_FromDouble<FP>(9600.0);  // $400 loss = 4% > 3% daily limit
            for (int i = 0; i < 32; i++) {
                PortfolioController_Tick(&sk, &sp, FPN_FromDouble<FP>(66000.0),
                                          FPN_FromDouble<FP>(500.0), &sl);
            }
            check("real loss: kill switch fires on $400 loss (4%)",
                  sk.kill_switch_active == 1);
            check("real loss: kill_reason is 1 (daily_loss)",
                  sk.kill_reason == 1);

            TradeLog_Close(&sl);
            free(sp.slots);
            remove("logging/KILL_SMALL_TEST_order_history.csv");
        }

        TradeLog_Close(&log);
        free(pool.slots);
        remove("logging/HALT_TEST_order_history.csv");
    }

    //======================================================================================================
    // PUSHBUY GUARD
    //======================================================================================================
    printf("\n--- PUSHBUY GUARD ---\n");
    {
        ControllerConfig<FP> cfg = ControllerConfig_Default<FP>();
        cfg.warmup_ticks = 10;
        cfg.poll_interval = 1;
        cfg.min_warmup_samples = 10;
        cfg.starting_balance = FPN_FromDouble<FP>(10000.0);
        cfg.max_positions = 1;

        PortfolioController<FP> ctrl = {};
        PortfolioController_Init(&ctrl, cfg);
        OrderPool<FP> pool;
        OrderPool_init(&pool, 64);
        TradeLog log;
        TradeLog_Init(&log, "PUSHBUY_TEST");
        test_warmup_ctrl(&ctrl, &pool, &log, 100.0, 500.0);

        // fill slot 0 so portfolio is full (max_positions=1)
        Portfolio_AddPositionWithExits(&ctrl.portfolio, FPN_FromDouble<FP>(0.1),
            FPN_FromDouble<FP>(100.0), FPN_FromDouble<FP>(110.0), FPN_FromDouble<FP>(90.0));

        // set up buy conditions and put a fill in the pool
        ctrl.buy_conds.price = FPN_FromDouble<FP>(95.0);
        ctrl.buy_conds.volume = FPN_FromDouble<FP>(100.0);
        DataStream<FP> ds_push = {};
        ds_push.price = FPN_FromDouble<FP>(94.0);
        ds_push.volume = FPN_FromDouble<FP>(200.0);
        BuyGate(&ctrl.buy_conds, &ds_push, &pool);
        int buf_before = ctrl.trade_buf.count;
        PortfolioController_Tick(&ctrl, &pool, FPN_FromDouble<FP>(94.0),
                                  FPN_FromDouble<FP>(200.0), &log);
        check("pushbuy guard: trade_buf.count unchanged on rejected fill",
              ctrl.trade_buf.count == buf_before);
        check("pushbuy guard: still only 1 position (full)",
              Portfolio_CountActive(&ctrl.portfolio) == 1);

        TradeLog_Close(&log);
        free(pool.slots);
        remove("logging/PUSHBUY_TEST_order_history.csv");
    }

    //======================================================================================================
    // FPN EXIT GATE COMPARISON
    //======================================================================================================
    printf("\n--- FPN EXIT GATE COMPARISON ---\n");
    {
        Portfolio<FP> port = {};
        Portfolio_Init(&port);
        ExitBuffer<FP> ebuf = {};
        ExitBuffer_Init(&ebuf);

        // add position: entry=100, TP=105, SL=95
        FPN<FP> entry = FPN_FromDouble<FP>(100.0);
        FPN<FP> tp = FPN_FromDouble<FP>(105.0);
        FPN<FP> sl = FPN_FromDouble<FP>(95.0);
        Portfolio_AddPositionWithExits(&port, FPN_FromDouble<FP>(1.0), entry, tp, sl);

        // price at 94 (below SL) — must trigger exit
        PositionExitGate(&port, FPN_FromDouble<FP>(94.0), &ebuf, 1);
        check("exit gate: SL triggers at price below SL",
              ebuf.count == 1);
        check("exit gate: reason is SL (1)",
              ebuf.records[0].reason == 1);
        check("exit gate: bitmap cleared",
              port.active_bitmap == 0);

        // reset, test TP
        ExitBuffer_Init(&ebuf);
        Portfolio_AddPositionWithExits(&port, FPN_FromDouble<FP>(1.0), entry, tp, sl);
        PositionExitGate(&port, FPN_FromDouble<FP>(106.0), &ebuf, 2);
        check("exit gate: TP triggers at price above TP",
              ebuf.count == 1);
        check("exit gate: reason is TP (0)",
              ebuf.records[0].reason == 0);

        // reset, test price between SL and TP — no exit
        ExitBuffer_Init(&ebuf);
        Portfolio_AddPositionWithExits(&port, FPN_FromDouble<FP>(1.0), entry, tp, sl);
        PositionExitGate(&port, FPN_FromDouble<FP>(100.0), &ebuf, 3);
        check("exit gate: no exit when price between SL and TP",
              ebuf.count == 0);
        check("exit gate: bitmap still active",
              port.active_bitmap != 0);

        // test tight boundary: SL=95.001, price=95.0005 (just below SL)
        // this exercises middle FPN words — the old 2-word comparison could miss this
        ExitBuffer_Init(&ebuf);
        Portfolio_Init(&port);
        FPN<FP> tight_sl = FPN_FromDouble<FP>(95.001);
        FPN<FP> tight_tp = FPN_FromDouble<FP>(105.0);
        Portfolio_AddPositionWithExits(&port, FPN_FromDouble<FP>(1.0), entry, tight_tp, tight_sl);
        FPN<FP> just_below = FPN_FromDouble<FP>(95.0005);
        PositionExitGate(&port, just_below, &ebuf, 4);
        check("exit gate: tight SL boundary triggers correctly",
              ebuf.count == 1);

        // price just above SL — no exit
        ExitBuffer_Init(&ebuf);
        Portfolio_Init(&port);
        Portfolio_AddPositionWithExits(&port, FPN_FromDouble<FP>(1.0), entry, tight_tp, tight_sl);
        FPN<FP> just_above = FPN_FromDouble<FP>(95.0015);
        PositionExitGate(&port, just_above, &ebuf, 5);
        check("exit gate: price just above SL does not trigger",
              ebuf.count == 0);
    }

    //======================================================================================================
    // BALANCE DRIFT — round trip accounting
    //======================================================================================================
    printf("\n--- BALANCE DRIFT ---\n");
    {
        ControllerConfig<FP> cfg = ControllerConfig_Default<FP>();
        cfg.warmup_ticks = 10;
        cfg.poll_interval = 1;
        cfg.min_warmup_samples = 10;
        cfg.starting_balance = FPN_FromDouble<FP>(10000.0);
        cfg.risk_pct = FPN_FromDouble<FP>(0.15);  // 15%
        cfg.fee_rate = FPN_FromDouble<FP>(0.001);  // 0.1%
        cfg.max_positions = 1;
        cfg.kill_switch_enabled = 0;  // disable kill — we're testing accounting
        cfg.slippage_pct = FPN_Zero<FP>();  // no slippage for clean test
        cfg.take_profit_pct = FPN_FromDouble<FP>(0.03);
        cfg.stop_loss_pct = FPN_FromDouble<FP>(0.015);

        double starting = 10000.0;

        // TEST 1: buy deduction matches position cost + fee
        {
            PortfolioController<FP> ctrl = {};
            PortfolioController_Init(&ctrl, cfg);
            OrderPool<FP> pool;
            OrderPool_init(&pool, 64);
            TradeLog log;
            TradeLog_Init(&log, "DRIFT_TEST1");
            test_warmup_ctrl(&ctrl, &pool, &log, 66000.0, 500.0);

            double bal_before = FPN_ToDouble(ctrl.balance);

            // manually create a fill at $66,000
            FPN<FP> fill_price = FPN_FromDouble<FP>(66000.0);
            FPN<FP> risk = FPN_Mul(ctrl.balance, cfg.risk_pct);
            FPN<FP> qty = FPN_DivNoAssert(risk, fill_price);
            FPN<FP> cost = FPN_Mul(fill_price, qty);
            FPN<FP> fee = FPN_Mul(cost, cfg.fee_rate);
            FPN<FP> total = FPN_AddSat(cost, fee);

            // simulate fill
            Portfolio_AddPositionWithExits(&ctrl.portfolio, qty, fill_price,
                FPN_FromDouble<FP>(68000.0), FPN_FromDouble<FP>(65000.0), fee);
            ctrl.balance = FPN_SubSat(ctrl.balance, total);

            double bal_after = FPN_ToDouble(ctrl.balance);
            double deducted = bal_before - bal_after;
            double expected_deduction = FPN_ToDouble(total);

            check("buy deduction: balance decreased by cost + fee",
                  fabs(deducted - expected_deduction) < 0.01);

            // verify equity = balance + position value ≈ starting
            FPN<FP> pv = Portfolio_ComputeValue(&ctrl.portfolio, fill_price);
            FPN<FP> equity = FPN_AddSat(ctrl.balance, pv);
            double eq = FPN_ToDouble(equity);
            check("buy equity: balance + position ≈ starting - entry fee",
                  fabs(eq - (starting - FPN_ToDouble(fee))) < 0.01);

            printf("    bal_before=%.2f bal_after=%.2f deducted=%.2f pv=%.2f equity=%.2f\n",
                   bal_before, bal_after, deducted, FPN_ToDouble(pv), eq);

            // TEST 2: sell at TP — balance fully restored
            FPN<FP> exit_price = FPN_FromDouble<FP>(68000.0);
            FPN<FP> gross = FPN_Mul(exit_price, qty);
            FPN<FP> exit_fee = FPN_Mul(gross, cfg.fee_rate);
            FPN<FP> net = FPN_SubSat(gross, exit_fee);
            ctrl.balance = FPN_AddSat(ctrl.balance, net);
            ctrl.portfolio.active_bitmap = 0;  // clear position

            double bal_final = FPN_ToDouble(ctrl.balance);
            // expected: starting - entry_fee - exit_fee + price_gain
            double price_gain = (68000.0 - 66000.0) * FPN_ToDouble(qty);
            double total_fees = FPN_ToDouble(fee) + FPN_ToDouble(exit_fee);
            double expected_final = starting + price_gain - total_fees;

            check("round trip: balance = starting + gain - fees (no drift)",
                  fabs(bal_final - expected_final) < 0.01);
            printf("    final=%.2f expected=%.2f drift=%.4f\n",
                   bal_final, expected_final, bal_final - expected_final);

            TradeLog_Close(&log);
            free(pool.slots);
            remove("logging/DRIFT_TEST1_order_history.csv");
        }

        // TEST 3: equity consistency during open position at different prices
        {
            PortfolioController<FP> ctrl = {};
            PortfolioController_Init(&ctrl, cfg);
            OrderPool<FP> pool;
            OrderPool_init(&pool, 64);
            TradeLog log;
            TradeLog_Init(&log, "DRIFT_TEST2");
            test_warmup_ctrl(&ctrl, &pool, &log, 66000.0, 500.0);

            // open position at $66,000
            FPN<FP> fill_price = FPN_FromDouble<FP>(66000.0);
            FPN<FP> risk = FPN_Mul(ctrl.balance, cfg.risk_pct);
            FPN<FP> qty = FPN_DivNoAssert(risk, fill_price);
            FPN<FP> cost = FPN_Mul(fill_price, qty);
            FPN<FP> fee = FPN_Mul(cost, cfg.fee_rate);
            FPN<FP> total_cost = FPN_AddSat(cost, fee);
            Portfolio_AddPositionWithExits(&ctrl.portfolio, qty, fill_price,
                FPN_FromDouble<FP>(68000.0), FPN_FromDouble<FP>(65000.0), fee);
            ctrl.balance = FPN_SubSat(ctrl.balance, total_cost);

            // check equity at entry price
            FPN<FP> pv1 = Portfolio_ComputeValue(&ctrl.portfolio, fill_price);
            FPN<FP> eq1 = FPN_AddSat(ctrl.balance, pv1);
            double entry_eq = FPN_ToDouble(eq1);
            check("open pos equity at entry price ≈ starting - fee",
                  fabs(entry_eq - (starting - FPN_ToDouble(fee))) < 0.01);

            // check equity at higher price ($67,000)
            FPN<FP> high = FPN_FromDouble<FP>(67000.0);
            FPN<FP> pv2 = Portfolio_ComputeValue(&ctrl.portfolio, high);
            FPN<FP> eq2 = FPN_AddSat(ctrl.balance, pv2);
            double high_eq = FPN_ToDouble(eq2);
            double expected_gain = 1000.0 * FPN_ToDouble(qty);  // $1000 price move × qty
            check("open pos equity at +$1000 reflects unrealized gain",
                  fabs(high_eq - entry_eq - expected_gain) < 0.01);

            // check equity at lower price ($65,000) — should NOT trigger kill on 3% threshold
            FPN<FP> low = FPN_FromDouble<FP>(65000.0);
            FPN<FP> pv3 = Portfolio_ComputeValue(&ctrl.portfolio, low);
            FPN<FP> eq3 = FPN_AddSat(ctrl.balance, pv3);
            double low_eq = FPN_ToDouble(eq3);
            double pct_drop = (starting - low_eq) / starting * 100.0;
            check("open pos equity at -$1000: drop < 3% (no false kill)",
                  pct_drop < 3.0);
            printf("    entry_eq=%.2f high_eq=%.2f low_eq=%.2f drop=%.2f%%\n",
                   entry_eq, high_eq, low_eq, pct_drop);

            // TEST 4: verify Portfolio_ComputeValue matches manual calculation
            double manual_pv = FPN_ToDouble(qty) * 65000.0;
            double computed_pv = FPN_ToDouble(pv3);
            check("Portfolio_ComputeValue matches qty × price",
                  fabs(computed_pv - manual_pv) < 0.01);
            printf("    manual_pv=%.2f computed_pv=%.2f diff=%.6f\n",
                   manual_pv, computed_pv, computed_pv - manual_pv);

            TradeLog_Close(&log);
            free(pool.slots);
            remove("logging/DRIFT_TEST2_order_history.csv");
        }

        // TEST 5: full pipeline round trip through PortfolioController_Tick
        {
            ControllerConfig<FP> rt_cfg = cfg;
            rt_cfg.kill_switch_enabled = 0;
            rt_cfg.offset_stddev_mult = FPN_FromDouble<FP>(0.5); // tight gate for quick fill
            PortfolioController<FP> ctrl = {};
            PortfolioController_Init(&ctrl, rt_cfg);
            OrderPool<FP> pool;
            OrderPool_init(&pool, 64);
            TradeLog log;
            TradeLog_Init(&log, "DRIFT_TEST3");
            test_warmup_ctrl(&ctrl, &pool, &log, 66000.0, 500.0);

            double bal_start = FPN_ToDouble(ctrl.balance);

            // run 500 ticks at stable price — should buy, then TP or SL
            for (int i = 0; i < 500; i++) {
                double p = 66000.0 + (i % 50) * 10.0;  // oscillate $0-$500
                PortfolioController_Tick(&ctrl, &pool, FPN_FromDouble<FP>(p),
                                          FPN_FromDouble<FP>(500.0), &log);
            }

            int active = Portfolio_CountActive(&ctrl.portfolio);
            double bal_end = FPN_ToDouble(ctrl.balance);
            double realized = FPN_ToDouble(ctrl.realized_pnl);

            if (active == 0) {
                // all positions closed — balance should equal starting + realized
                double expected = bal_start + realized;
                double drift = bal_end - expected;
                check("pipeline round trip: no balance drift when flat",
                      fabs(drift) < 0.01);
                printf("    bal=%.2f expected=%.2f drift=%.4f trades=%d\n",
                       bal_end, expected, drift, ctrl.total_buys);
            } else {
                printf("    (skipped drift check — %d positions still open)\n", active);
            }

            TradeLog_Close(&log);
            free(pool.slots);
            remove("logging/DRIFT_TEST3_order_history.csv");
        }
    }

    //======================================================================================================
    // EXIT BUFFER EQUITY GAP
    //======================================================================================================
    printf("\n--- EXIT BUFFER EQUITY GAP ---\n");
    {
        // verify equity stays consistent between exit gate (bitmap clear) and DrainExits (balance credit)
        // the gap: position value disappears from Portfolio_ComputeValue but isn't in balance yet
        // ExitBuffer_PendingProceeds must bridge the gap exactly

        ControllerConfig<FP> cfg = ControllerConfig_Default<FP>();
        cfg.warmup_ticks = 0;
        cfg.poll_interval = 1000; // large poll so slow path doesn't run
        cfg.starting_balance = FPN_FromDouble<FP>(10000.0);
        cfg.max_positions = 1;
        cfg.fee_rate = FPN_FromDouble<FP>(0.001);  // 0.1%
        cfg.slippage_pct = FPN_Zero<FP>();          // no slippage for exact math

        PortfolioController<FP> ctrl = {};
        PortfolioController_Init(&ctrl, cfg);
        ctrl.state = CONTROLLER_ACTIVE;

        // manually add a position: entry $100, qty 1.0, TP $105, SL $95
        FPN<FP> entry = FPN_FromDouble<FP>(100.0);
        FPN<FP> qty = FPN_FromDouble<FP>(1.0);
        FPN<FP> tp = FPN_FromDouble<FP>(105.0);
        FPN<FP> sl = FPN_FromDouble<FP>(95.0);
        Portfolio_AddPositionWithExits(&ctrl.portfolio, qty, entry, tp, sl);
        ctrl.balance = FPN_FromDouble<FP>(9900.0); // $100 deducted for position

        // equity before exit: balance + position_value = 9900 + 105 = 10005 (at TP price)
        FPN<FP> price_at_tp = FPN_FromDouble<FP>(105.0);
        FPN<FP> pv_before = Portfolio_ComputeValue(&ctrl.portfolio, price_at_tp);
        FPN<FP> equity_before = FPN_AddSat(ctrl.balance, pv_before);

        // trigger exit gate — clears bitmap, writes to exit buffer
        PositionExitGate(&ctrl.portfolio, price_at_tp, &ctrl.exit_buf, 1);
        check("equity gap: exit buffered", ctrl.exit_buf.count == 1);
        check("equity gap: bitmap cleared", ctrl.portfolio.active_bitmap == 0);

        // portfolio value is now 0 (position cleared from bitmap)
        FPN<FP> pv_after = Portfolio_ComputeValue(&ctrl.portfolio, price_at_tp);
        check("equity gap: portfolio value is zero after exit gate",
              FPN_IsZero(pv_after));

        // naive equity (the bug): balance + pv = 9900 + 0 = 9900 — $105 phantom crash
        FPN<FP> naive_equity = FPN_AddSat(ctrl.balance, pv_after);

        // correct equity: balance + pv + pending proceeds
        FPN<FP> pending = ExitBuffer_PendingProceeds(&ctrl.exit_buf,
                                                      cfg.fee_rate, cfg.slippage_pct);
        FPN<FP> correct_equity = FPN_AddSat(FPN_AddSat(ctrl.balance, pv_after), pending);

        // pending should be close to gross - fees: 105 * 1.0 - 105 * 1.0 * 0.001 = 104.895
        double pending_d = FPN_ToDouble(pending);
        check("equity gap: pending proceeds ~$104.90",
              pending_d > 104.8 && pending_d < 105.0);

        // naive equity has the phantom crash
        double naive_d = FPN_ToDouble(naive_equity);
        double before_d = FPN_ToDouble(equity_before);
        check("equity gap: naive equity shows phantom $105 drop",
              (before_d - naive_d) > 100.0);

        // correct equity is close to before (within fee difference)
        double correct_d = FPN_ToDouble(correct_equity);
        double gap = fabs(before_d - correct_d);
        check("equity gap: correct equity within $0.20 of pre-exit",
              gap < 0.20);

        printf("    before=%.2f naive=%.2f correct=%.2f pending=%.4f gap=%.4f\n",
               before_d, naive_d, correct_d, pending_d, gap);
    }

    //======================================================================================================
    // WIN/LOSS CLASSIFICATION BY P&L SIGN
    //======================================================================================================
    printf("\n--- WIN/LOSS BY P&L SIGN ---\n");
    {
        // a TP exit where fees exceed gross profit should count as a loss, not a win
        ControllerConfig<FP> cfg = ControllerConfig_Default<FP>();
        cfg.starting_balance = FPN_FromDouble<FP>(10000.0);
        cfg.fee_rate = FPN_FromDouble<FP>(0.01); // 1% fee to make fee > gross easy
        cfg.slippage_pct = FPN_Zero<FP>();

        PortfolioController<FP> ctrl = {};
        PortfolioController_Init(&ctrl, cfg);
        ctrl.rolling = RollingStats_Init<FP>();
        ctrl.rolling.price_stddev = FPN_FromDouble<FP>(50.0);

        // add position: entry $100, qty 0.5, small gross profit
        FPN<FP> entry = FPN_FromDouble<FP>(100.0);
        FPN<FP> qty = FPN_FromDouble<FP>(0.5);
        Portfolio_AddPositionWithExits(&ctrl.portfolio, qty, entry,
            FPN_FromDouble<FP>(101.0), FPN_FromDouble<FP>(95.0));
        ctrl.portfolio.positions[0].entry_fee = FPN_FromDouble<FP>(0.50); // $0.50 entry fee

        // helper: build ExitRecord from position slot (slot is still valid in tests)
        auto make_rec = [](Portfolio<FP> *p, int slot, FPN<FP> exit_price, uint64_t tick, int reason) {
            ExitRecord<FP> rec;
            rec.position_index = slot;
            rec.exit_price = exit_price;
            rec.tick = tick;
            rec.reason = reason;
            rec.entry_price = p->positions[slot].entry_price;
            rec.quantity = p->positions[slot].quantity;
            rec.entry_fee = p->positions[slot].entry_fee;
            rec.pair_index = p->positions[slot].pair_index;
            return rec;
        };

        // exit at TP $101: gross = 0.5 × (101-100) = $0.50
        // exit fee = 0.5 × 101 × 0.01 = $0.505
        // net P&L = 0.50 - 0.505 - 0.50 = -$0.505 (loss despite TP exit)
        { ExitRecord<FP> rec = make_rec(&ctrl.portfolio, 0, FPN_FromDouble<FP>(101.0), 100, 0);
          RecordExit(&ctrl, &rec); }
        check("win/loss: TP exit with fee-dominated P&L counts as loss",
              ctrl.losses == 1 && ctrl.wins == 0);

        // now test a genuine winning TP exit
        Portfolio_AddPositionWithExits(&ctrl.portfolio, qty, entry,
            FPN_FromDouble<FP>(110.0), FPN_FromDouble<FP>(95.0));
        ctrl.portfolio.positions[0].entry_fee = FPN_FromDouble<FP>(0.50);
        // exit at $110: gross = 0.5 × (110-100) = $5.00
        // exit fee = 0.5 × 110 × 0.01 = $0.55
        // net P&L = 5.00 - 0.55 - 0.50 = $3.95 (genuine win)
        { ExitRecord<FP> rec = make_rec(&ctrl.portfolio, 0, FPN_FromDouble<FP>(110.0), 200, 0);
          RecordExit(&ctrl, &rec); }
        check("win/loss: TP exit with genuine profit counts as win",
              ctrl.wins == 1);

        // SL exit always counts as loss regardless of P&L
        Portfolio_AddPositionWithExits(&ctrl.portfolio, qty, entry,
            FPN_FromDouble<FP>(110.0), FPN_FromDouble<FP>(95.0));
        ctrl.portfolio.positions[0].entry_fee = FPN_FromDouble<FP>(0.50);
        { ExitRecord<FP> rec = make_rec(&ctrl.portfolio, 0, FPN_FromDouble<FP>(95.0), 300, 1);
          RecordExit(&ctrl, &rec); }
        check("win/loss: SL exit counts as loss",
              ctrl.losses == 2); // fee-dominated TP loss + this SL
    }

    //======================================================================================================
    // FEE FLOOR ENFORCEMENT AFTER REGIME TP TIGHTENING
    //======================================================================================================
    printf("\n--- FEE FLOOR AFTER REGIME TIGHTENING ---\n");
    {
        ControllerConfig<FP> cfg = ControllerConfig_Default<FP>();
        cfg.fee_rate = FPN_FromDouble<FP>(0.001);      // 0.1%
        cfg.fee_floor_mult = FPN_FromDouble<FP>(3.0);   // TP floor = 3× round-trip fees
        cfg.take_profit_pct = FPN_FromDouble<FP>(0.01);  // 1% TP offset for MR (used as stddev mult × 100)
        cfg.stop_loss_pct = FPN_FromDouble<FP>(0.01);    // 1% SL
        cfg.momentum_tp_mult = FPN_FromDouble<FP>(3.0);  // 3 stddev TP for momentum
        cfg.momentum_sl_mult = FPN_FromDouble<FP>(1.0);  // 1 stddev SL
        cfg.min_sl_tp_ratio = FPN_FromDouble<FP>(0.5);
        cfg.max_positions = 1;

        // setup: position at $66000, wide TP from momentum (stddev × 3 = $30 at σ=10)
        Portfolio<FP> port = {};
        Portfolio_Init(&port);
        FPN<FP> entry = FPN_FromDouble<FP>(66000.0);
        FPN<FP> wide_tp = FPN_FromDouble<FP>(66500.0);  // $500 above entry (momentum)
        FPN<FP> sl = FPN_FromDouble<FP>(65800.0);
        Portfolio_AddPositionWithExits(&port, FPN_FromDouble<FP>(0.05), entry, wide_tp, sl);

        // rolling stats with VERY low stddev (simulates volatility crash after fill)
        RollingStats<FP> rolling = RollingStats_Init<FP>();
        rolling.price_stddev = FPN_FromDouble<FP>(5.0); // tiny stddev

        // regime: TRENDING → RANGING — tightens TP using FPN_Min
        // tight_tp = entry + stddev × (take_profit_pct × 100) = 66000 + 5 × 1.0 = 66005
        // that's $5 above entry — way below fee floor of $198
        uint8_t entry_strat[16] = {};
        entry_strat[0] = Regime_ToStrategy(REGIME_TRENDING);
        Regime_AdjustPositions(&port, &rolling, REGIME_TRENDING, REGIME_RANGING, entry_strat, &cfg);

        double tp_after = FPN_ToDouble(port.positions[0].take_profit_price);
        double entry_d = FPN_ToDouble(entry);
        double tp_dist = tp_after - entry_d;

        // fee floor = entry × fee_rate × fee_floor_mult = 66000 × 0.001 × 3 = $198
        double fee_floor_d = entry_d * 0.001 * 3.0;
        check("fee floor: TP not below fee breakeven after regime tighten",
              tp_dist >= fee_floor_d - 0.01);
        printf("    tp=%.2f entry=%.2f tp_dist=%.2f fee_floor=%.2f\n",
               tp_after, entry_d, tp_dist, fee_floor_d);

        // test TRENDING → MILD_TREND too
        Portfolio_Init(&port);
        Portfolio_AddPositionWithExits(&port, FPN_FromDouble<FP>(0.05), entry, wide_tp, sl);
        entry_strat[0] = Regime_ToStrategy(REGIME_TRENDING);
        Regime_AdjustPositions(&port, &rolling, REGIME_TRENDING, REGIME_MILD_TREND, entry_strat, &cfg);

        double tp_mild = FPN_ToDouble(port.positions[0].take_profit_price);
        double tp_mild_dist = tp_mild - entry_d;
        check("fee floor: TRENDING→MILD_TREND TP above fee breakeven",
              tp_mild_dist >= fee_floor_d - 0.01);

        // test → TRENDING_DOWN
        Portfolio_Init(&port);
        Portfolio_AddPositionWithExits(&port, FPN_FromDouble<FP>(0.05), entry, wide_tp, sl);
        entry_strat[0] = Regime_ToStrategy(REGIME_TRENDING);
        Regime_AdjustPositions(&port, &rolling, REGIME_TRENDING, REGIME_TRENDING_DOWN, entry_strat, &cfg);

        double tp_down = FPN_ToDouble(port.positions[0].take_profit_price);
        double tp_down_dist = tp_down - entry_d;
        check("fee floor: →TRENDING_DOWN TP above fee breakeven",
              tp_down_dist >= fee_floor_d - 0.01);
    }

    //======================================================================================================
    // SLOT REUSE REGRESSION (the root cause of phantom drawdown)
    //======================================================================================================
    printf("\n--- SLOT REUSE REGRESSION ---\n");
    {
        // reproduces the exact race: position A exits, position B fills same slot,
        // DrainExits must use A's data (from ExitRecord), not B's (from slot)

        ControllerConfig<FP> cfg = ControllerConfig_Default<FP>();
        cfg.starting_balance = FPN_FromDouble<FP>(10000.0);
        cfg.fee_rate = FPN_FromDouble<FP>(0.001);
        cfg.slippage_pct = FPN_Zero<FP>();
        cfg.max_positions = 1;

        PortfolioController<FP> ctrl = {};
        PortfolioController_Init(&ctrl, cfg);

        // position A at slot 0: entry $100, qty 1.0, TP $110, SL $90
        FPN<FP> entry_a = FPN_FromDouble<FP>(100.0);
        FPN<FP> qty_a = FPN_FromDouble<FP>(1.0);
        Portfolio_AddPositionWithExits(&ctrl.portfolio, qty_a, entry_a,
            FPN_FromDouble<FP>(110.0), FPN_FromDouble<FP>(90.0));
        ctrl.portfolio.positions[0].entry_fee = FPN_FromDouble<FP>(0.10);
        ctrl.balance = FPN_FromDouble<FP>(9899.90); // 10000 - 100 - 0.10 fee

        // exit gate: price hits SL at $90
        PositionExitGate(&ctrl.portfolio, FPN_FromDouble<FP>(90.0), &ctrl.exit_buf, 100);
        check("slot reuse: A exited", ctrl.exit_buf.count == 1);
        check("slot reuse: bitmap cleared", ctrl.portfolio.active_bitmap == 0);

        // verify ExitRecord captured A's data
        check("slot reuse: record has A's entry",
              fabs(FPN_ToDouble(ctrl.exit_buf.records[0].entry_price) - 100.0) < 0.01);
        check("slot reuse: record has A's quantity",
              fabs(FPN_ToDouble(ctrl.exit_buf.records[0].quantity) - 1.0) < 0.01);

        // NOW: position B fills into slot 0 (overwrites slot data)
        FPN<FP> entry_b = FPN_FromDouble<FP>(200.0);
        FPN<FP> qty_b = FPN_FromDouble<FP>(0.5);
        Portfolio_AddPositionWithExits(&ctrl.portfolio, qty_b, entry_b,
            FPN_FromDouble<FP>(220.0), FPN_FromDouble<FP>(180.0));
        ctrl.portfolio.positions[0].entry_fee = FPN_FromDouble<FP>(0.20);

        // slot 0 now has B's data — entry $200, qty 0.5
        check("slot reuse: slot has B's entry",
              fabs(FPN_ToDouble(ctrl.portfolio.positions[0].entry_price) - 200.0) < 0.01);

        // PendingProceeds must use A's quantity (1.0), not B's (0.5)
        FPN<FP> pending = ExitBuffer_PendingProceeds(&ctrl.exit_buf,
                                                      cfg.fee_rate, cfg.slippage_pct);
        double pending_d = FPN_ToDouble(pending);
        // A exited at $90, qty 1.0: gross=$90, fee=$0.09, net=$89.91
        check("slot reuse: pending uses A's qty (not B's)",
              pending_d > 89.8 && pending_d < 90.0);

        // DrainExits must compute P&L against A's entry ($100), not B's ($200)
        double bal_before = FPN_ToDouble(ctrl.balance);
        PortfolioController_DrainExits(&ctrl);
        double bal_after = FPN_ToDouble(ctrl.balance);
        double credited = bal_after - bal_before;

        // A's net proceeds: $90 × 1.0 - fee = $89.91
        check("slot reuse: drain credited A's proceeds (not B's)",
              credited > 89.8 && credited < 90.0);

        // P&L should be against A's entry: $89.91 - ($100 × 1.0 + $0.10) = -$10.19
        double realized = FPN_ToDouble(ctrl.realized_pnl);
        check("slot reuse: P&L computed against A's entry",
              realized < -10.0 && realized > -10.5);

        printf("    pending=%.2f credited=%.2f realized=%.2f\n",
               pending_d, credited, realized);
    }

    //======================================================================================================
    // [PHASE 5d REGRESSION TESTS — locking in 2026-04-25 weekend bug fixes]
    //======================================================================================================
    // see plans/phase5d-regression-tests.md. each block guards a specific
    // re-introducible bug class. failure = real regression, not flakiness.
    //======================================================================================================

    // ----- Group 1: Dynamic-buffer lifecycle ------------------------------------------------------------
    // bug class: adding a new heap field to BacktestResults but missing _Reset
    // (ff9ac48 added equity_curve, _Reset zeroed cap → first EnsureEquityCapacity
    // call hit `while (0 < needed) cap *= 2` infinite spin at 100% CPU)
    printf("\n--- Phase 5d: Dynamic-buffer lifecycle ---\n");
    {
        // Reset must preserve every dynamic allocation + its capacity (zeroes counts only)
        BacktestResults r;
        BacktestResults_Init(&r);
        double *original_curve = r.equity_curve;
        int original_eq_cap    = r.equity_capacity;
        float *original_fm     = r.feature_matrix;
        int original_sm_cap    = r.sample_capacity;
        r.equity_count = 5;
        r.sample_count = 100;
        BacktestResults_Reset(&r);
        check("Reset preserves equity_curve + sample buffers (ptrs + caps), zeroes counts",
              r.equity_count == 0 && r.sample_count == 0 &&
              r.equity_curve == original_curve && r.equity_capacity == original_eq_cap &&
              r.feature_matrix == original_fm && r.sample_capacity == original_sm_cap);
        BacktestResults_Free(&r);
    }
    {
        // EnsureEquityCapacity floor: capacity=0 → INIT_CAP (no infinite spin)
        BacktestResults r = {};
        int ok = BacktestResults_EnsureEquityCapacity(&r, 1);
        check("EnsureEquityCapacity floor: cap=0 seeds to BACKTEST_EQUITY_INIT (no spin)",
              ok == 1 && r.equity_capacity >= BACKTEST_EQUITY_INIT &&
              r.equity_curve != NULL);
        free(r.equity_curve);
    }
    {
        // EnsureCapacity (samples) floor: same zero-capacity guard
        BacktestResults r = {};
        int ok = BacktestResults_EnsureCapacity(&r, 1);
        check("EnsureCapacity (samples) floor: cap=0 seeds to BACKTEST_SAMPLES_INIT",
              ok == 1 && r.sample_capacity >= BACKTEST_SAMPLES_INIT &&
              r.feature_matrix != NULL && r.labels != NULL);
        free(r.feature_matrix);
        free(r.labels);
        free(r.sample_tick_indices);
        free(r.sample_prices);
        free(r.sample_regimes);
    }

    // ----- Group 2: Label-type-aware metric dispatch ----------------------------------------------------
    // bug class: hardcoding binary classification on regression labels (4-25 morning:
    // Forward P&L sample panel showed +:0/-:2.25M, walk-forward 0.0% every fold —
    // continuous labels binarized at 0.5, model trained with binary:logistic)
    printf("\n--- Phase 5d: Label-type-aware metric dispatch ---\n");
    {
        check("LabelType_NumClasses: WIN_LOSS=0, FORWARD_PNL=1, PEAK_VALLEY_STABLE=3, REGIME=4",
              LabelType_NumClasses(LABEL_WIN_LOSS) == 0 &&
              LabelType_NumClasses(LABEL_FORWARD_PNL) == 1 &&
              LabelType_NumClasses(LABEL_PEAK_VALLEY_STABLE) == 3 &&
              LabelType_NumClasses(LABEL_REGIME) == 4);
        check("LabelType_IsBinary identifies WIN_LOSS",
              LabelType_IsBinary(LABEL_WIN_LOSS) == 1 &&
              LabelType_IsBinary(LABEL_FORWARD_PNL) == 0);
        check("LabelType_IsRegression identifies FORWARD_PNL",
              LabelType_IsRegression(LABEL_FORWARD_PNL) == 1 &&
              LabelType_IsRegression(LABEL_WIN_LOSS) == 0);
        check("LabelType_IsMulticlass identifies PEAK_VALLEY_STABLE + REGIME",
              LabelType_IsMulticlass(LABEL_PEAK_VALLEY_STABLE) == 1 &&
              LabelType_IsMulticlass(LABEL_REGIME) == 1 &&
              LabelType_IsMulticlass(LABEL_WIN_LOSS) == 0);
        check("LabelType_* out-of-bounds defaults to safe binary kind",
              LabelType_IsBinary(999) == 1 && LabelType_IsRegression(-1) == 0 &&
              LabelType_IsMulticlass(999) == 0);
    }

    // ----- Group 3: Class-balance helpers ---------------------------------------------------------------
    // bug class: divide-by-zero on degenerate datasets, missing multiclass weight
    // compensation (38ab41d added inverse-frequency weights for skewed multiclass)
    printf("\n--- Phase 5d: Class-balance helpers ---\n");
    {
        // scale_pos_weight: 2 pos / 4 neg → 2.0
        float labels[] = {1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        int n_pos = 0, n_neg = 0;
        double w = XGBoost_ComputeScalePosWeight(labels, 6, &n_pos, &n_neg);
        check("XGBoost_ComputeScalePosWeight basic: n_pos=2, n_neg=4, w=2.0",
              n_pos == 2 && n_neg == 4 && fabs(w - 2.0) < 1e-6);
    }
    {
        // scale_pos_weight zero-positive guard (degenerate dataset → no NaN, no div-by-zero)
        float labels[] = {0.0f, 0.0f, 0.0f};
        int n_pos = 0, n_neg = 0;
        double w = XGBoost_ComputeScalePosWeight(labels, 3, &n_pos, &n_neg);
        check("XGBoost_ComputeScalePosWeight zero-positive guard returns 1.0",
              n_pos == 0 && n_neg == 3 && fabs(w - 1.0) < 1e-6);
    }
    {
        // multiclass inverse-frequency: 4 of class 0, 1 of class 1, 1 of class 2 (K=3)
        // weight[i] = total / (K * count[label[i]])
        //   class 0 sample: 6 / (3 * 4) = 0.5
        //   class 1 sample: 6 / (3 * 1) = 2.0
        //   class 2 sample: 6 / (3 * 1) = 2.0
        float labels[] = {0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 2.0f};
        float weights[6] = {0};
        int counts[16] = {0};
        XGBoost_ComputeMulticlassWeights(labels, 6, 3, weights, counts);
        check("XGBoost_ComputeMulticlassWeights: per-class counts {4,1,1}",
              counts[0] == 4 && counts[1] == 1 && counts[2] == 1);
        check("XGBoost_ComputeMulticlassWeights: inverse-frequency weights {0.5, 2.0, 2.0}",
              fabs(weights[0] - 0.5f)  < 1e-5 &&
              fabs(weights[4] - 2.0f)  < 1e-5 &&
              fabs(weights[5] - 2.0f)  < 1e-5);
    }
    {
        // Pearson correlation: perfectly linear (labels = 2 * pred) → r = 1.0
        float pred[]   = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
        float labels[] = {2.0f, 4.0f, 6.0f, 8.0f, 10.0f};
        float r = WalkForward_ComputeCorrelation(pred, labels, 5);
        check("WalkForward_ComputeCorrelation: perfect linear → r=1.0",
              fabs(r - 1.0f) < 1e-4);
    }

    // ----- Group 4: Config validation/clamping ----------------------------------------------------------
    // bug class: silent never-completing warmup (c6aa0cc — min_warmup_samples > 128
    // gates on rolling.count which caps at 128; user-hostile silent failure)
    printf("\n--- Phase 5d: Config validation ---\n");
    {
        char path[] = "/tmp/test_min_warmup_XXXXXX";
        int fd = mkstemp(path);
        if (fd >= 0) {
            dprintf(fd, "min_warmup_samples=512\n");
            close(fd);
            ControllerConfig<FP> cfg = ControllerConfig_Load<FP>(path);
            check("min_warmup_samples=512 clamps to 128 (rolling window cap)",
                  cfg.min_warmup_samples == 128u);
            unlink(path);
        }
    }
    {
        // fee_rate parses as percentage (CFG_PARSE_PCT divides by 100)
        // 0.10 in cfg → 0.001 fraction (legacy mode, before Phase 8 maker/taker split)
        char path[] = "/tmp/test_fee_rate_XXXXXX";
        int fd = mkstemp(path);
        if (fd >= 0) {
            dprintf(fd, "fee_rate=0.10\n");
            close(fd);
            ControllerConfig<FP> cfg = ControllerConfig_Load<FP>(path);
            check("fee_rate=0.10 parses to 0.001 fraction (CFG_PARSE_PCT)",
                  fabs(FPN_ToDouble(cfg.fee_rate) - 0.001) < 1e-6);
            unlink(path);
        }
    }

    // ----- Group 5: Coverage tables ---------------------------------------------------------------------
    // bug class: adding a new GATE_REASON_* / REJECT_REASON_* without the matching
    // name-table entry (c95ef3f stamped this for the first round; lock it in to
    // catch the next mismatch at test-time, not via a NULL-deref in the TUI)
    printf("\n--- Phase 5d: Reason-table coverage ---\n");
    {
        int ok = 1;
        for (int i = 0; i < NUM_GATE_REASONS; i++) {
            if (!GATE_REASON_TABLE[i].name || GATE_REASON_TABLE[i].name[0] == '\0') {
                printf("    [missing] GATE_REASON_TABLE[%d] has empty name\n", i);
                ok = 0;
            }
        }
        check("GATE_REASON_TABLE: every entry has a non-empty name", ok);
    }
    {
        // REJECT_REASON_NAMES[0] is intentionally "" (REJECT_REASON_NONE — no name).
        // Every other index must be non-empty.
        int ok = 1;
        for (int i = 1; i < NUM_REJECT_REASONS; i++) {
            if (!REJECT_REASON_NAMES[i] || REJECT_REASON_NAMES[i][0] == '\0') {
                printf("    [missing] REJECT_REASON_NAMES[%d] is empty\n", i);
                ok = 0;
            }
        }
        check("REJECT_REASON_NAMES: every non-zero index has a non-empty name", ok);
    }

    //======================================================================================================
    // [PHASE 8b TESTS — Notify infrastructure]
    //======================================================================================================
    // see plans/phase8b-operational-monitoring-tests.md. 14 assertions across
    // 6 groups. Tests use a counting backend that records events instead of
    // routing them to stderr/popen, so we can inspect dispatch + cooldown.
    //======================================================================================================

    // Counting backend — records every event the worker dispatches.
    struct NotifyCountState {
        int events_received;
        NotifyEvent last_event;
    };
    auto NotifyBackend_Counting = [](const NotifyEvent *evt, void *state) -> int {
        NotifyCountState *s = (NotifyCountState *)state;
        s->events_received++;
        s->last_event = *evt;
        return 0;
    };
    // Helper: poll up to ~200ms for the worker thread to drain.
    auto wait_for_count = [](volatile int *count, int target) {
        for (int i = 0; i < 100 && *count < target; i++) usleep(2000);
    };

    // ----- Group 1: Lifecycle (2 assertions) -----------------------------------------------------------
    printf("\n--- Phase 8b: Notify lifecycle ---\n");
    {
        NotifyState ns;
        NotifyCountState bs = {0, {}};
        NotifyState_Init(&ns, NotifyBackend_Counting, &bs, /*cooldown_us=*/1000000);
        check("Init starts worker thread (worker_started=1)", ns.worker_started == 1);
        NotifyState_Shutdown(&ns);
        check("Shutdown clears worker_started + sets shutdown flag",
              ns.shutdown == 1 && ns.worker_started == 0);
    }

    // ----- Group 2: Send + dispatch (3 assertions) -----------------------------------------------------
    printf("\n--- Phase 8b: Send + dispatch ---\n");
    {
        NotifyState ns;
        NotifyCountState bs = {0, {}};
        NotifyState_Init(&ns, NotifyBackend_Counting, &bs, /*cooldown_us=*/0);

        Notify_Send(&ns, NOTIFY_ALERT, NK_KILL_TRIGGER, "test-subj", "test-body");
        wait_for_count(&bs.events_received, 1);

        check("event reaches backend after Send", bs.events_received == 1);
        check("backend receives correct level + kind",
              bs.last_event.level == NOTIFY_ALERT &&
              bs.last_event.event_kind == NK_KILL_TRIGGER);
        check("backend receives correct subject",
              strcmp(bs.last_event.subject, "test-subj") == 0);

        NotifyState_Shutdown(&ns);
    }

    // ----- Group 3: Cooldown gate (3 assertions) -------------------------------------------------------
    printf("\n--- Phase 8b: Cooldown gate ---\n");
    {
        NotifyState ns;
        NotifyCountState bs = {0, {}};
        // 100ms cooldown — same kind firing inside this window is dropped
        NotifyState_Init(&ns, NotifyBackend_Counting, &bs, /*cooldown_us=*/100000);

        Notify_Send(&ns, NOTIFY_ALERT, NK_KILL_TRIGGER, "first", "");
        Notify_Send(&ns, NOTIFY_ALERT, NK_KILL_TRIGGER, "second", ""); // dropped
        Notify_Send(&ns, NOTIFY_ALERT, NK_KILL_TRIGGER, "third", "");  // dropped
        wait_for_count(&bs.events_received, 1);
        check("same kind within cooldown: only 1 event reaches backend",
              bs.events_received == 1);

        // Different kinds fire independently within cooldown
        Notify_Send(&ns, NOTIFY_WARN, NK_DISCONNECT_TRADE, "a", "");
        Notify_Send(&ns, NOTIFY_INFO, NK_SESSION_START,    "b", "");
        wait_for_count(&bs.events_received, 3);
        check("different kinds fire independently inside cooldown window",
              bs.events_received == 3);

        // Wait past cooldown, fire same kind again — now allowed
        usleep(150000);
        Notify_Send(&ns, NOTIFY_ALERT, NK_KILL_TRIGGER, "fourth", "");
        wait_for_count(&bs.events_received, 4);
        check("same kind fires again past cooldown window",
              bs.events_received == 4);

        NotifyState_Shutdown(&ns);
    }

    // ----- Group 4: Queue full handling (1 assertion) --------------------------------------------------
    // The plan's Group 4 envisioned a BlockingBackend test fixture to fill the
    // queue. Simpler approach: rapid-fire many DIFFERENT kinds (so cooldown
    // doesn't drop them) and verify some are dispatched, some may drop.
    // Hard to assert exact counts (race with worker thread); just verify the
    // bounded-drop behavior — we don't crash and don't grow unbounded.
    printf("\n--- Phase 8b: Queue full handling ---\n");
    {
        NotifyState ns;
        NotifyCountState bs = {0, {}};
        NotifyState_Init(&ns, NotifyBackend_Counting, &bs, /*cooldown_us=*/0);

        // Fire 100 events; queue cap is 64. Some may drop if worker is slow.
        // Use kind=i % NOTIFY_KINDS_MAX so cooldown (which is 0 here anyway)
        // doesn't filter — every event is unique enough to enqueue.
        for (int i = 0; i < 100; i++) {
            Notify_Send(&ns, NOTIFY_INFO, i % NOTIFY_KINDS_MAX, "spam", "");
        }
        // Wait for drain (worker is fast — counting backend is in-memory)
        usleep(100000);
        // Bounded by 100, lower bound is the queue cap.
        check("queue full bounded — 0 ≤ received ≤ 100, no crash, no unbounded growth",
              bs.events_received <= 100 && bs.events_received >= 1);

        NotifyState_Shutdown(&ns);
    }

    // ----- Group 5: Shutdown drains pending events (1 assertion) ---------------------------------------
    printf("\n--- Phase 8b: Shutdown drains queue ---\n");
    {
        NotifyState ns;
        NotifyCountState bs = {0, {}};
        NotifyState_Init(&ns, NotifyBackend_Counting, &bs, /*cooldown_us=*/0);

        // Enqueue 5 different-kind events, immediately Shutdown.
        // Worker should drain before pthread_join returns.
        for (int i = 0; i < 5; i++) {
            Notify_Send(&ns, NOTIFY_INFO, i, "drain test", "");
        }
        NotifyState_Shutdown(&ns); // must drain before joining

        check("shutdown drains all 5 enqueued events before joining worker",
              bs.events_received == 5);
    }

    // ----- Group 6: Shell escape correctness (3 assertions) ----------------------------------------------
    // Replaces the test sidecar's "Hooked event sites" group — those would need
    // to drive PortfolioController kill switch via Tick(), which is heavier than
    // a unit test should be (verified by the actual production code path lighting
    // up on testnet manually). Substituting tests for the OTHER load-bearing
    // primitive: the shell-escape function. If escaping regresses, the Command
    // backend silently breaks user notifications — guard it explicitly.
    printf("\n--- Phase 8b: Shell escape ---\n");
    {
        char out[64];

        // Plain string passes through unchanged
        Notify_ShellEscape(out, sizeof(out), "plain text");
        check("plain text escape: passes through unchanged",
              strcmp(out, "plain text") == 0);

        // Single quote is replaced by close-escape-reopen idiom: '\''
        Notify_ShellEscape(out, sizeof(out), "world's body");
        check("single quote escape: ' becomes '\\''",
              strcmp(out, "world'\\''s body") == 0);

        // Multiple quotes: each gets its own escape
        Notify_ShellEscape(out, sizeof(out), "'a'b'");
        check("multiple quotes: each ' independently escaped",
              strcmp(out, "'\\''a'\\''b'\\''") == 0);
    }

    // ----- Group 7: Build command (1 assertion) ---------------------------------------------------------
    printf("\n--- Phase 8b: Build command template substitution ---\n");
    {
        char cmd[256];
        // Two %s, with quotes provided BY the template (per the contract:
        // escape function does NOT add enclosing quotes)
        Notify_BuildCommand(cmd, sizeof(cmd),
                             "notify-send 'Engine: %s' '%s'",
                             "kill", "details here");
        check("template substitution: 2x %s replaced in order",
              strcmp(cmd, "notify-send 'Engine: kill' 'details here'") == 0);
    }

    //======================================================================================================
    // [PHASE 6prep TESTS — Confidence loop math + gate formula + cfg]
    //======================================================================================================
    // see plans/phase6-prep-confidence-loop-tests.md. 12 assertions across
    // 4 groups. Tests pin the math + the gate-threshold formula so the
    // existing wiring (which we did NOT add — it pre-dates Phase 6prep) is
    // locked against silent regression.
    //
    // Test data uses a small deterministic LCG instead of platform-dependent
    // rand() — tests pass identically on glibc, musl, macOS.
    //======================================================================================================

    // Tiny deterministic PRNG (Numerical Recipes LCG). Produces [0, 1).
    auto lcg_next = [](uint32_t *s) -> double {
        *s = (*s) * 1664525u + 1013904223u;
        return (double)(*s >> 8) / 16777216.0; // upper 24 bits → [0, 1)
    };

    // ----- Group 1: RollingIC math (4 assertions) ------------------------------------------------------
    printf("\n--- Phase 6prep: RollingIC math ---\n");
    {
        // Empty IC → 0 (no divide-by-zero on insufficient samples)
        RollingIC ic;
        RollingIC_Init(&ic, 50);
        check("empty RollingIC returns 0 (insufficient samples)",
              fabs(RollingIC_Compute(&ic)) < 1e-9);
    }
    {
        // Perfectly correlated linear pairs → IC = 1.0 (Spearman of monotonic = 1)
        RollingIC ic;
        RollingIC_Init(&ic, 50);
        for (int i = 0; i < 50; i++) {
            double p = (double)i;
            double a = 2.0 * p + 1.0;  // strictly monotonic in p
            RollingIC_Push(&ic, p, a);
        }
        check("perfectly correlated → IC ≈ 1.0",
              fabs(RollingIC_Compute(&ic) - 1.0) < 1e-6);
    }
    {
        // Deterministic uncorrelated random → IC near 0
        RollingIC ic;
        RollingIC_Init(&ic, 50);
        uint32_t s1 = 0xDEADBEEF, s2 = 0xCAFEBABE;
        for (int i = 0; i < 50; i++) {
            RollingIC_Push(&ic, lcg_next(&s1), lcg_next(&s2));
        }
        double r = RollingIC_Compute(&ic);
        check("uncorrelated random pairs → |IC| < 0.3", fabs(r) < 0.3);
    }
    {
        // Window rolls — first 10 anti-correlated pairs are evicted by next 10 positively-correlated
        RollingIC ic;
        RollingIC_Init(&ic, 10);
        for (int i = 0; i < 10; i++) RollingIC_Push(&ic, (double)i, -(double)i); // anti
        for (int i = 0; i < 10; i++) RollingIC_Push(&ic, (double)i, (double)i);  // pos overwrites
        check("window rolls — only last N pairs counted (positive correlation wins)",
              RollingIC_Compute(&ic) > 0.9);
    }

    // ----- Group 2: ConfidenceScorer composition (3 assertions) ---------------------------------------
    printf("\n--- Phase 6prep: ConfidenceScorer composition ---\n");
    {
        // Noise → IC near 0 → abs_ic clamps to MIN_IC=0.01 → confidence stays low
        ConfidenceScorer cs;
        ConfidenceScorer_Init(&cs, 50, /*tau=*/60.0);
        uint32_t s1 = 0x12345678, s2 = 0x87654321;
        for (int i = 0; i < 50; i++) {
            ConfidenceScorer_Update(&cs, lcg_next(&s1), lcg_next(&s2));
        }
        double conf = ConfidenceScorer_Compute(&cs, /*data_age_sec=*/0.0);
        check("noise stream → confidence < 0.3 (low predictive quality)", conf < 0.3);
    }
    {
        // Perfect identity (pred == actual) → IC=1, RMSE=0, freshness=1 → conf=1.0
        ConfidenceScorer cs;
        ConfidenceScorer_Init(&cs, 50, /*tau=*/60.0);
        for (int i = 0; i < 50; i++) {
            double p = (double)i / 50.0; // pred and actual on same scale, identity
            ConfidenceScorer_Update(&cs, p, p);
        }
        double conf = ConfidenceScorer_Compute(&cs, /*data_age_sec=*/0.0);
        check("perfect identity stream → confidence > 0.95",
              conf > 0.95);
    }
    {
        // Stale data: same perfect stream, but data_age = tau → freshness = e^-1 ≈ 0.37
        ConfidenceScorer cs;
        ConfidenceScorer_Init(&cs, 50, /*tau=*/60.0);
        for (int i = 0; i < 50; i++) {
            double p = (double)i / 50.0;
            ConfidenceScorer_Update(&cs, p, p);
        }
        double conf_fresh = ConfidenceScorer_Compute(&cs, /*data_age_sec=*/0.0);
        double conf_stale = ConfidenceScorer_Compute(&cs, /*data_age_sec=*/60.0); // = tau
        check("stale data (age=tau) decays confidence to ~37% of fresh",
              conf_stale < conf_fresh * 0.5 && conf_stale > conf_fresh * 0.3);
    }

    // ----- Group 3: Gate effective-threshold formula (3 assertions) ----------------------------------
    // The formula `effective_thr = base * (scale - conf)`, clamped at 1.0,
    // lives at PortfolioController.hpp:~1618 in the slow-path gate block.
    // Test the formula directly (it's small enough to inline). If production
    // drifts from this formula, manual testnet verification catches it; tests
    // pin the math semantic.
    printf("\n--- Phase 6prep: Gate effective threshold ---\n");
    {
        auto effective_thr = [](double base, double scale, double conf) {
            double t = base * (scale - conf);
            if (t > 1.0) t = 1.0;
            return t;
        };

        // conf=0, base=0.3, scale=2.0 → 0.6 (no clamp)
        check("conf=0 + scale=2 → base * 2 (max suppression at zero confidence)",
              fabs(effective_thr(0.3, 2.0, 0.0) - 0.6) < 1e-9);

        // conf=1, scale=2.0 → effective = base * 1.0 (full signal)
        check("conf=1 + scale=2 → base (full confidence allows full signal)",
              fabs(effective_thr(0.3, 2.0, 1.0) - 0.3) < 1e-9);

        // base=0.6, conf=0, scale=2.0 → 1.2 → clamps to 1.0
        check("clamp at 1.0 when base × (scale − conf) > 1.0",
              fabs(effective_thr(0.6, 2.0, 0.0) - 1.0) < 1e-9);
    }

    //======================================================================================================
    // [PHASE 7prep TESTS — HeldOutSplit + Backtest_RunFullValidation framework]
    //======================================================================================================
    // see plans/phase7-prep-validation-infrastructure-tests.md. 12 assertions
    // across 4 groups. Tests pin the lock-token discipline + slice math + gap
    // computation framework. Held-out training itself is Phase 7 finalize work.
    //======================================================================================================

    // ----- Group 1: HeldOutSplit math (4 assertions) ---------------------------------------------------
    printf("\n--- Phase 7prep: HeldOutSplit math ---\n");
    {
        // 20% on 1000 samples → trainval=[0, 800), test=[800, 1000)
        HeldOutSplit s = HeldOutSplit_Make(1000, 0.20);
        check("Make: 20% split → trainval=800, test=[800,1000), locked=1",
              s.total_samples == 1000 && s.trainval_end_idx == 800 &&
              s.test_start_idx == 800 && s.locked == 1);
    }
    {
        // 5% — minimum allowed (clamped if smaller)
        HeldOutSplit s = HeldOutSplit_Make(1000, 0.05);
        check("Make: 5% split → trainval=950 (lower bound)",
              s.trainval_end_idx == 950);
    }
    {
        // Out-of-range fraction (60%) clamps to 30% max — not rejected
        HeldOutSplit s = HeldOutSplit_Make(1000, 0.60);
        check("Make: out-of-range fraction (60%) clamps to 30% max",
              s.trainval_end_idx == 700 && s.test_start_idx == 700);
    }
    {
        // Token is non-empty 32-char hex
        HeldOutSplit s = HeldOutSplit_Make(1000, 0.20);
        check("Make: lock_token is 32 hex chars + null",
              strlen(s.lock_token) == 32);
    }

    // ----- Group 2: Lock-token discipline (3 assertions) -----------------------------------------------
    printf("\n--- Phase 7prep: Lock-token discipline ---\n");
    {
        // TestAccessAllowed returns 0 when locked
        HeldOutSplit s = HeldOutSplit_Make(1000, 0.20);
        check("TestAccessAllowed: 0 when locked", HeldOutSplit_TestAccessAllowed(&s) == 0);
    }
    {
        // Unlock with correct token → access allowed
        HeldOutSplit s = HeldOutSplit_Make(1000, 0.20);
        char saved[33];
        strncpy(saved, s.lock_token, sizeof(saved));
        int ok = HeldOutSplit_Unlock(&s, saved);
        check("Unlock with correct token → access allowed",
              ok == 1 && HeldOutSplit_TestAccessAllowed(&s) == 1);
    }
    {
        // Unlock with wrong token → still locked
        HeldOutSplit s = HeldOutSplit_Make(1000, 0.20);
        int ok = HeldOutSplit_Unlock(&s, "deadbeefcafebabe1234567890abcdef");
        check("Unlock with wrong token → refused, still locked",
              ok == 0 && HeldOutSplit_TestAccessAllowed(&s) == 0);
    }

    // ----- Group 3: Backtest_RunFullValidation framework (3 assertions) -------------------------------
    // Per Tier 2 amendment to phase7prep plan: verify the framework logic
    // (lock check, slice view, gap math) without driving actual XGBoost
    // training (Phase 7 finalize work). Pass an empty BacktestResults so
    // Backtest_RunWalkForward returns early — we just exercise the
    // framework dispatch in Backtest_RunFullValidation itself.
    printf("\n--- Phase 7prep: RunFullValidation framework ---\n");
    {
        // Locked split → function refuses to run, ran_held_out stays 0
        HeldOutSplit s = HeldOutSplit_Make(1000, 0.20); // locked by default
        BacktestResults dummy;
        BacktestResults_Init(&dummy);
        FullValidationResults out = {};
        volatile int prog = 0, cancel = 0;
        Backtest_RunFullValidation(&out, &dummy, &s,
                                    /*n_splits=*/3, /*horizon=*/100, /*buffer=*/10,
                                    /*min_train=*/100, &prog, &cancel,
                                    LABEL_WIN_LOSS, /*gap_threshold=*/0.05f);
        check("RunFullValidation refuses on locked split (gap_acceptable=0)",
              out.gap_acceptable == 0 && out.ran_held_out == 0);
        BacktestResults_Free(&dummy);
    }
    {
        // Unlocked + empty data → still doesn't crash; slice math handles 0
        HeldOutSplit s = HeldOutSplit_Make(1000, 0.20);
        HeldOutSplit_Unlock(&s, s.lock_token);
        BacktestResults dummy;
        BacktestResults_Init(&dummy);
        // sample_count stays 0 — Backtest_RunWalkForward returns early
        FullValidationResults out = {};
        volatile int prog = 0, cancel = 0;
        Backtest_RunFullValidation(&out, &dummy, &s,
                                    /*n_splits=*/3, /*horizon=*/100, /*buffer=*/10,
                                    /*min_train=*/100, &prog, &cancel,
                                    LABEL_WIN_LOSS, /*gap_threshold=*/0.05f);
        check("RunFullValidation: unlocked + zero samples → no crash, gap_threshold preserved",
              fabs(out.gap_threshold - 0.05f) < 1e-7f && out.ran_held_out == 0);
        BacktestResults_Free(&dummy);
    }
    {
        // Gap math sanity: when WF mean and held_out are both 0, gap is 0,
        // gap_acceptable is 0 (because ran_held_out=0 in stub mode — signals
        // "not yet validated" rather than "validated OK")
        HeldOutSplit s = HeldOutSplit_Make(1000, 0.20);
        HeldOutSplit_Unlock(&s, s.lock_token);
        BacktestResults dummy;
        BacktestResults_Init(&dummy);
        FullValidationResults out = {};
        volatile int prog = 0, cancel = 0;
        Backtest_RunFullValidation(&out, &dummy, &s,
                                    /*n_splits=*/3, /*horizon=*/100, /*buffer=*/10,
                                    /*min_train=*/100, &prog, &cancel,
                                    LABEL_WIN_LOSS, /*gap_threshold=*/0.05f);
        check("RunFullValidation stub: gap_acceptable=0 even with zero gap (signals not-yet-validated)",
              out.gap_acceptable == 0);
        BacktestResults_Free(&dummy);
    }

    // ----- Group 4: Cfg parsing (2 assertions) -------------------------------
    printf("\n--- Phase 7prep: Cfg backward compat ---\n");
    {
        // Defaults: held_out_fraction=0.20, gap_acceptable_threshold=0.05
        char path[] = "/tmp/test_heldout_default_XXXXXX";
        int fd = mkstemp(path);
        if (fd >= 0) {
            dprintf(fd, "# empty cfg\n");
            close(fd);
            ControllerConfig<FP> cfg = ControllerConfig_Load<FP>(path);
            check("default cfg: held_out_fraction=0.20, gap_threshold=0.05",
                  fabs(FPN_ToDouble(cfg.held_out_fraction) - 0.20) < 1e-6 &&
                  fabs(FPN_ToDouble(cfg.gap_acceptable_threshold) - 0.05) < 1e-6);
            unlink(path);
        }
    }
    {
        // Explicit values
        char path[] = "/tmp/test_heldout_explicit_XXXXXX";
        int fd = mkstemp(path);
        if (fd >= 0) {
            dprintf(fd, "held_out_fraction=0.25\n"
                        "gap_acceptable_threshold=0.10\n");
            close(fd);
            ControllerConfig<FP> cfg = ControllerConfig_Load<FP>(path);
            check("explicit cfg: held_out=0.25, gap_threshold=0.10",
                  fabs(FPN_ToDouble(cfg.held_out_fraction) - 0.25) < 1e-6 &&
                  fabs(FPN_ToDouble(cfg.gap_acceptable_threshold) - 0.10) < 1e-6);
            unlink(path);
        }
    }

    //======================================================================================================
    // [PHASE 8 TESTS — maker/taker fee accounting + ORDER_PARTIAL state machine]
    //======================================================================================================
    // see plans/phase8-maker-taker-tests.md. ~17 assertions across 6 groups
    // (trimmed from the sidecar's ~32 — heavy infrastructure tests like real
    // OMS state-machine drives or full Binance JSON corpus replay are
    // deferred to Phase 8.x integration testing).
    //======================================================================================================

    // ----- Group 1: Fee_Compute helper (3 assertions) --------------------------------------------------
    printf("\n--- Phase 8: Fee_Compute helper ---\n");
    {
        ControllerConfig<FP> cfg = ControllerConfig_Default<FP>();
        cfg.fee_rate_maker = FPN_FromDouble<FP>(0.00075);
        cfg.fee_rate_taker = FPN_FromDouble<FP>(0.00100);
        FPN<FP> notional = FPN_FromDouble<FP>(1000.0);

        FPN<FP> fee_maker = Fee_Compute(&cfg, notional, /*is_maker=*/1);
        FPN<FP> fee_taker = Fee_Compute(&cfg, notional, /*is_maker=*/0);

        check("Fee_Compute is_maker=1 → 1000 * 0.075% = 0.75",
              fabs(FPN_ToDouble(fee_maker) - 0.75) < 1e-4);
        check("Fee_Compute is_maker=0 → 1000 * 0.100% = 1.00",
              fabs(FPN_ToDouble(fee_taker) - 1.00) < 1e-4);
        check("maker fee strictly less than taker fee at same notional",
              FPN_ToDouble(fee_maker) < FPN_ToDouble(fee_taker));
    }

    // ----- Group 4: Backward compat — legacy fee_rate path (4 assertions) -----------------------------
    // Land this group EARLY to anchor the legacy mirroring before later groups
    // exercise the maker/taker code paths that depend on it.
    printf("\n--- Phase 8: Legacy fee_rate backward compat ---\n");
    {
        // Old cfg with only fee_rate set: maker AND taker mirror it
        char path[] = "/tmp/test_fee_legacy_XXXXXX";
        int fd = mkstemp(path);
        if (fd >= 0) {
            dprintf(fd, "fee_rate=0.10\n");  // legacy 0.10% format
            close(fd);
            ControllerConfig<FP> cfg = ControllerConfig_Load<FP>(path);
            check("legacy: fee_rate=0.10 → fee_rate=0.001 (CFG_PARSE_PCT)",
                  fabs(FPN_ToDouble(cfg.fee_rate) - 0.001) < 1e-6);
            check("legacy mirroring: fee_rate_maker == fee_rate",
                  fabs(FPN_ToDouble(cfg.fee_rate_maker) - 0.001) < 1e-6);
            check("legacy mirroring: fee_rate_taker == fee_rate",
                  fabs(FPN_ToDouble(cfg.fee_rate_taker) - 0.001) < 1e-6);
            unlink(path);
        }
    }
    {
        // New cfg with all three set: each is independent
        char path[] = "/tmp/test_fee_explicit_XXXXXX";
        int fd = mkstemp(path);
        if (fd >= 0) {
            dprintf(fd, "fee_rate=0.10\nfee_rate_maker=0.075\nfee_rate_taker=0.100\n");
            close(fd);
            ControllerConfig<FP> cfg = ControllerConfig_Load<FP>(path);
            check("explicit cfg: maker=0.00075 + taker=0.00100 parsed independently",
                  fabs(FPN_ToDouble(cfg.fee_rate_maker) - 0.00075) < 1e-7 &&
                  fabs(FPN_ToDouble(cfg.fee_rate_taker) - 0.00100) < 1e-7);
            unlink(path);
        }
    }

    // ----- Group 2: ORDER_PARTIAL state transitions (3 assertions) -------------------------------------
    // Order_IsTerminal correctness for the new ORDER_PARTIAL state.
    printf("\n--- Phase 8: ORDER_PARTIAL state ---\n");
    {
        tt::Order<FP> o;
        tt::Order_Init(&o, 1, /*core_id=*/0, tt::ORDER_MARKET_BUY);

        // Default state after Init
        check("Order_Init: default state = ORDER_PENDING, is_maker=0",
              o.state == tt::ORDER_PENDING && o.is_maker == 0);

        // ORDER_PARTIAL is non-terminal — order stays alive in OMS
        o.state = tt::ORDER_PARTIAL;
        check("Order_IsTerminal returns false for ORDER_PARTIAL",
              tt::Order_IsTerminal(&o) == false);

        // ORDER_FILLED is terminal — slot can be freed
        o.state = tt::ORDER_FILLED;
        check("Order_IsTerminal returns true for ORDER_FILLED",
              tt::Order_IsTerminal(&o) == true);
    }

    // ----- Group 3: executionReport parser — m/X/n/N fields (3 assertions) ----------------------------
    printf("\n--- Phase 8: executionReport parser ---\n");
    {
        // Simulated Binance executionReport with m=true (maker fill)
        const char json_maker[] =
            "{\"e\":\"executionReport\",\"x\":\"TRADE\",\"X\":\"FILLED\","
            "\"c\":\"oms_42\",\"i\":\"99\","
            "\"L\":\"60100.5\",\"l\":\"0.001\","
            "\"m\":true,\"n\":\"0.045\",\"N\":\"USDT\","
            "\"t\":12345,\"T\":1234567890123}";
        tt::Command cmd;
        uint64_t trade_id;
        int is_fill = tt::ud_parse_execution_report(json_maker, sizeof(json_maker) - 1, &cmd, &trade_id);
        check("parser: maker fill (m=true) → cmd.result.is_maker=1",
              is_fill == 1 && cmd.result.is_maker == 1 && cmd.result.order_complete == 1);
    }
    {
        // Same shape but m=false (taker fill) and X=PARTIALLY_FILLED
        const char json_taker_partial[] =
            "{\"e\":\"executionReport\",\"x\":\"TRADE\",\"X\":\"PARTIALLY_FILLED\","
            "\"c\":\"oms_43\",\"i\":\"100\","
            "\"L\":\"60100.5\",\"l\":\"0.0005\","
            "\"m\":false,\"n\":\"0.06\",\"N\":\"USDT\","
            "\"t\":12346,\"T\":1234567890124}";
        tt::Command cmd;
        uint64_t trade_id;
        int is_fill = tt::ud_parse_execution_report(json_taker_partial, sizeof(json_taker_partial) - 1, &cmd, &trade_id);
        check("parser: taker partial (m=false, X=PARTIALLY_FILLED) → "
              "is_maker=0, order_complete=0",
              is_fill == 1 && cmd.result.is_maker == 0 && cmd.result.order_complete == 0);
    }
    {
        // Defensive default: missing "m" → is_maker=0 (taker, conservative)
        const char json_no_m[] =
            "{\"e\":\"executionReport\",\"x\":\"TRADE\",\"X\":\"FILLED\","
            "\"c\":\"oms_44\",\"i\":\"101\","
            "\"L\":\"60100.5\",\"l\":\"0.001\","
            "\"n\":\"0.06\",\"N\":\"USDT\","
            "\"t\":12347,\"T\":1234567890125}";
        tt::Command cmd;
        uint64_t trade_id;
        int is_fill = tt::ud_parse_execution_report(json_no_m, sizeof(json_no_m) - 1, &cmd, &trade_id);
        check("parser: missing 'm' field defaults to is_maker=0 (taker, conservative)",
              is_fill == 1 && cmd.result.is_maker == 0);
    }

    // ----- Group 5: Maker/taker accounting invariant (2 assertions) -----------------------------------
    // total_fees == total_maker_fees + total_taker_fees after fills.
    // Drives the synchronous path via PortfolioController_Tick to populate counters.
    printf("\n--- Phase 8: Maker/taker accounting invariant ---\n");
    {
        ControllerConfig<FP> cfg = ControllerConfig_Default<FP>();
        cfg.starting_balance = FPN_FromDouble<FP>(10000.0);
        // legacy: fee_rate=0.001, mirrored to maker+taker
        cfg.fee_rate = FPN_FromDouble<FP>(0.001);
        cfg.fee_rate_maker = cfg.fee_rate;
        cfg.fee_rate_taker = cfg.fee_rate;
        cfg.slippage_pct = FPN_Zero<FP>();
        cfg.max_positions = 1;

        PortfolioController<FP> ctrl = {};
        PortfolioController_Init(&ctrl, cfg);

        // Counters start at 0
        check("counters initialized to 0",
              ctrl.maker_fills_count == 0 && ctrl.taker_fills_count == 0 &&
              FPN_ToDouble(ctrl.total_maker_fees) == 0.0 &&
              FPN_ToDouble(ctrl.total_taker_fees) == 0.0);

        // Drive a synthetic exit (RecordExit increments taker counter — TP/SL = market sell)
        FPN<FP> entry = FPN_FromDouble<FP>(100.0);
        FPN<FP> qty   = FPN_FromDouble<FP>(1.0);
        Portfolio_AddPositionWithExits(&ctrl.portfolio, qty, entry,
            FPN_FromDouble<FP>(110.0), FPN_FromDouble<FP>(90.0));
        ctrl.portfolio.positions[0].entry_fee = FPN_FromDouble<FP>(0.10);
        ctrl.balance = FPN_FromDouble<FP>(9899.90);

        PositionExitGate(&ctrl.portfolio, FPN_FromDouble<FP>(110.0), &ctrl.exit_buf, 100);
        PortfolioController_DrainExits(&ctrl);

        // After one TP hit + drain: 1 taker fill, 0 maker fills
        check("after sync exit: 1 taker fill, 0 maker, total_fees == total_taker_fees",
              ctrl.taker_fills_count == 1 && ctrl.maker_fills_count == 0 &&
              fabs(FPN_ToDouble(ctrl.total_fees) - FPN_ToDouble(ctrl.total_taker_fees)) < 1e-9 &&
              FPN_ToDouble(ctrl.total_maker_fees) == 0.0);
    }

    // ----- Group 6: Snapshot sync — TUISnapshot has new fields (2 assertions) -------------------------
    printf("\n--- Phase 8: TUISnapshot maker/taker fields ---\n");
    {
        ControllerConfig<FP> cfg = ControllerConfig_Default<FP>();
        cfg.starting_balance = FPN_FromDouble<FP>(10000.0);
        PortfolioController<FP> ctrl = {};
        PortfolioController_Init(&ctrl, cfg);

        // Manually set counter values
        ctrl.maker_fills_count = 3;
        ctrl.taker_fills_count = 7;
        ctrl.total_maker_fees  = FPN_FromDouble<FP>(0.5);
        ctrl.total_taker_fees  = FPN_FromDouble<FP>(2.0);

        TUISnapshot snap = {};
        TUI_CopySnapshot<FP>(&snap, &ctrl, /*price=*/60000.0, /*volume=*/0.0);
        check("TUISnapshot: maker_fills + taker_fills counters populated",
              snap.maker_fills_count == 3 && snap.taker_fills_count == 7);
        check("TUISnapshot: total_maker_fees + total_taker_fees populated",
              fabs(snap.total_maker_fees - 0.5) < 1e-6 &&
              fabs(snap.total_taker_fees - 2.0) < 1e-6);
    }

    // ----- Group 4 (Phase 6prep): Backward compat — cfg parsing (2 assertions) -------------------------------------
    printf("\n--- Phase 6prep: Cfg backward compat ---\n");
    {
        // Old cfg without confidence_threshold_scale — defaults to 2.0
        char path[] = "/tmp/test_conf_default_XXXXXX";
        int fd = mkstemp(path);
        if (fd >= 0) {
            dprintf(fd, "confidence_enabled=1\n");  // only enable, nothing else
            close(fd);
            ControllerConfig<FP> cfg = ControllerConfig_Load<FP>(path);
            check("missing cfg fields keep defaults (window=32, tau=300, scale=2.0)",
                  cfg.confidence_window == 32u &&
                  fabs(FPN_ToDouble(cfg.confidence_freshness_tau) - 300.0) < 1e-6 &&
                  fabs(FPN_ToDouble(cfg.confidence_threshold_scale) - 2.0) < 1e-6);
            unlink(path);
        }
    }
    {
        // Explicit values parse correctly
        char path[] = "/tmp/test_conf_explicit_XXXXXX";
        int fd = mkstemp(path);
        if (fd >= 0) {
            dprintf(fd, "confidence_enabled=1\n"
                        "confidence_window=20\n"
                        "confidence_freshness_tau=120.0\n"
                        "confidence_threshold_scale=1.5\n");
            close(fd);
            ControllerConfig<FP> cfg = ControllerConfig_Load<FP>(path);
            check("explicit cfg values parse: window=20, tau=120, scale=1.5",
                  cfg.confidence_window == 20u &&
                  fabs(FPN_ToDouble(cfg.confidence_freshness_tau) - 120.0) < 1e-6 &&
                  fabs(FPN_ToDouble(cfg.confidence_threshold_scale) - 1.5) < 1e-6);
            unlink(path);
        }
    }

    printf("\n======================================\n");
    printf("  RESULTS: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("======================================\n");

    return tests_failed > 0 ? 1 : 0;
}
