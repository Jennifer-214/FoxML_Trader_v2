// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [SHARDED BACKTEST]
//======================================================================================================
// Phase 13 of the per-core sharding migration. Single-threaded sharded
// backtest path. Mirrors Backtest_Run's interface but uses the per-core
// architecture from CoreFrameworks/ShardedBacktestDriver.hpp internally.
//
// Why a separate file: the legacy Backtest_Run is ~350 LOC of replay logic
// tightly coupled to PortfolioController. Adding a second 200 LOC path inline
// would balloon the file. Keeping them separate makes it easy to:
//   1. Compare implementations side by side during the migration
//   2. Eventually delete the legacy path when sharded is the default
//   3. Test each path in isolation
//
// The dispatcher in BacktestEngine.hpp peeks at config.engine_mode and routes
// to either Backtest_Run (legacy) or BacktestSharded_Run (this file).
//
// What this DOES populate in BacktestResults:
//   - stats.total_pnl, stats.total_trades, stats.ticks_processed
//   - stats.win_rate, stats.avg_win, stats.avg_loss, stats.profit_factor
//   - stats.max_drawdown, stats.max_drawdown_pct
//   - equity_curve (per-trade snapshots)
//
// What this DOES NOT populate (out of scope for the first migration cut):
//   - feature_matrix (ML feature collection — sharded path doesn't have
//     RollingStats wired into per-core context yet)
//   - regime tracking, gate reason diagnostics
//   - per-strategy stats breakdown
//
// These can be added once the sharded path is the default and the legacy
// PortfolioController is wired into the controller core for the slow-path
// stuff that doesn't change with sharding.
//======================================================================================================

#pragma once

#include "../CoreFrameworks/ControllerConfig.hpp"
#include "../CoreFrameworks/EventLoopAggregates.hpp"
#include "../CoreFrameworks/ExecutionCore.hpp"
#include "../CoreFrameworks/ShardedBacktestDriver.hpp"
#include "../CoreFrameworks/Tick.hpp"
#include "../DataStream/EngineTUI.hpp"  // for TUISnapshot
#include "../FixedPoint/FixedPointN.hpp"
#include "../ML_Headers/RollingStats.hpp"
#include "BacktestEngine.hpp"  // for BacktestResults, BacktestRunConfig, BacktestData_Load
#include "LabelFunctions.hpp"  // for HistoricalTick

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/time.h>

