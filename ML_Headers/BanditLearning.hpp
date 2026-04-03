// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [BANDIT LEARNING — EXP3-IX ONLINE STRATEGY SELECTION]
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

// default parameters (from FoxML bandit.py + weight_optimizer.py)
#define BANDIT_GAMMA_DEFAULT       0.05    // exploration rate
#define BANDIT_ETA_MAX_DEFAULT     0.07    // max learning rate
#define BANDIT_BLEND_RATIO_DEFAULT 0.30    // 30% bandit influence
#define BANDIT_MIN_SAMPLES_DEFAULT 100     // trades before bandit activates
#define BANDIT_RAMP_UP_DEFAULT     100     // trades to reach full blend
#define BANDIT_MAX_ARMS            8       // max arms supported

//======================================================================================================
// [BANDIT STATE]
//======================================================================================================
struct BanditState {
    double weights[BANDIT_MAX_ARMS];       // unnormalized weights (exp3-ix)
    double cum_reward[BANDIT_MAX_ARMS];    // cumulative P&L per arm (bps)
    int pulls[BANDIT_MAX_ARMS];            // pull count per arm
    char arm_names[BANDIT_MAX_ARMS][32];   // human-readable arm names
    int n_arms;
    int total_steps;
    double gamma;           // exploration rate
    double eta_max;         // max learning rate
    double blend_ratio;     // bandit influence fraction
    int min_samples;        // minimum trades before bandit activates
    int ramp_up_samples;    // trades to ramp from 0 to blend_ratio
};

//======================================================================================================
// [INIT]
//======================================================================================================
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
        snprintf(b->arm_names[i], sizeof(b->arm_names[i]), "arm_%d", i);
    }
}

// convenience: init with default FoxML parameters
static inline void Bandit_InitDefault(BanditState *b, int n_arms) {
    Bandit_Init(b, n_arms, BANDIT_GAMMA_DEFAULT, BANDIT_ETA_MAX_DEFAULT,
                BANDIT_BLEND_RATIO_DEFAULT, BANDIT_MIN_SAMPLES_DEFAULT, BANDIT_RAMP_UP_DEFAULT);
}

// set arm name (call after init)
static inline void Bandit_SetArmName(BanditState *b, int arm, const char *name) {
    if (arm >= 0 && arm < b->n_arms)
        snprintf(b->arm_names[arm], sizeof(b->arm_names[arm]), "%s", name);
}

//======================================================================================================
// [PROBABILITIES]
// p_i = (1 - gamma) * (w_i / sum_w) + gamma / K
//======================================================================================================
static inline void Bandit_GetProbabilities(const BanditState *b, double *probs_out) {
    double sum_w = 0.0;
    for (int i = 0; i < b->n_arms; i++)
        sum_w += b->weights[i];

    double K = (double)b->n_arms;
    if (sum_w <= 0.0) {
        // fallback to uniform
        for (int i = 0; i < b->n_arms; i++)
            probs_out[i] = 1.0 / K;
        return;
    }

    double prob_sum = 0.0;
    for (int i = 0; i < b->n_arms; i++) {
        double normalized = b->weights[i] / sum_w;
        probs_out[i] = (1.0 - b->gamma) * normalized + b->gamma / K;
        if (probs_out[i] < 1e-10) probs_out[i] = 1e-10;
        prob_sum += probs_out[i];
    }
    // renormalize
    if (prob_sum > 0.0) {
        for (int i = 0; i < b->n_arms; i++)
            probs_out[i] /= prob_sum;
    }
}

//======================================================================================================
// [SELECT ARM]
// samples from probability distribution. caller provides uniform random in [0,1).
// returns arm index. use a PRNG or hardware RNG for the random value.
//======================================================================================================
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

//======================================================================================================
// [UPDATE]
// importance-weighted reward update:
//   r_hat = reward / p_arm
//   w_arm *= exp(eta * r_hat)
// with adaptive eta: min(eta_max, sqrt(ln(K) / (K * T)))
//======================================================================================================
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

    // weight update
    b->weights[arm] *= exp(eta * r_hat);

    // numerical stability: prevent explosion or vanishing
    double max_w = 0.0;
    for (int i = 0; i < b->n_arms; i++)
        if (b->weights[i] > max_w) max_w = b->weights[i];
    if (max_w > 1e6) {
        for (int i = 0; i < b->n_arms; i++)
            b->weights[i] /= max_w;
    }
    for (int i = 0; i < b->n_arms; i++)
        if (b->weights[i] < 1e-10) b->weights[i] = 1e-10;
}

//======================================================================================================
// [NORMALIZED WEIGHTS]
// returns weights summing to 1.0 (for blending / display)
//======================================================================================================
static inline void Bandit_GetWeights(const BanditState *b, double *weights_out) {
    double sum_w = 0.0;
    for (int i = 0; i < b->n_arms; i++)
        sum_w += b->weights[i];
    if (sum_w <= 0.0) {
        for (int i = 0; i < b->n_arms; i++)
            weights_out[i] = 1.0 / b->n_arms;
        return;
    }
    for (int i = 0; i < b->n_arms; i++)
        weights_out[i] = b->weights[i] / sum_w;
}

//======================================================================================================
// [BLEND WITH STATIC WEIGHTS]
// final = (1 - effective_blend) * static + effective_blend * bandit
//
// ramp-up schedule (from FoxML weight_optimizer.py):
//   steps < min_samples:           effective_blend = 0 (100% static)
//   min_samples <= steps < min+ramp: linear ramp from 0 to blend_ratio
//   steps >= min+ramp:             effective_blend = blend_ratio
//======================================================================================================
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
    // renormalize
    if (sum > 0.0) {
        for (int i = 0; i < b->n_arms; i++)
            blended_out[i] /= sum;
    }
}

//======================================================================================================
// [DIAGNOSTICS]
//======================================================================================================
static inline void Bandit_Print(const BanditState *b) {
    double probs[BANDIT_MAX_ARMS], weights[BANDIT_MAX_ARMS];
    Bandit_GetProbabilities(b, probs);
    Bandit_GetWeights(b, weights);

    fprintf(stderr, "[bandit] %d arms, %d steps, blend=%.1f%%\n",
            b->n_arms, b->total_steps, Bandit_EffectiveBlend(b) * 100.0);
    for (int i = 0; i < b->n_arms; i++) {
        double avg = (b->pulls[i] > 0) ? b->cum_reward[i] / b->pulls[i] : 0.0;
        fprintf(stderr, "  %s: pulls=%d, avg=%.1f bps, weight=%.3f, prob=%.3f\n",
                b->arm_names[i], b->pulls[i], avg, weights[i], probs[i]);
    }
}

#endif // BANDIT_LEARNING_HPP
