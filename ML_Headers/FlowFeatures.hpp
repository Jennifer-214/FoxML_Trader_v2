// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FLOW FEATURES — Wave 1 of Track D, post-Track-E]
//======================================================================================================
// Three new state structs feeding the v4.5 feature pack expansion:
//
//   - BookImbalanceHistory — D.1: time-series of book imbalance samples.
//     Push at slow-path cadence; mean over short + long window + drift
//     (current - long mean) feed FEAT_BOOK_IMB_MEAN_SHORT/LONG/DRIFT.
//
//   - FlowState — D.2: signed-volume EWMA at three half-lives (10s, 1m,
//     5m). Time-decayed so recent imbalance dominates. Push at slow-path
//     with timestamp_us + signed volume (taker buy = positive, taker
//     sell = negative). Feeds FEAT_FLOW_10S/1M/5M.
//
//   - LargeTradeState — D.4: rolling window of recent trade sizes. Push
//     per slow-path with current trade size (or summed-volume-this-cycle).
//     Z-score of current size vs window feeds FEAT_LARGE_TRADE_Z.
//
// Cadence: ALL THREE PUSH AT SLOW-PATH (every poll_interval ticks),
// matching CumDelta_Push / TickRate_Push. Train-serve parity holds as
// long as both sharded paths push at the same cadence with the same
// inputs.
//
// Type discipline:
//   - BookImbalanceHistory: FPN<F> throughout (book_imbalance is FPN
//     in BookSnapshot, sum + mean stay FPN).
//   - FlowState: double internally (EWMA decay uses exp(); double is
//     natural; values bounded by recent volume so no precision concern).
//     RegimeSignals.flow_* fields are FPN<F> (converted in
//     Regime_ComputeSignals) to match the rest of the feature pack.
//   - LargeTradeState: FPN<F> for sums; double for z-score (matches
//     RegimeSignals.large_trade_z's double type, mirrors tick_rate_z).
//======================================================================================================

#ifndef FLOW_FEATURES_HPP
#define FLOW_FEATURES_HPP

#include "../FixedPoint/FixedPointN.hpp"
#include <cmath>
#include <cstdint>
#include <cstring>

//======================================================================================================
// [BOOK IMBALANCE HISTORY — D.1]
//======================================================================================================
// Fixed-size ring buffer of recent book_imbalance samples. Window W
// chosen at instantiation (default 1024 = ~17 minutes at slow_path_-
// interval=100, ~1 sample/sec under a busy market).
//
// Maintains a running sum so MeanLong is O(1). MeanShort iterates the
// last K samples (K << W) — O(K) per call, called once per slow path.
//======================================================================================================
template <unsigned F, unsigned W = 1024>
struct BookImbalanceHistory {
    FPN<F> samples[W];
    FPN<F> sum;          // running sum over all valid samples
    int    count;        // number of valid samples in [0, W]
    int    head;         // next write position
};

template <unsigned F, unsigned W = 1024>
static inline void BookImbHistory_Init(BookImbalanceHistory<F, W> *s) {
    memset(s, 0, sizeof(*s));
    s->sum   = FPN_Zero<F>();
    s->count = 0;
    s->head  = 0;
    for (unsigned i = 0; i < W; i++) s->samples[i] = FPN_Zero<F>();
}

template <unsigned F, unsigned W = 1024>
static inline void BookImbHistory_Push(BookImbalanceHistory<F, W> *s, FPN<F> sample) {
    // Evict oldest if buffer full
    if (s->count >= (int)W) {
        s->sum = FPN_Sub(s->sum, s->samples[s->head]);
    } else {
        s->count++;
    }
    s->samples[s->head] = sample;
    s->sum = FPN_Add(s->sum, sample);
    s->head = (s->head + 1) % W;
}

// Mean over all valid samples — O(1).
template <unsigned F, unsigned W = 1024>
static inline FPN<F> BookImbHistory_MeanLong(const BookImbalanceHistory<F, W> *s) {
    if (s->count <= 0) return FPN_Zero<F>();
    return FPN_DivNoAssert(s->sum, FPN_FromDouble<F>((double)s->count));
}

// Most recent pushed sample. Zero when buffer is empty.
template <unsigned F, unsigned W = 1024>
static inline FPN<F> BookImbHistory_Last(const BookImbalanceHistory<F, W> *s) {
    if (s->count <= 0) return FPN_Zero<F>();
    int idx = (s->head - 1 + (int)W) % (int)W;
    return s->samples[idx];
}

// Mean over last K samples (K <= count). O(K) — called once per slow path
// so cost is bounded.
template <unsigned F, unsigned W = 1024>
static inline FPN<F> BookImbHistory_MeanShort(const BookImbalanceHistory<F, W> *s, int k) {
    if (s->count <= 0 || k <= 0) return FPN_Zero<F>();
    if (k > s->count) k = s->count;
    FPN<F> acc = FPN_Zero<F>();
    // Walk backward from head — newest k samples are at head-1, head-2, ...
    for (int i = 0; i < k; i++) {
        int idx = (s->head - 1 - i + (int)W) % (int)W;
        acc = FPN_Add(acc, s->samples[idx]);
    }
    return FPN_DivNoAssert(acc, FPN_FromDouble<F>((double)k));
}

