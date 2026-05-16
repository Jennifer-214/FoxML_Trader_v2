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
// Operator selects via cfg.bandit_algorithm enum (5-state post-v5.15.5.F.4d):
//   0 = EXP3                     (default; bytewise-identical to pre-v5.14.10; Thompson frozen)
//   1 = THOMPSON                 (Bayesian posterior sampling; non-stationary-friendly; Exp3 frozen)
//   2 = EXP3_OP_THOMPSON_GHOST   (Exp3 drives decisions + Thompson shadow-learns; legacy "BOTH" semantic
//                                  preserved + Class 24 fix — Thompson posterior NOW updates from rewards)
//   3 = THOMPSON_OP_EXP3_GHOST   (NEW v5.15.5.F.4d — Thompson drives decisions + Exp3 shadow-learns)
//   4 = BLENDED                  (NEW v5.15.5.F.4d EXPERIMENTAL — weighted blend Exp3 + Thompson via
//                                  cfg.thompson_exp3_blend_alpha; weights = (1-α) × Exp3 + α × Thompson_softmax)
//
// State semantics + bit columns drive auto-derived dispatch tables (multi-state-dispatch-with-per-
// state-update-metadata.md Stage 3 ACTIVE first canonical at .F.4d):
//   - `exp3_up`     bit — does this state update Exp3 posterior on reward attribution?
//   - `thompson_up` bit — does this state update Thompson posterior on reward attribution?
//   - `drives`      token — which algo drives decisions (EXP3 / THOMPSON / BLENDED)
//
// DESIGN — UNIFORM 6-ARG DISPATCH CONTRACT (widened from 5-arg at .F.4d for BLENDED):
//   void BanditAlgoFn(BanditState* exp3, ThompsonBanditState* thompson,
//                     int n_arms, double blend_alpha,
//                     double* weights_out, int* chosen_arm_out);
//
//   - exp3            : BanditState* (used by EXP3 + ghost-modes + BLENDED; nullable for THOMPSON)
//   - thompson        : ThompsonBanditState* (used by THOMPSON + ghost-modes + BLENDED; nullable for EXP3)
//   - n_arms          : active arm count (must match both states' n_arms)
//   - blend_alpha     : Exp3↔Thompson blend ratio for BLENDED state ([0..1]; 0 = pure Exp3, 1 = pure Thompson).
//                       Ignored by EXP3 / THOMPSON / ghost-modes (caller may pass any value).
//                       Per § J of .F.4d merged plan body — fn-arg passing avoids Class 27 cfg-mirror cache
//                       on BanditState (alpha is per-core resolved value bound at dispatch time, not cached).
//   - weights_out     : OUT — caller-supplied buffer of BANDIT_MAX_ARMS doubles.
//                       Each compute fn writes EXACTLY n_arms entries; rest unmodified.
//   - chosen_arm_out  : OUT — single int (writeable; nullable to discard).
//
// Each compute fn writes BOTH outputs (uniform contract regardless of algo):
//   - EXP3:                       weights = Bandit_GetProbabilities; chosen = argmax(weights)
//   - THOMPSON:                   chosen = Thompson_Sample; weights = one-hot at chosen
//   - EXP3_OP_THOMPSON_GHOST:     weights = Exp3 probs (drives blending); chosen = Exp3's argmax
//                                  (chosen flipped from Thompson's pick at .F.4d to fix Class 24 sister
//                                  attribution bug; Thompson Sample side-effected for telemetry —
//                                  populates last_predicted_buy_thompson_arm via caller-side capture).
//   - THOMPSON_OP_EXP3_GHOST:     chosen = Thompson_Sample; weights = one-hot at chosen.
//                                  Exp3 GetProbabilities called for telemetry only (logged via calib).
//   - BLENDED:                    weights = (1-α) × Exp3_probs + α × Thompson_softmax(mu_post);
//                                  chosen = argmax(weights). Per § J — uses Thompson_GetSoftmaxWeights
//                                  helper for branchless softmax (no PRNG advance on weights derivation).
//
// WHY 7-COL TUPLE (was 4-col pre-.F.4d): registry feeds dispatch table + enum + ToString/FromString
// + bounds-checked wrapper + auto-derived per-state update masks (exp3_up + thompson_up reductions)
// + dead-state assert. Adding a 6th algorithm becomes 1 row with metadata tuple → both dispatch
// tables (buy + exit auto-mirror via FOREACH_BANDIT_SIDE) + masks + slow-path predicates extend.
//
// Pattern documented in DESIGN_SPECS/curve-registry-pattern.md + multi-state-dispatch-with-per-
// state-update-metadata.md. Slow-path-only; hot path UNTOUCHED.
//======================================================================================================
#ifndef BANDIT_ALGORITHM_REGISTRY_HPP
#define BANDIT_ALGORITHM_REGISTRY_HPP

