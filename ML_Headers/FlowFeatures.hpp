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
//     RegimeSignals.flow_* fields are ALSO double (RegimeDetector.hpp
//     flow_10s/1m/5m); Regime_ComputeSignals copies them RAW, and the
//     conversion to FPN_Binary happens later, at ML_Compute_Flow10s/1m/5m
//     (FeatureRegistry.hpp). Corrected E.1.2.G — this comment previously
//     claimed the RegimeSignals fields were FPN_Binary "converted in
//     Regime_ComputeSignals"; both halves were false, and being false about
//     WHERE the conversion happens is what makes a stale comment expensive.
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
#include <type_traits>  // E.1.2.G — D-465's "the wrong call does not compile" is asserted, not claimed

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
// [UPDATED]_[2026-08-10]
//----------------------------------------------------------------------
// [SIZE]_[16448B]
// [ALIGN]_[64]
// [CACHE_LINES]_[257]
// [STRADDLE]_[unverified: samples]
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
// [REFERENCE]_[DESIGN_SPEC]_[sliding-window-online-statistics-pattern.md]
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
// [STRUCT]_[EwmaSum] / [STRUCT]_[EwmaAvg]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [BINARY_FP] [DETERMINISM]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the two exponential-decay recurrences as distinct TYPES (D-465) — the state carries its own kind, so calling the wrong step function does not compile]
// [REFERENCE]_[DESIGN_SPEC]_[[canonical-sister-extension-discipline]]
//======================================================================
// [CODE]
//======================================================================
// D-465. There are TWO decay recurrences here and they are NOT interchangeable
// (the long-form why is in the [COMMENT] block under Ewma_AccumulateStep below):
//
//   SUM      e <- e*d + x          flow quantities; no fixed point in x
//   AVERAGE  e <- e*d + (1-d)*x    levels; fixed point AT x, so half-life-stable
//
// Until now the rule "SUM for flow, AVERAGE for levels" was a COMMENT, and a
// comment protects only the instance in front of it. The ladder adds ten
// accumulators — 2 SUM, 8 AVERAGE — and a mis-assignment is silent: at a 2h
// half-life a price SUM settles near 432000*price and the feature ships as a
// TICK-RATE proxy wearing a momentum name. Nothing crashes; the model just
// learns noise.
//
// Making the STATE carry the kind turns that from a convention into a type
// error. `EwmaSum_Step(&price_ema, ...)` does not compile when `price_ema` is
// an EwmaAvg. Chosen over D-458's FOREACH_LONG_EWMA registry because it closes
// the same class with no MetaRegistry row (H15), no PARENT row (H19), no DOMAIN
// token, and no CI surface at all — and because a per-row `kind` COLUMN would
// have left the VWAP pair legal-but-wrong: both legs work under either
// recurrence (the factor cancels in the ratio) but a MIX inflates VWAP ~432000x,
// which a column cannot forbid and a shared type makes unrepresentable.
//
// One field each, deliberately: EwmaAvg needs no companion weight accumulator
// because Ewma_NormalizedStep already carries the (1-d) weight in the recurrence
// itself. Had it needed one, 8 AVERAGE fields x 32 B would have pushed FlowState
// past 256 B and changed the growth's cache-line math.
struct EwmaSum { FPN_Binary<64> v; };   // decaying SUM     — flow
struct EwmaAvg { FPN_Binary<64> v; };   // decaying AVERAGE — levels

// [ASSERT]_[LAYOUT_LOCK]_[sizeof(EwmaSum) == 16 && sizeof(EwmaAvg) == 16]
static_assert(sizeof(EwmaSum) == 16 && sizeof(EwmaAvg) == 16,
    "Ewma* wrappers MUST stay single-FPN_Binary<64> sized — FlowState's 256 B budget "
    "and its cache-line math assume 16 B per accumulator.");
static_assert(alignof(EwmaSum) == 16 && alignof(EwmaAvg) == 16,
    "Ewma* alignment feeds FlowState's field packing; a change moves every offset after it.");
