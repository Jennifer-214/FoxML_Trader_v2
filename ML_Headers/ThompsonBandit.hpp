// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[ML_Headers/ThompsonBandit.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [SLOW_PATH] [DETERMINISM]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[Gaussian-posterior Thompson sampling (v5.14.10.A) — deterministic splitmix64 PRNG + Box-Muller; runs beside Exp3-IX for A/B in dual-mode]
// [CONTAINS]
//   - [STRUCT]_[ThompsonBanditState]
//   - [FUNCTION]_[Thompson_Sample]   (Init/Update/sink-wrappers/GetProbabilities/GetSoftmaxWeights share the file)
//======================================================================================================
// Per-arm Gaussian conjugate posterior (known observation variance). Selection
// via posterior sampling (one draw per arm; argmax wins). Update via closed-form
// Bayesian rule. Parallel to BanditState (Exp3-IX in BanditLearning.hpp); both
// can run simultaneously in cfg.bandit_algorithm=2 dual-mode for A/B telemetry.
//
// WHY Thompson alongside Exp3:
//   Crypto markets are NON-STATIONARY (regime shifts; alpha decay). Thompson's
//   randomized exploration adapts more naturally than Exp3's deterministic
//   exponential weights. Operator picks via cfg.bandit_algorithm enum:
//     0 = Exp3 (default; bytewise-identical to pre-v5.14.10)
//     1 = Thompson
//     2 = Both (Exp3 drives action; Thompson logs choice for offline A/B)
//   Per-arm reward observability (see NodeModelZoo.hpp's per-arm reward capture — every
//   ensemble arm's prediction is independently graded against actual price)
//   makes cfg=2 parallel-training MATHEMATICALLY VALID — both bandits learn
//   from the same per-arm signal stream regardless of which one's CHOICE
//   drove trading. Selection strategies diverge over time; offline analysis
//   answers "which algo would have driven better aggregate PnL."
//
// REPLAY-DETERMINISM CONTRACT (PARITY-014 fix):
//   - PRNG: own splitmix64 (algorithm fully specified in this file; deterministic
//     across all platforms — NOT std::mt19937_64 which has 312-word internal
//     state that's awkward to persist; NOT std::normal_distribution which is
//     libstdc++-implementation-defined and breaks cross-binary determinism).
//   - splitmix64 state = single uint64_t; trivially serializable to JSON.
//   - Box-Muller normal sampling: own implementation; pure IEEE-754 sqrt/log/cos/sin;
//     deterministic across compilers + libstdc++ versions.
//   - SHA-256-locked sample-trace snapshot test in tests/controller_test.cpp
//     enforces cross-binary cross-version reproducibility.
//
// SLOW-PATH ONLY. Hot path UNTOUCHED (hot-path-changelog discipline).
// Cost per Thompson_Sample: ~80ns (8 splitmix64 draws → 4 Box-Muller pairs
// → 8 normals → argmax). Slow-path budget 100µs → 0.08% utilization.
//
// FUTURE: persistence (Save/LoadJSON) lands in v5.14.10.C — single uint64_t
// rng_state field is the entire serializable RNG state.
//======================================================================================================
#ifndef THOMPSON_BANDIT_HPP
#define THOMPSON_BANDIT_HPP

#include <math.h>     // sqrt, log, cos, sin, M_PI, fmax
#include <string.h>   // memset
#include <stdint.h>   // uint32_t, uint64_t
#include "BanditLearning.hpp"  // BANDIT_MAX_ARMS (shared; Thompson and Exp3 use same arm cap)

//======================================================================================================
// DEFAULTS
//======================================================================================================
// Operator-tunable via cfg fields (added in v5.14.10.B). Defaults preserve
// "uninformative prior" semantics: posterior mean starts at 0, posterior
// precision starts at 1.0 (loose belief; first reward shifts strongly).
#define THOMPSON_MU_PRIOR_DEFAULT          0.0
#define THOMPSON_PRECISION_PRIOR_DEFAULT   1.0
#define THOMPSON_PRECISION_OBS_DEFAULT     1.0
#define THOMPSON_RNG_SEED_DEFAULT          42ULL    // operator-tunable; 0 = use this default

