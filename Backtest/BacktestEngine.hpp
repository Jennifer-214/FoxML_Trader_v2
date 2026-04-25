// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [BACKTEST ENGINE]
//======================================================================================================
// replay loop for historical tick data through the identical engine code path.
// produces the exact same trade results as live — same BuyGate, ExitGate,
// PortfolioController_Tick calls in the same order.
//
// mirrors main.cpp:363-547 — if main.cpp's tick loop changes, update this.
//======================================================================================================
#ifndef BACKTEST_ENGINE_HPP
#define BACKTEST_ENGINE_HPP

#include "../CoreFrameworks/PortfolioController.hpp"
#include "../CoreFrameworks/OrderGates.hpp"
#include "../DataStream/TradeLog.hpp"
#include "../ML_Headers/ModelInference.hpp"
#include "../GUI/CandleAccumulator.hpp"
#include "LabelFunctions.hpp"
#include "BacktestSnapshot.hpp"
#include "ValidationSplit.hpp"
#include "OverfitDetection.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/time.h>
#include <sys/stat.h>

// FPN width — must match the engine build
#ifndef BACKTEST_FP
#define BACKTEST_FP 64
#endif

// HistoricalTick is defined in LabelFunctions.hpp (single definition point)

//======================================================================================================
// [DATA LOADER]
//======================================================================================================
// loads Binance aggTrades CSV format:
//   id,price,qty,first_id,last_id,timestamp,is_buyer_maker
// or TickRecorder format:
//   timestamp_us,price,quantity,is_buyer_maker
//======================================================================================================
static inline int BacktestData_DetectFormat(const char *header) {
    // TickRecorder format starts with "timestamp_us"
    if (strncmp(header, "timestamp_us", 12) == 0) return 1;
    // Binance aggTrades has 7 fields starting with numeric ID
    return 0;
}

static inline int BacktestData_Load(HistoricalTick *ticks, int *count, int max_ticks,
                                     const char *csv_path) {
    FILE *f = fopen(csv_path, "r");
    if (!f) {
        fprintf(stderr, "[backtest] failed to open %s\n", csv_path);
        return 0;
    }

    char line[512];
    // read header to detect format
    if (!fgets(line, sizeof(line), f)) { fclose(f); return 0; }
    int format = BacktestData_DetectFormat(line);

    *count = 0;
    while (fgets(line, sizeof(line), f) && *count < max_ticks) {
        HistoricalTick *t = &ticks[*count];

        if (format == 1) {
            // TickRecorder: timestamp_us,price,quantity,is_buyer_maker
            char *p = line;
            t->timestamp_us = strtoll(p, &p, 10); if (*p == ',') p++;
            t->price = strtod(p, &p); if (*p == ',') p++;
            t->qty = strtod(p, &p); if (*p == ',') p++;
            t->is_buyer_maker = (int)strtol(p, &p, 10);
        } else {
            // Binance aggTrades: id,price,qty,first_id,last_id,timestamp,is_buyer_maker
            char *p = line;
            strtoll(p, &p, 10); if (*p == ',') p++;                     // skip id
            t->price = strtod(p, &p); if (*p == ',') p++;
            t->qty = strtod(p, &p); if (*p == ',') p++;
            strtoll(p, &p, 10); if (*p == ',') p++;                     // skip first_id
            strtoll(p, &p, 10); if (*p == ',') p++;                     // skip last_id
            t->timestamp_us = strtoll(p, &p, 10); if (*p == ',') p++;
            // is_buyer_maker can be "true"/"false" or 1/0
            if (*p == 't' || *p == 'T') t->is_buyer_maker = 1;
            else if (*p == 'f' || *p == 'F') t->is_buyer_maker = 0;
            else t->is_buyer_maker = (int)strtol(p, &p, 10);
        }

        if (t->price > 0.0 && t->qty > 0.0)
            (*count)++;
    }

    fclose(f);
    fprintf(stderr, "[backtest] loaded %d ticks from %s\n", *count, csv_path);
    return 1;
}

//======================================================================================================
// [RUN CONFIG]
//======================================================================================================
struct BacktestRunConfig {
    char data_paths[MAX_DATA_FILES][256];
    int num_data_files;
    char config_path[256];
    ControllerConfig<BACKTEST_FP> config_override;
    int use_config_override;
    int collect_features;
    int label_type;         // LABEL_WIN_LOSS, LABEL_BARRIER, etc.
    double label_tp_pct;    // TP barrier for win/loss and barrier labels (e.g. 1.5 = 1.5%)
    double label_sl_pct;    // SL barrier (e.g. 1.0 = 1.0%)
    int label_forward_ticks; // forward window for forward_pnl label (e.g. 1000)
};

//======================================================================================================
// [STATS]
//======================================================================================================
struct BacktestStats {
    double sharpe_ratio;
    double profit_factor;
    double expectancy;
    double max_drawdown;
    double max_drawdown_pct;
    double win_rate;
    double total_pnl;
    double total_fees;
    double return_pct;
    uint32_t total_trades;
    uint32_t wins, losses;
    double avg_win, avg_loss;
    double avg_hold_ticks;
    double elapsed_ms;
    uint64_t ticks_processed;
};

//======================================================================================================
// [RESULTS]
//======================================================================================================
#define BACKTEST_EQUITY_INIT   8192    // initial equity_curve allocation, grows 2x as needed
#define BACKTEST_SAMPLES_INIT  500000  // initial sample buffer allocation, grows as needed
#define BACKTEST_TRADE_CSV     "logging/BACKTEST_order_history.csv"

struct BacktestResults {
    BacktestStats stats;
    // equity curve — recorded per completed trade. Long backtests at 50-200
    // trades/day across multi-year datasets exceed any small fixed cap, so
    // this is dynamic. Stats (Sharpe / max DD / return) compute from this
    // array — silent truncation = wrong stats. NEVER cap this silently.
    double *equity_curve;
    int equity_count;
    int equity_capacity;
    char trade_csv_path[256];
    // ML features (dynamically allocated, grows as needed)
    float *feature_matrix;   // [sample_capacity * MODEL_MAX_FEATURES]
    float *labels;           // [sample_capacity]
    int   *sample_tick_indices; // tick index of each sample (for label computation)
    double *sample_prices;     // price at each sample point
    int   *sample_regimes;     // regime at each sample point
    int sample_count;
    int sample_capacity;       // current allocation size
    // config used (for comparison)
    ControllerConfig<BACKTEST_FP> config_used;
};

