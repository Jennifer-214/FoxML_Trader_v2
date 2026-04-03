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
//   timestamp_ms,price,quantity,is_buyer_maker
//======================================================================================================
static inline int BacktestData_DetectFormat(const char *header) {
    // TickRecorder format starts with "timestamp_ms"
    if (strncmp(header, "timestamp_ms", 12) == 0) return 1;
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
            // TickRecorder: timestamp_ms,price,quantity,is_buyer_maker
            char *p = line;
            t->timestamp_ms = strtoll(p, &p, 10); if (*p == ',') p++;
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
            t->timestamp_ms = strtoll(p, &p, 10); if (*p == ',') p++;
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
    char data_paths[16][256];
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
#define BACKTEST_MAX_EQUITY    8192
#define BACKTEST_MAX_SAMPLES   50000

struct BacktestResults {
    BacktestStats stats;
    double equity_curve[BACKTEST_MAX_EQUITY];
    int equity_count;
    char trade_csv_path[256];
    // ML features (populated when collect_features=1)
    float feature_matrix[BACKTEST_MAX_SAMPLES * MODEL_MAX_FEATURES];
    float labels[BACKTEST_MAX_SAMPLES];
    int sample_tick_indices[BACKTEST_MAX_SAMPLES]; // tick index of each sample (for label computation)
    double sample_prices[BACKTEST_MAX_SAMPLES];     // price at each sample point
    int sample_regimes[BACKTEST_MAX_SAMPLES];       // regime at each sample point
    int sample_count;
    // config used (for comparison)
    ControllerConfig<BACKTEST_FP> config_used;
};

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

//======================================================================================================
// [RUN]
//======================================================================================================
// the core replay loop — mirrors main.cpp:363-547
//======================================================================================================
static inline void Backtest_Run(BacktestResults *results, const BacktestRunConfig *run_cfg,
                                 volatile int *progress_pct, volatile int *cancel_flag,
                                 CandleAccumulator *candle_acc,
                                 TUISnapshot *out_snapshot = NULL) {
    memset(results, 0, sizeof(*results));

    // load config
    ControllerConfig<BACKTEST_FP> cfg;
    if (run_cfg->use_config_override) {
        cfg = run_cfg->config_override;
    } else {
        cfg = ControllerConfig_Load<BACKTEST_FP>(run_cfg->config_path);
    }
    results->config_used = cfg;

    // init controller (same as main.cpp:159-161)
    PortfolioController<BACKTEST_FP> ctrl;
    ctrl.rolling_long = NULL;
    PortfolioController_Init(&ctrl, cfg);

    // init order pool (same as main.cpp:174)
    OrderPool<BACKTEST_FP> pool;
    OrderPool_init(&pool, 64);

    // init trade log — write to backtest output
    mkdir("logging", 0755);
    TradeLog log;
    snprintf(results->trade_csv_path, sizeof(results->trade_csv_path),
             "logging/backtest_order_history.csv");
    TradeLog_Init(&log, "BACKTEST");

    // load all data files
    // allocate on heap — tick data can be large (~40MB per day)
    int max_ticks = 2000000; // 2M ticks per file
    HistoricalTick *ticks = (HistoricalTick *)malloc(max_ticks * sizeof(HistoricalTick));
    if (!ticks) {
        fprintf(stderr, "[backtest] failed to allocate tick buffer\n");
        return;
    }

    struct timeval t_start, t_end;
    gettimeofday(&t_start, NULL);

    int total_processed = 0;
    int total_ticks_all_files = 0;

    // first pass: count total ticks for progress bar
    for (int f = 0; f < run_cfg->num_data_files; f++) {
        FILE *fp = fopen(run_cfg->data_paths[f], "r");
        if (!fp) continue;
        int lines = 0;
        char buf[512];
        while (fgets(buf, sizeof(buf), fp)) lines++;
        fclose(fp);
        total_ticks_all_files += lines - 1; // subtract header
    }
    if (total_ticks_all_files <= 0) total_ticks_all_files = 1; // avoid div by zero

    double price_d_last = 0.0; // track last price for snapshot

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

            // build DataStream (same struct as live)
            DataStream<BACKTEST_FP> tick;
            tick.price = FPN_FromDouble<BACKTEST_FP>(ticks[i].price);
            tick.volume = FPN_FromDouble<BACKTEST_FP>(ticks[i].qty);
            tick.price_d = ticks[i].price;
            tick.volume_d = ticks[i].qty;
            tick.is_buyer_maker = ticks[i].is_buyer_maker;
            price_d_last = ticks[i].price;

            // exit gate on EVERY tick (same as main.cpp:381-392)
            if (ctrl.portfolio.active_bitmap)
                PositionExitGate(&ctrl.portfolio, tick.price, &ctrl.exit_buf, ctrl.total_ticks);

            // buy gate (same as main.cpp BuyGate call after burst drain)
            BuyGate(&ctrl.buy_conds, &tick, &pool);

            // core tick processing (same as main.cpp PortfolioController_Tick call)
            PortfolioController_Tick(&ctrl, &pool, tick.price, tick.volume,
                                     &log, tick.is_buyer_maker);

            // feed candle accumulator for chart (with historical timestamps)
            if (candle_acc)
                CandleAccumulator_PushWithTime(candle_acc, tick.price_d, tick.volume_d,
                                               tick.is_buyer_maker,
                                               (double)(ticks[i].timestamp_ms / 1000));

            // track equity curve (on each trade completion)
            if (ctrl.total_buys > 0 && (ctrl.wins + ctrl.losses) > (uint32_t)results->equity_count) {
                if (results->equity_count < BACKTEST_MAX_EQUITY) {
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
                results->sample_count < BACKTEST_MAX_SAMPLES &&
                ctrl.state != CONTROLLER_WARMUP) {
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

    // compute drawdown + sharpe from equity curve
    if (results->equity_count > 1)
        BacktestStats_ComputeFromEquity(&results->stats, results->equity_curve, results->equity_count);

    // populate TUISnapshot for dashboard panels (if requested)
    if (out_snapshot) {
        BacktestSnapshot_Copy<BACKTEST_FP>(out_snapshot, &ctrl, price_d_last, 0.0);
    }

    // post-processing: compute labels (needs forward-looking tick data)
    // reload last data file for label computation (labels look forward within this file)
    if (run_cfg->collect_features && results->sample_count > 0 && run_cfg->num_data_files > 0) {
        // reload the last file (or all files concatenated for multi-file)
        int label_count = 0;
        HistoricalTick *label_ticks = (HistoricalTick *)malloc(2000000 * sizeof(HistoricalTick));
        if (label_ticks) {
            for (int f = 0; f < run_cfg->num_data_files; f++)
                BacktestData_Load(label_ticks + label_count, &label_count, 2000000 - label_count,
                                  run_cfg->data_paths[f]);

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
    free(ctrl.rolling_long);

    fprintf(stderr, "[backtest] completed: %lu ticks in %.1fms, %u trades, P&L $%.2f\n",
            results->stats.ticks_processed, elapsed,
            results->stats.total_trades, results->stats.total_pnl);
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
static inline float WalkForward_ComputeAccuracy(const float *predictions, const float *labels,
                                                  int count, float threshold) {
    if (count <= 0) return 0.0f;
    int correct = 0;
    for (int i = 0; i < count; i++) {
        int pred_class = (predictions[i] >= threshold) ? 1 : 0;
        int true_class = (labels[i] >= 0.5f) ? 1 : 0;
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

    int valid = ValidationSplit_Generate(wf->splits, data->sample_count,
                                          n_splits, horizon_ticks,
                                          buffer_ticks, min_train_samples);
    wf->valid_folds = valid;

    if (valid == 0) {
        fprintf(stderr, "[walkforward] no valid folds — aborting\n");
        *progress_pct = 100;
        return;
    }

    ValidationSplit_Print(wf->splits, n_splits);

    // allocate prediction buffer (reused per fold)
    float *predictions = (float *)malloc(data->sample_count * sizeof(float));
    if (!predictions) {
        fprintf(stderr, "[walkforward] failed to allocate prediction buffer\n");
        *progress_pct = 100;
        return;
    }

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

        fprintf(stderr, "[walkforward] fold %d/%d: training on %d samples, testing on %d...\n",
                f + 1, n_splits, sp->train_count, sp->test_count);

        // build XGBoost DMatrix for train set
        const float *train_features = &data->feature_matrix[sp->train_start * MODEL_NUM_FEATURES];
        const float *train_labels = &data->labels[sp->train_start];
        const float *test_features = &data->feature_matrix[sp->test_start * MODEL_NUM_FEATURES];
        const float *test_labels = &data->labels[sp->test_start];

        DMatrixHandle dtrain = NULL, dtest = NULL;
        BoosterHandle booster = NULL;

        // create train DMatrix
        int ret = XGDMatrixCreateFromMat(train_features, sp->train_count,
                                          MODEL_NUM_FEATURES, -1.0f, &dtrain);
        if (ret != 0) {
            fprintf(stderr, "[walkforward] fold %d: failed to create train DMatrix: %s\n",
                    f + 1, XGBGetLastError());
            fr->valid = 0;
            continue;
        }
        XGDMatrixSetFloatInfo(dtrain, "label", train_labels, sp->train_count);

        // create test DMatrix
        ret = XGDMatrixCreateFromMat(test_features, sp->test_count,
                                      MODEL_NUM_FEATURES, -1.0f, &dtest);
        if (ret != 0) {
            fprintf(stderr, "[walkforward] fold %d: failed to create test DMatrix: %s\n",
                    f + 1, XGBGetLastError());
            XGDMatrixFree(dtrain);
            fr->valid = 0;
            continue;
        }
        XGDMatrixSetFloatInfo(dtest, "label", test_labels, sp->test_count);

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
            if (ret == 0 && (int)out_len == sp->train_count) {
                fr->train_accuracy = WalkForward_ComputeAccuracy(
                    out_result, train_labels, sp->train_count, 0.5f);
            }
        }

        // predict on test set (the metric that matters)
        {
            bst_ulong out_len;
            const float *out_result;
            ret = XGBoosterPredict(booster, dtest, 0, 0, 0, &out_len, &out_result);
            if (ret == 0 && (int)out_len == sp->test_count) {
                fr->val_accuracy = WalkForward_ComputeAccuracy(
                    out_result, test_labels, sp->test_count, 0.5f);
            }
        }

        // feature importances (stability tracking hook)
        // XGBoost importance via dump → we use a simpler approach: score type
        // for now, zero-fill — populated when we add importance extraction
        memset(fr->feature_importances, 0, sizeof(fr->feature_importances));

        fr->train_samples = sp->train_count;
        fr->test_samples = sp->test_count;
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

    free(predictions);

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
            int dummy_progress = 0;
            Backtest_Run(&results, &run, &dummy_progress, cancel_flag, NULL);

            opt->stats[idx] = results.stats;
            opt->metric[idx] = OptimizerMetric(&results.stats, metric_idx);

            if (opt->metric[idx] > best_metric) {
                best_metric = opt->metric[idx];
                opt->best_idx = idx;
            }
        }
    }
}

#endif // BACKTEST_ENGINE_HPP
