// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[Backtest/LabelFunctions.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML] [BACKTEST]]
// [SEAM]_[train-serve label-set identity — LABEL_REGISTRY_HASH in every stamp; mismatch = load-time rejection]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[table-driven ML target-label SSoT — one FOREACH_TARGET row auto-flows the LABEL enum + the label table + the registry hash; the forward-scan compute leaves + kind helpers share the registry block]
// [CONTAINS]
//   - [STRUCT]_[HistoricalTick]
//   - [REGISTRY]_[FOREACH_TARGET]   (the LABEL enum + 11 Label_* leaves + label_table + LABEL_REGISTRY_HASH + LabelType_* helpers share the block)
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
// NUM_REGIMES — the REGIME row's num_classes is compile-time BOUND to the
// FOREACH_REGIME SSoT (static_assert below the label table; TD-241 close).
// StrategyInterface is dependency-light (cstdint/cstddef only).
#include "../Strategies/StrategyInterface.hpp"
#include <math.h>   // NAN/isnan — the forward-scan leaves return NAN for
                     // not-enough-history (E.1.2.D leaf 13 / S3-F8: this rode a
                     // transitive include before; now explicit)

//======================================================================
// [STRUCT]_[HistoricalTick]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[one historical aggTrade record — the single definition point; the tick array the forward-scan labels read]
//======================================================================
// [CODE]
//======================================================================
struct HistoricalTick {
    double price;
    double qty;
    int64_t timestamp_us; // Binance aggTrades: microseconds since epoch (NOT milliseconds)
    int is_buyer_maker;
};
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// HistoricalTick lives here (single definition point).
// BacktestEngine.hpp includes this file.
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
// [SIZE]_[32B]
// [ALIGN]_[8]
// [CACHE_LINES]_[1]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[HistoricalTick]
//======================================================================

//======================================================================
// [REGISTRY]_[FOREACH_TARGET]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML] [FRAMEWORK_DISCIPLINE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the target-label registry — one row auto-flows the LABEL enum, the label table, and the FNV registry hash; APPEND-ONLY, reorder/remove flips the hash and refuses old stamps]
// [COLUMN]_[id_suffix]_[UPPERCASE token -> LABEL_<id_suffix> enum; order LOCKED + append-only, stamps hash the order]
// [COLUMN]_[name]_[cfg parser key + stamp body identifier; folded into LABEL_REGISTRY_HASH]
// [COLUMN]_[display]_[GUI dropdown string]
// [COLUMN]_[desc]_[human description]
// [COLUMN]_[fn]_[Label_<Name> compute leaf matching the LabelFn signature]
// [COLUMN]_[num_classes]_[0 binary, 1 regression, 2-or-more multiclass; ":nc"+value folded into the hash]
// [REFERENCE]_[INVARIANT]_[[H15] [H21]]
// [REFERENCE]_[TECH_DEBT]_[TECH_DEBT-241]
//======================================================================
// [CODE]
//======================================================================
#define FOREACH_TARGET(X) \
    X(WIN_LOSS,           "win_loss",           "Win/Loss",           "Binary: 1=profitable entry, 0=loss",                 Label_WinLoss,           0, TP_PCT) \
    X(BARRIER,            "barrier",            "Barrier",            "First-passage: +tp% before -sl% (0.5=neutral)",      Label_Barrier,           0, TP_PCT) \
    X(FORWARD_PNL,        "forward_pnl",        "Forward P&L",        "Continuous: % return over N ticks",                  Label_ForwardPnl,        1, TP_UNUSED) \
    X(REGIME,             "regime",             "Regime",             "Multi-class: regime at sample point",                Label_Regime,            5, TP_UNUSED) \
    X(VOL_BARRIER,        "vol_barrier",        "Vol Barrier",        "Vol-scaled: k*sigma barrier (FoxML)",                Label_VolBarrier,        0, TP_SIGMA_K) \
    X(WILL_PEAK,          "will_peak",          "Will Peak",          "Binary: 1=price peaks within N ticks",               Label_WillPeak,          0, TP_UNUSED) \
    X(WILL_VALLEY,        "will_valley",        "Will Valley",        "Binary: 1=price valleys within N ticks",             Label_WillValley,        0, TP_UNUSED) \
    X(PEAK_VALLEY_STABLE, "peak_valley_stable", "Peak/Valley/Stable", "3-class: 0=stable, 1=peak, 2=valley (softmax)",      Label_PeakValleyStable,  3, TP_PCT) \
    /* v5.14.5.A — cross-sectional targets (FoxML_Core port). Single-symbol */ \
    /*               mode = degenerate (returns raw return per Compute fn);   */ \
    /*               CS aggregation activates with multi-symbol (v5.16+).     */ \
    X(CS_PERCENTILE_RANK,    "cs_percentile_rank",    "CS Percentile Rank",    "Per-timestamp rank/(N+1); SS-degenerate=raw return",       Label_CSPercentileRank,    1, TP_UNUSED) \
    X(CS_ZSCORE_ROBUST,      "cs_zscore_robust",      "CS Robust Z-Score",     "Per-timestamp (r-median)/(1.4826*MAD); SS-degenerate=raw", Label_CSZScoreRobust,      1, TP_UNUSED) \
    X(CS_VOLSCALED_DEMEANED, "cs_volscaled_demeaned", "CS Vol-scaled Demeaned","Per-timestamp (r/vol)-mean(r/vol); SS-degenerate=raw",     Label_CSVolScaledDemeaned, 1, TP_UNUSED) \
    /* E.1.2.G — 3-class sister of VOL_BARRIER. APPENDED, never inserted: LABEL_* */ \
    /*            values are persisted via label_registry_hash, so the position   */ \
    /*            of every existing row is frozen (H21).                          */ \
    X(VOL_BARRIER_3C,     "vol_barrier_3c",     "Vol Barrier 3-Class","3-class k*sigma: 0=stable, 1=down-first(peak), 2=up-first(valley)", Label_VolBarrier3C, 3, TP_SIGMA_K)

