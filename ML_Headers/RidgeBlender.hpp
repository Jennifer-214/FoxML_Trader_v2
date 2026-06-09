// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

#ifndef RIDGE_BLENDER_HPP
#define RIDGE_BLENDER_HPP

//======================================================================================================
// [RIDGE RISK-PARITY BLENDING — v5.14.0]
//======================================================================================================
// Multi-model alpha combination via Markowitz-style cost-aware weighting.
// Solves   w  ∝  (Σ + λI)^{-1} μ   via Cholesky decomposition, where:
//
//   Σ      = N×N correlation matrix of standardized predictions across models
//   λ      = ridge regularization (default 0.15; cfg.ridge_lambda)
//   μ[i]   = max(IC_i - cost_penalty × cost_i, min_ic_floor)
//          = net IC after cost penalization
//
// Why Ridge complements (not replaces) Exp3-IX bandit:
//   - Bandit selects ONE arm per regime (G.4 selection mode) or weighted-
//     blends across horizons via bandit weights (G.7 weighted mode), but
//     does NOT account for correlation BETWEEN model predictions. Two
//     highly-correlated models both winning bandit weight → double-
//     counting the same alpha source.
//   - Ridge weights correlated models DOWN (they're saying the same thing).
//   - cost_penalty term penalizes fee-heavy models (or AOT-vs-XGBoost
//     latency cost differences when v5.12.2.D AOT lands).
//   - λI regularization keeps the linear system well-conditioned even
//     when Σ is rank-deficient (constant predictions → singular Σ).
//   - Cholesky failure (any L[i][i] ≤ 0) → graceful fallback to uniform
//     weights; bandit dispatch path handles this transparently.
//
// FPN at boundaries (output weights), double internally for Cholesky
// numerical stability. Boundary-stable refactor pattern per CLAUDE.local.md
// 2026-05-06 — fixed-point math drives the snapshot/serialization-stable
// API; matrix decomp uses IEEE-754 double for 12-15 decimal precision.
//
// Bytewise determinism: Cholesky on doubles is deterministic given
// identical input + identical compiler flags. FPN_Sqrt (used at the
// FPN boundary) is bytewise-deterministic per v5.10.0b's Newton-Raphson
// implementation. Replay-determinism test (v5.9.2) verifies this end-
// to-end across runs.
//
// Algorithm cost (slow-path per cycle when enabled):
//   - BuildCorr:    O(N² × K) where K = recent prediction history depth.
//                   For N=8, K=64 → ~4096 ops; ~1µs.
//   - Cholesky:     O(N³ / 6) decomp + O(N²) solve. For N=8 → ~85 ops
//                   + 8 sqrts; ~2µs.
//   - Total:        ~3µs/cycle. Slow-path budget = 100µs p99; well within.
//
// Reference: FoxML_Core LIVE_TRADING/blending/ridge_weights.py:25-139
//======================================================================================================

#include <cassert>
#include <cmath>
#include <cstring>
#include <cstdint>

#if defined(__AVX512F__)
#include <immintrin.h>  // v5.14.11.B.3 — UpdateOnline + BuildCorr AVX-512 vectorization
#endif

#include "../FixedPoint/FixedPointN.hpp"

// File-scope (not tt::) — matches existing ML_Headers convention
// (BanditState, ModelHandle, EnsembleModelZoo all at file scope).

// MAX_RIDGE_MODELS bounds the matrix dimensions. Set ≥ ENSEMBLE_HORIZON_MAX
// so the same RidgeWeights struct can blend either across horizons (per
// ridge_across_horizons cfg) or within a horizon's role-arms.
static constexpr int MAX_RIDGE_MODELS = 8;

// RIDGE_HISTORY_DEPTH bounds the sliding window for online correlation.
// v5.14.11.A — single source of truth for K (was duplicated in 2 dispatch
// sites at StrategyParameters.hpp); helpers below + dispatch all reference
// this constant.
static constexpr int RIDGE_HISTORY_DEPTH = 64;

//======================================================================================================
// [RIDGEWEIGHTS STRUCT]
//======================================================================================================
// Output (boundary-stable: FPN<F> for snapshot/serialization stability)
// + internal scratch (double for Cholesky numerical stability).
//
// Allocated INSIDE EnsembleModelZoo (per-core; already heap-allocated).
// No false sharing — slow-path single-writer + single-reader on its own
// core's ezoo.
//
// Cache impact: ~5KB at MAX_RIDGE_MODELS=8 (8×8 doubles × 3 matrices +
// scratch + output). Fits in L2; not in hot-path read set.
//
// SoA layout: matrices flattened as 2D arrays for compiler vectorization
// (Cholesky inner loops can SIMD-fuse on -O2 + AVX2/AVX-512 builds).
//======================================================================================================
template <unsigned F>
struct RidgeWeights {
    // === OUTPUT (boundary; FPN for snapshot stability) ===
    // Final per-model weights, sum-to-1, all ≥ 0. Caller passes to
    // Model_Predict_Ensemble_Weighted as `const double*` after
    // FPN_ToDouble conversion at the call site.
    FPN<F>  w[MAX_RIDGE_MODELS];

