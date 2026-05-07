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
#include <stdlib.h>
#include <time.h>
#include "../CoreFrameworks/ParseFast.hpp"  // v5.11.4.C — std::from_chars wrapper for locale-immune parsing
#include <unistd.h>     // unlink, write

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

//======================================================================================================
// [PERSISTENCE — JSON SAVE/LOAD]  (v5.10.0a.G.9)
//======================================================================================================
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
// Reader uses strtod which is locale-aware — pinning the LC_NUMERIC at
// process boot is the engine's responsibility (already done; see
// EngineSharded boot path).

#define BANDIT_STATE_FORMAT_VERSION 1

// Save BanditState array (one per regime) to JSON file. Returns 1 on
// success, 0 on any failure (open, write, rename). Atomic via tmpfile +
// rename.
//
// regime_names: optional; if non-NULL, written as "regime_name" field
// inside each entry. Caller responsibility to provide an array of size
// n_regimes (NUM_REGIMES). NULL → omits the field.
static inline int Bandit_SaveJSON(const BanditState* bandits,
                                    int n_regimes,
                                    const char* path,
                                    const char* model_bundle_sha256_hex,
                                    const char* const* regime_names) {
    if (!bandits || !path || n_regimes <= 0) return 0;
    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    FILE* f = fopen(tmp_path, "w");
    if (!f) return 0;

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
        fprintf(f, "      \"arm_names\": [");
        for (int a = 0; a < b.n_arms; ++a) {
            fprintf(f, "%s\"%s\"", (a ? ", " : ""), b.arm_names[a]);
        }
        fprintf(f, "]\n");
        fprintf(f, "    }%s\n", (r < n_regimes - 1) ? "," : "");
    }
    fprintf(f, "  ]\n");
    fprintf(f, "}\n");
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

// Helper: scan a JSON-ish key and skip ahead. Returns ptr to value start,
// or NULL if not found. NOT a real JSON parser — just walks to "key": and
// returns position past the colon. Whitespace + escapes minimal.
static inline const char* Bandit_JsonFindKey(const char* haystack,
                                               const char* key) {
    if (!haystack || !key) return NULL;
    char needle[96];
    int wrote = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (wrote <= 0 || wrote >= (int)sizeof(needle)) return NULL;
    const char* p = strstr(haystack, needle);
    if (!p) return NULL;
    p += wrote;
    while (*p == ' ' || *p == ':' || *p == '\t') ++p;
    return p;
}

// Parse a numeric array starting at p (after the '['). Reads up to max
// values; returns count parsed. Stops at ']'.
static inline int Bandit_JsonParseDoubleArray(const char* p, double* out,
                                                int max_count) {
    if (!p || !out) return 0;
    while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '[') ++p;
    int count = 0;
    while (*p && *p != ']' && count < max_count) {
        // v5.11.4.C — locale-immune via std::from_chars (replaces strtod
        // which honors LC_NUMERIC). Same end-pointer "no progress" sentinel.
        const char* end_ptr = nullptr;
        double v = tt::parse_double_fast_advance(p, &end_ptr);
        if (end_ptr == p) break;  // no number consumed
        out[count++] = v;
        p = end_ptr;
        while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\t') ++p;
    }
    return count;
}