#include <strings.h>     // strcasecmp
#include <stdlib.h>      // atoi
#include "BanditLearning.hpp"   // BanditState + Bandit_GetProbabilities
#include "ThompsonBandit.hpp"   // ThompsonBanditState + Thompson_Sample

//======================================================================================================
// [DISPATCH CONTRACT TYPE — 6-arg (widened from 5-arg at v5.15.5.F.4d for BLENDED state-4)]
//======================================================================================================
typedef void (*BanditAlgoFn)(BanditState* exp3,
                              ThompsonBanditState* thompson,
                              int n_arms,
                              double blend_alpha,        // v5.15.5.F.4d — for BLENDED; ignored by others
                              double* weights_out,
                              int* chosen_arm_out);

//======================================================================================================
// [FORWARD-DECLARE COMPUTE FNS]
//======================================================================================================
// Forward-declare so the dispatch table below can reference them.
// Definitions follow at end of file.
// 5 apply_fns post-v5.15.5.F.4d (was 3 pre-.F.4d; BanditAlgo_Both_Apply deleted as orphan after
// cfg=2 reassigns to Exp3_Drives_Thompson_Ghost_Apply per Option C wire-preserving expansion).
inline void BanditAlgo_Exp3_Apply(BanditState* exp3, ThompsonBanditState* thompson,
                                   int n_arms, double blend_alpha,
                                   double* weights_out, int* chosen_arm_out);
inline void BanditAlgo_Thompson_Apply(BanditState* exp3, ThompsonBanditState* thompson,
                                       int n_arms, double blend_alpha,
                                       double* weights_out, int* chosen_arm_out);
inline void BanditAlgo_Exp3_Drives_Thompson_Ghost_Apply(BanditState* exp3, ThompsonBanditState* thompson,
                                                          int n_arms, double blend_alpha,
                                                          double* weights_out, int* chosen_arm_out);
inline void BanditAlgo_Thompson_Drives_Exp3_Ghost_Apply(BanditState* exp3, ThompsonBanditState* thompson,
                                                          int n_arms, double blend_alpha,
                                                          double* weights_out, int* chosen_arm_out);
inline void BanditAlgo_Blended_Apply(BanditState* exp3, ThompsonBanditState* thompson,
                                       int n_arms, double blend_alpha,
                                       double* weights_out, int* chosen_arm_out);