//======================================================================
// [END_CODE]
//======================================================================
// [END_STRUCT]_[EwmaSum]
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
// Half-lives in MICROSECONDS — the unit FlowState_Push is handed. Named + integer
// so the decay ratio can be formed without ever crossing `double` (see the push).
// These are compile-time constants, not persisted or wire-visible (no H21 slot).
static constexpr int64_t FLOW_HALFLIFE_10S_US =  10'000'000;
static constexpr int64_t FLOW_HALFLIFE_1M_US  =  60'000'000;
static constexpr int64_t FLOW_HALFLIFE_5M_US  = 300'000'000;

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
// [FUNCTION]_[Ewma_AccumulateStep]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [DETERMINISM]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the decaying-SUM step: prev*decay + sample. For FLOW quantities (signed volume), where the accumulated total IS the signal]
//======================================================================
// [CODE]
//======================================================================
static inline double Ewma_AccumulateStep(double prev, double decay, double sample) {
    return prev * decay + sample;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]_[why these two forms are named — E.1.2.G]
//----------------------------------------------------------------------
// There are TWO exponential-decay recurrences in this file's problem space and
// they are NOT interchangeable. Naming them is the point of this pair: the
// feature-horizon ladder's first draft proposed reusing FlowState_Push for
// long-half-life PRICE emas on the reasoning "it is the proven pattern with new
// half-life constants" — which is true of the DECAY, and false of the
// RECURRENCE.
//
//   ACCUMULATE (this fn):  e <- e*d + x        -- a decaying SUM
//   NORMALIZE  (below):    e <- e*d + (1-d)*x  -- a decaying AVERAGE
//
// For signed volume the sum is correct and intended: "how much net flow has
// there been lately, weighted toward recent". It has no fixed point in x -- for
// a constant x it converges to x/(1-d), which GROWS as the half-life grows.
//
// That property is fatal for a LEVEL. At half-life 2h and a ~60/s push cadence,
// (1-d) is about 2.3e-6, so a price accumulator settles near 432000*price and
// (price - ema)/ema pins to about -1.0 -- a constant whose only real variation
// is the TICK RATE. Four ret_ema_* rows and rvol_ratio would have shipped as
// tick-rate proxies wearing momentum names, and nothing would have crashed.
//
// Rule: SUM for flow, AVERAGE for levels (price, realized vol, VWAP legs).
//======================================================================
// [END_FUNCTION]_[Ewma_AccumulateStep]
//======================================================================

//======================================================================
// [FUNCTION]_[Ewma_NormalizedStep]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [BINARY_FP] [DETERMINISM]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the decaying-AVERAGE step: prev*decay + (1-decay)*sample, FPN_Binary per D-455. For LEVELS — has a fixed point at the sample, so it is half-life-stable]
// [REFERENCE]_[INVARIANT]_[[H4] [H11]]
//======================================================================
// [CODE]
//======================================================================
static inline FPN_Binary<64> Ewma_NormalizedStep(FPN_Binary<64> prev,
                                                 FPN_Binary<64> decay,
                                                 FPN_Binary<64> sample) {
    const FPN_Binary<64> one = FPN_FromDouble<64>(1.0);
    return FPN_Add(FPN_Mul(prev, decay), FPN_Mul(FPN_Sub(one, decay), sample));
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// FPN_Binary rather than double per D-455: these accumulate over hours-to-days
// and are the rider-2 persist targets, so determinism across runs and binaries
// (H9/H10) governs, not throughput. Branchless + constant-work (H11).
//
// SEEDING is the caller's job and it matters more here than for the sum form.
// From a zero start the average climbs toward the sample over ~one half-life;
// at 24h that is a day of a feature reading low with no indication it is
// warming. Callers seed on first push (the FlowState_Push convention) AND owe a
// min-history gate before serving the row.
//======================================================================
// [END_FUNCTION]_[Ewma_NormalizedStep]
//======================================================================

//======================================================================
// [FUNCTION]_[EwmaSum_Step] / [FUNCTION]_[EwmaAvg_Step] (+ their Seed pair)
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [BINARY_FP] [DETERMINISM]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the typed steps (D-465) — each accepts ONLY its own state type, so a wrong-form call is a compile error rather than a silently wrong feature]
// [REFERENCE]_[INVARIANT]_[[H4] [H11]]
//======================================================================
// [CODE]
//======================================================================
// NAMED AFTER THE STATE, not after the recurrence — deliberately, and this is the
// whole point of the pair.
//
// The 2 new SUM rows are FPN_Binary per D-455, but Ewma_AccumulateStep above is
// `double` (it serves the three LEGACY ewma_10s/1m/5m fields, which D-455
// deliberately leaves as double). So an FPN accumulate step is genuinely needed.
// Adding it as `Ewma_AccumulateStep(FPN_Binary<64>, ...)` would create an OVERLOAD
// PAIR on the accumulate name — which is exactly the reach-for-it-by-imitation
// affordance the [COMMENT] block above was written to kill, re-created by the fix
// for it. Overload resolution would silently pick a form based on argument type
// rather than on the caller's intent.
//
// Keying the name to the STATE instead means the call site reads as an assertion
// about what kind of accumulator this is, and the compiler checks that assertion.
static inline void EwmaSum_Step(EwmaSum *s, FPN_Binary<64> decay, FPN_Binary<64> sample) {
    s->v = FPN_Add(FPN_Mul(s->v, decay), sample);            // e <- e*d + x
}
static inline void EwmaAvg_Step(EwmaAvg *s, FPN_Binary<64> decay, FPN_Binary<64> sample) {
    s->v = Ewma_NormalizedStep(s->v, decay, sample);         // e <- e*d + (1-d)*x
}

