// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[ML_Headers/BanditLearning.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [SLOW_PATH] [DETERMINISM]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[Exp3-IX multi-armed bandit (FoxML bandit.py port) — importance-weighted online arm selection + static-blend ramp + JSON state persistence; AVX-512 kernel with bytewise-identical scalar fallback]
// [CONTAINS]
//   - [STRUCT]_[BanditDisplayMeta]        (+ InitDefault / SetArmName helpers)
//   - [STRUCT]_[BanditState]
//   - [FUNCTION]_[Bandit_Init]            (+ InitDefault)
//   - [FUNCTION]_[Bandit_GetProbabilities]
//   - [FUNCTION]_[Bandit_Select]
//   - [FUNCTION]_[Bandit_Update]
//   - [FUNCTION]_[Bandit_GetWeights]
//   - [FUNCTION]_[Bandit_BlendWeights]    (+ EffectiveBlend)
//   - [FUNCTION]_[Bandit_SaveJSON]
//   - [FUNCTION]_[Bandit_LoadJSON]        (+ the tt::json_io primitives)
//======================================================================================================
// port of FoxML/private LIVE_TRADING/learning/bandit.py + weight_optimizer.py.
// Exp3-IX multi-armed bandit for online strategy/model selection with
// importance-weighted updates and adaptive learning rate.
//
// arms are generic: strategies now (SimpleDip, MR, Momentum, EMACross, ML),
// config variants later. string-named for display, int-indexed for speed.
//
// algorithm (from FoxML bandit.py):
//   selection: p_i = (1 - gamma) * (w_i / sum_w) + gamma / K
//   update:    w_i *= exp(eta * reward / p_i)    (importance-weighted)
//   eta:       min(eta_max, sqrt(ln(K) / (K * T)))  (adaptive)
//
// blending (from FoxML weight_optimizer.py):
//   final = (1 - effective_blend) * static + effective_blend * bandit
//   ramp-up: 0% for first min_samples trades, linear to blend_ratio over ramp_up
//
// FoxML constants:
//   gamma = 0.05     (exploration rate)
//   eta_max = 0.07   (max learning rate)
//   blend_ratio = 0.30 (30% bandit influence)
//   min_samples = 100 (trades before bandit activates)
//   ramp_up = 100    (trades to reach full blend)
//
// SHARED: used by both backtest suite (offline eval) and live engine (strategy selection).
//
// FUTURE HOOKS:
//   config variants: arm names "config_aggressive", "config_conservative"
//   persistence: save/restore across sessions
//     → see ~/FoxML/private/LIVE_TRADING/learning/persistence.py
//   reward tracking: per-trade P&L attribution
//     → see ~/FoxML/private/LIVE_TRADING/learning/reward_tracker.py
//======================================================================================================
#ifndef BANDIT_LEARNING_HPP
#define BANDIT_LEARNING_HPP

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <cstdint>      // uint32_t / uint64_t (parse_uint32_array etc.); explicit per IWYU discipline
#include "../CoreFrameworks/ParseFast.hpp"  // v5.11.4.C — std::from_chars wrapper for locale-immune parsing
#include <unistd.h>     // unlink, write
#include <locale.h>     // v5.14.10.C — uselocale/newlocale for LC_NUMERIC=C pinning at JSON emit (TECH_DEBT-027 close)
#if defined(__AVX512F__)
#include <immintrin.h>  // v5.11.7 — AVX-512 vectorization of Bandit_GetProbabilities
#endif

// default parameters (from FoxML bandit.py + weight_optimizer.py)
#define BANDIT_GAMMA_DEFAULT       0.05    // exploration rate
#define BANDIT_ETA_MAX_DEFAULT     0.07    // max learning rate
#define BANDIT_BLEND_RATIO_DEFAULT 0.30    // 30% bandit influence
#define BANDIT_MIN_SAMPLES_DEFAULT 100     // trades before bandit activates
#define BANDIT_RAMP_UP_DEFAULT     100     // trades to reach full blend
#define BANDIT_MAX_ARMS            8       // max arms supported

// s5 bandit ship (2026-08-23) — numerical-stability constants for Bandit_Update.
// Both close MEASURED live defects in the persisted Exp3 state, not hypotheticals.
//
// BANDIT_UPDATE_EXPO_LIMIT — the max |eta * r_hat| fed to exp(). WHY 36.7:
// ln(1e16) is exactly the dynamic range between the weight floor (1e-10) and the
// explosion-renorm trigger (1e6), so a single update can traverse the whole live
// range and no further. Above it two things break:
//   * >709.78 overflows exp() to +inf, and the explosion guard's own inf/inf
//     divide then MINTS a NaN that no comparison guard can clear (BT-1/BT-2 — a
//     `-nan` reached models/classification/twins/bandit_state.json and survived
//     save/load forever).
//   * even far below overflow, one lucky trade on a floored arm can multiply by
//     e^154 ~ 1e66, forcing the renorm to divide EVERY arm by ~1e60 and crushing
//     the previous leader to the floor — the regime's learned ordering erased
//     with no NaN anywhere (s5-F7 single-step renorm-crush).
// Cost of the clamp: healthy updates whose exponent already exceeded 36.7 now
// move less per step. Deliberate — those steps were destroying the ordering they
// were supposed to refine. Replay/backtest results shift accordingly; free under
// project_no_live_models_dev_test_only.
#define BANDIT_UPDATE_EXPO_LIMIT   36.7    // ln(1e16) = floor(1e-10) -> renorm(1e6) range

// BANDIT_WEIGHT_LOW_WATER — when the LARGEST weight sinks below this, scale the
// whole vector up so max == 1.0. Ratio-preserving, so it changes no arm's
// relative standing; it only keeps the vector off the 1e-10 floor, where
// ordering is destroyed by clamping rather than by evidence (BT-11: HFT_4
// regime 4 sat at [1e-10,1e-10,1e-10] after 456 real steps). Sister to the
// >1e6 explosion renorm below it — same mechanism, opposite direction.
#define BANDIT_WEIGHT_LOW_WATER    1e-3

