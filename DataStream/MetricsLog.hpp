// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[DataStream/MetricsLog.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE] [PERSISTENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[LEGACY single-core-era diagnostics CSV — ORPHANED at HEAD (zero includers/callers anywhere incl. tests; verified 2026-07-17); deletion candidate, see the TECH_DEBT-031 currency note]
// [CONTAINS]
//   - [STRUCT]_[MetricsLog]   (+ Init / Close / _metrics_timestamp ride)
//   - [FUNCTION]_[MetricsLog_SlowPath]   (+ MetricsLog_Event)
//======================================================================================================
// slow-path diagnostics logger — records engine state every slow-path cycle
// separate from trade log (which only records buys/sells)
// outputs CSV for easy analysis: grep, awk, or load into pandas
//
// columns:
//   tick, timestamp, event, regime, strategy, hysteresis, proposed_regime,
//   positions, balance, equity, realized_pnl, unrealized_pnl,
//   price, avg, stddev, slope_pct, r2, long_slope_pct,
//   buy_gate_price, buy_gate_vol, gate_direction, gate_distance_pct,
//   breakout_mult, offset_mult, vol_mult, spacing,
//   details (free-form for events)
//
// events:
//   SLOW_PATH    — periodic state snapshot (every slow-path cycle)
//   REGIME_CHANGE — regime transition with old/new
//   POSITION_ADJUST — TP/SL modified on regime switch
//   FILL         — new position entered
//   EXIT         — position closed (TP/SL/TIME)
//   WARMUP_DONE  — warmup completed, trading enabled
//   VOLATILE_PAUSE — buying paused due to volatile regime
//======================================================================================================
#ifndef METRICS_LOG_HPP
#define METRICS_LOG_HPP

#include <stdio.h>
#include <time.h>

//======================================================================
// [STRUCT]_[MetricsLog]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[FILE* + initialized flag (Init / Close / _metrics_timestamp ride) — append-mode CSV, header written on empty file]
//======================================================================
// [CODE]
//======================================================================
struct MetricsLog {
    FILE *file;
    int initialized;
};

static inline void MetricsLog_Init(MetricsLog *log, const char *filepath) {
    log->file = fopen(filepath, "a"); // append mode — survives restarts
    log->initialized = (log->file != NULL);
    if (!log->initialized) {
        fprintf(stderr, "[METRICS] failed to open %s\n", filepath);
        return;
    }

    // write header if file is empty
    fseek(log->file, 0, SEEK_END);
    if (ftell(log->file) == 0) {
        fprintf(log->file,
            "tick,timestamp,event,regime,strategy,hysteresis,proposed,"
            "positions,balance,equity,realized,unrealized,"
            "price,avg,stddev,slope_pct,r2,long_slope_pct,"
            "buy_price,buy_vol,gate_dir,gate_dist_pct,"
            "breakout_mult,offset_mult,vol_mult,spacing,"
            "details\n");
        fflush(log->file);
    }
}

static inline void MetricsLog_Close(MetricsLog *log) {
    if (log->file) { fflush(log->file); fclose(log->file); }
    log->file = NULL;
    log->initialized = 0;
}

// get ISO-ish timestamp for the log
static inline void _metrics_timestamp(char *buf, int bufsize) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    snprintf(buf, bufsize, "%04d-%02d-%02d %02d:%02d:%02d",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec);
}