// Seeding is the caller's job (see Ewma_NormalizedStep's [COMMENT]) and matters
// most for the AVERAGE form: from zero it climbs toward the sample over ~one
// half-life, so a 24h row would read low for a day with nothing indicating it is
// still warming. Named rather than left as a bare `s->v = x` so the first-push arm
// is greppable, and so the seed cannot be mistaken for a step.
//
// The min-history gate (D-467) is the OTHER half of that contract and is not
// optional: seeding fixes the starting VALUE, it does not make the row meaningful
// before enough history exists.
static inline void EwmaSum_Seed(EwmaSum *s, FPN_Binary<64> sample) { s->v = sample; }
static inline void EwmaAvg_Seed(EwmaAvg *s, FPN_Binary<64> sample) { s->v = sample; }

// ── D-465's claim, ASSERTED rather than stated ──────────────────────────────
// The decision's whole value is "a wrong-form call does not compile". A runtime
// char cannot demonstrate that — code which fails to compile cannot be executed
// by a test. These four static_asserts ARE the proof, and they re-run on every
// build rather than once at review time.
//
// The positive pair is what keeps the negative pair non-vacuous: without it, a
// typo that broke BOTH signatures would still satisfy the "must not accept"
// assertions and look like success. Same shape as the non-vacuity controls on the
// SHALT and arch-drift chars this ship already carries.
static_assert(std::is_invocable_v<decltype(EwmaSum_Step) &, EwmaSum *, FPN_Binary<64>, FPN_Binary<64>>,
    "EwmaSum_Step must accept its OWN state (non-vacuity control for the assertions below).");
static_assert(std::is_invocable_v<decltype(EwmaAvg_Step) &, EwmaAvg *, FPN_Binary<64>, FPN_Binary<64>>,
    "EwmaAvg_Step must accept its OWN state (non-vacuity control for the assertions below).");
static_assert(!std::is_invocable_v<decltype(EwmaSum_Step) &, EwmaAvg *, FPN_Binary<64>, FPN_Binary<64>>,
    "D-465 VIOLATED: EwmaSum_Step accepts an EwmaAvg* — the SUM recurrence on a LEVEL is the "
    "432000x tick-rate-proxy bug the type split exists to make unrepresentable.");
