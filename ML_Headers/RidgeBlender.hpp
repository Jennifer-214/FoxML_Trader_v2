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

#include <cmath>
#include <cstring>
#include <cstdint>

#include "../FixedPoint/FixedPointN.hpp"

// File-scope (not tt::) — matches existing ML_Headers convention
// (BanditState, ModelHandle, EnsembleModelZoo all at file scope).

// MAX_RIDGE_MODELS bounds the matrix dimensions. Set ≥ ENSEMBLE_HORIZON_MAX
// so the same RidgeWeights struct can blend either across horizons (per
// ridge_across_horizons cfg) or within a horizon's role-arms.
static constexpr int MAX_RIDGE_MODELS = 8;

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
    int     last_compute_us;              // wall-clock cost (for /latency-track)
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
    // === DECOMPOSITION ===
    // L[i][i] = sqrt((Σ[i][i] + λ) - Σ_{k<i}(L[i][k]²))
    // L[i][j] = (Σ[i][j] - Σ_{k<j}(L[i][k]·L[j][k])) / L[j][j]   for j < i
    for (int i = 0; i < n; ++i) {
        // Off-diagonal: L[i][0..i-1]
        for (int j = 0; j < i; ++j) {
            double s = sigma[i][j];
            for (int k = 0; k < j; ++k) {
                s -= L_out[i][k] * L_out[j][k];
            }
            // L[j][j] guaranteed > 0 from prior diagonal step (or we
            // would have already returned -1).
            L_out[i][j] = s / L_out[j][j];
        }
        // Diagonal: L[i][i]
        double diag = sigma[i][i] + ridge_lambda;
        for (int k = 0; k < i; ++k) {
            diag -= L_out[i][k] * L_out[i][k];
        }
        if (diag <= 0.0) {
            // Singular or negative-definite (shouldn't happen with λ > 0
            // unless Σ has invalid entries). Fail fast.
            return -1;
        }
        L_out[i][i] = std::sqrt(diag);
        // Zero upper triangle for cleanliness (not strictly needed for
        // forward/back solve below, but helps debugging).
        for (int j = i + 1; j < n; ++j) {
            L_out[i][j] = 0.0;
        }
    }

    // === FORWARD SOLVE: L y = μ ===
    for (int i = 0; i < n; ++i) {
        double s = mu[i];
        for (int k = 0; k < i; ++k) {
            s -= L_out[i][k] * y_out[k];
        }
        y_out[i] = s / L_out[i][i];
    }

    // === BACK SOLVE: L^T w = y ===
    for (int i = n - 1; i >= 0; --i) {
        double s = y_out[i];
        for (int k = i + 1; k < n; ++k) {
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
// [BUILD CORRELATION MATRIX — from prediction history]
//======================================================================================================
// Per-pair Pearson correlation over standardized predictions:
//   corr[i][j] = mean((p_i - mean_i) × (p_j - mean_j)) / (std_i × std_j)
//
// Input: `history[]` is a flat array of K records, each with N model
// predictions. For EnsembleModelZoo, this maps to ezoo->reward_ring's
// PredictionRecord.predictions[] array (256 records, ENSEMBLE_HORIZON_MAX
// predictions each).
//
// Algorithm:
//   1. Compute per-model mean + std over K predictions.
//   2. For each pair (i, j): standardized cross-product accumulated.
//   3. Normalize by (std_i × std_j × K).
//   4. Guard std < 1e-9 (constant predictions) → identity row for that
//      model (correlated only with itself; off-diagonal = 0).
//
// Self-correlation (corr[i][i]) = 1.0 by construction. Σ + λI ensures
// strict positive-definiteness for Cholesky.
//
// Cost: O(N² × K). For N=8, K=64 → ~4096 ops; ~1µs.
//======================================================================================================
template <unsigned F>
inline void RidgeBlender_BuildCorr(double corr_out[MAX_RIDGE_MODELS][MAX_RIDGE_MODELS],
                                     const float* predictions_history,  // K × N flat
                                     int n_history,
                                     int n_models) {
    if (n_history < 2 || n_models < 1) {
        // Not enough history for meaningful correlation — identity matrix
        // (orthogonal models; Cholesky succeeds with diagonal-only Σ).
        for (int i = 0; i < MAX_RIDGE_MODELS; ++i) {
            for (int j = 0; j < MAX_RIDGE_MODELS; ++j) {
                corr_out[i][j] = (i == j) ? 1.0 : 0.0;
            }
        }
        return;
    }
    if (n_models > MAX_RIDGE_MODELS) n_models = MAX_RIDGE_MODELS;

    // === Per-model mean + std ===
    double mean[MAX_RIDGE_MODELS] = {0.0};
    double var[MAX_RIDGE_MODELS]  = {0.0};
    double std[MAX_RIDGE_MODELS]  = {0.0};

    for (int k = 0; k < n_history; ++k) {
        for (int i = 0; i < n_models; ++i) {
            mean[i] += (double)predictions_history[k * n_models + i];
        }
    }
    double inv_K = 1.0 / (double)n_history;
    for (int i = 0; i < n_models; ++i) {
        mean[i] *= inv_K;
    }
    for (int k = 0; k < n_history; ++k) {
        for (int i = 0; i < n_models; ++i) {
            double d = (double)predictions_history[k * n_models + i] - mean[i];
            var[i] += d * d;
        }
    }
    for (int i = 0; i < n_models; ++i) {
        var[i] *= inv_K;
        std[i] = std::sqrt(var[i]);
    }

    // === Pairwise correlation ===
    for (int i = 0; i < n_models; ++i) {
        // Diagonal = 1 by construction
        corr_out[i][i] = 1.0;
        if (std[i] < 1e-9) {
            // Constant predictions — identity row for this model
            for (int j = 0; j < n_models; ++j) {
                if (j != i) corr_out[i][j] = 0.0;
            }
            continue;
        }
        // Off-diagonal upper triangle
        for (int j = i + 1; j < n_models; ++j) {
            if (std[j] < 1e-9) {
                corr_out[i][j] = 0.0;
                corr_out[j][i] = 0.0;
                continue;
            }
            double s = 0.0;
            for (int k = 0; k < n_history; ++k) {
                double d_i = (double)predictions_history[k * n_models + i] - mean[i];
                double d_j = (double)predictions_history[k * n_models + j] - mean[j];
                s += d_i * d_j;
            }
            double c = (s * inv_K) / (std[i] * std[j]);
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