static inline void BacktestResults_Init(BacktestResults *r) {
    memset(r, 0, sizeof(*r));
    r->sample_capacity = BACKTEST_SAMPLES_INIT;
    r->feature_matrix      = (float *)malloc(r->sample_capacity * MODEL_MAX_FEATURES * sizeof(float));
    r->labels              = (float *)malloc(r->sample_capacity * sizeof(float));
    r->sample_tick_indices = (int *)malloc(r->sample_capacity * sizeof(int));
    r->sample_prices       = (double *)malloc(r->sample_capacity * sizeof(double));
    r->sample_regimes      = (int *)malloc(r->sample_capacity * sizeof(int));
    r->equity_capacity = BACKTEST_EQUITY_INIT;
    r->equity_curve    = (double *)malloc(r->equity_capacity * sizeof(double));
}

static inline void BacktestResults_Free(BacktestResults *r) {
    free(r->feature_matrix);
    free(r->labels);
    free(r->sample_tick_indices);
    free(r->sample_prices);
    free(r->sample_regimes);
    free(r->equity_curve);
    r->feature_matrix = NULL;
    r->labels = NULL;
    r->sample_tick_indices = NULL;
    r->sample_prices = NULL;
    r->sample_regimes = NULL;
    r->equity_curve = NULL;
    r->sample_count = 0;
    r->sample_capacity = 0;
    r->equity_count = 0;
    r->equity_capacity = 0;
}

// Reset counts/scalars while PRESERVING heap allocations + their capacities.
// Hand-rolled save/restore blocks in the run paths missed equity_curve when
// it became dynamic (ff9ac48), which caused the first trade exit to call
// EnsureEquityCapacity with capacity=0, hitting `while (0 < needed) cap *= 2`
// — infinite spin at 100% CPU on the worker thread.
//
// When extending BacktestResults with a new dynamic field: update _Init,
// _Free, AND this _Reset. The Ensure*Capacity helpers are also defended
// against zero capacity (defense-in-depth) but this is the load-bearing fix.
static inline void BacktestResults_Reset(BacktestResults *r) {
    float *fm  = r->feature_matrix;
    float *lb  = r->labels;
    int   *ti  = r->sample_tick_indices;
    double *sp = r->sample_prices;
    int   *sr  = r->sample_regimes;
    int    sample_cap = r->sample_capacity;
    double *ec = r->equity_curve;
    int    eq_cap = r->equity_capacity;

    memset(r, 0, sizeof(*r));

    r->feature_matrix      = fm;
    r->labels              = lb;
    r->sample_tick_indices = ti;
    r->sample_prices       = sp;
    r->sample_regimes      = sr;
    r->sample_capacity     = sample_cap;
    r->equity_curve        = ec;
    r->equity_capacity     = eq_cap;
}

// grow sample buffers by 2x when full
static inline int BacktestResults_EnsureCapacity(BacktestResults *r, int needed) {
    if (needed <= r->sample_capacity) return 1;
    // floor: if capacity leaked to 0 (forgot to preserve in a reset path),
    // seed from BACKTEST_SAMPLES_INIT instead of spinning on `0 *= 2`.
    int new_cap = r->sample_capacity > 0 ? r->sample_capacity * 2 : BACKTEST_SAMPLES_INIT;
    while (new_cap < needed) new_cap *= 2;
    float *fm  = (float *)realloc(r->feature_matrix, new_cap * MODEL_MAX_FEATURES * sizeof(float));
    float *lb  = (float *)realloc(r->labels, new_cap * sizeof(float));
    int   *ti  = (int *)realloc(r->sample_tick_indices, new_cap * sizeof(int));
    double *sp = (double *)realloc(r->sample_prices, new_cap * sizeof(double));
    int   *sr  = (int *)realloc(r->sample_regimes, new_cap * sizeof(int));
    if (!fm || !lb || !ti || !sp || !sr) {
        fprintf(stderr, "[backtest] failed to grow sample buffers to %d (%.0f MB)\n",
                new_cap, new_cap * (MODEL_MAX_FEATURES * 4.0 + 4 + 4 + 8 + 4) / 1e6);
        return 0; // keep old pointers, caller should stop collecting
    }
    r->feature_matrix = fm;
    r->labels = lb;
    r->sample_tick_indices = ti;
    r->sample_prices = sp;
    r->sample_regimes = sr;
    r->sample_capacity = new_cap;
    return 1;
}

// grow equity_curve by 2x when full. CRITICAL — stats compute from this
// array, so silent truncation produces wrong Sharpe / max DD / return.
static inline int BacktestResults_EnsureEquityCapacity(BacktestResults *r, int needed) {
    if (needed <= r->equity_capacity) return 1;
    // floor: see BacktestResults_EnsureCapacity — same zero-capacity spin guard.
    int new_cap = r->equity_capacity > 0 ? r->equity_capacity * 2 : BACKTEST_EQUITY_INIT;
    while (new_cap < needed) new_cap *= 2;
    double *ec = (double *)realloc(r->equity_curve, new_cap * sizeof(double));
    if (!ec) {
        fprintf(stderr, "[backtest] failed to grow equity_curve to %d (%.0f MB) — stats will be partial\n",
                new_cap, new_cap * 8.0 / 1e6);
        return 0;
    }
    r->equity_curve = ec;
    r->equity_capacity = new_cap;
    return 1;
}

//======================================================================================================
// [STATS COMPUTE]
//======================================================================================================
static inline void BacktestStats_Compute(BacktestStats *stats,
                                          const PortfolioController<BACKTEST_FP> *ctrl,
                                          double starting_balance,
                                          double elapsed_ms) {
    stats->total_trades = ctrl->total_buys;
    stats->wins = ctrl->wins;
    stats->losses = ctrl->losses;
    stats->total_pnl = FPN_ToDouble(ctrl->realized_pnl);
    stats->total_fees = FPN_ToDouble(ctrl->total_fees);
    stats->ticks_processed = ctrl->total_ticks;
    stats->elapsed_ms = elapsed_ms;

    // win rate
    stats->win_rate = (stats->total_trades > 0)
        ? (double)stats->wins / stats->total_trades * 100.0
        : 0.0;

    // averages
    double gw = FPN_ToDouble(ctrl->gross_wins);
    double gl = FPN_ToDouble(ctrl->gross_losses);
    stats->avg_win  = (stats->wins > 0)   ? gw / stats->wins   : 0.0;
    stats->avg_loss = (stats->losses > 0) ? gl / stats->losses : 0.0;

    // profit factor
    stats->profit_factor = (gl > 0.0001) ? gw / gl : 0.0;

    // expectancy: (win_rate * avg_win) - (loss_rate * avg_loss)
    double wr = stats->wins > 0 ? (double)stats->wins / stats->total_trades : 0.0;
    double lr = 1.0 - wr;
    stats->expectancy = (wr * stats->avg_win) - (lr * fabs(stats->avg_loss));

    // return %
    stats->return_pct = (starting_balance > 0.0)
        ? stats->total_pnl / starting_balance * 100.0
        : 0.0;

    // avg hold ticks
    stats->avg_hold_ticks = (stats->total_trades > 0)
        ? (double)ctrl->total_hold_ticks / stats->total_trades
        : 0.0;

    // max drawdown — compute from equity curve in results (caller's job)
    // sharpe — needs equity curve data too
}

