// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [LABEL FUNCTIONS]
//======================================================================================================
// table-driven target label system for ML training data.
// adding a new label type = 1 function + 1 table entry.
//
// labels are computed by scanning forward through historical tick data from
// the sample point. this requires access to the full tick array (not just
// the current tick), so labels are computed in a post-processing pass
// after the backtest completes.
//
// barrier labels borrowed from FoxML/private target construction logic.
//======================================================================================================
#ifndef LABEL_FUNCTIONS_HPP
#define LABEL_FUNCTIONS_HPP

#include <stdint.h>

// HistoricalTick lives here (single definition point).
// BacktestEngine.hpp includes this file.
struct HistoricalTick {
    double price;
    double qty;
    int64_t timestamp_us; // Binance aggTrades: microseconds since epoch (NOT milliseconds)
    int is_buyer_maker;
};

#define LABEL_WIN_LOSS     0   // 1 = next trade was profitable after fees, 0 = loss
#define LABEL_BARRIER      1   // 1 = price hits +tp% before -sl% (first-passage)
#define LABEL_FORWARD_PNL  2   // continuous: forward return over N ticks (regression target)
#define LABEL_REGIME       3   // regime that was active (multi-class)
#define LABEL_VOL_BARRIER  4   // vol-scaled barrier: k * rolling_vol (from FoxML barrier.py)
#define LABEL_WILL_PEAK    5   // 1 = price peaks within N ticks (barrier gate training)
#define LABEL_WILL_VALLEY  6   // 1 = price valleys within N ticks (barrier gate training)

//======================================================================================================
// [WIN/LOSS]
// looks forward to the next completed trade — was it profitable?
// simplest label: 1 = win, 0 = loss. uses the existing engine's exit logic.
// note: labels trades, not ticks. many ticks will have label=0 because
// no trade was entered at that point.
//======================================================================================================
static inline float Label_WinLoss(const HistoricalTick *ticks, int tick_idx, int total_ticks,
                                   double sample_price, double tp_pct, double sl_pct,
                                   int /* forward_ticks */) {
    // scan forward: does price hit +tp% before -sl%?
    // this approximates whether a trade entered here would be profitable
    double tp_target = sample_price * (1.0 + tp_pct / 100.0);
    double sl_target = sample_price * (1.0 - sl_pct / 100.0);

    for (int j = tick_idx + 1; j < total_ticks; j++) {
        if (ticks[j].price >= tp_target) return 1.0f;  // hit TP first = win
        if (ticks[j].price <= sl_target) return 0.0f;   // hit SL first = loss
    }
    return 0.0f; // ran out of data = no exit = loss (conservative)
}

//======================================================================================================
// [BARRIER]
// first-passage label from FoxML/private: will price hit +X% before -Y%?
// tp_pct and sl_pct are the barrier sizes (e.g. 1.5 = 1.5%).
// same as win/loss but with configurable asymmetric barriers.
//======================================================================================================
static inline float Label_Barrier(const HistoricalTick *ticks, int tick_idx, int total_ticks,
                                   double sample_price, double tp_pct, double sl_pct,
                                   int /* forward_ticks */) {
    double up_barrier   = sample_price * (1.0 + tp_pct / 100.0);
    double down_barrier = sample_price * (1.0 - sl_pct / 100.0);

    for (int j = tick_idx + 1; j < total_ticks; j++) {
        if (ticks[j].price >= up_barrier)   return 1.0f;  // hit up barrier first
        if (ticks[j].price <= down_barrier) return 0.0f;   // hit down barrier first
    }
    return 0.5f; // neither hit = neutral (useful for 3-class later)
}

//======================================================================================================
// [FORWARD P&L]
// continuous label: return over the next N ticks.
// useful for regression (predict magnitude, not just direction).
//======================================================================================================
static inline float Label_ForwardPnl(const HistoricalTick *ticks, int tick_idx, int total_ticks,
                                      double sample_price, double /* tp_pct */, double /* sl_pct */,
                                      int forward_ticks) {
    int target_idx = tick_idx + forward_ticks;
    if (target_idx >= total_ticks) target_idx = total_ticks - 1;
    if (target_idx <= tick_idx) return 0.0f;

    double future_price = ticks[target_idx].price;
    return (float)((future_price - sample_price) / sample_price * 100.0); // % return
}