//======================================================================================================
// [REGISTRY TUPLE]
//======================================================================================================
// Tuple (7-col post-v5.15.5.F.4d; was 4-col):
//   X(name, enum_value, compute_fn, exp3_up, thompson_up, drives, doc_string)
//   name         — UPPERCASE token; used for BANDIT_ALGO_<name> enum
//   enum_value   — numeric value (cfg-stored; NEVER renumber after release; OPTION C wire-byte
//                   preservation — cfg=0/1/2 wire bytes unchanged across .F.4d; cfg=3/4 NEW)
//   compute_fn   — free-function symbol matching the BanditAlgoFn dispatch contract (6-arg)
//   exp3_up      — 0/1; does this state update Exp3 posterior on reward attribution?
//                   Drives auto-derived BANDIT_EXP3_UPDATE_MASK reduction.
//   thompson_up  — 0/1; does this state update Thompson posterior on reward attribution?
//                   Drives auto-derived BANDIT_THOMPSON_UPDATE_MASK reduction + closes Class 24
//                   (Thompson_Update wired only when this bit is set; replaces fragile callsite check).
//   drives       — token (EXP3 / THOMPSON / BLENDED); which algo drives decisions.
//                   Auto-derives slow-path gate predicates (e.g., "should we compute Exp3 probs?").
//   doc_string   — operator-facing description for cfg.example + GUI tooltip
//
// IMPORTANT: enum values must be DENSE + CONTIGUOUS starting at 0 (fn-ptr table
// is indexed by enum value). Static_assert below enforces this.
//
// OPTION C wire-byte preservation (per § B.1 of .F.4d merged plan body): legacy stamps with
// cfg=0/1/2 load unchanged on .F.4d engine. cfg=2 BEHAVIOR shifts (Thompson now learns;
// Class 24 sister attribution fix flips chosen_arm to Exp3's pick) but WIRE bytes preserved.
// Behavior-change documented in cfg.example tooltip + .F.4d postmortem.
#define FOREACH_BANDIT_ALGORITHM(X)                                                                                                       \
    /*  name                       value  apply_fn                                          exp3_up  thompson_up  drives    doc */         \
    X(  EXP3,                      0,     BanditAlgo_Exp3_Apply,                            1,       0,           EXP3,     "Exp3-IX only; Thompson frozen (default; bytewise-identical pre-v5.14.10)") \
    X(  THOMPSON,                  1,     BanditAlgo_Thompson_Apply,                        0,       1,           THOMPSON, "Thompson only; Exp3 frozen (.F.4d Class 24 fix: Thompson posterior NOW updates from rewards)") \
    X(  EXP3_OP_THOMPSON_GHOST,    2,     BanditAlgo_Exp3_Drives_Thompson_Ghost_Apply,      1,       1,           EXP3,     "Exp3 drives + Thompson shadow-learns (legacy 'BOTH' semantic preserved + Thompson telemetry + Class 24 fix; cfg=2 wire bytes unchanged from v5.14.10)") \
    X(  THOMPSON_OP_EXP3_GHOST,    3,     BanditAlgo_Thompson_Drives_Exp3_Ghost_Apply,      1,       1,           THOMPSON, "NEW v5.15.5.F.4d — Thompson drives + Exp3 shadow-learns (explicit ghost mode for Thompson-led decisions with Exp3 telemetry)") \
    X(  BLENDED,                   4,     BanditAlgo_Blended_Apply,                         1,       1,           BLENDED,  "NEW v5.15.5.F.4d EXPERIMENTAL — Exp3+Thompson weighted blend via thompson_exp3_blend_alpha; weights = (1-α)×Exp3 + α×Thompson_softmax")

//======================================================================================================
// [AUTO-GENERATED ENUM + COUNT + DENSITY ASSERT]
//======================================================================================================
#define X_GEN_BANDIT_ALGO_ENUM(name, val, fn, exp3_up, thompson_up, drives, doc) BANDIT_ALGO_##name = val,
enum BanditAlgorithm {
    FOREACH_BANDIT_ALGORITHM(X_GEN_BANDIT_ALGO_ENUM)
};
#undef X_GEN_BANDIT_ALGO_ENUM

// Count via +1 reduction (deferred expansion; helper macro stays defined).
#define X_GEN_BANDIT_ALGO_COUNT_ONE(name, val, fn, exp3_up, thompson_up, drives, doc) +1
#define FOREACH_BANDIT_ALGORITHM_COUNT (0 FOREACH_BANDIT_ALGORITHM(X_GEN_BANDIT_ALGO_COUNT_ONE))