namespace tt {

//======================================================================================================
// [HELPERS]
//======================================================================================================
// Convert a HistoricalTick (double-based, from Binance aggTrades) into a
// Tick<F> (the per-core architecture's tick struct). Done once per tick on
// the backtest's single thread; cost is negligible compared to the gate eval.
//======================================================================================================
template <unsigned F>
static inline Tick<F> SharedBacktest_FromHistorical(const HistoricalTick* h, uint64_t seq) {
    Tick<F> t;
    memset(&t, 0, sizeof(t));
    t.price     = FPN_FromDouble<F>(h->price);
    t.volume    = FPN_FromDouble<F>(h->qty);
    t.timestamp = (uint64_t)h->timestamp_us;
    t.sequence  = seq;
    return t;
}

//======================================================================================================
// [RUN]
//======================================================================================================
// Sharded backtest entry point. Same signature as Backtest_Run for the
// dispatcher. Loads tick files, runs the per-core architecture against them,
// aggregates results.
//======================================================================================================
static inline void BacktestSharded_Run(BacktestResults *results,
                                        const BacktestRunConfig *run_cfg,
                                        volatile int *progress_pct,
                                        volatile int *cancel_flag,
                                        CandleAccumulator *candle_acc,
                                        TUISnapshot *out_snapshot = NULL) {
    (void)candle_acc;   // not yet wired in sharded path
    (void)out_snapshot; // not yet wired in sharded path

    // Reset results — preserve dynamic allocations like the legacy path does
    {
        float *fm = results->feature_matrix;
        float *lb = results->labels;
        int   *ti = results->sample_tick_indices;
        double *sp = results->sample_prices;
        int   *sr = results->sample_regimes;
        int cap = results->sample_capacity;
        memset(results, 0, sizeof(*results));
        results->feature_matrix = fm;
        results->labels = lb;
        results->sample_tick_indices = ti;
        results->sample_prices = sp;
        results->sample_regimes = sr;
        results->sample_capacity = cap;
    }

    // Load config (or use override)
    ControllerConfig<BACKTEST_FP> cfg;
    if (run_cfg->use_config_override) {
        cfg = run_cfg->config_override;
    } else {
        cfg = ControllerConfig_Load<BACKTEST_FP>(run_cfg->config_path);
    }
    cfg.slow_path_max_secs = 999999;
    results->config_used = cfg;

    fprintf(stderr, "[backtest sharded] mode=sharded cores=%u strategy=%d\n",
            (unsigned)cfg.num_execution_cores, cfg.default_strategy);

    // Strategy gate: only SimpleDip is ported in this build. Bail with a
    // clear error if the user picked something else.
    int requested_strategy = cfg.default_strategy;
    if (requested_strategy < 0) requested_strategy = STRATEGY_SIMPLE_DIP;
    if (requested_strategy != STRATEGY_SIMPLE_DIP) {
        fprintf(stderr, "[backtest sharded] ERROR: only SimpleDip (strategy 2) is currently "
                        "ported to the sharded path. requested=%d\n", requested_strategy);
        return;
    }

    //----------------------------------------------------------------------
    // Set up the per-core engine
    //----------------------------------------------------------------------
    // Phase 03 chunk 1B: construct OMS first, then wire EventLoopState to it.
    ExchangeAdapter<BACKTEST_FP> empty_adapter{};
    OrderManagerState<BACKTEST_FP> oms;
    OrderManager_Init(&oms, empty_adapter, 0, cfg.starting_balance, cfg.fee_rate);
    EventLoopState<BACKTEST_FP> state;
    EventLoopState_Init(&state, &oms);

    // Configure kill switch from the existing config fields. The drawdown
    // field in cfg is already a fraction (parsed via CFG_PARSE_PCT) so it
    // matches what _ConfigureKillSwitch expects.
    if (cfg.kill_switch_enabled) {
        EventLoopState_ConfigureKillSwitch(&state,
            FPN_Zero<BACKTEST_FP>(),  // no hard balance floor in legacy mode either
            cfg.kill_switch_drawdown_pct);
    }

    int num_cores = (int)cfg.num_execution_cores;
    if (num_cores < 1) num_cores = 1;
    if (num_cores > MAX_EXECUTION_CORES) num_cores = MAX_EXECUTION_CORES;

    // Allocate per-core resources on the stack — small fixed array, no malloc
    static SPSCRing<Tick<BACKTEST_FP>, EXECUTION_CORE_TICK_RING_SIZE> tick_rings[MAX_EXECUTION_CORES];
    static ExecutionCore<BACKTEST_FP> cores[MAX_EXECUTION_CORES];

    // Risk slice per core: starting balance / num_cores. Each core sizes its
    // entries against this allocation, so the aggregate exposure matches what
    // a single-controller engine would do with risk_pct.
    double total_balance = FPN_ToDouble(cfg.starting_balance);
    double per_core_balance = (total_balance * FPN_ToDouble(cfg.risk_pct)) / (double)num_cores;
    if (per_core_balance < 1.0) per_core_balance = 1.0;

    for (int i = 0; i < num_cores; ++i) {
        SPSCRing_Init(&tick_rings[i]);
        ExecutionCore_Init(&cores[i], (uint16_t)i, &tick_rings[i]);
        // intended TP/SL/qty get filled in by the slow-path strategy rebuild;
        // initial values from config are placeholders.
        FPN<BACKTEST_FP> init_tp = FPN_FromDouble<BACKTEST_FP>(total_balance);  // never hit
        FPN<BACKTEST_FP> init_sl = FPN_Zero<BACKTEST_FP>();                     // never hit
        FPN<BACKTEST_FP> init_qty = FPN_Zero<BACKTEST_FP>();                    // no entries until rebuild
        EventLoopState_RegisterCore(&state, &cores[i], init_tp, init_sl, init_qty);
        EventLoopState_SetCoreStrategy(&state, i,
            (uint8_t)STRATEGY_SIMPLE_DIP,
            FPN_FromDouble<BACKTEST_FP>(per_core_balance));
        ExecutionCore_SetPermission(&cores[i], 1);
    }

    // RollingStats lives on the stack here so the slow-path parameter rebuild
    // has fresh data. Single-threaded backtest, no concurrency concerns.
    // Both windows (W=128 short, W=512 long) are maintained because production
    // SimpleDip uses MAX(short_max, long_max) for the dip reference high.
    static RollingStats<BACKTEST_FP, 128> rolling = RollingStats_Init<BACKTEST_FP, 128>();
    static RollingStats<BACKTEST_FP, 512> rolling_long = RollingStats_Init<BACKTEST_FP, 512>();
    rolling = RollingStats_Init<BACKTEST_FP, 128>();      // explicit reset each run
    rolling_long = RollingStats_Init<BACKTEST_FP, 512>(); // explicit reset each run

    ShardedBacktestDriver<BACKTEST_FP, 128, 512> drv;
    ShardedBacktestDriver_Init(&drv, &state, &rolling, &cfg, (int)cfg.poll_interval, &rolling_long);

    //----------------------------------------------------------------------
    // Replay loop
    //----------------------------------------------------------------------
    struct timeval t_start, t_end;
    gettimeofday(&t_start, NULL);

    int total_processed = 0;
    int total_ticks_all_files = 0;

    // First pass: count ticks per file for the progress bar
    int *file_tick_counts = (int *)calloc(run_cfg->num_data_files, sizeof(int));
    int max_ticks_in_file = 0;
    for (int f = 0; f < run_cfg->num_data_files; f++) {
        FILE *fp = fopen(run_cfg->data_paths[f], "r");
        if (!fp) continue;
        int lines = 0;
        char buf[512];
        while (fgets(buf, sizeof(buf), fp)) lines++;
        fclose(fp);
        file_tick_counts[f] = lines - 1;
        total_ticks_all_files += file_tick_counts[f];
        if (file_tick_counts[f] > max_ticks_in_file) max_ticks_in_file = file_tick_counts[f];
    }
    if (total_ticks_all_files <= 0) total_ticks_all_files = 1;

    int max_ticks = max_ticks_in_file + 1024;
    if (max_ticks < 1024) max_ticks = 1024;
    HistoricalTick *ticks = (HistoricalTick *)malloc(max_ticks * sizeof(HistoricalTick));
    if (!ticks) {
        fprintf(stderr, "[backtest sharded] failed to allocate tick buffer\n");
        free(file_tick_counts);
        return;
    }

    // Track per-trade outcomes for win/loss/avg stats. We sample the
    // realized_pnl after each exit to compute the per-trade delta.
    double last_realized_pnl = 0.0;
    double cumulative_wins = 0.0;
    double cumulative_losses = 0.0;
    int win_count = 0;
    int loss_count = 0;
    double peak_equity = total_balance;
    double max_drawdown = 0.0;

    // DEBUG: track price range and dump threshold once after first slow path
    double price_lo = 1e18, price_hi = 0.0;
    int debug_dumped = 0;

    for (int f = 0; f < run_cfg->num_data_files; f++) {
        int count = 0;
        if (!BacktestData_Load(ticks, &count, max_ticks, run_cfg->data_paths[f]))
            continue;

        for (int i = 0; i < count; i++) {
            if (*cancel_flag) goto done;

            Tick<BACKTEST_FP> t = SharedBacktest_FromHistorical<BACKTEST_FP>(&ticks[i], (uint64_t)total_processed);

            // DEBUG: track price range
            if (ticks[i].price < price_lo) price_lo = ticks[i].price;
            if (ticks[i].price > price_hi) price_hi = ticks[i].price;

            // Step the per-core engine through this tick. Internally:
            //   1. RollingStats_Push so the slow path has fresh data
            //   2. Fan out to every core (1..num_cores)
            //   3. DrainEvents (process any entries / exits)
            //   4. On cadence: rebuild parameters, push, evaluate kill switch
            ShardedBacktest_RunTick(&drv, t, total_processed);

            // DEBUG: dump pending params after the first slow-path rebuild
            if (!debug_dumped && drv.slow_path_runs > 0) {
                debug_dumped = 1;
                fprintf(stderr, "[backtest sharded DEBUG] after first slow path (tick %d):\n",
                        total_processed);
                fprintf(stderr, "  price range so far : %.4f .. %.4f\n", price_lo, price_hi);
                fprintf(stderr, "  rolling.price_max  : %.4f\n", FPN_ToDouble(rolling.price_max));
                fprintf(stderr, "  rolling.volume_avg : %.4f\n", FPN_ToDouble(rolling.volume_avg));
                fprintf(stderr, "  rolling_long.max   : %.4f\n", FPN_ToDouble(rolling_long.price_max));
                fprintf(stderr, "  core[0] pending bg_threshold : %.4f\n",
                        FPN_ToDouble(state.cores[0].pending_params.bg_price_threshold));
                fprintf(stderr, "  core[0] pending tp_price     : %.4f\n",
                        FPN_ToDouble(state.cores[0].pending_params.sg_take_profit_price));
                fprintf(stderr, "  core[0] pending sl_price     : %.4f\n",
                        FPN_ToDouble(state.cores[0].pending_params.sg_stop_loss_price));
                fprintf(stderr, "  core[0] pending trade_size   : %.8f\n",
                        FPN_ToDouble(state.cores[0].pending_params.trade_size));
                fprintf(stderr, "  core[0] pending strategy_id  : %u\n",
                        (unsigned)state.cores[0].pending_params.strategy_id);
                fprintf(stderr, "  core[0] pending flags        : 0x%02x\n",
                        (unsigned)state.cores[0].pending_params.flags);
                fprintf(stderr, "  core[0] permission           : %u\n",
                        (unsigned)__atomic_load_n(&cores[0].permission, __ATOMIC_ACQUIRE));
            }

            // After the drain, check if any new exits happened by comparing
            // realized_pnl. If it changed, classify as win/loss and bump the
            // equity curve.
            double current_realized = FPN_ToDouble(state.oms->realized_pnl);
            if (current_realized != last_realized_pnl) {
                double trade_pnl = current_realized - last_realized_pnl;
                if (trade_pnl > 0.0) {
                    cumulative_wins += trade_pnl;
                    win_count++;
                } else if (trade_pnl < 0.0) {
                    cumulative_losses += (-trade_pnl);
                    loss_count++;
                }
                last_realized_pnl = current_realized;

                // Equity curve sample (one per completed trade)
                if (results->equity_count < BACKTEST_MAX_EQUITY) {
                    double bal = FPN_ToDouble(state.oms->balance);
                    results->equity_curve[results->equity_count] = bal;
                    results->equity_count++;
                }
            }

            // Track peak equity + max drawdown using EventLoopAggregates
            // (no current price snapshot needed here, balance is enough)
            double cur_equity = FPN_ToDouble(state.oms->balance);
            if (cur_equity > peak_equity) peak_equity = cur_equity;
            double dd = peak_equity - cur_equity;
            if (dd > max_drawdown) max_drawdown = dd;

            total_processed++;
            if ((total_processed & 0x3FFF) == 0) {
                *progress_pct = (int)(100.0 * total_processed / total_ticks_all_files);
            }
        }
    }

done:
    *progress_pct = 100;

    // Final drain to flush anything still in flight from the last tick
    EventLoop_DrainEvents(&state);

    gettimeofday(&t_end, NULL);
    double elapsed = (t_end.tv_sec - t_start.tv_sec) * 1000.0
                   + (t_end.tv_usec - t_start.tv_usec) / 1000.0;

    //----------------------------------------------------------------------
    // Populate BacktestResults stats
    //----------------------------------------------------------------------
    BacktestStats *stats = &results->stats;
    memset(stats, 0, sizeof(*stats));

    double final_balance = FPN_ToDouble(state.oms->balance);
    double final_pnl = FPN_ToDouble(state.oms->realized_pnl);

    stats->total_pnl = final_pnl;
    stats->total_trades = (uint32_t)(state.total_entries + state.total_exits) / 2;
    if (stats->total_trades == 0 && state.total_exits > 0) {
        stats->total_trades = (uint32_t)state.total_exits;
    }
    stats->ticks_processed = (uint64_t)total_processed;
    stats->wins = (uint32_t)win_count;
    stats->losses = (uint32_t)loss_count;
    int decided = win_count + loss_count;
    if (decided > 0) {
        stats->win_rate = (double)win_count / (double)decided;
    }
    if (win_count > 0) stats->avg_win = cumulative_wins / win_count;
    if (loss_count > 0) stats->avg_loss = cumulative_losses / loss_count;
    if (cumulative_losses > 0.0) {
        stats->profit_factor = cumulative_wins / cumulative_losses;
    }
    stats->max_drawdown = max_drawdown;
    if (peak_equity > 0.0) {
        stats->max_drawdown_pct = (max_drawdown / peak_equity) * 100.0;
    }
    double starting_bal = FPN_ToDouble(cfg.starting_balance);
    if (starting_bal > 0.0) {
        stats->return_pct = ((final_balance - starting_bal) / starting_bal) * 100.0;
    }
    stats->elapsed_ms = elapsed;

    free(ticks);
    free(file_tick_counts);

    fprintf(stderr, "[backtest sharded] completed: %d ticks in %.1fms, %u trades (%u/%u W/L), P&L $%.2f\n",
            total_processed, elapsed,
            stats->total_trades, stats->wins, stats->losses,
            stats->total_pnl);
    fprintf(stderr, "[backtest sharded DEBUG] state.total_entries=%lu state.total_exits=%lu\n",
            (unsigned long)state.total_entries, (unsigned long)state.total_exits);
}

}  // namespace tt