//======================================================================================================
// [MAX DRAWDOWN + SHARPE from equity curve]
//======================================================================================================
static inline void BacktestStats_ComputeFromEquity(BacktestStats *stats,
                                                    const double *equity, int count) {
    // max drawdown
    double peak = equity[0];
    double max_dd = 0.0;
    double max_dd_pct = 0.0;
    for (int i = 1; i < count; i++) {
        if (equity[i] > peak) peak = equity[i];
        double dd = peak - equity[i];
        if (dd > max_dd) max_dd = dd;
        double dd_pct = (peak > 0.0) ? dd / peak : 0.0;
        if (dd_pct > max_dd_pct) max_dd_pct = dd_pct;
    }
    stats->max_drawdown = max_dd;
    stats->max_drawdown_pct = max_dd_pct * 100.0;

    // sharpe ratio (annualized, assuming ~365 trading days)
    if (count < 2) { stats->sharpe_ratio = 0.0; return; }
    // compute returns between equity points
    double sum_r = 0.0, sum_r2 = 0.0;
    int n = count - 1;
    for (int i = 1; i < count; i++) {
        double r = (equity[i - 1] != 0.0) ? (equity[i] - equity[i - 1]) / fabs(equity[i - 1]) : 0.0;
        sum_r += r;
        sum_r2 += r * r;
    }
    double mean = sum_r / n;
    double var = (sum_r2 / n) - (mean * mean);
    double stddev = (var > 0.0) ? sqrt(var) : 0.0;
    // annualize: assume each equity point is ~1 trade, scale by sqrt(trades/year)
    // rough: if 30 trades/day, 365 days = 10950 trades/year
    stats->sharpe_ratio = (stddev > 1e-12) ? mean / stddev * sqrt((double)n) : 0.0;
}

// Forward decl for the sharded backtest path. The actual implementation lives
// in Backtest/BacktestSharded.hpp which is included AFTER this declaration so
// the dispatcher can call it. Both functions share the BacktestRunConfig +
// BacktestResults shape so the suite GUI doesn't care which path produced the
// results.
namespace tt {
static inline void BacktestSharded_Run(BacktestResults *results,
                                        const BacktestRunConfig *run_cfg,
                                        volatile int *progress_pct,
                                        volatile int *cancel_flag,
                                        CandleAccumulator *candle_acc,
                                        TUISnapshot *out_snapshot);
}

