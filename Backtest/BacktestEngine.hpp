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

#include "PhaseTimers.hpp"  // v5.10.0 Item A — per-phase backtest timers
#include "../CoreFrameworks/PortfolioController.hpp"
#include "../CoreFrameworks/OrderGates.hpp"
#include "../CoreFrameworks/MetricCompute.hpp"  // v5.8.4c: shared metric helpers
#include "../CoreFrameworks/ParseFast.hpp"      // F-054: tt::parse_double_fast_advance (locale-immune replay parse)
#include "../DataStream/TradeLog.hpp"
#include "../ML_Headers/ModelInference.hpp"
#include "../ML_Headers/FeatureRegistry.hpp"  // v5.8.6: FEATURE_REGISTRY_HASH() for auto-stamp
#include "../ML_Headers/BuildFlags.hpp"       // v5.9.5h: BUILD_FLAGS_HASH() for cross-build drift detection
#include "../ML_Headers/StampHelper.hpp"      // v5.15.3.A: Stamp_AssembleAndEmit canonical orchestration helper
#include "../Version.hpp"                      // v5.8.6: ENGINE_VERSION_STRING for auto-stamp
#include "../MemHeaders/HealthLog.hpp"        // v5.11.32: Health_Log for WF observability
#include "../MemHeaders/DebugLog.hpp"         // v5.11.32: LOG_DEBUG_ENGINE for compile-time-only diags
#include "../GUI/CandleAccumulator.hpp"
#include "LabelFunctions.hpp"
#include "BacktestSnapshot.hpp"
#include "XGBHyperparams.hpp"  // v5.9.5h: single source of truth for XGBoost hyperparams
#include "ValidationSplit.hpp"
#include "OverfitDetection.hpp"
#include "HeldOutSplit.hpp"  // Phase 7prep — locked held-out test set discipline
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <pthread.h>     // v5.10.0a.F — parallel hyperparam sweep workers
#include <functional>    // v5.10.0a.F — std::function holder for sweep cell lambda

// FPN_Binary width — must match the engine build
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
            const char *e;  // F-054: locale-immune float parse advances a const cursor; bridge back to p (line[] is mutable). Integer strtoll/strtol are locale-immune → unchanged (TECH_DEBT-144).
            t->timestamp_us = strtoll(p, &p, 10); if (*p == ',') p++;
            t->price = tt::parse_double_fast_advance(p, &e); p = (char*)e; if (*p == ',') p++;
            t->qty   = tt::parse_double_fast_advance(p, &e); p = (char*)e; if (*p == ',') p++;
            t->is_buyer_maker = (int)strtol(p, &p, 10);
        } else {
            // Binance aggTrades: id,price,qty,first_id,last_id,timestamp,is_buyer_maker
            char *p = line;
            const char *e;  // F-054 (see format==1 note): float parses locale-immune; integer skips unchanged
            strtoll(p, &p, 10); if (*p == ',') p++;                     // skip id
            t->price = tt::parse_double_fast_advance(p, &e); p = (char*)e; if (*p == ',') p++;
            t->qty   = tt::parse_double_fast_advance(p, &e); p = (char*)e; if (*p == ',') p++;
            strtoll(p, &p, 10); if (*p == ',') p++;                     // skip first_id
            strtoll(p, &p, 10); if (*p == ',') p++;                     // skip last_id
            t->timestamp_us = strtoll(p, &p, 10); if (*p == ',') p++;
            // is_buyer_maker can be "true"/"false" or 1/0
            if (*p == 't' || *p == 'T') t->is_buyer_maker = 1;
            else if (*p == 'f' || *p == 'F') t->is_buyer_maker = 0;
            else t->is_buyer_maker = (int)strtol(p, &p, 10);
        }

        // v5.9.5j.2 — bogus-ts filter. TickRecorder occasionally writes
        // truncated rows (write interrupted mid-CSV: only 6 of 8 fields,
        // ts column ends up containing a partial '17144' instead of
        // full ms timestamp '1714348800000'). Sanity bound: any tick
        // with ts < 2017-07-14 (1.5e12 ms) is corrupt — skip.
        // Format-1 (TickRecorder) uses microseconds; bound 1.5e15.
        // Format-0 (Binance aggTrades) uses milliseconds; bound 1.5e12.
        const int64_t MIN_VALID_TS = (format == 1)
            ? 1500000000000000LL    // 1.5e15 µs = 2017-07-14
            : 1500000000000LL;       // 1.5e12 ms = 2017-07-14
        if (t->price > 0.0 && t->qty > 0.0 && t->timestamp_us >= MIN_VALID_TS)
            (*count)++;
    }

    fclose(f);
    fprintf(stderr, "[backtest] loaded %d ticks from %s\n", *count, csv_path);
    return *count > 0 ? 1 : 0;
}

//======================================================================================================
// [TICK SORT VALIDATION — v5.9.2c]
//======================================================================================================
// Validates the tick array is timestamp-monotonic (`ticks[i].timestamp_us
// >= ticks[i-1].timestamp_us`). Closes the silent-drift class where
// concatenated daily exports / mistyped tick replays produce out-of-order
// CSVs that silently corrupt rolling stats / ROR / tick-rate features at
// training time. Caller passes cfg's csv_sort_check_mode to choose
// behavior on violation: WARN (default) / STRICT / AUTO.
//
// Returns: 0 = clean (no violations or auto-sorted), -1 = STRICT refusal.
// Caller in STRICT mode should treat -1 as "abort run".
//======================================================================================================
static inline int HistoricalTick_CmpByTime(const void *a, const void *b) {
    int64_t ta = ((const HistoricalTick*)a)->timestamp_us;
    int64_t tb = ((const HistoricalTick*)b)->timestamp_us;
    if (ta < tb) return -1;
    if (ta > tb) return 1;
    return 0;
}