// Compile-time enforcement that enum values are dense 0..N-1. fn-ptr dispatch
// table indexes by enum value; sparse values would create a hole that calls
// the wrong fn (or sentinel-zero → crash on indirect call).
//
// 5 dense values post-v5.15.5.F.4d (Option C wire-byte preservation — cfg=0/1/2 unchanged from
// pre-.F.4d wire encoding; cfg=3/4 NEW).
static_assert(BANDIT_ALGO_EXP3 == 0,                       "FOREACH_BANDIT_ALGORITHM enum must be dense starting at 0");
static_assert(BANDIT_ALGO_THOMPSON == 1,                   "FOREACH_BANDIT_ALGORITHM enum must be dense (THOMPSON=1)");
static_assert(BANDIT_ALGO_EXP3_OP_THOMPSON_GHOST == 2,     "FOREACH_BANDIT_ALGORITHM enum must be dense (EXP3_OP_THOMPSON_GHOST=2) — OPTION C wire-byte preservation for legacy cfg=2 stamps (was BANDIT_ALGO_BOTH pre-.F.4d; semantic reassigned with Class 24 sister attribution fix per .F.4d § B.1)");
static_assert(BANDIT_ALGO_THOMPSON_OP_EXP3_GHOST == 3,     "FOREACH_BANDIT_ALGORITHM enum must be dense (THOMPSON_OP_EXP3_GHOST=3) — NEW at .F.4d");
static_assert(BANDIT_ALGO_BLENDED == 4,                    "FOREACH_BANDIT_ALGORITHM enum must be dense (BLENDED=4) — NEW at .F.4d");
static_assert(FOREACH_BANDIT_ALGORITHM_COUNT == 5,
    "FOREACH_BANDIT_ALGORITHM_COUNT == 5 post-v5.15.5.F.4d; if growing past 5, ensure dense values + "
    "update Order::flags_packed bandit_active_state bit-cap (must stay ≤ 8 for 3-bit slot per "
    "bandit_dispatch_table.hpp bit-width static_assert).");

