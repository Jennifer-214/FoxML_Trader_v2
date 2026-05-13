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
#include <cstddef>   // v5.15.5.D.A — offsetof for layout-lock static_asserts
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
// v5.15.5.D.A — alignas(64) + HOT-first reorg per cache-layout-discipline-
// for-hot-side-structs.md Rule 4. HOT cluster (sum + count + head) sits at
// offset 0..31 = 1 cache line; COLD samples[W] follows at offset 32. The
// 32 B trailing pad from alignas(64) is structural minimum (24,608 natural;
// mod 64 = 32; next multiple = 24,640). v5.15.5.D.B inserts `short_sum`
// between sum and count (HOT cluster grows 32 → 56 B; trailing pad shrinks
// 32 → 8 B; sizeof unchanged).
template <unsigned F, unsigned W = 1024>
struct alignas(64) BookImbalanceHistory {
    // HOT cluster (offset 0; touched every slow-path cycle by Push + read fns)
    FPN<F> sum;          // running sum over all valid samples
    int    count;        // number of valid samples in [0, W]
    int    head;         // next write position
    // COLD cluster (offset 32; samples[head] touched 1× per Push; MeanShort
    // walks K=64 sequential elements once per read in .D.A — converted to
    // O(1) via short_sum running aggregate in .D.B)
    FPN<F> samples[W];
};

// v5.15.5.D.A — Layout lock for the canonical production instantiation.
// 32 B HOT scalars + 24,576 B COLD samples + 32 B alignas(64) trailing pad
// = 24,640 B = 385 cache lines exact. The 32 B trailing pad is structural
// minimum given alignas(64) requirement; reducing requires changing W
// (sub-optimal per layout-puzzle analysis) or filling pad with a useful
// field (no current consumer per CLAUDE.md item 16 reuse-audit).
// Typedef wraps the template instantiation so the comma in <64, 1024> isn't
// parsed as a macro-arg separator inside offsetof().
using BookImbHistDefaultT = BookImbalanceHistory<64, 1024>;
static_assert(sizeof(BookImbHistDefaultT) == 24640,
    "BookImbalanceHistory<64,1024> sizeof MUST be 24,640 B (385 cache lines).");
static_assert(offsetof(BookImbHistDefaultT, sum) == 0,
    "BookImbalanceHistory HOT scalar `sum` MUST sit at offset 0.");
static_assert(offsetof(BookImbHistDefaultT, samples) == 32,
    "BookImbalanceHistory COLD `samples` MUST sit at offset 32 (after HOT cluster).");
static_assert(alignof(BookImbHistDefaultT) == 64,
    "BookImbalanceHistory MUST be cache-line aligned.");

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
// v5.15.5.D.A — alignas(64) ensures FlowState's 32 B never straddles two
// cache lines. All 4 fields are HOT (Push and read both touch all 4 every
// slow-path cycle); no HOT/WARM/COLD tier needed (whole struct fits in 1
// cache line). Trailing 32 B pad is structural minimum given alignas(64)
// requirement (32 B natural; pad to 64).
struct alignas(64) FlowState {
    double ewma_10s;     // signed-volume EWMA, half-life 10s
    double ewma_1m;      // half-life 60s
    double ewma_5m;      // half-life 300s
    uint64_t last_us;    // timestamp of last push (0 = no prior)
};

// v5.15.5.D.A — Layout lock for FlowState.
static_assert(sizeof(FlowState) == 64,
    "FlowState sizeof MUST be 64 B (1 cache line).");
static_assert(offsetof(FlowState, ewma_10s) == 0,
    "FlowState fields MUST sit at offset 0.");
static_assert(alignof(FlowState) == 64,
    "FlowState MUST be cache-line aligned.");

static inline void FlowState_Init(FlowState *s) {
    s->ewma_10s = 0.0;
    s->ewma_1m  = 0.0;
    s->ewma_5m  = 0.0;
    s->last_us  = 0;
}