    // === INTERNAL SCRATCH (double; Cholesky numerical stability) ===
    double  corr_matrix[MAX_RIDGE_MODELS][MAX_RIDGE_MODELS];
    double  mu[MAX_RIDGE_MODELS];        // net IC (cost-penalized)
    double  L[MAX_RIDGE_MODELS][MAX_RIDGE_MODELS];  // Cholesky factor (lower triangular)
    double  y[MAX_RIDGE_MODELS];         // forward-substitution intermediate
    double  w_internal[MAX_RIDGE_MODELS];

    // === DIAGNOSTICS ===
    int     n_models;                     // active model count (≤ MAX_RIDGE_MODELS)
    int     fallback_to_uniform;          // 1 if Cholesky failed (singular Σ)
    // NOTE: last_compute_us dropped v5.14.11.A — verified zero readers/writers
    // codebase-wide; pure dead-code cleanup per /parity-check re-audit.

    // === ONLINE INCREMENTAL STATE (v5.14.11.A; sliding-window K=RIDGE_HISTORY_DEPTH=64) ===
    // Sum-of-squares form for fixed-window incremental statistics per
    // DESIGN_SPECS/sliding-window-online-statistics-pattern.md. Bounded
    // inputs [0,1] (sigmoid output) × K=64 → cancellation error ~10^-14;
    // 5 orders of magnitude headroom below 1e-9 PARITY tolerance.
    // Zero-init via RidgeWeights_Init memset; no explicit init needed.
    //
    // v5.14.11.B.7 — alignas(64) on the online state cluster ensures
    // each cache-line-sized field starts on a cache line boundary.
    // online_sum_x[8] (64B = 1 cache line) at cache-aligned offset →
    // online_sum_xx[8][8] (512B = 8 cache lines) at +64 (still aligned)
    // → online_window_count (8B) at +576. Hot reads in UpdateOnline +
    // FinalizeCorrFromSums touch fewer partial cache lines.
    alignas(64) double   online_sum_x[MAX_RIDGE_MODELS];
    double               online_sum_xx[MAX_RIDGE_MODELS][MAX_RIDGE_MODELS];
    uint64_t             online_window_count;       // ≤ K; saturates at K
};

