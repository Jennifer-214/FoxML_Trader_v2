// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[ML_Headers/FlowFeatures.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [BINARY_FP] [DATA_ORIENTED_DESIGN]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[order-flow feature state (Track D) — book-imbalance history, signed-volume EWMAs, large-trade + spread z-score windows; all pushed at slow-path cadence, feeding the regime feature pack]
// [CONTAINS]
//   - [STRUCT]_[BookImbalanceHistory]   (+ [FUNCTION]_[BookImbHistory_Push] family: Init/MeanLong/MeanShortFast/Last/MeanShort)
//   - [STRUCT]_[FlowState]              (+ [FUNCTION]_[FlowState_Push] family: Init)
//   - [STRUCT]_[LargeTradeState]        (+ [FUNCTION]_[LargeTradeState_Push] family: Init/ZScore/Last)
//   - [STRUCT]_[SpreadState]            (+ [FUNCTION]_[SpreadState_Push] family: Init/ZScore/Last)
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
//   - BookImbalanceHistory: FPN_Binary<F> throughout (book_imbalance is FPN_Binary
//     in BookSnapshot, sum + mean stay FPN_Binary).
//   - FlowState: double internally (EWMA decay uses exp(); double is
//     natural; values bounded by recent volume so no precision concern).
//     RegimeSignals.flow_* fields are FPN_Binary<F> (converted in
//     Regime_ComputeSignals) to match the rest of the feature pack.
//   - LargeTradeState: FPN_Binary<F> for sums; double for z-score (matches
//     RegimeSignals.large_trade_z's double type, mirrors tick_rate_z).
//======================================================================================================

#ifndef FLOW_FEATURES_HPP
#define FLOW_FEATURES_HPP

#include "../FixedPoint/FixedPointN.hpp"
#include <cmath>
#include <cstddef>   // v5.15.5.D.A — offsetof for layout-lock static_asserts
#include <cstdint>
#include <cstring>

//======================================================================
// [STRUCT]_[BookImbalanceHistory]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [BINARY_FP] [DATA_ORIENTED_DESIGN]]
// [SCOPE]_[NODE]
// [INSTANTIATION]_[[1024]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[ring of book-imbalance samples (D.1) — running long + SHORT_K sums make MeanLong + MeanShortFast O(1); HOT scalars lead, COLD ring follows]
// [REFERENCE]_[DESIGN_SPEC]_[sliding-window-online-statistics-pattern]
// [REFERENCE]_[INVARIANT]_[[H4] [H6]]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F, unsigned W = 1024>
struct alignas(64) BookImbalanceHistory {
    // v5.15.5.D.B — Compile-time-fixed short-window size. Production caller
    // is RegimeDetector @ Strategies/RegimeDetector.hpp (uses MeanShortFast
    // = short_sum / effective_k, equivalent to MeanShort(64) bytewise).
    // Tests still call MeanShort(s, k) with k=2 — that path keeps the O(K)
    // walk for non-canonical k.
    static constexpr int SHORT_K = 64;

