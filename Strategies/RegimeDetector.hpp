// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [REGIME DETECTOR]
//======================================================================================================
// classifies market state and switches the active strategy using score-based detection
//
// regimes:
//   REGIME_RANGING    — low directional slope, no consistent trend → mean reversion
//   REGIME_TRENDING   — sustained directional move confirmed across timeframes → momentum
//   REGIME_VOLATILE   — volatility spike with no direction → pause buying
//
// detection uses signals from RollingStats (128-tick + 512-tick windows) and ROR regressor:
//   - price_slope + price_r_squared from both windows (multi-timeframe confirmation)
//   - variance ratio between windows (relative volatility, self-adapting)
//   - ROR slope-of-slopes (trend acceleration — catches new trends early)
//   - volume_slope (volume confirmation)
//
// each signal contributes to a trending_score or volatile_score. highest score wins.
// higher confidence (more signals agree) can reduce hysteresis for faster switching.
//
// extensibility: add a field to RegimeSignals + one comparison in Regime_Classify.
// future hooks: FoxML model output, order flow, microstructure signals.
//
// position handling on regime switch:
//   MR → momentum: widen TP (let trend run), tighten SL (cut if wrong)
//   momentum → MR: tighten TP (take profit), widen SL (allow chop)
//   volatile: no adjustment (panic-adjusting causes more harm)
//   adjustment is one-shot at transition. new positions get native TP/SL.
//======================================================================================================
#ifndef REGIME_DETECTOR_HPP
#define REGIME_DETECTOR_HPP

#include <cstdint>  // v5.15.5.F.4d TECH_DEBT-083 close — explicit IWYU for uintN_t (was transitively pulled)
#include "StrategyInterface.hpp"
#include "../CoreFrameworks/ControllerConfig.hpp"
#include "../CoreFrameworks/Portfolio.hpp"
#include "../ML_Headers/RollingStats.hpp"
#include "../ML_Headers/ROR_regressor.hpp"
#include "../ML_Headers/FlowFeatures.hpp"  // v4.5 Wave 1 — D.1/D.2/D.4 state
#include <time.h>
#include <stdlib.h>  // v5.4.0 Phase A.2: getenv() for TT_REGIME_DEBUG diagnostic gate
#include <stdio.h>   // v5.15.5.F.4d.1.E.1.2 Step-2: FILE*/fwrite/fread for the persist delegate
#include <string.h>  // v5.15.5.F.4d.1.E.1.2 Step-2: memcpy for the commit walker

// regime constants, RegimeInfo, and REGIME_STRATEGY_TABLE are in StrategyInterface.hpp

//======================================================================================================
// [REGIME SIGNALS]
//======================================================================================================
// collected once per slow-path cycle from rolling stats, rolling_long, and ROR
// this is the extensibility point: adding a new signal = adding a field here
// + one comparison in Regime_Classify
//======================================================================================================
template <unsigned F> struct RegimeSignals {
    // short window (128-tick)
    FPN_Binary<F> short_slope;       // relative price slope (slope / avg)
    FPN_Binary<F> short_r2;          // price regression R² (trend consistency)
    FPN_Binary<F> short_variance;    // price variance
    // long window (512-tick)
    FPN_Binary<F> long_slope;        // relative price slope
    FPN_Binary<F> long_r2;           // price regression R²
    FPN_Binary<F> long_variance;     // price variance
    // derived signals
    FPN_Binary<F> vol_ratio;         // short_variance / long_variance (volatility spike)
    FPN_Binary<F> ror_slope;         // slope-of-slopes (trend acceleration)
    FPN_Binary<F> volume_slope;      // volume trend (confirmation)
    FPN_Binary<F> volume_delta;      // net buy/sell pressure [-1.0, +1.0] (from Binance "m" field)
    // EMA/SMA crossover signals — primary trending/ranging indicator
    // EMA reacts every tick (~333-tick window), SMA lags (128/512 samples at slow-path rate)
    // spread = (ema - sma) / sma: positive = EMA above SMA = bullish, magnitude = strength
    FPN_Binary<F> ema_sma_spread;    // normalized spread vs 128-sample SMA
    FPN_Binary<F> ema_sma_spread_long; // normalized spread vs 512-sample SMA (multi-timeframe)
    int    ema_above_sma;     // 1 if ema > short SMA (bullish crossover state)
    // data sufficiency flags
    int short_count;
    int long_count;
    int ror_ready;            // 1 if ROR has enough data for meaningful output
    // ML model output (populated by ModelInference if model loaded, zero otherwise)
    FPN_Binary<F> model_score;       // raw model prediction [0, 1] — higher = more likely trending
    // v4.3 — medium-horizon feature expansion. All zero-default if the
    // optional state/params aren't supplied to Regime_ComputeSignals.
    FPN_Binary<F> mid_slope;         // 256-tick relative slope (between short and long)
    FPN_Binary<F> mid_r2;            // 256-tick R²
    FPN_Binary<F> cumdelta;          // rolling cumulative buyer-vs-seller aggression
    double hour_sin;          // sin(2π × hour_utc / 24), cyclical hour encoding
    double hour_cos;          // cos(2π × hour_utc / 24)
    FPN_Binary<F> vol_regime_ratio;  // short_stddev / baseline_stddev (4096-tick)
    double tick_rate_z;       // current ticks/sec z-score vs trailing baseline
    FPN_Binary<F> dist_to_high;      // (baseline_max - current_price) / current_price
    FPN_Binary<F> dist_to_low;       // (current_price - baseline_min) / current_price
    // v4.5 — Wave 1 feature pack expansion (D.1 + D.2 + D.4). All zero-
    // default when the corresponding state isn't supplied to
    // Regime_ComputeSignals.
    FPN_Binary<F>  book_imb_mean_short; // mean of last 64 book_imbalance samples
    FPN_Binary<F>  book_imb_mean_long;  // mean over full BookImbalanceHistory window
    FPN_Binary<F>  book_imb_drift;      // current book_imbalance - mean_long
    double  flow_10s;            // signed-volume EWMA, half-life 10s
    double  flow_1m;             // half-life 60s
    double  flow_5m;             // half-life 300s
    double  large_trade_z;       // z-score of current trade size vs window
    // v4.6 Wave 2 — D.3 spread dynamics
    double  spread_bps;          // current spread / mid_price × 10000 (basis points)
    double  spread_zscore;       // z-score of current spread vs trailing window
};