//======================================================================================================
// [CHOLESKY DECOMPOSITION — internal kernel]
//======================================================================================================
// Standard textbook algorithm. Computes lower-triangular L such that
// L × L^T = (Σ + λI). Then forward-solves L y = μ + back-solves L^T w = y.
//
// Returns 0 on success, -1 on failure (any L[i][i] ≤ 0 → singular Σ).
// On failure, caller's fallback_to_uniform path kicks in.
//
// Numerical stability: λI added to the diagonal (the "ridge") protects
// against near-singular Σ. cfg.ridge_lambda = 0.15 is the FoxML_Core-
// validated default. Lower values (e.g., 0.01) give sharper weights but
// can fail Cholesky on highly-correlated arms; higher values (≥ 1.0)
// dilute weights toward uniform.
//======================================================================================================
template <unsigned F>
inline int Cholesky_Solve(double L_out[MAX_RIDGE_MODELS][MAX_RIDGE_MODELS],
                          double y_out[MAX_RIDGE_MODELS],
                          double w_out[MAX_RIDGE_MODELS],
                          const double sigma[MAX_RIDGE_MODELS][MAX_RIDGE_MODELS],
                          const double mu[MAX_RIDGE_MODELS],
                          double ridge_lambda,
                          int n) {
    // v5.14.11.B.1 — fully branchless + constant-iter Cholesky.
    // Inner reductions iterate exactly MAX_RIDGE_MODELS=8 times (no variable-
    // iteration loops; no if guards inside reduce loops). Pattern documented
    // in DESIGN_SPECS/branchless-math-kernel-pattern.md.
    //
    // Zero-invariants establish bytewise-equivalence with prior variable-iter
    // form via IEEE-754 x - 0.0 = x exact and x * 0.0 = 0.0 exact:
    //   - L_out pre-zeroed per row at row START (replaces old "zero upper
    //     triangle" pass at row END; establishes invariant for all decomp +
    //     forward solve inner reductions)
    //   - y_out pre-zeroed before forward solve
    //   - w_out pre-zeroed before back-solve
    //
    // Compiler auto-vectorizes constant-8 inner loops via -O3 -march=native;
    // no explicit AVX-512 intrinsics in Cholesky_Solve (single code path;
    // bytewise-stable per build).
    //
    // Bytewise-equivalent to v5.14.10 scalar Cholesky output (same final
    // accumulator values; same operation order at semantic level).
    //
    // === DECOMPOSITION ===
    // L[i][i] = sqrt((Σ[i][i] + λ) - Σ_{k<i}(L[i][k]²))
    // L[i][j] = (Σ[i][j] - Σ_{k<j}(L[i][k]·L[j][k])) / L[j][j]   for j < i
    for (int i = 0; i < n; ++i) {
        // Pre-zero L_out[i][0..MAX-1] at row START — establishes the
        // constant-8 invariant for off-diagonal + diagonal reductions.
        // Replaces the old "zero upper triangle" tail pass.
        for (int k = 0; k < MAX_RIDGE_MODELS; ++k) {
            L_out[i][k] = 0.0;
        }

        // Off-diagonal: L[i][0..i-1]
        for (int j = 0; j < i; ++j) {
            double s = sigma[i][j];
            // Constant-8 reduce. L_out[i][k]=0 for k >= j (pre-zero invariant
            // + L_out[i][0..j-1] filled in earlier j iters of this row).
            // L_out[j][k]=0 for k > j (pre-zero invariant on row j; row j was
            // processed earlier when outer i was at j). Zero contributions are
            // bytewise no-ops per IEEE-754 x*0=0 and x-0=x exact.
            for (int k = 0; k < MAX_RIDGE_MODELS; ++k) {
                s -= L_out[i][k] * L_out[j][k];
            }
            // L[j][j] guaranteed > 0 from prior diagonal step.
            L_out[i][j] = s / L_out[j][j];
        }

        // Diagonal: L[i][i]
        double diag = sigma[i][i] + ridge_lambda;
        // Constant-8 reduce. L_out[i][0..i-1] just computed in off-diag iters;
        // L_out[i][k]=0 for k >= i (pre-zero invariant; k=i not yet written).
        for (int k = 0; k < MAX_RIDGE_MODELS; ++k) {
            diag -= L_out[i][k] * L_out[i][k];
        }
        if (diag <= 0.0) {
            // Singular or negative-definite (shouldn't happen with λ > 0
            // unless Σ has invalid entries). Function-entry-frequency
            // legitimate error fallback per CLAUDE.md item 18.
            return -1;
        }
        L_out[i][i] = std::sqrt(diag);
        // NOTE: old "zero upper triangle for cleanliness" pass removed;
        // pre-zero at row START already establishes the invariant.
    }

    // === FORWARD SOLVE: L y = μ ===
    // Pre-zero y_out for constant-8 reduce (k > i contributions zero out via
    // y_out[k]=0 × L_out[i][k]=0 = 0; per IEEE-754 0 - 0 = 0 exact).
    for (int k = 0; k < MAX_RIDGE_MODELS; ++k) {
        y_out[k] = 0.0;
    }
    for (int i = 0; i < n; ++i) {
        double s = mu[i];
        // Constant-8 reduce. L_out[i][k]=0 for k > i (upper triangle; pre-zero
        // invariant from decomp). y_out[k]=0 for k >= i (pre-zero of y_out
        // above; computed only for k < i in earlier outer iters).
        for (int k = 0; k < MAX_RIDGE_MODELS; ++k) {
            s -= L_out[i][k] * y_out[k];
        }
        y_out[i] = s / L_out[i][i];
    }

    // === BACK SOLVE: L^T w = y ===
    // Pre-zero w_out for constant-8 reduce. Handles k = i and k < i terms as
    // L_out[k][i] × w_out[k] = (real or 0) × 0 = 0. For k > i: real values
    // (computed in earlier outer iters as outer goes n-1 → 0).
    for (int k = 0; k < MAX_RIDGE_MODELS; ++k) {
        w_out[k] = 0.0;
    }
    for (int i = n - 1; i >= 0; --i) {
        double s = y_out[i];
        // Constant-8 reduce.
        //   k < i: L_out[k][i]=0 (upper triangle of row k; pre-zero invariant)
        //   k = i: L_out[i][i] (diagonal; non-zero) × w_out[i] (= 0; not yet
        //          written this iter; pre-zeroed). Contribution = 0.
        //   k > i: L_out[k][i] (real lower-triangle entry) × w_out[k] (real;
        //          computed in earlier outer iter since back-solve walks
        //          n-1 → 0). Real contribution.
        //   k >= n: L_out[k][i]=0 (row k pre-zeroed; never written for k >= n).
        for (int k = 0; k < MAX_RIDGE_MODELS; ++k) {
            s -= L_out[k][i] * w_out[k];
        }
        w_out[i] = s / L_out[i][i];
    }

    return 0;
}