// Auto-generated LABEL_* constants. Order matches FOREACH_TARGET.
// Trailing LABEL_COUNT_AUTO acts as the count (one past the last value).
enum {
#define X(id_suffix, name, display, desc, fn, nc, tpk) LABEL_##id_suffix,
    FOREACH_TARGET(X)
#undef X
    LABEL_COUNT_AUTO
};

//----------------------------------------------------------------------
// [SECTION]_[tp_kind — what the 5th label-fn argument MEANS]
//----------------------------------------------------------------------
// s5 (2026-08-23) — the `tp_kind` registry column exists because the 5th
// parameter of a LabelFn is NOT one thing. Three different meanings share
// that slot:
//   TP_PCT      — a percent-of-price up-barrier: price*(1 + tp/100).
//                 WIN_LOSS, BARRIER, PEAK_VALLEY_STABLE.
//   TP_SIGMA_K  — a SIGMA MULTIPLIER: price*(1 + k*rolling_vol). VOL_BARRIER
//                 names the same parameter `barrier_k` for exactly this reason
//                 (LabelFunctions.hpp ~:205).
//   TP_UNUSED   — the leaf ignores it outright (`(void)tp_pct;` or an unnamed
//                 parameter). Seven of the eleven rows.
//
// WHY A COLUMN AND NOT AN if-CHAIN AT THE CONSUMER: a hardcoded label-name
// list living in BacktestEngine.hpp is the Class-19 shape, and it cannot force
// a NEW row to declare itself. As a column, a 12th FOREACH_TARGET row will not
// COMPILE until its author states what its 5th argument means — the same
// completeness discipline `num_classes` already carries. That is the whole
// point: the defect this closes (a percent added to a multiplier) was invisible
// precisely because nothing in the type system knew the units differed.
//
// NOT folded into label_registry_hash_compute() — the hash folds `name` and
// `num_classes` only, so adding this column leaves LABEL_REGISTRY_HASH()
// unchanged and every existing stamp keeps loading. Deliberate: the column
// describes what a parameter ALWAYS meant; it does not change any label's
// output. (A fee-bearing VOL_BARRIER run genuinely changes semantics, but that
// combination was a bug, not a shipped behaviour worth preserving.)
enum LabelTpKind {
    TP_UNUSED = 0,   // leaf ignores the parameter
    TP_PCT,          // percent-of-price barrier
    TP_SIGMA_K,      // sigma multiplier (k * rolling_vol)
};

//----------------------------------------------------------------------
// [SECTION]_[WIN/LOSS]
//----------------------------------------------------------------------
// looks forward to the next completed trade — was it profitable?
// simplest label: 1 = win, 0 = loss. uses the existing engine's exit logic.
// note: labels trades, not ticks. many ticks will have label=0 because
// no trade was entered at that point.
//----------------------------------------------------------------------
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