//======================================================================================================
// v4.3 — auxiliary state for new features (not maintained by RollingStats)
//======================================================================================================
// Cumulative trade-side delta — running net buyer aggression over a rolling
// window. is_buyer_maker=1 means the buyer was the maker → seller was the
// taker → net delta -= qty. is_buyer_maker=0 → buyer was taker → +qty.
// Ring buffer of recent N samples for windowed aggregation.
//======================================================================================================
#define CUMDELTA_WINDOW 1024
template <unsigned F> struct CumDeltaState {
    FPN_Binary<F> sum;               // current window sum
    FPN_Binary<F> samples[CUMDELTA_WINDOW];
    int    head;
    int    count;
};

template <unsigned F>
inline void CumDelta_Init(CumDeltaState<F>* s) {
    s->sum = FPN_Zero<F>();
    s->head = 0;
    s->count = 0;
    for (int i = 0; i < CUMDELTA_WINDOW; ++i) s->samples[i] = FPN_Zero<F>();
}

template <unsigned F>
inline void CumDelta_Push(CumDeltaState<F>* s, FPN_Binary<F> qty, int is_buyer_maker) {
    // is_buyer_maker=1 → seller aggression (negative); =0 → buyer aggression (+).
    FPN_Binary<F> signed_qty = is_buyer_maker ? FPN_Negate(qty) : qty;
    if (s->count < CUMDELTA_WINDOW) {
        s->samples[s->head] = signed_qty;
        s->sum = FPN_Add(s->sum, signed_qty);
        s->head = (s->head + 1) % CUMDELTA_WINDOW;
        s->count++;
    } else {
        // evict oldest, add new
        FPN_Binary<F> evict = s->samples[s->head];
        s->sum = FPN_Sub(s->sum, evict);
        s->samples[s->head] = signed_qty;
        s->sum = FPN_Add(s->sum, signed_qty);
        s->head = (s->head + 1) % CUMDELTA_WINDOW;
    }
}

//======================================================================================================
// Tick arrival rate Z-score — current ticks/sec vs trailing baseline.
// Maintains a rolling-stat estimate of inter-tick latency to compute mean
// and stddev; current rate's z-score is informative for burst detection.
//======================================================================================================
#define TICKRATE_WINDOW 1024
struct TickRateState {
    uint64_t recent_ts_us[TICKRATE_WINDOW];  // microsecond timestamps
    int    head;
    int    count;
    double trailing_mean_rate;  // baseline ticks-per-second mean
    double trailing_stddev_rate; // baseline stddev
};

static inline void TickRate_Init(TickRateState* s) {
    s->head = 0;
    s->count = 0;
    s->trailing_mean_rate = 0.0;
    s->trailing_stddev_rate = 0.0;
    for (int i = 0; i < TICKRATE_WINDOW; ++i) s->recent_ts_us[i] = 0;
}

static inline void TickRate_Push(TickRateState* s, uint64_t timestamp_us) {
    s->recent_ts_us[s->head] = timestamp_us;
    s->head = (s->head + 1) % TICKRATE_WINDOW;
    if (s->count < TICKRATE_WINDOW) s->count++;
    // refresh baseline stats periodically (every full buffer)
    if (s->count == TICKRATE_WINDOW && s->head == 0) {
        // compute current per-second rate using start/end timestamps
        uint64_t first = s->recent_ts_us[0];
        uint64_t last = s->recent_ts_us[TICKRATE_WINDOW - 1];
        if (last > first) {
            double span_sec = (double)(last - first) / 1e6;
            double rate = (double)TICKRATE_WINDOW / span_sec;
            // update mean using exponential decay
            if (s->trailing_mean_rate == 0.0) {
                s->trailing_mean_rate = rate;
                s->trailing_stddev_rate = rate * 0.1;  // ~10% as initial stddev guess
            } else {
                double alpha = 0.05;  // slow decay
                double diff = rate - s->trailing_mean_rate;
                s->trailing_mean_rate += alpha * diff;
                // EWMA stddev approximation
                s->trailing_stddev_rate = (1 - alpha) * s->trailing_stddev_rate + alpha * (diff < 0 ? -diff : diff);
            }
        }
    }
}

static inline double TickRate_CurrentZ(const TickRateState* s) {
    if (s->count < 2 || s->trailing_stddev_rate <= 0.0) return 0.0;
    // current rate from last two samples
    int prev = (s->head - 2 + TICKRATE_WINDOW) % TICKRATE_WINDOW;
    int curr = (s->head - 1 + TICKRATE_WINDOW) % TICKRATE_WINDOW;
    if (s->recent_ts_us[curr] <= s->recent_ts_us[prev]) return 0.0;
    double inter = (double)(s->recent_ts_us[curr] - s->recent_ts_us[prev]) / 1e6;
    if (inter <= 0.0) return 0.0;
    double current_rate = 1.0 / inter;
    return (current_rate - s->trailing_mean_rate) / s->trailing_stddev_rate;
}