//======================================================================================================
// [RIDGE BLENDER — main entry]
//======================================================================================================
// Compute Ridge weights from per-model IC + cost. Builds μ[i] =
// max(IC[i] - cost_penalty × cost[i], min_ic_floor); calls Cholesky_Solve;
// clips negatives + renormalizes to sum=1; falls back to uniform on
// singular Σ.
//
// Caller must have already populated `out->corr_matrix[][]` via
// RidgeBlender_BuildCorr (or equivalent — e.g., from a snapshot of last
// K predictions across models).
//
// Returns 0 on success, -1 on Cholesky failure (uniform weights returned
// in `out->w[]` regardless; fallback_to_uniform set to 1).
//
// Cfg parameters (all FPN<F> on cfg, converted to double here):
//   - ridge_lambda     (default 0.15; safe for typical N=2..8)
//   - cost_penalty     (default 0.5; fee-cost weighting in net IC)
//   - min_ic_floor     (default 0.001; prevents zero-weight starvation)
//======================================================================================================
template <unsigned F>
inline int RidgeBlender_Compute(RidgeWeights<F>* out,
                                  const double ic[],
                                  const double cost[],
                                  int n_models,
                                  double ridge_lambda,
                                  double cost_penalty,
                                  double min_ic_floor) {
    if (!out || n_models < 1) return -1;
    if (n_models > MAX_RIDGE_MODELS) n_models = MAX_RIDGE_MODELS;

    out->n_models = n_models;
    out->fallback_to_uniform = 0;

    // === Build μ vector: net IC = max(IC - cost_penalty × cost, min_ic_floor) ===
    for (int i = 0; i < n_models; ++i) {
        double net = ic[i] - cost_penalty * (cost ? cost[i] : 0.0);
        out->mu[i] = (net > min_ic_floor) ? net : min_ic_floor;
    }

    // === Cholesky solve ===
    int rc = Cholesky_Solve<F>(out->L, out->y, out->w_internal,
                                 out->corr_matrix, out->mu,
                                 ridge_lambda, n_models);

    if (rc != 0) {
        // Singular Σ — fall back to uniform weights
        out->fallback_to_uniform = 1;
        double uniform = 1.0 / (double)n_models;
        for (int i = 0; i < n_models; ++i) {
            out->w_internal[i] = uniform;
            out->w[i] = FPN_FromDouble<F>(uniform);
        }
        return -1;
    }

    // === Clip non-negative + renormalize to sum=1 ===
    // (Ridge solution can produce small negative weights for highly-
    // correlated models; we clip + renormalize per FoxML_Core convention.)
    double sum = 0.0;
    for (int i = 0; i < n_models; ++i) {
        if (out->w_internal[i] < 0.0) out->w_internal[i] = 0.0;
        sum += out->w_internal[i];
    }
    if (sum < 1e-12) {
        // All weights clipped to 0 (extreme case) — uniform fallback
        out->fallback_to_uniform = 1;
        double uniform = 1.0 / (double)n_models;
        for (int i = 0; i < n_models; ++i) {
            out->w_internal[i] = uniform;
            out->w[i] = FPN_FromDouble<F>(uniform);
        }
        return 0;
    }
    for (int i = 0; i < n_models; ++i) {
        out->w_internal[i] /= sum;
        out->w[i] = FPN_FromDouble<F>(out->w_internal[i]);
    }

    return 0;
}

//======================================================================================================
// [FINALIZE CORRELATION MATRIX FROM SUMS — shared math kernel]
//======================================================================================================
// v5.14.11.A — converts running sums (sum_x[N], sum_xx[N][N], window_count)
// into N×N correlation matrix. Sum-of-squares form per
// DESIGN_SPECS/sliding-window-online-statistics-pattern.md.
//
// Shared by BOTH:
//   - Refactored RidgeBlender_BuildCorr (full-recompute path; accumulates
//     sum_x + sum_xx single-pass over flat history, then calls this finalize)
//   - RidgeBlender_OnlineCycleStep with use_online_incremental=true
//     (uses RidgeWeights<F>::online_sum_x + online_sum_xx directly)
//
// Architectural unification per Caramel decision 2026-05-11 (C):
// single math kernel eliminates parallel correlation formulas → both paths
// produce identical output given identical sums (within ~1e-13 sum convergence
// when paths see same K records).
//
// Math (sum-of-squares form; numerically stable for bounded inputs × bounded K):
//   mean[i]   = sum_x[i] / K
//   var[i]    = max(sum_xx[i][i] / K - mean[i]², 0)
//   cov[i][j] = sum_xx[i][j] / K - mean[i] × mean[j]
//   corr[i][j] = cov[i][j] / sqrt(var[i] × var[j])    (clamped to [-1, 1])
//
// Edge cases:
//   - window_count < 2  → identity matrix (not enough data)
//   - var[i] < 1e-18    → identity row (constant predictions; Cholesky-safe)
//   - n_models < n_in_sums → unused slots zeroed (Cholesky-safe)
//
// Cost: O(N²) — N(N+1)/2 unique pairs × constant per-pair work.
//======================================================================================================
template <unsigned F>
inline void RidgeBlender_FinalizeCorrFromSums(double corr_out[MAX_RIDGE_MODELS][MAX_RIDGE_MODELS],
                                                const double sum_x[MAX_RIDGE_MODELS],
                                                const double sum_xx[MAX_RIDGE_MODELS][MAX_RIDGE_MODELS],
                                                uint64_t window_count,
                                                int n_models) {
    if (window_count < 2 || n_models < 1) {
        // Not enough data — identity matrix (Cholesky-safe with diagonal-only Σ).
        for (int i = 0; i < MAX_RIDGE_MODELS; ++i) {
            for (int j = 0; j < MAX_RIDGE_MODELS; ++j) {
                corr_out[i][j] = (i == j) ? 1.0 : 0.0;
            }
        }
        return;
    }
    if (n_models > MAX_RIDGE_MODELS) n_models = MAX_RIDGE_MODELS;

    const double K = (double)window_count;

    // === Per-model mean + variance from sums ===
    double mean[MAX_RIDGE_MODELS];
    double var[MAX_RIDGE_MODELS];
    for (int i = 0; i < n_models; ++i) {
        mean[i] = sum_x[i] / K;
        double v = sum_xx[i][i] / K - mean[i] * mean[i];
        // Numerical safety: cancellation error can produce small negatives
        // for near-constant inputs; clamp to 0 (caller's std=sqrt(var) is
        // then 0 → identity row below).
        var[i] = (v > 0.0) ? v : 0.0;
    }

    // === Pairwise correlation ===
    for (int i = 0; i < n_models; ++i) {
        corr_out[i][i] = 1.0;
        const double std_i = std::sqrt(var[i]);
        if (std_i < 1e-9) {
            // Constant predictions — identity row
            for (int j = 0; j < n_models; ++j) {
                if (j != i) corr_out[i][j] = 0.0;
            }
            continue;
        }
        for (int j = i + 1; j < n_models; ++j) {
            const double std_j = std::sqrt(var[j]);
            if (std_j < 1e-9) {
                corr_out[i][j] = 0.0;
                corr_out[j][i] = 0.0;
                continue;
            }
            const double cov = sum_xx[i][j] / K - mean[i] * mean[j];
            double c = cov / (std_i * std_j);
            // Clamp to [-1, 1] for numerical safety
            if (c > 1.0) c = 1.0;
            if (c < -1.0) c = -1.0;
            corr_out[i][j] = c;
            corr_out[j][i] = c;
        }
    }

    // Zero unused slots (n_models < MAX_RIDGE_MODELS)
    for (int i = n_models; i < MAX_RIDGE_MODELS; ++i) {
        for (int j = 0; j < MAX_RIDGE_MODELS; ++j) {
            corr_out[i][j] = (i == j) ? 1.0 : 0.0;
            corr_out[j][i] = corr_out[i][j];
        }
    }
}