//----------------------------------------------------------------------
// [SECTION]_[BARRIER]
//----------------------------------------------------------------------
// first-passage label from FoxML/private: will price hit +X% before -Y%?
// tp_pct and sl_pct are the barrier sizes (e.g. 1.5 = 1.5%).
// same as win/loss but with configurable asymmetric barriers.
//----------------------------------------------------------------------
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

//----------------------------------------------------------------------
// [SECTION]_[FORWARD P&L]
//----------------------------------------------------------------------
// continuous label: return over the next N ticks.
// useful for regression (predict magnitude, not just direction).
//----------------------------------------------------------------------
static inline float Label_ForwardPnl(const HistoricalTick *ticks, int tick_idx, int total_ticks,
                                      double sample_price, double /* tp_pct */, double /* sl_pct */,
                                      int forward_ticks) {
    int target_idx = tick_idx + forward_ticks;
    if (target_idx >= total_ticks) target_idx = total_ticks - 1;
    if (target_idx <= tick_idx) return 0.0f;

    double future_price = ticks[target_idx].price;
    return (float)((future_price - sample_price) / sample_price * 100.0); // % return
}

//----------------------------------------------------------------------
// [SECTION]_[REGIME]
//----------------------------------------------------------------------
// which regime was the engine in at this sample point?
// values follow the FOREACH_REGIME SSoT (Strategies/StrategyInterface.hpp):
// 0=RANGING, 1=TRENDING, 2=VOLATILE, 3=TRENDING_DOWN, 4=MILD_TREND.
// useful for training a regime classifier model.
// TECH_DEBT-241 CLOSED (2026-08-26): the row's num_classes is 5 again and a
// static_assert below the table BINDS it to NUM_REGIMES, so adding a 6th
// regime without bumping the row is a COMPILE ERROR (a sampled MILD_TREND
// used to hand XGBoost label 4.0 under num_class=4 → train-time refusal).
// The row keeps a LITERAL deliberately: the registry hash stringizes the nc
// token (":nc" #nc), so writing NUM_REGIMES in the row would freeze the hash
// across a future regime-count change — the exact drift the hash exists to
// refuse. Hash flip greenlit per project_no_live_models (epoch break free).
//----------------------------------------------------------------------
static inline float Label_Regime(const HistoricalTick * /* ticks */, int /* tick_idx */,
                                  int /* total_ticks */, double /* sample_price */,
                                  double /* tp_pct */, double /* sl_pct */,
                                  int regime_at_sample) {
    return (float)regime_at_sample;
}

//----------------------------------------------------------------------
// [SECTION]_[VOL-SCALED BARRIER]
//----------------------------------------------------------------------
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
//----------------------------------------------------------------------
// E.1.2.G — extracted so the binary VOL_BARRIER and the 3-class VOL_BARRIER_3C
// compute sigma from ONE body (SSoT). Pure code motion: same operations in the
// same order, so both labels stay bytewise identical to the pre-extraction
// VOL_BARRIER. Returns 1 with *out_vol written, or 0 for "insufficient history
// or degenerate vol" — the caller supplies its own neutral class, because the
// binary encoding (0.5 = neutral) and the 3-class encoding (0 = stable) differ.
static inline int LabelVol_RollingSigma(const HistoricalTick *ticks, int tick_idx,
                                        int vol_window, double *out_vol) {
    // need at least min_periods returns to compute vol (FoxML: min_periods = 5)
    int min_periods = 5;
    if (tick_idx < min_periods + 1) return 0;

    int start = tick_idx - vol_window;
    if (start < 1) start = 1;              // need at least 1 prior tick for returns
    int n_returns = tick_idx - start;
    if (n_returns < min_periods) return 0;

    double sum = 0.0, sum_sq = 0.0;
    for (int j = start; j < tick_idx; j++) {
        if (ticks[j - 1].price <= 0.0) continue;
        double r = (ticks[j].price - ticks[j - 1].price) / ticks[j - 1].price;
        sum += r;
        sum_sq += r * r;
    }

    double mean = sum / n_returns;
    double variance = (sum_sq / n_returns) - (mean * mean);
    if (variance <= 0.0) return 0;          // zero vol = no signal

    // manual sqrt to avoid pulling in math.h just for this (Newton, 8 iters)
    double x = variance;
    double guess = x * 0.5;
    if (guess <= 0.0) guess = 1e-10;
    for (int iter = 0; iter < 8; iter++)
        guess = 0.5 * (guess + x / guess);
    if (guess <= 1e-15) return 0;           // degenerate

    *out_vol = guess;
    return 1;
}