static inline int Bandit_JsonParseIntArray(const char* p, int* out,
                                              int max_count) {
    if (!p || !out) return 0;
    while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '[') ++p;
    int count = 0;
    while (*p && *p != ']' && count < max_count) {
        char* end_ptr = NULL;
        long v = strtol(p, &end_ptr, 10);
        if (end_ptr == p) break;
        out[count++] = (int)v;
        p = end_ptr;
        while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\t') ++p;
    }
    return count;
}

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
    if (fsize <= 0 || fsize > 1024 * 1024) {  // 1MB cap; bandit JSON is tiny
        fclose(f);
        return 0;
    }
    char* buf = (char*)malloc((size_t)fsize + 1);
    if (!buf) { fclose(f); return 0; }
    size_t read_n = fread(buf, 1, (size_t)fsize, f);
    fclose(f);
    buf[read_n] = '\0';

    // format_version check
    const char* p = Bandit_JsonFindKey(buf, "format_version");
    if (!p) { free(buf); return 0; }
    int fmt = (int)strtol(p, NULL, 10);
    if (fmt != BANDIT_STATE_FORMAT_VERSION) { free(buf); return 0; }

    // n_arms check
    p = Bandit_JsonFindKey(buf, "n_arms");
    if (!p) { free(buf); return 0; }
    int file_n_arms = (int)strtol(p, NULL, 10);
    if (file_n_arms != expected_n_arms) { free(buf); return 0; }

    // n_regimes check (file may have more, never fewer)
    p = Bandit_JsonFindKey(buf, "n_regimes");
    if (!p) { free(buf); return 0; }
    int file_n_regimes = (int)strtol(p, NULL, 10);
    if (file_n_regimes < n_regimes) { free(buf); return 0; }

    // SHA check (only if caller supplied expected)
    if (expected_model_bundle_sha256_hex && expected_model_bundle_sha256_hex[0]) {
        p = Bandit_JsonFindKey(buf, "model_bundle_sha256");
        if (!p) { free(buf); return 0; }
        // Skip leading whitespace + open quote
        while (*p == ' ' || *p == '\t') ++p;
        if (*p != '"') { free(buf); return 0; }
        ++p;
        const char* end = strchr(p, '"');
        if (!end) { free(buf); return 0; }
        size_t sha_len = (size_t)(end - p);
        if (sha_len != strlen(expected_model_bundle_sha256_hex) ||
            memcmp(p, expected_model_bundle_sha256_hex, sha_len) != 0) {
            free(buf);
            return 0;
        }
    }

    // Per-regime parse: walk to "regimes" array, then for each "regime_id"
    // entry, populate weights / cum_reward / pulls.
    p = Bandit_JsonFindKey(buf, "regimes");
    if (!p) { free(buf); return 0; }
    while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '[') ++p;

    for (int r = 0; r < n_regimes; ++r) {
        // Find next "regime_id" past current p
        const char* rid_p = Bandit_JsonFindKey(p, "regime_id");
        if (!rid_p) {
            free(buf);
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
        const char* ts_p = Bandit_JsonFindKey(rid_p, "total_steps");
        if (ts_p) b.total_steps = (int)strtol(ts_p, NULL, 10);

        // weights
        const char* w_p = Bandit_JsonFindKey(rid_p, "weights");
        if (w_p) {
            double tmp_w[BANDIT_MAX_ARMS];
            int got = Bandit_JsonParseDoubleArray(w_p, tmp_w, BANDIT_MAX_ARMS);
            int copy = (got < b.n_arms) ? got : b.n_arms;
            for (int a = 0; a < copy; ++a) b.weights[a] = tmp_w[a];
        }

        // cum_reward
        const char* cr_p = Bandit_JsonFindKey(rid_p, "cum_reward");
        if (cr_p) {
            double tmp_cr[BANDIT_MAX_ARMS];
            int got = Bandit_JsonParseDoubleArray(cr_p, tmp_cr, BANDIT_MAX_ARMS);
            int copy = (got < b.n_arms) ? got : b.n_arms;
            for (int a = 0; a < copy; ++a) b.cum_reward[a] = tmp_cr[a];
        }

        // pulls
        const char* pl_p = Bandit_JsonFindKey(rid_p, "pulls");
        if (pl_p) {
            int tmp_p[BANDIT_MAX_ARMS];
            int got = Bandit_JsonParseIntArray(pl_p, tmp_p, BANDIT_MAX_ARMS);
            int copy = (got < b.n_arms) ? got : b.n_arms;
            for (int a = 0; a < copy; ++a) b.pulls[a] = tmp_p[a];
        }

        // Advance p past this entry's closing brace
        const char* end_brace = strchr(rid_p, '}');
        p = end_brace ? end_brace + 1 : rid_p + 1;
    }

    free(buf);
    return 1;
}

#endif // BANDIT_LEARNING_HPP