//======================================================================
// [STRUCT]_[BanditDisplayMeta]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[human-readable arm names, display-only — extracted OUT of BanditState (v5.15.5.A.3) so hot-side reads never pull 256B of labels; InitDefault + SetArmName ride in this section]
// [REFERENCE]_[DESIGN_SPEC]_[cache-layout-discipline-for-hot-side-structs]
//======================================================================
// [CODE]
//======================================================================
struct BanditDisplayMeta {
    char arm_names[BANDIT_MAX_ARMS][32];   // human-readable arm names
};

// Initialize display meta with default "arm_0", "arm_1", ... labels.
// Caller invokes once after pairing; or overrides individual arms via
// BanditDisplayMeta_SetArmName.
static inline void BanditDisplayMeta_InitDefault(BanditDisplayMeta *m, int n_arms) {
    if (!m) return;
    for (int i = 0; i < BANDIT_MAX_ARMS; ++i) {
        if (i < n_arms) {
            snprintf(m->arm_names[i], sizeof(m->arm_names[i]), "arm_%d", i);
        } else {
            m->arm_names[i][0] = '\0';
        }
    }
}

// Set a custom human-readable name for an arm (display only).
static inline void BanditDisplayMeta_SetArmName(BanditDisplayMeta *m, int arm, const char *name) {
    if (!m || arm < 0 || arm >= BANDIT_MAX_ARMS || !name) return;
    snprintf(m->arm_names[arm], sizeof(m->arm_names[arm]), "%s", name);
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// v5.15.5.A.3 — extracted from BanditState per DESIGN_SPECS/
// cache-layout-discipline-for-hot-side-structs.md Rule 1 (extract display-only
// fields out of hot-side structs). arm_names is 256B (8 × 32) of display-only
// data that was sitting inside BanditState, getting pulled into L1 every time
// slow-path read weights/cum_reward/pulls. With NUM_REGIMES=5 BanditState
// instances on ezoo, that was 1280B of arm_names noise per ezoo.
//
// New layout: BanditState has just the hot/slow-path math state.
// BanditDisplayMeta lives separately at the OWNER level (PortfolioController
// has 1 next to its bandit; EnsembleModelZoo has [NUM_REGIMES] in COLD cluster
// next to bandits[]). Functions that NEED arm names take a BanditDisplayMeta*
// parameter (nullable; falls back to "arm_N" default when null).
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
//----------------------------------------------------------------------
// [SIZE]_[256B]
// [ALIGN]_[1]
// [CACHE_LINES]_[4]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[BanditDisplayMeta]
//======================================================================

//======================================================================
// [STRUCT]_[BanditState]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [SLOW_PATH]]
// [SCOPE]_[NODE]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[pure Exp3-IX math state — weights / cum_reward / pulls per arm + the exploration and blend knobs; display labels live in BanditDisplayMeta]
//======================================================================
// [CODE]
//======================================================================
struct BanditState {
    double weights[BANDIT_MAX_ARMS];       // unnormalized weights (exp3-ix)
    double cum_reward[BANDIT_MAX_ARMS];    // cumulative P&L per arm (bps)
    int pulls[BANDIT_MAX_ARMS];            // pull count per arm
    int n_arms;
    int total_steps;
    double gamma;           // exploration rate
    double eta_max;         // max learning rate
    double blend_ratio;     // bandit influence fraction
    int min_samples;        // minimum trades before bandit activates
    int ramp_up_samples;    // trades to ramp from 0 to blend_ratio
};
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// v5.15.5.A.3 — arm_names extracted to BanditDisplayMeta (above). Pure
// math state; ~256B smaller than pre-v5.15.5.
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
//----------------------------------------------------------------------
// [SIZE]_[200B]
// [ALIGN]_[8]
// [CACHE_LINES]_[4]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[BanditState]
//======================================================================

// v5.15.5.A.3 — shrinkage assertion: BanditState lost the 256B arm_names
// (BANDIT_MAX_ARMS=8 × 32 = 256 bytes). New size should be <= OLD_SIZE - 256.
// Pre-extraction sizeof was 456 bytes (weights[64] + cum_reward[64] + pulls[32]
// + arm_names[256] + scalars[44]); post should be ~200 bytes.
// [ASSERT]_[LAYOUT_LOCK]_[sizeof(BanditState) <= 256 — post-extraction shrinkage lock]
static_assert(sizeof(BanditState) <= 256,
              "v5.15.5.A.3 BanditState shrinkage check: extracted arm_names "
              "should bring sizeof down to ~200 bytes; if assertion trips, "
              "either struct gained fields or extraction was partial");

//======================================================================
// [FUNCTION]_[Bandit_Init]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BOOT_TIME] [ML_INFERENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[uniform-weight init with clamped knobs (FoxML defaults on zero/negative args); InitDefault rides in this section]
//======================================================================
// [CODE]
//======================================================================
static inline void Bandit_Init(BanditState *b, int n_arms,
                                double gamma, double eta_max,
                                double blend_ratio, int min_samples, int ramp_up) {
    memset(b, 0, sizeof(*b));
    if (n_arms < 2) n_arms = 2;
    if (n_arms > BANDIT_MAX_ARMS) n_arms = BANDIT_MAX_ARMS;
    b->n_arms = n_arms;
    b->gamma = (gamma > 0.0) ? gamma : BANDIT_GAMMA_DEFAULT;
    b->eta_max = (eta_max > 0.0) ? eta_max : BANDIT_ETA_MAX_DEFAULT;
    b->blend_ratio = (blend_ratio > 0.0) ? blend_ratio : BANDIT_BLEND_RATIO_DEFAULT;
    b->min_samples = (min_samples > 0) ? min_samples : BANDIT_MIN_SAMPLES_DEFAULT;
    b->ramp_up_samples = (ramp_up > 0) ? ramp_up : BANDIT_RAMP_UP_DEFAULT;

    // uniform initial weights
    for (int i = 0; i < n_arms; i++) {
        b->weights[i] = 1.0;
        // v5.15.5.A.3 — arm_names extracted to BanditDisplayMeta; caller
        // pairs a BanditDisplayMeta with this BanditState and calls
        // BanditDisplayMeta_InitDefault separately if default labels needed.
    }
}

// convenience: init with default FoxML parameters
static inline void Bandit_InitDefault(BanditState *b, int n_arms) {
    Bandit_Init(b, n_arms, BANDIT_GAMMA_DEFAULT, BANDIT_ETA_MAX_DEFAULT,
                BANDIT_BLEND_RATIO_DEFAULT, BANDIT_MIN_SAMPLES_DEFAULT, BANDIT_RAMP_UP_DEFAULT);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[Bandit_Init]
//======================================================================

//======================================================================
// [FUNCTION]_[Bandit_GetProbabilities]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [ML_INFERENCE] [DETERMINISM]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[Exp3-IX selection probabilities — AVX-512 elementwise kernel with a BYTEWISE-IDENTICAL scalar fallback; sum reductions stay scalar for rounding-order determinism]
// [DIAGRAM]_[formula]
//   p_i = (1 - gamma) * (w_i / sum_w) + gamma / K
// [REFERENCE]_[INVARIANT]_[H10]
// [REFERENCE]_[TECH_DEBT]_[TECH_DEBT-158]
//======================================================================
// [CODE]
//======================================================================
// asan cannot model AVX-512 masked/wide ops (_mm512_mask_storeu_pd / _mm512_loadu_pd over the
// 8-wide weights/probs buffers, BANDIT_MAX_ARMS=8) and false-positives a SEGV on this correct-by-
// construction SIMD kernel — verified bit-exact vs the scalar reference by the v5.11.7 byte-determinism
// tests. Suppress asan here; ubsan + the normal -O3 build still exercise it. (TECH_DEBT-158 close-out.)
__attribute__((no_sanitize("address")))
static inline void Bandit_GetProbabilities(const BanditState *b, double *probs_out) {
    // v5.11.7 — sum reduction stays SCALAR for bytewise determinism.
    // _mm512_reduce_add_pd does tree reduction (different rounding than
    // left-to-right scalar sum), so the same set of doubles can produce
    // different bytes. Hot-path is the elementwise normalize+floor (which
    // IS deterministic across SIMD vs scalar since each element is
    // computed independently with the same IEEE-754 ops).
    double sum_w = 0.0;
    for (int i = 0; i < b->n_arms; i++)
        sum_w += b->weights[i];

    double K = (double)b->n_arms;
    // s5 bandit ship — THE H10 CLOSE, and it lives HERE on purpose: this is the
    // last code both builds execute before the __AVX512F__ fork below.
    //
    // A non-finite weight makes sum_w non-finite (NaN propagates through +; ±inf
    // likewise), so widening this ONE predicate routes the entire hazardous input
    // domain to the uniform fallback in code that is bytewise identical on both
    // paths — and the fork then only ever sees inputs the two implementations
    // agree on. The alternative (per-fork NaN handling) would have to keep two
    // implementations in sync forever.
    //
    // What it fixes: `_mm512_max_pd(probs, floor)` returns its SECOND operand
    // when either is NaN, laundering NaN into 1e-10, while the scalar
    // `if (probs_out[i] < 1e-10)` is false for NaN and leaves it. On the twins
    // artifact the AVX build returned [0.333,0.333,0.333] and the scalar build
    // returned [nan,nan,nan] — an OBSERVED H10 violation, and under -march=native
    // also a cross-HOST determinism break.
    //
    // Do NOT "fix" this by making the scalar floor an fmax to match the
    // intrinsic: that canonizes NaN-laundering as the defined semantic and hides
    // poisoned state forever instead of surfacing it.
    // Spec: DESIGN_SPECS/refactor-patterns/simd-fallback-nonfinite-parity-discipline.md
    if (!(sum_w > 0.0) || !isfinite(sum_w)) {
        // fallback to uniform
        for (int i = 0; i < b->n_arms; i++)
            probs_out[i] = 1.0 / K;
        return;
    }

#if defined(__AVX512F__)
    // v5.11.7 — vectorize the elementwise normalize + affine-blend + floor.
    // Audit Part 5: weights[BANDIT_MAX_ARMS=8] fits cleanly in __m512d.
    // n_arms is variable (typical=5); mask to lower n_arms lanes only.
    //
    // Bytewise determinism: each element computed via IEEE-754 div/mul/add/max
    // with identical operands as the scalar path. Same input → same output
    // bit-for-bit. Verified by v5.11.7 EXTENSIBILITY tests below.
    // BYTEWISE-DETERMINISM CRITICAL: must match scalar IEEE-754 op order.
    //   - _mm512_div_pd(x, y) NOT _mm512_mul_pd(x, 1/y)  (1-ULP could differ)
    //   - _mm512_fmadd_pd(a, b, c) for `a*b + c` — gcc -O3 with default
    //     -ffp-contract=fast fuses scalar `(1-gamma)*normd + g/K` into FMA,
    //     so the AVX path must use FMA too to stay bytewise-equivalent.
    //     If the build ever switches to -ffp-contract=off, swap to
    //     separate _mm512_mul_pd + _mm512_add_pd.
    const __mmask8 mask = (__mmask8)((1u << b->n_arms) - 1u);
    __m512d w         = _mm512_loadu_pd(b->weights);
    __m512d sum_w_vec = _mm512_set1_pd(sum_w);
    __m512d normd     = _mm512_div_pd(w, sum_w_vec);
    __m512d one_min_g = _mm512_set1_pd(1.0 - b->gamma);
    __m512d g_over_K  = _mm512_set1_pd(b->gamma / K);
    __m512d probs     = _mm512_fmadd_pd(one_min_g, normd, g_over_K);
    __m512d floor     = _mm512_set1_pd(1e-10);
    probs             = _mm512_max_pd(probs, floor);
    // Mask-store: write only the lower n_arms lanes; upper lanes remain
    // unwritten (caller must size probs_out at BANDIT_MAX_ARMS=8 doubles
    // per the function contract).
    _mm512_mask_storeu_pd(probs_out, mask, probs);
    // Scalar sum of the just-written probs (preserves left-to-right order
    // for bytewise determinism vs prior version).
    double prob_sum = 0.0;
    for (int i = 0; i < b->n_arms; i++) prob_sum += probs_out[i];
#else
    double prob_sum = 0.0;
    for (int i = 0; i < b->n_arms; i++) {
        double normalized = b->weights[i] / sum_w;
        probs_out[i] = (1.0 - b->gamma) * normalized + b->gamma / K;
        if (probs_out[i] < 1e-10) probs_out[i] = 1e-10;
        prob_sum += probs_out[i];
    }
#endif
    // renormalize
    if (prob_sum > 0.0) {
#if defined(__AVX512F__)
        // BYTEWISE-DETERMINISM CRITICAL: scalar `probs[i] /= prob_sum` is
        // a divide. _mm512_mul_pd(p, 1/prob_sum) would be mul-by-reciprocal
        // (1-ULP could differ). Use _mm512_div_pd to match scalar exactly.
        const __mmask8 mask = (__mmask8)((1u << b->n_arms) - 1u);
        __m512d p           = _mm512_maskz_loadu_pd(mask, probs_out);
        __m512d psum_vec    = _mm512_set1_pd(prob_sum);
        p                   = _mm512_div_pd(p, psum_vec);
        _mm512_mask_storeu_pd(probs_out, mask, p);
#else
        for (int i = 0; i < b->n_arms; i++)
            probs_out[i] /= prob_sum;
#endif
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[Bandit_GetProbabilities]
//======================================================================

//======================================================================
// [FUNCTION]_[Bandit_Select]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [ML_INFERENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[cumulative-distribution sample over GetProbabilities — caller supplies the uniform random in [0,1)]
//======================================================================
// [CODE]
//======================================================================
static inline int Bandit_Select(const BanditState *b, double uniform_rand) {
    double probs[BANDIT_MAX_ARMS];
    Bandit_GetProbabilities(b, probs);

    // cumulative distribution sampling
    double cumulative = 0.0;
    for (int i = 0; i < b->n_arms; i++) {
        cumulative += probs[i];
        if (uniform_rand < cumulative)
            return i;
    }
    return b->n_arms - 1; // numerical safety: pick last arm
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// samples from probability distribution. caller provides uniform random in [0,1).
// returns arm index. use a PRNG or hardware RNG for the random value.
//======================================================================
// [END_FUNCTION]_[Bandit_Select]
//======================================================================

//======================================================================
// [FUNCTION]_[Bandit_Update]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [ML_INFERENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[importance-weighted Exp3-IX reward update with adaptive eta + explosion/vanish clamps (branchless max-find per H20)]
// [DIAGRAM]_[formula]
//   r_hat  = reward / p_arm
//   w_arm *= exp(eta * r_hat)
//   eta    = min(eta_max, sqrt(ln(K) / (K * T)))
// [REFERENCE]_[INVARIANT]_[H20]
// [REFERENCE]_[CLASS]_[28]
//======================================================================
// [CODE]
//======================================================================
static inline void Bandit_Update(BanditState *b, int arm, double reward_bps) {
    if (arm < 0 || arm >= b->n_arms) return;

    b->total_steps++;
    b->pulls[arm]++;
    b->cum_reward[arm] += reward_bps;

    // get probability for importance weighting
    double probs[BANDIT_MAX_ARMS];
    Bandit_GetProbabilities(b, probs);
    double p_arm = probs[arm];

    // importance-weighted reward estimate
    double r_hat = reward_bps / p_arm;

    // adaptive learning rate: eta = min(eta_max, sqrt(ln(K) / (K * T)))
    double eta = b->eta_max;
    if (b->total_steps > 0) {
        double K = (double)b->n_arms;
        double T = (double)b->total_steps;
        double eta_computed = sqrt(log(K) / (K * T));
        if (eta_computed < eta) eta = eta_computed;
    }

    // weight update — the exponent is CLAMPED before exp(), which is the fix
    // BT-2 actually needs. Rejecting a non-finite r_hat would NOT have helped:
    // with the shipped params (gamma=0.05, K=3) the probability floor is
    // gamma/K ~ 0.0167, so a legal +200bps trade-close reward yields
    // r_hat ~ 12000 — perfectly finite — and eta*r_hat still reaches ~1200,
    // far past exp()'s 709.78 overflow. The old code overflowed to +inf and the
    // renorm below turned inf/inf into the persisted NaN. Clamp the exponent,
    // not its inputs. (H20: ternaries lower to cmov/minsd-maxsd, no branch.)
    double expo = eta * r_hat;
    expo = expo >  BANDIT_UPDATE_EXPO_LIMIT ?  BANDIT_UPDATE_EXPO_LIMIT : expo;
    expo = expo < -BANDIT_UPDATE_EXPO_LIMIT ? -BANDIT_UPDATE_EXPO_LIMIT : expo;
    // Finite backstop: a NaN exponent selects NEITHER clamp arm (every ordered
    // comparison with NaN is false), so it would reach exp() intact. 0.0 is the
    // identity multiplier — a poisoned update becomes a no-op instead of
    // spreading. Unreachable once the load side rejects non-finite state; kept
    // because the cost is one cmov and the failure mode it prevents is permanent.
    expo = isfinite(expo) ? expo : 0.0;
    b->weights[arm] *= exp(expo);

    // numerical stability: prevent explosion or vanishing
    // v5.15.5.F.4d Step 6 (§ L) — Class 28 cmov branchless max-find (H20 / determinism over throughput).
    double max_w = 0.0;
    for (int i = 0; i < b->n_arms; i++) {
        int win = b->weights[i] > max_w;
        max_w   = win ? b->weights[i] : max_w;
    }
    if (max_w > 1e6) {
        for (int i = 0; i < b->n_arms; i++)
            b->weights[i] /= max_w;
    } else if (max_w > 0.0 && max_w < BANDIT_WEIGHT_LOW_WATER) {
        // s5 bandit ship (BT-11 structural prevent) — the explosion guard's
        // mirror image. Scaling the vector so max == 1.0 is ratio-preserving:
        // no arm's standing changes, the vector just stops sinking toward the
        // 1e-10 floor where the NEXT floor-clamp would flatten real differences
        // into a tie. Mutually exclusive with the >1e6 branch above (after that
        // divide, max == 1.0 by construction).
        const double scale = 1.0 / max_w;
        for (int i = 0; i < b->n_arms; i++)
            b->weights[i] *= scale;
    }
    // Floor, finite-backstopped as a select (H20). `w < 1e-10` alone is false for
    // NaN, which is precisely how the twins NaN survived every pass of this loop
    // (verified: 100 consecutive guard passes left it untouched).
    for (int i = 0; i < b->n_arms; i++) {
        const double w = b->weights[i];
        b->weights[i] = (w < 1e-10 || !isfinite(w)) ? 1e-10 : w;
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[Bandit_Update]
//======================================================================

//======================================================================
// [FUNCTION]_[Bandit_GetWeights]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [ML_INFERENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[weights normalized to sum 1.0 (for blending / display); uniform fallback on non-positive sum]
//======================================================================
// [CODE]
//======================================================================
static inline void Bandit_GetWeights(const BanditState *b, double *weights_out) {
    double sum_w = 0.0;
    for (int i = 0; i < b->n_arms; i++)
        sum_w += b->weights[i];
    // s5 bandit ship — same widened predicate as Bandit_GetProbabilities. This
    // is a SEPARATE entry point into the weights (EngineTUI display + the legacy
    // PortfolioController blend read it directly, never via GetProbabilities), so
    // it needs its own finite gate rather than inheriting one.
    if (!(sum_w > 0.0) || !isfinite(sum_w)) {
        for (int i = 0; i < b->n_arms; i++)
            weights_out[i] = 1.0 / b->n_arms;
        return;
    }
    for (int i = 0; i < b->n_arms; i++)
        weights_out[i] = b->weights[i] / sum_w;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[Bandit_GetWeights]
//======================================================================

//======================================================================
// [FUNCTION]_[Bandit_BlendWeights]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [ML_INFERENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[static<->bandit weight blend with the FoxML ramp-up schedule; EffectiveBlend rides in this section]
// [DIAGRAM]_[formula]
//   final = (1 - effective_blend) * static + effective_blend * bandit
//======================================================================
// [CODE]
//======================================================================
static inline double Bandit_EffectiveBlend(const BanditState *b) {
    if (b->total_steps < b->min_samples) return 0.0;
    if (b->ramp_up_samples <= 0) return b->blend_ratio;
    int excess = b->total_steps - b->min_samples;
    double progress = (double)excess / b->ramp_up_samples;
    if (progress > 1.0) progress = 1.0;
    return b->blend_ratio * progress;
}

static inline void Bandit_BlendWeights(const BanditState *b,
                                         const double *static_weights,
                                         double *blended_out) {
    double effective = Bandit_EffectiveBlend(b);

    if (effective <= 0.0) {
        // 100% static
        for (int i = 0; i < b->n_arms; i++)
            blended_out[i] = static_weights[i];
        return;
    }

    double bandit_w[BANDIT_MAX_ARMS];
    Bandit_GetWeights(b, bandit_w);

    double sum = 0.0;
    for (int i = 0; i < b->n_arms; i++) {
        blended_out[i] = (1.0 - effective) * static_weights[i] + effective * bandit_w[i];
        sum += blended_out[i];
    }
    // renormalize — finite-gated for the same reason as the guards above. The
    // bandit half is now finite by construction; this covers a non-finite
    // static_weights[] arriving from a caller, where dividing would spread the
    // poison across every arm instead of leaving it in the one it came from.
    if (sum > 0.0 && isfinite(sum)) {
        for (int i = 0; i < b->n_arms; i++)
            blended_out[i] /= sum;
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// ramp-up schedule (from FoxML weight_optimizer.py):
//   steps < min_samples:           effective_blend = 0 (100% static)
//   min_samples <= steps < min+ramp: linear ramp from 0 to blend_ratio
//   steps >= min+ramp:             effective_blend = blend_ratio
//======================================================================
// [END_FUNCTION]_[Bandit_BlendWeights]
//======================================================================

//======================================================================
// [FUNCTION]_[Bandit_Print]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [MONITORING_PLANE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[stderr diagnostics — per-arm pulls/avg/weight/prob; optional BanditDisplayMeta for human-readable names]
//======================================================================
// [CODE]
//======================================================================
static inline void Bandit_Print(const BanditState *b,
                                 const BanditDisplayMeta *m = nullptr) {
    double probs[BANDIT_MAX_ARMS], weights[BANDIT_MAX_ARMS];
    Bandit_GetProbabilities(b, probs);
    Bandit_GetWeights(b, weights);

    fprintf(stderr, "[bandit] %d arms, %d steps, blend=%.1f%%\n",
            b->n_arms, b->total_steps, Bandit_EffectiveBlend(b) * 100.0);
    for (int i = 0; i < b->n_arms; i++) {
        double avg = (b->pulls[i] > 0) ? b->cum_reward[i] / b->pulls[i] : 0.0;
        char default_name[16];
        const char *name;
        if (m && m->arm_names[i][0]) {
            name = m->arm_names[i];
        } else {
            snprintf(default_name, sizeof(default_name), "arm_%d", i);
            name = default_name;
        }
        fprintf(stderr, "  %s: pulls=%d, avg=%.1f bps, weight=%.3f, prob=%.3f\n",
                name, b->pulls[i], avg, weights[i], probs[i]);
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// v5.15.5.A.3 — Bandit_Print takes optional display meta. Pass nullptr
// to print default "arm_N" labels; pass a populated BanditDisplayMeta*
// to print human-readable names.
//======================================================================
// [END_FUNCTION]_[Bandit_Print]
//======================================================================

#define BANDIT_STATE_FORMAT_VERSION 1

//======================================================================
// [FUNCTION]_[Bandit_SaveJSON]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [PERSISTENCE] [DETERMINISM]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[per-regime BanditState array -> JSON sidecar; atomic tmp+rename write; LC_NUMERIC=C pinned per-thread at emit]
// [REFERENCE]_[INVARIANT]_[H21]
// [REFERENCE]_[TECH_DEBT]_[TECH_DEBT-27]
//======================================================================
// [CODE]
//======================================================================
static inline int Bandit_SaveJSON(const BanditState* bandits,
                                    int n_regimes,
                                    const char* path,
                                    const char* model_bundle_sha256_hex,
                                    const char* const* regime_names) {
    if (!bandits || !path || n_regimes <= 0) return 0;
    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    FILE* f = fopen(tmp_path, "w");
    if (!f) {
        // E.1.2.D D-a — loud-fail: the bare `return 0` here was the silence
        // that let an unwritable state path hide for a family's whole
        // lifetime (every caller is `if (saved) {log}` with no else).
        fprintf(stderr, "[bandit] SAVE FAILED: fopen(%s): %s\n",
                tmp_path, strerror(errno));
        return 0;
    }

    // v5.14.10.C — TECH_DEBT-027 close. Pin LC_NUMERIC=C (per-thread via
    // uselocale) so %.17g emits ASCII decimal point regardless of process
    // locale. Without this, an engine launched under LC_NUMERIC=de_DE
    // would write "0,55" instead of "0.55"; tt::parse_double_fast (locale-
    // immune via from_chars) would parse "0" → silent state corruption.
    locale_t pinned_locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
    locale_t prev_locale = (locale_t)0;
    if (pinned_locale) prev_locale = uselocale(pinned_locale);

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long long ns = (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;

    int n_arms = bandits[0].n_arms;
    fprintf(f, "{\n");
    fprintf(f, "  \"format_version\": %d,\n", BANDIT_STATE_FORMAT_VERSION);
    fprintf(f, "  \"saved_at_ts_ns\": %lld,\n", ns);
    fprintf(f, "  \"model_bundle_sha256\": \"%s\",\n",
            model_bundle_sha256_hex ? model_bundle_sha256_hex : "");
    fprintf(f, "  \"n_regimes\": %d,\n", n_regimes);
    fprintf(f, "  \"n_arms\": %d,\n", n_arms);
    fprintf(f, "  \"regimes\": [\n");
    for (int r = 0; r < n_regimes; ++r) {
        const BanditState& b = bandits[r];
        fprintf(f, "    {\n");
        fprintf(f, "      \"regime_id\": %d,\n", r);
        if (regime_names && regime_names[r]) {
            fprintf(f, "      \"regime_name\": \"%s\",\n", regime_names[r]);
        }
        fprintf(f, "      \"n_arms\": %d,\n", b.n_arms);
        fprintf(f, "      \"total_steps\": %d,\n", b.total_steps);
        fprintf(f, "      \"weights\": [");
        for (int a = 0; a < b.n_arms; ++a) {
            fprintf(f, "%s%.17g", (a ? ", " : ""), b.weights[a]);
        }
        fprintf(f, "],\n");
        fprintf(f, "      \"cum_reward\": [");
        for (int a = 0; a < b.n_arms; ++a) {
            fprintf(f, "%s%.17g", (a ? ", " : ""), b.cum_reward[a]);
        }
        fprintf(f, "],\n");
        fprintf(f, "      \"pulls\": [");
        for (int a = 0; a < b.n_arms; ++a) {
            fprintf(f, "%s%d", (a ? ", " : ""), b.pulls[a]);
        }
        fprintf(f, "],\n");
        // v5.15.5.A.3 — arm_names extracted to BanditDisplayMeta (not part
        // of BanditState anymore). Emit default "arm_N" labels in the JSON
        // for backward-compat shape; loaders don't parse this field anyway
        // (verified at Bandit_LoadJSON). Callers wanting custom names in
        // the JSON should call a future Bandit_SaveJSON variant that takes
        // a BanditDisplayMeta* array (not yet defined; add when needed).
        fprintf(f, "      \"arm_names\": [");
        for (int a = 0; a < b.n_arms; ++a) {
            fprintf(f, "%s\"arm_%d\"", (a ? ", " : ""), a);
        }
        fprintf(f, "]\n");
        fprintf(f, "    }%s\n", (r < n_regimes - 1) ? "," : "");
    }
    fprintf(f, "  ]\n");
    fprintf(f, "}\n");

    // Restore caller's locale before fclose (file handle isn't locale-bound;
    // restore order matches the stamp_write_for_model precedent in ModelInference.hpp).
    if (pinned_locale) {
        uselocale(prev_locale);
        freelocale(pinned_locale);
    }

    if (fclose(f) != 0) {
        unlink(tmp_path);
        return 0;
    }
    if (rename(tmp_path, path) != 0) {
        unlink(tmp_path);
        return 0;
    }
    return 1;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Save BanditState array (one per regime) to JSON file. Returns 1 on
// success, 0 on any failure (open, write, rename). Atomic via tmpfile +
// rename.
//
// regime_names: optional; if non-NULL, written as "regime_name" field
// inside each entry. Caller responsibility to provide an array of size
// n_regimes (NUM_REGIMES). NULL → omits the field.
//
// Bandit weights are valuable — they encode "what horizons work for THIS
// asset/regime/market." Without persistence, they reset to uniform every
// restart, wasting hours of online learning. G.9 persists arrays of
// BanditState (one per regime) to a JSON sidecar in the model bundle dir.
//
// Atomic write via fopen(tmp) + write + close + rename — matches stamp
// persistence pattern; no partial-file window for concurrent readers.
//
// SHA validation: bundle SHA carried in the JSON; load returns 0 (caller
// resets to uniform) on mismatch. Prevents stale weights from a different
// model bundle silently re-applying.
//
// LOCALE: writes use "C" formatting (%.17g) for cross-locale round-trips.
// The reader parses doubles via tt::parse_double_fast_advance
// (std::from_chars — locale-immune per v5.11.4.C), so round-trips hold
// regardless of process LC_NUMERIC.
//======================================================================
// [END_FUNCTION]_[Bandit_SaveJSON]
//======================================================================

//======================================================================
// [FUNCTION]_[Bandit_LoadJSON]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BOOT_TIME] [PERSISTENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[overlay persisted weights/cum_reward/pulls onto Init'd states — format/arms/regimes/SHA gates, uniform fallback on any miss; the tt::json_io primitives ride in this section]
// [REFERENCE]_[INVARIANT]_[H21]
//======================================================================
// [CODE]
//======================================================================
//----------------------------------------------------------------------
// [SECTION]_[tt::json_io primitives — generic, reusable across Bandit + Thompson + future state-persistence]
//----------------------------------------------------------------------
// v5.14.10.C — extracted to tt::json_io namespace from Bandit_Json* sister
// functions (per /merge-scan T4 finding). NOT a real JSON parser — just
// walks to "key": and returns position past the colon; reads numeric arrays
// until ']'. Sufficient for state-persistence sidecar files (bandit_state.json,
// thompson_state.json) where format is operator-controlled + tiny.
//
// Used by Bandit_LoadJSON below + EnsembleModelZoo_LoadThompsonState in NodeModelZoo.hpp.
namespace tt { namespace json_io {

// Scan for a JSON key and return position past the ':'. Returns nullptr if
// not found (caller treats as "field absent"; defaults / forward-compat-by-
// absence apply per Surface G discipline).
static inline const char* find_key(const char* haystack, const char* key) {
    if (!haystack || !key) return nullptr;
    char needle[96];
    int wrote = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (wrote <= 0 || wrote >= (int)sizeof(needle)) return nullptr;
    const char* p = strstr(haystack, needle);
    if (!p) return nullptr;
    p += wrote;
    while (*p == ' ' || *p == ':' || *p == '\t') ++p;
    return p;
}

// Parse a JSON numeric array starting at p. Skips leading whitespace + '['.
// Reads up to max_count doubles; returns count parsed. Stops at ']' or first
// non-numeric / malformed input.
//
// Locale-IMMUNE via tt::parse_double_fast_advance (uses std::from_chars per
// v5.11.4.C). Safe regardless of process LC_NUMERIC.
//
// s5 bandit ship (BT-3) — NON-FINITE DETECTION. `std::from_chars` accepts the
// literal tokens `nan` / `-nan` / `inf`, so a poisoned state file round-trips
// its poison straight back into live weights: verified against the real parser
// on the real bytes (`parsed 3 of 3 weights … weights[1] isnan=1`). That is why
// the twins `-nan` reloaded at EVERY boot and no amount of further training
// cleared it.
//
// The check lives HERE, in the ONE primitive all four state loaders share,
// rather than at each call site — four hand-written caller checks is the
// same mirror shape this ship exists to remove.
//
// `nonfinite_out` has NO DEFAULT ARGUMENT on purpose: the compiler then
// enumerates every caller for the author (the leaf-5 `labels_precomputed`
// precedent). A defaulted flag would let a future loader silently opt out of
// the guard and reintroduce BT-3 while every gate stayed green — Class-58
// complement blindness. Callers must reject the WHOLE FILE when it is set:
// this copies min(got, n_arms) values, so a per-value skip would leave a
// half-overlaid regime and still report success, which is worse than either
// loading or rejecting outright.
static inline int parse_double_array(const char* p, double* out, int max_count,
                                      int* nonfinite_out) {
    if (nonfinite_out) *nonfinite_out = 0;
    if (!p || !out) return 0;
    while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '[') ++p;
    int count = 0;
    while (*p && *p != ']' && count < max_count) {
        const char* end_ptr = nullptr;
        double v = tt::parse_double_fast_advance(p, &end_ptr);
        if (end_ptr == p) break;   // no number consumed
        if (!isfinite(v) && nonfinite_out) *nonfinite_out = 1;
        out[count++] = v;
        p = end_ptr;
        while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\t') ++p;
    }
    return count;
}

// Parse a JSON integer array. Same shape as parse_double_array but for ints.
static inline int parse_int_array(const char* p, int* out, int max_count) {
    if (!p || !out) return 0;
    while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '[') ++p;
    int count = 0;
    while (*p && *p != ']' && count < max_count) {
        char* end_ptr = nullptr;
        long v = strtol(p, &end_ptr, 10);
        if (end_ptr == p) break;
        out[count++] = (int)v;
        p = end_ptr;
        while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\t') ++p;
    }
    return count;
}

// v5.14.10.C — uint32 array variant (for thompson_state.total_pulls[] which
// uses uint32_t per BanditState.pulls[] precedent).
static inline int parse_uint32_array(const char* p, uint32_t* out, int max_count) {
    if (!p || !out) return 0;
    while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '[') ++p;
    int count = 0;
    while (*p && *p != ']' && count < max_count) {
        char* end_ptr = nullptr;
        unsigned long v = strtoul(p, &end_ptr, 10);
        if (end_ptr == p) break;
        out[count++] = (uint32_t)v;
        p = end_ptr;
        while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\t') ++p;
    }
    return count;
}

}} // namespace tt::json_io

//----------------------------------------------------------------------
// [SECTION]_[Bandit_LoadJSON]
//----------------------------------------------------------------------
// Load BanditState array from JSON file. Returns 1 if loaded + valid;
// 0 if missing/corrupt/sha-mismatch (caller falls back to uniform via
// Bandit_Init).
//
// Validates:
//   - File opens + reads
//   - format_version matches
//   - n_arms in JSON matches expected_n_arms
//   - n_regimes >= expected n_regimes
//   - SHA matches expected (if expected_sha non-empty; else skipped)
//
// Reads weights / cum_reward / pulls / total_steps directly into
// bandits[r] entries. Other fields (gamma, eta_max, etc.) come from
// caller's prior Bandit_Init call — load is overlay only.
static inline int Bandit_LoadJSON(BanditState* bandits,
                                    int n_regimes,
                                    const char* path,
                                    const char* expected_model_bundle_sha256_hex,
                                    int expected_n_arms) {
    if (!bandits || !path || n_regimes <= 0) return 0;
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    // v5.11.6.C — replace malloc with thread-local static buffer.
    // bandit_state.json is operator-controlled and tiny (<100 KB in
    // realistic deployments). Cap at 256 KB to keep the buffer on .bss
    // not the stack and to enforce a tighter bound than the prior 1 MB.
    // Removes the 13 `free(buf)` cleanup sites that previously had to
    // be threaded through every error-return.
    constexpr size_t BUF_CAP = 256 * 1024;
    if (fsize <= 0 || (size_t)fsize >= BUF_CAP) {
        fclose(f);
        return 0;
    }
    static thread_local char buf_storage[BUF_CAP];
    char* buf = buf_storage;
    size_t read_n = fread(buf, 1, (size_t)fsize, f);
    fclose(f);
    buf[read_n] = '\0';

    // format_version check
    const char* p = tt::json_io::find_key(buf, "format_version");
    if (!p) { return 0; }
    int fmt = (int)strtol(p, NULL, 10);
    if (fmt != BANDIT_STATE_FORMAT_VERSION) { return 0; }

    // n_arms check
    p = tt::json_io::find_key(buf, "n_arms");
    if (!p) { return 0; }
    int file_n_arms = (int)strtol(p, NULL, 10);
    if (file_n_arms != expected_n_arms) { return 0; }

    // n_regimes check (file may have more, never fewer)
    p = tt::json_io::find_key(buf, "n_regimes");
    if (!p) { return 0; }
    int file_n_regimes = (int)strtol(p, NULL, 10);
    if (file_n_regimes < n_regimes) { return 0; }

    // SHA check (only if caller supplied expected)
    if (expected_model_bundle_sha256_hex && expected_model_bundle_sha256_hex[0]) {
        p = tt::json_io::find_key(buf, "model_bundle_sha256");
        if (!p) { return 0; }
        // Skip leading whitespace + open quote
        while (*p == ' ' || *p == '\t') ++p;
        if (*p != '"') { return 0; }
        ++p;
        const char* end = strchr(p, '"');
        if (!end) { return 0; }
        size_t sha_len = (size_t)(end - p);
        if (sha_len != strlen(expected_model_bundle_sha256_hex) ||
            memcmp(p, expected_model_bundle_sha256_hex, sha_len) != 0) {
            
            return 0;
        }
    }

    // Per-regime parse: walk to "regimes" array, then for each "regime_id"
    // entry, populate weights / cum_reward / pulls.
    // s5 BT-3: sticky across ALL regimes — one poisoned value anywhere rejects
    // the file (checked after the loop, so the diagnostic names the file once
    // rather than once per regime).
    int file_nonfinite = 0;
    p = tt::json_io::find_key(buf, "regimes");
    if (!p) { return 0; }
    while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '[') ++p;

    for (int r = 0; r < n_regimes; ++r) {
        // Find next "regime_id" past current p
        const char* rid_p = tt::json_io::find_key(p, "regime_id");
        if (!rid_p) {
            
            return 0;
        }
        int regime_id = (int)strtol(rid_p, NULL, 10);
        if (regime_id < 0 || regime_id >= n_regimes) {
            // Skip unknown regime — tolerate forward-compat (e.g. NUM_REGIMES grew)
            p = rid_p + 4;
            continue;
        }
        BanditState& b = bandits[regime_id];
        // Don't reset b.n_arms / gamma — caller already initialized.
        // Just overlay the persisted state.

        // total_steps
        const char* ts_p = tt::json_io::find_key(rid_p, "total_steps");
        if (ts_p) b.total_steps = (int)strtol(ts_p, NULL, 10);

        // weights
        const char* w_p = tt::json_io::find_key(rid_p, "weights");
        if (w_p) {
            double tmp_w[BANDIT_MAX_ARMS];
            int nonfinite = 0;
            int got = tt::json_io::parse_double_array(w_p, tmp_w, BANDIT_MAX_ARMS, &nonfinite);
            if (nonfinite) { file_nonfinite = 1; }   // s5 BT-3: never overlay poison
            else {
                int copy = (got < b.n_arms) ? got : b.n_arms;
                for (int a = 0; a < copy; ++a) b.weights[a] = tmp_w[a];
            }
        }

        // cum_reward
        const char* cr_p = tt::json_io::find_key(rid_p, "cum_reward");
        if (cr_p) {
            double tmp_cr[BANDIT_MAX_ARMS];
            int nonfinite = 0;
            int got = tt::json_io::parse_double_array(cr_p, tmp_cr, BANDIT_MAX_ARMS, &nonfinite);
            if (nonfinite) { file_nonfinite = 1; }
            else {
                int copy = (got < b.n_arms) ? got : b.n_arms;
                for (int a = 0; a < copy; ++a) b.cum_reward[a] = tmp_cr[a];
            }
        }

        // pulls
        const char* pl_p = tt::json_io::find_key(rid_p, "pulls");
        if (pl_p) {
            int tmp_p[BANDIT_MAX_ARMS];
            int got = tt::json_io::parse_int_array(pl_p, tmp_p, BANDIT_MAX_ARMS);
            int copy = (got < b.n_arms) ? got : b.n_arms;
            for (int a = 0; a < copy; ++a) b.pulls[a] = tmp_p[a];
        }

        // Advance p past this entry's closing brace
        const char* end_brace = strchr(rid_p, '}');
        p = end_brace ? end_brace + 1 : rid_p + 1;
    }

    // s5 bandit ship (BT-3) — WHOLE-FILE reject on any non-finite value.
    //
    // Restoring uniform is what makes this honest rather than partial: earlier
    // regimes in the file may already have been overlaid before the poisoned one
    // was reached, and leaving that half-loaded state behind while telling the
    // caller "rejected" is the failure mode being fixed, one level up. Reset the
    // LEARNED fields only — gamma / eta_max / blend / n_arms are the caller's
    // Bandit_Init configuration and are not this file's to touch.
    //
    // Cost, stated plainly: a file with one poisoned regime loses its HEALTHY
    // regimes too. Accepted — a bundle that has minted a non-finite weight has
    // demonstrated its update history is untrustworthy, and uniform is a
    // defined starting point (missing-file already means exactly this).
    if (file_nonfinite) {
        for (int r = 0; r < n_regimes; ++r) {
            BanditState& b = bandits[r];
            b.total_steps = 0;
            for (int a = 0; a < BANDIT_MAX_ARMS; ++a) {
                b.weights[a]    = (a < b.n_arms) ? 1.0 : 0.0;
                b.cum_reward[a] = 0.0;
                b.pulls[a]      = 0;
            }
        }
        fprintf(stderr,
                "[bandit] %s: REJECTED — non-finite (NaN/Inf) value in persisted state; "
                "restored uniform. The file is not repaired: delete it, or it will be "
                "rejected again every boot.\n", path);
        return 0;
    }

    return 1;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[Bandit_LoadJSON]
//======================================================================

#endif // BANDIT_LEARNING_HPP