static inline float Label_VolBarrier(const HistoricalTick *ticks, int tick_idx, int total_ticks,
                                      double sample_price, double barrier_k, double /* sl_pct */,
                                      int vol_window) {
    // parameter defaults (from FoxML barrier.py)
    if (barrier_k <= 0.0) barrier_k = 0.5;   // FoxML: barrier_size = 0.5
    if (vol_window <= 0) vol_window = 20;     // FoxML: vol_window = 20

    double vol = 0.0;
    if (!LabelVol_RollingSigma(ticks, tick_idx, vol_window, &vol)) return 0.5f;

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

//----------------------------------------------------------------------
// [SECTION]_[VOL_BARRIER_3C]
//----------------------------------------------------------------------
// E.1.2.G — the 3-class sister of VOL_BARRIER. Same k*sigma first-passage
// geometry, but emitted as softmax classes instead of a binary-with-neutral,
// so class balance stays stable across volatility regimes: the barriers scale
// with realized vol, where a fixed-pct barrier family (PEAK_VALLEY_STABLE) has
// its class mix drift as vol does. That drift is the leading suspect behind
// train-below-pooled-baseline across time folds.
//
// Class encoding deliberately matches PEAK_VALLEY_STABLE's DIRECTION so the two
// 3-class targets stay mentally and numerically interchangeable downstream:
//   0 = neither barrier hit within the window  (stable / timeout)
//   1 = DOWN barrier first  -> you were at a peak    -> BAD entry
//   2 = UP   barrier first  -> you were at a valley  -> GOOD entry
// (PVS semantics verified at LabelFunctions.hpp Label_PeakValleyStable; the
// same 2=good-entry reading applies here.)
//
// Note the neutral differs from the binary sister BY DESIGN: VOL_BARRIER returns
// 0.5 for "no signal", which is a legitimate midpoint in a binary encoding but
// would be an invalid class index under softmax. Here insufficient history and
// timeout both fold into class 0.
static inline float Label_VolBarrier3C(const HistoricalTick *ticks, int tick_idx, int total_ticks,
                                        double sample_price, double barrier_k, double /* sl_pct */,
                                        int vol_window) {
    if (barrier_k <= 0.0) barrier_k = 0.5;
    if (vol_window <= 0) vol_window = 20;

    double vol = 0.0;
    if (!LabelVol_RollingSigma(ticks, tick_idx, vol_window, &vol)) return 0.0f;  // class 0 = stable

    double up_barrier   = sample_price * (1.0 + barrier_k * vol);
    double down_barrier = sample_price * (1.0 - barrier_k * vol);

    for (int j = tick_idx + 1; j < total_ticks; j++) {
        if (ticks[j].price >= up_barrier)   return 2.0f;  // up first  = valley = GOOD entry
        if (ticks[j].price <= down_barrier) return 1.0f;  // down first = peak  = bad entry
    }
    return 0.0f; // neither = stable
}

//----------------------------------------------------------------------
// [SECTION]_[WILL_PEAK / WILL_VALLEY]
//----------------------------------------------------------------------
// binary labels for barrier gate model training.
// WILL_PEAK: 1 if price reaches a local max within N ticks (extra_param = lookahead)
// WILL_VALLEY: 1 if price reaches a local min within N ticks
// "local max/min" = price is highest/lowest in a symmetric window around it
//----------------------------------------------------------------------
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

//----------------------------------------------------------------------
// [SECTION]_[PEAK_VALLEY_STABLE — 3-class softmax target for BarrierGate primary path]
//----------------------------------------------------------------------
// Returns: 0 = stable (neither barrier hit), 1 = peak (down barrier hit first),
//          2 = valley (up barrier hit first)
// Used to train an XGBoost multi:softprob model that outputs P(stable)/P(peak)/P(valley).
// Same first-passage scan as Label_Barrier but with explicit stable class (returns
// the third bucket as a proper class instead of the 0.5 neutral float).
//
// tp_pct = up barrier (price reaches +tp% → "valley" since the entry was at a low)
// sl_pct = down barrier (price reaches -sl% → "peak" since the entry was at a high)
// extra_param = lookahead ticks (default 500). 0 means "scan to end of data."
//----------------------------------------------------------------------
static float Label_PeakValleyStable(const HistoricalTick *ticks, int tick_idx, int total_ticks,
                                    double sample_price, double tp_pct, double sl_pct,
                                    int extra_param) {
    double up_barrier   = sample_price * (1.0 + tp_pct / 100.0);
    double down_barrier = sample_price * (1.0 - sl_pct / 100.0);
    int lookahead = (extra_param > 0) ? extra_param : 500;
    int end = tick_idx + lookahead;
    if (end > total_ticks) end = total_ticks;

    for (int j = tick_idx + 1; j < end; j++) {
        if (ticks[j].price >= up_barrier)   return 2.0f;  // up first → was at valley → good entry
        if (ticks[j].price <= down_barrier) return 1.0f;  // down first → was at peak → bad entry
    }
    return 0.0f;  // neither hit within lookahead → stable
}

//----------------------------------------------------------------------
// [SECTION]_[v5.14.5.A — CROSS-SECTIONAL TARGETS (FoxML_Core port)]
//----------------------------------------------------------------------
// Cross-sectional labels normalize/rank returns ACROSS symbols at each
// timestamp. Today's engine is single-symbol (BTCUSDT) so the
// cross-section size = 1 and all 3 metrics DEGENERATE to identity:
//   - rank/(N+1) = 1/2 = 0.5 always (constant)
//   - (r - median(r))/(MAD + ε) ≈ 0 always (single value = its own median)
//   - (r/vol) - mean((r/vol)) = 0 always
//
// To preserve OPTIONALITY for future v5.16+ multi-symbol port, the
// Compute fns return raw `future_return` for now. The CS aggregation
// (rank, median, mean) lives at BacktestResults level when multi-symbol
// streaming lands. Today, training on CS_* in single-symbol mode
// produces identical models to FORWARD_PNL — operator should be aware
// (see engine.cfg.example v5.14.5.A note).
//
// CALLER convention: same LabelFn signature as existing labels (matches
// LabelFn typedef at LabelFunctions.hpp:~290). tp_pct/sl_pct ignored
// (no barriers); extra_param = forward_ticks for horizon.
//
// FUTURE-THINKING (v5.16+ multi-symbol activation):
// - CS_PERCENTILE_RANK: replace `return future_return` with cross-section
//   rank lookup at BacktestResults level. Plumbing here is correct.
// - CS_ZSCORE_ROBUST: same, with median + MAD computed over symbols.
// - CS_VOLSCALED_DEMEANED: vol estimate sourced from per-symbol rolling
//   stats; mean computed across symbols.
// All 3 maintain identical signatures + return types; aggregation step
// is downstream of these Compute fns.

// CS Percentile Rank — single-symbol degenerate path returns raw return.
static float Label_CSPercentileRank(const HistoricalTick *ticks, int tick_idx, int total_ticks,
                                      double sample_price, double /*tp_pct*/, double /*sl_pct*/,
                                      int extra_param) {
    int forward_ticks = (extra_param > 0) ? extra_param : 100;
    if (tick_idx + forward_ticks >= total_ticks) return NAN;
    if (sample_price <= 0.0) return NAN;
    double future_return = (ticks[tick_idx + forward_ticks].price - sample_price) / sample_price;
    return (float)future_return;
}

// CS Robust Z-Score — single-symbol degenerate path returns raw return.
static float Label_CSZScoreRobust(const HistoricalTick *ticks, int tick_idx, int total_ticks,
                                    double sample_price, double /*tp_pct*/, double /*sl_pct*/,
                                    int extra_param) {
    int forward_ticks = (extra_param > 0) ? extra_param : 100;
    if (tick_idx + forward_ticks >= total_ticks) return NAN;
    if (sample_price <= 0.0) return NAN;
    double future_return = (ticks[tick_idx + forward_ticks].price - sample_price) / sample_price;
    return (float)future_return;
}

// CS Vol-Scaled Demeaned — single-symbol degenerate path returns raw return.
static float Label_CSVolScaledDemeaned(const HistoricalTick *ticks, int tick_idx, int total_ticks,
                                         double sample_price, double /*tp_pct*/, double /*sl_pct*/,
                                         int extra_param) {
    int forward_ticks = (extra_param > 0) ? extra_param : 100;
    if (tick_idx + forward_ticks >= total_ticks) return NAN;
    if (sample_price <= 0.0) return NAN;
    double future_return = (ticks[tick_idx + forward_ticks].price - sample_price) / sample_price;
    return (float)future_return;
}

//----------------------------------------------------------------------
// [SECTION]_[LABEL TABLE]
//----------------------------------------------------------------------
// table-driven: add new label = add 1 entry here + 1 function above
typedef float (*LabelFn)(const HistoricalTick *ticks, int tick_idx, int total_ticks,
                           double sample_price, double tp_pct, double sl_pct,
                           int extra_param);

// num_classes:
//   0 = binary classification (output is single P(class=1))
//   1 = regression (output is continuous value)
//  ≥2 = multiclass (output is K class probabilities, softmax-trained)
//======================================================================
// [STRUCT]_[LabelDef]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BACKTEST] [FRAMEWORK_DISCIPLINE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[one FOREACH_TARGET label-table row — id + snake/display names + description + the LabelFn pointer + num_classes (0=binary, 1=regression, >=2=multiclass)]
//======================================================================
// [CODE]
//======================================================================
struct LabelDef {
    int id;
    const char *name;          // snake_case for config / programmatic use
    const char *display_name;  // human-readable for GUI dropdown
    const char *description;
    LabelFn fn;
    int num_classes;
    // s5 — what this leaf's 5th argument MEANS (percent barrier / sigma
    // multiplier / ignored). See the tp_kind section comment above the enum.
    int tp_kind;
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
// [SIZE]_[48B]
// [ALIGN]_[8]
// [CACHE_LINES]_[1]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[LabelDef]
//======================================================================

// v5.10.0d — auto-generated from FOREACH_TARGET(X). Adding/removing/
// reordering rows must happen by editing the X-macro above; this table
// regenerates automatically. Eliminates the silent-divergence hazard
// between LABEL_* constants, label_table rows, and dispatcher sites.
#define X(id_suffix, name, display, desc, fn, nc, tpk) \
    { LABEL_##id_suffix, name, display, desc, fn, nc, tpk },
static constexpr LabelDef label_table[] = {
    FOREACH_TARGET(X)
};
#undef X

// TD-241's structural close: the REGIME row's class count is BOUND to the
// FOREACH_REGIME SSoT. A 6th regime added without bumping the row (or vice
// versa) is a COMPILE ERROR here, not a train-time surprise. The row stays a
// LITERAL because the registry hash stringizes the token (see the row banner).
static_assert(label_table[LABEL_REGIME].num_classes == NUM_REGIMES,
              "FOREACH_TARGET's REGIME row num_classes is out of sync with the FOREACH_REGIME "
              "SSoT (NUM_REGIMES) — update the row literal (this flips LABEL_REGISTRY_HASH: "
              "an epoch event, H21 greenlight required)");

// Preserve the existing LABEL_COUNT name for call-site compatibility.
// LABEL_COUNT_AUTO comes from the enum; one past the LAST registry row
// (append-only, so it grows with FOREACH_TARGET — do not name a member here).
static const int LABEL_COUNT = LABEL_COUNT_AUTO;

// v5.10.0d — FNV-1a registry hash over the X-macro's stable identifiers
// (key_name + ":nc" + num_classes). Adding, removing, or reordering rows
// flips this hash → stamp body's label_registry_hash mismatch on load
// → engine refuses to deploy a model trained against a different label
// set (mirrors v5.8.6 feature_registry_hash).
//
// Why num_classes contributes: if a label flips from binary (nc=0) to
// multiclass (nc=K), the prediction shape changes; a model trained as
// binary can't be deployed as multiclass even if the name matches.
namespace tt {
constexpr uint64_t LABEL_FNV_OFFSET_64 = 0xcbf29ce484222325ULL;
constexpr uint64_t LABEL_FNV_PRIME_64  = 0x100000001b3ULL;
}

inline uint64_t label_registry_hash_compute() {
    uint64_t h = tt::LABEL_FNV_OFFSET_64;
// s5: `tpk` intentionally NOT folded — see the tp_kind section comment. The
// hash pins the target SET and its class shapes; adding a column that describes
// a parameter's pre-existing meaning must not invalidate every stamp on disk.
#define X(id_suffix, name, display, desc, fn, nc, tpk) \
    do { \
        for (const char* p = name; *p; ++p) \
            h = (h ^ (uint64_t)(uint8_t)*p) * tt::LABEL_FNV_PRIME_64; \
        const char* nstr = ":nc" #nc; \
        for (const char* p = nstr; *p; ++p) \
            h = (h ^ (uint64_t)(uint8_t)*p) * tt::LABEL_FNV_PRIME_64; \
    } while (0);
    FOREACH_TARGET(X)
#undef X
    return h;
}

inline uint64_t LABEL_REGISTRY_HASH() {
    static const uint64_t h = label_registry_hash_compute();
    return h;
}

//----------------------------------------------------------------------
// [SECTION]_[LABEL KIND HELPERS]
//----------------------------------------------------------------------
// Single source of truth for "what kind of label is this." Reads num_classes
// from label_table[]. Every metric/display site that touches label values
// MUST branch on these — see "Label-type-aware metric invariant" in CLAUDE.md.
//
// num_classes encoding:
//   0  = binary classification    (label values 0.0, 1.0, optionally 0.5=neutral)
//   1  = regression               (label values continuous, any range)
//   ≥2 = multiclass softmax       (label values 0..K-1 as floats)

static inline int LabelType_NumClasses(int label_type) {
    if (label_type < 0 || label_type >= LABEL_COUNT) return 0; // safe default = binary
    return label_table[label_type].num_classes;
}

static inline int LabelType_IsBinary(int label_type) {
    return LabelType_NumClasses(label_type) == 0;
}

static inline int LabelType_IsRegression(int label_type) {
    return LabelType_NumClasses(label_type) == 1;
}

static inline int LabelType_IsMulticlass(int label_type) {
    return LabelType_NumClasses(label_type) >= 2;
}

//----------------------------------------------------------------------
// s5 — the ONE rule for "what TP barrier was this model actually trained
// against". Both the label walk and the stamp population call THIS, so the
// value a model learned and the value the engine later serves cannot drift.
//
// It exists because they DID drift: leaf-15 added the round-trip fee inside
// Backtest_ComputeLabelsBatch, where it was a transient — the labels used
// tp+fee while the stamp (and therefore the served bracket) recorded the raw
// tp, so the engine served a bracket NARROWER than the model was trained to
// predict. An M5 train-serve parity break, invisible because the effective
// barrier was never a value anything could read.
//
// The fee is a correction to a PERCENT threshold, so it applies only to
// TP_PCT leaves. Adding it to VOL_BARRIER's TP_SIGMA_K would be a unit error:
// with the FoxML default k=0.5, a 0.2 fee silently produced k=0.7 — a 40%
// wider vol barrier. TP_UNUSED leaves are unaffected either way, but routing
// them through here makes that provable rather than accidental.
static inline double Label_ResolveEffectiveTp(int label_type, double tp_pct,
                                               double roundtrip_fee_pct) {
    if (label_type < 0 || label_type >= LABEL_COUNT) return tp_pct;
    if (label_table[label_type].tp_kind != TP_PCT)   return tp_pct;
    if (!(roundtrip_fee_pct > 0.0))                  return tp_pct;
    return tp_pct + roundtrip_fee_pct;
}

// Display name for the kind itself ("binary" / "regression" / "multiclass").
// Used in log lines and tooltips.
static inline const char *LabelType_KindName(int label_type) {
    if (LabelType_IsRegression(label_type)) return "regression";
    if (LabelType_IsMulticlass(label_type)) return "multiclass";
    return "binary";
}
//======================================================================
// [END_CODE]
//======================================================================
// [ROW]_[regime]_[num_classes=4 but FOREACH_REGIME has 5 values (0..4); a sampled MILD_TREND(4) exceeds num_class=4 at XGBoost train — TECH_DEBT-241]
//======================================================================
// [COMMENT]
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// v5.10.0d — FOREACH_TARGET X-macro registry (audit Idea #2). Mirrors
// FOREACH_FEATURE / FOREACH_STRATEGY pattern from v5.8.x. Adding,
// removing, or reordering labels flips LABEL_REGISTRY_HASH() →
// load-time stamp refusal, preventing silent label-set drift between
// trainer and serve.
//
// Row schema:
//   X(id_suffix, "key_name", "Display", "Description", Compute_Fn, num_classes)
//   - id_suffix: appended to LABEL_<id_suffix> auto-generated constant
//   - key_name: cfg parser key + stamp body identifier (load-bearing)
//   - num_classes: 0 = binary, 1 = regression, ≥2 = multiclass
//
// APPEND-ONLY discipline. Reordering or removing a row flips the hash
// → all stamps signed under the prior order refuse to load. Append at
// the end.
//======================================================================
// [FUTURE_WORK]_[TECH_DEBT]_[TECH_DEBT-241]
//======================================================================
// [END_REGISTRY]_[FOREACH_TARGET]
//======================================================================

//======================================================================
// [FUNCTION]_[Training_ResolveRole]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [ML] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the ONE role-file derivation — side selects the ROLE (side=1 => "exit", co-located); label kind picks among the buy roles otherwise; extracted pure so tests pin it and the two former hand-copies cannot drift (E.1.2.C 3-role)]
//======================================================================
// [CODE]
//======================================================================
static inline const char* Training_ResolveRole(int label_type, int training_side) {
    if (training_side == 1) return "exit";   // E.1.2.C — the exit role file,
                                             // CO-LOCATED; the ensemble loader
                                             // already walks <dir>/exit.json.
    if (label_type == LABEL_PEAK_VALLEY_STABLE) return "barrier";
    // E.1.2.G — VOL_BARRIER_3C is the vol-scaled sister of PEAK_VALLEY_STABLE
    // (same 3-class stable/peak/valley encoding, barriers scaled by realized
    // vol instead of fixed pct), so it produces the SAME role file. Note it
    // does NOT follow its binary namesake VOL_BARRIER -> "buy_signal": the role
    // follows the CLASS STRUCTURE, not the barrier geometry.
    if (label_type == LABEL_VOL_BARRIER_3C)     return "barrier";
    if (label_type == LABEL_REGIME)             return "regime";
    return "buy_signal";
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[Training_ResolveRole]
//======================================================================

//======================================================================
// [FUNCTION]_[Training_SideLabelGate]
//----------------------------------------------------------------------
// [TAG]_[[GUI] [ML] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the side x label tier — 0 REFUSE / 1 WARN / 2 OK; extracted pure from the ImGui TU so this is drivable by a table test, tiers byte-unchanged]
//======================================================================
// [CODE]
//======================================================================
static inline int Training_SideLabelGate(int label_type, int training_side) {
    if (training_side != 1) return 2;
    switch (label_type) {
        case LABEL_WILL_PEAK:
        case LABEL_PEAK_VALLEY_STABLE:
        // E.1.2.G — VOL_BARRIER_3C joins the peak-bearing arm. The D-b rule is
        // "does the label carry a genuine PEAK class?", not "is it a barrier?":
        // its class 1 (down-barrier first) IS the peak, exactly as PVS's is.
        // Its BINARY namesake VOL_BARRIER stays REFUSE and that is not an
        // inconsistency — a binary up/down label is entry-DIRECTION, so an exit
        // model trained on it inverts (fires at valleys). Separating "stable"
        // into its own class is precisely what makes the 3-class form a valid
        // exit target.
        case LABEL_VOL_BARRIER_3C:     return 2;
        // D-b (2026-08-22, operator-decided): WILL_VALLEY + VOL_BARRIER moved
        // WARN -> REFUSE, unifying every ENTRY-DIRECTION label under the
        // default arm. The BARRIER/VOL_BARRIER asymmetry (structural twins,
        // one REFUSE one WARN) had no stated reason, and a WARN on
        // WILL_VALLEY permitted training an exit model that fires at
        // valleys — a sell-your-lows model, the same inverted-semantics
        // family as the fourteen-month entry inversion. The earlier
        // /decision-check refutation of this flip applied while the tier
        // gated nothing; the train predicates have consumed it since
        // 057e891, so the tier has teeth now. WARN(1) is currently
        // UNINHABITED — the tier VALUE stays (worst-tier-wins math + any
        // future genuinely-ambiguous label).
        default:                       return 0;  // WIN_LOSS / BARRIER / VOL_BARRIER
                                                  // / WILL_VALLEY / FORWARD_PNL
                                                  // / REGIME / CS_*
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// The PRODUCER is the only place this can be enforced: label truth reaches
// no wire key (measured — the stamp carries label_params, label_registry_hash
// and model_num_outputs, none of which identifies the target row), so a
// WIN_LOSS model trained at side=1 stamps role="exit" honestly and PASSES the
// load-side role check. Training at side=1 on an entry-goodness label yields a
// semantically INVERTED exit model.
//
// Extracted from a lambda in BacktestPanels.hpp at E.1.2.C for the same reason
// Training_ResolveRole moved here one commit earlier: that header is included
// by exactly one ImGui TU, so a gate living there is unreachable from the ANSI
// test TU and cannot be pinned. It was the one leg of the D2 verdict without a
// table test. TIERS ARE BYTE-UNCHANGED by the move — the WARN pair stays WARN.
// A /decision-check REFUTED flipping them to REFUSE (the refutation: this tier
// gates only the COLLECT buttons, so retiering an unenforced tier buys nothing;
// that is fixed separately by wiring the train predicates to it).
//
// The caller aggregates: when a per-horizon label set is in play, the gate is
// evaluated per member and the WORST (numerically lowest) tier wins, so a
// single bad horizon cannot ride in behind a benign combo selection.
//======================================================================
// [END_FUNCTION]_[Training_SideLabelGate]
//======================================================================

#endif // LABEL_FUNCTIONS_HPP