//======================================================================================================
// [COMPUTE SIGNALS]
//======================================================================================================
// fills RegimeSignals from current rolling stats, rolling_long, and ROR
// called once per slow-path cycle before Regime_Classify
//======================================================================================================
// v4.3 — feature-pack expansion. Optional new state pointers populate the
// new fields when non-null; when null, new fields stay zero-initialized so
// older callers remain compatible at compile time. The FEAT_* indices in
// the model still expect those features though — old v1 models will fail
// the version check on load.
template <unsigned F>
inline void Regime_ComputeSignals(RegimeSignals<F> *sig,
                                   const RollingStats<F> *rolling,
                                   const RollingStats<F, 512> *rolling_long,
                                   const RORRegressor<F> *ror,
                                   FPN_Binary<F> ema_price,
                                   const RollingStats<F, 256> *rolling_medium = nullptr,
                                   const RollingStats<F, 1024> *rolling_baseline = nullptr,
                                   const CumDeltaState<F> *cumdelta = nullptr,
                                   const TickRateState *tick_rate = nullptr,
                                   uint64_t timestamp_us = 0,
                                   // v4.5 Wave 1 — optional state for D.1, D.2, D.4
                                   // (book imbalance history, flow EWMAs, large
                                   // trade z-score). Zero-defaults when null,
                                   // mirroring v4.3's pattern.
                                   const void *book_imb_history = nullptr,
                                   const void *flow_state = nullptr,
                                   const void *large_trade_state = nullptr,
                                   // v4.6 Wave 2 — D.3 spread dynamics. spread_-
                                   // current + mid_price come from BookSnapshot
                                   // (the same struct DepthSharedState +
                                   // DepthReplayState expose). spread_state is
                                   // a SpreadState<F, 1024> ring for z-score.
                                   const void *spread_state = nullptr,
                                   double current_spread = 0.0,
                                   double current_mid_price = 0.0) {
    // short window signals
    sig->short_count    = rolling->count;
    sig->short_r2       = rolling->price_r_squared;
    sig->short_variance = rolling->price_variance;
    sig->volume_slope   = rolling->volume_slope;

    // relative slope: normalize by price so threshold is asset-independent
    if (!FPN_IsZero(rolling->price_avg))
        sig->short_slope = FPN_DivNoAssert(rolling->price_slope, rolling->price_avg);
    else
        sig->short_slope = FPN_Zero<F>();

    // long window signals
    sig->long_count    = rolling_long->count;
    sig->long_r2       = rolling_long->price_r_squared;
    sig->long_variance = rolling_long->price_variance;

    if (!FPN_IsZero(rolling_long->price_avg))
        sig->long_slope = FPN_DivNoAssert(rolling_long->price_slope, rolling_long->price_avg);
    else
        sig->long_slope = FPN_Zero<F>();

    // variance ratio: current volatility relative to baseline
    // > 1.0 means volatility is elevated vs longer-term average
    // self-adapting: $50 stddev in calm market (baseline $20) = 6.25x, same $50 in volatile ($45) = 1.23x
    if (rolling_long->count >= 64 && !FPN_IsZero(rolling_long->price_variance))
        sig->vol_ratio = FPN_DivNoAssert(rolling->price_variance, rolling_long->price_variance);
    else
        sig->vol_ratio = FPN_FromDouble<F>(1.0); // default: no spike detected

    // ROR: slope-of-slopes (trend acceleration)
    // positive = trend getting steeper, negative = trend flattening/reversing
    sig->ror_ready = (ror->count >= MAX_WINDOW);
    if (sig->ror_ready) {
        // compute ROR regression on slope samples
        LinearRegression3XResult<F> ror_result = RORRegressor_Compute(
            const_cast<RORRegressor<F>*>(ror));
        sig->ror_slope = ror_result.model.slope;
    } else {
        sig->ror_slope = FPN_Zero<F>();
    }

    // volume delta: net buy/sell pressure from short window
    sig->volume_delta = rolling->volume_delta;

    // model score: initialized to zero, populated externally by PortfolioController
    // if a regime enrichment model is loaded
    sig->model_score = FPN_Zero<F>();

    // EMA/SMA crossover: (ema - sma) / sma
    // normalized so threshold is asset-independent (same value works for BTC and ETH)
    // branchless: compute the spread always (FPN_DivNoAssert by zero safely saturates → deterministic), then
    // mask to 0 when either operand is zero. H20: branchless even if slower — a mispredict's variance cascades.
    int valid = !FPN_IsZero(rolling->price_avg) & !FPN_IsZero(ema_price);
    unsigned __int128 vmask = -(unsigned __int128)(unsigned)valid;
    FPN_Binary<F> spread = FPN_DivNoAssert(FPN_Sub(ema_price, rolling->price_avg), rolling->price_avg);
    sig->ema_sma_spread = { (__int128)((unsigned __int128)spread.v & vmask) };   // spread if valid, else 0
    sig->ema_above_sma  = (sig->ema_sma_spread.v > 0);                           // 0 when masked to 0

    if (!FPN_IsZero(rolling_long->price_avg) && !FPN_IsZero(ema_price)) {
        sig->ema_sma_spread_long = FPN_DivNoAssert(
            FPN_Sub(ema_price, rolling_long->price_avg), rolling_long->price_avg);
    } else {
        sig->ema_sma_spread_long = FPN_Zero<F>();
    }

    // v4.3 — medium-horizon features. Each one zero-defaults if its required
    // state isn't supplied; train-serve parity is preserved as long as both
    // backtest and live populate the same state with the same cadence.

    // FEAT_MID_SLOPE / FEAT_MID_R2 — 256-tick mid-window slope
    if (rolling_medium && rolling_medium->count >= 32 && !FPN_IsZero(rolling_medium->price_avg)) {
        sig->mid_slope = FPN_DivNoAssert(rolling_medium->price_slope, rolling_medium->price_avg);
        sig->mid_r2    = rolling_medium->price_r_squared;
    } else {
        sig->mid_slope = FPN_Zero<F>();
        sig->mid_r2    = FPN_Zero<F>();
    }

    // FEAT_CUMDELTA — rolling buyer-vs-seller aggression
    if (cumdelta && cumdelta->count >= 32) {
        // normalize by window size for scale-invariance
        FPN_Binary<F> n = FPN_FromDouble<F>((double)cumdelta->count);
        sig->cumdelta = FPN_DivNoAssert(cumdelta->sum, n);
    } else {
        sig->cumdelta = FPN_Zero<F>();
    }

    // FEAT_HOUR_SIN / FEAT_HOUR_COS — cyclical hour-of-day encoding.
    // timestamp_us=0 (e.g. tests / no-clock paths) → both zero.
    // v5.10.0b.2: sin/cos via FPN_Sin/FPN_Cos (bytewise-deterministic
    // Taylor with range reduction); double API stays for boundary-stable
    // scope (RegimeSignals.hour_sin/cos are double).
    if (timestamp_us > 0) {
        time_t t = (time_t)(timestamp_us / 1000000ULL);
        struct tm utc;
        gmtime_r(&t, &utc);
        double hour_f = (double)utc.tm_hour + (double)utc.tm_min / 60.0;
        const double TAU = 2.0 * 3.14159265358979323846;
        FPN_Binary<F> arg = FPN_FromDouble<F>(TAU * hour_f / 24.0);
        sig->hour_sin = FPN_ToDouble(FPN_Sin(arg));
        sig->hour_cos = FPN_ToDouble(FPN_Cos(arg));
    } else {
        sig->hour_sin = 0.0;
        sig->hour_cos = 0.0;
    }

    // FEAT_VOL_REGIME_RAT — current short stddev / longer-baseline stddev
    if (rolling_baseline && rolling_baseline->count >= 256 &&
        !FPN_IsZero(rolling_baseline->price_stddev) && !FPN_IsZero(rolling->price_stddev)) {
        sig->vol_regime_ratio = FPN_DivNoAssert(rolling->price_stddev, rolling_baseline->price_stddev);
    } else {
        sig->vol_regime_ratio = FPN_FromDouble<F>(1.0);  // default: no abnormality
    }

    // FEAT_TICK_RATE_Z — current ticks/sec z-score vs baseline
    sig->tick_rate_z = (tick_rate ? TickRate_CurrentZ(tick_rate) : 0.0);

    // FEAT_DIST_TO_HIGH / FEAT_DIST_TO_LOW — distance from baseline extremes
    if (rolling_baseline && rolling_baseline->count >= 256 &&
        !FPN_IsZero(rolling_baseline->price_avg)) {
        FPN_Binary<F> current = rolling->price_avg;  // using rolling avg as proxy for current price
        if (!FPN_IsZero(current)) {
            sig->dist_to_high = FPN_DivNoAssert(
                FPN_Sub(rolling_baseline->price_max, current), current);
            sig->dist_to_low = FPN_DivNoAssert(
                FPN_Sub(current, rolling_baseline->price_min), current);
        } else {
            sig->dist_to_high = FPN_Zero<F>();
            sig->dist_to_low = FPN_Zero<F>();
        }
    } else {
        sig->dist_to_high = FPN_Zero<F>();
        sig->dist_to_low = FPN_Zero<F>();
    }

    // v4.5 Wave 1 — D.1: book imbalance over time. Reads from
    // BookImbalanceHistory; mean_short over last 64 samples, mean_long
    // over full window, drift = current - mean_long. Zero-default when
    // state pointer is null OR count < 2 (cold start).
    if (book_imb_history) {
        const BookImbalanceHistory<F, 1024> *h =
            (const BookImbalanceHistory<F, 1024> *)book_imb_history;
        if (h->count >= 2) {
            // v5.15.5.D.B — MeanShortFast (O(1) running short_sum read) replaces
            // MeanShort(h, 64) (O(64) walk; ~24 cache lines / cycle). Bytewise-
            // identical for the K=64 canonical case; verified in controller_test.cpp.
            sig->book_imb_mean_short = BookImbHistory_MeanShortFast(h);
            sig->book_imb_mean_long  = BookImbHistory_MeanLong(h);
            FPN_Binary<F> last = BookImbHistory_Last(h);
            sig->book_imb_drift = FPN_Sub(last, sig->book_imb_mean_long);
        } else {
            sig->book_imb_mean_short = FPN_Zero<F>();
            sig->book_imb_mean_long  = FPN_Zero<F>();
            sig->book_imb_drift      = FPN_Zero<F>();
        }
    } else {
        sig->book_imb_mean_short = FPN_Zero<F>();
        sig->book_imb_mean_long  = FPN_Zero<F>();
        sig->book_imb_drift      = FPN_Zero<F>();
    }

    // v4.5 Wave 1 — D.2: signed-volume EWMAs. FlowState's EWMAs ARE the
    // features (no additional computation needed). Zero-default when state
    // pointer is null.
    if (flow_state) {
        const FlowState *fs = (const FlowState *)flow_state;
        sig->flow_10s = fs->ewma_10s;
        sig->flow_1m  = fs->ewma_1m;
        sig->flow_5m  = fs->ewma_5m;
    } else {
        sig->flow_10s = 0.0;
        sig->flow_1m  = 0.0;
        sig->flow_5m  = 0.0;
    }

    // v4.5 Wave 1 — D.4: large-trade z-score. Compares the most recently
    // pushed trade size against the rolling window's distribution. Zero-
    // default when state pointer is null OR window has < 2 samples.
    if (large_trade_state) {
        const LargeTradeState<F, 1024> *lt =
            (const LargeTradeState<F, 1024> *)large_trade_state;
        FPN_Binary<F> last = LargeTradeState_Last(lt);
        sig->large_trade_z = LargeTradeState_ZScore(lt, last);
    } else {
        sig->large_trade_z = 0.0;
    }

    // v4.6 Wave 2 — D.3: spread dynamics.
    //   spread_bps   = current_spread / mid_price × 10000 (basis points).
    //                  Zero-default when mid_price is zero (cold start) or
    //                  current_spread is zero (no depth feed).
    //   spread_zscore = z-score of current_spread vs SpreadState window.
    //                   Zero-default when state is null or count < 2.
    if (current_mid_price > 1e-12 && current_spread > 0.0) {
        sig->spread_bps = (current_spread / current_mid_price) * 10000.0;
    } else {
        sig->spread_bps = 0.0;
    }
    if (spread_state) {
        const SpreadState<F, 1024> *ss =
            (const SpreadState<F, 1024> *)spread_state;
        sig->spread_zscore = SpreadState_ZScore(ss,
            FPN_FromDouble<F>(current_spread));
    } else {
        sig->spread_zscore = 0.0;
    }
}

