// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[ML_Headers/RollingTurnover.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [MONITORING_PLANE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[top-K arm-pick stability (v5.14.1.G) — symmetric-difference ratio between consecutive masks; observability only, never a trading input]
// [CONTAINS]
//   - [STRUCT]_[RollingTurnover]
//   - [FUNCTION]_[RollingTurnover_Push]   (Init/topk_mask_from_weights/Compute read-outs share the block)
//======================================================================================================
// Tracks how stable the ensemble's top-K arm picks are over time.
// Symmetric-difference between consecutive top-K bit-masks → ratio in
// [0, 1]:
//   0.0 = identical sets every cycle (stable convictions)
//   1.0 = fully disjoint sets every cycle (thrashing model)
//
// Per-core diagnostic surfaced via PerNodeSnap.ml_portfolio_turnover.
// Operator visibility metric — does NOT influence trading decisions.
// State lives on EventLoopState.nodes[].turnover (per-core, ephemeral).
// NOT on ConfidenceScorer (would break the legacy PortfolioController
// snapshot save/load pair per Class 4 — snapshot save/load asymmetry).
//
// 8-arm ensemble fits in uint8_t bit-mask; popcount is 1 cycle on x86.
//======================================================================================================
#ifndef ROLLING_TURNOVER_HPP
#define ROLLING_TURNOVER_HPP

#include <stdint.h>
#include <string.h>

#define ROLLING_TURNOVER_MAX_WINDOW 256
#define ROLLING_TURNOVER_MAX_TOPK   8   // matches ENSEMBLE_HORIZON_MAX