//======================================================================================================
// [REGIME]
// which regime was the engine in at this sample point?
// 0 = ranging, 1 = trending, 2 = volatile
// useful for training a regime classifier model.
//======================================================================================================
static inline float Label_Regime(const HistoricalTick * /* ticks */, int /* tick_idx */,
                                  int /* total_ticks */, double /* sample_price */,
                                  double /* tp_pct */, double /* sl_pct */,
                                  int regime_at_sample) {
    return (float)regime_at_sample;
}

//======================================================================================================
// [VOL-SCALED BARRIER]
// port of FoxML/private barrier.py compute_barrier_targets().
// barriers scale with rolling volatility instead of fixed percentage.
// adapts to market conditions: wider barriers in high-vol, tighter in low-vol.
//
// algorithm (from FoxML barrier.py):
//   1. compute returns: r[i] = (price[i] - price[i-1]) / price[i-1]
//   2. rolling vol: stddev(returns[i-vol_window : i])
//   3. up barrier: price * (1 + barrier_k * vol)
//   4. down barrier: price * (1 - barrier_k * vol)
//   5. scan forward from t+1: which barrier hits first? (time contract preserved)
//
// FoxML constants: barrier_size = 0.5 (k*sigma), vol_window = 20, min_periods = 5
// source: ~/FoxML/private/DATA_PROCESSING/targets/barrier.py
//======================================================================================================
static inline float Label_VolBarrier(const HistoricalTick *ticks, int tick_idx, int total_ticks,
                                      double sample_price, double barrier_k, double /* sl_pct */,
                                      int vol_window) {
    // parameter defaults (from FoxML barrier.py)
    if (barrier_k <= 0.0) barrier_k = 0.5;   // FoxML: barrier_size = 0.5
    if (vol_window <= 0) vol_window = 20;     // FoxML: vol_window = 20

    // need at least min_periods returns to compute vol (FoxML: min_periods = 5)
    int min_periods = 5;
    if (tick_idx < min_periods + 1) return 0.5f; // not enough history, neutral

    // compute rolling volatility (stddev of returns over last vol_window ticks)
    // uses a ring of returns ending at tick_idx
    int start = tick_idx - vol_window;
    if (start < 1) start = 1; // need at least 1 prior tick for returns
    int n_returns = tick_idx - start;
    if (n_returns < min_periods) return 0.5f; // not enough data for reliable vol

    // single-pass mean + variance (Welford-style for numerical stability)
    double sum = 0.0, sum_sq = 0.0;
    for (int j = start; j < tick_idx; j++) {
        if (ticks[j - 1].price <= 0.0) continue;
        double r = (ticks[j].price - ticks[j - 1].price) / ticks[j - 1].price;
        sum += r;
        sum_sq += r * r;
    }

    double mean = sum / n_returns;
    double variance = (sum_sq / n_returns) - (mean * mean);
    if (variance <= 0.0) return 0.5f; // zero vol = no signal

    double vol = 0.0;
    // manual sqrt to avoid pulling in math.h just for this
    // Newton's method: 4 iterations is plenty for double precision
    {
        double x = variance;
        double guess = x * 0.5;
        if (guess <= 0.0) guess = 1e-10;
        for (int iter = 0; iter < 8; iter++)
            guess = 0.5 * (guess + x / guess);
        vol = guess;
    }

    if (vol <= 1e-15) return 0.5f; // degenerate

    // vol-scaled barriers (from FoxML: barrier = k * rolling_vol)
    double up_barrier   = sample_price * (1.0 + barrier_k * vol);
    double down_barrier = sample_price * (1.0 - barrier_k * vol);

    // first-passage scan from t+1 (time contract: label never includes current tick)
    for (int j = tick_idx + 1; j < total_ticks; j++) {
        if (ticks[j].price >= up_barrier)   return 1.0f;  // hit up barrier first
        if (ticks[j].price <= down_barrier) return 0.0f;   // hit down barrier first
    }
    return 0.5f; // neither hit = neutral
}