//======================================================================================================
// [STATE]
//======================================================================================================
template <unsigned F> struct RegimeState {
    int current_regime;          // REGIME_RANGING, REGIME_TRENDING, REGIME_VOLATILE
    int proposed_regime;         // what the classifier thinks (before hysteresis)
    int hysteresis_count;        // how many consecutive cycles the proposed regime has held
    int hysteresis_threshold;    // must hold for N cycles before switching (e.g. 5)
    int last_strategy_id;        // tracks which strategy was active before transition
    uint64_t regime_start_tick;  // tick at which current regime started
    time_t regime_start_time;    // wall clock time at regime start (for duration display)
    // v5.7.1: expose the regime classifier's intermediate scores so the
    // entry-quality log can record "what the classifier saw at fill time."
    // Single-source rule (EXECUTION_DISPLAY_INVARIANTS.md) — Regime_Classify
    // writes these on every cycle; entry log reads at fill moment. Reading
    // doesn't recompute. Never persisted (snapshot re-derives on warmup).
    int last_trending_score;     // last classification cycle's trending_score
    int last_volatile_score;     // last classification cycle's volatile_score
};

//======================================================================================================
// [REGIME STATE PERSISTENCE DELEGATE]  (v5.15.5.F.4d.1.E.1.2 — Step 2, D-305)
//======================================================================================================
// Field-by-field snapshot persistence for RegimeState<F>, mirroring the ConfidenceScorer
// delegate (ML_Headers/ConfidenceScore.hpp FOREACH_CONFIDENCE_PERSIST_FIELD). Wire byte
// sequence IDENTICAL to the pre-registry hand-loop (ShardedSnapshotPersist.hpp save block).
// The 2 classifier score ints (last_trending_score / last_volatile_score) are NOT persisted —
// re-derived on warmup (see above) — so a nested staging instance's copies stay uncommitted.
// Adding a persisted field = ONE registry row + a SHARDED_SNAPSHOT_VERSION bump (H21). H15:
// enrolled in CoreFrameworks/MetaRegistry.hpp. NEVER fwrite(&rs, sizeof(RegimeState)) — the
// blob carries the 2 unpersisted ints + trailing pad (48B struct vs 36B wire): field-by-field only.
//
// 7 fields IN WIRE ORDER (int x5, then uint64_t, then time_t):
#define FOREACH_REGIME_PERSIST_FIELD(X)   \
    X(current_regime,       int,      1)  \
    X(proposed_regime,      int,      1)  \
    X(hysteresis_count,     int,      1)  \
    X(hysteresis_threshold, int,      1)  \
    X(last_strategy_id,     int,      1)  \
    X(regime_start_tick,    uint64_t, 1)  \
    X(regime_start_time,    time_t,   1)