// v5.10.0b.2.5.C: decay computation goes through FPN_Exp (bytewise-
// deterministic across compilers / -O levels) instead of IEEE-754 exp.
// EWMA storage stays double for RegimeSignals compatibility; the
// bytewise contract is "same input → same stored bytes" guaranteed by
// FPN_FromDouble + FPN_Exp + FPN_ToDouble all being deterministic.
// Full RegimeSignals→FPN cascade is a v5.11 ship (large blast radius).
static inline void FlowState_Push(FlowState *s, uint64_t timestamp_us, double signed_volume) {
    if (s->last_us == 0) {
        s->ewma_10s = signed_volume;
        s->ewma_1m  = signed_volume;
        s->ewma_5m  = signed_volume;
        s->last_us  = timestamp_us;
        return;
    }
    if (timestamp_us <= s->last_us) {
        s->ewma_10s += signed_volume;
        s->ewma_1m  += signed_volume;
        s->ewma_5m  += signed_volume;
        return;
    }
    double dt = (double)(timestamp_us - s->last_us) / 1e6;  // seconds

    // FPN-native decay: -dt / halflife → exp via Taylor.
    FPN<64> dt_fpn = FPN_FromDouble<64>(dt);
    FPN<64> hl_10s = FPN_FromDouble<64>(10.0);
    FPN<64> hl_1m  = FPN_FromDouble<64>(60.0);
    FPN<64> hl_5m  = FPN_FromDouble<64>(300.0);

    FPN<64> arg_10s = FPN_DivNoAssert(dt_fpn, hl_10s); arg_10s.sign = 1;
    FPN<64> arg_1m  = FPN_DivNoAssert(dt_fpn, hl_1m);  arg_1m.sign  = 1;
    FPN<64> arg_5m  = FPN_DivNoAssert(dt_fpn, hl_5m);  arg_5m.sign  = 1;

    double decay_10s = FPN_ToDouble(FPN_Exp(arg_10s));
    double decay_1m  = FPN_ToDouble(FPN_Exp(arg_1m));
    double decay_5m  = FPN_ToDouble(FPN_Exp(arg_5m));

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
// v5.15.5.D.A — alignas(64) + HOT-first reorg per cache-layout-discipline-
// for-hot-side-structs.md Rule 4. HOT cluster (sum + sum_sq + count + head)
// sits at offset 0..55 = 1 cache line minus 8 B; COLD sizes[W] follows at
// offset 56. The 8 B trailing pad from alignas(64) is structural minimum
// (24,632 natural; mod 64 = 24; next multiple = 24,640).
template <unsigned F, unsigned W = 1024>
struct alignas(64) LargeTradeState {
    // HOT cluster (offset 0; touched every slow-path cycle by Push + read fns)
    FPN<F> sum;          // running sum
    FPN<F> sum_sq;       // running sum of squares
    int    count;
    int    head;
    // COLD cluster (offset 56; sizes[head] touched 1× per Push; ZScore is
    // already O(1) using running sum + sum_sq, no walk)
    FPN<F> sizes[W];     // ring of recent sizes
};

// v5.15.5.D.A — Layout lock for LargeTradeState<64, 1024>.
// 56 B HOT scalars + 24,576 B COLD sizes + 8 B alignas(64) trailing pad
// = 24,640 B = 385 cache lines exact. Typedef wraps template instantiation
// for offsetof macro-arg parsing.
using LargeTradeStateDefaultT = LargeTradeState<64, 1024>;
static_assert(sizeof(LargeTradeStateDefaultT) == 24640,
    "LargeTradeState<64,1024> sizeof MUST be 24,640 B (385 cache lines).");
static_assert(offsetof(LargeTradeStateDefaultT, sum) == 0,
    "LargeTradeState HOT scalar `sum` MUST sit at offset 0.");
static_assert(offsetof(LargeTradeStateDefaultT, sizes) == 56,
    "LargeTradeState COLD `sizes` MUST sit at offset 56 (after HOT cluster).");
static_assert(alignof(LargeTradeStateDefaultT) == 64,
    "LargeTradeState MUST be cache-line aligned.");

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
// v5.10.0b.2.5.C: variance + stddev computed in FPN<F> via FPN_Sqrt
// (bytewise-deterministic). Return type stays `double` to keep
// touch-site count low — the RegimeSignals.large_trade_z field stays
// double, so downstream consumers don't change. The bytewise contract
// is enforced through deterministic FPN_Sub/Mul/Sqrt/DivNoAssert and
// the final FPN_ToDouble.
template <unsigned F, unsigned W = 1024>
static inline double LargeTradeState_ZScore(const LargeTradeState<F, W> *s, FPN<F> current_size) {
    if (s->count < 2) return 0.0;
    FPN<F> n_fpn   = FPN_FromInt<F>(s->count);
    FPN<F> mean    = FPN_DivNoAssert(s->sum, n_fpn);
    FPN<F> mean_sq = FPN_DivNoAssert(s->sum_sq, n_fpn);
    FPN<F> var     = FPN_Sub(mean_sq, FPN_Mul(mean, mean));
    if (FPN_IsZero(var) || var.sign != 0) return 0.0;  // var <= 0 → degenerate

    FPN<F> stddev = FPN_Sqrt(var);
    if (FPN_IsZero(stddev)) return 0.0;
    FPN<F> z = FPN_DivNoAssert(FPN_Sub(current_size, mean), stddev);
    return FPN_ToDouble(z);
}

// Most recent pushed size. Zero when empty.
template <unsigned F, unsigned W = 1024>
static inline FPN<F> LargeTradeState_Last(const LargeTradeState<F, W> *s) {
    if (s->count <= 0) return FPN_Zero<F>();
    int idx = (s->head - 1 + (int)W) % (int)W;
    return s->sizes[idx];
}

//======================================================================================================
// [SPREAD STATE — D.3]
//======================================================================================================
// Rolling window of recent bid-ask spread samples. Same shape as
// LargeTradeState (running sum + sum_sq for O(1) mean + variance), kept
// as a separate struct for clarity at call sites and future divergence
// (e.g. spread-specific normalizations like spread/price ratio).
//
// Window W default 1024 ≈ 17 minutes at slow_path=100 cadence.
//
// FEAT_SPREAD_BPS comes from current spread normalized by mid_price ×
// 10000 (computed inline at Regime_ComputeSignals time, no state needed).
// FEAT_SPREAD_ZSCORE is the z-score of current spread vs this state's
// distribution.
//======================================================================================================
// v5.15.5.D.A — alignas(64) + HOT-first reorg per cache-layout-discipline-
// for-hot-side-structs.md Rule 4. Identical shape to LargeTradeState; HOT
// cluster (sum + sum_sq + count + head) at offset 0..55; COLD samples[W]
// at offset 56. The 8 B trailing pad is structural minimum.
template <unsigned F, unsigned W = 1024>
struct alignas(64) SpreadState {
    // HOT cluster (offset 0; touched every slow-path cycle)
    FPN<F> sum;
    FPN<F> sum_sq;
    int    count;
    int    head;
    // COLD cluster (offset 56)
    FPN<F> samples[W];
};

// v5.15.5.D.A — Layout lock for SpreadState<64, 1024>.
using SpreadStateDefaultT = SpreadState<64, 1024>;
static_assert(sizeof(SpreadStateDefaultT) == 24640,
    "SpreadState<64,1024> sizeof MUST be 24,640 B (385 cache lines).");
static_assert(offsetof(SpreadStateDefaultT, sum) == 0,
    "SpreadState HOT scalar `sum` MUST sit at offset 0.");
static_assert(offsetof(SpreadStateDefaultT, samples) == 56,
    "SpreadState COLD `samples` MUST sit at offset 56 (after HOT cluster).");
static_assert(alignof(SpreadStateDefaultT) == 64,
    "SpreadState MUST be cache-line aligned.");

template <unsigned F, unsigned W = 1024>
static inline void SpreadState_Init(SpreadState<F, W> *s) {
    memset(s, 0, sizeof(*s));
    s->sum    = FPN_Zero<F>();
    s->sum_sq = FPN_Zero<F>();
    s->count  = 0;
    s->head   = 0;
    for (unsigned i = 0; i < W; i++) s->samples[i] = FPN_Zero<F>();
}

template <unsigned F, unsigned W = 1024>
static inline void SpreadState_Push(SpreadState<F, W> *s, FPN<F> sample) {
    if (s->count >= (int)W) {
        FPN<F> evicted = s->samples[s->head];
        s->sum    = FPN_Sub(s->sum, evicted);
        s->sum_sq = FPN_Sub(s->sum_sq, FPN_Mul(evicted, evicted));
    } else {
        s->count++;
    }
    s->samples[s->head] = sample;
    s->sum    = FPN_Add(s->sum, sample);
    s->sum_sq = FPN_Add(s->sum_sq, FPN_Mul(sample, sample));
    s->head = (s->head + 1) % W;
}

// v5.10.0b.2.5.C: variance + stddev computed in FPN<F> via FPN_Sqrt.
// Same boundary-stable shape as LargeTradeState_ZScore — return double
// to avoid cascading into RegimeSignals.spread_zscore consumers.
template <unsigned F, unsigned W = 1024>
static inline double SpreadState_ZScore(const SpreadState<F, W> *s, FPN<F> current_spread) {
    if (s->count < 2) return 0.0;
    FPN<F> n_fpn   = FPN_FromInt<F>(s->count);
    FPN<F> mean    = FPN_DivNoAssert(s->sum, n_fpn);
    FPN<F> mean_sq = FPN_DivNoAssert(s->sum_sq, n_fpn);
    FPN<F> var     = FPN_Sub(mean_sq, FPN_Mul(mean, mean));
    if (FPN_IsZero(var) || var.sign != 0) return 0.0;

    FPN<F> stddev = FPN_Sqrt(var);
    if (FPN_IsZero(stddev)) return 0.0;
    FPN<F> z = FPN_DivNoAssert(FPN_Sub(current_spread, mean), stddev);
    return FPN_ToDouble(z);
}

template <unsigned F, unsigned W = 1024>
static inline FPN<F> SpreadState_Last(const SpreadState<F, W> *s) {
    if (s->count <= 0) return FPN_Zero<F>();
    int idx = (s->head - 1 + (int)W) % (int)W;
    return s->samples[idx];
}

#endif // FLOW_FEATURES_HPP