static_assert(!std::is_invocable_v<decltype(EwmaAvg_Step) &, EwmaSum *, FPN_Binary<64>, FPN_Binary<64>>,
    "D-465 VIOLATED: EwmaAvg_Step accepts an EwmaSum* — a flow quantity normalized as a level "
    "silently loses the accumulated total that IS the signal.");
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[EwmaSum_Step]
//======================================================================

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
        s->ewma_10s = signed_volume;  // [LAT_EXEMPT]_[D-455 feature<->double seam: FlowState's EWMAs are double BY DECISION; H4-exempt]
        s->ewma_1m  = signed_volume;  // [LAT_EXEMPT]_[D-455 feature<->double seam: FlowState's EWMAs are double BY DECISION; H4-exempt]
        s->ewma_5m  = signed_volume;  // [LAT_EXEMPT]_[D-455 feature<->double seam: FlowState's EWMAs are double BY DECISION; H4-exempt]
        s->last_us  = timestamp_us;
        return;
    }
    if (timestamp_us <= s->last_us) {
        s->ewma_10s += signed_volume;  // [LAT_EXEMPT]_[D-455 feature<->double seam: FlowState's EWMAs are double BY DECISION; H4-exempt]
        s->ewma_1m  += signed_volume;  // [LAT_EXEMPT]_[D-455 feature<->double seam: FlowState's EWMAs are double BY DECISION; H4-exempt]
        s->ewma_5m  += signed_volume;  // [LAT_EXEMPT]_[D-455 feature<->double seam: FlowState's EWMAs are double BY DECISION; H4-exempt]
        return;
    }
    // v5.15.5.F.4d.1.E.1.2.G (re-gate F2) — the decay ratio is formed in INTEGER
    // MICROSECOND space and crosses into FPN exactly once. The prior form rounded
    // TWICE before the divide (`(double)dt_us / 1e6`, then FPN_FromDouble) and put a
    // hardware `divsd` on the slow path, which check_latency_path_conformance.py
    // gates (H4 / §5). dt_us/hl_us is the same dimensionless ratio as dt_s/hl_s, and
    // integer→FPN is EXACT below 2^63 — so this is strictly MORE precise, not a trade.
    const uint64_t dt_us = timestamp_us - s->last_us;   // > 0: the <= branch returned

    // FPN_Binary-native decay: -dt / halflife → exp via Taylor.
    FPN_Binary<64> dt_fpn = FPN_FromInt<64>((int64_t)dt_us);
    FPN_Binary<64> hl_10s = FPN_FromInt<64>(FLOW_HALFLIFE_10S_US);
    FPN_Binary<64> hl_1m  = FPN_FromInt<64>(FLOW_HALFLIFE_1M_US);
    FPN_Binary<64> hl_5m  = FPN_FromInt<64>(FLOW_HALFLIFE_5M_US);

    // -dt/halflife for the decay exponent (was `.sign = 1` on the positive div; 16B two's-comp → negate).
    FPN_Binary<64> arg_10s = FPN_Negate(FPN_DivNoAssert(dt_fpn, hl_10s));
    FPN_Binary<64> arg_1m  = FPN_Negate(FPN_DivNoAssert(dt_fpn, hl_1m));
    FPN_Binary<64> arg_5m  = FPN_Negate(FPN_DivNoAssert(dt_fpn, hl_5m));

    double decay_10s = FPN_ToDouble(FPN_Exp(arg_10s));  // [LAT_EXEMPT]_[D-455 feature<->double seam: FlowState's EWMAs are double BY DECISION; H4-exempt]
    double decay_1m  = FPN_ToDouble(FPN_Exp(arg_1m));  // [LAT_EXEMPT]_[D-455 feature<->double seam: FlowState's EWMAs are double BY DECISION; H4-exempt]
    double decay_5m  = FPN_ToDouble(FPN_Exp(arg_5m));  // [LAT_EXEMPT]_[D-455 feature<->double seam: FlowState's EWMAs are double BY DECISION; H4-exempt]

    // ACCUMULATE form — signed volume is a flow, so the decaying SUM is the
    // intended quantity. Named rather than inlined so the ladder's LEVEL rows
    // cannot reach for this recurrence by imitation (see Ewma_NormalizedStep).
    s->ewma_10s = Ewma_AccumulateStep(s->ewma_10s, decay_10s, signed_volume);  // [LAT_EXEMPT]_[D-455 feature<->double seam: FlowState's EWMAs are double BY DECISION; H4-exempt]
    s->ewma_1m  = Ewma_AccumulateStep(s->ewma_1m,  decay_1m,  signed_volume);  // [LAT_EXEMPT]_[D-455 feature<->double seam: FlowState's EWMAs are double BY DECISION; H4-exempt]
    s->ewma_5m  = Ewma_AccumulateStep(s->ewma_5m,  decay_5m,  signed_volume);  // [LAT_EXEMPT]_[D-455 feature<->double seam: FlowState's EWMAs are double BY DECISION; H4-exempt]
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
// [UPDATED]_[2026-08-10]
//----------------------------------------------------------------------
// [SIZE]_[16448B]
// [ALIGN]_[64]
// [CACHE_LINES]_[257]
// [STRADDLE]_[unverified: sizes]
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
// [UPDATED]_[2026-08-10]
//----------------------------------------------------------------------
// [SIZE]_[16448B]
// [ALIGN]_[64]
// [CACHE_LINES]_[257]
// [STRADDLE]_[unverified: samples]
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