//======================================================================================================
// [RUN]
//======================================================================================================
// the core replay loop — mirrors main.cpp:363-547
//
// Phase 13 dispatch: peek at engine_mode at the top. If sharded, route to the
// per-core path in BacktestSharded.hpp and return. Otherwise fall through to
// the legacy single-threaded code below, which is unchanged.
//======================================================================================================
static inline void Backtest_Run(BacktestResults *results, const BacktestRunConfig *run_cfg,
                                 volatile int *progress_pct, volatile int *cancel_flag,
                                 CandleAccumulator *candle_acc,
                                 TUISnapshot *out_snapshot = NULL) {
    // Phase 13 dispatch: peek at engine_mode WITHOUT touching results yet,
    // so the sharded path's own reset is honored.
    {
        ControllerConfig<BACKTEST_FP> peek;
        if (run_cfg->use_config_override) {
            peek = run_cfg->config_override;
        } else {
            peek = ControllerConfig_Load<BACKTEST_FP>(run_cfg->config_path);
        }
        if (peek.engine_mode == ENGINE_MODE_SHARDED) {
            tt::BacktestSharded_Run(results, run_cfg, progress_pct, cancel_flag,
                                     candle_acc, out_snapshot);
            return;
        }
    }

    // reset results — preserve dynamic allocations, just reset counts
    BacktestResults_Reset(results);

    // load config
    ControllerConfig<BACKTEST_FP> cfg;
    if (run_cfg->use_config_override) {
        cfg = run_cfg->config_override;
    } else {
        cfg = ControllerConfig_Load<BACKTEST_FP>(run_cfg->config_path);
    }
    // train-serve parity: disable wall-clock time floor so backtest is purely
    // tick-driven.  live engine uses slow_path_max_secs to trigger RollingStats_Push
    // during sparse markets — backtest must NOT do this or features diverge.
    cfg.slow_path_max_secs = 999999;

    results->config_used = cfg;

    // init controller (same as main.cpp:159-161)
    PortfolioController<BACKTEST_FP> ctrl;
    ctrl.rolling_long = NULL;
    PortfolioController_Init(&ctrl, cfg);
    ctrl.sim_time = 0;          // will be set from first tick timestamp
    ctrl.last_slow_time = 0;    // triggers time seed on first tick

    // gate reason diagnostics — count slow-path cycles per gate reason
    int gate_counts[NUM_GATE_REASONS] = {};
    int total_slow_cycles = 0;

    // init order pool (same as main.cpp:174)
    OrderPool<BACKTEST_FP> pool;
    OrderPool_init(&pool, 64);

    // init trade log — write to backtest output
    mkdir("logging", 0755);
    // truncate trade log for each run (live engine appends, but backtest should start fresh)
    remove(BACKTEST_TRADE_CSV);
    TradeLog log;
    snprintf(results->trade_csv_path, sizeof(results->trade_csv_path),
             BACKTEST_TRADE_CSV);
    TradeLog_Init(&log, "BACKTEST");

    // load all data files
    struct timeval t_start, t_end;
    gettimeofday(&t_start, NULL);

    int total_processed = 0;
    int total_ticks_all_files = 0;

    // first pass: count total ticks per file for progress bar + allocation
    int *file_tick_counts = (int *)calloc(run_cfg->num_data_files, sizeof(int));
    int max_ticks_in_file = 0;
    for (int f = 0; f < run_cfg->num_data_files; f++) {
        FILE *fp = fopen(run_cfg->data_paths[f], "r");
        if (!fp) continue;
        int lines = 0;
        char buf[512];
        while (fgets(buf, sizeof(buf), fp)) lines++;
        fclose(fp);
        file_tick_counts[f] = lines - 1; // subtract header
        total_ticks_all_files += file_tick_counts[f];
        if (file_tick_counts[f] > max_ticks_in_file)
            max_ticks_in_file = file_tick_counts[f];
    }
    if (total_ticks_all_files <= 0) total_ticks_all_files = 1; // avoid div by zero

    // allocate tick buffer sized to the largest file (no silent truncation)
    int max_ticks = max_ticks_in_file + 1024; // small padding
    if (max_ticks < 1024) max_ticks = 1024;
    HistoricalTick *ticks = (HistoricalTick *)malloc(max_ticks * sizeof(HistoricalTick));
    if (!ticks) {
        fprintf(stderr, "[backtest] failed to allocate tick buffer (%d ticks, %.0f MB)\n",
                max_ticks, max_ticks * sizeof(HistoricalTick) / 1e6);
        free(file_tick_counts);
        return;
    }
    fprintf(stderr, "[backtest] tick buffer: %d ticks (%.0f MB) for %d files\n",
            max_ticks, max_ticks * sizeof(HistoricalTick) / 1e6, run_cfg->num_data_files);

    double price_d_last = 0.0; // track last price for snapshot
    int64_t last_day_ms = 0;  // day boundary detection (ms timestamp of last midnight)

    // replay each file
    for (int f = 0; f < run_cfg->num_data_files; f++) {
        int count = 0;
        if (!BacktestData_Load(ticks, &count, max_ticks, run_cfg->data_paths[f]))
            continue;

        //==========================================================================================
        // REPLAY LOOP — mirrors main.cpp:363-547
        // if main.cpp's tick processing order changes, update this to match
        //==========================================================================================
        for (int i = 0; i < count; i++) {
            if (*cancel_flag) goto done;

            // day boundary: mirrors live engine 24h reconnect
            // force-close all positions, reset session state, clear kill switch
            // timestamp_us is actually microseconds — convert to day boundary in μs
            int64_t tick_day = (ticks[i].timestamp_us / 86400000000LL) * 86400000000LL;
            if (tick_day != last_day_ms && last_day_ms != 0) {
                time_t day_ts = (time_t)(tick_day / 1000000);
                struct tm *dt = gmtime(&day_ts);
                fprintf(stderr, "[backtest] day boundary: %04d-%02d-%02d | positions=%d kill=%d bal=%.2f\n",
                    dt->tm_year+1900, dt->tm_mon+1, dt->tm_mday,
                    __builtin_popcount(ctrl.portfolio.active_bitmap),
                    ctrl.kill_switch_active, FPN_ToDouble(ctrl.balance));
                FPN<BACKTEST_FP> last_price_fpn = FPN_FromDouble<BACKTEST_FP>(price_d_last);

                // force-close all active positions at last known price
                uint16_t bmp = ctrl.portfolio.active_bitmap;
                while (bmp) {
                    int idx = __builtin_ctz(bmp);
                    if (ctrl.exit_buf.count < 16) {
                        ExitRecord<BACKTEST_FP> *rec = &ctrl.exit_buf.records[ctrl.exit_buf.count];
                        rec->position_index = idx;
                        rec->exit_price     = last_price_fpn;
                        rec->tick           = ctrl.total_ticks;
                        rec->entry_price    = ctrl.portfolio.positions[idx].entry_price;
                        rec->quantity       = ctrl.portfolio.positions[idx].quantity;
                        rec->entry_fee      = ctrl.portfolio.positions[idx].entry_fee;
                        rec->pair_index     = ctrl.portfolio.positions[idx].pair_index;
                        // use TP/SL reason based on P&L so wins/losses count correctly
                        int profitable = FPN_GreaterThan(last_price_fpn, rec->entry_price);
                        rec->reason = profitable ? 0 : 1; // 0=TP, 1=SL
                        ctrl.exit_buf.count++;
                        ctrl.portfolio.active_bitmap &= ~(1 << idx);
                    }
                    bmp &= bmp - 1;
                }
                // drain exits — P&L accounting for forced closes
                if (ctrl.exit_buf.count > 0) {
                    PortfolioController_DrainExits(&ctrl);
                    TradeLogBuffer_Drain(&ctrl.trade_buf, &log);
                    ExitBuffer_Clear(&ctrl.exit_buf);
                }

                // reset session state
                FPN<BACKTEST_FP> equity = ctrl.balance; // no positions after force-close
                ctrl.session_start_equity = equity;
                ctrl.peak_equity = equity;
                ctrl.daily_realized_pnl = FPN_Zero<BACKTEST_FP>();
                ctrl.sl_cooldown_counter = 0;
                ctrl.idle_cycles = 0;

                // clear kill switch + all halts
                ctrl.kill_switch_active = 0;
                ctrl.kill_reason = 0;
                ctrl.buying_halted = 0;
                ctrl.halt_reason = 0;
                ctrl.kill_recovery_counter = 0;

                ctrl.session_high = price_d_last;
                ctrl.session_low = price_d_last;
            }
            last_day_ms = tick_day;

            // build DataStream (same struct as live)
            DataStream<BACKTEST_FP> tick;
            tick.price = FPN_FromDouble<BACKTEST_FP>(ticks[i].price);
            tick.volume = FPN_FromDouble<BACKTEST_FP>(ticks[i].qty);
            tick.price_d = ticks[i].price;
            tick.volume_d = ticks[i].qty;
            tick.is_buyer_maker = ticks[i].is_buyer_maker;
            price_d_last = ticks[i].price;

            // set simulated clock from historical timestamp
            // timestamp_us is actually microseconds in Binance aggTrades format
            ctrl.sim_time = (time_t)(ticks[i].timestamp_us / 1000000);
            // seed time state on first tick of each file (avoids clock gaps between files)
            if (i == 0 || total_processed == 0) {
                ctrl.last_slow_time = (uint64_t)ctrl.sim_time;
                ctrl.regime.regime_start_time = ctrl.sim_time;
            }

            // exit gate on EVERY tick (same as main.cpp:381-392)
            if (ctrl.portfolio.active_bitmap)
                PositionExitGate(&ctrl.portfolio, tick.price, &ctrl.exit_buf, ctrl.total_ticks);

            // buy gate (same as main.cpp BuyGate call after burst drain)
            BuyGate(&ctrl.buy_conds, &tick, &pool);

            // core tick processing (same as main.cpp PortfolioController_Tick call)
            PortfolioController_Tick(&ctrl, &pool, tick.price, tick.volume,
                                     &log, tick.is_buyer_maker);

            // feed candle accumulator for chart (with historical timestamps)
            // throttle to every 100th tick — 1-min candles don't need every tick,
            // and unthrottled mutex contention (237M locks) freezes the GUI thread
            if (candle_acc && (total_processed % 100) == 0)
                CandleAccumulator_PushWithTime(candle_acc, tick.price_d, tick.volume_d,
                                               tick.is_buyer_maker,
                                               (double)(ticks[i].timestamp_us / 1000000));

            // track equity curve (on each trade completion).
            // grows dynamically — CAPPING THIS CONTAMINATES STATS (Sharpe/DD/return).
            if (ctrl.total_buys > 0 && (ctrl.wins + ctrl.losses) > (uint32_t)results->equity_count) {
                if (BacktestResults_EnsureEquityCapacity(results, results->equity_count + 1)) {
                    double bal = FPN_ToDouble(ctrl.balance);
                    double rpnl = FPN_ToDouble(ctrl.realized_pnl);
                    results->equity_curve[results->equity_count] = bal + rpnl;
                    results->equity_count++;
                }
            }

            // feature collection (on slow path only, when enabled)
            // labels are computed in a post-processing pass after replay
            // (they need forward-looking data that hasn't been seen yet)
            if (run_cfg->collect_features && ctrl.tick_count == 0 &&
                ctrl.state != CONTROLLER_WARMUP &&
                BacktestResults_EnsureCapacity(results, results->sample_count + 1)) {
                ModelFeatures_Pack<BACKTEST_FP>(
                    &results->feature_matrix[results->sample_count * MODEL_NUM_FEATURES],
                    &ctrl.last_signals,
                    &ctrl.rolling,
                    ctrl.rolling_long);
                results->sample_tick_indices[results->sample_count] = total_processed;
                results->sample_prices[results->sample_count] = ticks[i].price;
                results->sample_regimes[results->sample_count] = ctrl.regime.current_regime;
                results->labels[results->sample_count] = 0.0f; // filled in post-pass
                results->sample_count++;
            }

            // gate reason diagnostics (every slow-path cycle)
            if (ctrl.tick_count == 0 && ctrl.state != CONTROLLER_WARMUP) {
                int gr = ctrl.gate_reason;
                if (gr >= 0 && gr < NUM_GATE_REASONS) gate_counts[gr]++;
                total_slow_cycles++;
            }

            total_processed++;
            // update progress every 10K ticks (avoid atomic contention)
            if ((total_processed & 0x3FFF) == 0)
                *progress_pct = (int)(100.0 * total_processed / total_ticks_all_files);
        }
    }

done:
    *progress_pct = 100;

    gettimeofday(&t_end, NULL);
    double elapsed = (t_end.tv_sec - t_start.tv_sec) * 1000.0
                   + (t_end.tv_usec - t_start.tv_usec) / 1000.0;

    // force-drain remaining trade log entries
    TradeLogBuffer_Drain(&ctrl.trade_buf, &log);
    TradeLog_Close(&log);

    // compute stats
    double start_bal = FPN_ToDouble(cfg.starting_balance);
    BacktestStats_Compute(&results->stats, &ctrl, start_bal, elapsed);
    results->stats.ticks_processed = total_processed; // override: use actual file tick count

    // sharpe from equity curve, drawdown from controller (equity = balance + positions)
    if (results->equity_count > 1)
        BacktestStats_ComputeFromEquity(&results->stats, results->equity_curve, results->equity_count);
    // override equity-curve drawdown with controller's (tracks true equity, not just balance)
    results->stats.max_drawdown = FPN_ToDouble(ctrl.max_drawdown);
    double pe = FPN_ToDouble(ctrl.peak_equity);
    results->stats.max_drawdown_pct = (pe > 0.0)
        ? (results->stats.max_drawdown / pe) * 100.0 : 0.0;

    // populate TUISnapshot for dashboard panels (if requested)
    if (out_snapshot) {
        BacktestSnapshot_Copy<BACKTEST_FP>(out_snapshot, &ctrl, price_d_last, 0.0);
    }

    // post-processing: compute labels (needs forward-looking tick data)
    // reload last data file for label computation (labels look forward within this file)
    if (run_cfg->collect_features && results->sample_count > 0 && run_cfg->num_data_files > 0) {
        // reload all files concatenated for label computation (labels need forward-looking data)
        int label_count = 0;
        int label_max = total_processed + 1024; // size to actual data (no arbitrary cap)
        HistoricalTick *label_ticks = (HistoricalTick *)malloc((size_t)label_max * sizeof(HistoricalTick));
        fprintf(stderr, "[backtest] allocating label buffer: %d ticks (%.0f MB)\n",
                label_max, (double)label_max * sizeof(HistoricalTick) / 1e6);
        if (label_ticks) {
            for (int f = 0; f < run_cfg->num_data_files; f++) {
                if (label_count >= label_max) break;
                int before = label_count;
                BacktestData_Load(label_ticks + label_count, &label_count, label_max - label_count,
                                  run_cfg->data_paths[f]);
            }

            // get label function from table
            LabelFn label_fn = NULL;
            for (int l = 0; l < LABEL_COUNT; l++) {
                if (label_table[l].id == run_cfg->label_type) {
                    label_fn = label_table[l].fn;
                    break;
                }
            }
            if (!label_fn) label_fn = Label_WinLoss; // fallback

            double tp = run_cfg->label_tp_pct > 0 ? run_cfg->label_tp_pct : 1.5;
            double sl = run_cfg->label_sl_pct > 0 ? run_cfg->label_sl_pct : 1.0;
            int fwd = run_cfg->label_forward_ticks > 0 ? run_cfg->label_forward_ticks : 1000;

            for (int s = 0; s < results->sample_count; s++) {
                int tidx = results->sample_tick_indices[s];
                if (tidx >= label_count) tidx = label_count - 1;
                int extra = (run_cfg->label_type == LABEL_REGIME)
                    ? results->sample_regimes[s] : fwd;
                results->labels[s] = label_fn(label_ticks, tidx, label_count,
                                               results->sample_prices[s], tp, sl, extra);
            }
            free(label_ticks);

            fprintf(stderr, "[backtest] computed %d labels (type=%d, tp=%.1f%%, sl=%.1f%%)\n",
                    results->sample_count, run_cfg->label_type, tp, sl);
        }
    }

    // cleanup
    free(ticks);
    free(file_tick_counts);
    free(ctrl.rolling_long);

    fprintf(stderr, "[backtest] completed: %lu ticks in %.1fms, %u trades, P&L $%.2f\n",
            results->stats.ticks_processed, elapsed,
            results->stats.total_trades, results->stats.total_pnl);

    // gate reason breakdown — shows WHY the engine isn't trading
    // names come from GATE_REASON_TABLE[] (single source of truth in PortfolioController.hpp)
    if (total_slow_cycles > 0) {
        fprintf(stderr, "[backtest] gate reason breakdown (%d slow-path cycles):\n", total_slow_cycles);
        for (int g = 0; g < NUM_GATE_REASONS; g++) {
            if (gate_counts[g] > 0) {
                fprintf(stderr, "  %-12s %7d  (%5.1f%%)\n",
                        GATE_REASON_TABLE[g].name, gate_counts[g],
                        100.0 * gate_counts[g] / total_slow_cycles);
            }
        }
    }
}

