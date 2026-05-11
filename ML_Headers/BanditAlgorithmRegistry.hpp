// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [BANDIT ALGORITHM REGISTRY — v5.14.10.A]
//======================================================================================================
// FOREACH_BANDIT_ALGORITHM(X) registry — adding a new bandit algorithm is 1 row:
//   1. Append X(NAME, val, fn, "doc") below
//   2. Implement BanditAlgo_<NAME>_Apply with the uniform 5-arg contract
//   Auto-generated: enum BanditAlgorithm, dispatch table bandit_algorithm_fns[],
//   FOREACH_BANDIT_ALGORITHM_COUNT, ToString/FromString, bounds-checked wrapper.
//
// Operator selects via cfg.bandit_algorithm enum (added in v5.14.10.B):
//   0 = EXP3     (default; bytewise-identical to pre-v5.14.10)
//   1 = THOMPSON (Bayesian posterior sampling; non-stationary-friendly)
//   2 = BOTH     (cfg=2 dual-mode for parallel-training A/B telemetry)
//
// DESIGN — UNIFORM 5-ARG DISPATCH CONTRACT:
//   void BanditAlgoFn(BanditState* exp3, ThompsonBanditState* thompson,
//                     int n_arms, double* weights_out, int* chosen_arm_out);
//
//   - exp3            : BanditState* (used by EXP3 + BOTH; nullable for THOMPSON)
//   - thompson        : ThompsonBanditState* (used by THOMPSON + BOTH; nullable for EXP3)
//   - n_arms          : active arm count (must match both states' n_arms)
//   - weights_out     : OUT — caller-supplied buffer of BANDIT_MAX_ARMS doubles
//                       Each compute fn writes EXACTLY n_arms entries; rest unmodified.
//   - chosen_arm_out  : OUT — single int (writeable; nullable to discard)
//
// Each compute fn writes BOTH outputs (uniform contract regardless of algo):
//   - EXP3:     weights_out = Bandit_GetProbabilities; chosen_arm_out = argmax(weights_out)
//   - THOMPSON: chosen_arm_out = Thompson_Sample; weights_out = one-hot at chosen_arm
//   - BOTH:     weights_out = EXP3 weights (drives action); chosen_arm_out = THOMPSON's choice
//                (logged via FOREACH_CALIB_LOG_COL in .D — Thompson is shadow-trained;
//                EXP3 makes the actual decision. Per-arm reward attribution updates BOTH
//                states downstream — see CoreModelZoo.hpp:881-882 reward attribution.)
//
// WHY 4-COL TUPLE (not 5-col Option D): registry feeds dispatch table + enum +
// ToString/FromString + bounds-checked wrapper. No GUI-panel auto-extension
// needed (ML Status panel renders ONE algo at a time; not all 3 simultaneously).
// Doc column suffices for engine.cfg.example auto-doc. Future expansion to
// 5-col (display_label + section) deferred until 4th algorithm forces redesign.
//
// Pattern documented in DESIGN_SPECS/curve-registry-pattern.md.
// Slow-path-only; hot path UNTOUCHED.
//======================================================================================================
#ifndef BANDIT_ALGORITHM_REGISTRY_HPP
#define BANDIT_ALGORITHM_REGISTRY_HPP

#include <strings.h>     // strcasecmp
#include <stdlib.h>      // atoi
#include "BanditLearning.hpp"   // BanditState + Bandit_GetProbabilities
#include "ThompsonBandit.hpp"   // ThompsonBanditState + Thompson_Sample

//======================================================================================================
// [DISPATCH CONTRACT TYPE]
//======================================================================================================
typedef void (*BanditAlgoFn)(BanditState* exp3,
                              ThompsonBanditState* thompson,
                              int n_arms,
                              double* weights_out,
                              int* chosen_arm_out);

//======================================================================================================
// [FORWARD-DECLARE COMPUTE FNS]
//======================================================================================================
// Forward-declare so the dispatch table below can reference them.
// Definitions follow at end of file.
inline void BanditAlgo_Exp3_Apply(BanditState* exp3, ThompsonBanditState* thompson,
                                   int n_arms, double* weights_out, int* chosen_arm_out);
inline void BanditAlgo_Thompson_Apply(BanditState* exp3, ThompsonBanditState* thompson,
                                       int n_arms, double* weights_out, int* chosen_arm_out);
inline void BanditAlgo_Both_Apply(BanditState* exp3, ThompsonBanditState* thompson,
                                   int n_arms, double* weights_out, int* chosen_arm_out);