//======================================================================================================
// [STRUCT]_[ThompsonBanditState]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [SLOW_PATH]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[per-arm mu/precision posteriors + pull counts + the deterministic PRNG state]
//======================================================================
// [CODE]
//======================================================================
// STATE
//======================================================================================================
struct ThompsonBanditState {
    double   mu_post[BANDIT_MAX_ARMS];           // posterior mean per arm
    double   precision_post[BANDIT_MAX_ARMS];    // posterior precision per arm (= 1/variance)
    uint32_t total_pulls[BANDIT_MAX_ARMS];       // pull count per arm (matches BanditState.pulls width)
    int      n_arms;                              // active arms (≤ BANDIT_MAX_ARMS)
    int32_t  _padding = 0;                        // v5.14.11.B.2 — explicit zero-init padding
                                                  // (4B gap before double mu_prior; 8-byte align).
                                                  // Pattern: DESIGN_SPECS/struct-padding-determinism-pattern.md.
                                                  // Currently latent (Thompson_SaveJSON is field-by-field, not
                                                  // memcmp); fix is preventive for future byte-comparison usage.
    double   mu_prior;                            // operator-tunable; default 0.0
    double   precision_prior;                     // operator-tunable; default 1.0
    double   precision_obs;                       // observation precision (1/reward_variance); default 1.0
    uint64_t rng_state;                           // splitmix64 state (entire serializable RNG)
};

//======================================================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Per-arm Gaussian conjugate posterior. POD struct (memset-friendly).
// Layout: doubles aligned, then ints, then small fields. ~112 bytes per state
// (8 arms × 8B mu_post + 8 arms × 8B precision_post + 8 arms × 4B total_pulls
// + 4B n_arms + 3 × 8B hyperparams + 8B rng_state + ~8B padding).
//======================================================================
// [DERIVED]   (tool-refreshed — do NOT hand-edit; check_cache_layout --fix owns these)
//----------------------------------------------------------------------
// [SIZE]_[200B]
// [ALIGN]_[8]
// [CACHE_LINES]_[4]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[ThompsonBanditState]
//======================================================================