//======================================================================================================
// [WALK-FORWARD VALIDATION]
//======================================================================================================
// uses purged temporal CV from ValidationSplit.hpp to train + evaluate per fold.
// this is the REAL performance metric — in-sample accuracy is meaningless for financial ML.
//
// flow:
//   1. caller runs Backtest_Run with collect_features=1 → features + labels in BacktestResults
//   2. Backtest_RunWalkForward operates on that feature matrix
//   3. per fold: train XGBoost on train slice → predict test slice → compute accuracy
//   4. OverfitDetection_Check per fold
//   5. report mean ± std validation accuracy across folds
//
// requires USE_XGBOOST to be defined (suite build has -DUSE_XGBOOST=ON).
// without XGBoost, returns immediately with num_folds=0.
//
// source: FoxML intelligent_trainer.py walk-forward loop pattern
//======================================================================================================

#define WALKFORWARD_MAX_FOLDS VALIDATION_MAX_FOLDS

struct WalkForwardFoldResult {
    float train_accuracy;
    float val_accuracy;
    int train_samples;
    int test_samples;
    OverfitReport overfit;
    float feature_importances[MODEL_MAX_FEATURES]; // from XGBoost (stability tracking hook)
    int valid;
};

struct WalkForwardResults {
    WalkForwardFoldResult folds[WALKFORWARD_MAX_FOLDS];
    PurgedSplit splits[WALKFORWARD_MAX_FOLDS];
    int num_folds;          // total folds requested
    int valid_folds;        // folds that had enough data
    float mean_val_accuracy;
    float std_val_accuracy;
    float mean_train_accuracy;
    int overfit_count;      // folds flagged as overfit
    double elapsed_ms;
    char fingerprint[65];   // SHA256 of config + data (empty if not computed)
};