    // HOT cluster (offset 0; touched every slow-path cycle by Push + read fns)
    FPN_Binary<F> sum;          // running sum over all valid samples (window W)
    FPN_Binary<F> short_sum;    // v5.15.5.D.B — running sum over last SHORT_K samples
    int    count;        // number of valid samples in [0, W]
    int    head;         // next write position
    // COLD cluster (samples[head] touched 1× per Push;
    // samples[head - SHORT_K] touched 1× per Push for short-window eviction
    // — typically L1-warm since K=64 cycles ago was visited recently)
    FPN_Binary<F> samples[W];
};
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Fixed-size ring buffer of recent book_imbalance samples. Window W
// chosen at instantiation (default 1024 = ~17 minutes at slow_path_-
// interval=100, ~1 sample/sec under a busy market).
//
// Maintains a running sum so MeanLong is O(1). MeanShort iterates the
// last K samples (K << W) — O(K) per call, called once per slow path.
//======================================================================
// [COMMENT]_[layout — v5.15.5.D.A/B]
//----------------------------------------------------------------------
// v5.15.5.D.A/B — alignas(64) + HOT-first reorg per cache-layout-discipline-
// for-hot-side-structs.md Rule 4. HOT cluster (sum + short_sum + count + head)
// leads the struct; COLD samples[W] follows (exact offsets + sizeof pinned by
// the layout-lock asserts below — the mechanical SSoT).
//
// v5.15.5.D.B — `short_sum` maintains a running sum over the SHORT_K most
// recent samples. The pre-.D.B BookImbHistory_MeanShort(k=64) did an O(K)
// sequential walk every slow-path cycle (~24 cache lines / read in
// RegimeDetector). With short_sum, MeanShortFast reads it in O(1). Pattern:
// DESIGN_SPECS/sliding-window-online-statistics-pattern.md Approach 3
// (sliding-window incremental) Multi-window variant; 2nd canonical
// application after v5.14.11.A RidgeBlender correlation matrix.
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
//----------------------------------------------------------------------
// [SIZE]_[16448B]
// [ALIGN]_[64]
// [CACHE_LINES]_[257]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[BookImbalanceHistory]
//======================================================================

// v5.15.5.D.A/B — Layout lock for the canonical production instantiation;
// padding analysis rigorously verified — see plan
// 2026-05-13-v5.15.5.D-flowfeatures-cache-layout-sweep.md.
// Typedef wraps the template instantiation so the comma in <64, 1024> isn't
// parsed as a macro-arg separator inside offsetof().
using BookImbHistDefaultT = BookImbalanceHistory<64, 1024>;
// [ASSERT]_[LAYOUT_LOCK]_[sizeof(BookImbalanceHistory<64,1024>) == 16448]
static_assert(sizeof(BookImbHistDefaultT) == 16448,
    "BookImbalanceHistory<64,1024> sizeof MUST be 16,448 B (257 cache lines; Ship-A 16B FPN_Binary, was 24,640).");
// [ASSERT]_[LAYOUT_LOCK]_[offsetof(sum) == 0 — HOT cluster leads]
static_assert(offsetof(BookImbHistDefaultT, sum) == 0,
    "BookImbalanceHistory HOT scalar `sum` MUST sit at offset 0.");
static_assert(offsetof(BookImbHistDefaultT, short_sum) == 16,
    "BookImbalanceHistory HOT scalar `short_sum` MUST sit at offset 16 "
    "(immediately after sum in HOT cluster; Ship-A 16B FPN_Binary, was 24). Pattern: sliding-window-online-"
    "statistics-pattern.md Multi-window variant.");
// [ASSERT]_[LAYOUT_LOCK]_[offsetof(samples) == 48 — COLD ring after the HOT cluster]
static_assert(offsetof(BookImbHistDefaultT, samples) == 48,
    "BookImbalanceHistory COLD `samples` MUST sit at offset 48 (after HOT cluster; Ship-A 16B FPN_Binary, was 56).");
// [ASSERT]_[LAYOUT_LOCK]_[alignof(BookImbalanceHistory) == 64]
static_assert(alignof(BookImbHistDefaultT) == 64,
    "BookImbalanceHistory MUST be cache-line aligned.");

//======================================================================
// [FUNCTION]_[BookImbHistory_Push]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [BINARY_FP]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[dual-window ring push — evicts long (W) + short (SHORT_K) sums before overwrite; Init/MeanLong/MeanShortFast/Last/MeanShort ride in this section]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F, unsigned W = 1024>
static inline void BookImbHistory_Init(BookImbalanceHistory<F, W> *s) {
    memset(s, 0, sizeof(*s));
    s->sum       = FPN_Zero<F>();
    s->short_sum = FPN_Zero<F>();   // v5.15.5.D.B — running short-window sum
    s->count     = 0;
    s->head      = 0;
    for (unsigned i = 0; i < W; i++) s->samples[i] = FPN_Zero<F>();
}