//======================================================================
// [STRUCT]_[RollingTurnover]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [MONITORING_PLANE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the top-K mask ring + head/count/window/topk cursor + last-push ratio; ephemeral per-node diagnostic state]
//======================================================================
// [CODE]
//======================================================================
struct RollingTurnover {
    uint8_t topk_mask_ring[ROLLING_TURNOVER_MAX_WINDOW];
    int     head;             // next write slot (modulo window)
    int     count;            // total pushes (≤ window)
    int     window;           // active window size (cfg-tunable; ≤ MAX_WINDOW)
    int     topk;             // K value (cfg-tunable; ≤ MAX_TOPK)
    double  last_turnover;    // most recent push's symmetric-diff ratio
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
// [SIZE]_[280B]
// [ALIGN]_[8]
// [CACHE_LINES]_[5]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[RollingTurnover]
//======================================================================

//======================================================================
// [FUNCTION]_[RollingTurnover_Push]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [MONITORING_PLANE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[popcount(prev XOR cur) / popcount-union ratio into the ring; Init/topk_mask_from_weights/Compute ride in this block]
// [REFERENCE]_[CLASS]_[28]
// [REFERENCE]_[INVARIANT]_[H20]
//======================================================================
// [CODE]
//======================================================================

//======================================================================================================
// INIT
//======================================================================================================
// Validates window + topk; clamps to safe range. Zero-init buffers.
//======================================================================================================
static inline void RollingTurnover_Init(RollingTurnover *rt,
                                          int window, int topk) {
    if (!rt) return;
    memset(rt, 0, sizeof(*rt));
    if (window < 2) window = 2;
    if (window > ROLLING_TURNOVER_MAX_WINDOW) window = ROLLING_TURNOVER_MAX_WINDOW;
    if (topk < 1) topk = 1;
    if (topk > ROLLING_TURNOVER_MAX_TOPK) topk = ROLLING_TURNOVER_MAX_TOPK;
    rt->window = window;
    rt->topk   = topk;
}

//======================================================================================================
// TOP-K MASK FROM WEIGHTS
//======================================================================================================
// Compute top-K bit-mask from weights array (descending sort by weight,
// take top K indices, set bit i for each top-K member).
//
// O(N*K) selection — fine for N=8, K=3 = 24 ops per call (~30-50ns).
// Branchless inner loop where possible (compiler should vectorize the
// max-finding scan).
//======================================================================================================
static inline uint8_t topk_mask_from_weights(const double* weights, int n,
                                               int topk) {
    if (!weights || n <= 0 || topk <= 0) return 0;
    if (topk >= n) {
        // All arms qualify; mask = (1<<n) - 1.
        // Cap at MAX_TOPK to avoid wider bit shifts than uint8_t supports.
        int cap = (n < ROLLING_TURNOVER_MAX_TOPK) ? n : ROLLING_TURNOVER_MAX_TOPK;
        return (uint8_t)((1u << cap) - 1u);
    }
    int picked[ROLLING_TURNOVER_MAX_TOPK] = {0};
    uint8_t mask = 0;
    int n_capped = (n > ROLLING_TURNOVER_MAX_TOPK) ? ROLLING_TURNOVER_MAX_TOPK : n;
    int k_capped = (topk > n_capped) ? n_capped : topk;
    for (int k = 0; k < k_capped; k++) {
        int    best_idx = -1;
        double best_val = -1e300;
        for (int i = 0; i < n_capped; i++) {
            if (picked[i]) continue;
            // v5.15.5.F.4d Step 6 (§ L) — Class 28 cmov branchless argmax (H20).
            // `picked[i] continue` above stays as branch (state-dependent; per-iter result varies but
            // predicts well per call; not the data-dependent dispatch H20 targets).
            int win  = weights[i] > best_val;
            best_val = win ? weights[i] : best_val;
            best_idx = win ? i          : best_idx;
        }
        if (best_idx >= 0) {
            picked[best_idx] = 1;
            mask |= (uint8_t)(1u << best_idx);
        }
    }
    return mask;
}

//======================================================================================================
// PUSH
//======================================================================================================
// Append a new top-K mask to the ring; compute symmetric difference
// against previous mask in same call. Returns the just-computed
// turnover ratio for this push (caller may use for spot-checks; the
// rolling average comes from RollingTurnover_Compute).
//
// Symmetric difference: (A XOR B) bits give the bits that differ.
// Divide by (topk * 2) because |A∆B| ≤ |A|+|B| = 2K when both have
// exactly K bits. Yields [0, 1] range.
//======================================================================================================
static inline double RollingTurnover_Push(RollingTurnover *rt, uint8_t mask) {
    if (!rt || rt->window <= 0 || rt->topk <= 0) return 0.0;
    double turnover = 0.0;
    if (rt->count > 0) {
        int prev_idx = (rt->head - 1 + rt->window) % rt->window;
        uint8_t prev_mask = rt->topk_mask_ring[prev_idx];
        uint8_t diff = mask ^ prev_mask;
        int diff_bits = __builtin_popcount(diff);
        turnover = (double)diff_bits / (double)(rt->topk * 2);
    }
    int slot = rt->head % rt->window;
    rt->topk_mask_ring[slot] = mask;
    rt->head = (rt->head + 1) % rt->window;
    if (rt->count < rt->window) rt->count++;
    rt->last_turnover = turnover;
    return turnover;
}

//======================================================================================================
// COMPUTE
//======================================================================================================
// Average symmetric-difference ratio across the window (count - 1
// pair-wise diffs from the ring's chronological order). Cold-start
// (count < 2): returns 0.0 (no diffs to compute).
//
// Cost: O(window) per call; ~5ns per popcount × window = ~500ns at
// window=100. Called per slow-path snapshot publish; within budget.
//
// FUTURE OPPORTUNITY (per CLAUDE.md item 17): cache the per-cycle
// popcount sum incrementally on Push → O(1) per Compute. Defer
// until profiler flags this as load-bearing.
//======================================================================================================
static inline double RollingTurnover_Compute(const RollingTurnover *rt) {
    if (!rt || rt->count < 2 || rt->topk <= 0) return 0.0;
    double sum = 0.0;
    int n = rt->count;
    int w = rt->window;
    for (int i = 1; i < n; i++) {
        int curr_idx = (rt->head - n + i + w) % w;
        int prev_idx = (rt->head - n + i - 1 + w) % w;
        uint8_t diff = rt->topk_mask_ring[curr_idx] ^ rt->topk_mask_ring[prev_idx];
        sum += (double)__builtin_popcount(diff) / (double)(rt->topk * 2);
    }
    return sum / (double)(n - 1);
}

//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[RollingTurnover_Push]
//======================================================================
#endif // ROLLING_TURNOVER_HPP