//======================================================================================================
// [BUILD CORRELATION MATRIX — from prediction history; single-pass sum-of-squares]
//======================================================================================================
// v5.14.11.A REFACTORED — single-pass sum-of-squares accumulation +
// shared FinalizeCorrFromSums. Replaces the prior 3-pass mean/var/corr
// recomputation; unified math kernel with online incremental path.
//
// Algebraic equivalence (within ~1e-13 IEEE-754 finite precision) to prior
// 3-pass form. Intentional bytewise break with v5.14.10 — PARITY contract
// at v5.14.10 → v5.14.11 boundary is TOLERANCE 1e-9 (documented in plan
// + PARITY_ISSUES.md). cfg=0 + cfg=1 within v5.14.11 share this finalize
// kernel → tolerance ~1e-13 (sum convergence) between modes.
//
// Input: `history[]` is a flat array of K records, each with N model
// predictions. For EnsembleModelZoo, this maps to ezoo->reward_ring's
// PredictionRecord.predictions[] array (populated via BuildHistoryFromRing
// or OnlineCycleStep helper).
//
// Cost: O(N² × K) — single-pass sum-of-squares accumulation. For N=8,
// K=64 → ~4096 ops; ~700-1000ns (vs prior 3-pass ~1µs; ~300ns saving
// per Caramel decision (C) 2026-05-11 latency reduction).
//
// Pattern documented in DESIGN_SPECS/sliding-window-online-statistics-pattern.md.
//======================================================================================================
template <unsigned F>
__attribute__((no_sanitize("address")))   // asan can't model the AVX-512 masked load/store on the 8-wide
                                           // buffers (correct-by-construction; verified by the v5.14.11.B.3
                                           // byte-determinism tests). Sister to Bandit_GetProbabilities. TECH_DEBT-158.
inline void RidgeBlender_BuildCorr(double corr_out[MAX_RIDGE_MODELS][MAX_RIDGE_MODELS],
                                     const float* predictions_history,  // K × N flat
                                     int n_history,
                                     int n_models) {
    if (n_history < 2 || n_models < 1) {
        // Not enough history — identity matrix (Cholesky-safe with diagonal-only Σ).
        for (int i = 0; i < MAX_RIDGE_MODELS; ++i) {
            for (int j = 0; j < MAX_RIDGE_MODELS; ++j) {
                corr_out[i][j] = (i == j) ? 1.0 : 0.0;
            }
        }
        return;
    }
    if (n_models > MAX_RIDGE_MODELS) n_models = MAX_RIDGE_MODELS;

    // === Single-pass sum-of-squares accumulation ===
    double sum_x[MAX_RIDGE_MODELS]                       = {0.0};
    double sum_xx[MAX_RIDGE_MODELS][MAX_RIDGE_MODELS]    = {{0.0}};

#if defined(__AVX512F__)
    // v5.14.11.B.3 — AVX-512 single-pass accumulation per
    // DESIGN_SPECS/avx512-byte-determinism-pattern.md + branchless-math-kernel-pattern.md.
    //   - Uniform mask across 8 lanes (n_models ≤ MAX_RIDGE_MODELS = 8); no inner if-guards
    //   - _mm512_fmadd_pd per-lane matches scalar vfmadd231sd (Rule 3)
    //   - _mm512_maskz_loadu zero-extends unused lanes; safe contribution
    //   - _mm512_mask_storeu_pd writes only masked lanes; rest of memory untouched
    const __mmask8 mask = (__mmask8)((1u << n_models) - 1u);
    for (int k = 0; k < n_history; ++k) {
        // Load record k's predictions as 8 floats (zero-extended via maskz), cvt to 8 doubles
        __m256 v_rec_f = _mm256_maskz_loadu_ps(mask, &predictions_history[k * n_models]);
        __m512d v_rec  = _mm512_cvtps_pd(v_rec_f);
        // sum_x += v_rec
        __m512d sx     = _mm512_maskz_loadu_pd(mask, sum_x);
        sx             = _mm512_add_pd(sx, v_rec);
        _mm512_mask_storeu_pd(sum_x, mask, sx);
        // sum_xx[i][0..n-1] += xi × v_rec for each row i
        for (int i = 0; i < n_models; ++i) {
            const __m512d v_xi = _mm512_set1_pd((double)predictions_history[k * n_models + i]);
            __m512d sxx_i      = _mm512_maskz_loadu_pd(mask, &sum_xx[i][0]);
            sxx_i              = _mm512_fmadd_pd(v_xi, v_rec, sxx_i);
            _mm512_mask_storeu_pd(&sum_xx[i][0], mask, sxx_i);
        }
    }
#else
    // Scalar reference (byte-determinism baseline per Rule 5)
    for (int k = 0; k < n_history; ++k) {
        for (int i = 0; i < n_models; ++i) {
            const double xi = (double)predictions_history[k * n_models + i];
            sum_x[i] += xi;
            for (int j = 0; j < n_models; ++j) {
                const double xj = (double)predictions_history[k * n_models + j];
                sum_xx[i][j] += xi * xj;
            }
        }
    }
#endif

    // === Shared finalize ===
    RidgeBlender_FinalizeCorrFromSums<F>(corr_out, sum_x, sum_xx,
                                           (uint64_t)n_history, n_models);
}