template <unsigned F, unsigned W = 1024>
static inline void BookImbHistory_Push(BookImbalanceHistory<F, W> *s, FPN_Binary<F> sample) {
    // Long-window maintenance: evict samples[head] (W-cycles-old) if buffer full
    if (s->count >= (int)W) {
        s->sum = FPN_Sub(s->sum, s->samples[s->head]);
    } else {
        s->count++;
    }

    // v5.15.5.D.B — Short-window maintenance: evict samples[head - SHORT_K] when
    // current count exceeds SHORT_K (warm-up phase count <= K → no eviction, both
    // sums accumulate identically until count == K + 1). Eviction must happen
    // BEFORE the new sample overwrites samples[head]. samples[head - K] is
    // typically L1-warm (visited K=64 cycles ago; small enough to retain).
    // FPN_Add associativity holds for book-imbalance magnitudes (|x| ≤ 1; sum
    // ≤ 64 ≪ FPN_Binary<64>'s ±2^63 range; no saturation → exact integer arithmetic
    // → bytewise associative). Bytewise parity vs walked MeanShort(64) locked
    // by tests/controller_test.cpp v5.15.5.D.B parity loop.
    if (s->count > BookImbalanceHistory<F, W>::SHORT_K) {
        int evict_short = (s->head + (int)W - BookImbalanceHistory<F, W>::SHORT_K) % (int)W;
        s->short_sum = FPN_Sub(s->short_sum, s->samples[evict_short]);
    }

    s->samples[s->head] = sample;
    s->sum       = FPN_Add(s->sum, sample);
    s->short_sum = FPN_Add(s->short_sum, sample);   // v5.15.5.D.B
    s->head      = (s->head + 1) % W;
}

// Mean over all valid samples — O(1).
template <unsigned F, unsigned W = 1024>
static inline FPN_Binary<F> BookImbHistory_MeanLong(const BookImbalanceHistory<F, W> *s) {
    if (s->count <= 0) return FPN_Zero<F>();
    return FPN_DivNoAssert(s->sum, FPN_FromDouble<F>((double)s->count));
}

// v5.15.5.D.B — O(1) fast-path accessor for the canonical k=SHORT_K case.
// Replaces RegimeDetector's MeanShort(h, 64) call — eliminates the ~24
// cache-line sequential walk per slow-path cycle. The general MeanShort(s, k)
// below stays unchanged for tests (k=2) and any future flexible-k consumer.
//
// Bytewise-identical to MeanShort(s, SHORT_K) for matching state (FPN_Add
// associativity holds for bounded inputs). Verified by parity test in
// tests/controller_test.cpp ("v5.15.5.D.B bytewise parity" section).
//
// Pattern: DESIGN_SPECS/sliding-window-online-statistics-pattern.md Multi-
// window variant; 2nd canonical application of the sliding-window pattern.
template <unsigned F, unsigned W = 1024>
static inline FPN_Binary<F> BookImbHistory_MeanShortFast(const BookImbalanceHistory<F, W> *s) {
    if (s->count <= 0) return FPN_Zero<F>();
    int effective_k = (s->count < BookImbalanceHistory<F, W>::SHORT_K)
                          ? s->count
                          : BookImbalanceHistory<F, W>::SHORT_K;
    return FPN_DivNoAssert(s->short_sum, FPN_FromDouble<F>((double)effective_k));
}

// Most recent pushed sample. Zero when buffer is empty.
template <unsigned F, unsigned W = 1024>
static inline FPN_Binary<F> BookImbHistory_Last(const BookImbalanceHistory<F, W> *s) {
    if (s->count <= 0) return FPN_Zero<F>();
    int idx = (s->head - 1 + (int)W) % (int)W;
    return s->samples[idx];
}

