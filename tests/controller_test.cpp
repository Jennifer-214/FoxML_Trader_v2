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
#include <string.h>
#include <math.h>
#include "../DataStream/MockGenerator.hpp"
#include "../CoreFrameworks/PortfolioController.hpp"

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

    remove("TEST_order_history.csv");

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
    FILE *f = fopen("TEST_order_history.csv", "r");
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
    remove("TEST_order_history.csv");
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

    remove("INTG_order_history.csv");

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
    FILE *f = fopen("INTG_order_history.csv", "r");
    check("trade log file created", f != 0);
    if (f) {
        int lines = 0;
        char line[512];
        while (fgets(line, sizeof(line), f)) lines++;
        check("trade log has entries", lines > 1); // header + at least one trade
        printf("  trade log lines: %d\n", lines);
        fclose(f);
    }
    remove("INTG_order_history.csv");
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

    printf("\n======================================\n");
    printf("  RESULTS: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("======================================\n");

    return tests_failed > 0 ? 1 : 0;
}