// AUTOPOPULATE fwrite — field-by-field write. Returns -1 on any fwrite failure.
#define REGIME_FWRITE_FIELD_(name, type, n)  \
    if (fwrite(&rs->name, sizeof(type), (size_t)(n), f) != (size_t)(n)) return -1;
template <unsigned F>
inline int RegimeState_FieldwiseWrite(const RegimeState<F> *rs, FILE *f) {
    FOREACH_REGIME_PERSIST_FIELD(REGIME_FWRITE_FIELD_)
    return 0;
}

// AUTOPOPULATE fread — field-by-field read into a staging instance. Same wire format.
#define REGIME_FREAD_FIELD_(name, type, n)  \
    if (fread(&rs->name, sizeof(type), (size_t)(n), f) != (size_t)(n)) return -1;
template <unsigned F>
inline int RegimeState_FieldwiseRead(RegimeState<F> *rs, FILE *f) {
    FOREACH_REGIME_PERSIST_FIELD(REGIME_FREAD_FIELD_)
    return 0;
}

// AUTOPOPULATE commit — copy the persisted subset staging->runtime (after all reads
// validate; atomicity via the commit-after-read pattern). The 2 unpersisted score ints
// are untouched, so ctx.regime_state retains its live/boot value for them.
#define REGIME_COMMIT_FIELD_(name, type, n)  \
    memcpy(&dst->name, &src->name, sizeof(type) * (size_t)(n));
template <unsigned F>
inline void RegimeState_CommitPersistedFields(RegimeState<F> *dst, const RegimeState<F> *src) {
    FOREACH_REGIME_PERSIST_FIELD(REGIME_COMMIT_FIELD_)
}

#undef REGIME_FWRITE_FIELD_
#undef REGIME_FREAD_FIELD_
#undef REGIME_COMMIT_FIELD_

// Count-lock (primary forcing function per D-302 Option B): EXACTLY 7 persisted fields.
// A row add/drop trips this at compile time → forces a SHARDED_SNAPSHOT_VERSION bump (H21).
#define REGIME_PERSIST_COUNT_ONE_(name, type, n) +1
constexpr int FOREACH_REGIME_PERSIST_FIELD_COUNT = 0 FOREACH_REGIME_PERSIST_FIELD(REGIME_PERSIST_COUNT_ONE_);
#undef REGIME_PERSIST_COUNT_ONE_
static_assert(FOREACH_REGIME_PERSIST_FIELD_COUNT == 7,
    "RegimeState wire format = EXACTLY 7 persisted fields (current/proposed_regime, "
    "hysteresis_count/threshold, last_strategy_id, regime_start_tick/time); a change "
    "requires a SHARDED_SNAPSHOT_VERSION bump + loader migration (H21).");

//======================================================================================================
// [INIT]
//======================================================================================================
template <unsigned F>
inline void Regime_Init(RegimeState<F> *state, int hysteresis_threshold) {
    state->current_regime = REGIME_RANGING;  // start conservative
    state->proposed_regime = REGIME_RANGING;
    state->hysteresis_count = 0;
    state->hysteresis_threshold = hysteresis_threshold;
    state->last_trending_score = 0;  // v5.7.1
    state->last_volatile_score = 0;
    state->last_strategy_id = STRATEGY_MEAN_REVERSION;
    state->regime_start_tick = 0;
    state->regime_start_time = time(NULL);
}