// Mean over last K samples (K <= count). O(K) — called once per slow path
// so cost is bounded.
template <unsigned F, unsigned W = 1024>
static inline FPN_Binary<F> BookImbHistory_MeanShort(const BookImbalanceHistory<F, W> *s, int k) {
    if (s->count <= 0 || k <= 0) return FPN_Zero<F>();
    if (k > s->count) k = s->count;
    FPN_Binary<F> acc = FPN_Zero<F>();
    // Walk backward from head — newest k samples are at head-1, head-2, ...
    for (int i = 0; i < k; i++) {
        int idx = (s->head - 1 - i + (int)W) % (int)W;
        acc = FPN_Add(acc, s->samples[idx]);
    }
    return FPN_DivNoAssert(acc, FPN_FromDouble<F>((double)k));
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[BookImbHistory_Push]
//======================================================================

//======================================================================
// [STRUCT]_[FlowState]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [DATA_ORIENTED_DESIGN] [DETERMINISM]]
// [SCOPE]_[NODE]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[signed-volume EWMAs at 10s / 1m / 5m half-lives (D.2) — continuous-time decay, cadence-independent; whole struct is one HOT cache line]
// [REFERENCE]_[INVARIANT]_[H6]
//======================================================================
// [CODE]
//======================================================================
struct alignas(64) FlowState {
    double ewma_10s;     // signed-volume EWMA, half-life 10s
    double ewma_1m;      // half-life 60s
    double ewma_5m;      // half-life 300s
    uint64_t last_us;    // timestamp of last push (0 = no prior)
};
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
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
//======================================================================
// [COMMENT]_[layout — v5.15.5.D.A]
//----------------------------------------------------------------------
// v5.15.5.D.A — alignas(64) ensures FlowState's 32 B never straddles two
// cache lines. All 4 fields are HOT (Push and read both touch all 4 every
// slow-path cycle); no HOT/WARM/COLD tier needed (whole struct fits in 1
// cache line). Trailing 32 B pad is structural minimum given alignas(64)
// requirement (32 B natural; pad to 64).
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
//----------------------------------------------------------------------
// [SIZE]_[64B]
// [ALIGN]_[64]
// [CACHE_LINES]_[1]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[FlowState]
//======================================================================

// v5.15.5.D.A — Layout lock for FlowState.
// [ASSERT]_[LAYOUT_LOCK]_[sizeof(FlowState) == 64]
static_assert(sizeof(FlowState) == 64,
    "FlowState sizeof MUST be 64 B (1 cache line).");
// [ASSERT]_[LAYOUT_LOCK]_[offsetof(ewma_10s) == 0]
static_assert(offsetof(FlowState, ewma_10s) == 0,
    "FlowState fields MUST sit at offset 0.");
// [ASSERT]_[LAYOUT_LOCK]_[alignof(FlowState) == 64]
static_assert(alignof(FlowState) == 64,
    "FlowState MUST be cache-line aligned.");

//======================================================================
// [FUNCTION]_[FlowState_Push]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [DETERMINISM]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[EWMA decay push — dt-scaled exp decay through FPN_Exp for bytewise determinism; first-push seeds, non-advancing timestamps accumulate; Init rides in this section]
//======================================================================
// [CODE]
//======================================================================
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
// Full RegimeSignals→FPN_Binary cascade is a v5.11 ship (large blast radius).
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

    // FPN_Binary-native decay: -dt / halflife → exp via Taylor.
    FPN_Binary<64> dt_fpn = FPN_FromDouble<64>(dt);
    FPN_Binary<64> hl_10s = FPN_FromDouble<64>(10.0);
    FPN_Binary<64> hl_1m  = FPN_FromDouble<64>(60.0);
    FPN_Binary<64> hl_5m  = FPN_FromDouble<64>(300.0);

    // -dt/halflife for the decay exponent (was `.sign = 1` on the positive div; 16B two's-comp → negate).
    FPN_Binary<64> arg_10s = FPN_Negate(FPN_DivNoAssert(dt_fpn, hl_10s));
    FPN_Binary<64> arg_1m  = FPN_Negate(FPN_DivNoAssert(dt_fpn, hl_1m));
    FPN_Binary<64> arg_5m  = FPN_Negate(FPN_DivNoAssert(dt_fpn, hl_5m));

    double decay_10s = FPN_ToDouble(FPN_Exp(arg_10s));
    double decay_1m  = FPN_ToDouble(FPN_Exp(arg_1m));
    double decay_5m  = FPN_ToDouble(FPN_Exp(arg_5m));

    s->ewma_10s = s->ewma_10s * decay_10s + signed_volume;
    s->ewma_1m  = s->ewma_1m  * decay_1m  + signed_volume;
    s->ewma_5m  = s->ewma_5m  * decay_5m  + signed_volume;
    s->last_us = timestamp_us;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[FlowState_Push]
//======================================================================

//======================================================================
// [STRUCT]_[LargeTradeState]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [BINARY_FP] [DATA_ORIENTED_DESIGN]]
// [SCOPE]_[NODE]
// [INSTANTIATION]_[[1024]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[ring of recent trade sizes (D.4) — running sum + sum_sq give O(1) mean/variance for the large-trade z-score; HOT scalars lead, COLD ring follows]
// [REFERENCE]_[INVARIANT]_[[H4] [H6]]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F, unsigned W = 1024>
struct alignas(64) LargeTradeState {
    // HOT cluster (offset 0; touched every slow-path cycle by Push + read fns)
    FPN_Binary<F> sum;          // running sum
    FPN_Binary<F> sum_sq;       // running sum of squares
    int    count;
    int    head;
    // COLD cluster (sizes[head] touched 1× per Push; ZScore is
    // already O(1) using running sum + sum_sq, no walk)
    FPN_Binary<F> sizes[W];     // ring of recent sizes
};
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Ring buffer of recent trade sizes. Maintains running sum + sum_sq for
// O(1) mean + variance. Z-score of a current trade size: (size - mean) /
// stddev. Output is double (matches RegimeSignals.large_trade_z's type
// to mirror tick_rate_z's pattern).
//
// Window W default 1024 ≈ 17 minutes at slow_path=100 cadence under a
// busy market. Same W as BookImbalanceHistory for symmetry.
//======================================================================
// [COMMENT]_[layout — v5.15.5.D.A]
//----------------------------------------------------------------------
// v5.15.5.D.A — alignas(64) + HOT-first reorg per cache-layout-discipline-
// for-hot-side-structs.md Rule 4. HOT cluster (sum + sum_sq + count + head)
// leads the struct; COLD sizes[W] follows (exact offsets + sizeof pinned by
// the layout-lock asserts below — the mechanical SSoT).
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
//----------------------------------------------------------------------
// [SIZE]_[16448B]
// [ALIGN]_[64]
// [CACHE_LINES]_[257]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[LargeTradeState]
//======================================================================

// v5.15.5.D.A — Layout lock for LargeTradeState<64, 1024>. Typedef wraps
// template instantiation for offsetof macro-arg parsing.
using LargeTradeStateDefaultT = LargeTradeState<64, 1024>;
// [ASSERT]_[LAYOUT_LOCK]_[sizeof(LargeTradeState<64,1024>) == 16448]
static_assert(sizeof(LargeTradeStateDefaultT) == 16448,
    "LargeTradeState<64,1024> sizeof MUST be 16,448 B (257 cache lines; Ship-A 16B FPN_Binary, was 24,640).");
// [ASSERT]_[LAYOUT_LOCK]_[offsetof(sum) == 0 — HOT cluster leads]
static_assert(offsetof(LargeTradeStateDefaultT, sum) == 0,
    "LargeTradeState HOT scalar `sum` MUST sit at offset 0.");
// [ASSERT]_[LAYOUT_LOCK]_[offsetof(sizes) == 48 — COLD ring after the HOT cluster]
static_assert(offsetof(LargeTradeStateDefaultT, sizes) == 48,
    "LargeTradeState COLD `sizes` MUST sit at offset 48 (after HOT cluster; Ship-A 16B FPN_Binary, was 56).");
// [ASSERT]_[LAYOUT_LOCK]_[alignof(LargeTradeState) == 64]
static_assert(alignof(LargeTradeStateDefaultT) == 64,
    "LargeTradeState MUST be cache-line aligned.");

//======================================================================
// [FUNCTION]_[LargeTradeState_Push]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [BINARY_FP]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[ring push maintaining sum + sum_sq (evict-before-overwrite); Init/ZScore/Last ride in this section]
//======================================================================
// [CODE]
//======================================================================
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
static inline void LargeTradeState_Push(LargeTradeState<F, W> *s, FPN_Binary<F> size) {
    if (s->count >= (int)W) {
        FPN_Binary<F> evicted = s->sizes[s->head];
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
// v5.10.0b.2.5.C: variance + stddev computed in FPN_Binary<F> via FPN_Sqrt
// (bytewise-deterministic). Return type stays `double` to keep
// touch-site count low — the RegimeSignals.large_trade_z field stays
// double, so downstream consumers don't change. The bytewise contract
// is enforced through deterministic FPN_Sub/Mul/Sqrt/DivNoAssert and
// the final FPN_ToDouble.
template <unsigned F, unsigned W = 1024>
static inline double LargeTradeState_ZScore(const LargeTradeState<F, W> *s, FPN_Binary<F> current_size) {
    if (s->count < 2) return 0.0;
    FPN_Binary<F> n_fpn   = FPN_FromInt<F>(s->count);
    FPN_Binary<F> mean    = FPN_DivNoAssert(s->sum, n_fpn);
    FPN_Binary<F> mean_sq = FPN_DivNoAssert(s->sum_sq, n_fpn);
    FPN_Binary<F> var     = FPN_Sub(mean_sq, FPN_Mul(mean, mean));
    if (var.v <= 0) return 0.0;  // var <= 0 → degenerate (was IsZero || sign!=0; 16B two's-comp)

    FPN_Binary<F> stddev = FPN_Sqrt(var);
    if (FPN_IsZero(stddev)) return 0.0;
    FPN_Binary<F> z = FPN_DivNoAssert(FPN_Sub(current_size, mean), stddev);
    return FPN_ToDouble(z);
}

// Most recent pushed size. Zero when empty.
template <unsigned F, unsigned W = 1024>
static inline FPN_Binary<F> LargeTradeState_Last(const LargeTradeState<F, W> *s) {
    if (s->count <= 0) return FPN_Zero<F>();
    int idx = (s->head - 1 + (int)W) % (int)W;
    return s->sizes[idx];
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[LargeTradeState_Push]
//======================================================================

//======================================================================
// [STRUCT]_[SpreadState]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [BINARY_FP] [DATA_ORIENTED_DESIGN]]
// [SCOPE]_[NODE]
// [INSTANTIATION]_[[1024]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[ring of bid-ask spread samples (D.3) — same running-sum shape as LargeTradeState, kept separate for call-site clarity + future divergence; feeds FEAT_SPREAD_ZSCORE]
// [REFERENCE]_[INVARIANT]_[[H4] [H6]]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F, unsigned W = 1024>
struct alignas(64) SpreadState {
    // HOT cluster (offset 0; touched every slow-path cycle)
    FPN_Binary<F> sum;
    FPN_Binary<F> sum_sq;
    int    count;
    int    head;
    // COLD cluster
    FPN_Binary<F> samples[W];
};
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
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
//======================================================================
// [COMMENT]_[layout — v5.15.5.D.A]
//----------------------------------------------------------------------
// v5.15.5.D.A — alignas(64) + HOT-first reorg per cache-layout-discipline-
// for-hot-side-structs.md Rule 4. Identical shape to LargeTradeState; HOT
// cluster (sum + sum_sq + count + head) leads, COLD samples[W] follows
// (exact offsets + sizeof pinned by the layout-lock asserts below).
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
//----------------------------------------------------------------------
// [SIZE]_[16448B]
// [ALIGN]_[64]
// [CACHE_LINES]_[257]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[SpreadState]
//======================================================================

// v5.15.5.D.A — Layout lock for SpreadState<64, 1024>.
using SpreadStateDefaultT = SpreadState<64, 1024>;
// [ASSERT]_[LAYOUT_LOCK]_[sizeof(SpreadState<64,1024>) == 16448]
static_assert(sizeof(SpreadStateDefaultT) == 16448,
    "SpreadState<64,1024> sizeof MUST be 16,448 B (257 cache lines; Ship-A 16B FPN_Binary, was 24,640).");
// [ASSERT]_[LAYOUT_LOCK]_[offsetof(sum) == 0 — HOT cluster leads]
static_assert(offsetof(SpreadStateDefaultT, sum) == 0,
    "SpreadState HOT scalar `sum` MUST sit at offset 0.");
// [ASSERT]_[LAYOUT_LOCK]_[offsetof(samples) == 48 — COLD ring after the HOT cluster]
static_assert(offsetof(SpreadStateDefaultT, samples) == 48,
    "SpreadState COLD `samples` MUST sit at offset 48 (after HOT cluster; Ship-A 16B FPN_Binary, was 56).");
// [ASSERT]_[LAYOUT_LOCK]_[alignof(SpreadState) == 64]
static_assert(alignof(SpreadStateDefaultT) == 64,
    "SpreadState MUST be cache-line aligned.");

//======================================================================
// [FUNCTION]_[SpreadState_Push]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [BINARY_FP]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[ring push maintaining sum + sum_sq (evict-before-overwrite); Init/ZScore/Last ride in this section]
//======================================================================
// [CODE]
//======================================================================
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
static inline void SpreadState_Push(SpreadState<F, W> *s, FPN_Binary<F> sample) {
    if (s->count >= (int)W) {
        FPN_Binary<F> evicted = s->samples[s->head];
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

// v5.10.0b.2.5.C: variance + stddev computed in FPN_Binary<F> via FPN_Sqrt.
// Same boundary-stable shape as LargeTradeState_ZScore — return double
// to avoid cascading into RegimeSignals.spread_zscore consumers.
template <unsigned F, unsigned W = 1024>
static inline double SpreadState_ZScore(const SpreadState<F, W> *s, FPN_Binary<F> current_spread) {
    if (s->count < 2) return 0.0;
    FPN_Binary<F> n_fpn   = FPN_FromInt<F>(s->count);
    FPN_Binary<F> mean    = FPN_DivNoAssert(s->sum, n_fpn);
    FPN_Binary<F> mean_sq = FPN_DivNoAssert(s->sum_sq, n_fpn);
    FPN_Binary<F> var     = FPN_Sub(mean_sq, FPN_Mul(mean, mean));
    if (var.v <= 0) return 0.0;   // var <= 0 → degenerate (was IsZero || sign!=0; 16B two's-comp)

    FPN_Binary<F> stddev = FPN_Sqrt(var);
    if (FPN_IsZero(stddev)) return 0.0;
    FPN_Binary<F> z = FPN_DivNoAssert(FPN_Sub(current_spread, mean), stddev);
    return FPN_ToDouble(z);
}

template <unsigned F, unsigned W = 1024>
static inline FPN_Binary<F> SpreadState_Last(const SpreadState<F, W> *s) {
    if (s->count <= 0) return FPN_Zero<F>();
    int idx = (s->head - 1 + (int)W) % (int)W;
    return s->samples[idx];
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[SpreadState_Push]
//======================================================================

#endif // FLOW_FEATURES_HPP