// Dead-state assertion (per § B.5 of v5.15.5.F.4d merged plan body — every state must update at
// least one algorithm; pure-dispatch states with both update bits 0 would be ineffective).
#define _BANDIT_STATE_NONDEAD_ASSERT(name, val, fn, exp3_up, thompson_up, drives, doc) \
    static_assert((exp3_up) || (thompson_up), \
        "BANDIT_ALGORITHM state " #name " updates neither Exp3 nor Thompson — would be a dead state");
FOREACH_BANDIT_ALGORITHM(_BANDIT_STATE_NONDEAD_ASSERT)
#undef _BANDIT_STATE_NONDEAD_ASSERT

//======================================================================================================
// [AUTO-DERIVED PER-STATE UPDATE MASKS — v5.15.5.F.4d Step 2.B + § B.5]
//======================================================================================================
// Per § B.5 of v5.15.5.F.4d merged plan body. X-macro reduction over per-row `exp3_up` / `thompson_up`
// metadata bits produces uint8_t bitmaps indexed by enum value. Bit N is set IFF algorithm N updates
// the respective algorithm's posterior on reward attribution.
//
// Consumers:
//   - `SlowPathGateRegistry.hpp` THOMPSON_ACTIVE / BANDIT_SHADOW_LEARNING gate predicates (§ I) —
//     `(MASK >> bandit_algorithm) & 1u` for branchless metadata-driven gating
//   - `bandit_dispatch_table.hpp` `?:` chain auto-derives leaf reward fn per algorithm row from
//     (exp3_up, thompson_up) bits — these masks are an alternative consumer (slow-path predicate
//     shortcut vs full dispatch-table indirection)
//   - Test fixtures verifying gate predicate semantics
//
// Adding a 6th algorithm row → masks auto-extend to bit 5+. Zero per-site update.
//======================================================================================================
#define _BANDIT_EXP3_MASK_BIT(name, val, fn, exp3_up, thompson_up, drives, doc) \
    | (((uint8_t)(exp3_up)) << (val))
static constexpr uint8_t BANDIT_EXP3_UPDATE_MASK = (uint8_t)(0 FOREACH_BANDIT_ALGORITHM(_BANDIT_EXP3_MASK_BIT));
#undef _BANDIT_EXP3_MASK_BIT

#define _BANDIT_THOMPSON_MASK_BIT(name, val, fn, exp3_up, thompson_up, drives, doc) \
    | (((uint8_t)(thompson_up)) << (val))
static constexpr uint8_t BANDIT_THOMPSON_UPDATE_MASK = (uint8_t)(0 FOREACH_BANDIT_ALGORITHM(_BANDIT_THOMPSON_MASK_BIT));
#undef _BANDIT_THOMPSON_MASK_BIT

// Compile-time sanity per 5-state expansion at .F.4d (Option C wire preservation):
//   EXP3      (cfg=0): exp3_up=1, thompson_up=0 → EXP3_MASK bit 0 = 1; THOMPSON_MASK bit 0 = 0
//   THOMPSON  (cfg=1): exp3_up=0, thompson_up=1 → EXP3_MASK bit 1 = 0; THOMPSON_MASK bit 1 = 1
//   EXP3_OP_THOMPSON_GHOST (cfg=2): both → both masks bit 2 = 1
//   THOMPSON_OP_EXP3_GHOST (cfg=3): both → both masks bit 3 = 1
//   BLENDED   (cfg=4): both → both masks bit 4 = 1
// Net: EXP3_MASK = 0b11101 = 0x1D; THOMPSON_MASK = 0b11110 = 0x1E; both intersect = 0b11100 = 0x1C
static_assert(BANDIT_EXP3_UPDATE_MASK     == 0x1Du, "EXP3 update mask invariant: bits 0,2,3,4 set (EXP3 + 3 ghost/blended states)");
static_assert(BANDIT_THOMPSON_UPDATE_MASK == 0x1Eu, "THOMPSON update mask invariant: bits 1,2,3,4 set (THOMPSON + 3 ghost/blended states)");
static_assert((BANDIT_EXP3_UPDATE_MASK & BANDIT_THOMPSON_UPDATE_MASK) == 0x1Cu,
              "SHADOW_LEARNING (both algos update) invariant: bits 2,3,4 set (the 3 ghost/blended states)");

//======================================================================================================
// [DISPATCH TABLE — function pointers indexed by enum value]
//======================================================================================================
// Slow-path: 1 indirect call (~1-2ns); branch predictor handles cfg-stable
// algorithm choice (operator typically picks one mode and runs).
#define X_GEN_BANDIT_ALGO_FN_PTR(name, val, fn, exp3_up, thompson_up, drives, doc) fn,
static const BanditAlgoFn bandit_algorithm_fns[] = {
    FOREACH_BANDIT_ALGORITHM(X_GEN_BANDIT_ALGO_FN_PTR)
};
#undef X_GEN_BANDIT_ALGO_FN_PTR

//======================================================================================================
// [TOSTRING / FROMSTRING — cfg parser + GUI display]
//======================================================================================================
static inline const char* BanditAlgorithm_ToString(int algo) {
    switch (algo) {
        #define X_GEN_BANDIT_ALGO_TOSTRING(name, val, fn, exp3_up, thompson_up, drives, doc) case val: return #name;
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
    #define X_GEN_BANDIT_ALGO_FROMSTRING(name, val, fn, exp3_up, thompson_up, drives, doc) \
        if (strcasecmp(s, #name) == 0) return val;
    FOREACH_BANDIT_ALGORITHM(X_GEN_BANDIT_ALGO_FROMSTRING)
    #undef X_GEN_BANDIT_ALGO_FROMSTRING
    // v5.15.5.F.4d legacy alias — preserve operator cfg backward compat per Option C
    // wire-byte preservation. Pre-.F.4d cfg=2 was "BOTH"; canonical name is now
    // "EXP3_OP_THOMPSON_GHOST" but legacy string form keeps working so existing
    // engine.cfg files don't need editing. ToString returns canonical new name.
    if (strcasecmp(s, "BOTH") == 0) return BANDIT_ALGO_EXP3_OP_THOMPSON_GHOST;
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
                                          double blend_alpha,
                                          double* weights_out,
                                          int* chosen_arm_out) {
    if (algo < 0 || algo >= FOREACH_BANDIT_ALGORITHM_COUNT) {
        bandit_algorithm_fns[BANDIT_ALGO_EXP3](exp3, thompson, n_arms, blend_alpha, weights_out, chosen_arm_out);
        return;
    }
    bandit_algorithm_fns[algo](exp3, thompson, n_arms, blend_alpha, weights_out, chosen_arm_out);
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
// default). Thompson state + blend_alpha ignored.
inline void BanditAlgo_Exp3_Apply(BanditState* exp3, ThompsonBanditState* thompson,
                                   int n_arms, double blend_alpha,
                                   double* weights_out, int* chosen_arm_out) {
    (void)thompson;       // unused by EXP3
    (void)blend_alpha;    // unused by EXP3 (only BLENDED uses)
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
    // Argmax for chosen_arm_out — branchless via cmov (H20 / Class 28 prevention; tie-break favors lower index).
    if (chosen_arm_out) {
        int best = 0;
        double best_w = weights_out[0];
        for (int i = 1; i < n_arms; i++) {
            int win   = weights_out[i] > best_w;
            best      = win ? i : best;
            best_w    = win ? weights_out[i] : best_w;
        }
        *chosen_arm_out = best;
    }
}

// THOMPSON — sample from posterior; weights = one-hot at chosen_arm. Exp3 state
// + blend_alpha ignored. Posterior state advances (rng_state mutates) as part of sampling.
inline void BanditAlgo_Thompson_Apply(BanditState* exp3, ThompsonBanditState* thompson,
                                       int n_arms, double blend_alpha,
                                       double* weights_out, int* chosen_arm_out) {
    (void)exp3;           // unused by THOMPSON
    (void)blend_alpha;    // unused by THOMPSON (only BLENDED uses)
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

// EXP3_OP_THOMPSON_GHOST (cfg=2 post-.F.4d) — Exp3 drives decisions; Thompson shadow-learns.
// Preserves legacy "BOTH" semantic at cfg=2 (Exp3 weights drive ensemble blending; cfg=2
// wire bytes unchanged per Option C). Class 24 sister attribution fix at .F.4d: chosen_arm
// now reflects Exp3's argmax (NOT Thompson's pick) so reward attribution lands on the arm
// that ACTUALLY drove the decision. Thompson_Sample still called for telemetry side effect
// (RNG advances; caller captures last_predicted_buy_thompson_arm separately per § A.0 plan body).
// Per-arm reward observability (CoreModelZoo.hpp:881-882) — both bandits learn from same
// per-arm signal regardless of which one chose; Thompson posterior NOW updates from rewards
// (Class 24 fix — pre-.F.4d Thompson never updated despite mode being settable).
inline void BanditAlgo_Exp3_Drives_Thompson_Ghost_Apply(BanditState* exp3, ThompsonBanditState* thompson,
                                                          int n_arms, double blend_alpha,
                                                          double* weights_out, int* chosen_arm_out) {
    (void)blend_alpha;    // unused by ghost-modes (only BLENDED uses)
    if (!exp3 || !weights_out) {
        if (weights_out) {
            double inv_n = (n_arms > 0) ? (1.0 / (double)n_arms) : 0.0;
            for (int i = 0; i < n_arms; i++) weights_out[i] = inv_n;
        }
        if (chosen_arm_out) *chosen_arm_out = 0;
        return;
    }
    // Exp3 drives — its weights flow to ensemble blender
    Bandit_GetProbabilities(exp3, weights_out);
    // Thompson sample for telemetry side effect (RNG advances; populates last_predicted_buy_thompson_arm
    // via caller-side capture — see ML_BuildParameters scope per § A.0 of .F.4d merged plan body)
    if (thompson) {
        (void)Thompson_Sample(thompson);   // discard return; telemetry captured separately
    }
    // chosen_arm reflects DRIVING algorithm for correct reward attribution (Class 24 fix at .F.4d)
    if (chosen_arm_out) {
        int best = 0;
        double best_w = weights_out[0];
        for (int i = 1; i < n_arms; i++) {
            int win   = weights_out[i] > best_w;       // cmov branchless (Class 28 prevention)
            best      = win ? i : best;
            best_w    = win ? weights_out[i] : best_w;
        }
        *chosen_arm_out = best;
    }
}

// THOMPSON_OP_EXP3_GHOST (cfg=3; NEW at .F.4d) — Thompson drives decisions; Exp3 shadow-learns.
// Mirror of cfg=2 with sides flipped — Thompson's pick drives action; Exp3 GetProbabilities
// called for telemetry only (does NOT consume Thompson RNG; logged via calib for offline A/B).
// Both bandits update from per-arm reward signal downstream (per-arm observability invariant).
inline void BanditAlgo_Thompson_Drives_Exp3_Ghost_Apply(BanditState* exp3, ThompsonBanditState* thompson,
                                                          int n_arms, double blend_alpha,
                                                          double* weights_out, int* chosen_arm_out) {
    (void)blend_alpha;    // unused by ghost-modes (only BLENDED uses)
    if (!thompson || !weights_out) {
        if (weights_out) {
            double inv_n = (n_arms > 0) ? (1.0 / (double)n_arms) : 0.0;
            for (int i = 0; i < n_arms; i++) weights_out[i] = inv_n;
        }
        if (chosen_arm_out) *chosen_arm_out = 0;
        return;
    }
    // Compute Exp3 probabilities for telemetry side effect (does NOT consume Thompson RNG).
    // Stored into local buffer; caller can capture via separate channel for calib log per § N + § O.
    double exp3_probs_telemetry[BANDIT_MAX_ARMS];
    if (exp3) Bandit_GetProbabilities(exp3, exp3_probs_telemetry);
    (void)exp3_probs_telemetry;   // referenced by future calib emit per § N
    // Thompson drives — sample for arm selection
    int chosen = Thompson_Sample(thompson);
    // weights_out reflects Thompson's pick (one-hot at chosen)
    for (int i = 0; i < n_arms; i++) weights_out[i] = (i == chosen) ? 1.0 : 0.0;
    if (chosen_arm_out) *chosen_arm_out = chosen;
}

// BLENDED (cfg=4; NEW at .F.4d EXPERIMENTAL) — operator-weighted blend of Exp3 + Thompson.
// weights[i] = (1-α) × Exp3_probs[i] + α × Thompson_softmax(mu_post)[i]
// where α = blend_alpha (caller passes resolved cfg.thompson_exp3_blend_alpha per-core value;
// avoids Class 27 scalar mirror by binding at dispatch time NOT caching on BanditState).
// Uses Thompson_GetSoftmaxWeights helper (closed-form deterministic; no PRNG advance) — cheaper +
// more stable than Monte-Carlo Thompson_GetProbabilities for the blend input. Argmax over blended
// weights via cmov (H20 / Class 28 prevention). Per § J of .F.4d merged plan body.
inline void BanditAlgo_Blended_Apply(BanditState* exp3, ThompsonBanditState* thompson,
                                       int n_arms, double blend_alpha,
                                       double* weights_out, int* chosen_arm_out) {
    if (!exp3 || !thompson || !weights_out) {
        if (weights_out) {
            double inv_n = (n_arms > 0) ? (1.0 / (double)n_arms) : 0.0;
            for (int i = 0; i < n_arms; i++) weights_out[i] = inv_n;
        }
        if (chosen_arm_out) *chosen_arm_out = 0;
        return;
    }
    // Compute both components into local buffers.
    double exp3_probs[BANDIT_MAX_ARMS];
    double thompson_weights[BANDIT_MAX_ARMS];
    Bandit_GetProbabilities(exp3, exp3_probs);
    Thompson_GetSoftmaxWeights(thompson, thompson_weights);
    // Clamp alpha to [0, 1] defensively (cfg parse already enforces but defensive against
    // future callsite mis-resolution; cmov branchless).
    double a = (blend_alpha < 0.0) ? 0.0 : ((blend_alpha > 1.0) ? 1.0 : blend_alpha);
    double one_minus_a = 1.0 - a;
    // Blend into weights_out.
    for (int i = 0; i < n_arms; i++) {
        weights_out[i] = one_minus_a * exp3_probs[i] + a * thompson_weights[i];
    }
    // Renormalize (each input was a probability distribution; blend stays normalized in theory
    // but rounding error or truncated tails could leave it slightly off). Branchless cmov on
    // sum=0 (degenerate; shouldn't happen with normalized inputs but defensive).
    double sum = 0.0;
    for (int i = 0; i < n_arms; i++) sum += weights_out[i];
    double inv_sum = (sum > 0.0) ? (1.0 / sum) : 1.0;
    for (int i = 0; i < n_arms; i++) weights_out[i] *= inv_sum;
    // Argmax — branchless cmov (H20 / Class 28).
    if (chosen_arm_out) {
        int best = 0;
        double best_w = weights_out[0];
        for (int i = 1; i < n_arms; i++) {
            int win   = weights_out[i] > best_w;
            best      = win ? i : best;
            best_w    = win ? weights_out[i] : best_w;
        }
        *chosen_arm_out = best;
    }
}

#endif // BANDIT_ALGORITHM_REGISTRY_HPP