//======================================================================================================
// [FLOW STATE — D.2]
//======================================================================================================
// Three EWMAs of signed volume at half-lives 10s / 1min / 5min. Each
// push:
//   1. dt = (timestamp_us - last_us) / 1e6  (seconds since last push)
//   2. decay = exp(-dt / halflife)
//   3. ewma = ewma * decay + signed_volume
//   4. last_us = timestamp_us
//
// First push (last_us == 0) sets all EWMAs to signed_volume directly,
// no decay (no prior sample to decay).
//
// Why EWMA + not a fixed-window: continuous-time decay matches the
// economic reality (recent flow matters more than ancient flow), and
// it's cadence-independent — pushing every slow-path firing (variable
// inter-tick time) produces the same approximate continuous EWMA as
// pushing every wall-clock second would.
//======================================================================================================
struct FlowState {
    double ewma_10s;     // signed-volume EWMA, half-life 10s
    double ewma_1m;      // half-life 60s
    double ewma_5m;      // half-life 300s
    uint64_t last_us;    // timestamp of last push (0 = no prior)
};

static inline void FlowState_Init(FlowState *s) {
    s->ewma_10s = 0.0;
    s->ewma_1m  = 0.0;
    s->ewma_5m  = 0.0;
    s->last_us  = 0;
}

static inline void FlowState_Push(FlowState *s, uint64_t timestamp_us, double signed_volume) {
    if (s->last_us == 0) {
        // First sample — seed all EWMAs with signed_volume
        s->ewma_10s = signed_volume;
        s->ewma_1m  = signed_volume;
        s->ewma_5m  = signed_volume;
        s->last_us  = timestamp_us;
        return;
    }
    if (timestamp_us <= s->last_us) {
        // Backward / same timestamp — just add signed_volume without decay
        s->ewma_10s += signed_volume;
        s->ewma_1m  += signed_volume;
        s->ewma_5m  += signed_volume;
        return;
    }
    double dt = (double)(timestamp_us - s->last_us) / 1e6;  // seconds
    double decay_10s = exp(-dt / 10.0);
    double decay_1m  = exp(-dt / 60.0);
    double decay_5m  = exp(-dt / 300.0);
    s->ewma_10s = s->ewma_10s * decay_10s + signed_volume;
    s->ewma_1m  = s->ewma_1m  * decay_1m  + signed_volume;
    s->ewma_5m  = s->ewma_5m  * decay_5m  + signed_volume;
    s->last_us = timestamp_us;
}

//======================================================================================================
// [LARGE TRADE STATE — D.4]
//======================================================================================================
// Ring buffer of recent trade sizes. Maintains running sum + sum_sq for
// O(1) mean + variance. Z-score of a current trade size: (size - mean) /
// stddev. Output is double (matches RegimeSignals.large_trade_z's type
// to mirror tick_rate_z's pattern).
//
// Window W default 1024 ≈ 17 minutes at slow_path=100 cadence under a
// busy market. Same W as BookImbalanceHistory for symmetry.
//======================================================================================================
template <unsigned F, unsigned W = 1024>
struct LargeTradeState {
    FPN<F> sizes[W];     // ring of recent sizes
    FPN<F> sum;          // running sum
    FPN<F> sum_sq;       // running sum of squares
    int    count;
    int    head;
};

template <unsigned F, unsigned W = 1024>
static inline void LargeTradeState_Init(LargeTradeState<F, W> *s) {
    memset(s, 0, sizeof(*s));
    s->sum    = FPN_Zero<F>();
    s->sum_sq = FPN_Zero<F>();
    s->count  = 0;
    s->head   = 0;
    for (unsigned i = 0; i < W; i++) s->sizes[i] = FPN_Zero<F>();
}

template <unsigned F, unsigned W = 1024>
static inline void LargeTradeState_Push(LargeTradeState<F, W> *s, FPN<F> size) {
    if (s->count >= (int)W) {
        FPN<F> evicted = s->sizes[s->head];
        s->sum    = FPN_Sub(s->sum, evicted);
        s->sum_sq = FPN_Sub(s->sum_sq, FPN_Mul(evicted, evicted));
    } else {
        s->count++;
    }
    s->sizes[s->head] = size;
    s->sum    = FPN_Add(s->sum, size);
    s->sum_sq = FPN_Add(s->sum_sq, FPN_Mul(size, size));
    s->head = (s->head + 1) % W;
}

// Z-score of `current_size` against the window's distribution.
// Returns 0 if count < 2 or stddev == 0 (degenerate / cold start).
template <unsigned F, unsigned W = 1024>
static inline double LargeTradeState_ZScore(const LargeTradeState<F, W> *s, FPN<F> current_size) {
    if (s->count < 2) return 0.0;
    double n = (double)s->count;
    double mean = FPN_ToDouble(s->sum) / n;
    double mean_sq = FPN_ToDouble(s->sum_sq) / n;
    double var = mean_sq - mean * mean;
    if (var <= 0.0) return 0.0;
    double stddev = sqrt(var);
    if (stddev <= 1e-12) return 0.0;
    double cur = FPN_ToDouble(current_size);
    return (cur - mean) / stddev;
}

// Most recent pushed size. Zero when empty.
template <unsigned F, unsigned W = 1024>
static inline FPN<F> LargeTradeState_Last(const LargeTradeState<F, W> *s) {
    if (s->count <= 0) return FPN_Zero<F>();
    int idx = (s->head - 1 + (int)W) % (int)W;
    return s->sizes[idx];
}

#endif // FLOW_FEATURES_HPP