//======================================================================================================
// [WILL_PEAK / WILL_VALLEY]
// binary labels for barrier gate model training.
// WILL_PEAK: 1 if price reaches a local max within N ticks (extra_param = lookahead)
// WILL_VALLEY: 1 if price reaches a local min within N ticks
// "local max/min" = price is highest/lowest in a symmetric window around it
//======================================================================================================
static float Label_WillPeak(const HistoricalTick *ticks, int tick_idx, int total_ticks,
                             double sample_price, double tp_pct, double sl_pct,
                             int extra_param) {
    (void)tp_pct; (void)sl_pct;
    int lookahead = (extra_param > 0) ? extra_param : 500;
    int end = tick_idx + lookahead;
    if (end > total_ticks) end = total_ticks;
    // find max price in lookahead window
    double peak = sample_price;
    int peak_idx = tick_idx;
    for (int j = tick_idx + 1; j < end; j++) {
        if (ticks[j].price > peak) { peak = ticks[j].price; peak_idx = j; }
    }
    // peak near start = we're about to peak
    int near_start = (peak_idx - tick_idx) < (lookahead / 4);
    double rise_pct = (peak - sample_price) / sample_price;
    return (near_start && rise_pct > 0.001) ? 1.0f : 0.0f;
}

static float Label_WillValley(const HistoricalTick *ticks, int tick_idx, int total_ticks,
                               double sample_price, double tp_pct, double sl_pct,
                               int extra_param) {
    (void)tp_pct; (void)sl_pct;
    int lookahead = (extra_param > 0) ? extra_param : 500;
    int end = tick_idx + lookahead;
    if (end > total_ticks) end = total_ticks;
    double valley = sample_price;
    int valley_idx = tick_idx;
    for (int j = tick_idx + 1; j < end; j++) {
        if (ticks[j].price < valley) { valley = ticks[j].price; valley_idx = j; }
    }
    int near_start = (valley_idx - tick_idx) < (lookahead / 4);
    double drop_pct = (sample_price - valley) / sample_price;
    return (near_start && drop_pct > 0.001) ? 1.0f : 0.0f;
}

//======================================================================================================
// [LABEL TABLE]
// table-driven: add new label = add 1 entry here + 1 function above
//======================================================================================================
typedef float (*LabelFn)(const HistoricalTick *ticks, int tick_idx, int total_ticks,
                           double sample_price, double tp_pct, double sl_pct,
                           int extra_param);

struct LabelDef {
    int id;
    const char *name;
    const char *description;
    LabelFn fn;
};

static const LabelDef label_table[] = {
    { LABEL_WIN_LOSS,    "win_loss",      "Binary: 1=profitable entry, 0=loss",       Label_WinLoss    },
    { LABEL_BARRIER,     "barrier",       "First-passage: +tp% before -sl%",          Label_Barrier    },
    { LABEL_FORWARD_PNL, "forward_pnl",   "Continuous: % return over N ticks",        Label_ForwardPnl },
    { LABEL_REGIME,      "regime",        "Multi-class: regime at sample point",      Label_Regime     },
    { LABEL_VOL_BARRIER, "vol_barrier",   "Vol-scaled: k*sigma barrier (FoxML)",      Label_VolBarrier },
    { LABEL_WILL_PEAK,   "will_peak",    "Binary: 1=price peaks within N ticks",     Label_WillPeak   },
    { LABEL_WILL_VALLEY, "will_valley",  "Binary: 1=price valleys within N ticks",   Label_WillValley },
};

static const int LABEL_COUNT = sizeof(label_table) / sizeof(label_table[0]);

#endif // LABEL_FUNCTIONS_HPP