// compute accuracy: fraction of predictions matching labels (for classification)
// threshold: prediction >= thresh → class 1, else class 0
// uses > 0.5f for truth so neutral (0.5) labels are never counted as positive
static inline float WalkForward_ComputeAccuracy(const float *predictions, const float *labels,
                                                  int count, float threshold) {
    if (count <= 0) return 0.0f;
    int correct = 0;
    for (int i = 0; i < count; i++) {
        int pred_class = (predictions[i] >= threshold) ? 1 : 0;
        int true_class = (labels[i] > 0.5f) ? 1 : 0;
        if (pred_class == true_class) correct++;
    }
    return (float)correct / count;
}

static inline void Backtest_RunWalkForward(WalkForwardResults *wf,
                                            const BacktestResults *data,
                                            int n_splits, int horizon_ticks,
                                            int buffer_ticks, int min_train_samples,
                                            volatile int *progress_pct,
                                            volatile int *cancel_flag) {
    memset(wf, 0, sizeof(*wf));
    *progress_pct = 0;

#ifndef USE_XGBOOST
    fprintf(stderr, "[walkforward] XGBoost not compiled in — cannot train. "
            "rebuild with -DUSE_XGBOOST=ON\n");
    *progress_pct = 100;
    return;
#else
    if (data->sample_count < 100) {
        fprintf(stderr, "[walkforward] only %d samples — need at least 100 for walk-forward\n",
                data->sample_count);
        *progress_pct = 100;
        return;
    }

    struct timeval t_start, t_end;
    gettimeofday(&t_start, NULL);

    // generate purged folds
    if (n_splits < 2) n_splits = 5;
    if (buffer_ticks <= 0) buffer_ticks = PURGE_BUFFER_DEFAULT;
    if (min_train_samples < 50) min_train_samples = 50;
    wf->num_folds = n_splits;

    // pre-compact: extract non-neutral samples into contiguous arrays
    // barrier labels produce ~97% neutrals, so we must split over non-neutrals only
    // otherwise later folds' test sets land in the all-neutral tail and get 0 samples
    int nn_count = 0; // non-neutral count
    for (int i = 0; i < data->sample_count; i++) {
        if (data->labels[i] != 0.5f) nn_count++;
    }

    if (nn_count < 100) {
        fprintf(stderr, "[walkforward] only %d non-neutral samples — need at least 100\n", nn_count);
        *progress_pct = 100;
        return;
    }

    // allocate compacted non-neutral data (features + labels + original indices for purge)
    float *nn_features = (float *)malloc(nn_count * MODEL_NUM_FEATURES * sizeof(float));
    float *nn_labels   = (float *)malloc(nn_count * sizeof(float));
    int   *nn_indices  = (int *)malloc(nn_count * sizeof(int)); // original sample index
    if (!nn_features || !nn_labels || !nn_indices) {
        fprintf(stderr, "[walkforward] failed to allocate compaction buffers\n");
        free(nn_features); free(nn_labels); free(nn_indices);
        *progress_pct = 100;
        return;
    }

    int j = 0;
    for (int i = 0; i < data->sample_count; i++) {
        if (data->labels[i] == 0.5f) continue;
        memcpy(&nn_features[j * MODEL_NUM_FEATURES],
               &data->feature_matrix[i * MODEL_NUM_FEATURES],
               MODEL_NUM_FEATURES * sizeof(float));
        nn_labels[j] = data->labels[i];
        nn_indices[j] = i;
        j++;
    }

    fprintf(stderr, "[walkforward] non-neutral samples: %d / %d total (%.1f%%)\n",
            nn_count, data->sample_count, 100.0 * nn_count / data->sample_count);

    // compute purge gap in non-neutral index space
    // original purge_gap is in sample indices; scale by non-neutral density
    int raw_purge = PurgeGap_Compute(horizon_ticks, buffer_ticks);
    double nn_density = (double)nn_count / data->sample_count;
    int nn_purge = (int)(raw_purge * nn_density + 0.5);
    if (nn_purge < 1) nn_purge = 1;

    fprintf(stderr, "[walkforward] purge gap: %d raw → %d in non-neutral space (density %.3f)\n",
            raw_purge, nn_purge, nn_density);

    // generate folds over non-neutral samples using explicit purge gap
    // (ValidationSplit_Generate would apply FeatureLookback_Max in raw space, not non-neutral space)
    int valid = ValidationSplit_GenerateExplicit(wf->splits, nn_count,
                                                 n_splits, nn_purge,
                                                 min_train_samples);
    wf->valid_folds = valid;

    if (valid == 0) {
        fprintf(stderr, "[walkforward] no valid folds — aborting\n");
        free(nn_features); free(nn_labels); free(nn_indices);
        *progress_pct = 100;
        return;
    }

    ValidationSplit_Print(wf->splits, n_splits);

    float sum_val = 0.0f, sum_val_sq = 0.0f, sum_train = 0.0f;
    int counted_folds = 0;

    for (int f = 0; f < n_splits; f++) {
        if (*cancel_flag) break;

        PurgedSplit *sp = &wf->splits[f];
        WalkForwardFoldResult *fr = &wf->folds[f];
        memset(fr, 0, sizeof(*fr));

        if (!sp->valid) {
            fr->valid = 0;
            continue;
        }

        int n_train = sp->train_count;
        int n_test  = sp->test_count;

        fprintf(stderr, "[walkforward] fold %d/%d: train=%d, test=%d (non-neutral)\n",
                f + 1, n_splits, n_train, n_test);

        if (n_train < 10 || n_test < 5) {
            fprintf(stderr, "[walkforward] fold %d: too few samples — skipping\n", f + 1);
            fr->valid = 0;
            continue;
        }

        // pointers directly into the pre-compacted non-neutral arrays
        const float *train_features = &nn_features[sp->train_start * MODEL_NUM_FEATURES];
        const float *train_labels   = &nn_labels[sp->train_start];
        const float *test_features  = &nn_features[sp->test_start * MODEL_NUM_FEATURES];
        const float *test_labels    = &nn_labels[sp->test_start];

        DMatrixHandle dtrain = NULL, dtest = NULL;
        BoosterHandle booster = NULL;

        // create train DMatrix from pre-compacted non-neutral data
        int ret = XGDMatrixCreateFromMat(train_features, n_train,
                                          MODEL_NUM_FEATURES, -1.0f, &dtrain);
        if (ret != 0) {
            fprintf(stderr, "[walkforward] fold %d: failed to create train DMatrix: %s\n",
                    f + 1, XGBGetLastError());
            fr->valid = 0;
            continue;
        }
        XGDMatrixSetFloatInfo(dtrain, "label", train_labels, n_train);

        // create test DMatrix from pre-compacted non-neutral data
        ret = XGDMatrixCreateFromMat(test_features, n_test,
                                      MODEL_NUM_FEATURES, -1.0f, &dtest);
        if (ret != 0) {
            fprintf(stderr, "[walkforward] fold %d: failed to create test DMatrix: %s\n",
                    f + 1, XGBGetLastError());
            XGDMatrixFree(dtrain);
            fr->valid = 0;
            continue;
        }
        XGDMatrixSetFloatInfo(dtest, "label", test_labels, n_test);

        // create and train booster
        ret = XGBoosterCreate(&dtrain, 1, &booster);
        if (ret != 0) {
            XGDMatrixFree(dtrain);
            XGDMatrixFree(dtest);
            fr->valid = 0;
            continue;
        }

        // training params — match the suite's existing training config
        XGBoosterSetParam(booster, "objective", "binary:logistic");
        XGBoosterSetParam(booster, "max_depth", "6");
        XGBoosterSetParam(booster, "eta", "0.1");
        XGBoosterSetParam(booster, "subsample", "0.8");
        XGBoosterSetParam(booster, "colsample_bytree", "0.8");
        XGBoosterSetParam(booster, "min_child_weight", "5");
        XGBoosterSetParam(booster, "nthread", "1");
        XGBoosterSetParam(booster, "verbosity", "0");
        XGBoosterSetParam(booster, "seed", "42");

        // train with early stopping eval on test set
        DMatrixHandle evals[] = { dtrain, dtest };
        const char *eval_names[] = { "train", "val" };
        int n_rounds = 200;

        for (int r = 0; r < n_rounds; r++) {
            ret = XGBoosterUpdateOneIter(booster, r, dtrain);
            if (ret != 0) break;
        }

        // predict on train set (for overfit detection)
        {
            bst_ulong out_len;
            const float *out_result;
            ret = XGBoosterPredict(booster, dtrain, 0, 0, 0, &out_len, &out_result);
            if (ret == 0 && (int)out_len == n_train) {
                fr->train_accuracy = WalkForward_ComputeAccuracy(
                    out_result, train_labels, n_train, 0.5f);
            }
        }

        // predict on test set (the metric that matters)
        {
            bst_ulong out_len;
            const float *out_result;
            ret = XGBoosterPredict(booster, dtest, 0, 0, 0, &out_len, &out_result);
            if (ret == 0 && (int)out_len == n_test) {
                fr->val_accuracy = WalkForward_ComputeAccuracy(
                    out_result, test_labels, n_test, 0.5f);
            }
        }

        // feature importances (stability tracking hook)
        // XGBoost importance via dump → we use a simpler approach: score type
        // for now, zero-fill — populated when we add importance extraction
        memset(fr->feature_importances, 0, sizeof(fr->feature_importances));

        fr->train_samples = n_train;
        fr->test_samples = n_test;
        fr->valid = 1;

        // overfit detection per fold
        fr->overfit = OverfitDetection_CheckDefaults(
            fr->train_accuracy, -1.0f, fr->val_accuracy, MODEL_NUM_FEATURES);
        OverfitDetection_Print(&fr->overfit, f);

        // accumulate stats
        sum_val += fr->val_accuracy;
        sum_val_sq += fr->val_accuracy * fr->val_accuracy;
        sum_train += fr->train_accuracy;
        if (fr->overfit.is_overfit) wf->overfit_count++;
        counted_folds++;

        // cleanup
        XGBoosterFree(booster);
        XGDMatrixFree(dtrain);
        XGDMatrixFree(dtest);

        *progress_pct = (int)(100.0 * (f + 1) / n_splits);

        fprintf(stderr, "[walkforward] fold %d/%d: train_acc=%.4f, val_acc=%.4f%s\n",
                f + 1, n_splits, fr->train_accuracy, fr->val_accuracy,
                fr->overfit.is_overfit ? " [OVERFIT]" : "");
    }

    free(nn_features);
    free(nn_labels);
    free(nn_indices);

    // compute aggregate stats
    if (counted_folds > 0) {
        wf->mean_val_accuracy = sum_val / counted_folds;
        wf->mean_train_accuracy = sum_train / counted_folds;
        float var = (sum_val_sq / counted_folds) - (wf->mean_val_accuracy * wf->mean_val_accuracy);
        wf->std_val_accuracy = (var > 0.0f) ? (float)sqrt((double)var) : 0.0f;
    }

    gettimeofday(&t_end, NULL);
    wf->elapsed_ms = (t_end.tv_sec - t_start.tv_sec) * 1000.0
                   + (t_end.tv_usec - t_start.tv_usec) / 1000.0;

    *progress_pct = 100;

    fprintf(stderr, "\n[walkforward] === RESULTS ===\n");
    fprintf(stderr, "  valid folds: %d/%d\n", counted_folds, n_splits);
    fprintf(stderr, "  mean val accuracy:   %.4f +/- %.4f\n",
            wf->mean_val_accuracy, wf->std_val_accuracy);
    fprintf(stderr, "  mean train accuracy: %.4f\n", wf->mean_train_accuracy);
    fprintf(stderr, "  train/val gap:       %.4f\n",
            wf->mean_train_accuracy - wf->mean_val_accuracy);
    fprintf(stderr, "  overfit folds:       %d/%d\n", wf->overfit_count, counted_folds);
    fprintf(stderr, "  elapsed:             %.1f ms\n", wf->elapsed_ms);
    fprintf(stderr, "==============================\n\n");

#endif // USE_XGBOOST
}

