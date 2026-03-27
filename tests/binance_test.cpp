// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [BINANCE INTEGRATION TEST]
//======================================================================================================
// connects to live data stream, runs the full pipeline for N ticks, prints results
// no TUI - just stderr output for test verification
// uses data-stream.binance.vision (no geo-restriction, public data)
//======================================================================================================
#include "../DataStream/BinanceCrypto.hpp"
#include "../CoreFrameworks/PortfolioController.hpp"
#include "../MemHeaders/PoolAllocator.hpp"
#include "../DataStream/TradeLog.hpp"
#include <stdio.h>

constexpr unsigned FP = 64;

int main() {
    //==================================================================================================
    // config
    //==================================================================================================
    BinanceConfig bcfg;
    strcpy(bcfg.symbol, "btcusdt");
    bcfg.use_testnet       = 0;
    bcfg.use_binance_us    = 0;
    bcfg.poll_timeout_ms   = 5000;
    bcfg.reconnect_delay   = 3;
    bcfg.wind_down_minutes = 5;

    ControllerConfig<FP> ccfg = ControllerConfig_Default<FP>();
    ccfg.warmup_ticks = 16;     // shorter warmup for testing
    ccfg.poll_interval = 10;    // faster slow-path for testing

    //==================================================================================================
    // init
    //==================================================================================================
    fprintf(stderr, "[TEST] connecting to Binance data stream...\n");

    BinanceStream bs;
    if (!BinanceStream_Init(&bs, &bcfg)) {
        fprintf(stderr, "[TEST] FAILED to connect\n");
        return 1;
    }

    PortfolioController<FP> ctrl = {};
    PortfolioController_Init(&ctrl, ccfg);

    OrderPool<FP> pool;
    OrderPool_init(&pool, 64);

    TradeLog log;
    TradeLog_Init(&log, "TEST_btcusdt");

    fprintf(stderr, "[TEST] connected - running pipeline for 200 ticks...\n");
    fprintf(stderr, "[TEST] warmup: %u ticks, poll_interval: %u\n",
            ccfg.warmup_ticks, ccfg.poll_interval);

    //==================================================================================================
    // run pipeline
    //==================================================================================================
    DataStream<FP> last_stream = {};
    int target_ticks = 200;
    int ticks = 0;
    int state_reported = 0;
    int has_data = 0;

    while (ticks < target_ticks) {
        int ready = BinanceStream_Poll(&bs, bcfg.poll_timeout_ms);

        if (ready == POLL_NONE) {
            fprintf(stderr, "[TEST] timeout at tick %d\n", ticks);
            continue;
        }

        if (ready & POLL_SOCKET) {
            // drain burst
            while (1) {
                DataStream<FP> tick;
                if (!BinanceStream_ReadTick(&bs, &tick)) {
                    fprintf(stderr, "[TEST] read failed at tick %d\n", ticks);
                    goto done;
                }
                last_stream = tick;
                has_data = 1;
                PositionExitGate(&ctrl.portfolio, last_stream.price, &ctrl.exit_buf, ctrl.total_ticks);
                if (!BinanceStream_HasPending(&bs)) break;
            }
        }

        // engine tick - only after first real data
        if (!has_data) continue;
        BuyGate(&ctrl.buy_conds, &last_stream, &pool);
        PositionExitGate(&ctrl.portfolio, last_stream.price, &ctrl.exit_buf, ctrl.total_ticks);
        PortfolioController_Tick(&ctrl, &pool, last_stream.price, last_stream.volume, &log);

        ticks++;

        // report warmup -> active transition
        if (ctrl.state == CONTROLLER_ACTIVE && !state_reported) {
            fprintf(stderr, "[TEST] WARMUP COMPLETE at tick %d\n", ticks);
            fprintf(stderr, "[TEST]   buy gate price:  %.2f\n", FPN_ToDouble(ctrl.buy_conds.price));
            fprintf(stderr, "[TEST]   buy gate volume: %.8f\n", FPN_ToDouble(ctrl.buy_conds.volume));
            state_reported = 1;
        }

        // periodic status
        if (ticks % 50 == 0) {
            double price = FPN_ToDouble(last_stream.price);
            double pnl   = FPN_ToDouble(ctrl.portfolio_delta);
            int positions = Portfolio_CountActive(&ctrl.portfolio);
            fprintf(stderr, "[TEST] tick %d/%d  price: %.2f  positions: %d  P&L: $%.4f  trades: %lu\n",
                    ticks, target_ticks, price, positions, pnl, (unsigned long)log.trade_count);
        }
    }

done:
    //==================================================================================================
    // results
    //==================================================================================================
    fprintf(stderr, "\n==================================================\n");
    fprintf(stderr, "  INTEGRATION TEST RESULTS\n");
    fprintf(stderr, "==================================================\n");
    fprintf(stderr, "  Ticks received:   %d\n", ticks);
    fprintf(stderr, "  Final price:      $%.2f\n", FPN_ToDouble(last_stream.price));
    fprintf(stderr, "  Open positions:   %d/16\n", Portfolio_CountActive(&ctrl.portfolio));
    fprintf(stderr, "  Unrealized P&L:   $%.4f\n", FPN_ToDouble(ctrl.portfolio_delta));
    fprintf(stderr, "  Trade log:        %lu entries\n", (unsigned long)log.trade_count);
    fprintf(stderr, "  Controller state: %s\n", ctrl.state == CONTROLLER_ACTIVE ? "ACTIVE" : "WARMUP");
    fprintf(stderr, "  Buy gate price:   $%.2f\n", FPN_ToDouble(ctrl.buy_conds.price));
    fprintf(stderr, "  Buy gate volume:  %.8f\n", FPN_ToDouble(ctrl.buy_conds.volume));
    fprintf(stderr, "==================================================\n");

    int passed = (ticks >= target_ticks) && (ctrl.state == CONTROLLER_ACTIVE);
    fprintf(stderr, "\n[TEST] %s\n", passed ? "PASSED" : "FAILED");

    TradeLog_Close(&log);
    BinanceStream_Close(&bs);
    free(pool.slots);

    return passed ? 0 : 1;
}