// _regime_str and _strategy_str defined in TradeLog.hpp (shared)
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]   (tool-refreshed — layout emitter cannot probe this block yet; header is ORPHANED (zero includers) so the struct is in no TU's layout dump — quartet lands if revived, D-327)
//======================================================================
// [END_STRUCT]_[MetricsLog]
//======================================================================

//======================================================================
// [FUNCTION]_[MetricsLog_SlowPath]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[per-slow-path-cycle full-state CSV row (MetricsLog_Event rides — sparse row + free-form details) — LEGACY PortfolioController<F> reader; orphaned with the file]
//======================================================================
// [CODE]
//======================================================================
// call every slow-path cycle — records full engine state
template <unsigned F>
static inline void MetricsLog_SlowPath(MetricsLog *log,
                                         const PortfolioController<F> *ctrl,
                                         double current_price) {
    if (!log->initialized) return;

    char ts[32]; _metrics_timestamp(ts, sizeof(ts));

    double avg     = FPN_ToDouble(ctrl->rolling.price_avg);
    double stddev  = FPN_ToDouble(ctrl->rolling.price_stddev);
    double slope   = (avg != 0.0) ? (FPN_ToDouble(ctrl->rolling.price_slope) / avg) * 100.0 : 0.0;
    double r2      = ctrl->mean_rev.has_regression ? FPN_ToDouble(ctrl->mean_rev.last_regression.r_squared) : 0.0;
    double long_sl = (avg != 0.0) ? (FPN_ToDouble(ctrl->rolling_long->price_slope) / avg) * 100.0 : 0.0;

    double buy_p   = FPN_ToDouble(ctrl->buy_conds.price);
    double buy_v   = FPN_ToDouble(ctrl->buy_conds.volume);
    double gate_dist = (avg != 0.0) ? ((current_price - buy_p) / avg) * 100.0 : 0.0;

    double breakout = FPN_ToDouble(ctrl->momentum.live_breakout_mult);
    double offset   = FPN_ToDouble(ctrl->mean_rev.live_stddev_mult);
    double vmult    = (ctrl->strategy_id == 1)
                        ? FPN_ToDouble(ctrl->momentum.live_vol_mult)
                        : FPN_ToDouble(ctrl->mean_rev.live_vol_mult);
    double spacing  = FPN_ToDouble(RollingStats_EntrySpacing(&ctrl->rolling, ctrl->config.spacing_multiplier));

    int positions = __builtin_popcount(ctrl->portfolio.active_bitmap);
    double balance = Money_ToDouble(ctrl->balance);
    double realized = Money_ToDouble(ctrl->realized_pnl);
    double unrealized = Money_ToDouble(ctrl->portfolio_delta);
    double equity = balance + Money_ToDouble(Portfolio_ComputeValue(&ctrl->portfolio, Money{ money_from_double_payload(current_price) }));

    fprintf(log->file,
        "%lu,%s,SLOW_PATH,%s,%s,%d,%s,"
        "%d,%.4f,%.4f,%.4f,%.4f,"
        "%.2f,%.2f,%.2f,%.6f,%.4f,%.6f,"
        "%.2f,%.8f,%d,%.4f,"
        "%.4f,%.4f,%.4f,%.2f,"
        "\n",
        (unsigned long)ctrl->total_ticks, ts,
        _regime_str(ctrl->regime.current_regime),
        _strategy_str(ctrl->strategy_id),
        ctrl->regime.hysteresis_count,
        _regime_str(ctrl->regime.proposed_regime),
        positions, balance, equity, realized, unrealized,
        current_price, avg, stddev, slope, r2, long_sl,
        buy_p, buy_v, ctrl->buy_conds.gate_direction, gate_dist,
        breakout, offset, vmult, spacing);

    fflush(log->file);
}

template <unsigned F>
static inline void MetricsLog_Event(MetricsLog *log,
                                      const PortfolioController<F> *ctrl,
                                      double current_price,
                                      const char *event,
                                      const char *details) {
    if (!log->initialized) return;

    char ts[32]; _metrics_timestamp(ts, sizeof(ts));

    int positions = __builtin_popcount(ctrl->portfolio.active_bitmap);
    double balance = Money_ToDouble(ctrl->balance);
    double realized = Money_ToDouble(ctrl->realized_pnl);
    double unrealized = Money_ToDouble(ctrl->portfolio_delta);
    double avg = FPN_ToDouble(ctrl->rolling.price_avg);
    double equity = balance + Money_ToDouble(Portfolio_ComputeValue(&ctrl->portfolio, Money{ money_from_double_payload(current_price) }));

    fprintf(log->file,
        "%lu,%s,%s,%s,%s,%d,%s,"
        "%d,%.4f,%.4f,%.4f,%.4f,"
        "%.2f,%.2f,,,,,,,,,,,,%s\n",
        (unsigned long)ctrl->total_ticks, ts, event,
        _regime_str(ctrl->regime.current_regime),
        _strategy_str(ctrl->strategy_id),
        ctrl->regime.hysteresis_count,
        _regime_str(ctrl->regime.proposed_regime),
        positions, balance, equity, realized, unrealized,
        current_price, avg, details);

    fflush(log->file);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[MetricsLog_SlowPath]
//======================================================================

#endif // METRICS_LOG_HPP