//======================================================================================================
// [CONFIG FIELD SETTER]
//======================================================================================================
// sets a config field by key name + double value. used by optimizer to sweep parameters.
// returns 1 if field was found and set, 0 if unknown key.
// handles both FPN and PCT fields (PCT keys are stored as decimal, value comes in as %).
//======================================================================================================
static inline int ConfigField_Set(ControllerConfig<BACKTEST_FP> *cfg, const char *key, double value) {
    // percentage fields (config says 4.0, stored as 0.04)
    #define OPT_SET_PCT(name) \
        if (strcmp(key, #name) == 0) { cfg->name = FPN_FromDouble<BACKTEST_FP>(value / 100.0); return 1; }
    // raw FPN fields
    #define OPT_SET_FPN(name) \
        if (strcmp(key, #name) == 0) { cfg->name = FPN_FromDouble<BACKTEST_FP>(value); return 1; }
    // uint32 fields
    #define OPT_SET_U32(name) \
        if (strcmp(key, #name) == 0) { cfg->name = (uint32_t)value; return 1; }

    OPT_SET_PCT(take_profit_pct)
    OPT_SET_PCT(stop_loss_pct)
    OPT_SET_PCT(fee_rate)
    OPT_SET_PCT(entry_offset_pct)
    OPT_SET_PCT(slippage_pct)
    OPT_SET_PCT(max_exposure_pct)
    OPT_SET_PCT(risk_pct)
    OPT_SET_PCT(max_drawdown_pct)
    OPT_SET_FPN(offset_stddev_mult)
    OPT_SET_FPN(spacing_multiplier)
    OPT_SET_FPN(momentum_breakout_mult)
    OPT_SET_FPN(momentum_tp_mult)
    OPT_SET_FPN(momentum_sl_mult)
    OPT_SET_FPN(tp_hold_score)
    OPT_SET_FPN(tp_trail_mult)
    OPT_SET_FPN(sl_trail_mult)
    OPT_SET_FPN(no_trade_band_mult)
    OPT_SET_FPN(ml_buy_threshold)
    OPT_SET_FPN(danger_warn_stddevs)
    OPT_SET_FPN(danger_crash_stddevs)
    OPT_SET_U32(poll_interval)
    OPT_SET_U32(warmup_ticks)
    OPT_SET_U32(max_hold_ticks)
    OPT_SET_U32(sl_cooldown_base)

    #undef OPT_SET_PCT
    #undef OPT_SET_FPN
    #undef OPT_SET_U32
    return 0;
}

//======================================================================================================
// [OPTIMIZER]
//======================================================================================================
#define OPT_MAX_PARAMS 2
#define OPT_MAX_STEPS  50
#define OPT_MAX_GRID   (OPT_MAX_STEPS * OPT_MAX_STEPS)

struct OptimizerRange {
    char key[32];
    double lo, hi, step;
    int steps() const { return (step > 1e-12) ? (int)((hi - lo) / step) + 1 : 1; }
};

struct OptimizerResults {
    double metric[OPT_MAX_GRID];       // selected metric per cell
    BacktestStats stats[OPT_MAX_GRID]; // full stats per cell
    double param_vals[OPT_MAX_PARAMS][OPT_MAX_STEPS]; // actual parameter values
    int dims[OPT_MAX_PARAMS];          // steps per dimension
    int num_params;
    int total_runs;
    int best_idx;
};

// metric selector
#define OPT_METRIC_SHARPE      0
#define OPT_METRIC_PF          1
#define OPT_METRIC_EXPECTANCY  2
#define OPT_METRIC_RETURN      3
#define OPT_METRIC_PNL         4

static inline double OptimizerMetric(const BacktestStats *s, int metric) {
    switch (metric) {
        case OPT_METRIC_SHARPE:     return s->sharpe_ratio;
        case OPT_METRIC_PF:         return s->profit_factor;
        case OPT_METRIC_EXPECTANCY: return s->expectancy;
        case OPT_METRIC_RETURN:     return s->return_pct;
        case OPT_METRIC_PNL:        return s->total_pnl;
        default:                    return s->total_pnl;
    }
}

static inline void Backtest_RunSweep(OptimizerResults *opt,
                                      const BacktestRunConfig *base_cfg,
                                      const OptimizerRange *ranges, int num_params,
                                      int metric_idx,
                                      volatile int *current_run, volatile int *total_runs,
                                      volatile int *cancel_flag) {
    opt->num_params = num_params;
    opt->dims[0] = ranges[0].steps();
    opt->dims[1] = (num_params > 1) ? ranges[1].steps() : 1;
    opt->total_runs = opt->dims[0] * opt->dims[1];
    *total_runs = opt->total_runs;
    *current_run = 0;
    opt->best_idx = 0;
    double best_metric = -1e30;

    // store parameter values
    for (int i = 0; i < opt->dims[0]; i++)
        opt->param_vals[0][i] = ranges[0].lo + i * ranges[0].step;
    if (num_params > 1)
        for (int i = 0; i < opt->dims[1]; i++)
            opt->param_vals[1][i] = ranges[1].lo + i * ranges[1].step;

    // load base config once
    ControllerConfig<BACKTEST_FP> base = ControllerConfig_Load<BACKTEST_FP>(base_cfg->config_path);

    for (int i0 = 0; i0 < opt->dims[0]; i0++) {
        for (int i1 = 0; i1 < opt->dims[1]; i1++) {
            if (*cancel_flag) return;

            int idx = i0 * opt->dims[1] + i1;
            *current_run = idx + 1;

            // apply parameter overrides
            ControllerConfig<BACKTEST_FP> cfg = base;
            ConfigField_Set(&cfg, ranges[0].key, opt->param_vals[0][i0]);
            if (num_params > 1)
                ConfigField_Set(&cfg, ranges[1].key, opt->param_vals[1][i1]);

            // run backtest with this config
            BacktestRunConfig run = *base_cfg;
            run.config_override = cfg;
            run.use_config_override = 1;
            run.collect_features = 0;

            BacktestResults results;
            BacktestResults_Init(&results);
            int dummy_progress = 0;
            Backtest_Run(&results, &run, &dummy_progress, cancel_flag, NULL);

            opt->stats[idx] = results.stats;
            opt->metric[idx] = OptimizerMetric(&results.stats, metric_idx);
            BacktestResults_Free(&results);

            if (opt->metric[idx] > best_metric) {
                best_metric = opt->metric[idx];
                opt->best_idx = idx;
            }
        }
    }
}

#endif // BACKTEST_ENGINE_HPP