//======================================================================================================
// [REGISTRY TUPLE]
//======================================================================================================
// Tuple: X(name, enum_value, compute_fn, doc_string)
//   name        — UPPERCASE token; used for BANDIT_ALGO_<name> enum
//   enum_value  — numeric value (cfg-stored; NEVER renumber after release)
//   compute_fn  — free-function symbol matching the BanditAlgoFn dispatch contract
//   doc_string  — operator-facing description for cfg.example + GUI tooltip
//
// IMPORTANT: enum values must be DENSE + CONTIGUOUS starting at 0 (fn-ptr table
// is indexed by enum value). Static_assert below enforces this.
#define FOREACH_BANDIT_ALGORITHM(X)                                                                       \
    X(EXP3,     0, BanditAlgo_Exp3_Apply,     "Exp3-IX (default; bytewise-identical to pre-v5.14.10)")    \
    X(THOMPSON, 1, BanditAlgo_Thompson_Apply, "Bayesian Thompson sampling; non-stationary-friendly")      \
    X(BOTH,     2, BanditAlgo_Both_Apply,     "Run both for parallel-training A/B telemetry (cfg=2)")

//======================================================================================================
// [AUTO-GENERATED ENUM + COUNT + DENSITY ASSERT]
//======================================================================================================
#define X_GEN_BANDIT_ALGO_ENUM(name, val, fn, doc) BANDIT_ALGO_##name = val,
enum BanditAlgorithm {
    FOREACH_BANDIT_ALGORITHM(X_GEN_BANDIT_ALGO_ENUM)
};
#undef X_GEN_BANDIT_ALGO_ENUM

// Count via +1 reduction (deferred expansion; helper macro stays defined).
#define X_GEN_BANDIT_ALGO_COUNT_ONE(name, val, fn, doc) +1
#define FOREACH_BANDIT_ALGORITHM_COUNT (0 FOREACH_BANDIT_ALGORITHM(X_GEN_BANDIT_ALGO_COUNT_ONE))

// Compile-time enforcement that enum values are dense 0..N-1. fn-ptr dispatch
// table indexes by enum value; sparse values would create a hole that calls
// the wrong fn (or sentinel-zero → crash on indirect call).
static_assert(BANDIT_ALGO_EXP3 == 0,     "FOREACH_BANDIT_ALGORITHM enum must be dense starting at 0");
static_assert(BANDIT_ALGO_THOMPSON == 1, "FOREACH_BANDIT_ALGORITHM enum must be dense (THOMPSON=1)");
static_assert(BANDIT_ALGO_BOTH == 2,     "FOREACH_BANDIT_ALGORITHM enum must be dense (BOTH=2)");
static_assert(FOREACH_BANDIT_ALGORITHM_COUNT == 3,
    "FOREACH_BANDIT_ALGORITHM_COUNT == 3 today; if growing past 3, ensure dense values + update bounds");

//======================================================================================================
// [DISPATCH TABLE — function pointers indexed by enum value]
//======================================================================================================
// Slow-path: 1 indirect call (~1-2ns); branch predictor handles cfg-stable
// algorithm choice (operator typically picks one mode and runs).
#define X_GEN_BANDIT_ALGO_FN_PTR(name, val, fn, doc) fn,
static const BanditAlgoFn bandit_algorithm_fns[] = {
    FOREACH_BANDIT_ALGORITHM(X_GEN_BANDIT_ALGO_FN_PTR)
};
#undef X_GEN_BANDIT_ALGO_FN_PTR

//======================================================================================================
// [TOSTRING / FROMSTRING — cfg parser + GUI display]
//======================================================================================================
static inline const char* BanditAlgorithm_ToString(int algo) {
    switch (algo) {
        #define X_GEN_BANDIT_ALGO_TOSTRING(name, val, fn, doc) case val: return #name;
        FOREACH_BANDIT_ALGORITHM(X_GEN_BANDIT_ALGO_TOSTRING)
        #undef X_GEN_BANDIT_ALGO_TOSTRING
        default: return "INVALID";
    }
}