static inline int BacktestData_ValidateSort(HistoricalTick *ticks, int count,
                                             int mode, const char *label) {
    if (count < 2) return 0;
    int violations = 0;
    int first_idx = -1;
    for (int i = 1; i < count; i++) {
        if (ticks[i].timestamp_us < ticks[i-1].timestamp_us) {
            violations++;
            if (first_idx < 0) first_idx = i;
        }
    }
    if (violations == 0) return 0;

    if (mode == CSV_SORT_STRICT) {
        fprintf(stderr,
            "[FATAL] csv_sort_check_mode=strict: %s has %d tick ordering "
            "violations (first at idx %d: ts=%lld < prev=%lld). Refusing load.\n",
            label, violations, first_idx,
            (long long)ticks[first_idx].timestamp_us,
            (long long)ticks[first_idx-1].timestamp_us);
        return -1;
    } else if (mode == CSV_SORT_AUTO) {
        qsort(ticks, (size_t)count, sizeof(HistoricalTick), HistoricalTick_CmpByTime);
        fprintf(stderr,
            "[INFO] csv_sort_check_mode=auto: %s had %d violations, sorted in-place.\n",
            label, violations);
        return 0;
    } else {  // CSV_SORT_WARN (default, or unknown mode treated as warn)
        fprintf(stderr,
            "[WARN] %s has %d tick ordering violations (first at idx %d). "
            "Features will be computed on out-of-order data. Set "
            "csv_sort_check_mode=2 (auto) to sort, or =1 (strict) to refuse.\n",
            label, violations, first_idx);
        return 0;
    }
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
    // v5.10.0a.next.1 — operator-explicit bandit state prior. When set,
    // BacktestSharded_Run loads bandit weights from this path AFTER the
    // default <node_model_dir>/bandit_state.json load, overriding it.
    // Bundle-ID check is SKIPPED on this path (operator may intentionally
    // bootstrap a new model bundle with weights from a sibling — e.g.
    // transfer learning across compatible horizon lists). Empty = no
    // prior, use default load only.
    char bandit_state_prior_path[400];
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
    // v5.8.4c: replaces the legacy -1.0 "no-losses" sentinel that used to
    // be packed into profit_factor itself. Set to 1 when wins > 0 and
    // losses == 0 — display layer renders "—" (or "∞") on this flag,
    // optimizer continues to read profit_factor numerically (now 0.0 in
    // that case). Cleanly separates math from display semantics.
    int all_wins_run;
    // v5.9.1: count of label samples that produced NaN/Inf at compute.
    // For binary/regression: wrapped to neutral default (0.5/0.0) and consumed.
    // For multiclass: sample is skipped (not included in training matrix), since
    // softmax has no defensible neutral default. nan_labels_dropped only
    // increments on the multiclass-skip path; binary/regression saved samples
    // are tracked via their own counters if needed.
    uint32_t nan_labels_total;     // total NaN/Inf label outputs encountered
    uint32_t nan_labels_dropped;   // multiclass samples skipped from training
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
// [TRAINING HELPERS]
//======================================================================================================
// XGBoost binary `scale_pos_weight` compensates for class imbalance.
// At ratio 0.2% positive (typical for tight-barrier label sets on BTC),
// the trivial "predict majority class" baseline gets >99% accuracy and
// the loss function has zero pressure to learn the minority class —
// every walk-forward fold flags as memorization. scale_pos_weight =
// n_neg/n_pos rebalances the loss so positives count equally per-class.
//
// Multiclass (multi:softprob) ignores scale_pos_weight; use per-sample
// weights via XGDMatrixSetFloatInfo("weight", ...) for those.
// Regression (reg:squarederror) doesn't need it.
//
// Threshold for "positive": label >= 0.5f. Matches the convention used
// in WalkForward_ComputeAccuracy and the binary-classifier label values
// (0.0 = negative, 1.0 = positive, 0.5 = neutral and already filtered).
static inline double XGBoost_ComputeScalePosWeight(const float *labels, int n,
                                                     int *out_n_pos = nullptr,
                                                     int *out_n_neg = nullptr) {
    int n_pos = 0, n_neg = 0;
    for (int i = 0; i < n; i++) {
        if (labels[i] >= 0.5f) n_pos++;
        else n_neg++;
    }
    if (out_n_pos) *out_n_pos = n_pos;
    if (out_n_neg) *out_n_neg = n_neg;
    // guard against zero-positive (degenerate dataset) — return 1.0 so
    // XGBoost doesn't divide by zero. caller should also surface this.
    if (n_pos == 0) return 1.0;
    return (double)n_neg / (double)n_pos;
}

// Multiclass: scale_pos_weight is binary-only. For multiclass softmax we use
// per-sample weights via XGDMatrixSetFloatInfo(d, "weight", ...). Inverse-
// frequency formula:
//
//   weight[i] = total / (K * count[label[i]])
//
// Each class contributes equally to the loss regardless of frequency. A class
// with 95% of samples gets weight ~0.21 per sample; a class with 1% gets
// weight ~33.0 per sample. Without this, multiclass with skewed distribution
// (e.g. PEAK_VALLEY_STABLE typically ~95% stable on tick-scale BTC) trains
// a model that trivially predicts the majority class for high accuracy but
// zero predictive value for the minority classes — same failure mode as
// binary class imbalance with no scale_pos_weight.
//
// out_weights buffer must be `count` floats. out_counts (optional, [num_classes])
// receives per-class sample counts so caller can log them.
static inline void XGBoost_ComputeMulticlassWeights(const float *labels, int count,
                                                      int num_classes, float *out_weights,
                                                      int *out_counts = nullptr) {
    if (num_classes < 2 || count <= 0) {
        for (int i = 0; i < count; i++) out_weights[i] = 1.0f;
        return;
    }
    int K = num_classes > 16 ? 16 : num_classes;
    int counts[16] = {0};
    for (int i = 0; i < count; i++) {
        int c = (int)(labels[i] + 0.5f);
        if (c >= 0 && c < K) counts[c]++;
    }
    for (int i = 0; i < count; i++) {
        int c = (int)(labels[i] + 0.5f);
        if (c >= 0 && c < K && counts[c] > 0) {
            out_weights[i] = (float)count / ((float)K * (float)counts[c]);
        } else {
            out_weights[i] = 1.0f;
        }
    }
    if (out_counts) {
        for (int k = 0; k < K; k++) out_counts[k] = counts[k];
    }
}

//======================================================================================================
// [STATS COMPUTE]
//======================================================================================================
// v5.8.4c: per-metric Compute_* helpers + MaxDrawdown_UpdateIncremental
// live in CoreFrameworks/MetricCompute.hpp (included via
// ShardedSnapshot.hpp / EngineTUI.hpp's transitive includes). Single
// source of truth across backtest, live TUI, and per-core snapshot
// paths — kills the 4-site profit_factor drift + 3-site expectancy
// fabs() inconsistency + 2-site max_drawdown reimplementation.
//======================================================================================================
static inline void BacktestStats_Compute(BacktestStats *stats,
                                          const PortfolioController<BACKTEST_FP> *ctrl,
                                          double starting_balance,
                                          double elapsed_ms) {
    stats->total_trades = ctrl->total_buys;
    stats->wins = ctrl->wins;
    stats->losses = ctrl->losses;
    stats->total_pnl = Money_ToDouble(ctrl->realized_pnl);
    stats->total_fees = Money_ToDouble(ctrl->total_fees);
    stats->ticks_processed = ctrl->total_ticks;
    stats->elapsed_ms = elapsed_ms;

    // v5.8.4c: route every metric through Compute_* helpers (single
    // source of truth shared with EngineTUI / ShardedSnapshot paths).
    double gw = Money_ToDouble(ctrl->gross_wins);
    double gl = Money_ToDouble(ctrl->gross_losses);
    stats->avg_win  = (stats->wins > 0)   ? gw / stats->wins   : 0.0;
    stats->avg_loss = (stats->losses > 0) ? gl / stats->losses : 0.0;
    stats->win_rate       = Compute_WinRate(stats->wins, stats->total_trades);
    stats->profit_factor  = Compute_ProfitFactor(gw, gl);
    stats->all_wins_run   = Compute_AllWinsRun(gw, gl);
    stats->expectancy     = Compute_Expectancy(stats->total_trades, stats->wins,
                                                stats->avg_win, stats->avg_loss);
    stats->return_pct     = Compute_ReturnPct(stats->total_pnl, starting_balance);
    stats->avg_hold_ticks = Compute_AvgHoldTicks(ctrl->total_hold_ticks, stats->total_trades);

    // max drawdown — compute from equity curve in results (caller's job)
    // sharpe — needs equity curve data too
}

//======================================================================================================
// [MAX DRAWDOWN + SHARPE from equity curve]
//======================================================================================================
static inline void BacktestStats_ComputeFromEquity(BacktestStats *stats,
                                                    const double *equity, int count) {
    // v5.8.4c: max drawdown via shared MaxDrawdown_UpdateIncremental helper —
    // BacktestSharded.hpp's per-tick path calls the same helper with the
    // same scalar update logic. Bytewise FP identity by construction.
    double peak = (count > 0) ? equity[0] : 0.0;
    double max_dd = 0.0;
    double max_dd_pct = 0.0;
    for (int i = 1; i < count; i++) {
        MaxDrawdown_UpdateIncremental(equity[i], &peak, &max_dd, &max_dd_pct);
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
// [LABEL COMPUTATION HELPER — Track E.1]
//======================================================================================================
// Compute labels for a populated BacktestResults using forward-looking tick
// data. Extracted from the legacy Backtest_Run body so both legacy and
// sharded paths can share it (sharded path adopted feature collection in
// E.1; this is the matching label-side parity).
//
// Caller must have already set results->sample_count, results->sample_tick_indices,
// results->sample_prices, and results->stats.ticks_processed. Uses
// run_cfg->data_paths to reload the full tick stream (labels need forward-
// looking data the replay already discarded).
//
// No-op when collect_features=0 or sample_count==0 — caller doesn't need to
// gate.
//======================================================================================================
// v5.10.0 Item B — streaming sliding-window label compute. Closes 2026-05-03
// OOM (28 GB label_ticks for 1-year, 57 GB for 2-year). Key insight: labels
// only need the FORWARD WINDOW from each sample's tick position, not the
// full historical tick array. By keeping just 2 files in memory at a time,
// peak RAM drops from O(total_ticks * 32B) to O(2 * max_per_file * 32B).
//
// Memory math (operator's box, 30.9 GiB):
//   1-year (895M ticks):  28.6 GB → ~160 MB peak (~180x reduction)
//   2-year (1.8B ticks):  57 GB (OOM) → ~160 MB peak (no OOM)
//   5-year (4.5B ticks):  144 GB (infeasible) → ~160 MB peak (now feasible)
//
// Algorithm:
//   1. Pre-pass: count ticks per file → file_offsets[] cumsum
//   2. Walk files; maintain 2-file sliding window:
//      - Invariant: buf[0..prev_file_count) = file f
//      - Invariant: buf[prev_file_count..total) = file f+1 (if exists)
//   3. Per-file: label all samples whose tidx ∈ [file_offsets[f], file_offsets[f+1])
//      using the combined buf — label_fn's forward-scan crosses file f → f+1
//      transparently since they're contiguous in memory.
//   4. Slide: memmove file f+1 to buf[0]; load file f+2 at buf[file_count].
//
// Samples are guaranteed sorted by tidx (appended monotonically in
// BacktestSharded.hpp's on_slow_path lambda). One linear cursor walk
// through samples; no per-file O(N) sample scans.
static inline void Backtest_ComputeLabelsFromSamples(BacktestResults *results,
                                                      const BacktestRunConfig *run_cfg) {
    if (!run_cfg->collect_features) return;
    if (results->sample_count <= 0) return;
    if (run_cfg->num_data_files <= 0) return;

    // v5.10.0 Item A — label_compute phase timer (wraps full body, RAII guard).
    uint64_t label_start_ns = tt::PhaseTimer_NowNs();
    struct LabelGuard {
        uint64_t start;
        ~LabelGuard() {
            tt::PhaseTimer_Global().label_compute_ns +=
                tt::PhaseTimer_NowNs() - start;
            tt::PhaseTimer_Global().populated = 1;
        }
    } _label_guard{label_start_ns};

    // Phase 1 — pre-pass: count ticks per file + compute cumulative offsets.
    // Cheap I/O (line-counting). Needed so we can locate which file contains
    // each sample's tidx during the streaming walk.
    int num_files = run_cfg->num_data_files;
    int* file_counts = (int*)calloc(num_files, sizeof(int));
    int64_t* file_offsets = (int64_t*)calloc(num_files + 1, sizeof(int64_t));
    if (!file_counts || !file_offsets) {
        free(file_counts);
        free(file_offsets);
        fprintf(stderr, "[backtest] label_compute: failed to allocate file index\n");
        return;
    }
    int max_per_file = 0;
    for (int f = 0; f < num_files; f++) {
        FILE* fp = fopen(run_cfg->data_paths[f], "r");
        if (!fp) {
            fprintf(stderr, "[backtest] label_compute: failed to open %s\n",
                    run_cfg->data_paths[f]);
            continue;
        }
        int lines = 0;
        char buf[512];
        while (fgets(buf, sizeof(buf), fp)) lines++;
        fclose(fp);
        file_counts[f] = (lines > 0) ? (lines - 1) : 0;  // header
        if (file_counts[f] > max_per_file) max_per_file = file_counts[f];
        file_offsets[f + 1] = file_offsets[f] + file_counts[f];
    }
    int64_t total_ticks = file_offsets[num_files];

    // Phase 2 — allocate 2-file sliding window. Round up max_per_file by 1024
    // for BacktestData_Load's slack (matches existing pattern).
    int per_file_cap = max_per_file + 1024;
    if (per_file_cap < 1024) per_file_cap = 1024;
    size_t buf_cap = (size_t)per_file_cap * 2;
    HistoricalTick* tick_buf = (HistoricalTick*)malloc(buf_cap * sizeof(HistoricalTick));
    if (!tick_buf) {
        free(file_counts);
        free(file_offsets);
        fprintf(stderr, "[backtest] label_compute: failed to alloc 2-file buf "
                "(%.0f MB peak)\n", (double)buf_cap * sizeof(HistoricalTick) / 1e6);
        return;
    }
    fprintf(stderr, "[backtest] label_compute: streaming 2-file window — "
            "peak %.0f MB (%d files, %lld total ticks)\n",
            (double)buf_cap * sizeof(HistoricalTick) / 1e6,
            num_files, (long long)total_ticks);

    // Phase 3 — resolve label config.
    LabelFn label_fn = NULL;
    for (int l = 0; l < LABEL_COUNT; l++) {
        if (label_table[l].id == run_cfg->label_type) {
            label_fn = label_table[l].fn;
            break;
        }
    }
    if (!label_fn) label_fn = Label_WinLoss;  // fallback
    double tp = run_cfg->label_tp_pct > 0 ? run_cfg->label_tp_pct : 1.5;
    double sl = run_cfg->label_sl_pct > 0 ? run_cfg->label_sl_pct : 1.0;
    int fwd = run_cfg->label_forward_ticks > 0 ? run_cfg->label_forward_ticks : 1000;
    int is_multiclass = LabelType_IsMulticlass(run_cfg->label_type);
    int is_regression = LabelType_IsRegression(run_cfg->label_type);
    int sort_mode = run_cfg->use_config_override
                  ? run_cfg->config_override.csv_sort_check_mode
                  : CSV_SORT_WARN;

    // Phase 4 — sliding-window load + label.
    int prev_file_count_in_buf = 0;  // file f's count once positioned at buf[0]
    int total_in_buf = 0;
    int sample_cursor = 0;            // monotonic walk through sorted samples
    int64_t prev_file_last_ts = 0;    // for inter-file ordering check

    for (int f = 0; f < num_files; f++) {
        // Maintain invariant: buf[0..prev_file_count) = file f;
        //                     buf[prev_file_count..total) = file f+1 (if exists)
        if (f == 0) {
            // Initial load: file 0 + file 1 (if exists).
            int n0 = 0;
            BacktestData_Load(tick_buf, &n0, per_file_cap, run_cfg->data_paths[0]);
            total_in_buf = n0;
            prev_file_count_in_buf = n0;
            // Per-file sort validation (replaces v5.9.2c concat validation).
            // Cheaper than concatenating; same coverage.
            char file_label[280];
            snprintf(file_label, sizeof(file_label), "label file 0 (%s)",
                     run_cfg->data_paths[0]);
            int sort_rc = BacktestData_ValidateSort(tick_buf, n0, sort_mode,
                                                     file_label);
            if (sort_rc < 0) {
                free(tick_buf); free(file_counts); free(file_offsets);
                return;
            }
            if (n0 > 0) prev_file_last_ts = tick_buf[n0 - 1].timestamp_us;
            if (num_files > 1) {
                int n1 = 0;
                BacktestData_Load(tick_buf + total_in_buf, &n1,
                                  (int)(buf_cap - total_in_buf),
                                  run_cfg->data_paths[1]);
                snprintf(file_label, sizeof(file_label), "label file 1 (%s)",
                         run_cfg->data_paths[1]);
                int sort_rc1 = BacktestData_ValidateSort(tick_buf + total_in_buf,
                                                         n1, sort_mode, file_label);
                if (sort_rc1 < 0) {
                    free(tick_buf); free(file_counts); free(file_offsets);
                    return;
                }
                // Inter-file ordering check.
                if (n1 > 0 && tick_buf[total_in_buf].timestamp_us < prev_file_last_ts) {
                    fprintf(stderr, "[label] WARN: file 1 starts before file 0 ends "
                            "(first ts %lld < prev last %lld)\n",
                            (long long)tick_buf[total_in_buf].timestamp_us,
                            (long long)prev_file_last_ts);
                }
                if (n1 > 0) prev_file_last_ts = tick_buf[total_in_buf + n1 - 1].timestamp_us;
                total_in_buf += n1;
            }
        } else {
            // Slide: memmove file f to buf[0]; load file f+1 at buf[file_count].
            int file_f_in_buf = total_in_buf - prev_file_count_in_buf;
            memmove(tick_buf, tick_buf + prev_file_count_in_buf,
                    (size_t)file_f_in_buf * sizeof(HistoricalTick));
            total_in_buf = file_f_in_buf;
            prev_file_count_in_buf = file_f_in_buf;
            if (f + 1 < num_files) {
                int n_next = 0;
                BacktestData_Load(tick_buf + total_in_buf, &n_next,
                                  (int)(buf_cap - total_in_buf),
                                  run_cfg->data_paths[f + 1]);
                char file_label[280];
                snprintf(file_label, sizeof(file_label), "label file %d (%s)",
                         f + 1, run_cfg->data_paths[f + 1]);
                int sort_rc = BacktestData_ValidateSort(tick_buf + total_in_buf,
                                                         n_next, sort_mode, file_label);
                if (sort_rc < 0) {
                    free(tick_buf); free(file_counts); free(file_offsets);
                    return;
                }
                if (n_next > 0 && tick_buf[total_in_buf].timestamp_us < prev_file_last_ts) {
                    fprintf(stderr, "[label] WARN: file %d starts before file %d ends "
                            "(first ts %lld < prev last %lld)\n",
                            f + 1, f,
                            (long long)tick_buf[total_in_buf].timestamp_us,
                            (long long)prev_file_last_ts);
                }
                if (n_next > 0) prev_file_last_ts = tick_buf[total_in_buf + n_next - 1].timestamp_us;
                total_in_buf += n_next;
            }
        }

        // Label samples whose global tidx falls in file f's range.
        int64_t file_f_start = file_offsets[f];
        int64_t file_f_end   = file_offsets[f + 1];
        while (sample_cursor < results->sample_count) {
            int64_t global_tidx = (int64_t)results->sample_tick_indices[sample_cursor];
            if (global_tidx >= file_f_end) break;  // sample belongs to a later file
            if (global_tidx < file_f_start) {
                // Out-of-order sample (shouldn't happen given monotonic
                // append in on_slow_path) — skip with a warn.
                fprintf(stderr, "[label] WARN: sample %d tidx=%lld < file %d start=%lld; skipping\n",
                        sample_cursor, (long long)global_tidx, f, (long long)file_f_start);
                results->labels[sample_cursor] = is_multiclass ? NAN
                                                : (is_regression ? 0.0f : 0.5f);
                if (is_multiclass) results->stats.nan_labels_dropped++;
                results->stats.nan_labels_total++;
                sample_cursor++;
                continue;
            }
            int local_tidx = (int)(global_tidx - file_f_start);
            // Forward-scan extends into file f+1 transparently (it's contiguous
            // in tick_buf). Pass total_in_buf as upper bound.
            int extra = (run_cfg->label_type == LABEL_REGIME)
                ? results->sample_regimes[sample_cursor] : fwd;
            float lbl = label_fn(tick_buf, local_tidx, total_in_buf,
                                 results->sample_prices[sample_cursor],
                                 tp, sl, extra);
            if (isnan(lbl) || isinf(lbl)) {
                results->stats.nan_labels_total++;
                if (is_multiclass) {
                    results->labels[sample_cursor] = NAN;
                    results->stats.nan_labels_dropped++;
                } else if (is_regression) {
                    results->labels[sample_cursor] = 0.0f;
                } else {
                    results->labels[sample_cursor] = 0.5f;
                }
            } else {
                results->labels[sample_cursor] = lbl;
            }
            sample_cursor++;
        }
    }

    free(tick_buf);
    free(file_counts);
    free(file_offsets);

    fprintf(stderr, "[backtest] computed %d labels (type=%d, tp=%.1f%%, sl=%.1f%%)",
            sample_cursor, run_cfg->label_type, tp, sl);
    if (results->stats.nan_labels_total > 0) {
        fprintf(stderr, " — NaN/Inf: %u total, %u dropped (multiclass)",
                results->stats.nan_labels_total,
                results->stats.nan_labels_dropped);
    }
    fputc('\n', stderr);
}

//======================================================================================================
// [RUN — Track E.7 thin wrapper]
//======================================================================================================
// After Track E.7 (2026-04-26), Backtest_Run is a thin wrapper around the
// sharded path. The legacy `PortfolioController_Tick`-driven body
// (~350 LOC) has been deleted. Sharded is the only backtest path.
//
// `engine_mode` from cfg is now ignored — every backtest runs through
// `BacktestSharded_Run`. Keeping the cfg field parsed (in
// `ControllerConfig.hpp`) for one release cycle so user cfgs setting
// `engine_mode=single_core` don't fail to load; it's a no-op going
// forward and will be removed in a follow-up release.
//
// All call sites (BacktestPanels.hpp Run Backtest button, Sweep,
// FullValidation, WalkForward downstream consumers) keep working
// transparently — same signature, same `BacktestResults` output, same
// label post-pass.
//
// What still works that used to live in the legacy body:
//   - `out_snapshot` populate: `BacktestSharded_Run` calls
//     `TUI_CopySnapshotSharded` at the end (E.7).
//   - `candle_acc` push: throttled per-tick in the replay loop (E.7).
//   - Feature collection: driver `on_slow_path` hook (E.1).
//   - Multi-strategy + ML model load: per-core loop (E.2).
//   - Depth replay + `book_imbalance` gate: `DepthReplayState` (E.3).
//   - Label computation: `Backtest_ComputeLabelsFromSamples` (E.1).
//
// What no longer works because legacy is gone:
//   - Per-tick `gate_reason` diagnostics (legacy printed a "gate
//     reason breakdown" at end of run). Sharded tracks halt_reason
//     per-core in `state.nodes[i].halt_reason`; aggregate breakdown
//     could be added if needed but isn't today. Not load-bearing.
//   - The `BacktestStats_Compute(stats, ctrl, ...)` path that read
//     fields off `PortfolioController`. Sharded computes equivalent
//     stats inline in `BacktestSharded_Run` (P&L, win/loss, drawdown,
//     equity curve).
//======================================================================================================
static inline void Backtest_Run(BacktestResults *results,
                                 const BacktestRunConfig *run_cfg,
                                 volatile int *progress_pct,
                                 volatile int *cancel_flag,
                                 CandleAccumulator *candle_acc,
                                 TUISnapshot *out_snapshot = NULL) {
    tt::BacktestSharded_Run(results, run_cfg, progress_pct, cancel_flag,
                            candle_acc, out_snapshot);
    // Labels need forward-looking ticks the replay already discarded.
    // Helper reloads + computes; no-op when collect_features=0.
    Backtest_ComputeLabelsFromSamples(results, run_cfg);
    // v5.10.0 Item A — dump phase summary at end of pipeline. Sharded run
    // already set total_ns; bump it to include label_compute time we just
    // did. Skip when nothing recorded (silent no-op).
    if (tt::PhaseTimer_Global().populated) {
        // Recompute total to include label_compute time. The sharded run's
        // total_ns is sum of phases at its exit; here we extend by the
        // label_compute_ns delta (already accumulated above).
        tt::PhaseTimer_Global().total_ns =
            tt::PhaseTimer_Global().parse_ns
          + tt::PhaseTimer_Global().fan_out_hot_ns
          + tt::PhaseTimer_Global().feature_collect_ns
          + tt::PhaseTimer_Global().label_compute_ns;
        tt::PhaseTimer_Summary(&tt::PhaseTimer_Global(), stderr);
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
    // classification metrics — populated for binary + multiclass label kinds
    float train_accuracy;     // [0..1]
    float val_accuracy;       // [0..1]
    // regression metrics — populated for regression label kind
    float train_mse;
    float val_mse;
    float train_correlation;  // Pearson r in [-1, +1]
    float val_correlation;
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
    // metric aggregates — interpretation depends on label_kind
    float mean_val_accuracy;       // binary/multiclass
    float std_val_accuracy;        // binary/multiclass
    float mean_train_accuracy;     // binary/multiclass
    float mean_val_mse;            // regression
    float mean_val_correlation;    // regression — load-bearing: signal vs noise
    float mean_train_correlation;  // regression
    int overfit_count;      // folds flagged as overfit (binary/multiclass only)
    int label_kind;         // 0=binary, 1=regression, ≥2=multiclass — display reads this
    int num_classes;        // ≥2 for multiclass; 0 for binary; 1 for regression
    double elapsed_ms;
    char fingerprint[65];   // SHA256 of config + data (empty if not computed)
};

//======================================================================================================
// [FULL VALIDATION — walk-forward + held-out gap]
//======================================================================================================
// Combines walk-forward CV (on train+val portion) with held-out test eval
// (on the locked portion). Reports both side-by-side and computes the
// generalization gap |WF_val - held_out|. Small gap = generalization is real;
// large gap = WF was likely overfit despite per-fold OK numbers.
//
// v5.3.0 Phase A: held-out training is now real (was stubbed in 7prep).
// HeldOutSplit_TrainEval trains one model on the train+val portion using
// the same hyperparameters as a WF fold (train-serve symmetry — gap measured
// against the same model class as deployed) and evaluates on the held-out
// portion. The metric returned is kind-appropriate (accuracy for classification,
// correlation for regression) and matches what WalkForwardResults reports as
// mean_val_*, so the gap |WF - held_out| is apples-to-apples.
//======================================================================================================

// HeldOutSplit_TrainEval — train one XGBoost model on [0, trainval_end_idx)
// of the data and evaluate on [trainval_end_idx, sample_count). Returns the
// kind-appropriate metric. cancel_flag is the standard worker cancellation
// sentinel (NULL = no cancellation).
//
// Why a single train+eval pass and not a CV: the WF mean already captures
// CV-style variance on the train+val portion. The held-out's job is to
// answer "does ONE model trained on the full train+val portion actually
// generalize to data we never touched?" — which is the deployment question.
struct HeldOutTrainEvalResult {
    int   ok;            // 1 if training + eval succeeded
    int   eval_count;    // size of held-out portion actually evaluated (post-filter)
    int   train_count;   // size of train portion actually trained on (post-filter)
    float metric;        // accuracy (classification) or Pearson correlation (regression)
    float mse;           // regression only; 0 for classification
    float correlation;   // regression only; 0 for classification
    float train_metric;  // training-fold metric (sanity check — should exceed `metric`)
};

// Forward declaration — definition follows Backtest_RunWalkForward so the
// helper has visibility into WalkForward_Compute* and XGBoost_Compute* funcs.
static inline HeldOutTrainEvalResult HeldOutSplit_TrainEval(
    const BacktestResults *data,
    const HeldOutSplit *split,
    int label_type,
    volatile int *cancel_flag);

//======================================================================================================
struct FullValidationResults {
    WalkForwardResults walkforward;        // WF CV on [0, trainval_end)

    // held-out eval — populated by Phase 7 finalize; framework only in 7prep
    int held_out_count;                    // size of held-out portion actually evaluated
    float held_out_metric;                 // accuracy or Pearson r per label_kind
    float held_out_mse;                    // regression only
    float held_out_correlation;            // regression only
    OverfitReport held_out_overfit;        // train_metric (from WF) vs val (from held-out)

    // generalization gap — load-bearing
    float wf_to_held_out_gap;              // |WF mean val - held_out| (label-kind-aware)
    int gap_acceptable;                    // 1 if gap < gap_threshold
    float gap_threshold;                   // for traceability — what was the threshold?

    int label_kind;                        // mirrored from WalkForwardResults for display
    int ran_held_out;                      // 1 if held-out training fired, 0 if stubbed/locked
    char fingerprint[65];                  // SHA256 (mirrored from walkforward)

    // v5.3.2 Phase C — auto-stamp on held-out completion. Caller pre-fills
    // auto_stamp_path + auto_stamp_secret + auto_stamp_format_version BEFORE
    // calling Backtest_RunFullValidation. When ran_held_out=1 + path non-empty,
    // RunFullValidation calls stamp_write_for_model and populates the result
    // fields below. Empty path = auto-stamp disabled (current behavior).
    char auto_stamp_path[512];             // request: full path to .bin file (empty = skip)
    char auto_stamp_secret[128];           // request: HMAC secret (empty = devmode placeholder)
    int  auto_stamp_format_version;        // request: 0 = use MODEL_FORMAT_VERSION
    int  auto_stamp_attempted;             // result: 1 if stamping fired (path was set)
    int  auto_stamp_ok;                    // result: 1 if stamp written successfully
    char auto_stamp_error[256];            // result: failure reason (if attempted && !ok)
    char auto_stamp_path_written[520];     // result: path of written stamp file
    // v5.11.41 — caller-populated request fields for per-horizon stamp body
    // forensics. label_lookahead_ticks/tp_pct/sl_pct live in BacktestRunConfig,
    // not in ControllerConfig (which is what RFV reads via data->config_used).
    // So multi-horizon worker (and single-horizon RFV button) populate these
    // BEFORE calling Backtest_RunFullValidation; RFV reads them when building
    // StampInferenceCfgInputs. Zero req_label_lookahead_ticks = skip emit
    // (legacy behavior preserved for callers that don't care). Closes
    // /parity-check 2026-05-07-stamp CRITICAL-1.
    int       req_label_lookahead_ticks;
    double    req_label_tp_pct;
    double    req_label_sl_pct;
    // v5.14.2.E.2.B — model-architectural fields for stamp body migration.
    // Populated by caller (Train Model / Train Multi-Horizon worker) BEFORE
    // calling Backtest_RunFullValidation. Zero-default = skip emit (legacy
    // behavior preserved for callers that don't care; engine falls back to
    // expected.cfg sidecar via VerifyExpected).
    int       req_num_outputs;       // 1=binary/regression, ≥2=multiclass
    char      req_role[16];          // "buy_signal" | "barrier" | "regime" | "exit"

    // v5.15.3.B.2 — Multi-horizon grid identification (PARITY-021 close).
    // Caller populates BEFORE calling Backtest_RunFullValidation. Single-
    // horizon callers leave at defaults (count=1, idx=0, horizon_count=1).
    // Multi-horizon worker (mh_run_one_horizon_fv) sets per-horizon values.
    // RFV reads these into StampArgs.grid_member_count/idx/horizon_count and
    // emits via Stamp_AssembleAndEmit — fields previously declared on the
    // stamp body schema (FOREACH_STAMP_BOUND_MODEL_CONST_PRE_CFG) but no
    // production caller populated them. Boot log warning "4/4 handles
    // missing grid_member_count" closes via this plumb-through.
    //
    // Appended at END of struct to preserve offsetof of all existing fields
    // (in-memory layout discipline; not the same as stamp wire format).
    int       req_grid_member_count = 1;
    int       req_grid_member_idx   = 0;
    int       req_horizon_count     = 1;
};

// Forward declaration — Backtest_RunWalkForward is defined further down in
// this header (the implementation is large enough to live near the bottom).
// Backtest_RunFullValidation calls it on a sliced view of BacktestResults.
// v5.10.0a.D — optional cfg_override param. When non-null, WF reads
// XGBoost hyperparams (xgb_subsample / xgb_colsample_bytree /
// xgb_min_child_weight / xgb_seed / xgb_eval_nthread) from override
// instead of data->config_used. Used by Backtest_RunHyperparamTrainSweep
// to vary hyperparams per sweep cell without copying the entire
// BacktestResults struct. Default-NULL preserves pre-v5.10.0a.D
// behavior bytewise.
static inline void Backtest_RunWalkForward(WalkForwardResults *wf,
                                            const BacktestResults *data,
                                            int n_splits, int horizon_ticks,
                                            int buffer_ticks, int min_train_samples,
                                            volatile int *progress_pct,
                                            volatile int *cancel_flag,
                                            int label_type,
                                            const ControllerConfig<BACKTEST_FP> *cfg_override = nullptr);

static inline void Backtest_RunFullValidation(FullValidationResults *out,
                                                const BacktestResults *data,
                                                const HeldOutSplit *split,
                                                int n_splits, int horizon,
                                                int buffer, int min_train,
                                                volatile int *progress,
                                                volatile int *cancel,
                                                int label_type,
                                                float gap_threshold) {
    // v5.11.49 — preserve caller-set REQUEST fields across the memset.
    // The pre-fix `memset(out, 0, sizeof(*out))` wiped auto_stamp_path /
    // auto_stamp_secret / auto_stamp_format_version / req_label_* that
    // the caller set BEFORE calling RFV — so the gate at line ~1100
    // (`if (out->ran_held_out && out->auto_stamp_path[0] != '\0')`)
    // ALWAYS saw an empty path → auto-stamp NEVER fired via this path.
    // Bug present since Phase 7prep c2 (commit 99ac494). Operators
    // worked around via manual tools/stamp_model.sh; deferred-items.md
    // had this as "auto-stamp internal copy failure".
    char saved_auto_stamp_path[512];
    char saved_auto_stamp_secret[128];
    int  saved_auto_stamp_format_version = out->auto_stamp_format_version;
    int  saved_req_label_lookahead_ticks = out->req_label_lookahead_ticks;
    double saved_req_label_tp_pct = out->req_label_tp_pct;
    double saved_req_label_sl_pct = out->req_label_sl_pct;
    memcpy(saved_auto_stamp_path,   out->auto_stamp_path,   sizeof(saved_auto_stamp_path));
    memcpy(saved_auto_stamp_secret, out->auto_stamp_secret, sizeof(saved_auto_stamp_secret));
    memset(out, 0, sizeof(*out));
    memcpy(out->auto_stamp_path,   saved_auto_stamp_path,   sizeof(out->auto_stamp_path));
    memcpy(out->auto_stamp_secret, saved_auto_stamp_secret, sizeof(out->auto_stamp_secret));
    out->auto_stamp_format_version = saved_auto_stamp_format_version;
    out->req_label_lookahead_ticks = saved_req_label_lookahead_ticks;
    out->req_label_tp_pct = saved_req_label_tp_pct;
    out->req_label_sl_pct = saved_req_label_sl_pct;
    out->gap_threshold = gap_threshold;

    // Refuse if split is locked (caller MUST unlock with token first)
    if (!split || split->locked) {
        fprintf(stderr, "[FULLVALIDATION] split is locked or null — refusing to run.\n"
                        "                 call HeldOutSplit_Unlock(split, token) first.\n");
        return;
    }
    if (!data || data->sample_count <= 0 || split->trainval_end_idx <= 0) {
        fprintf(stderr, "[FULLVALIDATION] no samples to run on (split shape invalid)\n");
        return;
    }

    // Sliced view: WF sees ONLY the train+val portion. Shallow copy of
    // BacktestResults — points at the same heap buffers but reports a
    // smaller sample_count, so Backtest_RunWalkForward never reads the
    // held-out region. No data is copied; the slice is just a count cap.
    BacktestResults slice = *data;
    slice.sample_count = split->trainval_end_idx;

    // Run walk-forward CV on the slice
    Backtest_RunWalkForward(&out->walkforward, &slice, n_splits, horizon,
                             buffer, min_train, progress, cancel, label_type);
    out->label_kind = out->walkforward.label_kind;
    memcpy(out->fingerprint, out->walkforward.fingerprint, sizeof(out->fingerprint));

    // Held-out training + eval (v5.3.0 Phase A — was stubbed in 7prep).
    // Trains one model on [0, trainval_end) using the same hyperparameters
    // as a WF fold, evaluates on [trainval_end, sample_count). Honors the
    // existing cancel_flag plumbing — early cancellation leaves
    // ran_held_out=0 so the gap stays at the degenerate baseline.
    HeldOutTrainEvalResult he = HeldOutSplit_TrainEval(data, split, label_type, cancel);
    out->ran_held_out         = he.ok;
    out->held_out_count       = he.eval_count;
    out->held_out_metric      = he.metric;
    out->held_out_mse         = he.mse;
    out->held_out_correlation = he.correlation;

    // v5.3.2 Phase C — auto-stamp hook. Caller passes auto_stamp_path +
    // auto_stamp_secret + auto_stamp_format_version through the
    // FullValidationResults's auto_stamp_* fields BEFORE calling. When
    // ran_held_out=1 AND auto_stamp_path is non-empty AND auto_stamp_secret
    // is set, fire stamp_write_for_model with the metrics just computed.
    // Keeps RunFullValidation's signature stable; gates everything on
    // the result struct's pre-populated request fields.
    out->auto_stamp_attempted = 0;
    out->auto_stamp_ok        = 0;
    out->auto_stamp_error[0]  = '\0';
    out->auto_stamp_path_written[0] = '\0';
    if (out->ran_held_out && out->auto_stamp_path[0] != '\0') {
        // v5.15.3.A — Stamp emit chain refactored to use Stamp_AssembleAndEmit
        // canonical helper. Replaces ~180 LOC of manual StampInferenceCfgInputs
        // assembly with StampArgs setup + helper call. Helper internally walks
        // STAMP_CFG_AUTOPOPULATE (cfg-bound fields) + manually populates per-
        // call model-const fields from StampArgs. Closes PARITY-020 + -021
        // structurally (any caller using the helper automatically gets all
        // stamp-bound cfg fields + grid_member identification).
        //
        // Byte-equivalence preserved: helper walks the same FOREACH_STAMP_BOUND_*
        // registries in the same canonical order; only refactor is HOW inf gets
        // populated. NEW emit: grid_member_count + grid_member_idx (always
        // emit; defaults 1/0 for single-horizon — additive change per Surface
        // G forward-compat; no MODEL_FORMAT_VERSION bump).
        tt::StampArgs<BACKTEST_FP> args;
        args.format_version = out->auto_stamp_format_version > 0
                            ? out->auto_stamp_format_version
                            : MODEL_FORMAT_VERSION;
        // wf_metric pick per label kind (matches engine's gap math)
        args.wf_metric = LabelType_IsRegression(label_type)
            ? (double)out->walkforward.mean_val_correlation
            : (double)out->walkforward.mean_val_accuracy;
        args.held_out_metric = (double)out->held_out_metric;
        args.gap_threshold   = (double)gap_threshold;
        args.label_kind      = label_type;

        // XGBoost hyperparams: RFV uses Defaults+cfg override (operator-tunable
        // subset). Pull from config_used.
        {
            tt::XGBHyperparams hp = tt::XGBHyperparams_Defaults();
            hp.subsample        = FPN_ToDouble(data->config_used.xgb_subsample);
            hp.colsample_bytree = FPN_ToDouble(data->config_used.xgb_colsample_bytree);
            hp.min_child_weight = data->config_used.xgb_min_child_weight;
            hp.seed             = data->config_used.xgb_seed;
            args.snap_max_depth        = hp.max_depth;
            args.snap_learning_rate    = (double)hp.learning_rate;
            args.snap_n_estimators     = hp.n_estimators;
            args.snap_subsample        = hp.subsample;
            args.snap_colsample_bytree = hp.colsample_bytree;
            args.snap_min_child_weight = hp.min_child_weight;
            args.snap_seed             = hp.seed;
            args.snap_tree_method      = data->config_used.xgb_tree_method;
        }
        args.snap_train_nthread = data->config_used.xgb_train_nthread > 0
                                ? data->config_used.xgb_train_nthread : 1;

        // Per-horizon label params (multi-horizon worker sets req_label_*;
        // single-horizon RFV button leaves at 0).
        args.horizon_ticks  = out->req_label_lookahead_ticks;
        args.horizon_tp_pct = out->req_label_tp_pct;
        args.horizon_sl_pct = out->req_label_sl_pct;

        // PARITY-021 close — grid identification (mh_run_one_horizon_fv
        // populates req_grid_*; single-horizon callers leave at defaults).
        args.grid_member_count = out->req_grid_member_count;
        args.grid_member_idx   = out->req_grid_member_idx;
        args.horizon_count     = out->req_horizon_count;

        // Architectural fields (training-time identity)
        args.req_num_outputs = out->req_num_outputs;
        args.req_role        = out->req_role;

        // v5.10.0 Item A — stamp_emit phase timer (kept; wraps helper call).
        uint64_t stamp_start_ns = tt::PhaseTimer_NowNs();
        StampWriteResult sr = tt::Stamp_AssembleAndEmit<BACKTEST_FP>(
            out->auto_stamp_path,
            out->auto_stamp_secret,
            data->config_used,
            args);
        tt::PhaseTimer_Global().stamp_emit_ns +=
            tt::PhaseTimer_NowNs() - stamp_start_ns;
        tt::PhaseTimer_Global().populated = 1;
        out->auto_stamp_attempted = 1;
        out->auto_stamp_ok = sr.ok;
        if (sr.ok) {
            size_t n = strlen(sr.stamp_path);
            if (n >= sizeof(out->auto_stamp_path_written))
                n = sizeof(out->auto_stamp_path_written) - 1;
            memcpy(out->auto_stamp_path_written, sr.stamp_path, n);
            out->auto_stamp_path_written[n] = '\0';
            fprintf(stderr, "[autostamp] wrote %s\n", sr.stamp_path);
        } else {
            size_t n = strlen(sr.error);
            if (n >= sizeof(out->auto_stamp_error))
                n = sizeof(out->auto_stamp_error) - 1;
            memcpy(out->auto_stamp_error, sr.error, n);
            out->auto_stamp_error[n] = '\0';
            fprintf(stderr, "[autostamp] FAIL: %s\n", sr.error);
        }
    }

    // Generalization gap: |WF mean - held_out|, label-kind-aware. With
    // ran_held_out=0 the gap is just the WF mean (degenerate but consistent).
    // When Phase 7 finalize ships, real held-out metrics will populate.
    if (LabelType_IsRegression(label_type)) {
        out->wf_to_held_out_gap = (float)fabs(
            (double)out->walkforward.mean_val_correlation - (double)out->held_out_correlation);
    } else {
        out->wf_to_held_out_gap = (float)fabs(
            (double)out->walkforward.mean_val_accuracy - (double)out->held_out_metric);
    }
    // gap_acceptable only meaningful when held-out actually ran. Default 0
    // when stubbed — signals "not yet validated" rather than "validated OK".
    out->gap_acceptable = (out->ran_held_out && out->wf_to_held_out_gap < gap_threshold) ? 1 : 0;

    // v5.10.0 Item A — extended phase summary at end of full pipeline.
    // Total = parse + fan_out_hot + feature_collect + label_compute
    //       + wf_eval + held_out_eval + stamp_emit
    // (xgboost_train is nested INSIDE wf_eval + held_out_eval; not added
    //  to total to avoid double-count.)
    if (tt::PhaseTimer_Global().populated) {
        tt::PhaseTimer_Global().total_ns =
            tt::PhaseTimer_Global().parse_ns
          + tt::PhaseTimer_Global().fan_out_hot_ns
          + tt::PhaseTimer_Global().feature_collect_ns
          + tt::PhaseTimer_Global().label_compute_ns
          + tt::PhaseTimer_Global().wf_eval_ns
          + tt::PhaseTimer_Global().held_out_eval_ns
          + tt::PhaseTimer_Global().stamp_emit_ns;
        tt::PhaseTimer_Summary(&tt::PhaseTimer_Global(), stderr);
    }
}

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

// v5.9.4a — "always-predict-best" baseline accuracy for a label distribution.
//   - Binary (K=2): max(0.5, max_class_freq) — uniform random OR majority
//   - K-class (K>=3): max(1.0/K, max_class_freq) — uniform random OR majority
//   - Regression: caller should not call this (use Pearson r diagnosis instead)
//
// Inputs:
//   num_classes — from label_table[].num_classes promoted to >=2 for binary,
//                 OR wf->num_classes / snap->num_classes for multiclass
//   class_counts — optional; if NULL or sample_count<=0, returns uniform 1/K
//   sample_count — total sample count
//
// Used by Training panel WF diagnosis (no-edge / marginal / real-edge bands)
// + Past Runs val accuracy color thresholds. Single-source-of-truth so both
// sites can't drift. Pre-v5.9.4a, both sites had hardcoded binary thresholds
// (0.52 / 0.55) which mis-diagnosed multiclass — caught in 2026-05-02 paper
// test where 3-class with 47% majority showed val=46.7% but binary heuristic
// said "no edge — below 50%" (technically right answer, wrong reasoning).
static inline float multiclass_baseline_accuracy(int num_classes,
                                                   const int* class_counts,
                                                   int sample_count) {
    if (num_classes < 2) num_classes = 2;  // binary floor
    float uniform = 1.0f / (float)num_classes;
    float majority = uniform;
    if (class_counts && sample_count > 0) {
        int K = num_classes > 16 ? 16 : num_classes;
        for (int k = 0; k < K; ++k) {
            float p = (float)class_counts[k] / (float)sample_count;
            if (p > majority) majority = p;
        }
    }
    return majority;
}

// multiclass accuracy: predictions is count × num_classes flat array (softmax probs).
// argmax over each row, compare to integer truth (rounded from label float).
static inline float WalkForward_ComputeMulticlassAccuracy(const float *predictions,
                                                            const float *labels,
                                                            int count, int num_classes) {
    if (count <= 0 || num_classes < 2) return 0.0f;
    int correct = 0;
    for (int i = 0; i < count; i++) {
        int best = 0;
        float best_p = predictions[i * num_classes];
        for (int k = 1; k < num_classes; k++) {
            float p = predictions[i * num_classes + k];
            if (p > best_p) { best_p = p; best = k; }
        }
        int truth = (int)(labels[i] + 0.5f);
        if (best == truth) correct++;
    }
    return (float)correct / count;
}

// regression: mean squared error. Lower = better. Sensitive to outliers.
static inline float WalkForward_ComputeMSE(const float *predictions, const float *labels,
                                             int count) {
    if (count <= 0) return 0.0f;
    double sum_sq = 0.0;
    for (int i = 0; i < count; i++) {
        double d = (double)predictions[i] - (double)labels[i];
        sum_sq += d * d;
    }
    return (float)(sum_sq / count);
}

// regression: Pearson correlation between predictions and labels in [-1, +1].
// 0 ≈ no signal. >0.05 ≈ weak but real signal. >0.2 ≈ meaningful for tick-scale.
// This is the metric that matters for "did the model learn anything" in
// regression — MSE alone can be misleading (a model predicting always-zero
// gets low MSE on small-magnitude targets while having zero predictive power).
static inline float WalkForward_ComputeCorrelation(const float *predictions,
                                                      const float *labels, int count) {
    if (count < 2) return 0.0f;
    double mean_p = 0.0, mean_l = 0.0;
    for (int i = 0; i < count; i++) {
        mean_p += predictions[i];
        mean_l += labels[i];
    }
    mean_p /= count;
    mean_l /= count;

    double sum_pl = 0.0, sum_pp = 0.0, sum_ll = 0.0;
    for (int i = 0; i < count; i++) {
        double dp = (double)predictions[i] - mean_p;
        double dl = (double)labels[i] - mean_l;
        sum_pl += dp * dl;
        sum_pp += dp * dp;
        sum_ll += dl * dl;
    }
    double denom = sqrt(sum_pp * sum_ll);
    if (denom <= 0.0) return 0.0f;
    return (float)(sum_pl / denom);
}

static inline void Backtest_RunWalkForward(WalkForwardResults *wf,
                                            const BacktestResults *data,
                                            int n_splits, int horizon_ticks,
                                            int buffer_ticks, int min_train_samples,
                                            volatile int *progress_pct,
                                            volatile int *cancel_flag,
                                            int label_type,
                                            const ControllerConfig<BACKTEST_FP> *cfg_override) {
    // v5.10.0 Item A — wf_eval phase timer (wraps full body).
    uint64_t wf_start_ns = tt::PhaseTimer_NowNs();
    memset(wf, 0, sizeof(*wf));
    *progress_pct = 0;
    // v5.10.0a.D — resolve effective cfg. Override path used by
    // Backtest_RunHyperparamTrainSweep to vary xgb_* per sweep cell;
    // default-NULL falls back to data->config_used (pre-v5.10.0a.D
    // behavior, bytewise-equivalent for non-sweep callers).
    const ControllerConfig<BACKTEST_FP>& eff_cfg =
        cfg_override ? *cfg_override : data->config_used;

    // label-type-aware: pick objective + metric kind once, branch downstream.
    // Defaults to binary if caller didn't specify (backward compat).
    int num_classes_lt = LabelType_NumClasses(label_type);
    int is_regression  = LabelType_IsRegression(label_type);
    int is_multiclass  = LabelType_IsMulticlass(label_type);
    wf->label_kind   = num_classes_lt;
    wf->num_classes  = num_classes_lt;

#ifndef USE_XGBOOST
    fprintf(stderr, "[walkforward] XGBoost not compiled in — cannot train. "
            "rebuild with -DUSE_XGBOOST=ON\n");
    *progress_pct = 100;
    tt::PhaseTimer_Global().wf_eval_ns +=
        tt::PhaseTimer_NowNs() - wf_start_ns;
    tt::PhaseTimer_Global().populated = 1;
    return;
#else
    if (data->sample_count < 100) {
        fprintf(stderr, "[walkforward] only %d samples — need at least 100 for walk-forward\n",
                data->sample_count);
        *progress_pct = 100;
        tt::PhaseTimer_Global().wf_eval_ns +=
            tt::PhaseTimer_NowNs() - wf_start_ns;
        tt::PhaseTimer_Global().populated = 1;
        return;
    }

    struct timeval t_start, t_end;
    gettimeofday(&t_start, NULL);

    // generate purged folds
    if (n_splits < 2) n_splits = 5;
    if (buffer_ticks <= 0) buffer_ticks = PURGE_BUFFER_DEFAULT;
    if (min_train_samples < 50) min_train_samples = 50;
    wf->num_folds = n_splits;

    // pre-compact: for binary labels, extract non-neutral samples (label != 0.5)
    // because barrier labels produce ~97% neutrals and splitting over the full
    // sample range puts later folds' test sets in the all-neutral tail.
    //
    // For regression, every sample is a valid label (0.5 is a legitimate value,
    // not a sentinel). Don't filter — copy all samples through.
    // For multiclass, labels are integer class ids 0..K-1, never 0.5 — no filter
    // is needed but harmless. Skip the filter for clarity + correctness.
    int filter_neutrals = LabelType_IsBinary(label_type);
    int filter_nan = LabelType_IsMulticlass(label_type);  // v5.9.1: NAN-marked = NaN-label-dropped

    int nn_count = 0;
    if (filter_neutrals) {
        for (int i = 0; i < data->sample_count; i++) {
            if (data->labels[i] != 0.5f) nn_count++;
        }
    } else if (filter_nan) {
        for (int i = 0; i < data->sample_count; i++) {
            if (!isnan(data->labels[i])) nn_count++;
        }
    } else {
        nn_count = data->sample_count;
    }

    if (nn_count < 100) {
        fprintf(stderr, "[walkforward] only %d %s samples — need at least 100\n",
                nn_count, filter_neutrals ? "non-neutral" : "labeled");
        *progress_pct = 100;
        return;
    }

    // allocate compacted data (features + labels + original indices for purge)
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
        if (filter_neutrals && data->labels[i] == 0.5f) continue;
        if (filter_nan && isnan(data->labels[i])) continue;
        memcpy(&nn_features[j * MODEL_NUM_FEATURES],
               &data->feature_matrix[i * MODEL_NUM_FEATURES],
               MODEL_NUM_FEATURES * sizeof(float));
        nn_labels[j] = data->labels[i];
        nn_indices[j] = i;
        j++;
    }

    fprintf(stderr, "[walkforward] %s samples: %d / %d total (%.1f%%) — kind=%s\n",
            filter_neutrals ? "non-neutral" : "labeled",
            nn_count, data->sample_count, 100.0 * nn_count / data->sample_count,
            LabelType_KindName(label_type));

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

        // training params — kind-aware objective (label-type-aware metric invariant).
        // Binary classification: binary:logistic + scale_pos_weight for class balance.
        // Multiclass: multi:softprob + num_class. scale_pos_weight is binary-only;
        //   multiclass class imbalance needs per-sample weights (deferred).
        // Regression: reg:squarederror, no class-weight concept.
        if (is_regression) {
            XGBoosterSetParam(booster, "objective", "reg:squarederror");
        } else if (is_multiclass) {
            XGBoosterSetParam(booster, "objective", "multi:softprob");
            char nc_s[8]; snprintf(nc_s, 8, "%d", num_classes_lt);
            XGBoosterSetParam(booster, "num_class", nc_s);
        } else {
            XGBoosterSetParam(booster, "objective", "binary:logistic");
        }
        // v5.9.5h — XGBHyperparams struct + apply helper. Single source of
        // truth shared with HeldOut training (BacktestEngine.hpp:~1592) and
        // Train Model worker (BacktestPanels.hpp). Defaults match the
        // pre-v5.9.5h hardcoded values bytewise; non-tuning operators get
        // identical training output.
        // v5.10.0 Item D — nthread reads from cfg.xgb_eval_nthread (default
        // 1; matches pre-v5.10 hardcoded behavior). Operator opts in to >1
        // by setting cfg explicitly. CFG_PARSE_INT clamps negatives to 0;
        // we coerce 0/negative to 1 here for safety.
        // v5.10.0a.D — read xgb_* from eff_cfg (override-or-data->config_used)
        // so hyperparam sweep can vary subsample / colsample / etc per cell.
        // XGBHyperparams_Defaults() returns hardcoded defaults; for sweep,
        // we copy eff_cfg's values into hp so they propagate to the booster.
        tt::XGBHyperparams hp = tt::XGBHyperparams_Defaults();
        hp.subsample          = (float)FPN_ToDouble(eff_cfg.xgb_subsample);
        hp.colsample_bytree   = (float)FPN_ToDouble(eff_cfg.xgb_colsample_bytree);
        hp.min_child_weight   = eff_cfg.xgb_min_child_weight;
        hp.seed               = eff_cfg.xgb_seed;
        if (eff_cfg.xgb_tree_method[0]) {
            strncpy(hp.tree_method, eff_cfg.xgb_tree_method, sizeof(hp.tree_method) - 1);
            hp.tree_method[sizeof(hp.tree_method) - 1] = '\0';
        }
        int eval_nthread = eff_cfg.xgb_eval_nthread > 0
                         ? eff_cfg.xgb_eval_nthread : 1;
        tt::XGBHyperparams_Apply(booster, hp, eval_nthread);
        // class balance — kind-specific.
        // Binary: scale_pos_weight = n_neg/n_pos (single param).
        // Multiclass: per-sample inverse-frequency weights via DMatrix info.
        // Regression: no class-imbalance concept.
        if (!is_regression && !is_multiclass) {
            int n_pos = 0, n_neg = 0;
            double spw = XGBoost_ComputeScalePosWeight(train_labels, n_train, &n_pos, &n_neg);
            char spw_s[24]; snprintf(spw_s, sizeof(spw_s), "%.4f", spw);
            XGBoosterSetParam(booster, "scale_pos_weight", spw_s);
            fprintf(stderr, "[walkforward] fold %d: class balance +%d / -%d → scale_pos_weight=%s\n",
                    f + 1, n_pos, n_neg, spw_s);
        } else if (is_multiclass) {
            float *mc_weights = (float *)malloc(n_train * sizeof(float));
            int   mc_counts[16] = {0};
            if (mc_weights) {
                XGBoost_ComputeMulticlassWeights(train_labels, n_train, num_classes_lt,
                                                  mc_weights, mc_counts);
                // v5.11.46 — cap per-sample weight at 5.0 to prevent
                // numerical issues during XGBoost gradient computation
                // when one class is very rare (e.g. c0=1.2% gives raw
                // weight ~27, large enough to cause gradient overflow
                // in some XGBoost versions → segfault in histogram
                // split-finding). Capping caps the importance weighting
                // for very rare classes; you lose some signal but stop
                // crashing.
                const float WEIGHT_CAP = 5.0f;
                int capped_count = 0;
                for (int wi = 0; wi < n_train; ++wi) {
                    if (mc_weights[wi] > WEIGHT_CAP) {
                        mc_weights[wi] = WEIGHT_CAP;
                        capped_count++;
                    }
                }
                XGDMatrixSetFloatInfo(dtrain, "weight", mc_weights, n_train);
                fprintf(stderr, "[walkforward] fold %d: multiclass class counts:", f + 1);
                for (int k = 0; k < num_classes_lt && k < 16; k++) {
                    fprintf(stderr, " c%d=%d (%.1f%%)", k, mc_counts[k],
                            n_train > 0 ? 100.0f * mc_counts[k] / n_train : 0.0f);
                }
                if (capped_count > 0) {
                    fprintf(stderr, " — per-sample weights applied (capped %d at %.1f)\n",
                            capped_count, WEIGHT_CAP);
                } else {
                    fprintf(stderr, " — per-sample weights applied\n");
                }
                free(mc_weights);
            }
        }
        // v5.11.46 — bisection markers for fold 2 segfault diagnosis.
        // If crash is in XGBoosterUpdateOneIter, we'll see "[WF marker]
        // fold N: pre-iter R" before crash. If pre-predict, see "[WF
        // marker] fold N: pre-predict". Helps narrow without ASAN.
        fprintf(stderr, "[WF marker] fold %d: pre-train-loop (booster=%p, dtrain=%p, n_train=%d)\n",
                f + 1, (void*)booster, (void*)dtrain, n_train);
        fflush(stderr);

        // train (no early stopping yet — full n_rounds always)
        int n_rounds = 200;
        // v5.10.0 Item A — xgboost_train phase timer (per-fold).
        uint64_t xgb_start_ns = tt::PhaseTimer_NowNs();
        // v5.11.31/.32 — track first failing iter + last successful iter.
        // Categorization: train-iter failure with XGB err string is a
        // WARN (always-on observability — operator wants to see this
        // without rebuilding); the per-fold "trained N/N successfully"
        // banner is INFO (cheap, only 1 line per fold).
        int last_iter_ret = 0;
        int last_iter_idx = -1;
        for (int r = 0; r < n_rounds; r++) {
            // v5.11.46 — log every 10 iters to bisect crash location
            if (r == 0 || r % 10 == 0) {
                fprintf(stderr, "[WF marker] fold %d: pre-iter %d\n", f + 1, r);
                fflush(stderr);
            }
            ret = XGBoosterUpdateOneIter(booster, r, dtrain);
            last_iter_ret = ret;
            last_iter_idx = r;
            if (ret != 0) {
                tt::Health_Log(tt::HEALTH_WARN, "wf-xgb", f + 1,
                    "UpdateOneIter ret=%d at iter %d — XGB err: %s",
                    ret, r, XGBGetLastError() ? XGBGetLastError() : "(null)");
                break;
            }
        }
        fprintf(stderr, "[WF marker] fold %d: train-loop complete (last_iter=%d)\n",
                f + 1, last_iter_idx);
        fflush(stderr);
        if (last_iter_ret == 0 && tt::Health_LogEnabled(tt::HEALTH_INFO)) {
            tt::Health_Log(tt::HEALTH_INFO, "wf-xgb", f + 1,
                "trained %d/%d iters successfully",
                last_iter_idx + 1, n_rounds);
        }
        tt::PhaseTimer_Global().xgboost_train_ns +=
            tt::PhaseTimer_NowNs() - xgb_start_ns;

        // predict on train + test, compute kind-appropriate metric
        {
            bst_ulong out_len_tr = 0, out_len_te = 0;
            const float *pred_tr = nullptr, *pred_te = nullptr;
            // v5.11.46 — bisection markers
            fprintf(stderr, "[WF marker] fold %d: pre-predict-train\n", f + 1);
            fflush(stderr);
            int predict_tr_ret = XGBoosterPredict(booster, dtrain, 0, 0, 0, &out_len_tr, &pred_tr);
            fprintf(stderr, "[WF marker] fold %d: post-predict-train (ret=%d, out_len=%lu)\n",
                    f + 1, predict_tr_ret, (unsigned long)out_len_tr);
            fflush(stderr);
            // v5.11.58 — XGBoost C API: XGBoosterPredict's `out_result` is
            // a pointer into Booster-owned memory (HostDeviceVector reused
            // across calls). A second Predict realloc-and-fills the same
            // buffer; at large sample counts (1.5M+ × num_classes), the
            // realloc moves the address and INVALIDATES the previous
            // pointer. Symptom: WF segfaults after post-predict-test marker
            // when train_accuracy computation reads stale pred_tr. Latent
            // since multiclass support; only triggered at scale (operator
            // hit it on 2-year × 3-horizon × multiclass run).
            //
            // Fix: snapshot pred_tr to caller-owned heap before second
            // Predict invalidates it. Free at end of predict block.
            float *pred_tr_copy = nullptr;
            if (predict_tr_ret == 0 && pred_tr != nullptr && out_len_tr > 0) {
                pred_tr_copy = (float*)malloc((size_t)out_len_tr * sizeof(float));
                if (pred_tr_copy) {
                    memcpy(pred_tr_copy, pred_tr, (size_t)out_len_tr * sizeof(float));
                    pred_tr = pred_tr_copy;  // downstream uses stable copy
                } else {
                    tt::Health_Log(tt::HEALTH_WARN, "wf-xgb", f + 1,
                        "pred_tr snapshot malloc failed (out_len=%lu) — "
                        "skipping train metrics this fold",
                        (unsigned long)out_len_tr);
                    pred_tr = nullptr;
                    predict_tr_ret = -1;  // forces pred_tr_ok=0 below
                }
            }
            int predict_te_ret = XGBoosterPredict(booster, dtest,  0, 0, 0, &out_len_te, &pred_te);
            fprintf(stderr, "[WF marker] fold %d: post-predict-test (ret=%d, out_len=%lu)\n",
                    f + 1, predict_te_ret, (unsigned long)out_len_te);
            fflush(stderr);
            int pred_tr_ok = (predict_tr_ret == 0);
            int pred_te_ok = (predict_te_ret == 0);
            // v5.11.32 — predict failure → WARN (always-on; this is the
            // class of bug that left WF accuracy at default 0.0 silently
            // pre-fix). XGBGetLastError() string is the smoking gun.
            if (!pred_tr_ok) {
                tt::Health_Log(tt::HEALTH_WARN, "wf-xgb", f + 1,
                    "Predict(train) ret=%d — XGB err: %s",
                    predict_tr_ret,
                    XGBGetLastError() ? XGBGetLastError() : "(null)");
            }
            if (!pred_te_ok) {
                tt::Health_Log(tt::HEALTH_WARN, "wf-xgb", f + 1,
                    "Predict(test) ret=%d — XGB err: %s",
                    predict_te_ret,
                    XGBGetLastError() ? XGBGetLastError() : "(null)");
            }

            if (is_regression) {
                if (pred_tr_ok && (int)out_len_tr == n_train) {
                    fr->train_mse         = WalkForward_ComputeMSE(pred_tr, train_labels, n_train);
                    fr->train_correlation = WalkForward_ComputeCorrelation(pred_tr, train_labels, n_train);
                }
                if (pred_te_ok && (int)out_len_te == n_test) {
                    fr->val_mse         = WalkForward_ComputeMSE(pred_te, test_labels, n_test);
                    fr->val_correlation = WalkForward_ComputeCorrelation(pred_te, test_labels, n_test);
                }
            } else if (is_multiclass) {
                int K = num_classes_lt;
                // v5.11.32 — observability discipline (suite-side, no
                // engine-slow-path latency cost). Categorization:
                //   * shape-mismatch SKIP → WARN (always-on; this is
                //     the silent-bug class that masked the WF
                //     regression for hours).
                //   * per-sample (argmax→label) sampling → DEBUG via
                //     LOG_DEBUG_ENGINE (compile-time gated; off in
                //     release; rebuild with ./build.sh debug to enable
                //     when reproducing a tricky bug).
                //   * post-compute accuracy summary → INFO (cheap,
                //     1 line per fold; aggregates the per-fold result).
                if (pred_tr_ok && (int)out_len_tr == n_train * K) {
                    fr->train_accuracy = WalkForward_ComputeMulticlassAccuracy(
                        pred_tr, train_labels, n_train, K);
#ifdef FOXML_DEBUG_LOGS
                    {
                        // Sample first 5 (argmax, label) pairs.
                        char sample_buf[128] = {0};
                        size_t off = 0;
                        int show = n_train < 5 ? n_train : 5;
                        for (int i = 0; i < show; i++) {
                            int best = 0;
                            float best_p = pred_tr[i * K];
                            for (int k = 1; k < K; k++) {
                                float p = pred_tr[i * K + k];
                                if (p > best_p) { best_p = p; best = k; }
                            }
                            int truth = (int)(train_labels[i] + 0.5f);
                            int wrote = snprintf(sample_buf + off,
                                                  sizeof(sample_buf) - off,
                                                  " (%d->%d)", best, truth);
                            if (wrote > 0) off += wrote;
                            else break;
                        }
                        LOG_DEBUG_ENGINE("wf-fold-train", f + 1,
                            "argmax/label samples:%s acc=%.4f",
                            sample_buf, fr->train_accuracy);
                    }
#endif
                    if (tt::Health_LogEnabled(tt::HEALTH_INFO)) {
                        tt::Health_Log(tt::HEALTH_INFO, "wf-fold", f + 1,
                            "train_accuracy=%.4f n_train=%d K=%d",
                            fr->train_accuracy, n_train, K);
                    }
                } else {
                    tt::Health_Log(tt::HEALTH_WARN, "wf-fold", f + 1,
                        "train SKIP — pred_tr_ok=%d out_len_tr=%lu expected=%d "
                        "(n_train=%d K=%d) → train_accuracy stays at 0.0 default",
                        pred_tr_ok, (unsigned long)out_len_tr, n_train * K,
                        n_train, K);
                }
                if (pred_te_ok && (int)out_len_te == n_test * K) {
                    fr->val_accuracy = WalkForward_ComputeMulticlassAccuracy(
                        pred_te, test_labels, n_test, K);
#ifdef FOXML_DEBUG_LOGS
                    {
                        char sample_buf[128] = {0};
                        size_t off = 0;
                        int show = n_test < 5 ? n_test : 5;
                        for (int i = 0; i < show; i++) {
                            int best = 0;
                            float best_p = pred_te[i * K];
                            for (int k = 1; k < K; k++) {
                                float p = pred_te[i * K + k];
                                if (p > best_p) { best_p = p; best = k; }
                            }
                            int truth = (int)(test_labels[i] + 0.5f);
                            int wrote = snprintf(sample_buf + off,
                                                  sizeof(sample_buf) - off,
                                                  " (%d->%d)", best, truth);
                            if (wrote > 0) off += wrote;
                            else break;
                        }
                        LOG_DEBUG_ENGINE("wf-fold-val", f + 1,
                            "argmax/label samples:%s acc=%.4f",
                            sample_buf, fr->val_accuracy);
                    }
#endif
                    if (tt::Health_LogEnabled(tt::HEALTH_INFO)) {
                        tt::Health_Log(tt::HEALTH_INFO, "wf-fold", f + 1,
                            "val_accuracy=%.4f n_test=%d K=%d",
                            fr->val_accuracy, n_test, K);
                    }
                } else {
                    tt::Health_Log(tt::HEALTH_WARN, "wf-fold", f + 1,
                        "val SKIP — pred_te_ok=%d out_len_te=%lu expected=%d "
                        "(n_test=%d K=%d) → val_accuracy stays at 0.0 default",
                        pred_te_ok, (unsigned long)out_len_te, n_test * K,
                        n_test, K);
                }
            } else {
                if (pred_tr_ok && (int)out_len_tr == n_train) {
                    fr->train_accuracy = WalkForward_ComputeAccuracy(
                        pred_tr, train_labels, n_train, 0.5f);
                }
                if (pred_te_ok && (int)out_len_te == n_test) {
                    fr->val_accuracy = WalkForward_ComputeAccuracy(
                        pred_te, test_labels, n_test, 0.5f);
                }
            }
            // v5.11.58 — release the train-prediction snapshot
            if (pred_tr_copy) free(pred_tr_copy);
        }

        // feature importances (stability tracking hook) — zero-filled until
        // we wire XGBoost importance extraction
        memset(fr->feature_importances, 0, sizeof(fr->feature_importances));

        fr->train_samples = n_train;
        fr->test_samples  = n_test;
        fr->valid = 1;

        // overfit detection — kind-appropriate. Classification uses accuracy
        // thresholds (binary + multiclass). Regression uses Pearson correlation
        // thresholds — see OverfitDetection_CheckRegression for rationale.
        if (is_regression) {
            fr->overfit = OverfitDetection_CheckRegressionDefaults(
                fr->train_correlation, fr->val_correlation, MODEL_NUM_FEATURES);
        } else {
            fr->overfit = OverfitDetection_CheckDefaults(
                fr->train_accuracy, -1.0f, fr->val_accuracy, MODEL_NUM_FEATURES);
        }
        OverfitDetection_Print(&fr->overfit, f);
        if (fr->overfit.is_overfit) wf->overfit_count++;

        // accumulate aggregates — kind-appropriate
        if (is_regression) {
            sum_val      += fr->val_mse;          // MSE for "lower=better" aggregate
            sum_val_sq   += fr->val_mse * fr->val_mse;
            sum_train    += fr->train_mse;
            wf->mean_val_correlation   += fr->val_correlation;   // mean of fold rs
            wf->mean_train_correlation += fr->train_correlation;
        } else {
            sum_val    += fr->val_accuracy;
            sum_val_sq += fr->val_accuracy * fr->val_accuracy;
            sum_train  += fr->train_accuracy;
        }
        counted_folds++;

        // cleanup
        XGBoosterFree(booster);
        XGDMatrixFree(dtrain);
        XGDMatrixFree(dtest);

        *progress_pct = (int)(100.0 * (f + 1) / n_splits);

        // log per-fold result — kind-appropriate
        if (is_regression) {
            fprintf(stderr, "[walkforward] fold %d/%d: train_mse=%.6f val_mse=%.6f | corr train=%.4f val=%.4f\n",
                    f + 1, n_splits, fr->train_mse, fr->val_mse,
                    fr->train_correlation, fr->val_correlation);
        } else {
            fprintf(stderr, "[walkforward] fold %d/%d: train_acc=%.4f, val_acc=%.4f%s\n",
                    f + 1, n_splits, fr->train_accuracy, fr->val_accuracy,
                    fr->overfit.is_overfit ? " [OVERFIT]" : "");
        }
    }

    free(nn_features);
    free(nn_labels);
    free(nn_indices);

    // compute aggregate stats — kind-appropriate
    if (counted_folds > 0) {
        if (is_regression) {
            wf->mean_val_mse           = sum_val   / counted_folds;
            wf->mean_val_correlation  /= counted_folds;
            wf->mean_train_correlation /= counted_folds;
            // mean_val/train_accuracy stay 0 — display reads label_kind to pick
        } else {
            wf->mean_val_accuracy   = sum_val / counted_folds;
            wf->mean_train_accuracy = sum_train / counted_folds;
            float var = (sum_val_sq / counted_folds) - (wf->mean_val_accuracy * wf->mean_val_accuracy);
            wf->std_val_accuracy = (var > 0.0f) ? (float)sqrt((double)var) : 0.0f;
        }
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
    // v5.10.0 Item A — wf_eval normal-path accumulator. Includes nested
    // xgboost_train_ns time (which is recorded separately around each
    // XGBoosterUpdateOneIter call so PhaseTimer_Summary can show the
    // breakdown).
    tt::PhaseTimer_Global().wf_eval_ns +=
        tt::PhaseTimer_NowNs() - wf_start_ns;
    tt::PhaseTimer_Global().populated = 1;
}

//======================================================================================================
// [HELD-OUT TRAIN + EVAL — definition]
//======================================================================================================
// Forward-declared near the FullValidationResults struct so
// Backtest_RunFullValidation can call it; defined here so all the helper
// functions it uses (WalkForward_Compute*, XGBoost_Compute*) are visible.
static inline HeldOutTrainEvalResult HeldOutSplit_TrainEval(
    const BacktestResults *data,
    const HeldOutSplit *split,
    int label_type,
    volatile int *cancel_flag) {
    HeldOutTrainEvalResult r = {};

    // v5.10.0 Item A — held_out_eval phase timer.
    uint64_t ho_start_ns = tt::PhaseTimer_NowNs();
    // RAII-ish accumulate on every return path via lambda + scope_guard.
    // C++ has no defer; use a local struct dtor.
    struct PhaseGuard {
        uint64_t start;
        ~PhaseGuard() {
            tt::PhaseTimer_Global().held_out_eval_ns +=
                tt::PhaseTimer_NowNs() - start;
            tt::PhaseTimer_Global().populated = 1;
        }
    } pt_guard{ho_start_ns};

#ifndef USE_XGBOOST
    fprintf(stderr, "[heldout] XGBoost not compiled in — cannot train. "
                    "rebuild with -DUSE_XGBOOST=ON\n");
    return r;
#else
    if (!data || !split) return r;
    if (split->locked) {
        fprintf(stderr, "[heldout] split is locked — cannot evaluate. "
                        "call HeldOutSplit_Unlock(split, token) first.\n");
        return r;
    }
    if (data->sample_count <= split->trainval_end_idx) {
        fprintf(stderr, "[heldout] no held-out samples (sample_count=%d, trainval_end=%d)\n",
                data->sample_count, split->trainval_end_idx);
        return r;
    }

    int num_classes     = LabelType_NumClasses(label_type);
    int is_regression   = LabelType_IsRegression(label_type);
    int is_multiclass   = LabelType_IsMulticlass(label_type);
    int filter_neutrals = LabelType_IsBinary(label_type);
    int filter_nan      = is_multiclass;  // v5.9.1: NAN-marked = NaN-label-dropped

    int n_train = 0, n_eval = 0;
    for (int i = 0; i < data->sample_count; ++i) {
        if (filter_neutrals && data->labels[i] == 0.5f) continue;
        if (filter_nan && isnan(data->labels[i])) continue;
        if (i < split->trainval_end_idx) n_train++;
        else                              n_eval++;
    }

    if (n_train < 100 || n_eval < 10) {
        fprintf(stderr, "[heldout] insufficient samples after filter: train=%d eval=%d\n",
                n_train, n_eval);
        return r;
    }

    float *train_features = (float*)malloc((size_t)n_train * MODEL_NUM_FEATURES * sizeof(float));
    float *train_labels   = (float*)malloc((size_t)n_train * sizeof(float));
    float *eval_features  = (float*)malloc((size_t)n_eval  * MODEL_NUM_FEATURES * sizeof(float));
    float *eval_labels    = (float*)malloc((size_t)n_eval  * sizeof(float));

    DMatrixHandle dtrain = NULL, deval = NULL;
    BoosterHandle booster = NULL;
    float *mc_weights = NULL;

    do {
        if (!train_features || !train_labels || !eval_features || !eval_labels) {
            fprintf(stderr, "[heldout] failed to allocate compaction buffers\n");
            break;
        }

        int ti = 0, ei = 0;
        for (int i = 0; i < data->sample_count; ++i) {
            if (filter_neutrals && data->labels[i] == 0.5f) continue;
            if (filter_nan && isnan(data->labels[i])) continue;
            if (i < split->trainval_end_idx) {
                memcpy(&train_features[ti * MODEL_NUM_FEATURES],
                       &data->feature_matrix[i * MODEL_NUM_FEATURES],
                       MODEL_NUM_FEATURES * sizeof(float));
                train_labels[ti++] = data->labels[i];
            } else {
                memcpy(&eval_features[ei * MODEL_NUM_FEATURES],
                       &data->feature_matrix[i * MODEL_NUM_FEATURES],
                       MODEL_NUM_FEATURES * sizeof(float));
                eval_labels[ei++] = data->labels[i];
            }
        }

        fprintf(stderr, "[heldout] training on %d samples, evaluating on %d (kind=%s)\n",
                n_train, n_eval, LabelType_KindName(label_type));

        if (XGDMatrixCreateFromMat(train_features, n_train, MODEL_NUM_FEATURES, -1.0f, &dtrain) != 0) {
            fprintf(stderr, "[heldout] failed to create train DMatrix: %s\n", XGBGetLastError());
            break;
        }
        XGDMatrixSetFloatInfo(dtrain, "label", train_labels, n_train);

        if (XGDMatrixCreateFromMat(eval_features, n_eval, MODEL_NUM_FEATURES, -1.0f, &deval) != 0) {
            fprintf(stderr, "[heldout] failed to create eval DMatrix: %s\n", XGBGetLastError());
            break;
        }
        XGDMatrixSetFloatInfo(deval, "label", eval_labels, n_eval);

        if (XGBoosterCreate(&dtrain, 1, &booster) != 0) {
            fprintf(stderr, "[heldout] failed to create booster: %s\n", XGBGetLastError());
            break;
        }

        // Hyperparameters MUST match Backtest_RunWalkForward's per-fold setup
        // exactly. Held-out is the deployment proxy; it should train the same
        // model class WF measured. Diverging here = drift between gap
        // measurement and reality.
        if (is_regression) {
            XGBoosterSetParam(booster, "objective", "reg:squarederror");
        } else if (is_multiclass) {
            XGBoosterSetParam(booster, "objective", "multi:softprob");
            char nc_s[8]; snprintf(nc_s, sizeof(nc_s), "%d", num_classes);
            XGBoosterSetParam(booster, "num_class", nc_s);
        } else {
            XGBoosterSetParam(booster, "objective", "binary:logistic");
        }
        // v5.9.5h — XGBHyperparams struct + apply helper. Mirrors WF site
        // above for train-serve parity.
        // v5.10.0 Item D — nthread reads from cfg.xgb_eval_nthread (default
        // 1; matches pre-v5.10 hardcoded behavior). Held-out is the
        // canonical-validation pass — multi-thread breaks bytewise
        // reproducibility of held_out_metric, so default stays single-thread.
        tt::XGBHyperparams hp = tt::XGBHyperparams_Defaults();
        int eval_nthread = data->config_used.xgb_eval_nthread > 0
                         ? data->config_used.xgb_eval_nthread : 1;
        tt::XGBHyperparams_Apply(booster, hp, eval_nthread);

        if (!is_regression && !is_multiclass) {
            int n_pos = 0, n_neg = 0;
            double spw = XGBoost_ComputeScalePosWeight(train_labels, n_train, &n_pos, &n_neg);
            char spw_s[24]; snprintf(spw_s, sizeof(spw_s), "%.4f", spw);
            XGBoosterSetParam(booster, "scale_pos_weight", spw_s);
        } else if (is_multiclass) {
            mc_weights = (float*)malloc((size_t)n_train * sizeof(float));
            int mc_counts[16] = {0};
            if (mc_weights) {
                XGBoost_ComputeMulticlassWeights(train_labels, n_train, num_classes,
                                                  mc_weights, mc_counts);
                XGDMatrixSetFloatInfo(dtrain, "weight", mc_weights, n_train);
            }
        }

        int n_rounds = 200;
        int train_aborted = 0;
        // v5.10.0 Item A — xgboost_train phase timer (held-out training).
        uint64_t xgb_ho_start_ns = tt::PhaseTimer_NowNs();
        for (int rr = 0; rr < n_rounds; ++rr) {
            if (cancel_flag && *cancel_flag) {
                train_aborted = 1;
                break;
            }
            if (XGBoosterUpdateOneIter(booster, rr, dtrain) != 0) break;
        }
        tt::PhaseTimer_Global().xgboost_train_ns +=
            tt::PhaseTimer_NowNs() - xgb_ho_start_ns;
        if (train_aborted) {
            fprintf(stderr, "[heldout] training cancelled at round %d\n", n_rounds);
            break;
        }

        bst_ulong out_len_tr = 0, out_len_ev = 0;
        const float *pred_tr = NULL, *pred_ev = NULL;
        int pr_tr_ok = (XGBoosterPredict(booster, dtrain, 0, 0, 0, &out_len_tr, &pred_tr) == 0);
        // v5.11.58 — see WF predict block for full rationale. XGBoost reuses
        // its prediction buffer across Predict calls; second Predict
        // invalidates the first pointer at large sample counts. Snapshot
        // pred_tr to caller heap before second Predict.
        float *pred_tr_copy = NULL;
        if (pr_tr_ok && pred_tr != NULL && out_len_tr > 0) {
            pred_tr_copy = (float*)malloc((size_t)out_len_tr * sizeof(float));
            if (pred_tr_copy) {
                memcpy(pred_tr_copy, pred_tr, (size_t)out_len_tr * sizeof(float));
                pred_tr = pred_tr_copy;
            } else {
                fprintf(stderr, "[heldout] pred_tr snapshot malloc failed (out_len=%lu) — "
                                "skipping train metrics\n",
                        (unsigned long)out_len_tr);
                pred_tr = NULL;
                pr_tr_ok = 0;
            }
        }
        int pr_ev_ok = (XGBoosterPredict(booster, deval,  0, 0, 0, &out_len_ev, &pred_ev) == 0);

        if (is_regression) {
            if (pr_tr_ok && (int)out_len_tr == n_train) {
                r.train_metric = WalkForward_ComputeCorrelation(pred_tr, train_labels, n_train);
            }
            if (pr_ev_ok && (int)out_len_ev == n_eval) {
                r.metric      = WalkForward_ComputeCorrelation(pred_ev, eval_labels, n_eval);
                r.mse         = WalkForward_ComputeMSE(pred_ev, eval_labels, n_eval);
                r.correlation = r.metric;
            }
        } else if (is_multiclass) {
            int K = num_classes;
            if (pr_tr_ok && (int)out_len_tr == n_train * K) {
                r.train_metric = WalkForward_ComputeMulticlassAccuracy(pred_tr, train_labels, n_train, K);
            }
            if (pr_ev_ok && (int)out_len_ev == n_eval * K) {
                r.metric = WalkForward_ComputeMulticlassAccuracy(pred_ev, eval_labels, n_eval, K);
            }
        } else {
            if (pr_tr_ok && (int)out_len_tr == n_train) {
                r.train_metric = WalkForward_ComputeAccuracy(pred_tr, train_labels, n_train, 0.5f);
            }
            if (pr_ev_ok && (int)out_len_ev == n_eval) {
                r.metric = WalkForward_ComputeAccuracy(pred_ev, eval_labels, n_eval, 0.5f);
            }
        }

        r.eval_count  = n_eval;
        r.train_count = n_train;
        r.ok = 1;

        fprintf(stderr, "[heldout] result: train_metric=%.4f held_out_metric=%.4f%s\n",
                r.train_metric, r.metric,
                is_regression ? " (correlation)" : " (accuracy)");
        // v5.11.58 — release the train-prediction snapshot
        if (pred_tr_copy) free(pred_tr_copy);
    } while (0);

    if (booster) XGBoosterFree(booster);
    if (dtrain)  XGDMatrixFree(dtrain);
    if (deval)   XGDMatrixFree(deval);
    free(train_features);
    free(train_labels);
    free(eval_features);
    free(eval_labels);
    free(mc_weights);
    return r;
#endif
}

//======================================================================================================
// [CONFIG FIELD SETTER]
//======================================================================================================
// sets a config field by key name + double value. used by optimizer to sweep parameters.
// returns 1 if field was found and set, 0 if unknown key.
// handles both FPN_Binary and PCT fields (PCT keys are stored as decimal, value comes in as %).
//======================================================================================================
static inline int ConfigField_Set(ControllerConfig<BACKTEST_FP> *cfg, const char *key, double value) {
    // percentage fields (config says 4.0, stored as 0.04)
    #define OPT_SET_PCT(name) \
        if (strcmp(key, #name) == 0) { cfg->name = Money{ money_from_double_payload(value / 100.0) }; return 1; }
    // raw FPN_Binary fields
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

    // v5.10.0a — XGBoost hyperparam sweeping. Hyperparams have been
    // cfg-bound since v5.9.5h; v5.10.0D added thread-count fields.
    // Operator can now grid-search over these via OptimizerPanel.
    // xgb_tree_method (string) is intentionally excluded — sweep over
    // discrete categorical values is not supported by OptimizerRange's
    // numeric step model.
    #define OPT_SET_INT(name) \
        if (strcmp(key, #name) == 0) { cfg->name = (int)value; return 1; }
    OPT_SET_FPN(xgb_subsample)
    OPT_SET_FPN(xgb_colsample_bytree)
    OPT_SET_INT(xgb_min_child_weight)
    OPT_SET_INT(xgb_seed)
    OPT_SET_INT(xgb_train_nthread)
    OPT_SET_INT(xgb_eval_nthread)
    // Note: label_tp_pct / label_sl_pct / label_forward_ticks are on
    // BacktestRunConfig (per-run), not ControllerConfig. Sweeping those
    // requires a separate Sweep extension; deferred until operator
    // demand. For now: train your candidates with TrainingPanel and use
    // the Optimizer for ML hyperparams + engine cfg fields.

    #undef OPT_SET_PCT
    #undef OPT_SET_FPN
    #undef OPT_SET_U32
    #undef OPT_SET_INT
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

//======================================================================================================
// [v5.10.0a.D — HYPERPARAM TRAINING SWEEP]
//======================================================================================================
// Trains an XGBooster per sweep cell using shared feature_matrix +
// labels (operator must Collect Features once first). Per cell,
// applies override hyperparams via WF's cfg_override path, runs WF,
// records val_accuracy as the cell's metric.
//
// Different from Backtest_RunSweep:
//   - No Backtest_Run per cell (no engine sweep)
//   - Per cell: clone cfg + apply ConfigField_Set, call WF with
//     cfg_override, record metric
//   - Output: OptimizerResults (same struct; metric is val_accuracy
//     not P&L)
//
// Training-side hyperparam search. Operator workflow:
//   1. Train Model panel: click Collect Features (populates
//      feature_matrix + labels)
//   2. New Hyperparam Sweep button: pick xgb_subsample range, click
//   3. This function trains N XGBoosters, picks best by val_acc
//   4. Operator inspects results table, decides which params to
//      use for production train
//
// Single-thread baseline; parallelism via v5.10.0a.F (cfg.xgb_train_nthread).
//
// Metric: WF mean_val_accuracy (binary/multiclass) or
// mean_val_correlation (regression). Stored as positive number; higher = better.
//======================================================================================================
#ifdef USE_XGBOOST
static inline void Backtest_RunHyperparamTrainSweep(
    OptimizerResults *opt,
    const BacktestResults *feature_data,
    const OptimizerRange *ranges, int num_params,
    int label_type,
    int wf_n_splits, int wf_horizon, int wf_buffer, int wf_min_train,
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

    // Pre-compute parameter values (matches Backtest_RunSweep convention)
    for (int i = 0; i < opt->dims[0]; i++)
        opt->param_vals[0][i] = ranges[0].lo + i * ranges[0].step;
    if (num_params > 1)
        for (int i = 0; i < opt->dims[1]; i++)
            opt->param_vals[1][i] = ranges[1].lo + i * ranges[1].step;

    if (!feature_data || feature_data->sample_count <= 0) {
        fprintf(stderr, "[hpsweep] no feature data — Collect Features first\n");
        return;
    }

    // Base cfg = the cfg the features were collected under. Each cell
    // overrides specific hyperparam fields without disturbing the rest.
    ControllerConfig<BACKTEST_FP> base_cfg = feature_data->config_used;

    // v5.10.0a.F — parallel pthread training. cfg.xgb_train_nthread > 1
    // dispatches cells across N pthread workers; each worker creates its
    // own DMatrix + Booster (per XGBoost 3.3.0 thread-safety: independent
    // booster instances are thread-safe across threads). Determinism
    // preserved: per-booster nthread=1 inside parallel sweep so within-
    // booster work stays single-threaded. Bytewise-equivalent to serial
    // sweep for the same seed + data.
    int n_workers = base_cfg.xgb_train_nthread > 0
                  ? base_cfg.xgb_train_nthread : 1;
    if (n_workers > opt->total_runs) n_workers = opt->total_runs;

    fprintf(stderr, "[hpsweep] starting %d-cell sweep (sample_count=%d, n_workers=%d)\n",
            opt->total_runs, feature_data->sample_count, n_workers);

    // Per-cell helper — runs a single cell, writes to opt->stats[idx] +
    // opt->metric[idx]. Single-writer per cell; no contention with other
    // workers since each worker handles disjoint cell indices.
    auto run_cell = [&](int idx) {
        int i0 = idx / opt->dims[1];
        int i1 = idx % opt->dims[1];
        ControllerConfig<BACKTEST_FP> cell_cfg = base_cfg;
        // Force per-booster nthread=1 in parallel mode for determinism;
        // operator's xgb_eval_nthread is preserved in serial mode (only
        // overridden when running parallel sweep).
        if (n_workers > 1) cell_cfg.xgb_eval_nthread = 1;
        ConfigField_Set(&cell_cfg, ranges[0].key, opt->param_vals[0][i0]);
        if (num_params > 1)
            ConfigField_Set(&cell_cfg, ranges[1].key, opt->param_vals[1][i1]);

        WalkForwardResults wf_results;
        int local_progress = 0;
        Backtest_RunWalkForward(&wf_results, feature_data,
                                 wf_n_splits, wf_horizon, wf_buffer,
                                 wf_min_train, &local_progress,
                                 cancel_flag, label_type, &cell_cfg);

        memset(&opt->stats[idx], 0, sizeof(opt->stats[idx]));
        double cell_metric = (wf_results.label_kind == 1)
            ? (double)wf_results.mean_val_correlation
            : (double)wf_results.mean_val_accuracy;
        opt->metric[idx] = cell_metric;
    };

    if (n_workers <= 1) {
        // SERIAL PATH (also used when total_runs == 1)
        for (int idx = 0; idx < opt->total_runs; ++idx) {
            if (cancel_flag && *cancel_flag) {
                fprintf(stderr, "[hpsweep] cancelled at cell %d/%d\n",
                        idx, opt->total_runs);
                return;
            }
            *current_run = idx + 1;
            run_cell(idx);
            if (opt->metric[idx] > best_metric) {
                best_metric = opt->metric[idx];
                opt->best_idx = idx;
            }
        }
    } else {
        // PARALLEL PATH — N workers in round-robin over cells.
        // Worker_k handles cells {k, k+N, k+2*N, ...}. Disjoint indices,
        // no shared writes to opt->. Coordinator joins all workers, then
        // single-threaded best-cell selection.
        struct WorkerCtx {
            int worker_id;
            int n_workers;
            int total_runs;
            volatile int *cancel_flag;
            volatile int *current_run;
            std::function<void(int)> *run_cell_fn;
        };

        pthread_t tids[OPT_MAX_GRID];  // bound by total cell cap
        WorkerCtx ctxs[OPT_MAX_GRID];
        std::function<void(int)> run_cell_holder = run_cell;

        auto worker_thunk = +[](void *arg) -> void* {
            WorkerCtx *c = (WorkerCtx*)arg;
            for (int idx = c->worker_id; idx < c->total_runs; idx += c->n_workers) {
                if (c->cancel_flag && *c->cancel_flag) return nullptr;
                (*c->run_cell_fn)(idx);
                // current_run is monotonic-ish; workers write asynchronously
                // (this is just for UI progress, exact ordering not required)
                __atomic_store_n(c->current_run, idx + 1, __ATOMIC_RELAXED);
            }
            return nullptr;
        };

        for (int k = 0; k < n_workers; ++k) {
            ctxs[k].worker_id = k;
            ctxs[k].n_workers = n_workers;
            ctxs[k].total_runs = opt->total_runs;
            ctxs[k].cancel_flag = cancel_flag;
            ctxs[k].current_run = current_run;
            ctxs[k].run_cell_fn = &run_cell_holder;
            pthread_create(&tids[k], nullptr, worker_thunk, &ctxs[k]);
        }
        for (int k = 0; k < n_workers; ++k) {
            pthread_join(tids[k], nullptr);
        }
        // Single-threaded best-cell selection post-join.
        for (int idx = 0; idx < opt->total_runs; ++idx) {
            if (opt->metric[idx] > best_metric) {
                best_metric = opt->metric[idx];
                opt->best_idx = idx;
            }
        }
    }
    fprintf(stderr, "[hpsweep] complete; best cell=%d metric=%.4f\n",
            opt->best_idx, best_metric);
}
#endif // USE_XGBOOST

#endif // BACKTEST_ENGINE_HPP