//======================================================================================================
// [UPDATE ONLINE — sliding-window incremental sum-of-squares]
//======================================================================================================
// v5.14.11.A — per-record incremental update for the sliding-window K=64
// correlation matrix. Two modes (selected by predictions_oldest_or_null):
//
//   1. Window NOT YET FULL (count < K): standard sum-of-squares add.
//      Caller passes predictions_oldest_or_null = nullptr.
//   2. Window FULL (count >= K): drop oldest + add new in one operation.
//      Caller passes predictions_oldest_or_null = pointer to oldest record.
//
// Bounded-input guard: predictions must be in [0,1] for the sum-of-squares
// numerical-stability argument to hold. BARRIER ensembles produce sigmoid
// outputs (naturally [0,1]); non-BARRIER misconfigurations are caught in
// debug builds via assert. Release builds compile out the assert.
//
// Per /parity-check re-audit 2026-05-11 LOW recommendation + Caramel
// decision (B): fold into .A. Compiled out in release; debug catches.
//
// Cost: O(N²) — outer-product update with N=8 → ~64 fmadd; ~30ns scalar
// (AVX-512 vectorization deferred to .B).
//
// Pattern documented in DESIGN_SPECS/sliding-window-online-statistics-pattern.md.
//======================================================================================================
template <unsigned F>
__attribute__((no_sanitize("address")))   // asan can't model the AVX-512 masked load/store (see RidgeBlender_BuildCorr / Bandit_GetProbabilities). TECH_DEBT-158.
inline void RidgeBlender_UpdateOnline(RidgeWeights<F>* rw,
                                        const float* predictions_new,
                                        const float* predictions_oldest_or_null,
                                        int n_models) {
    if (!rw || !predictions_new || n_models < 1) return;
    if (n_models > MAX_RIDGE_MODELS) n_models = MAX_RIDGE_MODELS;

    // === Defensive bounded-input guard (debug only; release compiles out) ===
    // Per /parity-check re-audit 2026-05-11 LOW recommendation. Catches
    // non-BARRIER model misconfigurations that would blow cancellation-error
    // bound in finalize.
    for (int i = 0; i < n_models; ++i) {
        assert(predictions_new[i] >= 0.0f && predictions_new[i] <= 1.0f);
        if (predictions_oldest_or_null) {
            assert(predictions_oldest_or_null[i] >= 0.0f &&
                   predictions_oldest_or_null[i] <= 1.0f);
        }
    }

    if (predictions_oldest_or_null == nullptr) {
        // === Window not yet full — standard add ===
#if defined(__AVX512F__)
        // v5.14.11.B.3 — AVX-512 outer-product update per
        // DESIGN_SPECS/avx512-byte-determinism-pattern.md + branchless-math-kernel-pattern.md.
        //   - _mm512_fmadd_pd per-lane ≡ scalar vfmadd231sd (Rule 3; gcc -O3 -ffp-contract=fast)
        //   - No _mm512_reduce_*; sum_x update is plain vector add (Rule 1)
        //   - Mask uniform across 8 lanes; no inner if-guards (branchless)
        const __mmask8 mask = (__mmask8)((1u << n_models) - 1u);
        __m256 v_new_f = _mm256_maskz_loadu_ps(mask, predictions_new);
        __m512d v_new  = _mm512_cvtps_pd(v_new_f);
        // sum_x[0..n-1] += v_new
        __m512d sx     = _mm512_maskz_loadu_pd(mask, rw->online_sum_x);
        sx             = _mm512_add_pd(sx, v_new);
        _mm512_mask_storeu_pd(rw->online_sum_x, mask, sx);
        // sum_xx[i][0..n-1] += xi × v_new per row i
        for (int i = 0; i < n_models; ++i) {
            const __m512d v_xi = _mm512_set1_pd((double)predictions_new[i]);
            __m512d sxx_i      = _mm512_maskz_loadu_pd(mask, &rw->online_sum_xx[i][0]);
            sxx_i              = _mm512_fmadd_pd(v_xi, v_new, sxx_i);
            _mm512_mask_storeu_pd(&rw->online_sum_xx[i][0], mask, sxx_i);
        }
#else
        // Scalar reference (byte-determinism baseline per Rule 5)
        for (int i = 0; i < n_models; ++i) {
            const double xi = (double)predictions_new[i];
            rw->online_sum_x[i] += xi;
            for (int j = 0; j < n_models; ++j) {
                const double xj = (double)predictions_new[j];
                rw->online_sum_xx[i][j] += xi * xj;
            }
        }
#endif
        rw->online_window_count++;
    } else {
        // === Window full — drop oldest + add new (fused replacement) ===
        // Net delta: sum_x += (new - old); sum_xx += (new ⊗ new - old ⊗ old).
        // Bounded window means accumulator magnitudes stay bounded →
        // no drift accumulation → no periodic reset needed.
#if defined(__AVX512F__)
        // v5.14.11.B.3 — AVX-512 drop-add. Per-lane shape:
        //   v_new_term = xn × v_new (vmulpd)
        //   v_old_term = xo × v_old (vmulpd)
        //   v_delta    = v_new_term - v_old_term (vsubpd)
        //   sxx_i     += v_delta (vaddpd)
        // Total 4 ops per lane; matches scalar 4-op below bytewise.
        // Scalar below uses EXPLICIT temporaries to prevent gcc -ffp-contract=fast
        // from fusing across the difference (would produce vfmadd231 + vfnmadd231
        // instead of 4 plain ops; would break cross-build byte-determinism).
        const __mmask8 mask = (__mmask8)((1u << n_models) - 1u);
        __m256 v_new_f = _mm256_maskz_loadu_ps(mask, predictions_new);
        __m256 v_old_f = _mm256_maskz_loadu_ps(mask, predictions_oldest_or_null);
        __m512d v_new  = _mm512_cvtps_pd(v_new_f);
        __m512d v_old  = _mm512_cvtps_pd(v_old_f);
        // sum_x += (v_new - v_old)
        __m512d v_diff = _mm512_sub_pd(v_new, v_old);
        __m512d sx     = _mm512_maskz_loadu_pd(mask, rw->online_sum_x);
        sx             = _mm512_add_pd(sx, v_diff);
        _mm512_mask_storeu_pd(rw->online_sum_x, mask, sx);
        // sum_xx[i] += (xn × v_new - xo × v_old) per row i — explicit 4-op
        for (int i = 0; i < n_models; ++i) {
            const __m512d v_xn = _mm512_set1_pd((double)predictions_new[i]);
            const __m512d v_xo = _mm512_set1_pd((double)predictions_oldest_or_null[i]);
            __m512d v_new_term = _mm512_mul_pd(v_xn, v_new);
            __m512d v_old_term = _mm512_mul_pd(v_xo, v_old);
            __m512d v_delta    = _mm512_sub_pd(v_new_term, v_old_term);
            __m512d sxx_i      = _mm512_maskz_loadu_pd(mask, &rw->online_sum_xx[i][0]);
            sxx_i              = _mm512_add_pd(sxx_i, v_delta);
            _mm512_mask_storeu_pd(&rw->online_sum_xx[i][0], mask, sxx_i);
        }
#else
        // Scalar reference — explicit 4-op form matches AVX-512 op sequence
        // (prevents -ffp-contract=fast from fusing `xn*xnj - xo*xoj` into 2 FMAs
        // instead of 4 plain ops; explicit temporaries break the fusion pattern).
        for (int i = 0; i < n_models; ++i) {
            const double xn = (double)predictions_new[i];
            const double xo = (double)predictions_oldest_or_null[i];
            rw->online_sum_x[i] += (xn - xo);
            for (int j = 0; j < n_models; ++j) {
                const double xnj = (double)predictions_new[j];
                const double xoj = (double)predictions_oldest_or_null[j];
                const double v_new_term = xn * xnj;
                const double v_old_term = xo * xoj;
                const double v_delta    = v_new_term - v_old_term;
                rw->online_sum_xx[i][j] += v_delta;
            }
        }
#endif
        // window_count stays at K (saturated)
    }
}


