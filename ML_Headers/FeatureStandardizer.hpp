// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FEATURE STANDARDIZER — v5.9.3a]
//======================================================================================================
// Mean-centering + unit-variance scaling for ML features. Applied between
// Features_PackAll and Model_Predict on the slow path.
//
// Why double, not FPN<F>:
//   Training-side reference is Python/numpy/XGBoost which compute mean +
//   stddev in IEEE-754 double. FPN math produces subtly different bits
//   for the same inputs. Using FPN at apply-time vs double at compute-time
//   = guaranteed bytewise mismatch. Standardizer runs on the slow path
//   (between PackAll + Predict, ~once per slow-path cycle), so the
//   FPN/branchless rule that protects the hot path doesn't apply.
//   See DOCS/CLAUDE_ML_INVARIANTS.md "Train-serve scaler parity" rule.
//
// Sidecar binary format (`.scaler` next to the model `.bin`):
//   [u32 magic = 0xFE5C1AE2]
//   [u32 num_features]                  // must equal NUM_REGISTERED_FEATURES at load
//   [u64 feature_registry_hash]         // must equal FEATURE_REGISTRY_HASH() at load
//   [u32 stddev_floor_q]                // Q32 fixed-point (1e-9 default)
//   [double mean[N]]
//   [double stddev[N]]
//   [u8 sha256[32] of body up to here]
//
// Two-layer registry binding:
//   1. Stamp body has scaler_sha256 (sidecar's full-file SHA)
//   2. Sidecar embeds feature_registry_hash + num_features
//   Both must match build's FEATURE_REGISTRY_HASH() / NUM_REGISTERED_FEATURES
//   at load. Drift is impossible to ship without explicit retrain.
//
// Status v5.9.3a: scaler infrastructure ships DISABLED. Compute/Persist
// (training-side) + apply-site callers land in v5.9.3b. Load + verify
// path is the v5.9.3a deliverable. v5.9.2 parity regression test
// passes unchanged by construction (no caller invokes Apply).
//
// FUTURE HOOKS (post-v5.9.3):
//   - per-feature feature flags (enable scaling per FeatureId)
//   - robust scaling alternative (median + MAD instead of mean + stddev)
//   - online standardization (rolling stats; streaming updates)
//======================================================================================================
#ifndef FEATURE_STANDARDIZER_HPP
#define FEATURE_STANDARDIZER_HPP

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>      // std::fmax (branchless on x86 maxsd), std::isfinite
#include <unistd.h>    // unlink/rename for atomic write
#include <algorithm>   // v5.14.1.D — std::sort for FitWinsor percentile pick
#include "FeatureRegistry.hpp"   // NUM_REGISTERED_FEATURES, FEATURE_REGISTRY_HASH
#include "../MemHeaders/HmacSha256.hpp"  // sha256_file_hex_inproc + sha256 bytes helpers