//======================================================================================================
// [CLASSIFY — SCORE-BASED]
//======================================================================================================
// each signal contributes +1 to a regime score. highest score wins.
// higher confidence = more signals agreeing = faster hysteresis passage.
//
// trending signals:
//   1. short window slope above threshold
//   2. long window slope above threshold (multi-timeframe confirmation)
//   3. short window R² above threshold (consistent direction)
//   4. ROR slope positive (trend accelerating — catches NEW trends earlier)
//   5. volume rising while price trending (volume confirmation)
//
// volatile signals:
//   1. variance ratio spiking vs baseline (relative, self-adapting)
//   2. low R² despite high variance (big moves, no direction)
//
// RANGING is the default when neither trending nor volatile has enough evidence.
//======================================================================================================
template <unsigned F>
inline int Regime_Classify(RegimeState<F> *state,
                            const RegimeSignals<F> *sig,
                            const ControllerConfig<F> *cfg) {
    // cold start: stay in RANGING until short window has enough data
    if (sig->short_count < 64)
        return state->current_regime;

    // --- trending score (EMA/SMA crossover-based) ---
    // EMA reacts every tick, SMA lags — crossover detects trends as they start
    // spread = (ema - sma) / sma: magnitude = trend strength, sign = direction
    int trending_score = 0;
    int up_signals = 0;
    int down_signals = 0;

    // signal 1: short crossover — EMA vs 128-sample SMA
    FPN_Binary<F> abs_spread = FPN_Abs(sig->ema_sma_spread);
    int crossover_strong = FPN_GreaterThan(abs_spread, cfg->regime_crossover_threshold);
    trending_score += crossover_strong;
    up_signals += crossover_strong & sig->ema_above_sma;
    down_signals += crossover_strong & !sig->ema_above_sma;

    // signal 2: long crossover — EMA vs 512-sample SMA (multi-timeframe confirmation)
    FPN_Binary<F> abs_spread_long = FPN_Abs(sig->ema_sma_spread_long);
    int long_has_data = (sig->long_count >= 64);
    int long_ema_above = (sig->ema_sma_spread_long.v > 0);   // strictly positive (was !sign & !IsZero; 16B)
    int long_crossover_strong = long_has_data &
        FPN_GreaterThan(abs_spread_long, cfg->regime_crossover_threshold);
    trending_score += long_crossover_strong;
    up_signals += long_crossover_strong & long_ema_above;
    down_signals += long_crossover_strong & !long_ema_above;

    // hidden downtrend: EMA far below long SMA + short crossover neutral
    // catches macro downtrends where short window mean-reverts inside the trend
    int long_down_only = long_has_data
        & FPN_GreaterThan(abs_spread_long, FPN_Mul(cfg->regime_crossover_threshold, FPN_FromDouble<F>(2.0)))
        & !long_ema_above & !crossover_strong;
    down_signals += long_down_only;
    trending_score += long_down_only;

    // signal 4: price movement is consistent (high R² — orthogonal to crossover)
    int consistent = FPN_GreaterThan(sig->short_r2, cfg->regime_r2_threshold);
    trending_score += consistent;

    // signal 5: trend is accelerating (ROR — orthogonal, catches steepening trends)
    int ror_positive = sig->ror_ready & FPN_GreaterThan(sig->ror_slope, FPN_Zero<F>());
    int ror_negative = sig->ror_ready & FPN_LessThan(sig->ror_slope, FPN_Zero<F>());
    trending_score += (ror_positive | ror_negative);
    up_signals += ror_positive;
    down_signals += ror_negative;

    // signal 6: volume rising in direction of crossover (confirmation)
    int vol_confirms = FPN_GreaterThan(sig->volume_slope, FPN_Zero<F>()) & crossover_strong;
    trending_score += vol_confirms;

    // signal 7: ML model regime enrichment (Mode A)
    // model_score > 0.5 = model predicts trending, weighted by regime_model_weight
    if (!FPN_IsZero(sig->model_score)) {
        int model_trending = FPN_GreaterThan(sig->model_score, FPN_FromDouble<F>(0.5));
        int weight = (int)FPN_ToDouble(cfg->regime_model_weight);
        trending_score += model_trending * weight;
    }

    // --- volatile score (unchanged — vol_ratio based) ---
    int volatile_score = 0;

    // variance spike relative to longer-term baseline (self-adapting)
    int vol_spike = FPN_GreaterThan(sig->vol_ratio, cfg->regime_vol_spike_ratio);
    volatile_score += vol_spike;

    // high variance but no consistent direction (choppy)
    int inconsistent = !consistent;
    volatile_score += vol_spike & inconsistent;

    // --- classify ---
    // trending needs at least 2 signals AND at least one crossover signal
    // volatile needs at least 2 signals (spike + no direction)
    // direction: more down signals = TRENDING_DOWN, otherwise TRENDING (up)
    // uptrend split: strong crossover = TRENDING (momentum), mild = MILD_TREND (EMA Cross)
    int has_crossover = crossover_strong | long_crossover_strong;
    int detected;
    if (trending_score >= 2 && has_crossover && consistent && trending_score > volatile_score) {
        if (down_signals > up_signals) {
            detected = REGIME_TRENDING_DOWN;
        } else {
            // split uptrend: use max of short/long spread for strength assessment
            FPN_Binary<F> max_spread = FPN_Max(abs_spread, abs_spread_long);
            int strong = FPN_GreaterThan(max_spread, cfg->regime_strong_crossover);
            detected = strong ? REGIME_TRENDING : REGIME_MILD_TREND;
        }
    } else if (volatile_score >= 2 && volatile_score > trending_score)
        detected = REGIME_VOLATILE;
    else
        detected = REGIME_RANGING;

    // v5.7.1: persist the scores so the entry-quality log can read them
    // at fill time without recomputing. Single-source rule.
    state->last_trending_score = trending_score;
    state->last_volatile_score = volatile_score;

    // hysteresis: proposed regime must hold for N consecutive cycles before switching
    if (detected == state->proposed_regime) {
        state->hysteresis_count++;
    } else {
        state->proposed_regime = detected;
        state->hysteresis_count = 1;
    }

    if (state->hysteresis_count >= state->hysteresis_threshold
        && state->proposed_regime != state->current_regime) {
        state->current_regime = state->proposed_regime;
    }

    // v5.4.0 Phase A.2 diagnostic — gated on TT_REGIME_DEBUG env var.
    // Prints every 16th call to avoid stderr spam. Logs all the inputs
    // the classifier saw, so we can tell whether the classifier is
    // receiving sane data or whether the regression is upstream
    // (data-flow / per-core slow_state population issue).
    {
        static int debug_enabled = -1;
        if (debug_enabled == -1) {
            const char* e = getenv("TT_REGIME_DEBUG");
            debug_enabled = (e && e[0] && e[0] != '0') ? 1 : 0;
        }
        if (debug_enabled) {
            // Per-engine call counter; thread_local so per_node_slow's
            // independent threads don't race on it. Cap at 16 engines.
            static thread_local uint32_t dbg_cycle = 0;
            dbg_cycle++;
            if ((dbg_cycle & 0x0F) == 0) {  // every 16 cycles
                fprintf(stderr,  // [LAT_EXEMPT]_[env-gated cold debug]
                    "[regime-dbg] short_count=%d ema_sma_spread=%.6f "
                    "long_spread=%.6f r2=%.4f ror_slope=%.6f ror_ready=%d "
                    "vol_ratio=%.4f trending=%d volatile=%d "
                    "detected=%d current=%d proposed=%d hyst=%d/%d\n",
                    sig->short_count,
                    FPN_ToDouble(sig->ema_sma_spread),
                    FPN_ToDouble(sig->ema_sma_spread_long),
                    FPN_ToDouble(sig->short_r2),
                    FPN_ToDouble(sig->ror_slope),
                    sig->ror_ready,
                    FPN_ToDouble(sig->vol_ratio),
                    trending_score, volatile_score,
                    detected, state->current_regime, state->proposed_regime,
                    state->hysteresis_count, state->hysteresis_threshold);
            }
        }
    }

    return state->current_regime;
}