//======================================================================================================
// [BUILD HISTORY FROM RING — single-source ring iteration helper]
//======================================================================================================
// v5.14.11.A — C1 helper extraction per Caramel decision 2026-05-11.
// Single source of truth for ring-walk-backwards-from-head pattern;
// replaces TWO mirror loops at StrategyParameters.hpp:985-994 (buy) and
// :1184-1193 (exit). Class 18 mirror prevention per CLAUDE.md item 19.
//
// Templated on PredictionRecordT to avoid circular include with
// CoreModelZoo.hpp. Caller provides concrete EnsembleModelZoo<F>::PredictionRecord
// type at instantiation; helper accesses .predictions[i] field.
//
// Walks ring backwards from ring_head; writes most-recent-first into
// history_out as a flat [avail × n_models] float array.
//
// Capacity contract: caller provides history_out buffer of size
// RIDGE_HISTORY_DEPTH × MAX_RIDGE_MODELS = 512 floats. Helper writes
// only avail × n_models entries; caller's BuildCorr / FinalizeCorrFromSums
// reads only that range.
//
// Returns: avail = min(predict_call_count, RIDGE_HISTORY_DEPTH). 0 if
// no records available.
//======================================================================================================
template <unsigned F, typename PredictionRecordT>
inline int RidgeBlender_BuildHistoryFromRing(const PredictionRecordT* ring,
                                               int ring_head,
                                               int ring_size,
                                               uint64_t predict_call_count,
                                               int n_models,
                                               float* history_out) {
    if (!ring || !history_out || n_models < 1 || ring_size < 1) return 0;
    if (n_models > MAX_RIDGE_MODELS) n_models = MAX_RIDGE_MODELS;

    const int avail = (int)(predict_call_count < (uint64_t)RIDGE_HISTORY_DEPTH
                            ? predict_call_count : RIDGE_HISTORY_DEPTH);
    if (avail < 1) return 0;

    // Walk ring backwards from head — most-recent-first
    for (int k = 0; k < avail; ++k) {
        const int ring_idx = (ring_head - 1 - k + ring_size) % ring_size;
        for (int i = 0; i < n_models; ++i) {
            history_out[k * n_models + i] = ring[ring_idx].predictions[i];
        }
    }
    return avail;
}