//======================================================================
// [FUNCTION]_[Thompson_Sample]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [SLOW_PATH] [DETERMINISM]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[splitmix64 -> Box-Muller -> one posterior draw per arm -> argmax; Init/Update (closed-form Bayes) + Pattern-5 sink wrappers + probability/softmax read-outs share the section]
//======================================================================
// [CODE]
//======================================================================
// PRNG: splitmix64
//======================================================================================================
// Reference: Sebastiano Vigna, "Further scramblings of Marsaglia's xorshift
// generators" (2014). Public domain. State = single uint64_t. Period = 2^64.
// Statistical quality sufficient for bandit posterior sampling (PassesTestU01
// SmallCrush + Crush; not Cryptographically secure — irrelevant for our use).
//
// Why this PRNG (not std::mt19937_64): single-uint64 state is trivially
// persistable; algorithm is fully specified (deterministic across all platforms)
// without depending on libstdc++-implementation-defined behavior.
//
// USAGE: caller owns the uint64 state; this fn advances + returns next 64 bits.
//======================================================================================================
static inline uint64_t Thompson_Splitmix64_Next(uint64_t* state) {
    uint64_t z = (*state += 0x9E3779B97F4A7C15ULL);    // golden-ratio increment
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

//======================================================================================================
// UNIFORM CONVERSION: raw uint64 → [0, 1)
//======================================================================================================
// 53-bit precision (max double mantissa). Matches std::generate_canonical
// behavior bit-for-bit. Avoids the "exactly 1.0" edge case (would break Box-
// Muller's log(0) handling).
static inline double Thompson_RawToUniform(uint64_t raw) {
    return (double)(raw >> 11) * (1.0 / (double)(1ULL << 53));
}

//======================================================================================================
// BOX-MULLER: 2 uniforms → 2 standard normal samples
//======================================================================================================
// Standard polar form. Two independent N(0,1) samples per pair of uniforms;
// caller is expected to use both (we don't cache z1 to keep state simple).
//
// Defensive on u1=0: would give log(0)=-inf → NaN. Clamp to tiny epsilon.
// Caller must ensure u1, u2 ∈ [0, 1).
static inline void Thompson_BoxMuller_Pair(double u1, double u2,
                                            double* z0_out, double* z1_out) {
    if (u1 < 1e-300) u1 = 1e-300;   // log(0) guard
    double r = sqrt(-2.0 * log(u1));
    double theta = 2.0 * M_PI * u2;
    *z0_out = r * cos(theta);
    *z1_out = r * sin(theta);
}

//======================================================================================================
// INIT
//======================================================================================================
// Zero-init state + apply prior to all arms + seed PRNG. After this, every
// arm has identical posterior (mu_post = mu_prior, precision_post = precision_prior).
// Update calls shift posteriors per-arm based on observed rewards.
//
// Defensive: n_arms clamped to [2, BANDIT_MAX_ARMS]. Hyperparam defaults
// applied if caller passes <= 0 for prior/observation precision.
static inline void Thompson_Init(ThompsonBanditState* tb, int n_arms,
                                  double mu_prior, double precision_prior,
                                  double precision_obs, uint64_t rng_seed) {
    memset(tb, 0, sizeof(*tb));
    if (n_arms < 2) n_arms = 2;
    if (n_arms > BANDIT_MAX_ARMS) n_arms = BANDIT_MAX_ARMS;
    tb->n_arms = n_arms;
    tb->mu_prior = mu_prior;   // 0.0 default; not range-checked (any real value valid)
    tb->precision_prior = (precision_prior > 0.0) ? precision_prior : THOMPSON_PRECISION_PRIOR_DEFAULT;
    tb->precision_obs = (precision_obs > 0.0) ? precision_obs : THOMPSON_PRECISION_OBS_DEFAULT;
    tb->rng_state = (rng_seed != 0ULL) ? rng_seed : THOMPSON_RNG_SEED_DEFAULT;
    for (int i = 0; i < n_arms; i++) {
        tb->mu_post[i] = tb->mu_prior;
        tb->precision_post[i] = tb->precision_prior;
    }
}

// Convenience: init with default hyperparameters + default seed (42).
// Typical for unit tests + boot-time init before cfg fields wire in (.B).
static inline void Thompson_InitDefault(ThompsonBanditState* tb, int n_arms) {
    Thompson_Init(tb, n_arms,
        THOMPSON_MU_PRIOR_DEFAULT,
        THOMPSON_PRECISION_PRIOR_DEFAULT,
        THOMPSON_PRECISION_OBS_DEFAULT,
        THOMPSON_RNG_SEED_DEFAULT);
}

//======================================================================================================
// UPDATE — Bayesian posterior shift on observed reward
//======================================================================================================
// Closed-form Gaussian conjugate update (known observation variance):
//   precision_new = precision_old + precision_obs
//   mu_new = (precision_old * mu_old + precision_obs * reward) / precision_new
//
// As updates accumulate, precision_post grows (variance shrinks) → posterior
// tightens around the true mean → samples cluster near mu_post (less exploration).
//
// Pure deterministic math; no PRNG used (only Sample uses PRNG).
// No-op for invalid arm (defensive).
static inline void Thompson_Update(ThompsonBanditState* tb, int arm, double reward) {
    // v5.15.5.F.4d Step 6 (§ K) — __builtin_expect-rare on bounds guard (H20 Exception #2; defensive;
    // production never hits this by construction since callers pass valid arms from dispatch tables).
    if (__builtin_expect(arm < 0 || arm >= tb->n_arms, 0)) return;
    double prec_old = tb->precision_post[arm];
    double mu_old = tb->mu_post[arm];
    double prec_new = prec_old + tb->precision_obs;
    tb->mu_post[arm] = (prec_old * mu_old + tb->precision_obs * reward) / prec_new;
    tb->precision_post[arm] = prec_new;
    tb->total_pulls[arm]++;
}

//======================================================================================================
// PATTERN 5 SINK-FN-POINTER WRAPPERS — Thompson_Update branchless dispatch (v5.15.5.F.4d)
//======================================================================================================
// Per DESIGN_SPECS/sink-fn-pointer-for-optional-side-effect-pattern.md (Pattern 5 of
// branchless-dispatch-discipline.md).
//
// PROBLEM these wrappers solve: pre-.F.4d reward-attribution dispatch sites had a per-call branch
//   if (cfg.bandit_algorithm == THOMPSON || cfg.bandit_algorithm == BOTH) {
//       Thompson_Update(&ezoo->buy_thompson_bandits[r], arm, reward);
//   }
// That branch is DATA-DEPENDENT (cfg-stable but predictor-warm-up cost on cfg-flip + violates H20
// + leaves callsite-by-callsite drift risk when adding new bandit modes — each new mode needs every
// dispatch site updated). It's the Class 24 sister structural risk + a Class 28 instance.
//
// SOLUTION via Pattern 5:
//   1. Define a NO-OP sink (compile-time-known empty body) + a REAL sink (delegates to Thompson_Update).
//   2. EnsembleModelZoo<F> carries a fn-pointer field `buy_thompson_update_fn` (sister: `exit_thompson_update_fn`
//      for the exit side per FOREACH_BANDIT_SIDE auto-mirror).
//   3. Default value at struct init: `&noop_thompson_update`. Boot wiring at
//      EnsembleModelZoo_InitBuyThompsonBandits (or _InitExitThompsonBandits for exit side) sets it to
//      `&real_thompson_update` when the Thompson subsystem actually initializes.
//   4. All reward-attribution dispatch sites call UNCONDITIONALLY through the fn pointer:
//          ezoo->buy_thompson_update_fn(&ezoo->buy_thompson_bandits[regime], arm, reward);
//      Branchless. ~1-2 ns indirect call. No per-call cfg read. No predictor-warmup cost on cfg-flip
//      (predictor sees same fn-pointer target every call until boot reconfigures; that's a 1-time event,
//      not a per-call event).
//
// Future bandit-mode additions: add 1 row to FOREACH_BANDIT_ALGORITHM with appropriate
// (exp3_up, thompson_up) metadata bits. The reward dispatch table in bandit_dispatch_table.hpp auto-
// selects the correct leaf-reward-fn per row. NO per-callsite update needed across consumer sites.
// This is the structural Class-28 closure for the dispatch family + Class-24 closure (Thompson posterior
// now reliably updates at every reward event when the subsystem is wired, regardless of which dispatch
// site fires the reward).
//
// Branchless cost analysis (per H20 + cycles/cache cost framework):
//   - noop call: predicted indirect call → ~1-2 ns; body is empty (compiler may inline-elide entirely)
//   - real call: predicted indirect call → ~1-2 ns + Thompson_Update body (~10 ns; closed-form math)
//   Total slow-path cost: <15 ns per call. Slow-path budget 100µs → 0.015% utilization. Negligible.
//======================================================================================================
inline void noop_thompson_update(ThompsonBanditState* /*tb*/, int /*arm*/, double /*reward*/) {
    // Compile-time-known empty body. ezoo->buy_thompson_update_fn resolves here from struct construction
    // until EnsembleModelZoo_InitBuyThompsonBandits boot-wires to real_thompson_update. Acts as the
    // "Thompson subsystem not active on this ezoo" branch — but BRANCHLESS at the consumer site
    // (uniform indirect call; no per-call cfg read).
}

inline void real_thompson_update(ThompsonBanditState* tb, int arm, double reward) {
    Thompson_Update(tb, arm, reward);
}

// Pattern 5 sink fn-pointer typedef — both `buy_thompson_update_fn` + `exit_thompson_update_fn` fields
// on EnsembleModelZoo<F> share this signature. FOREACH_BANDIT_SIDE auto-mirror generates the two
// field declarations + boot-wiring per side from this single typedef.
using ThompsonUpdateFn = void (*)(ThompsonBanditState*, int /*arm*/, double /*reward*/);

//======================================================================================================
// SAMPLE — draw one posterior sample per arm; return argmax
//======================================================================================================
// For each arm: sample = mu_post + z / sqrt(precision_post), where z ~ N(0,1).
// Returns the arm with the largest sample.
//
// PRNG cost: 2 splitmix64 calls per Box-Muller pair → 1 pair per 2 arms →
// ~ceil(n_arms / 2) pairs total → ~n_arms splitmix calls. For 8 arms: 8 calls.
// Total per Sample: ~80ns (splitmix is ~5ns; box-muller log/sqrt/cos/sin is ~10ns).
//
// Mutates tb->rng_state (advances the PRNG); not thread-safe.
static inline int Thompson_Sample(ThompsonBanditState* tb) {
    if (tb->n_arms < 2) return 0;
    double samples[BANDIT_MAX_ARMS];
    int i = 0;
    while (i < tb->n_arms) {
        // Pair of uniforms → pair of normals (Box-Muller)
        uint64_t r0 = Thompson_Splitmix64_Next(&tb->rng_state);
        uint64_t r1 = Thompson_Splitmix64_Next(&tb->rng_state);
        double u0 = Thompson_RawToUniform(r0);
        double u1 = Thompson_RawToUniform(r1);
        double z0, z1;
        Thompson_BoxMuller_Pair(u0, u1, &z0, &z1);
        // sigma = 1/sqrt(precision); sample = mu + z*sigma. Floor precision to
        // avoid div-by-zero on degenerate state.
        double sigma0 = 1.0 / sqrt(fmax(tb->precision_post[i], 1e-12));
        samples[i] = tb->mu_post[i] + z0 * sigma0;
        i++;
        if (i < tb->n_arms) {
            double sigma1 = 1.0 / sqrt(fmax(tb->precision_post[i], 1e-12));
            samples[i] = tb->mu_post[i] + z1 * sigma1;
            i++;
        }
    }
    // v5.15.5.F.4d Step 6 (§ K + § L) — Class 28 cmov branchless argmax (H20).
    // Argmax with left-to-right tie-break favors lower index (deterministic; replay-safe).
    int best = 0;
    double best_val = samples[0];
    for (int j = 1; j < tb->n_arms; j++) {
        int win  = samples[j] > best_val;
        best     = win ? j          : best;
        best_val = win ? samples[j] : best_val;
    }
    return best;
}

//======================================================================================================
// GET PROBABILITIES — Monte Carlo posterior probability estimation
//======================================================================================================
// Estimates P(arm i has highest posterior mean) via N=128 internal draws.
// Used for TELEMETRY only (calibration log, ML Status panel display); production
// arm selection uses Thompson_Sample directly to preserve true posterior dynamics.
//
// Uses a CLONE of tb (does NOT mutate caller's rng_state). Cost ~10µs at 128
// draws × 8 arms; called rarely (per-fill or per-frame, not per-cycle).
//
// probs_out must be sized BANDIT_MAX_ARMS doubles. Unused entries (i ≥ n_arms)
// are zeroed.
static inline void Thompson_GetProbabilities(const ThompsonBanditState* tb, double* probs_out) {
    static const int N_DRAWS = 128;
    if (!probs_out) return;
    for (int i = 0; i < BANDIT_MAX_ARMS; i++) probs_out[i] = 0.0;
    if (!tb || tb->n_arms < 2) {
        if (tb && tb->n_arms == 1) probs_out[0] = 1.0;
        return;
    }
    ThompsonBanditState clone = *tb;   // local copy; mutates clone.rng_state only
    int counts[BANDIT_MAX_ARMS] = {0};
    for (int d = 0; d < N_DRAWS; d++) {
        int chosen = Thompson_Sample(&clone);
        counts[chosen]++;
    }
    double inv_n = 1.0 / (double)N_DRAWS;
    for (int i = 0; i < tb->n_arms; i++) {
        probs_out[i] = (double)counts[i] * inv_n;
    }
}

//======================================================================================================
// GET SOFTMAX WEIGHTS — posterior-derived weight vector for BLENDED dispatch
//======================================================================================================
// Maps each arm's posterior mean mu_post[i] to a normalized softmax weight:
//   weights[i] = exp(mu_post[i] - max(mu_post)) / Σ_j exp(mu_post[j] - max(mu_post))
//
// Max-subtract is for numerical stability (prevents exp() overflow on large mu_post
// and underflow on negative-large values; the shifted argument always has its largest
// element equal to 0 so exp() returns ≥1 for the max-arm and (0,1] for others).
//
// Used by BanditAlgo_Blended_Apply (state 4; v5.15.5.F.4d) to derive the Thompson
// contribution to the blended Exp3+Thompson weight vector. Distinct from
// Thompson_GetProbabilities (which is Monte-Carlo P(arm-i-is-argmax) for telemetry).
// Softmax-of-means is the closed-form mapping; Monte-Carlo would be over-precision-
// adjusted samples. BLENDED uses softmax-of-means as the cheaper deterministic input
// to the blend.
//
// Branchless inner loops (H20 — determinism-preferred dispatch on SP/HP). Max-find
// loop uses cmov pattern; normalize loop is straight-line; defensive cmov on sum=0
// avoids div-by-zero on degenerate state (shouldn't happen post-max-subtract since
// exp(0)=1 guarantees sum ≥ 1, but defensive for safety).
//
// Cost: ~1µs at 8 arms (one exp() per arm dominates; ~125ns per exp on x86_64 with
// glibc libm). Slow-path only; called rarely (once per BLENDED dispatch — every
// bandit-update cycle on the path that selects via blended weighting).
//
// Replay-determinism: pure deterministic math (no PRNG); identical mu_post[] inputs
// produce bytewise-identical weights_out across runs and binaries (subject to libm
// exp() implementation determinism — glibc exp is deterministic across versions
// per IEEE-754 correctly-rounded semantics).
//
// weights_out must be sized BANDIT_MAX_ARMS doubles. Unused entries (i ≥ n_arms)
// are zeroed. Defensive on nullptr / degenerate n_arms < 2.
static inline void Thompson_GetSoftmaxWeights(const ThompsonBanditState* tb, double* weights_out) {
    if (!weights_out) return;
    for (int i = 0; i < BANDIT_MAX_ARMS; i++) weights_out[i] = 0.0;
    if (!tb || tb->n_arms < 2) {
        if (tb && tb->n_arms == 1) weights_out[0] = 1.0;
        return;
    }
    // Branchless max-find via cmov (H20). Compiler lowers ternary to cmov on x86_64.
    double max_mu = tb->mu_post[0];
    for (int i = 1; i < tb->n_arms; i++) {
        int gt = tb->mu_post[i] > max_mu;
        max_mu = gt ? tb->mu_post[i] : max_mu;
    }
    // Compute exp(mu - max) per arm + accumulate sum.
    double sum = 0.0;
    for (int i = 0; i < tb->n_arms; i++) {
        double e = exp(tb->mu_post[i] - max_mu);
        weights_out[i] = e;
        sum += e;
    }
    // Normalize. Defensive cmov on sum=0 (degenerate; uniform fallback).
    double inv_sum = (sum > 0.0) ? (1.0 / sum) : (1.0 / (double)tb->n_arms);
    for (int i = 0; i < tb->n_arms; i++) {
        weights_out[i] *= inv_sum;
    }
}

//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[Thompson_Sample]
//======================================================================
#endif // THOMPSON_BANDIT_HPP