// Accepts numeric form ("0", "1", "2") OR string form ("EXP3", "THOMPSON",
// "BOTH"); case-insensitive on string. Returns -1 on miss (caller treats as
// CRITICAL operator error — don't silently default).
static inline int BanditAlgorithm_FromString(const char* s) {
    if (!s || !*s) return -1;
    if (s[0] >= '0' && s[0] <= '9') {
        int v = atoi(s);
        if (v >= 0 && v < FOREACH_BANDIT_ALGORITHM_COUNT) return v;
        return -1;
    }
    #define X_GEN_BANDIT_ALGO_FROMSTRING(name, val, fn, doc) \
        if (strcasecmp(s, #name) == 0) return val;
    FOREACH_BANDIT_ALGORITHM(X_GEN_BANDIT_ALGO_FROMSTRING)
    #undef X_GEN_BANDIT_ALGO_FROMSTRING
    return -1;
}

//======================================================================================================
// [DISPATCH WRAPPER — bounds-checked]
//======================================================================================================
// Caller passes any int algorithm value; out-of-range degrades to EXP3 (preserves
// pre-v5.14.10 behavior). Out-of-range happens on corrupted cfg / stamp-bound
// parse failure; the safe fallback is the bytewise-identical default path.
static inline void BanditAlgorithm_Apply(int algo,
                                          BanditState* exp3,
                                          ThompsonBanditState* thompson,
                                          int n_arms,
                                          double* weights_out,
                                          int* chosen_arm_out) {
    if (algo < 0 || algo >= FOREACH_BANDIT_ALGORITHM_COUNT) {
        bandit_algorithm_fns[BANDIT_ALGO_EXP3](exp3, thompson, n_arms, weights_out, chosen_arm_out);
        return;
    }
    bandit_algorithm_fns[algo](exp3, thompson, n_arms, weights_out, chosen_arm_out);
}

//======================================================================================================
// [COMPUTE FNS — uniform 5-arg contract]
//======================================================================================================
// Each fn writes BOTH outputs (weights + chosen_arm) regardless of which algo
// "drives" the decision. Ensemble blending in ML_BuildParameters reads weights;
// telemetry / cfg=2 calib log reads chosen_arm.
//
// Defensive on null state pointers — degenerates to uniform weights + arm 0.
// In production this should NEVER happen (caller wires both states); defensive
// check protects against partial-init or test-harness misuse.
//======================================================================================================

// EXP3 — wraps existing Bandit_GetProbabilities + argmax. Bytewise-identical to
// pre-v5.14.10 behavior when called via this dispatch (cfg.bandit_algorithm=0
// default). Thompson state ignored.
inline void BanditAlgo_Exp3_Apply(BanditState* exp3, ThompsonBanditState* thompson,
                                   int n_arms, double* weights_out, int* chosen_arm_out) {
    (void)thompson;   // unused by EXP3
    if (!exp3 || !weights_out) {
        // Defensive uniform fallback
        if (weights_out) {
            double inv_n = (n_arms > 0) ? (1.0 / (double)n_arms) : 0.0;
            for (int i = 0; i < n_arms; i++) weights_out[i] = inv_n;
        }
        if (chosen_arm_out) *chosen_arm_out = 0;
        return;
    }
    Bandit_GetProbabilities(exp3, weights_out);
    // Argmax for chosen_arm_out (left-to-right tie-break favors lower index)
    if (chosen_arm_out) {
        int best = 0;
        double best_w = weights_out[0];
        for (int i = 1; i < n_arms; i++) {
            if (weights_out[i] > best_w) {
                best_w = weights_out[i];
                best = i;
            }
        }
        *chosen_arm_out = best;
    }
}

// THOMPSON — sample from posterior; weights = one-hot at chosen_arm. Exp3 state
// ignored. Posterior state advances (rng_state mutates) as part of sampling.
inline void BanditAlgo_Thompson_Apply(BanditState* exp3, ThompsonBanditState* thompson,
                                       int n_arms, double* weights_out, int* chosen_arm_out) {
    (void)exp3;   // unused by THOMPSON
    if (!thompson || !weights_out) {
        if (weights_out) {
            double inv_n = (n_arms > 0) ? (1.0 / (double)n_arms) : 0.0;
            for (int i = 0; i < n_arms; i++) weights_out[i] = inv_n;
        }
        if (chosen_arm_out) *chosen_arm_out = 0;
        return;
    }
    int chosen = Thompson_Sample(thompson);
    // One-hot weights at chosen arm
    for (int i = 0; i < n_arms; i++) weights_out[i] = (i == chosen) ? 1.0 : 0.0;
    if (chosen_arm_out) *chosen_arm_out = chosen;
}

// BOTH — cfg=2 dual-mode. Exp3 weights drive action (consumed by ensemble
// blender in ML_BuildParameters); Thompson chosen_arm logged for telemetry
// (calibration log column added in v5.14.10.D). Both states advance on
// reward attribution downstream (per-arm reward observability — see
// CoreModelZoo.hpp:881-882 — applies to both bandits independently).
inline void BanditAlgo_Both_Apply(BanditState* exp3, ThompsonBanditState* thompson,
                                   int n_arms, double* weights_out, int* chosen_arm_out) {
    if (!exp3 || !thompson || !weights_out) {
        if (weights_out) {
            double inv_n = (n_arms > 0) ? (1.0 / (double)n_arms) : 0.0;
            for (int i = 0; i < n_arms; i++) weights_out[i] = inv_n;
        }
        if (chosen_arm_out) *chosen_arm_out = 0;
        return;
    }
    // Exp3 drives action — write its weights for ensemble blending
    Bandit_GetProbabilities(exp3, weights_out);
    // Thompson logs choice for telemetry
    int thompson_chosen = Thompson_Sample(thompson);
    if (chosen_arm_out) *chosen_arm_out = thompson_chosen;
}

#endif // BANDIT_ALGORITHM_REGISTRY_HPP