//======================================================================================================
// [STRATEGY MAPPING]
//======================================================================================================
static inline int Regime_ToStrategy(int regime) {
    return (regime >= 0 && regime < NUM_REGIMES)
        ? REGIME_STRATEGY_TABLE[regime] : STRATEGY_MEAN_REVERSION;
}

//======================================================================================================
// [ADJUST POSITIONS ON REGIME SWITCH]
//======================================================================================================
// called once when current_regime changes. walks active positions and adjusts TP/SL
// to match the new regime's risk profile.
//
// MR → momentum: widen TP (let trend run), tighten SL (cut if wrong)
// momentum → MR: tighten TP (take profit before reversal), widen SL (allow chop)
// volatile: no adjustment (panic-adjusting in volatility causes more harm)
//
// only adjusts positions entered under the PREVIOUS strategy.
//======================================================================================================
template <unsigned F>
inline void Regime_AdjustPositions(Portfolio<F> *portfolio,
                                     const RollingStats<F> *rolling,
                                     int old_regime, int new_regime,
                                     const uint8_t *entry_strategy,
                                     const ControllerConfig<F> *cfg) {
    int old_strategy = Regime_ToStrategy(old_regime);
    FPN_Binary<F> stddev = rolling->price_stddev;

    // guard: flat market (stddev=0) would produce zero offsets → TP=SL=entry → immediate exit
    if (FPN_IsZero(stddev)) return;

    FPN_Binary<F> hundred = FPN_FromDouble<F>(100.0);
    Money half = cfg->min_sl_tp_ratio;

    uint16_t active = portfolio->active_bitmap;
    while (active) {
        int idx = __builtin_ctz(active);

        if (entry_strategy[idx] == old_strategy) {
            Position<F> *pos = &portfolio->positions[idx];

            if (old_regime == REGIME_RANGING && new_regime == REGIME_TRENDING) {
                // momentum_tp/sl_mult are direct stddev multipliers — no ×100
                FPN_Binary<F> wide_tp_offset = FPN_Mul(stddev, cfg->momentum_tp_mult);
                Money wide_tp = Money_Add(pos->entry_price, Money_FromBinary(wide_tp_offset));  // D-170 feature->money egress
                pos->take_profit_price = Money_Max(pos->take_profit_price, wide_tp);

                FPN_Binary<F> tight_sl_offset = FPN_Mul(stddev, cfg->momentum_sl_mult);
                Money tight_sl = Money_Sub(pos->entry_price, Money_FromBinary(tight_sl_offset));  // D-170 feature->money egress
                pos->stop_loss_price = Money_Max(pos->stop_loss_price, tight_sl);

                // SL floor: ensure SL distance >= 0.5 × TP distance (2:1 min reward/risk)
                Money tp_dist = Money_Sub(pos->take_profit_price, pos->entry_price);
                Money min_sl_dist = Money_Mul(tp_dist, half);
                Money sl_floor = Money_Sub(pos->entry_price, min_sl_dist);  // D-170 feature->money egress
                pos->stop_loss_price = Money_Min(pos->stop_loss_price, sl_floor);
            }
            // RANGING → MILD_TREND: both buy dips, widen TP slightly (uptrend confirmed)
            else if (old_regime == REGIME_RANGING && new_regime == REGIME_MILD_TREND) {
                FPN_Binary<F> mr_tp_offset = FPN_Mul(stddev, FPN_Mul(Money_ToBinary(cfg->take_profit_pct), hundred));
                FPN_Binary<F> mild_widen = FPN_Mul(mr_tp_offset, FPN_FromDouble<F>(1.3));
                Money wider_tp = Money_Add(pos->entry_price, Money_FromBinary(mild_widen));  // D-170 feature->money egress
                pos->take_profit_price = Money_Max(pos->take_profit_price, wider_tp);

                Money tp_dist = Money_Sub(pos->take_profit_price, pos->entry_price);
                Money min_sl_dist = Money_Mul(tp_dist, half);
                Money sl_floor = Money_Sub(pos->entry_price, min_sl_dist);  // D-170 feature->money egress
                pos->stop_loss_price = Money_Min(pos->stop_loss_price, sl_floor);
            }
            // MILD_TREND → TRENDING: uptrend strengthening, widen TP to momentum levels
            else if (old_regime == REGIME_MILD_TREND && new_regime == REGIME_TRENDING) {
                FPN_Binary<F> wide_tp_offset = FPN_Mul(stddev, cfg->momentum_tp_mult);
                Money wide_tp = Money_Add(pos->entry_price, Money_FromBinary(wide_tp_offset));  // D-170 feature->money egress
                pos->take_profit_price = Money_Max(pos->take_profit_price, wide_tp);

                FPN_Binary<F> tight_sl_offset = FPN_Mul(stddev, cfg->momentum_sl_mult);
                Money tight_sl = Money_Sub(pos->entry_price, Money_FromBinary(tight_sl_offset));  // D-170 feature->money egress
                pos->stop_loss_price = Money_Max(pos->stop_loss_price, tight_sl);

                Money tp_dist = Money_Sub(pos->take_profit_price, pos->entry_price);
                Money min_sl_dist = Money_Mul(tp_dist, half);
                Money sl_floor = Money_Sub(pos->entry_price, min_sl_dist);  // D-170 feature->money egress
                pos->stop_loss_price = Money_Min(pos->stop_loss_price, sl_floor);
            }
            // TRENDING/TRENDING_DOWN/MILD_TREND → RANGING: tighten TP, widen SL for chop
            else if ((old_regime == REGIME_TRENDING || old_regime == REGIME_TRENDING_DOWN
                      || old_regime == REGIME_MILD_TREND)
                     && new_regime == REGIME_RANGING) {
                FPN_Binary<F> tight_tp_offset = FPN_Mul(stddev, FPN_Mul(Money_ToBinary(cfg->take_profit_pct), hundred));
                Money tight_tp = Money_Add(pos->entry_price, Money_FromBinary(tight_tp_offset));  // D-170 feature->money egress
                pos->take_profit_price = Money_Min(pos->take_profit_price, tight_tp);

                // fee floor: TP must cover round-trip fees even after tightening
                Money fee_floor = Money_Add(pos->entry_price,
                    Money_Mul(Money_Mul(pos->entry_price, cfg->fee_rate), cfg->fee_floor_mult));
                pos->take_profit_price = Money_Max(pos->take_profit_price, fee_floor);

                FPN_Binary<F> wide_sl_offset = FPN_Mul(stddev, FPN_Mul(Money_ToBinary(cfg->stop_loss_pct), hundred));
                Money wide_sl = Money_Sub(pos->entry_price, Money_FromBinary(wide_sl_offset));  // D-170 feature->money egress
                pos->stop_loss_price = Money_Min(pos->stop_loss_price, wide_sl);

                // SL ceiling: if TP was tightened (vol dropped since fill), don't let SL
                // stay at the old high-vol width. SL distance must not exceed TP distance.
                // without this, a fill at high σ followed by regime switch at low σ
                // produces SL > TP (inverted risk/reward)
                Money tp_dist = Money_Sub(pos->take_profit_price, pos->entry_price);
                Money sl_ceiling = Money_Sub(pos->entry_price, tp_dist);
                pos->stop_loss_price = Money_Max(pos->stop_loss_price, sl_ceiling);

                // SL floor: ensure SL distance >= 0.5 × TP distance (2:1 min reward/risk)
                Money min_sl_dist = Money_Mul(tp_dist, half);
                Money sl_floor = Money_Sub(pos->entry_price, min_sl_dist);  // D-170 feature->money egress
                pos->stop_loss_price = Money_Min(pos->stop_loss_price, sl_floor);
            }
            // TRENDING → MILD_TREND: trend weakening, tighten TP moderately
            else if (old_regime == REGIME_TRENDING && new_regime == REGIME_MILD_TREND) {
                FPN_Binary<F> mr_tp_offset = FPN_Mul(stddev, FPN_Mul(Money_ToBinary(cfg->take_profit_pct), hundred));
                FPN_Binary<F> mild_tp = FPN_Mul(mr_tp_offset, FPN_FromDouble<F>(1.3));
                Money tighter_tp = Money_Add(pos->entry_price, Money_FromBinary(mild_tp));  // D-170 feature->money egress
                pos->take_profit_price = Money_Min(pos->take_profit_price, tighter_tp);

                // fee floor: TP must cover round-trip fees even after tightening
                Money fee_floor = Money_Add(pos->entry_price,
                    Money_Mul(Money_Mul(pos->entry_price, cfg->fee_rate), cfg->fee_floor_mult));
                pos->take_profit_price = Money_Max(pos->take_profit_price, fee_floor);

                // SL ceiling + floor
                Money tp_dist = Money_Sub(pos->take_profit_price, pos->entry_price);
                Money sl_ceiling = Money_Sub(pos->entry_price, tp_dist);
                pos->stop_loss_price = Money_Max(pos->stop_loss_price, sl_ceiling);
                Money min_sl_dist = Money_Mul(tp_dist, half);
                Money sl_floor = Money_Sub(pos->entry_price, min_sl_dist);  // D-170 feature->money egress
                pos->stop_loss_price = Money_Min(pos->stop_loss_price, sl_floor);
            }
            // entering downtrend: tighten TP (take profits), tighten SL (cut losses)
            else if (new_regime == REGIME_TRENDING_DOWN) {
                FPN_Binary<F> tight_tp_offset = FPN_Mul(stddev, cfg->momentum_tp_mult);
                Money tight_tp = Money_Add(pos->entry_price, Money_FromBinary(tight_tp_offset));  // D-170 feature->money egress
                pos->take_profit_price = Money_Min(pos->take_profit_price, tight_tp);

                // fee floor: TP must cover round-trip fees even after tightening
                Money fee_floor = Money_Add(pos->entry_price,
                    Money_Mul(Money_Mul(pos->entry_price, cfg->fee_rate), cfg->fee_floor_mult));
                pos->take_profit_price = Money_Max(pos->take_profit_price, fee_floor);

                FPN_Binary<F> tight_sl_offset = FPN_Mul(stddev, cfg->momentum_sl_mult);
                Money tight_sl = Money_Sub(pos->entry_price, Money_FromBinary(tight_sl_offset));  // D-170 feature->money egress
                pos->stop_loss_price = Money_Max(pos->stop_loss_price, tight_sl);

                // SL ceiling: SL distance must not exceed TP distance (no inverted risk/reward)
                Money tp_dist = Money_Sub(pos->take_profit_price, pos->entry_price);
                Money sl_ceiling = Money_Sub(pos->entry_price, tp_dist);
                pos->stop_loss_price = Money_Max(pos->stop_loss_price, sl_ceiling);

                // SL floor: ensure SL distance >= 0.5 × TP distance (2:1 min reward/risk)
                Money min_sl_dist = Money_Mul(tp_dist, half);
                Money sl_floor = Money_Sub(pos->entry_price, min_sl_dist);  // D-170 feature->money egress
                pos->stop_loss_price = Money_Min(pos->stop_loss_price, sl_floor);
            }
        }

        active &= active - 1;
    }
}

#endif // REGIME_DETECTOR_HPP