//======================================================================================================
// [ONLINE CYCLE STEP — full per-cycle dispatch (full-recompute OR incremental)]
//======================================================================================================
// v5.14.11.A — C1 helper wrapper per Caramel decision 2026-05-11.
// Full per-Ridge-cycle dispatch; both Ridge call sites (buy + exit) reduce
// to a single helper call. Eliminates the parallel ring-walk + BuildCorr
// mirror at StrategyParameters.hpp:996 and :1195.
//
// Dispatches between:
//   - use_online_incremental=false: full-recompute via BuildHistoryFromRing
//     + refactored BuildCorr (calls shared FinalizeCorrFromSums internally).
//     Used when cfg.ridge_online_corr=0 (default; cfg=0 replay regime).
//   - use_online_incremental=true: sliding-window incremental via
//     UpdateOnline + shared FinalizeCorrFromSums. Used when
//     cfg.ridge_online_corr=1 (operator opt-in; cfg=1 replay regime).
//
// Returns:
//    0 — corr_matrix populated in rw->corr_matrix; caller proceeds to
//        RidgeBlender_Compute
//   -1 — not enough history (predict_call_count < 2); caller falls through
//        to bandit weights (Ridge override skipped this cycle)
//
// Cost (slow-path; per cycle, per core):
//   - cfg=0 (full): ~700-1000ns (refactored BuildCorr + finalize)
//   - cfg=1 (online): ~70-100ns (UpdateOnline + finalize; AVX-512 in .B
//     drops further to ~30ns)
//   - Per-record add/replace: O(N²) outer-product
//   - Finalize: O(N²) per-pair scale + sqrt
//
// Pattern: structural unification per Caramel "redesign for better
// functionality forward" 2026-05-11. Shared math kernel eliminates
// parallel correlation formulas; single source of truth for the
// corr-from-sums math.
//======================================================================================================
template <unsigned F, typename PredictionRecordT>
inline int RidgeBlender_OnlineCycleStep(RidgeWeights<F>* rw,
                                          const PredictionRecordT* ring,
                                          int ring_head,
                                          int ring_size,
                                          uint64_t predict_call_count,
                                          int n_models,
                                          bool use_online_incremental) {
    if (!rw || !ring || n_models < 1 || ring_size < 1) return -1;
    if (predict_call_count < 2) return -1;
    if (n_models > MAX_RIDGE_MODELS) n_models = MAX_RIDGE_MODELS;

    if (use_online_incremental) {
        // === cfg=1: sliding-window incremental ===
        // Latest record: ring[(ring_head - 1 + ring_size) % ring_size]
        const int latest_idx = (ring_head - 1 + ring_size) % ring_size;
        const float* preds_new = ring[latest_idx].predictions;

        // Oldest record (only if window full): K records back
        const float* preds_oldest = nullptr;
        if (rw->online_window_count >= (uint64_t)RIDGE_HISTORY_DEPTH) {
            const int oldest_idx = (ring_head - 1 - RIDGE_HISTORY_DEPTH + ring_size) % ring_size;
            preds_oldest = ring[oldest_idx].predictions;
        }

        RidgeBlender_UpdateOnline<F>(rw, preds_new, preds_oldest, n_models);

        RidgeBlender_FinalizeCorrFromSums<F>(rw->corr_matrix,
                                               rw->online_sum_x,
                                               rw->online_sum_xx,
                                               rw->online_window_count,
                                               n_models);

        return (rw->online_window_count >= 2) ? 0 : -1;
    } else {
        // === cfg=0: full-recompute (default) ===
        float history[RIDGE_HISTORY_DEPTH * MAX_RIDGE_MODELS];
        const int avail = RidgeBlender_BuildHistoryFromRing<F, PredictionRecordT>(
            ring, ring_head, ring_size, predict_call_count, n_models, history);
        if (avail < 2) return -1;
        RidgeBlender_BuildCorr<F>(rw->corr_matrix, history, avail, n_models);
        return 0;
    }
}

//======================================================================================================
// [INIT — zero-init for embedding in EnsembleModelZoo]
//======================================================================================================
template <unsigned F>
inline void RidgeWeights_Init(RidgeWeights<F>* rw) {
    if (!rw) return;
    std::memset(rw, 0, sizeof(*rw));
    // Identity correlation matrix = no info, orthogonal models.
    // Cholesky succeeds with diagonal-only Σ (regularized by ridge λ).
    for (int i = 0; i < MAX_RIDGE_MODELS; ++i) {
        rw->corr_matrix[i][i] = 1.0;
    }
}

#endif  // RIDGE_BLENDER_HPP