namespace tt {

//======================================================================================================
// [SIDECAR FORMAT CONSTANTS]
//======================================================================================================
// Magic number identifies a v5.9.3+ scaler binary. Distinct value chosen so
// random file headers don't accidentally validate as scaler files.
// v5.14.1.D — magic bumped from 0xFE5C1AE2 (v0; pre-winsor) to 0xFE5C1AE3
// (v1; winsor-aware). Surface G discipline at the sidecar level: legacy
// sidecars refused at load with operator-readable error directing them to
// regenerate via Run Full Validation. Operator regenerates once.
static constexpr uint32_t SCALER_MAGIC_V0 = 0xFE5C1AE2u;  // pre-v5.14.1.D
static constexpr uint32_t SCALER_MAGIC    = 0xFE5C1AE3u;  // v5.14.1.D+ (winsor)

// Scaler sidecar body layout — single source of truth.
//
// On-disk file: [body || sha256(body)]
//   - body: the fields below, packed in this exact order
//   - sha256: 32 bytes appended after the body, verified at Load
//
// Body fields (in serialization order):
//   magic         [4 bytes, uint32_t]  — SCALER_MAGIC, identifies file type
//   num_features  [4 bytes, uint32_t]  — must equal NUM_REGISTERED_FEATURES
//                                        (refused at load if mismatched)
//   registry_hash [8 bytes, uint64_t]  — FOREACH_FEATURE compile-time hash;
//                                        binds scaler to feature registry
//                                        (v5.9.3a; refused on cross-build
//                                        mismatch in strict mode)
//   stddev_floor  [4 bytes, uint32_t]  — Q32-encoded floor; apply uses
//                                        fmax(stddev, floor) to avoid
//                                        div-by-zero on constant features
//   mean[N]       [8N bytes, double*N] — per-feature mean
//   stddev[N]     [8N bytes, double*N] — per-feature stddev
//
// Total = 20 + 16*NUM_REGISTERED_FEATURES bytes (default N=34 → 564 bytes).
//
// Both Persist (write) and Load (verify-SHA) build the body from these
// fields. SCALER_BODY_BYTES is used to size the in-memory staging buffer
// in both paths — they must stay byte-aligned with the writes below.
//
// v5.9.5a: extracted as named constant after v5.9.3a's `registry_hash`
// addition silently broke the manual `8 + 4 + 8N*2` formula (was off-by-8;
// caused stack overflow caught by -Wstringop-overflow at v5.9.5 sprint
// exit). Using sizeof() for each field is type-safe — changing a field's
// type updates this constant automatically; adding a new field requires
// editing this list (forces conscious update of both write paths).
static constexpr size_t SCALER_BODY_BYTES =
    sizeof(uint32_t)                              // magic
  + sizeof(uint32_t)                              // num_features
  + sizeof(uint64_t)                              // registry_hash
  + sizeof(uint32_t)                              // stddev_floor (Q32)
  + sizeof(double) * NUM_REGISTERED_FEATURES      // mean[N]
  + sizeof(double) * NUM_REGISTERED_FEATURES      // stddev[N]
  // v5.14.1.D additions:
  + sizeof(uint8_t)                               // has_winsor_bounds
  + sizeof(double) * NUM_REGISTERED_FEATURES      // winsor_low[N]
  + sizeof(double) * NUM_REGISTERED_FEATURES;     // winsor_high[N]

// Default stddev floor. For constant features (stddev=0) or near-constant
// (stddev < floor), apply uses fmax(stddev, floor) to avoid div-by-zero
// or pathological magnification. Persisted in sidecar as Q32 fixed-point
// (Decision: persist, not just constexpr — future changes to constexpr
// don't silently change behavior for existing models).
static constexpr double SCALER_STDDEV_FLOOR = 1e-9;

// Q32 encode/decode helpers. stddev_floor_q = (uint32_t)(floor * 2^32).
// For 1e-9: 1e-9 × 4294967296 ≈ 4.295 → rounds to 4. Q32 resolution =
// 2^-32 ≈ 2.33e-10, fine-grained enough for the 1e-9 floor.
static inline uint32_t scaler_floor_to_q32(double f) {
    if (f <= 0.0) return 0;
    return (uint32_t)(f * 4294967296.0);  // 2^32
}
static inline double scaler_q32_to_floor(uint32_t q) {
    return (double)q / 4294967296.0;
}

//======================================================================================================
// [FEATURE STANDARDIZER STRUCT]
//======================================================================================================
// Inline struct (no malloc) per v5.9.3a audit decision. NUM_REGISTERED_FEATURES
// is constexpr → struct size known at compile time. ~600 bytes per
// instance for current 34 features. Lives inline on each ModelHandle.
//
// has_scaler: 0 by default (no scaler loaded). Set to 1 after successful
// FeatureStandardizer_Load. Apply path early-returns when 0 (identity).
//======================================================================================================
struct FeatureStandardizer {
    int      has_scaler;           // 0 = identity (no scaler loaded), 1 = active
    uint64_t registry_hash;        // FEATURE_REGISTRY_HASH() at training time
    uint32_t num_features;         // NUM_REGISTERED_FEATURES at training time
    double   stddev_floor;         // typically SCALER_STDDEV_FLOOR (1e-9)
    double   mean[NUM_REGISTERED_FEATURES];
    double   stddev[NUM_REGISTERED_FEATURES];
    // v5.14.1.D — feature winsorization bounds. Per-feature percentile
    // clips applied in Apply BEFORE mean-center + unit-var. Reduces
    // noise from 5σ outliers (flash crashes, exchange glitches). Each
    // ModelHandle has its own scaler → heterogeneous winsor models
    // supported (3 winsor variants per role across 3 cores, blended
    // via bandit/Ridge per existing ensemble infrastructure).
    //
    // has_winsor_bounds=0 default + winsor_low=-INFINITY +
    // winsor_high=+INFINITY → fmin/fmax pass-through, zero observable
    // cost vs no-winsor case. Init populates these no-op defaults.
    uint8_t  has_winsor_bounds;
    double   winsor_low[NUM_REGISTERED_FEATURES];
    double   winsor_high[NUM_REGISTERED_FEATURES];
};

//======================================================================================================
// [INIT]
//======================================================================================================
// Zero-init the struct. has_scaler=0 means "identity applied" — caller
// can safely use the struct without explicit init, but explicit init is
// recommended for clarity.
static inline void FeatureStandardizer_Init(FeatureStandardizer* sc) {
    if (!sc) return;
    sc->has_scaler = 0;
    sc->registry_hash = 0;
    sc->num_features = 0;
    sc->stddev_floor = SCALER_STDDEV_FLOOR;
    for (unsigned i = 0; i < NUM_REGISTERED_FEATURES; ++i) {
        sc->mean[i] = 0.0;
        sc->stddev[i] = 1.0;  // neutral default — apply produces identity
    }
    // v5.14.1.D — winsor defaults: bounds = ±INFINITY → fmin/fmax pass-
    // through (zero observable cost when has_winsor_bounds=0). Apply
    // works correctly even if has_winsor_bounds gets set to 1 with these
    // defaults (still a no-op clip; both clipped values pass through any
    // finite input).
    sc->has_winsor_bounds = 0;
    for (unsigned i = 0; i < NUM_REGISTERED_FEATURES; ++i) {
        sc->winsor_low[i]  = -INFINITY;
        sc->winsor_high[i] = +INFINITY;
    }
}

//======================================================================================================
// [APPLY] — slow-path, between Features_PackAll and Model_Predict
//======================================================================================================
// Standardize features in-place. Reads from + writes to the same float[]
// buffer. Math: out[i] = ((double)in[i] - mean[i]) / fmax(stddev[i], floor);
// cast back to float at the boundary.
//
// When has_scaler=0: returns 0 immediately (identity, no work).
// When has_scaler=1: applies standardization + post-apply finite check
// (NaN/Inf in output sets sentinel=-1, caller skips the prediction
// cycle, v5.9.0 NaN-guard pattern).
//
// Returns: 0 = OK / no-op, -1 = output produced NaN/Inf (caller skips
// prediction). Pre-apply NaN check is Features_PackAll's job (v5.9.0).
//
// V5.9.3a STATUS: function defined, NO CALLERS YET. v5.9.3b adds the
// 5 Model_Predict-adjacent call sites (StrategyParameters.hpp:723/751,
// MLStrategy.hpp:129, PortfolioController.hpp:1639/1806).
//======================================================================================================
static inline int FeatureStandardizer_Apply(const FeatureStandardizer* sc,
                                              float* features, int n) {
    if (!sc || !sc->has_scaler) return 0;     // identity (no work)
    if (n <= 0 || (unsigned)n > NUM_REGISTERED_FEATURES) return -1;

    double floor = (sc->stddev_floor > 0.0) ? sc->stddev_floor : SCALER_STDDEV_FLOOR;

    for (int i = 0; i < n; ++i) {
        double in   = (double)features[i];
        // v5.14.1.D — branchless winsor clip BEFORE scale.
        // bounds = [-INFINITY, +INFINITY] from Init → fmin/fmax pass-through
        // (zero observable cost when has_winsor_bounds=0). When training-time
        // RFV has fit per-feature percentile bounds, in is clipped to the
        // [low, high] interval, taming outliers BEFORE mean-center + scale.
        // Compiles to maxsd + minsd on x86 (2 instructions, branchless).
        double clipped = fmin(fmax(in, sc->winsor_low[i]), sc->winsor_high[i]);
        double sd   = fmax(sc->stddev[i], floor);
        double diff = clipped - sc->mean[i];
        double out  = diff / sd;
        features[i] = (float)out;
    }
    // Post-apply finite check. Output COULD have non-finite values if
    // stddev was floored and mean was at FPN saturation (rare edge).
    // Pre-apply check at Features_PackAll already filters input NaN; this
    // catches anything introduced by the apply math itself.
    for (int i = 0; i < n; ++i) {
        if (!isfinite(features[i])) return -1;
    }
    return 0;
}

//======================================================================================================
// [LOAD] — engine boot, called from CoreModelZoo_TryLoadRole
//======================================================================================================
// Read the .scaler sidecar binary. Validates magic, num_features, and
// the body's embedded sha256. Caller (CoreModelZoo) compares the FULL
// file's SHA against stamp's scaler_sha256 separately (using
// sha256_file_hex_inproc). After Load returns 1, caller sets
// has_scaler=1 if all upstream checks (registry_hash match) pass.
//
// Returns: 1 = success, 0 = file missing, -1 = corrupt / mismatched
// magic / num_features / embedded SHA.
//======================================================================================================
static inline int FeatureStandardizer_Load(FeatureStandardizer* sc,
                                             const char* sidecar_path) {
    if (!sc || !sidecar_path) return -1;
    FeatureStandardizer_Init(sc);

    FILE* f = fopen(sidecar_path, "rb");
    if (!f) return 0;

    uint32_t magic = 0;
    if (fread(&magic, 4, 1, f) != 1 || magic != SCALER_MAGIC) {
        if (magic == SCALER_MAGIC_V0) {
            // v5.14.1.D — clean break with pre-winsor format. Operator
            // sees a clear actionable message instead of a generic mismatch.
            fprintf(stderr,
                "[scaler] %s: pre-v5.14.1.D format (magic=0x%08x); "
                "regenerate via Run Full Validation to upgrade to "
                "winsor-aware format (magic=0x%08x)\n",
                sidecar_path, magic, SCALER_MAGIC);
        } else {
            fprintf(stderr, "[scaler] %s: magic mismatch (got 0x%08x, expect 0x%08x)\n",
                    sidecar_path, magic, SCALER_MAGIC);
        }
        fclose(f);
        return -1;
    }
    uint32_t nfeat = 0;
    if (fread(&nfeat, 4, 1, f) != 1 ||
        nfeat != (uint32_t)NUM_REGISTERED_FEATURES) {
        fprintf(stderr, "[scaler] %s: num_features mismatch (sidecar=%u, build=%u)\n",
                sidecar_path, nfeat, (unsigned)NUM_REGISTERED_FEATURES);
        fclose(f);
        return -1;
    }
    if (fread(&sc->registry_hash, 8, 1, f) != 1) { fclose(f); return -1; }
    uint32_t floor_q = 0;
    if (fread(&floor_q, 4, 1, f) != 1) { fclose(f); return -1; }
    sc->num_features = nfeat;
    sc->stddev_floor = scaler_q32_to_floor(floor_q);
    if (sc->stddev_floor <= 0.0) sc->stddev_floor = SCALER_STDDEV_FLOOR;

    // Body SHA-256 covers magic + num_features + registry_hash + floor +
    // mean + stddev + (v5.14.1.D) has_winsor_bounds + winsor_low + winsor_high.
    // Computed against the read bytes; compared to the trailing sha256[32].
    if (fread(sc->mean,   sizeof(double), NUM_REGISTERED_FEATURES, f)
            != NUM_REGISTERED_FEATURES) { fclose(f); return -1; }
    if (fread(sc->stddev, sizeof(double), NUM_REGISTERED_FEATURES, f)
            != NUM_REGISTERED_FEATURES) { fclose(f); return -1; }
    // v5.14.1.D — winsor block
    uint8_t hwb_read = 0;
    if (fread(&hwb_read, 1, 1, f) != 1) { fclose(f); return -1; }
    sc->has_winsor_bounds = hwb_read;
    if (fread(sc->winsor_low,  sizeof(double), NUM_REGISTERED_FEATURES, f)
            != NUM_REGISTERED_FEATURES) { fclose(f); return -1; }
    if (fread(sc->winsor_high, sizeof(double), NUM_REGISTERED_FEATURES, f)
            != NUM_REGISTERED_FEATURES) { fclose(f); return -1; }
    uint8_t embedded_sha[32];
    if (fread(embedded_sha, 1, 32, f) != 32) { fclose(f); return -1; }
    fclose(f);

    // Recompute body SHA-256 from the in-memory state we just loaded.
    // (Body = bytes BEFORE the trailing sha256.) This catches any
    // mid-file tamper that didn't affect the trailing field.
    // Buffer size from SCALER_BODY_BYTES — see field layout there.
    uint8_t body_buf[SCALER_BODY_BYTES];
    size_t off = 0;
    uint32_t magic_w = SCALER_MAGIC;       memcpy(body_buf + off, &magic_w, 4); off += 4;
    uint32_t nf_w    = nfeat;              memcpy(body_buf + off, &nf_w,    4); off += 4;
    uint64_t hash_w  = sc->registry_hash;  memcpy(body_buf + off, &hash_w,  8); off += 8;
    uint32_t fq_w    = floor_q;            memcpy(body_buf + off, &fq_w,    4); off += 4;
    memcpy(body_buf + off, sc->mean,   sizeof(double) * NUM_REGISTERED_FEATURES);
    off += sizeof(double) * NUM_REGISTERED_FEATURES;
    memcpy(body_buf + off, sc->stddev, sizeof(double) * NUM_REGISTERED_FEATURES);
    off += sizeof(double) * NUM_REGISTERED_FEATURES;
    // v5.14.1.D — winsor block in SHA-recompute body
    uint8_t hwb_w = sc->has_winsor_bounds; memcpy(body_buf + off, &hwb_w, 1); off += 1;
    memcpy(body_buf + off, sc->winsor_low,  sizeof(double) * NUM_REGISTERED_FEATURES);
    off += sizeof(double) * NUM_REGISTERED_FEATURES;
    memcpy(body_buf + off, sc->winsor_high, sizeof(double) * NUM_REGISTERED_FEATURES);
    off += sizeof(double) * NUM_REGISTERED_FEATURES;
    uint8_t computed_sha[32];
    if (!tt::sha256_bytes(body_buf, off, computed_sha)) {
        fprintf(stderr, "[scaler] %s: SHA-256 compute failed\n", sidecar_path);
        return -1;
    }
    if (memcmp(computed_sha, embedded_sha, 32) != 0) {
        fprintf(stderr, "[scaler] %s: body SHA-256 mismatch (corrupted file)\n",
                sidecar_path);
        return -1;
    }

    sc->has_scaler = 1;
    return 1;
}

//======================================================================================================
// [VERIFY] — caller checks the loaded scaler against current build's invariants
//======================================================================================================
// Returns 1 if scaler matches build's FEATURE_REGISTRY_HASH() and
// num_features. Returns 0 on mismatch. Caller decides refusal vs warn
// per held_out_gate_strict.
static inline int FeatureStandardizer_VerifyAgainstBuild(const FeatureStandardizer* sc) {
    if (!sc || !sc->has_scaler) return 0;
    if (sc->num_features != (uint32_t)NUM_REGISTERED_FEATURES) return 0;
    if (sc->registry_hash != FEATURE_REGISTRY_HASH()) return 0;
    return 1;
}

//======================================================================================================
// [COMPUTE + PERSIST] — training-side, v5.9.3b will call from Backtest_TrainModel
//======================================================================================================
// Compute mean + stddev across the training set. Welford-style accumulator
// (numerically stable). Reads feature_matrix as float (per Features_PackAll
// output type), accumulates in double.
//
// V5.9.3a STATUS: function defined, NO CALLERS YET. v5.9.3b will integrate
// from Backtest_TrainModel.

//======================================================================================================
// [FIT_WINSOR_PERCENTILES — v5.14.1.D]
//======================================================================================================
// Compute per-feature winsor bounds from training data using cfg-tunable
// percentiles. For each feature column: gather values, sort, take
// floor(pct_low * N) and floor(pct_high * N) indices → winsor_low[i] /
// winsor_high[i].
//
// Caller responsibility: invoke AFTER FeatureStandardizer_Compute (which
// populates mean/stddev). Sets sc->has_winsor_bounds = 1 on success.
//
// Sentinel: if pct_low <= 0 OR pct_high >= 1.0 OR pct_low >= pct_high,
// no fit; bounds stay at ±INFINITY (Init defaults). Operator setting
// cfg.winsor_pct_low=0 + cfg.winsor_pct_high=1 effectively disables.
//
// Memory: stack-allocates a per-feature vector of doubles (1 column at
// a time → O(num_samples * sizeof(double)) per call). For N=2048 training
// samples = 16KB stack, well within ulimit. For larger N, caller scopes
// this to its own buffer (future extension if needed).
//
// Latency: O(N log N) per feature × NUM_REGISTERED_FEATURES; one-time
// training-side cost. Slow-path slow.
//======================================================================================================
static inline void FeatureStandardizer_FitWinsor(FeatureStandardizer* sc,
                                                   const float* feature_matrix,
                                                   int num_samples,
                                                   double pct_low,
                                                   double pct_high) {
    if (!sc) return;
    if (num_samples <= 0) return;
    // Sentinel: invalid percentiles → leave bounds at ±INFINITY (Init defaults)
    if (pct_low <= 0.0 || pct_high >= 1.0 || pct_low >= pct_high) {
        sc->has_winsor_bounds = 0;
        return;
    }
    // Per-feature column extraction + sort + percentile pick.
    // Stack alloc one column at a time (avoids num_features × num_samples
    // matrix transpose).
    double col[8192];  // hard cap; larger N → fall back to identity (no fit)
    if (num_samples > 8192) {
        sc->has_winsor_bounds = 0;
        return;
    }
    for (unsigned f = 0; f < NUM_REGISTERED_FEATURES; ++f) {
        for (int s = 0; s < num_samples; ++s) {
            // feature_matrix is row-major: matrix[s * NUM_FEATURES + f]
            col[s] = (double)feature_matrix[s * NUM_REGISTERED_FEATURES + f];
        }
        // std::sort in place; ascending. Mid-range index for percentile.
        std::sort(col, col + num_samples);
        int idx_low  = (int)(pct_low  * (double)num_samples);
        int idx_high = (int)(pct_high * (double)num_samples);
        if (idx_low  < 0)              idx_low  = 0;
        if (idx_high >= num_samples)   idx_high = num_samples - 1;
        if (idx_low  > idx_high)       idx_low  = idx_high;
        sc->winsor_low[f]  = col[idx_low];
        sc->winsor_high[f] = col[idx_high];
    }
    sc->has_winsor_bounds = 1;
}

static inline void FeatureStandardizer_Compute(FeatureStandardizer* sc,
                                                 const float* feature_matrix,
                                                 int num_samples,
                                                 double stddev_floor) {
    if (!sc) return;
    FeatureStandardizer_Init(sc);
    sc->num_features = NUM_REGISTERED_FEATURES;
    sc->registry_hash = FEATURE_REGISTRY_HASH();
    sc->stddev_floor = (stddev_floor > 0.0) ? stddev_floor : SCALER_STDDEV_FLOOR;
    if (num_samples <= 0) {
        sc->has_scaler = 1;  // valid struct, identity-equivalent (mean=0, stddev=1)
        return;
    }

    // Two-pass: simpler than Welford, sample-counts are bounded and double
    // precision is sufficient for typical feature ranges. Pass 1 = mean.
    for (unsigned f = 0; f < NUM_REGISTERED_FEATURES; ++f) {
        double sum = 0.0;
        for (int s = 0; s < num_samples; ++s) {
            sum += (double)feature_matrix[s * NUM_REGISTERED_FEATURES + f];
        }
        sc->mean[f] = sum / (double)num_samples;
    }
    // Pass 2 = stddev (sample stddev; n-1 denominator).
    if (num_samples > 1) {
        for (unsigned f = 0; f < NUM_REGISTERED_FEATURES; ++f) {
            double sum_sq = 0.0;
            for (int s = 0; s < num_samples; ++s) {
                double d = (double)feature_matrix[s * NUM_REGISTERED_FEATURES + f]
                         - sc->mean[f];
                sum_sq += d * d;
            }
            sc->stddev[f] = sqrt(sum_sq / (double)(num_samples - 1));
        }
    }
    sc->has_scaler = 1;
}

// Persist to sidecar via atomic write (.tmp + rename).
// Returns 1 on success, 0 on I/O failure.
static inline int FeatureStandardizer_Persist(const FeatureStandardizer* sc,
                                                const char* sidecar_path) {
    if (!sc || !sidecar_path || !sc->has_scaler) return 0;

    char tmp_path[600];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", sidecar_path);

    FILE* f = fopen(tmp_path, "wb");
    if (!f) return 0;

    // Build body in memory, compute SHA, then write body + SHA.
    // Buffer size from SCALER_BODY_BYTES — see field layout there.
    uint8_t body[SCALER_BODY_BYTES];
    size_t off = 0;
    uint32_t magic = SCALER_MAGIC;            memcpy(body + off, &magic, 4); off += 4;
    uint32_t nfeat = sc->num_features;        memcpy(body + off, &nfeat, 4); off += 4;
    uint64_t hash  = sc->registry_hash;       memcpy(body + off, &hash,  8); off += 8;
    uint32_t fq    = scaler_floor_to_q32(sc->stddev_floor);
                                              memcpy(body + off, &fq,    4); off += 4;
    memcpy(body + off, sc->mean,
           sizeof(double) * NUM_REGISTERED_FEATURES);
    off += sizeof(double) * NUM_REGISTERED_FEATURES;
    memcpy(body + off, sc->stddev,
           sizeof(double) * NUM_REGISTERED_FEATURES);
    off += sizeof(double) * NUM_REGISTERED_FEATURES;

    // v5.14.1.D — winsor block (canonical body position 7+).
    uint8_t hwb = sc->has_winsor_bounds;     memcpy(body + off, &hwb, 1); off += 1;
    memcpy(body + off, sc->winsor_low,
           sizeof(double) * NUM_REGISTERED_FEATURES);
    off += sizeof(double) * NUM_REGISTERED_FEATURES;
    memcpy(body + off, sc->winsor_high,
           sizeof(double) * NUM_REGISTERED_FEATURES);
    off += sizeof(double) * NUM_REGISTERED_FEATURES;

    uint8_t sha[32];
    if (!tt::sha256_bytes(body, off, sha)) {
        fclose(f);
        unlink(tmp_path);
        return 0;
    }

    if (fwrite(body, 1, off, f) != off ||
        fwrite(sha, 1, 32, f) != 32) {
        fclose(f);
        unlink(tmp_path);
        return 0;
    }
    fclose(f);

    if (rename(tmp_path, sidecar_path) != 0) {
        unlink(tmp_path);
        return 0;
    }
    return 1;
}

//======================================================================================================
// [FREE]
//======================================================================================================
// Inline struct — no heap allocation. Free is a logical no-op (just zeros
// the struct so future use re-initializes).
static inline void FeatureStandardizer_Free(FeatureStandardizer* sc) {
    if (!sc) return;
    FeatureStandardizer_Init(sc);
}

}  // namespace tt

#endif  // FEATURE_STANDARDIZER_HPP
