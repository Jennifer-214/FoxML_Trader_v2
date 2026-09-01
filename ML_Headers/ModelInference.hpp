// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the MIT License. See LICENSE file for details.

//======================================================================================================
// [FILE]_[ML_Headers/ModelInference.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [SLOW_PATH]]
// [SEAM]_[train-serve model + stamp contract — FEAT_* indices, MODEL_FORMAT_VERSION, and the HMAC-signed .stamp body are the identity the trainer emits and the engine verifies at load]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[XGBoost/LightGBM single-row inference + the H9 stamp emit/parse heart — ModelHandle lifecycle, ensemble predict paths, and the verify_model_stamp / stamp_write_for_model wire pair]
// [CONTAINS]
//   - [STRUCT]_[ModelHandle]              (bit-packed has_flags + X-macro stamp-derived fields; 5 cluster bands)
//   - [FUNCTION]_[Model_Init] / [Model_Load] / [Model_Free] / [Model_IsLoaded]   (handle lifecycle)
//   - [FUNCTION]_[Model_Predict] (+ _Normalized / _AtClass / _AOT stubs / _Ensemble / _Ensemble_Weighted / _PredictMulti)
//   - [FUNCTION]_[FeatureLookback_Max]    (+ CountEnabled; over the FEATURE_LOOKBACKS temporal-reach table)
//   - [FUNCTION]_[ModelFeatures_Pack]     (DEPRECATED frozen packer — equivalence-test reference only)
//   - [STRUCT]_[ModelStampResult] / [STRUCT]_[StampInferenceCfgInputs]   (X-macro-generated stamp parse/emit sides)
//   - [FUNCTION]_[sha256_file_hex]        (+ stamp_parse_line — verify helpers)
//   - [FUNCTION]_[verify_model_stamp]     (the stamp INGEST gate — H9 HMAC verify + drift checks)
//   - [FUNCTION]_[stamp_write_for_model]  (the stamp EMIT side — canonical body + HMAC sign + atomic write)
// [REFERENCE]_[INVARIANT]_[[H9] [H21]]
// [REFERENCE]_[DESIGN_SPEC]_[wire-format-byte-preservation-discipline]
//======================================================================================================
// thin C-style abstraction over XGBoost and LightGBM C APIs for single-row inference.
// compiles to complete no-ops when neither backend is enabled (zero overhead).
// both APIs take float* row vectors. Single-row inference cost is LINEAR in
// total tree count: ~550ns/round (3-class) + ~10µs fixed (measured 2026-08-22;
// 1050 trees ≈ 217µs) — the old "~1-5µs" held only for small binary models.
//
// usage:
//   ModelHandle<F> model;
//   Model_Init(&model);
//   Model_Load(&model, "model.xgb", MODEL_BACKEND_XGBOOST);
//   float features[16]; int n = ModelFeatures_Pack(features, &signals, &rolling, rolling_long);
//   float prediction = Model_Predict(&model, features, n);
//   Model_Free(&model);
//======================================================================================================
#ifndef MODEL_INFERENCE_HPP
#define MODEL_INFERENCE_HPP

#include <cstdint>  // v5.15.5.F.4d TECH_DEBT-083 close — explicit IWYU for uintN_t (was transitively pulled)
#include "../FixedPoint/FixedPointN.hpp"
#include "../ML_Headers/RollingStats.hpp"
#include "../MemHeaders/HmacSha256.hpp"  // v5.3.0 Phase B — in-process HMAC + SHA-256 (replaces popen paths)
#include "../Version.hpp"                 // v5.9.2b — ENGINE_VERSION_STRING for cross-major detection
#include <x86intrin.h>                 // E.1.2.E leaf 7 — __rdtsc for the ML_PREDICT accumulator.
                                       // EXPLICIT: it compiled in-tree only because a sibling header
                                       // pulled it in transitively; the B-Plus snippet compiler caught
                                       // it standalone ("__rdtsc was not declared"). Same latent-IWYU
                                       // class as the FailureModeRegistry include below (sister to
                                       // EngineCommon.hpp:96, which includes it for the same reason).
#include "TreeWalker.hpp"                // E.1.2.E — MODEL_BACKEND_FLAT_WALKER blob + walk/transform
#include "../MemHeaders/FailureModeRegistry.hpp"  // E.1.2.E — FAILURE_MASK_ml_walker_parity_failed
                                                  // (explicit IWYU: the walker branches live under
                                                  //  USE_XGBOOST, so an ANSI build compiled clean while
                                                  //  the GUI/suite lanes did not — a transitive include
                                                  //  would leave that asymmetry latent)
#include "FeatureRegistry.hpp"          // E.1.2.G — NUM_REGISTERED_FEATURES: the feature-count SSoT
#include "FeatureStandardizer.hpp"       // v5.9.3a — inline scaler struct on ModelHandle
#include "../CoreFrameworks/ParseFast.hpp"  // v5.11.4.C — std::from_chars wrapper (locale immunity)
// v5.15.5.F.4d.1.B.3 Step 2 (2026-05-24): #include "StampBoundCfgRegistry.hpp" REMOVED — file deleted (FOREACH_STAMP_BOUND_CFG body + STAMP_CFG_AUTOPOPULATE + COUNT macros); cfg_derived::populate_stamp_cfg_from_derived<F> framework call at CfgGateRegistry.hpp supersedes.
#include "StampBoundModelConstRegistry.hpp"  // v5.14.8.0+ — FOREACH_STAMP_BOUND_MODEL_CONST X-macro (registry, MASK constants, STAMP_HAS aliases, AUTOPOPULATE)
#include "../MemHeaders/CfgGateRegistry.hpp"  // v5.15.5.F.4d.1.B.3 Step 1.6.3 — STAMP_RESULT_DERIVED_FIELDS_AUTO_GEN macro + PARSE_STAMP_CFG_TO_DERIVED + cfg_derived::parse_stamp_cfg_to_derived template fn
#include <stdio.h>
#include <string.h>
#include <locale.h>                       // v5.3.0 Phase B — uselocale for canonical body LC_NUMERIC pinning
#include <unistd.h>                       // v5.3.0 Phase B — unlink/rename for atomic stamp writes

//======================================================================
// [SECTION]_[ML_PREDICT attribution accumulators — E.1.2.E leaf 7]
//----------------------------------------------------------------------
// SP_SECTION_ML_INFER brackets the ENTIRE strategy dispatch, not inference:
// feature packing, scaler apply, buy ensemble, exit ensemble, barrier
// multi-class, confidence scoring and bandit update all sit inside it. So the
// live 3306.7us reading (2026-08-24) says "the ML dispatch is expensive" and
// says NOTHING about how much of that the walker can remove. That ambiguity is
// TECH_DEBT-292's composite-bracket problem one level down, and its fix is the
// same: SPLIT THE BRACKET, THEN decide.
//
// WHY A THREAD-LOCAL ACCUMULATOR AND NOT PLUMBED FIELDS: the predict sites are
// PLURAL and spread across files (Strategies/MLStrategy.hpp, four sites in
// Strategies/StrategyParameters.hpp), and which of them fire depends on the
// live arms that cycle. A plumbed counter measures exactly the sites I
// remembered to bracket — and an enumeration I was confident about is precisely
// what was wrong earlier in this same ship (the "24 predicts" projection).
// Accumulating INSIDE the three inference entry points is complete BY
// CONSTRUCTION: every ensemble/weighted/composite path routes through them, so
// a predict site nobody enumerated still lands in the total.
//
// Single-writer by construction: each node's slow path is its own thread and
// these are thread_local, so there is no cross-thread contention and no
// alignas(64) obligation (H6 is about SHARED cross-thread fields).
//
// `inline` (C++17 inline variable) NOT `static` — `static thread_local` in a
// header mints a SEPARATE copy per TU, so the accumulation would silently split
// across translation units and under-report.
//
// COST: two rdtsc per predict against a call measured at 139us. The sibling
// ML_INFER bracket is likewise always-on (not LATENCY_PROFILING-gated), so this
// matches its cost profile rather than introducing a new one.
namespace tt {
inline thread_local uint64_t ml_predict_cycles = 0;   // summed rdtsc across predicts this dispatch
inline thread_local uint32_t ml_predict_count  = 0;   // how many predicts this dispatch

//======================================================================
// [STRUCT]_[MlPredictTimer]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [SLOW_PATH]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[stack-lifetime rdtsc scope guard — accumulates one predict's
//   cycles + bumps the count into the thread_local ML_PREDICT accumulators on
//   destruction. Constructed at each inference entry point; never heap, never
//   shared, never a resource owner.]
//======================================================================
// WHY A SCOPE GUARD AND NOT MANUAL BRACKETING: each predict entry point has
// several early returns (null handle, backend miss, library error, NaN
// rejection). Manual `t1 - t0` at the end silently misses every one of them,
// and a measurement that under-reports on exactly the failure paths is worse
// than no measurement — it would make a cycle that failed inference look cheap.
//
// WHY A DESTRUCTOR AT ALL, given the file's C-style/no-classes convention: the
// convention carves out RAII destructors, and this is that carve-out applied to
// the correctness of the measurement rather than to a resource. It owns
// nothing; its whole body is two adds on thread_local scalars.
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-08-25]
// [ALIGN]_[8]
// [CACHE_LINES]_[1]
// [STRADDLE]_[none]
//----------------------------------------------------------------------
// [SIZE]_[8B]
//======================================================================
// [CODE]
//======================================================================
struct MlPredictTimer {
    uint64_t t0;
    MlPredictTimer() : t0(__rdtsc()) {}
    ~MlPredictTimer() { ml_predict_cycles += (__rdtsc() - t0); ++ml_predict_count; }
};
// [END_CODE]
// [END_STRUCT]_[MlPredictTimer]
//======================================================================
}  // namespace tt
//======================================================================

// backend IDs
#define MODEL_BACKEND_NONE     0
#define MODEL_BACKEND_XGBOOST  1
#define MODEL_BACKEND_LIGHTGBM 2
// ---------------------------------------------------------------------
// SLOT 3 — RETIRED 2026-08-24 (E.1.2.E). Was MODEL_BACKEND_AOT (Treelite
// ahead-of-time compiled trees). H21 TOMBSTONE: the NUMBER stays reserved
// forever, never reassigned to a new backend — an un-updated node or an old
// persisted `ml_backend=3` must not silently resolve to something else
// (the Knight-Capital shape).
//
// It was never a backend, only the SHAPE of one. Every leg was measured
// ABSENT at retirement: `tools/aot_compile_model.sh` (step 2 of the operator
// workflow this comment used to teach) DOES NOT EXIST; the stamp fields it
// promised were never built; `use_aot_inference` was a Class-44 cfg orphan
// (parsed, never read — zero production readers tree-wide); and
// `Model_LoadAOT` / `Model_Predict_AOT` were always-fail stubs with ZERO
// callers. The dead CODE is deleted per H21 ("remove dead code, don't leave
// it compiled-in"); only the reserved number survives, and the cfg key is
// burned in check_identifier_retirement.py's RETIRED_NAMES so it cannot come
// back with a new meaning.
//
// The walker (slot 4) supersedes its entire purpose: sub-library-cost
// in-engine inference, but from a vendored-nothing flat-SoA blob with a
// load-time bit-parity oracle instead of a hundreds-of-MB toolchain.
#define MODEL_BACKEND_AOT_RESERVED 3

// v5.15.5.F.4d.1.E.1.2.E — flat-SoA in-engine tree walker (H21 APPEND).
// The cfg clamp INT(0,0,4) at CfgFieldRegistry already admits 4, so no cfg
// range change is needed. OPT-IN ONLY (W-a, D-432): selected by an explicit
// `ml_backend=4`, never promoted automatically, and it only ACTIVATES if the
// load-time parity oracle proves it bit-identical to the library — otherwise
// the backend stays XGBOOST and a failure flag surfaces (W-b).
#define MODEL_BACKEND_FLAT_WALKER 4

// feature indices — must match training pipeline exactly
// changing order here requires retraining models
#define FEAT_SHORT_SLOPE     0
#define FEAT_SHORT_R2        1
#define FEAT_SHORT_VARIANCE  2
#define FEAT_LONG_SLOPE      3
#define FEAT_LONG_R2         4
#define FEAT_LONG_VARIANCE   5
#define FEAT_VOL_RATIO       6
#define FEAT_ROR_SLOPE       7
#define FEAT_VOLUME_SLOPE    8
#define FEAT_VOLUME_DELTA    9
#define FEAT_EMA_SMA_SPREAD  10
#define FEAT_VWAP_DEV        11
#define FEAT_PRICE_STDDEV    12
#define FEAT_PRICE_AVG       13
#define FEAT_VOLUME_AVG      14
#define FEAT_EMA_ABOVE_SMA   15
// v4.3 — medium-horizon feature expansion. All append-only (CLAUDE.md
// "Adding a new ML feature" — never reorder, never remove).
#define FEAT_MID_SLOPE       16  // slope on 256-tick window (between short and long)
#define FEAT_MID_R2          17  // R² of mid-window slope
#define FEAT_CUMDELTA        18  // cumulative trade-side delta (buyer - seller agg) over rolling window
#define FEAT_HOUR_SIN        19  // cyclical hour-of-day: sin(2π × hour/24)
#define FEAT_HOUR_COS        20  // cyclical hour-of-day: cos(2π × hour/24)
#define FEAT_VOL_REGIME_RAT  21  // current short_stddev / longer-baseline stddev (4096-tick)
#define FEAT_TICK_RATE_Z     22  // current ticks/sec z-score vs trailing baseline
#define FEAT_DIST_TO_HIGH    23  // (price_max_baseline - current_price) / price (% below recent high)
#define FEAT_DIST_TO_LOW     24  // (current_price - price_min_baseline) / price (% above recent low)
// v4.5 Wave 1 — D.1 (book imbalance over time), D.2 (flow asymmetry),
// D.4 (large-trade detection). All append-only; new features grow indices
// monotonically.
#define FEAT_BOOK_IMB_MEAN_SHORT 25  // mean of last 64 book_imbalance samples
#define FEAT_BOOK_IMB_MEAN_LONG  26  // mean over full BookImbalanceHistory window (~17m)
#define FEAT_BOOK_IMB_DRIFT      27  // current book_imbalance - mean_long
#define FEAT_FLOW_10S            28  // signed-volume EWMA, half-life 10s
#define FEAT_FLOW_1M             29  // signed-volume EWMA, half-life 60s
#define FEAT_FLOW_5M             30  // signed-volume EWMA, half-life 300s
#define FEAT_LARGE_TRADE_Z       31  // z-score of current trade size vs trailing window
// v4.6 Wave 2 — D.3 (spread dynamics). Reads from BookSnapshot.spread
// (live) / DepthReplayState.current.spread (backtest) — both produce
// identical values for identical input bid/ask streams.
#define FEAT_SPREAD_BPS          32  // current spread / mid_price × 10000 (basis points)
#define FEAT_SPREAD_ZSCORE       33  // z-score of current spread vs trailing window
// v5.15.5.F.4d.1.E.1.2.G — DERIVED from the registry, no longer a literal.
//
// Was hardcoded `34`: a Class-18 mirror left behind when Features_PackAll +
// FOREACH_FEATURE became the feature SSoT at v5.8.1b and the registry then
// grew past 34 rows. The collector based each row at stride 34
// (BacktestSharded.hpp) while Features_PackAll writes NUM_REGISTERED_FEATURES
// floats there, so features 34..39 landed in the NEXT row's slot and were
// overwritten before any reader saw them — REGIME_TREND_STRENGTH,
// REGIME_VOL_ZSCORE, REGIME_CLASS_ONEHOT and all three FRAC_DIFF_PRICE_*
// never reached a training matrix. Deriving it closes the class: the stride
// cannot drift from the registry again.
//
// NOT to be confused with the legacy ModelFeatures_Pack below, which stays
// frozen at 34 BY DESIGN (it pins the equivalence test's legacy range). That
// freeze is about the frozen reference packer, never about this stride.
#define MODEL_NUM_FEATURES       ((int)NUM_REGISTERED_FEATURES)

// max features buffer — bumped 32 → 64 to leave headroom for D.3 (Wave 2:
// spread_bps, spread_zscore) and any further expansion without retouching
// every fixed-size feature buffer in the codebase.
#define MODEL_MAX_FEATURES   64

// E.1.2.G — the guard that was actually load-bearing. MODEL_MAX_FEATURES sizes
// SIX fixed-size feature buffers (StrategyParameters.hpp, MLStrategy.hpp,
// PortfolioController.hpp x2, BacktestEngine.hpp, BacktestPanels.hpp) while
// Features_PackAll writes out[FEATURE_##id] unbounded. Before this, the only
// thing keeping the registry under 64 was an assert in FeatureRegistry.hpp
// phrased as a BITMAP-width check — so widening the bitmap would have silently
// unguarded the buffers and made row #65 a stack smash. Pin the real invariant
// here, where both quantities are visible, and keep it independent of any
// bitmap-width change.
static_assert((int)NUM_REGISTERED_FEATURES <= MODEL_MAX_FEATURES,
    "feature registry exceeds MODEL_MAX_FEATURES: every fixed-size feature buffer "
    "(and the 1ULL<<idx feature-mask path) overflows. Raise MODEL_MAX_FEATURES and "
    "audit the mask cohort before adding the row.");

// model format version — increment when FEAT_* indices or count changes.
// embedded in trained models, checked at load time. old models with wrong
// version fail loudly instead of producing silent garbage predictions.
// FEAT_* constants are APPEND-ONLY — never reorder, never remove.
// v1: initial 16-feature pack
// v2 (v4.3): added 9 medium-horizon features (FEAT_MID_*, FEAT_CUMDELTA,
//           FEAT_HOUR_SIN/COS, FEAT_VOL_REGIME_RAT, FEAT_TICK_RATE_Z,
//           FEAT_DIST_TO_HIGH/LOW). Old v1 models will fail load.
// v3 (v4.5 Wave 1): added 7 microstructure features (FEAT_BOOK_IMB_*,
//           FEAT_FLOW_*, FEAT_LARGE_TRADE_Z). Old v2 models will fail load.
// v4 (v4.6 Wave 2): added 2 spread features (FEAT_SPREAD_BPS,
//           FEAT_SPREAD_ZSCORE). Old v3 models will fail load.
// v5 (v5.8.1a): introduce feature_registry_hash field in stamp body.
//           Hash is FNV-1a over FOREACH_FEATURE(X) enabled-row names +
//           versions (see FeatureRegistry.hpp). Stamps signed under one
//           registry refuse to load under a different registry. v4 stamps
//           lack the field and fail format-version check.
// v6 (v5.10.0b): bytewise-deterministic FPN_Binary-end-to-end slow path.
//           Multiple slow-path math primitives migrated from IEEE-754 to
//           pure-integer FPN_Binary: FlowFeatures EWMA decay (FPN_Exp), z-score
//           sqrt (FPN_Sqrt), RegimeDetector hour_sin/cos (FPN_Sin/Cos),
//           and FP64 divide (192-by-128 long division replaces long-double
//           FPU path). All slow-path features now produce bytewise-
//           identical output across compilers / -O levels / FMA support
//           given identical inputs. Bit-level shifts vs. v5 absorbed by
//           retraining; v5 stamps refuse to load with a "model trained
//           with pre-v5.10 IEEE-754 math; retrain required" message.
// v7 (v5.15.5.F.4d.1.E.1.2.G): MODEL_NUM_FEATURES stopped being a hardcoded
//           34 and became NUM_REGISTERED_FEATURES (40 at this ship). The
//           training matrix stride therefore changes, and every v6 model was
//           trained on 34 columns. Bumped per this constant's own contract
//           ("increment when FEAT_* indices or count changes") so v6 stamps
//           fail LOUDLY at load rather than being served a wider matrix —
//           the pre-existing `num_features > MODEL_NUM_FEATURES` oracle would
//           NOT have caught it (34 > 40 is false), and expected_num_features
//           is recorded in the stamp but never compared. Retrain required.
// v5.15.5.F.4d.1.E.1.2.G (re-gate H7) — 7 -> 8 for the feature-horizon ladder's
// 40 -> 60 count. Bumped for EXACTLY the reason 6 -> 7 was: the vector width
// changed, and TreeWalker_Predict never checks feature count. A stale 40-feature
// model handed a 60-wide vector reads slots 0..39 and returns a PLAUSIBLE answer —
// no crash, no NaN, just a confident prediction from the wrong columns.
//
// FEATURE_REGISTRY_HASH also moves (it always does when rows change) and D-464 now
// makes a hash mismatch refuse structurally, so this is the second of two
// independent gates rather than the only one. Both are kept deliberately: the hash
// catches a model trained against a different registry, the format version catches
// an artifact whose stamp cannot be read at all — TECH_DEBT-290's vacuous
// attribute check is precisely that hole.
#define MODEL_FORMAT_VERSION 8

// v5.15.5.F.4d.1.B.3 Step 1.6.7.1-3 — SOFT version bump infrastructure per
// DESIGN_SPECS/wire-format-patterns/wire-format-byte-preservation-discipline.md Layer 6b.
// CURRENT is the version emit produces; MAX_SUPPORTED is the upper bound parser accepts.
// Bump CURRENT when wire-format changes; bump MAX_SUPPORTED when next bump is planned.
// Pre-epoch versions (< EPOCH_FLOOR below) are HARD-INVALID at verify_model_stamp check 0c;
// the v1 legacy-prefixed-key dispatch layer was retired with the floor (TECH_DEBT-238, H21).
static constexpr uint32_t STAMP_FORMAT_VERSION_CURRENT      = 3;  // Ship-B DECIMAL epoch (money wire values re-encode); v2 = 16B-binary era (H21 tombstone), v1 = legacy prefix era
static constexpr uint32_t MAX_SUPPORTED_STAMP_FORMAT_VERSION = 3;  // parser accepts [EPOCH_FLOOR, MAX]; > MAX = future shape this engine can't interpret
// TECH_DEBT-237 close (2026-07-17) — the DECIMAL-EPOCH FLOOR. Stamps below it (v1/v2 + the
// field-absent pre-v5.9.0 case, which parses as 0) carry money wire values under the retired
// binary-FPN encoding and are UNCONDITIONALLY hard-invalid in verify_model_stamp (check 0c;
// valid=0, never -1 — the held_out_gate_strict fork only softens MISSING stamps, not pre-epoch
// ones). The floor is the EPOCH boundary and NEVER moves with future CURRENT bumps.
static constexpr uint32_t STAMP_FORMAT_VERSION_EPOCH_FLOOR   = 3;

// Ship-B P2 epoch guard (S-4/D-174 — the strict-gate bypass closed at the flip): stamps carry
// ~30 money fields whose wire values re-encode at the decimal epoch. Tripwire: the flip commit
// must bump CURRENT/MAX to 3 AND make pre-epoch refusal an UNCONDITIONAL hard-invalid class that
// BYPASSES the held_out_gate_strict fork (strict=0 loads r.valid=0 today — a v2 stamp must NOT
// enter the decimal engine), retiring the [1,2] legacy key dispatch (H21) + the retrain ritual.
// HISTORY: the flip (`838bf09`) satisfied this tripwire by the version bump alone — the refusal
// half slipped (Class-38 phantom, caught at the P6.96 rot-check). BOTH halves are now REAL:
// the 0c floor landed + the [1,2] legacy key dispatch retired (TECH_DEBT-237 / TECH_DEBT-238,
// closed 2026-07-17).
// [ASSERT]_[EPOCH_TRIPWIRE]_[MONEY_ENCODING_EPOCH == 0 || STAMP_FORMAT_VERSION_CURRENT >= 3]
static_assert(MONEY_ENCODING_EPOCH == 0u || STAMP_FORMAT_VERSION_CURRENT >= 3u,
              "Ship-B epoch: the engine money type flipped to decimal — bump STAMP_FORMAT_VERSION "
              "to 3 + unconditional pre-epoch stamp refusal (bypass the strict fork) in THIS commit.");
// [ASSERT]_[EPOCH_TRIPWIRE]_[EPOCH_FLOOR == 3 — the floor is the epoch boundary, pinned forever]
static_assert(STAMP_FORMAT_VERSION_EPOCH_FLOOR == 3u,
              "The decimal-epoch floor is the Ship-B boundary (3) and never moves with CURRENT "
              "bumps — a v3+ stamp is decimal-era by definition (H21 append-only semantics).");

//----------------------------------------------------------------------
// [SECTION]_[FEATURE LOOKBACK REGISTRY]
//----------------------------------------------------------------------
// per-feature metadata: how many ticks back each feature reads.
// used by:
//   - ValidationSplit (purge gap = max lookback across features + buffer)
//   - PortfolioController (warmup validation: warmup_ticks >= max lookback)
//
// when adding a new FEAT_* constant, add a matching entry here with its lookback.
// this is the single source of truth for feature temporal reach.
//
// FUTURE HOOKS:
//   multi-symbol: add symbol_id field when trading multiple pairs
//   feature growth: use 'enabled' field to toggle features without recompiling
//   feature selection: filter by enabled==1 before packing
//   stability tracking: save XGBoost importances per fold, compare across runs
//     → see ~/FoxML/private/TRAINING/stability/feature_importance/analysis.py
//     → thresholds: min_top_k_overlap=0.7, min_kendall_tau=0.6 (safety.yaml:157)
//======================================================================
// [STRUCT]_[FeatureLookback]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[one row of the FEATURE_LOOKBACKS table — a feature's FEAT_* index + name + temporal reach (lookback ticks) + enabled flag]
//======================================================================
// [CODE]
//======================================================================
struct FeatureLookback {
    int      feat_idx;        // FEAT_* constant
    const char *name;         // human-readable name (for display/debugging)
    // D-463 — TWO physical quantities, deliberately NOT one integer.
    //   lookback_ticks: a FINITE reach in RAW TICKS (rolling window / frac-diff taps).
    //                   0 for time-based features, which have no finite tick reach.
    //   half_life_us:   the EWMA half-life in MICROSECONDS. 0 for window features.
    // Storing the half-life as TIME is the point: a tick-count would need a rate, and
    // engine.cfg:31 documents the rate varying ~10x (active vs dead hours), so a
    // compile-time tick figure would be false most of the day. Consumers convert with
    // the cadence THEY can measure — see FeatureReach_MaxSamples.
    // uint64: a 24h half-life is 86,400,000,000 us and overflows int32.
    // D-469 — a THIRD reach column, and the one six enabled rows use EXCLUSIVELY.
    //   min_history_us: the warm-up floor in MICROSECONDS (the D-467 gate column).
    // It was dropped by this table's X expansion, so `dist_to_high_24h` /
    // `dist_to_low_24h` / `range_pos_24h` (24h) and `frac_diff_bars_d04/05/06` (4.2h)
    // arrived here with lookback_ticks == 0 AND half_life_us == 0 — a reach of ZERO
    // for features that genuinely read a full day back. Their reach is real, not a
    // warm-up artifact: the bucket ring is 288 x 5min = 24h exactly
    // (ML_Headers/FlowFeatures.hpp). Carrying the column is what lets
    // FeatureReach_MaxSamples see them at all.
    int      lookback_ticks;
    uint64_t half_life_us;
    uint64_t min_history_us;
    int      enabled;         // 1 = active, 0 = disabled (future: feature toggling)
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-09-01]
// [SIZE]_[48B]
// [ALIGN]_[8]
// [CACHE_LINES]_[1]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[FeatureLookback]
//======================================================================

// v5.15.5.F.4d.1.E.1.2.G — GENERATED from FOREACH_FEATURE, no longer hand-maintained.
//
// Was a manual 34-row table against a 40-row registry — a Class-18 mirror whose
// own comment claimed "append-only — matches FEAT_* ordering for direct indexing"
// while it had silently fallen 6 rows behind. That mattered well beyond display:
// FeatureLookback_Max() feeds PurgeGap_Compute (Backtest/ValidationSplit.hpp),
// which sets the train/validation purge gap, and Fingerprint_Compute hashes this
// table to "catch feature set changes" — neither could see a feature the mirror
// had never been told about.
//
// Generating it makes all four columns derived: adding a feature now raises the
// max lookback, widens the purge gap, and moves the fingerprint automatically.
// Ordering matches FEATURE_<ID> BY CONSTRUCTION (same X-macro walk), so the
// direct-indexing contract is now structural rather than asserted.
static const FeatureLookback FEATURE_LOOKBACKS[] = {
#define X(id, name, version, enabled, fn, note, staleness, lookback_ticks, half_life_us, min_history_us) \
    { FEATURE_##id, name, lookback_ticks, half_life_us, min_history_us, enabled },
    FOREACH_FEATURE(X)
#undef X
};

static const int FEATURE_LOOKBACK_COUNT = sizeof(FEATURE_LOOKBACKS) / sizeof(FEATURE_LOOKBACKS[0]);

//======================================================================
// [FUNCTION]_[FeatureLookback_Max]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[max temporal reach across enabled features (CountEnabled rides) — feeds the ValidationSplit purge gap + the PortfolioController warmup check]
//======================================================================
// [CODE]
//======================================================================
static inline int FeatureLookback_MaxTicks(void) {
    int max_lb = 0;
    for (int i = 0; i < FEATURE_LOOKBACK_COUNT; i++) {
        if (FEATURE_LOOKBACKS[i].enabled && FEATURE_LOOKBACKS[i].lookback_ticks > max_lb)
            max_lb = FEATURE_LOOKBACKS[i].lookback_ticks;
    }
    return max_lb;
}

// D-463 — the longest EWMA half-life, in microseconds. 0 when no time-based feature
// is enabled. Kept separate from the tick reach because they are different physical
// quantities; combining them requires a cadence only the caller knows.
static inline uint64_t FeatureHalfLife_MaxUs(void) {
    uint64_t max_hl = 0;
    for (int i = 0; i < FEATURE_LOOKBACK_COUNT; i++) {
        if (FEATURE_LOOKBACKS[i].enabled && FEATURE_LOOKBACKS[i].half_life_us > max_hl)
            max_hl = FEATURE_LOOKBACKS[i].half_life_us;
    }
    return max_hl;
}

// D-469 — the longest warm-up floor, in microseconds. For six enabled rows this is the
// ONLY column carrying their reach (the bucket-ring extrema + the frac-diff bar rows
// declare neither a tick window nor a half-life), so a reach walk that skips it reports
// ZERO for features that read a full day back. Sister of the two above by construction:
// same walk, same enabled filter, different physical column.
static inline uint64_t FeatureMinHistory_MaxUs(void) {
    uint64_t max_mh = 0;
    for (int i = 0; i < FEATURE_LOOKBACK_COUNT; i++) {
        if (FEATURE_LOOKBACKS[i].enabled && FEATURE_LOOKBACKS[i].min_history_us > max_mh)
            max_mh = FEATURE_LOOKBACKS[i].min_history_us;
    }
    return max_mh;
}

// How many EWMA half-lives count as "the feature still reads that far back".
// 3 half-lives retains ~12.5% weight; beyond that the contribution is below the
// noise floor of the features themselves. Named so the choice is visible and
// tunable rather than buried in an expression.
#define FEATURE_EWMA_REACH_HALF_LIVES 3

//======================================================================
// [FUNCTION]_[FeatureReach_MaxSamples]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [DETERMINISM]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the max feature reach expressed in FEATURE-MATRIX SAMPLES — the space the purge gap actually indexes; converts BOTH columns using cadences the caller measures, in exact integer arithmetic]
// [REFERENCE]_[DECISION]_[D-463]
//======================================================================
// [CODE]
//======================================================================
// ticks_per_sample: how many raw ticks between collected feature rows (poll_interval).
// sample_period_us: the OBSERVED mean wall-time between feature rows, measured from the
//                   dataset by the caller (total_span_us / sample_count). 0 = unknown,
//                   in which case the EWMA contribution is skipped rather than guessed.
//
// All-integer on purpose: these are microsecond COUNTS, so integer division is exact and
// bit-identical across runs and binaries (H9/H10). No FPN needed — FPN buys fractional
// precision this has no use for, and a double would add contraction variance for nothing.
static inline int FeatureReach_MaxSamples(int ticks_per_sample, uint64_t sample_period_us) {
    if (ticks_per_sample < 1) ticks_per_sample = 1;

    // window features: a RAW-TICK reach -> samples. This division is the ~100x
    // over-purge fix (D-463): the tick figure was previously used as a sample count.
    const int win_ticks   = FeatureLookback_MaxTicks();
    const int win_samples = (win_ticks + ticks_per_sample - 1) / ticks_per_sample;  // ceil

    // time features -> samples via the OBSERVED period. TWO time columns feed this,
    // not one (D-469): the EWMA half-life scaled by FEATURE_EWMA_REACH_HALF_LIVES, and
    // min_history_us, which is already a reach and is taken AS-IS (it is authored per
    // row as ~3 half-lives where an EWMA is involved, so scaling it again would double
    // -count). Whichever is larger wins; both are skipped when the period is unknown.
    //
    // WHY min_history_us belongs here and its omission was the defect: six ENABLED rows
    // declare NEITHER a tick window NOR a half-life, so before D-469 this function
    // returned the 11-sample window reach for a feature set that reads 24 HOURS back —
    // and the purge gap it feeds is the codebase's own stated leakage control
    // ("the eval side reads back through the feature reach into training ticks").
    // D-463 converted one arm of the max() and left another; this is the THIRD arm,
    // found by enumerating the reach COLUMNS rather than the reported symptom (M9).
    int time_samples = 0;
    const uint64_t hl_us = FeatureHalfLife_MaxUs();
    const uint64_t mh_us = FeatureMinHistory_MaxUs();
    if (sample_period_us > 0) {
        uint64_t reach_us = hl_us * (uint64_t)FEATURE_EWMA_REACH_HALF_LIVES;
        if (mh_us > reach_us) reach_us = mh_us;
        if (reach_us > 0) {
            const uint64_t n = (reach_us + sample_period_us - 1) / sample_period_us;  // ceil
            time_samples = (n > (uint64_t)INT32_MAX) ? INT32_MAX : (int)n;
        }
    }
    return (win_samples > time_samples) ? win_samples : time_samples;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[FeatureReach_MaxSamples]
//======================================================================

// count enabled features (for validation)
static inline int FeatureLookback_CountEnabled(void) {
    int count = 0;
    for (int i = 0; i < FEATURE_LOOKBACK_COUNT; i++) {
        if (FEATURE_LOOKBACKS[i].enabled) count++;
    }
    return count;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// compute max lookback across all enabled features
// used by: ValidationSplit (purge gap), PortfolioController (warmup check)
//======================================================================
// [END_FUNCTION]_[FeatureLookback_Max]
//======================================================================

//----------------------------------------------------------------------
// [SECTION]_[conditional includes — only pull in headers when backend is enabled]
//----------------------------------------------------------------------
#ifdef USE_XGBOOST
#include <xgboost/c_api.h>
#endif

#ifdef USE_LIGHTGBM
#include <LightGBM/c_api.h>
#endif

//======================================================================
// [STRUCT]_[ModelHandle]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [DATA_ORIENTED_DESIGN] [BITMAP_PACKED]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the runtime model handle — HOT inference scalars in line 1, HOT-2 composite arrays in line 2, WARM scaler, COLD X-macro stamp-derived fields, COLD-2 runtime paths; bit-packed has_flags]
// [INSTANTIATION]_[[64]]
// [REFERENCE]_[DESIGN_SPEC]_[per-snapshot-cluster-layout-pattern]
// [REFERENCE]_[DESIGN_SPEC]_[bitmap-flag-api]
// [REFERENCE]_[DESIGN_SPEC]_[[struct-padding-determinism-pattern] [bitmap-flag-api.md]]
// [REFERENCE]_[INVARIANT]_[[H12] [H18]]
// [REFERENCE]_[CLASS]_[18]
// [REFERENCE]_[TECH_DEBT]_[TECH_DEBT-14]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
struct alignas(64) ModelHandle {
    // v5.12.3.B+E — prediction normalizer. Maps heterogeneous model
    // outputs to a [0,1] buy-probability space so ensemble blend can
    // average across mixed model types. Default NORM_IDENTITY = passthrough
    // (preserves existing single-output semantics bytewise). Loader sets
    // this from stamp body's label_kind at load time; never mutated post-
    // load (per-handle invariant). normalizer_param holds tp_pct for
    // NORM_REGRESSION; unused for other kinds.
    //
    // Elision-friendly design: hot-path-equivalent
    // call site checks `if (m->normalizer == NORM_IDENTITY) return raw;`
    // FIRST. Default state → 1-line early return → optimizer treats as
    // ~1ns predicted-not-taken. Switch on enum is only entered when a
    // model is actually trained with non-IDENTITY normalizer. ~1ns
    // when off; ~5ns when on. Slow-path budget irrelevant either way
    // (Model_Predict's XGBoost C API call dominates; cost is linear in tree
    // count — ~550ns/round 3-class, ~217µs at 1050 trees, measured 2026-08-22).
    enum prediction_normalizer_t {
        NORM_IDENTITY        = 0,    // passthrough (default; current behavior)
        NORM_REGRESSION      = 1,    // [-tp_pct, +tp_pct] → [0, 1] via clamp(0.5 + raw / (2*tp), 0, 1)
        NORM_BARRIER_CLASS_1 = 2,    // 3-class barrier; explicit class-1 extraction
        NORM_COMPOSITE       = 3,    // uses target_classes/class_weights from this struct (3.A)
    };

    //---- [SECTION]_[HOT CLUSTER — read during Model_Predict inference (cache line 1)] ----
    void    *handle;                 //  8 B — opaque: BoosterHandle (XGB / LGBM)
    int      backend;                //  4 B — MODEL_BACKEND_NONE / XGBOOST / LIGHTGBM
    int      num_features;           //  4 B — expected input dimension
    // v5.11.62 — buy class for multiclass models. Default 0 (binary positive
    // class). NodeModelZoo loader sets: 0 for buy_signal; 1 for barrier
    // num_outputs=3 (PEAK_VALLEY_STABLE class 1 = peak); 0 for regime
    // (operator chooses semantics via cfg). Out-of-range falls back to 0.
    int      num_outputs;            //  4 B — 1 = binary/regression, ≥2 = multiclass softmax
    int      buy_class_idx;          //  4 B
    // v5.9.3a — Gap H observability. Set by NodeModelZoo_TryLoadRole when
    // scaler load fails in non-strict mode. Surfaces to
    // PerNodeSnap.ml_scaler_load_failed for ML Status panel.
    int      scaler_load_failed;     //  4 B
    uint8_t  normalizer;             //  1 B — prediction_normalizer_t enum; default NORM_IDENTITY
    // v5.12.3.A — composite-signal extractor. When num_classes_active > 1,
    // Model_Predict returns Σ class_weights[i] × out_result[target_classes[i]]
    // over the first num_classes_active entries. Default 1 + target[0]=buy_class_idx
    // + weights[0]=1.0 preserves single-class behavior bytewise.
    uint8_t  num_classes_active;     //  1 B — default 1
    int16_t  _hot_pad0 = 0;          //  2 B — explicit zero-init padding (H12)
    float    normalizer_param;       //  4 B — tp_pct for NORM_REGRESSION; unused otherwise
    // v5.15.0 — bit-packed has_* state for stamp-derived field presence
    // (REPLACES 14 uint8_t has_* direct fields). Bit positions allocated
    // by FOREACH_STAMP_BOUND_MODEL_CONST; access via STAMP_HAS(*h, <name>)
    // / STAMP_SET / STAMP_CLR aliases (shared with ModelStampResult +
    // StampInferenceCfgInputs).
    uint64_t has_flags;              //  8 B
    // v5.15.1 — drift_flags_at_load: bits set at TryLoadRole post-verify
    // chokepoint. Storage uses FOREACH_FAILURE_MODE bit positions via
    // FAILURE_MASK_<name> (e.g., FAILURE_MASK_feature_hash_drift). Read
    // by ShardedSnapshot_Publish which OR-aggregates across all 4 zoo
    // roles into PerNodeSnap.failure_flags. Repurposes 2 B of v5.15.0's
    // _hot_pad1; net cluster size unchanged.
    uint16_t drift_flags_at_load;    //  2 B
    // 14 B explicit zero-init padding (H12) fills HOT
    // cluster to exactly one cache line + keeps target_classes (next
    // cluster) at offset 64.
    uint16_t _hot_pad1a = 0;         //  2 B
    int32_t  _hot_pad1b[3] = {0, 0, 0};  // 12 B
    // = 64 B used; cache line 1 fully consumed.

    //---- [SECTION]_[HOT-2 — composite signal arrays read in Model_Predict (cache line 2)] ----
    int      target_classes[8];      // 32 B — default [buy_class_idx, 0, 0, ...]
    float    class_weights[8];       // 32 B — default [1.0, 0, 0, ...]

    //---- [SECTION]_[WARM CLUSTER — feature standardizer (read every inference)] ----
    // v5.9.3a — feature standardizer (mean-centering + unit-variance).
    // Inline (not heap) per audit decision: NUM_REGISTERED_FEATURES is
    // constexpr → struct size known at compile time. ~600 bytes; trivial
    // vs the mmap'd XGBoost booster size. Apply path early-returns when
    // has_scaler=0 (identity-applied).
    tt::FeatureStandardizer scaler;

    //---- [SECTION]_[COLD CLUSTER — stamp-derived value fields (X-macro auto-generated)] ----
    // Walks FOREACH_STAMP_BOUND_MODEL_CONST (union of PRE_CFG + POST_CFG)
    // with STAMP_HANDLE_GEN_<presence> dispatch. INCLUDE entries declared;
    // SKIP_HANDLE entries omitted (parser-side-only: held_out_fraction,
    // feature_scaler_present boolean, grid_member_*, label_registry_hash,
    // feature_mask, expected_*).
    #define X(name, group, presence, type, fmt, default_val, get_value, emit_when, doc) \
        STAMP_HANDLE_GEN_##presence(name, type)
    FOREACH_STAMP_BOUND_MODEL_CONST(X)
    #undef X

    // v5.15.5.F.4d.1.B.3 Phase F HIGH-1 (b) cascade close — cfg-derived cohort fields auto-gen on
    // ModelHandle (sister to the ModelStampResult struct-gen + StampInferenceCfgInputs auto-gen
    // sites below). Same STAMP_RESULT_DERIVED_FIELDS_AUTO_GEN() macro; F=64 brought into struct
    // scope (same F-into-scope pattern at both sister sites). After Phase F HIGH-1 (b) row deletion at Step 2:
    // handle.<name> auto-gen IS the sole source for cfg-derived field storage at runtime; NodeModelZoo
    // load-time copy flows sr (cfg-derived auto-gen on ModelStampResult) → handle via
    // COPY_RESULT_TO_HANDLE_FROM_DERIVED (the fifth walker). ⚠ HISTORY: this comment claimed that
    // copy existed from .B.3 — it did NOT (the walker was never built; fields died in the local sr
    // and drift rows compared handle-side zeros — PARITY-043). Made TRUE at E.1.2.C leg 2
    // (2026-08-20). Drift check via cfg_derived::drift_check_from_derived
    // reads handle.<name> (cfg-derived) + cfg.<name> (cfg-derived) — same source-of-truth surface.
    // EXCLUSION REDIRECT: 3 xgb_* names redirect to dead-prefixed per H18 sidecar (sister to
    // the ModelStampResult exclusion below + FOREACH_STAMP_RESULT_FIELD_EXCLUSION sidecar).
    static constexpr unsigned F_for_derived = F;  // bring template F into nested-struct scope
    #define xgb_min_child_weight _stamp_result_excluded_xgb_min_child_weight
    #define xgb_seed             _stamp_result_excluded_xgb_seed
    #define xgb_train_nthread    _stamp_result_excluded_xgb_train_nthread
    STAMP_RESULT_DERIVED_FIELDS_AUTO_GEN()
    #undef xgb_min_child_weight
    #undef xgb_seed
    #undef xgb_train_nthread

    //---- [SECTION]_[COLD-2 CLUSTER — runtime-only identifiers (NOT in stamp registry)] ----
    char     model_path[256];         // 256 B — display/logging path
    char     training_fingerprint[65];//  65 B — SHA256 of config+data at training time
};
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Bit-packed has_flags + X-macro-generated stamp-derived value fields.
// 14 uint8_t has_* direct fields → 1 uint64_t has_flags (bit-packed via
// FOREACH_STAMP_BOUND_MODEL_CONST allocation, accessed via STAMP_HAS /
// SET / CLR aliases shared with ModelStampResult / StampInferenceCfgInputs).
//
// Stamp-derived value fields auto-generated by walking
// FOREACH_STAMP_BOUND_MODEL_CONST with the presence-aware
// STAMP_HANDLE_GEN_<presence> dispatch:
//   INCLUDE     → field declared on handle (default; ~33 entries)
//   SKIP_HANDLE → parser/emit only; not on handle (~10 entries)
//
// Field names are CANONICAL — match registry wire keys + ModelStampResult
// + StampInferenceCfgInputs. Prefixed legacy names (stamp_xgb_max_depth,
// stamp_inf_confidence_threshold_scale, etc.) unified to unprefixed
// canonical names (xgb_max_depth, inference_cfg_confidence_threshold_scale).
//
// Closes TECH_DEBT-014; closes the Class 18 mirror between parser side
// (ModelStampResult, already X-macro-driven since v5.14.8.A.merged) and
// runtime side (ModelHandle, now equally X-macro-driven).
//
// Layout discipline (per per-snapshot-cluster-
// layout-pattern.md + bitmap-flag-api.md + struct-padding-determinism-pattern.md):
//   HOT cluster — read during Model_Predict inference (cache line 1)
//   WARM cluster — scaler (read during inference for feature normalization)
//   COLD cluster — stamp-derived metadata (boot-WARN, GUI panel surfaces)
//   COLD-2 cluster — runtime-only paths / fingerprint (not in registry)
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-08-31]
//----------------------------------------------------------------------
// [SIZE]_[7296B]
// [ALIGN]_[64]
// [CACHE_LINES]_[114]
// [STRADDLE]_[unverified: scaler_sha256 overlay_hash effective_hash run_name scaler_fit_data_hash removal_reasons_csv environment_tf_version environment_cpu_model environment_libgomp_version]
//======================================================================
// [END_STRUCT]_[ModelHandle]
//======================================================================
// Sanity asserts: ModelHandle must be cache-line sized + hot cluster fits
// the first cache line. Static_assert at template-instantiation site to
// catch field-additions that break the cluster layout.
//
// (Per per-snapshot-cluster-layout-pattern.md Step 5; complements
// H12 padding determinism.)
// [ASSERT]_[LAYOUT_LOCK]_[sizeof(ModelHandle<64>) % 64 == 0]
static_assert(sizeof(ModelHandle<64>) % 64 == 0,
    "ModelHandle<64> must be cache-line sized; a field addition broke the cluster layout. "
    "Audit cluster boundaries + adjust _hot_padN fields or reorder.");
// [ASSERT]_[LAYOUT_LOCK]_[alignof(ModelHandle<64>) == 64]
static_assert(alignof(ModelHandle<64>) == 64,
    "ModelHandle<64> must be 64-byte aligned for cache-line independence.");
// [ASSERT]_[LAYOUT_LOCK]_[offsetof(has_flags) < 64 — HOT cluster residency]
static_assert(offsetof(ModelHandle<64>, has_flags) < 64,
    "has_flags must live in HOT cluster (cache line 1) — a field addition pushed it out.");
// [ASSERT]_[LAYOUT_LOCK]_[offsetof(target_classes) == 64 — HOT-2 boundary]
static_assert(offsetof(ModelHandle<64>, target_classes) == 64,
    "target_classes must start cache line 2 (HOT-2 cluster boundary).");

//======================================================================
// [FUNCTION]_[Model_Init]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[brace-init zero-fill + the non-zero defaults (composite single-class, scaler identity) — the handle's clean-slate constructor]
// [REFERENCE]_[INVARIANT]_[H12]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline void Model_Init(ModelHandle<F> *m) {
    // v5.15.0 — Brace-init zero-fills the entire struct (matches
    // the ModelStampResult{} discipline in verify_model_stamp).
    // Replaces ~50 lines of explicit field zero-inits with one statement.
    // Default member initializers (_hot_pad0/1 = 0) preserve padding
    // determinism per H12.
    //
    // Zero-init correctly clears:
    //   - has_flags (all 13+ stamp-derived bit positions)
    //   - all X-macro-generated stamp value fields (~33)
    //   - HOT cluster (handle, backend, num_*, buy_class_idx, etc.)
    //   - HOT-2 arrays (target_classes / class_weights)
    //   - COLD paths (model_path / training_fingerprint)
    //
    // Override fields that need non-zero defaults below.
    *m = ModelHandle<F>{};
    // v5.12.3.A — composite-signal defaults: single-class extraction
    // equivalent to pre-v5.12.3.A behavior. Loader sets num_classes_active>1
    // + target_classes / class_weights from stamp body (Surface G) when
    // operator trains a model with composite-signal config.
    m->num_classes_active = 1;
    m->class_weights[0]   = 1.0f;
    // target_classes[0] = buy_class_idx; brace-init zeroed both, so this is
    // a no-op today but documents the invariant for future buy_class_idx
    // default changes.
    m->target_classes[0]  = m->buy_class_idx;
    // v5.9.3a — scaler init. FeatureStandardizer_Init sets non-zero
    // defaults (stddev[i]=1.0, winsor bounds=±INFINITY) beyond what
    // brace-init zero-fills. NodeModelZoo_TryLoadRole calls
    // FeatureStandardizer_Load post-Model_Load to populate from sidecar.
    tt::FeatureStandardizer_Init(&m->scaler);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[Model_Init]
//======================================================================

#ifdef USE_XGBOOST

//======================================================================
// [FUNCTION]_[TreeWalkerOracle_EmitLeafRows]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [BOOT_TIME]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[PATH-DIRECTED probe synthesis — DFS one tree root-to-leaf,
//   accumulating the [lo,hi) interval each split imposes, and emit ONE feature
//   row per reachable leaf that satisfies the whole path by construction.]
//======================================================================
// WHY THIS EXISTS: the first oracle set ONE feature at a time from a zero
// baseline, so it could only ever reach leaves whose entire root-to-leaf path is
// satisfiable by a single non-zero feature. MEASURED consequence on the real
// twins model: 3122 of 4200 leaves reached — 74%, with the other 26% requiring
// feature COMBINATIONS that one-at-a-time probing cannot generate. "Parity
// PROVEN" over a probe set that structurally cannot reach a quarter of the
// model's outputs is the PARTIAL-masquerading-as-TOTAL shape (M10).
//
// Solving the path constraints instead makes leaf coverage TOTAL BY
// CONSTRUCTION rather than by luck: every leaf gets a row built from its own
// path, so a leaf can only go unprobed if its path is genuinely infeasible
// (contradictory constraints — which is itself worth reporting).
//======================================================================
// [CODE]
//======================================================================
static inline void TreeWalkerOracle_EmitLeafRows(
        const FlatTreeModel* w, int32_t node, float* lo, float* hi,
        int num_features, float* rows, int* nrows, int max_rows) {
    if (*nrows >= max_rows) return;
    const FlatTreeNode* nd = &w->nodes[node];

    if (nd->meta & WALKER_META_IS_LEAF) {
        float* r = &rows[(size_t)(*nrows) * (size_t)num_features];
        for (int f = 0; f < num_features; ++f) {
            float v;
            if (lo[f] > -INFINITY)      v = lo[f];                          // satisfies f >= lo
            else if (hi[f] <  INFINITY) v = nextafterf(hi[f], -INFINITY);   // satisfies f <  hi
            else                        v = 0.0f;                           // unconstrained
            if (!(v >= lo[f] && v < hi[f]) && !(lo[f] == -INFINITY && hi[f] == INFINITY)) {
                return;   // contradictory path — unreachable leaf, emit nothing
            }
            // PARITY-049 — the -1.0f nudge that stood here is REMOVED, not ported to NaN.
            // It existed because -1.0f meant MISSING to both engines, so a synthesized row
            // landing on it would route default_left instead of exercising the split it was
            // built for. Now that -1.0f compares normally, nudging away from it would SKIP
            // legitimate -1.0 rows — narrowing leaf coverage on precisely the value this defect
            // was about. The NaN case needs no guard either: path-directed synthesis solves
            // `[lo, hi)` intervals and can never produce NaN.
            r[f] = v;
        }
        ++(*nrows);
        return;
    }

    const int   f   = (int)(nd->meta & WALKER_META_FEAT_MASK);
    const float thr = nd->cond;
    if (f < 0 || f >= num_features) return;

    // LEFT: f < thr  ⇒ tighten the upper bound.
    const float saved_hi = hi[f];
    if (thr < hi[f]) hi[f] = thr;
    if (lo[f] < hi[f]) TreeWalkerOracle_EmitLeafRows(w, nd->left, lo, hi, num_features, rows, nrows, max_rows);
    hi[f] = saved_hi;

    // RIGHT: f >= thr ⇒ tighten the lower bound.
    const float saved_lo = lo[f];
    if (thr > lo[f]) lo[f] = thr;
    if (lo[f] < hi[f]) TreeWalkerOracle_EmitLeafRows(w, nd->right, lo, hi, num_features, rows, nrows, max_rows);
    lo[f] = saved_lo;
}
// [END_CODE]
// [END_FUNCTION]_[TreeWalkerOracle_EmitLeafRows]
//======================================================================

//======================================================================
// [FUNCTION]_[TreeWalkerOracle_Verify]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [BOOT_TIME] [DETERMINISM]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[load-time BIT-PARITY gate: the walker only becomes the serve
//   backend if it reproduces XGBoosterPredict EXACTLY over a designed +
//   path-directed probe set, in BOTH margin and transformed space.]
//======================================================================
// WHY BOTH SPACES: margin (option_mask=1) isolates walk + accumulation;
// transformed (option_mask=0) additionally proves the softmax/sigmoid recipes.
// Only-transformed would let a walk error and a transform error cancel;
// only-margin would leave the recipes unverified.
//
// WHY BITWISE, not epsilon: an epsilon is a judgement about how much divergence
// is acceptable on a capital path, and it widens silently as models grow.
// Bit-equality needs no defending and is ACHIEVABLE by construction here,
// because the walk copies xgboost's own accumulation order and libm recipes
// (W-d, D-432).
//
// ⚠️ BATCHED ON PURPOSE — AND THIS IS NOT THE CACHE TRAP THE PLAN WARNS ABOUT.
// The plan's "fresh DMatrix per row" rule exists because REUSING one DMatrix
// handle across DIFFERENT data returns the FIRST row's cached prediction (the
// measured 2.6us false floor) — an oracle that agrees with itself. Building ONE
// DMatrix that CONTAINS all N distinct rows is the opposite: XGBoost predicts
// every row exactly once and returns N x K outputs. No handle is reused, no
// prediction is cached across differing inputs.
//
// The per-row version was MEASURED at 168 SECONDS for 4096 probes on the twins
// model (~41ms/probe: two 1-row DMatrix builds + two full 1050-tree traversals
// each, plus libgomp spin-up per call). That is ~3 minutes added to every boot,
// per model — a gate nobody would keep enabled is a gate that does not protect
// anything. Batching collapses 2N library calls into exactly 2.
//======================================================================
// [CODE]
//======================================================================
#define WALKER_ORACLE_MAX_PROBES 16384

template <unsigned F>
inline int TreeWalkerOracle_Verify(const FlatTreeModel* w, BoosterHandle booster,
                                   int num_features, char* reason, size_t reason_len,
                                   int* out_probes, int* out_leaves_hit,
                                   int* out_leaves_total) {
    if (reason && reason_len) reason[0] = '\0';
    if (out_probes) *out_probes = 0;
    if (out_leaves_hit) *out_leaves_hit = 0;
    if (out_leaves_total) *out_leaves_total = 0;

    if (!w || !booster || num_features <= 0) {
        if (reason) snprintf(reason, reason_len, "oracle: null model/booster or bad feature count");
        return 0;
    }
    if (num_features > MODEL_NUM_FEATURES) {
        if (reason) snprintf(reason, reason_len,
                             "oracle: num_features %d exceeds build's MODEL_NUM_FEATURES %d",
                             num_features, MODEL_NUM_FEATURES);
        return 0;
    }

    const int   K        = w->num_class;
    const int   max_rows = WALKER_ORACLE_MAX_PROBES;
    float* rows = (float*)calloc((size_t)max_rows * (size_t)num_features, sizeof(float));
    float* lo   = (float*)malloc((size_t)num_features * sizeof(float));
    float* hi   = (float*)malloc((size_t)num_features * sizeof(float));
    if (!rows || !lo || !hi) {
        free(rows); free(lo); free(hi);
        if (reason) snprintf(reason, reason_len, "oracle: probe-matrix alloc failed");
        return 0;
    }
    int n = 0;

    // ---- (1) all-zero row: the trivial path ----
    if (n < max_rows) { /* calloc already zeroed it */ ++n; }

    // ---- (2) per-feature -1.0f (the value that USED to be mis-routed) ----
    // RETARGETED at PARITY-049. This section used to write the missing sentinel, which was
    // -1.0f; the sentinel is now NaN, and section (3) below already probes NaN. Had this simply
    // followed the constant, (2) and (3) would have become the SAME probe and the corpus would
    // have silently lost a case — while still reporting full leaf coverage. So it now pins the
    // literal: -1.0f is a legitimate one-sided-book output that compares normally post-fix, and
    // is therefore exactly the input most likely to expose a walk/library disagreement here.
    // PARITY-047 is the standing proof this lane can be wrong while everything else looks right.
    for (int f = 0; f < num_features && n < max_rows; ++f, ++n)
        rows[(size_t)n * num_features + f] = XGB_LEGACY_MINUS_ONE_PROBE;

    // ---- (3) per-feature NaN ----
    // `x < cond` is FALSE for NaN, so a walk that compares instead of routing
    // default_left diverges HERE and nowhere else.
    for (int f = 0; f < num_features && n < max_rows; ++f, ++n)
        rows[(size_t)n * num_features + f] = nanf("");

    // ---- (4) PATH-DIRECTED: one row per reachable leaf, per tree ----
    // Replaces the old one-feature-at-a-time boundary probes, which reached only
    // 74% of leaves on the real model. Each row satisfies its leaf's entire
    // root-to-leaf constraint set by construction, so coverage is total except
    // for genuinely infeasible paths.
    for (int t = 0; t < w->num_trees && n < max_rows; ++t) {
        for (int f = 0; f < num_features; ++f) { lo[f] = -INFINITY; hi[f] = INFINITY; }
        TreeWalkerOracle_EmitLeafRows(w, w->tree_roots[t], lo, hi, num_features,
                                      rows, &n, max_rows);
    }
    free(lo); free(hi);

    if (n <= 0) {
        free(rows);
        if (reason) snprintf(reason, reason_len, "oracle: no probes synthesized");
        return 0;
    }

    // ---- ONE DMatrix over all N distinct rows; TWO predicts total ----
    DMatrixHandle dmat;
    if (XGDMatrixCreateFromMat(rows, (bst_ulong)n, (bst_ulong)num_features, XGB_MISSING_VALUE, &dmat) != 0) {
        free(rows);
        if (reason) snprintf(reason, reason_len, "oracle: DMatrix create failed (%d rows)", n);
        return 0;
    }

    bst_ulong len_m = 0, len_t = 0;
    const float *res_m = nullptr, *res_t = nullptr;
    int ok = 1;

    if (XGBoosterPredict(booster, dmat, 1, 0, 0, &len_m, &res_m) != 0 || !res_m) {
        if (reason) snprintf(reason, reason_len, "oracle: margin predict failed");
        ok = 0;
    }
    // COPY the margin block before the second call — out_result points at an
    // INTERNAL buffer that the next Predict overwrites. Comparing against it
    // afterwards would silently be comparing transformed-vs-transformed.
    float* margin = nullptr;
    if (ok) {
        margin = (float*)malloc((size_t)len_m * sizeof(float));
        if (!margin) { if (reason) snprintf(reason, reason_len, "oracle: margin copy alloc failed"); ok = 0; }
        else memcpy(margin, res_m, (size_t)len_m * sizeof(float));
    }
    if (ok && (XGBoosterPredict(booster, dmat, 0, 0, 0, &len_t, &res_t) != 0 || !res_t)) {
        if (reason) snprintf(reason, reason_len, "oracle: transformed predict failed");
        ok = 0;
    }
    if (ok && ((int)len_m != n * K || (int)len_t != n * K)) {
        if (reason) snprintf(reason, reason_len,
                             "oracle: output shape mismatch (got %d/%d, expected %d)",
                             (int)len_m, (int)len_t, n * K);
        ok = 0;
    }

    // ---- walk every row, compare BOTH spaces bitwise, count leaf coverage ----
    static const int COV_WORDS = (WALKER_MAX_TOTAL_NODES + 63) / 64;
    uint64_t* covered = ok ? (uint64_t*)calloc((size_t)COV_WORDS, sizeof(uint64_t)) : nullptr;
    if (ok && !covered) { if (reason) snprintf(reason, reason_len, "oracle: coverage bitmap alloc failed"); ok = 0; }

    for (int r = 0; ok && r < n; ++r) {
        const float* row = &rows[(size_t)r * num_features];
        for (int c = 0; c < K; ++c) {
            const int start = w->class_tree_start[c], cnt = w->class_tree_count[c];
            for (int t = 0; t < cnt; ++t) {
                int32_t leaf = TreeWalker_WalkOneIdx(w->nodes, w->tree_roots[start + t], row, w->max_depth);
                if (leaf >= 0 && leaf < w->total_nodes) covered[leaf >> 6] |= (1ULL << (leaf & 63));
            }
        }
        float wm[WALKER_MAX_CLASSES], wt[WALKER_MAX_CLASSES];
        if (TreeWalker_PredictMargin(w, row, wm) != WALKER_OK) {
            if (reason) snprintf(reason, reason_len, "oracle: walker margin refused on probe %d", r);
            ok = 0; break;
        }
        for (int c = 0; c < K; ++c) wt[c] = wm[c];
        if (TreeWalker_ApplyTransform(w, wt) != WALKER_OK) {
            if (reason) snprintf(reason, reason_len, "oracle: walker transform refused on probe %d", r);
            ok = 0; break;
        }
        for (int c = 0; c < K && ok; ++c) {
            // memcmp, not `==`: IEEE would call two NaNs unequal regardless of
            // representation. Parity is about the BITS.
            if (memcmp(&wm[c], &margin[(size_t)r * K + c], sizeof(float)) != 0) {
                if (reason) snprintf(reason, reason_len,
                    "oracle: MARGIN divergence on probe %d class %d (walker %.9g vs xgb %.9g)",
                    r, c, (double)wm[c], (double)margin[(size_t)r * K + c]);
                ok = 0;
            } else if (memcmp(&wt[c], &res_t[(size_t)r * K + c], sizeof(float)) != 0) {
                if (reason) snprintf(reason, reason_len,
                    "oracle: TRANSFORMED divergence on probe %d class %d (walker %.9g vs xgb %.9g)",
                    r, c, (double)wt[c], (double)res_t[(size_t)r * K + c]);
                ok = 0;
            }
        }
    }

    int hit = 0, leaves = 0;
    if (covered) {
        for (int i = 0; i < w->total_nodes; ++i) {
            if (!(w->nodes[i].meta & WALKER_META_IS_LEAF)) continue;
            ++leaves;
            if (covered[i >> 6] & (1ULL << (i & 63))) ++hit;
        }
    }

    XGDMatrixFree(dmat);
    free(covered); free(margin); free(rows);

    if (out_probes)       *out_probes = n;
    if (out_leaves_hit)   *out_leaves_hit = hit;
    if (out_leaves_total) *out_leaves_total = leaves;
    return ok;
}
// [END_CODE]
// [END_FUNCTION]_[TreeWalkerOracle_Verify]
//======================================================================
#endif  // USE_XGBOOST (TreeWalkerOracle_Verify)

//======================================================================
// [FUNCTION]_[Model_Load]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[backend-dispatched load — FNV checksum log, foxml_version format check (reject != MODEL_FORMAT_VERSION), fingerprint read, num_outputs probe via zero-row predict]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline int Model_Load(ModelHandle<F> *m, const char *path, int backend) {
    Model_Init(m);
    m->training_fingerprint[0] = '\0';

    if (!path || path[0] == '\0') return 0; // no path = disabled

    // compute simple file checksum for logging (FNV-1a, fast and dependency-free)
    // full SHA256 available via Fingerprint.hpp but would create circular include
    {
        FILE *cf = fopen(path, "rb");
        if (cf) {
            uint64_t hash = 14695981039346656037ULL; // FNV offset basis
            uint8_t buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), cf)) > 0)
                for (size_t i = 0; i < n; i++)
                    hash = (hash ^ buf[i]) * 1099511628211ULL;
            fclose(cf);
            fprintf(stderr, "[ML] model checksum: %016lx (%s)\n", (unsigned long)hash, path);
        }
    }

    // stash path for logging
    strncpy(m->model_path, path, sizeof(m->model_path) - 1);
    m->model_path[sizeof(m->model_path) - 1] = '\0';

#ifdef USE_XGBOOST
    // E.1.2.E leaf 3 — FLAT-WALKER backend, OPT-IN and PARITY-GATED (W-a/W-b, D-432).
    //
    // The walker NEVER activates on the operator's word alone. It activates only
    // if it reproduces the library BIT-FOR-BIT over the designed probe set at
    // load; otherwise the backend silently-but-loudly stays XGBOOST (behaviour
    // unchanged) and a failure flag surfaces. "Loud fallback" is deliberate: a
    // wrong prediction on a capital path is worth strictly less than no
    // speedup, so the burden of proof sits on the NEW path every boot, on the
    // operator's actual artifact — not on a fixture that resembles it.
    //
    // The oracle needs the library IN-PROCESS, which is why this whole branch
    // lives under USE_XGBOOST. A build without it cannot verify the walker, and
    // an unverifiable fast path is exactly what this gate exists to refuse — so
    // it falls through to the "not compiled in" tail below rather than
    // activating unchecked.
    if (backend == MODEL_BACKEND_FLAT_WALKER) {
        // 1) the ORACLE REFERENCE — the library booster, loaded exactly as the
        //    XGBOOST path would load it.
        BoosterHandle booster;
        if (XGBoosterCreate(NULL, 0, &booster) != 0) {
            fprintf(stderr, "[ML] walker: reference booster create failed: %s\n", XGBGetLastError());
            return 0;
        }
        if (XGBoosterLoadModel(booster, path) != 0) {
            fprintf(stderr, "[ML] walker: reference booster load failed for %s: %s\n",
                    path, XGBGetLastError());
            XGBoosterFree(booster);
            return 0;
        }

        // 2) the CANDIDATE — parse the artifact into the flat-SoA blob. The
        //    header rides its own load-time allocation (H1 load-path-sanctioned,
        //    same standing as the blob's aligned_alloc and the HotSwap precedent);
        //    steady state allocates nothing.
        FlatTreeModel* w = (FlatTreeModel*)calloc(1, sizeof(FlatTreeModel));
        if (!w) {
            fprintf(stderr, "[ML] walker: header alloc failed; staying on XGBOOST\n");
            m->handle = (void*)booster;
            m->backend = MODEL_BACKEND_XGBOOST;
            BITMAP_SET(m->drift_flags_at_load, FAILURE_MASK_ml_walker_parity_failed);
            {
                bst_ulong nf = 0;
                XGBoosterGetNumFeature(booster, &nf);
                m->num_features = (int)nf;
            }
            m->num_outputs = 1;
            return 1;
        }
        int prc = TreeWalker_ParseFromJson(w, path);
        if (prc != WALKER_OK) {
            // Class-49 disposition: a REFUSE code is not a parity failure. The
            // artifact is unsupported or corrupt, which is a different operator
            // action (retrain / re-export vs "the walker disagrees").
            fprintf(stderr, "[ML] walker: parse REFUSED (%d) for %s; staying on XGBOOST\n", prc, path);
            free(w);
            m->handle = (void*)booster;
            m->backend = MODEL_BACKEND_XGBOOST;
            BITMAP_SET(m->drift_flags_at_load, FAILURE_MASK_ml_walker_parity_failed);
            bst_ulong nf = 0;
            XGBoosterGetNumFeature(booster, &nf);
            m->num_features = (int)nf;
            m->num_outputs = 1;
            return 1;
        }

        // 3) the GATE.
        char why[256];
        int probes = 0, leaves_hit = 0, leaves_total = 0;
        int parity = TreeWalkerOracle_Verify<F>(w, booster, w->num_feature,
                                                why, sizeof(why),
                                                &probes, &leaves_hit, &leaves_total);
        if (parity) {
            // 4a) PROVEN — drop the library, serve from the blob.
            XGBoosterFree(booster);
            m->handle       = (void*)w;
            m->backend      = MODEL_BACKEND_FLAT_WALKER;
            m->num_features = w->num_feature;
            m->num_outputs  = w->num_class;
            fprintf(stderr,
                "[ML] walker ACTIVE: %s (%d trees, %d nodes, depth %d, %d class%s) — "
                "bit-parity PROVEN over %d probes; leaf coverage %d/%d\n",
                path, w->num_trees, w->total_nodes, w->max_depth, w->num_class,
                w->num_class == 1 ? "" : "es", probes, leaves_hit, leaves_total);

            // R4 — LOAD-TIME SERVE-COST WARN. The Limits.hpp caps are sized to the
            // BUDGET (2MB blob ceiling, depth ~10 headroom), deliberately not to
            // today's depth-2/7-node models, so an artifact can be perfectly legal
            // and still be a bad thing to serve per-cycle. The caps REFUSE; this
            // WARNS — the gap between "allowed" and "advisable" is exactly where a
            // silent latency regression lives, and the operator is the only one who
            // can decide whether a 1.5MB blob is worth its cache footprint.
            //
            // Both numbers are derived, not guessed: the blob is total_nodes x 16B
            // (the pinned FlatTreeNode size), and the per-prediction walk is
            // constant-iter, so steps = trees x max_depth EXACTLY — not an estimate.
            {
                const long blob_bytes = (long)w->total_nodes * (long)sizeof(FlatTreeNode);
                const long walk_steps = (long)w->num_trees * (long)w->max_depth;
                // 512KB ~ a typical L2 slice; past it the blob stops being
                // resident and the walk starts paying misses it cannot amortize.
                if (blob_bytes > 512L * 1024L) {
                    fprintf(stderr,
                        "[ML] walker WARN: blob is %ld KB (%d nodes x %zuB) — past the ~512KB "
                        "L2-residency guideline. Legal (cap is %d nodes) but the walk will take "
                        "cache misses every cycle; consider fewer/shallower trees.\n",
                        blob_bytes / 1024L, w->total_nodes, sizeof(FlatTreeNode),
                        WALKER_MAX_TOTAL_NODES);
                }
                if (walk_steps > 32768L) {
                    fprintf(stderr,
                        "[ML] walker WARN: %ld node-steps per prediction (%d trees x depth %d). "
                        "The walk is constant-iter, so this is the EXACT per-cycle cost, not an "
                        "average — it lands on the slow path every poll_interval ticks.\n",
                        walk_steps, w->num_trees, w->max_depth);
                }
            }
            if (leaves_total > 0 && leaves_hit * 2 < leaves_total) {
                // Not a refusal — a HONESTY line. Parity proven only where the
                // probes reached; say so rather than let "PROVEN" imply totality
                // (M10: this oracle is TOTAL over its probe set, PARTIAL over the
                // input space, and the operator should be told which).
                fprintf(stderr,
                    "[ML] walker NOTE: probes reached %d of %d leaves — parity is proven "
                    "over the probed paths, not over every reachable leaf.\n",
                    leaves_hit, leaves_total);
            }
            return 1;
        }

        // 4b) REFUSED — keep the library, raise the flag, say exactly why.
        fprintf(stderr,
            "[ML] walker REFUSED for %s — %s. Falling back to XGBOOST C API "
            "(predictions UNCHANGED); %d probes ran, leaf coverage %d/%d.\n",
            path, why[0] ? why : "parity check failed", probes, leaves_hit, leaves_total);
        TreeWalker_Free(w);
        free(w);
        m->handle  = (void*)booster;
        m->backend = MODEL_BACKEND_XGBOOST;
        BITMAP_SET(m->drift_flags_at_load, FAILURE_MASK_ml_walker_parity_failed);
        {
            bst_ulong nf = 0;
            XGBoosterGetNumFeature(booster, &nf);
            m->num_features = (int)nf;
        }
        m->num_outputs = 1;
        return 1;
    }

    if (backend == MODEL_BACKEND_XGBOOST) {
        BoosterHandle booster;
        int ret = XGBoosterCreate(NULL, 0, &booster);
        if (ret != 0) {
            fprintf(stderr, "[ML] XGBoost: failed to create booster: %s\n", XGBGetLastError());
            return 0;
        }
        ret = XGBoosterLoadModel(booster, path);
        if (ret != 0) {
            fprintf(stderr, "[ML] XGBoost: failed to load %s: %s\n", path, XGBGetLastError());
            XGBoosterFree(booster);
            return 0;
        }
        // set single-threaded for deterministic latency
        XGBoosterSetParam(booster, "nthread", "1");
        // version check — reject models trained with a different feature set
        const char *ver = NULL;
        int got_ver = XGBoosterGetAttr(booster, "foxml_version", &ver, (int[]){0});
        if (got_ver == 0 && ver) {
            int model_ver = atoi(ver);
            if (model_ver != MODEL_FORMAT_VERSION) {
                fprintf(stderr, "[ML] XGBoost: model %s was trained with format v%d, engine expects v%d — retrain required\n",
                        path, model_ver, MODEL_FORMAT_VERSION);
                XGBoosterFree(booster);
                return 0;
            }
        }
        // E.1.2.D (scan-2 NEW-1 back-stop) — a ZERO-TREE model is valid,
        // loadable JSON that can only predict base_score (the cancelled-train
        // husk shape; the trainer's save is now guarded, this catches
        // pre-guard artifacts and any other producer). WARN loud; refusal is
        // queued as an operator decision (strict-mode candidate) because
        // refusing here changes load semantics for husks already on disk.
        {
            int boosted_rounds = 0;
            if (XGBoosterBoostedRounds(booster, &boosted_rounds) == 0 &&
                boosted_rounds == 0) {
                fprintf(stderr, "[ML] WARN: %s loaded with ZERO trees — it can "
                        "only predict base_score (cancelled-train husk shape); "
                        "this arm's outputs are meaningless\n", path);
            }
        }
        // read training fingerprint (if embedded)
        const char *fp = NULL;
        int got_fp = XGBoosterGetAttr(booster, "foxml_fingerprint", &fp, (int[]){0});
        if (got_fp == 0 && fp) {
            strncpy(m->training_fingerprint, fp, 64);
            m->training_fingerprint[64] = '\0';
        }
        m->handle = (void*)booster;
        m->backend = MODEL_BACKEND_XGBOOST;
        m->num_features = MODEL_NUM_FEATURES;
        // detect num_outputs by running a single-row prediction with zeros.
        // for binary models out_len = 1; for multi:softprob out_len = num_class.
        // this is the stupid-proof check: lets the engine warn when a 3-class
        // model is loaded into a binary-config core (or vice versa).
        m->num_outputs = 1;
        {
            float zero_row[MODEL_MAX_FEATURES] = {0};
            DMatrixHandle probe;
            if (XGDMatrixCreateFromMat(zero_row, 1, MODEL_NUM_FEATURES, XGB_MISSING_VALUE, &probe) == 0) {
                bst_ulong out_len = 0;
                const float *out_result = NULL;
                if (XGBoosterPredict(booster, probe, 0, 0, 0, &out_len, &out_result) == 0) {
                    if (out_len > 0) m->num_outputs = (int)out_len;
                }
                XGDMatrixFree(probe);
            }
        }
        fprintf(stderr, "[ML] XGBoost model loaded: %s (%d features, %d output%s, format v%d%s%s)\n",
                path, m->num_features, m->num_outputs, m->num_outputs == 1 ? "" : "s",
                MODEL_FORMAT_VERSION,
                m->training_fingerprint[0] ? ", fingerprint: " : "",
                m->training_fingerprint[0] ? m->training_fingerprint : "");
        return 1;
    }
#endif

#ifdef USE_LIGHTGBM
    if (backend == MODEL_BACKEND_LIGHTGBM) {
        int num_iterations;
        BoosterHandle booster;
        int ret = LGBM_BoosterCreateFromModelfile(path, &num_iterations, &booster);
        if (ret != 0) {
            fprintf(stderr, "[ML] LightGBM: failed to load %s\n", path);
            return 0;
        }
        m->handle = (void*)booster;
        m->backend = MODEL_BACKEND_LIGHTGBM;
        m->num_features = MODEL_NUM_FEATURES;
        m->num_outputs = 1;
        LGBM_BoosterGetNumClasses(booster, &m->num_outputs);
        fprintf(stderr, "[ML] LightGBM model loaded: %s (%d features, %d output%s, %d iterations)\n",
                path, m->num_features, m->num_outputs, m->num_outputs == 1 ? "" : "s", num_iterations);
        return 1;
    }
#endif

    // backend requested but not compiled in
    if (backend != MODEL_BACKEND_NONE) {
        const char *names[] = {"none", "xgboost", "lightgbm", "aot"};
        const char *name = (backend >= 1 && backend <= 3) ? names[backend] : "unknown";
        fprintf(stderr, "[ML] backend '%s' requested but not compiled in (need -DUSE_%s=ON)\n",
                name, backend == 1 ? "XGBOOST" : (backend == 2 ? "LIGHTGBM" : "TREELITE_AOT"));
    }
    return 0;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[Model_Load]
//======================================================================

//======================================================================
// [FUNCTION]_[Model_Predict_Normalized]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [SLOW_PATH]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[Model_Predict + per-handle normalizer -> [0,1] buy-probability space; NORM_IDENTITY default is a ~1ns early return]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline float Model_Predict_Normalized(ModelHandle<F>* m,
                                        const float* features,
                                        int num_features) {
    float raw = Model_Predict(m, features, num_features);
    // Elision-friendly fast path: NORM_IDENTITY (default) → return raw.
    // Optimizer + branch predictor reduce this to ~1ns when default.
    if (m->normalizer == ModelHandle<F>::NORM_IDENTITY) return raw;

    switch (m->normalizer) {
        case ModelHandle<F>::NORM_REGRESSION: {
            // [-tp_pct, +tp_pct] → [0, 1]. tp from normalizer_param at load.
            float tp = m->normalizer_param;
            if (tp <= 0.0f) return 0.5f;  // defensive: invalid tp → neutral
            float v = 0.5f + raw / (2.0f * tp);
            return (v < 0.0f) ? 0.0f : (v > 1.0f ? 1.0f : v);
        }
        case ModelHandle<F>::NORM_BARRIER_CLASS_1:
            // Passthrough — Model_Predict already applied the handle's
            // buy_class_idx, whatever it is.
            //
            // E.1.2.C — the enum NAME and this comment's old text both said
            // "class 1", which was the INVERTED rule: for a PVS primary the
            // loader now sets 2 (valley = good entry) via
            // Model_PrimaryBuyClassIdx; only an EXIT handle keeps 1 (peak).
            // The name is kept because it is a pinned in-memory constant
            // (tests/controller_test.cpp pins == 2) and is NOT persisted, so
            // renaming buys nothing an H21 tombstone would protect — but do
            // not read it as documentation of which class is extracted.
            // This whole switch is unreachable in production regardless:
            // nothing writes `normalizer` off NORM_IDENTITY outside tests.
            return raw;
        case ModelHandle<F>::NORM_COMPOSITE:
            // Phase 3.A's composite extraction already ran inside
            // Model_Predict. Just clamp to [0, 1].
            return (raw < 0.0f) ? 0.0f : (raw > 1.0f ? 1.0f : raw);
        default:
            return raw;  // unrecognized → passthrough
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Wraps Model_Predict + applies the per-handle normalizer to map any
// model's raw output to a [0, 1] buy-probability space. Bandit ensemble
// blend can average normalized values across mixed model types
// (binary, regression, 3-class barrier).
//
// Elision-friendly: default NORM_IDENTITY → 1-line early return.
// Branch is heavily predicted (default state); ~1ns runtime cost when
// no model uses non-IDENTITY normalizer (= every existing operator
// model today). Switch on enum is entered only after a model is trained
// with a non-default label_kind that needs scale alignment.
//
// Strategy code unchanged per v5.11.62 invariant — strategy reads the
// (already-normalized) float and acts on it. Composition lives in
// Model_Predict + Model_Predict_Normalized; not in strategy.
//======================================================================
// [END_FUNCTION]_[Model_Predict_Normalized]
//======================================================================

//======================================================================
// [FUNCTION]_[Model_Predict_AtClass]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [SLOW_PATH]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[class-explicit predict — caller-supplied class_idx instead of m->buy_class_idx; decouples class extraction from role aliasing (v5.12.3.E foundation)]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline float Model_Predict_AtClass(ModelHandle<F>* m,
                                     const float* features,
                                     int num_features,
                                     int class_idx) {
    if (!m->handle) return 0.0f;
    tt::MlPredictTimer _mlt;   // E.1.2.E leaf 7 — ML_PREDICT attribution

    // E.1.2.E — walker path (class-explicit, like the XGBoost branch below).
    if (m->backend == MODEL_BACKEND_FLAT_WALKER) {
        const FlatTreeModel* w = (const FlatTreeModel*)m->handle;
        float out[WALKER_MAX_CLASSES];
        if (TreeWalker_Predict(w, features, out) != WALKER_OK) return 0.0f;
        int idx = class_idx;
        if (idx < 0 || idx >= w->num_class) idx = 0;
        return out[idx];
    }

#ifdef USE_XGBOOST
    if (m->backend == MODEL_BACKEND_XGBOOST) {
        BoosterHandle booster = (BoosterHandle)m->handle;
        DMatrixHandle dmat;
        int ret = XGDMatrixCreateFromMat(features, 1, num_features, XGB_MISSING_VALUE, &dmat);
        if (ret != 0) return 0.0f;
        bst_ulong out_len;
        const float *out_result;
        ret = XGBoosterPredict(booster, dmat, 0, 0, 0, &out_len, &out_result);
        XGDMatrixFree(dmat);
        if (ret != 0 || out_len == 0) return 0.0f;
        int idx = class_idx;
        if (idx < 0 || (unsigned long)idx >= out_len) idx = 0;
        return out_result[idx];
    }
#endif

    // R6 (E.1.2.E) — REFUSE rather than answer a DIFFERENT question.
    //
    // This used to fall through to Model_Predict, which extracts
    // m->buy_class_idx. The caller asked for class_idx; on any multiclass model
    // where class_idx != buy_class_idx that returned a confident probability
    // for the WRONG CLASS, with no error and no flag — and this function exists
    // precisely to decouple class extraction from role aliasing, so the
    // fallback contradicted its own purpose. On an exit-signal slot that is a
    // capital decision made on an inverted number (the PARITY-044 shape).
    //
    // The old comment defended it as "fine for LIGHTGBM since single-row
    // returns a scalar" — true for a 1-class LGBM model and silently false for
    // any other, which is the kind of narrow-case reasoning that reads as
    // general. A backend that cannot honour a class-explicit request must say
    // so; 0.0f is the codebase's no-signal value and every caller already
    // handles it.
    (void)class_idx; (void)num_features;
    fprintf(stderr,
        "[ML] Model_Predict_AtClass: backend %d cannot serve a class-explicit "
        "request (class %d); refusing rather than returning buy_class_idx %d.\n",
        m->backend, class_idx, m->buy_class_idx);
    return 0.0f;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Decouples the class-extraction concern from the role-aliasing concern.
// Strategy code (or ensemble blend) can ask for "this model's class N"
// without knowing role-name semantics (buy_signal vs barrier vs regime).
//
// Foundation for the v5.11.62 architectural cleanup: future loader
// refactor populates ezoo->primary_handles directly + sets per-handle
// buy_class_idx; consumers call Model_Predict_AtClass with the
// configured class index. Removes the tactical memcpy alias (which
// requires the borrowed flag bookkeeping).
//
// In this ship: just the helper. Loader integration + alias removal
// deferred to follow-up (when operator trains a 4th label kind that
// breaks the current tactical patch's assumptions).
//
// Behavior: identical to Model_Predict but uses caller-supplied
// class_idx instead of m->buy_class_idx. Default Model_Predict() ==
// Model_Predict_AtClass(m, features, n, m->buy_class_idx).
//======================================================================
// [END_FUNCTION]_[Model_Predict_AtClass]
//======================================================================

//======================================================================
// [FUNCTION]_[Model_PrimaryBuyClassIdx]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[buy-side class index for a primary-role handle — class 2 (valley = good entry) on a 3-class PVS model, class 0 on a binary one]
//======================================================================
// [CODE]
//======================================================================
static inline int Model_PrimaryBuyClassIdx(int num_outputs) {
    if (num_outputs >= 3) return 2;   // PVS-shaped: 0=stable, 1=peak, 2=valley
    if (num_outputs == 2) return 1;   // 2-class softmax: class 1 is the positive class
    return 0;                         // single output: the raw sigmoid IS P(good entry)
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// PEAK_VALLEY_STABLE order (LabelFunctions.hpp:305-306): 0=stable,
// 1=peak (down barrier first => we sampled at a HIGH => bad entry),
// 2=valley (up barrier first => we sampled at a LOW => good entry).
// So the ENTRY signal is class 2. Binary roles carry one output and the
// raw sigmoid already IS P(good entry), hence 0.
//
// This exists as a named function, and not as the inline ternary it
// replaces, because its exit-side sister below computes a DIFFERENT
// answer from the SAME input. The two ternaries previously sat 40 lines
// apart, looked identical, and one of them was wrong for 14 months —
// unspottable by reading either site alone.
//======================================================================
// [END_FUNCTION]_[Model_PrimaryBuyClassIdx]
//======================================================================

//======================================================================
// [FUNCTION]_[Model_ExitClassIdx]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[exit-side class index for an exit_predictor handle — class 1 (peak = good exit) on a 3-class PVS model, class 0 on a binary one]
//======================================================================
// [CODE]
//======================================================================
static inline int Model_ExitClassIdx(int num_outputs) {
    return (num_outputs >= 2) ? 1 : 0;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Class 1 (peak) is CORRECT here and the asymmetry with the buy sister
// is deliberate, not an oversight: you ENTER at a valley and you EXIT at
// a peak, so the same 3-class model answers two opposite questions. A
// binary exit model (WILL_PEAK, the leg-3 default) has one output that
// already IS P(peak), hence 0 — which is why a binary arm and a 3-class
// arm can share exit_predictor[] and still blend coherently.
//
// If these two ever return the same value, one of them is wrong.
//======================================================================
// [END_FUNCTION]_[Model_ExitClassIdx]
//======================================================================


//======================================================================
// [FUNCTION]_[Model_Predict]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [SLOW_PATH]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the single-row predict — backend dispatch, composite-signal extraction when num_classes_active > 1, buy-class selection for multiclass; 0.0f when no model]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline float Model_Predict(ModelHandle<F> *m, const float *features, int num_features) {
    if (!m->handle) return 0.0f;
    tt::MlPredictTimer _mlt;   // E.1.2.E leaf 7 — ML_PREDICT attribution

    // E.1.2.E walker path. Deliberately placed FIRST and outside USE_XGBOOST:
    // once parity is proven at load the walk is self-contained, so serving must
    // not depend on the library being compiled in. Composite extraction below
    // is a verbatim mirror of the XGBoost branch's — same semantics, same
    // out-of-range defensiveness — because a backend that quietly extracts a
    // DIFFERENT class than the library would defeat the very parity the load
    // gate just established.
    if (m->backend == MODEL_BACKEND_FLAT_WALKER) {
        const FlatTreeModel* w = (const FlatTreeModel*)m->handle;
        float out[WALKER_MAX_CLASSES];
        if (TreeWalker_Predict(w, features, out) != WALKER_OK) return 0.0f;
        const int n = w->num_class;
        if (m->num_classes_active > 1) {
            float acc = 0.0f;
            for (int i = 0; i < (int)m->num_classes_active && i < 8; ++i) {
                const int ci = m->target_classes[i];
                if (ci >= 0 && ci < n) acc += m->class_weights[i] * out[ci];
            }
            return acc;
        }
        const int idx = (m->buy_class_idx >= 0 && m->buy_class_idx < n) ? m->buy_class_idx : 0;
        return out[idx];
    }

#ifdef USE_XGBOOST
    if (m->backend == MODEL_BACKEND_XGBOOST) {
        BoosterHandle booster = (BoosterHandle)m->handle;
        DMatrixHandle dmat;
        // create single-row DMatrix from float array
        int ret = XGDMatrixCreateFromMat(features, 1, num_features, XGB_MISSING_VALUE, &dmat);
        if (ret != 0) return 0.0f;

        bst_ulong out_len;
        const float *out_result;
        ret = XGBoosterPredict(booster, dmat, 0, 0, 0, &out_len, &out_result);
        XGDMatrixFree(dmat);

        if (ret != 0 || out_len == 0) return 0.0f;
        // v5.12.3.A — composite-signal extraction. When num_classes_active > 1,
        // returns linear combination over target_classes[]. Out-of-range
        // class indices contribute 0 (defensive). When num_classes_active == 1
        // (default), falls through to the single-class path which is bytewise-
        // equivalent to v5.11.62 behavior.
        if (m->num_classes_active > 1) {
            float composite = 0.0f;
            uint8_t n = m->num_classes_active;
            if (n > 8) n = 8;
            for (uint8_t i = 0; i < n; ++i) {
                int cls = m->target_classes[i];
                if (cls < 0 || (unsigned long)cls >= out_len) continue;
                composite += m->class_weights[i] * out_result[cls];
            }
            return composite;
        }
        // v5.11.62 — for multiclass models (out_len > 1), return the
        // configured "buy class" probability instead of out_result[0].
        // Default buy_class_idx=0 preserves binary semantics.
        //
        // E.1.2.C — this comment used to read "loader sets buy_class_idx=1 …
        // class 1 = peak = price expected to rise = buy signal", which is the
        // inverted rule the session fixed, sitting on the live extraction line.
        // Per LabelFunctions.hpp the PVS order is 0=stable / 1=peak (down
        // barrier first = we sampled at a HIGH = BAD entry) / 2=valley (up
        // barrier first = GOOD entry). The loader now sets 2 for a PVS PRIMARY
        // handle and 1 for an EXIT handle — see Model_PrimaryBuyClassIdx and
        // Model_ExitClassIdx, which exist as two NAMED rules precisely because
        // one shared constant here read correct at whichever site you looked at.
        // Out-of-range index falls back to 0 (defensive).
        int idx = m->buy_class_idx;
        if (idx < 0 || (unsigned long)idx >= out_len) idx = 0;
        return out_result[idx];
    }
#endif

#ifdef USE_LIGHTGBM
    if (m->backend == MODEL_BACKEND_LIGHTGBM) {
        BoosterHandle booster = (BoosterHandle)m->handle;
        double out_result;
        int64_t out_len;
        // single-row prediction — fastest LGBM path
        int ret = LGBM_BoosterPredictForMatSingleRow(
            booster, features, C_API_DTYPE_FLOAT32,
            num_features, 1, // is_row_major
            C_API_PREDICT_NORMAL, 0, -1, "", // predict type, start iteration, num iteration, parameters
            &out_len, &out_result);
        if (ret != 0) return 0.0f;
        return (float)out_result;
    }
#endif

    return 0.0f;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// returns raw model output (probability for classifiers, value for regressors)
// returns 0.0f if no model loaded — caller should check Model_IsLoaded first
//======================================================================
// [END_FUNCTION]_[Model_Predict]
//======================================================================

//======================================================================
// [FUNCTION]_[Model_Predict_Ensemble]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [SLOW_PATH]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[v5.10.0a.G.4 multi-horizon argmax-confidence selection — the most-confident member's raw prediction wins (|p-0.5| binary / |p| regression); H20 cmov argmax]
// [REFERENCE]_[CLASS]_[28]
// [REFERENCE]_[INVARIANT]_[H20]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline float Model_Predict_Ensemble(ModelHandle<F> *models,
                                      int count,
                                      const float *features,
                                      int num_features,
                                      int *out_selected_idx = nullptr,
                                      float *out_per_arm_preds = nullptr) {
    // out_per_arm_preds (2026-08-22, B-12 cold-start dedupe): optional
    // per-arm export so callers needing the raw preds (G.8 reward records)
    // don't run a SECOND predict pass. Convention matches the historical
    // caller loop: unloaded arm -> 0.5f, loaded -> raw p (pre-NaN-skip).
    // Default nullptr = all existing callers byte-unchanged (out-param
    // only; no semantic default trap — a caller not wanting the export is
    // correct with null, which is why this default is AR-20-acceptable).
    if (count <= 0) {
        if (out_selected_idx) *out_selected_idx = -1;
        return 0.0f;
    }
    if (count == 1) {
        if (out_selected_idx) *out_selected_idx = 0;
        float p1 = Model_Predict(&models[0], features, num_features);
        if (out_per_arm_preds)
            out_per_arm_preds[0] = Model_IsLoaded(&models[0]) ? p1 : 0.5f;
        return p1;
    }

    float best_pred = 0.0f;
    float best_conf = -1.0f;  // sentinel: "no valid prediction yet"
    int   best_idx = 0;
    for (int i = 0; i < count; ++i) {
        if (!Model_IsLoaded(&models[i])) {
            if (out_per_arm_preds) out_per_arm_preds[i] = 0.5f;
            continue;
        }
        float p = Model_Predict(&models[i], features, num_features);
        if (out_per_arm_preds) out_per_arm_preds[i] = p;
        if (std::isnan(p) || std::isinf(p)) continue;
        // Confidence = |p - 0.5| for binary (centered at neutral), or
        // |p| for regression (zero = no edge). Both metrics: higher
        // value = more-confident model.
        float conf = (p > 1.0f || p < -1.0f) ? std::fabs(p)
                                              : std::fabs(p - 0.5f);
        // v5.15.5.F.4d Step 6 (§ L) — Class 28 cmov branchless argmax (H20).
        int win   = conf > best_conf;
        best_conf = win ? conf : best_conf;
        best_pred = win ? p    : best_pred;
        best_idx  = win ? i    : best_idx;
    }
    if (best_conf < 0.0f) {
        // No member produced a valid prediction; fall back to model[0]
        // raw output (matches single-model failure mode).
        if (out_selected_idx) *out_selected_idx = 0;
        return Model_Predict(&models[0], features, num_features);
    }
    if (out_selected_idx) *out_selected_idx = best_idx;
    return best_pred;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Predict via N independent ModelHandles trained as a multi-horizon
// ensemble. For each loaded model, compute prediction; return the
// HIGHEST-ABSOLUTE-DEVIATION-FROM-NEUTRAL prediction (the most-confident
// signal). Neutral = 0.5 for binary; this rule generalizes to "the
// model that's surest about its prediction wins."
//
// Selection logic:
//   - Binary models: distance from 0.5 = confidence; argmax(|p - 0.5|)
//   - Regression: |p| as confidence proxy (zero = no edge)
//
// Returns:
//   - The selected member's raw prediction value
//   - *out_selected_idx (optional): which member won (0..count-1)
//
// Operator-side workflow:
//   1. Train N horizons via Train Multi-Horizon (G.1)
//   2. Engine boot loads N models per role into EnsembleModelZoo (G.3)
//   3. Per slow-path predict, ensemble dispatch picks highest-confidence
//      model's output (this function)
//
// Latency: linear in N (each member predicts independently). Operator
// can measure via Item A timer; default cfg.horizon_count=0 keeps
// single-model path with zero overhead.
//
// Single-model fallback: if count <= 1, returns Model_Predict on
// models[0]. Safe to call from MLStrategy regardless of ensemble state.
//======================================================================
// [END_FUNCTION]_[Model_Predict_Ensemble]
//======================================================================

//======================================================================
// [FUNCTION]_[Model_Predict_Ensemble_Weighted]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [SLOW_PATH]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[v5.10.0a.G.7 Bandit-blend — NaN-skipped weighted average across arms, disabled-mask kill switch, agreement gate (split ensemble -> 0.5 no-edge sentinel), dominant-arm tracking]
// [REFERENCE]_[CLASS]_[28]
// [REFERENCE]_[INVARIANT]_[H20]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline float Model_Predict_Ensemble_Weighted(
    ModelHandle<F>* models,
    int count,
    const float* features,
    int num_features,
    const double* weights,
    uint32_t disabled_mask,
    double min_agreement_pct,
    int* out_dominant_idx,
    float* out_per_arm_predictions = nullptr) {  // v5.10.0a.G.8: optional buffer for reward record
    if (count <= 0) {
        if (out_dominant_idx) *out_dominant_idx = -1;
        return 0.0f;
    }
    if (count == 1) {
        if (disabled_mask & 1u) {
            if (out_dominant_idx) *out_dominant_idx = -1;
            return 0.5f;
        }
        if (out_dominant_idx) *out_dominant_idx = 0;
        return Model_Predict(&models[0], features, num_features);
    }

    // Local buffers sized to match ENSEMBLE_HORIZON_MAX (NodeModelZoo.hpp);
    // hardcoded 8 here to avoid circular include (this header is included
    // by NodeModelZoo.hpp).
    float predictions[8];
    int   valid[8];
    if (count > 8) count = 8;  // bound safety
    int   n_active = 0;
    int   n_long = 0, n_short = 0;
    for (int i = 0; i < count; ++i) {
        if (disabled_mask & (1u << i)) {
            valid[i] = 0;
            predictions[i] = 0.5f;
            continue;
        }
        if (!Model_IsLoaded(&models[i])) {
            valid[i] = 0;
            predictions[i] = 0.5f;
            continue;
        }
        float p = Model_Predict(&models[i], features, num_features);
        if (std::isnan(p) || std::isinf(p)) {
            valid[i] = 0;
            predictions[i] = 0.5f;
            continue;
        }
        valid[i] = 1;
        predictions[i] = p;
        n_active++;
        if (p > 0.5f) n_long++;
        else if (p < 0.5f) n_short++;
    }

    // Agreement filter (G.7 #5b safety check). Skips entry when ensemble
    // is internally split — high-conviction entries only.
    if (n_active > 0 && min_agreement_pct > 0.0) {
        double frac_long  = (double)n_long  / n_active;
        double frac_short = (double)n_short / n_active;
        double agreement  = (frac_long > frac_short) ? frac_long : frac_short;
        if (agreement < min_agreement_pct) {
            if (out_dominant_idx) *out_dominant_idx = -1;
            return 0.5f;  // no-edge sentinel; MLStrategy → no entry
        }
    }

    // Weighted blend across valid arms.
    double sum_w = 0.0, sum_wp = 0.0;
    double best_contrib = 0.0;
    int    best_idx = -1;
    for (int i = 0; i < count; ++i) {
        if (!valid[i]) continue;
        double w = weights[i];
        if (w <= 0.0) w = 1e-9;  // avoid zero-weight degenerate
        sum_w  += w;
        sum_wp += w * (double)predictions[i];
        // Track dominant arm: largest weight × |p - 0.5| contribution
        double contrib = w * std::fabs((double)predictions[i] - 0.5);
        // v5.15.5.F.4d Step 6 (§ L) — Class 28 cmov branchless argmax (H20).
        int win      = contrib > best_contrib;
        best_contrib = win ? contrib : best_contrib;
        best_idx     = win ? i       : best_idx;
    }
    if (sum_w <= 0.0 || n_active == 0) {
        // All-NaN or all-disabled: no signal. Fall back to first-loaded
        // model's raw predict for robustness (matches single-model failure
        // mode); if even that fails caller sees 0.0 / NaN.
        if (out_dominant_idx) *out_dominant_idx = -1;
        return 0.5f;
    }
    if (out_dominant_idx) *out_dominant_idx = best_idx;
    // v5.10.0a.G.8 — expose per-arm predictions for reward record write
    if (out_per_arm_predictions) {
        for (int i = 0; i < count; ++i)
            out_per_arm_predictions[i] = predictions[i];
    }
    return (float)(sum_wp / sum_w);
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Run prediction across N independent boosters; combine via weighted
// blend: final = Σ weight_i × pred_i / Σ weight_i (NaN-skipped + agreement-
// gated). Replaces G.4's argmax-confidence selection when operator sets
// ensemble_blend_mode=weighted (default).
//
// Inputs:
//   models: array of N independent ModelHandles (caller's responsibility
//           to ensure they share the same scaler — true by G.3 LoadFromCfg
//           invariant)
//   count: how many models populated (1..ENSEMBLE_HORIZON_MAX)
//   weights: per-arm weights from BanditState (already-normalized
//            probabilities, OR raw weights — function renormalizes)
//   disabled_mask: bit i set = skip horizon i (operator kill-switch via
//                  cfg.core_N_disabled_horizons, parsed by
//                  EnsembleModelZoo_SetDisabledHorizons)
//   min_agreement_pct: ≥X fraction of non-disabled horizons must predict
//                       same direction OR return 0.5 (no-edge sentinel,
//                       MLStrategy treats as no-entry). 0.0 = disabled.
//
// Outputs:
//   *out_dominant_idx: which arm contributed most to signal direction
//                       (argmax weight × |p − 0.5|); -1 if no entry
//   Returns: blended prediction, OR 0.5 if agreement check failed.
//
// Latency: linear in N (each model predicts independently). G.7 perf
// optimization #1: features are pre-standardized once before this call;
// each Model_Predict skips its own scaler.
//======================================================================
// [END_FUNCTION]_[Model_Predict_Ensemble_Weighted]
//======================================================================

//======================================================================
// [FUNCTION]_[Model_PredictMulti]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [SLOW_PATH]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[multi-class softmax output — fills out_buf with up to max_outputs class probabilities; 0 on failure, buf undisturbed]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline int Model_PredictMulti(ModelHandle<F> *m, const float *features, int num_features,
                               float *out_buf, int max_outputs) {
    if (!m->handle || max_outputs <= 0) return 0;
    tt::MlPredictTimer _mlt;   // E.1.2.E leaf 7 — ML_PREDICT attribution

    // E.1.2.E — full per-class vector from the walker. This is the shape the
    // composite consumers and the queued per-class blend need; a scalar-only
    // walker would have blocked that work (plan Shape, Option A).
    if (m->backend == MODEL_BACKEND_FLAT_WALKER) {
        const FlatTreeModel* w = (const FlatTreeModel*)m->handle;
        float out[WALKER_MAX_CLASSES];
        if (TreeWalker_Predict(w, features, out) != WALKER_OK) return 0;
        const int n = w->num_class < max_outputs ? w->num_class : max_outputs;
        for (int i = 0; i < n; ++i) out_buf[i] = out[i];
        return n;
    }

#ifdef USE_XGBOOST
    if (m->backend == MODEL_BACKEND_XGBOOST) {
        BoosterHandle booster = (BoosterHandle)m->handle;
        DMatrixHandle dmat;
        int ret = XGDMatrixCreateFromMat(features, 1, num_features, XGB_MISSING_VALUE, &dmat);
        if (ret != 0) return 0;

        bst_ulong out_len;
        const float *out_result;
        // XGBoost returns N×K floats for multi:softprob (N=1 row, K=num_class)
        // for binary objective, returns N floats (same as Model_Predict)
        ret = XGBoosterPredict(booster, dmat, 0, 0, 0, &out_len, &out_result);
        XGDMatrixFree(dmat);

        if (ret != 0 || out_len == 0) return 0;
        int n = (int)out_len < max_outputs ? (int)out_len : max_outputs;
        for (int i = 0; i < n; i++) out_buf[i] = out_result[i];
        return n;
    }
#endif

#ifdef USE_LIGHTGBM
    if (m->backend == MODEL_BACKEND_LIGHTGBM) {
        BoosterHandle booster = (BoosterHandle)m->handle;
        // need to know num_class first — query the booster
        int num_class = 1;
        LGBM_BoosterGetNumClasses(booster, &num_class);
        int n = num_class < max_outputs ? num_class : max_outputs;
        // LightGBM returns doubles, need a temp buffer
        double tmp[32];
        if (n > 32) n = 32; // safety clamp
        int64_t out_len;
        int ret = LGBM_BoosterPredictForMatSingleRow(
            booster, features, C_API_DTYPE_FLOAT32,
            num_features, 1,
            C_API_PREDICT_NORMAL, 0, -1, "",
            &out_len, tmp);
        if (ret != 0) return 0;
        int written = (int)out_len < n ? (int)out_len : n;
        for (int i = 0; i < written; i++) out_buf[i] = (float)tmp[i];
        return written;
    }
#endif

    return 0;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// fills `out_buf` with up to max_outputs class probabilities. returns the number
// of class outputs actually written (== num_class for the loaded model). on
// failure or no model loaded, returns 0 and leaves buf undisturbed.
//
// for binary classifiers, prefer Model_Predict — this works for them too but
// returns 1 output. the function is intended for models trained with
// objective=multi:softprob (XGBoost) or objective=multiclass (LightGBM).
//======================================================================
// [END_FUNCTION]_[Model_PredictMulti]
//======================================================================

//======================================================================
// [FUNCTION]_[Model_Free]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[backend-dispatched booster free; resets handle + backend to NONE (Model_IsLoaded rides below)]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline void Model_Free(ModelHandle<F> *m) {
    if (!m->handle) return;

    // E.1.2.E — the walker blob rides `handle` like any other backend payload,
    // so every existing free path (zoo teardown, hot-swap shadow release,
    // Model_Load re-entry) reclaims it for free. TWO frees, not one: the blob
    // (nodes + roots, one aligned_alloc) and the header that points at it.
    if (m->backend == MODEL_BACKEND_FLAT_WALKER) {
        FlatTreeModel* w = (FlatTreeModel*)m->handle;
        TreeWalker_Free(w);
        free(w);
        m->handle = NULL;
        m->backend = MODEL_BACKEND_NONE;
        return;
    }

#ifdef USE_XGBOOST
    if (m->backend == MODEL_BACKEND_XGBOOST) {
        XGBoosterFree((BoosterHandle)m->handle);
    }
#endif

#ifdef USE_LIGHTGBM
    if (m->backend == MODEL_BACKEND_LIGHTGBM) {
        LGBM_BoosterFree((BoosterHandle)m->handle);
    }
#endif

    m->handle = NULL;
    m->backend = MODEL_BACKEND_NONE;
}

template <unsigned F>
inline int Model_IsLoaded(const ModelHandle<F> *m) {
    return m->backend != MODEL_BACKEND_NONE && m->handle != NULL;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[Model_Free]
//======================================================================

//======================================================================
// [FUNCTION]_[ModelFeatures_Pack]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[DEPRECATED FROZEN packer — the equivalence-test reference for Features_PackAll (FeatureRegistry.hpp); NEVER change this body, change Features_PackAll + re-pin the hash]
// [REFERENCE]_[TECH_DEBT]_[TECH_DEBT-2]
//======================================================================
// [CODE]
//======================================================================
// Feature order is defined by FEAT_* constants — must match training pipeline.
// forward-declare RegimeSignals to avoid circular include.
template <unsigned F> struct RegimeSignals; // forward declaration

template <unsigned F>
inline int ModelFeatures_Pack(float *buf, const RegimeSignals<F> *sig,
                               const RollingStats<F> *r,
                               const RollingStats<F, 512> *r_long) {
    buf[FEAT_SHORT_SLOPE]    = (float)FPN_ToDouble(sig->short_slope);
    buf[FEAT_SHORT_R2]       = (float)FPN_ToDouble(sig->short_r2);
    buf[FEAT_SHORT_VARIANCE] = (float)FPN_ToDouble(sig->short_variance);
    buf[FEAT_LONG_SLOPE]     = (float)FPN_ToDouble(sig->long_slope);
    buf[FEAT_LONG_R2]        = (float)FPN_ToDouble(sig->long_r2);
    buf[FEAT_LONG_VARIANCE]  = (float)FPN_ToDouble(sig->long_variance);
    buf[FEAT_VOL_RATIO]      = (float)FPN_ToDouble(sig->vol_ratio);
    buf[FEAT_ROR_SLOPE]      = (float)FPN_ToDouble(sig->ror_slope);
    buf[FEAT_VOLUME_SLOPE]   = (float)FPN_ToDouble(sig->volume_slope);
    buf[FEAT_VOLUME_DELTA]   = (float)FPN_ToDouble(sig->volume_delta);
    buf[FEAT_EMA_SMA_SPREAD] = (float)FPN_ToDouble(sig->ema_sma_spread);
    buf[FEAT_VWAP_DEV]       = (float)FPN_ToDouble(r->vwap_deviation);
    buf[FEAT_PRICE_STDDEV]   = (float)FPN_ToDouble(r->price_stddev);
    buf[FEAT_PRICE_AVG]      = (float)FPN_ToDouble(r->price_avg);
    buf[FEAT_VOLUME_AVG]     = (float)FPN_ToDouble(r->volume_avg);
    buf[FEAT_EMA_ABOVE_SMA]  = (float)sig->ema_above_sma;
    // v4.3 — medium-horizon features
    buf[FEAT_MID_SLOPE]      = (float)FPN_ToDouble(sig->mid_slope);
    buf[FEAT_MID_R2]         = (float)FPN_ToDouble(sig->mid_r2);
    buf[FEAT_CUMDELTA]       = (float)FPN_ToDouble(sig->cumdelta);
    buf[FEAT_HOUR_SIN]       = (float)sig->hour_sin;
    buf[FEAT_HOUR_COS]       = (float)sig->hour_cos;
    buf[FEAT_VOL_REGIME_RAT] = (float)FPN_ToDouble(sig->vol_regime_ratio);
    buf[FEAT_TICK_RATE_Z]    = (float)sig->tick_rate_z;
    buf[FEAT_DIST_TO_HIGH]   = (float)FPN_ToDouble(sig->dist_to_high);
    buf[FEAT_DIST_TO_LOW]    = (float)FPN_ToDouble(sig->dist_to_low);
    // v4.5 Wave 1 — microstructure features (D.1, D.2, D.4)
    buf[FEAT_BOOK_IMB_MEAN_SHORT] = (float)FPN_ToDouble(sig->book_imb_mean_short);
    buf[FEAT_BOOK_IMB_MEAN_LONG]  = (float)FPN_ToDouble(sig->book_imb_mean_long);
    buf[FEAT_BOOK_IMB_DRIFT]      = (float)FPN_ToDouble(sig->book_imb_drift);
    buf[FEAT_FLOW_10S]            = (float)sig->flow_10s;
    buf[FEAT_FLOW_1M]             = (float)sig->flow_1m;
    buf[FEAT_FLOW_5M]             = (float)sig->flow_5m;
    buf[FEAT_LARGE_TRADE_Z]       = (float)sig->large_trade_z;
    // v4.6 Wave 2 — spread features (D.3)
    buf[FEAT_SPREAD_BPS]          = (float)sig->spread_bps;
    buf[FEAT_SPREAD_ZSCORE]       = (float)sig->spread_zscore;
    return MODEL_NUM_FEATURES;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Replaced by Features_PackAll in ML_Headers/FeatureRegistry.hpp (v5.8.1b).
// All 5 production callers (MLStrategy, StrategyParameters dispatcher,
// BacktestSharded, PortfolioController regime/barrier paths) flipped at
// v5.8.1b ship time.
//
// This function is now a frozen historical reference, kept ONLY so the
// EXTENSIBILITY equivalence test in controller_test.cpp can validate that
// Features_PackAll produces bytewise-identical output. Treat any change
// to this body as breaking the regression contract — change Features_PackAll
// instead, then re-pin the FEATURE_REGISTRY_HASH snapshot.
//
// (The original v5.8.1b note scheduled "full removal in v5.9" — that
// schedule lapsed BY CHOICE: the frozen packer is deliberately retained
// as the equivalence-test reference; its retirement rides with retiring
// the equivalence test itself, tracked alongside TECH_DEBT-002's legacy
// PortfolioController removal era. Frozen at 34 features by design —
// the equivalence pins the legacy range.)
//======================================================================
// [END_FUNCTION]_[ModelFeatures_Pack]
//======================================================================

//----------------------------------------------------------------------
// [SECTION]_[v5.2.0 — held-out gate: model stamp verification]
//----------------------------------------------------------------------

//======================================================================
// [STRUCT]_[ModelStampResult]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [PERSISTENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the stamp PARSE side — runtime verdict fields + bit-packed has_flags + X-macro-generated architectural/cfg-derived/late-emit value fields]
// [REFERENCE]_[INVARIANT]_[[H9] [H18]]
// [REFERENCE]_[DESIGN_SPEC]_[registry-tuple-as-single-source-of-truth]
// [REFERENCE]_[TECH_DEBT]_[TECH_DEBT-6]
//======================================================================
// [CODE]
//======================================================================
struct ModelStampResult {
    // v5.15.5.F.4d.1.B.3 Step 1.6.3 — F=64 brought into struct scope for STAMP_RESULT_DERIVED_FIELDS_AUTO_GEN()
    // expansion (master STORAGE_T includes FPN_Binary<F>; expansion needs F in scope).
    // Future: when ModelStampResult is templated, replace with template parameter.
    static constexpr unsigned F = 64;

    // === Runtime-only fields (NOT in registry) ===
    int      valid;             // 1 / 0 / -1 per above
    char     reason[256];       // human-readable failure reason
    // s5 BT-8 — the stamp's model_sha256, surfaced instead of staying a local.
    // It is already VERIFIED against the file's actual hash below (a mismatch
    // fails the stamp outright), so by the time a caller sees it, it is a
    // content identity for these exact model bytes. Empty for a stamp that
    // lacks the key. NOT a new fact — just one that stops being thrown away.
    char     model_sha256[80];
    int      model_format_version;
    double   generalization_gap;
    double   gap_threshold;
    uint64_t feature_registry_hash;  // v5.8.1a: 0 if absent (old stamps)
    char     engine_version[16];     // v5.8.6: SemVer string at training time, "" if absent.
                                     // v5.15.5.F.4d.1.B.3 — INTENTIONAL TRUNCATION at 15 chars +
                                     // null. The engine_version field is used for cross-major
                                     // version check (first integer digit; verify_model_stamp)
                                     // + operator-facing display ("unknown" if absent). Full
                                     // version string preservation is NOT a design requirement.
                                     // Wire body retains the full string (HMAC chain unaffected);
                                     // parser truncation is per-design. Tests use strncmp(..., 15)
                                     // to match this contract (see v5.8.6 + v5.8.8 sections).
                                     // Catches: silent str compare drift between truncated parsed
                                     // value vs full ENGINE_VERSION_STRING macro (caught at .B.3).
    int      stamp_format_version;   // v5.9.0: schema version of the stamp body itself.
                                     //         0 if absent (v5.8.x and older).
                                     //         1 = current (v5.9.0+).
    int      inference_cfg_drift_count;  // v5.9.2b runtime: 0 if all match; >0 = mismatched fields
    uint8_t  cross_major_engine;     // v5.9.2b runtime: stamp's engine_version differs by major

    // === Bit-packed has_* flags (v5.14.8.A.merged.1) ===
    // ⚠ NO INVENTORY HERE ON PURPOSE. This comment used to enumerate "13 bits — 6 group +
    // 7 standalone" and name them. By 2026-08-17 every part of that was false: `fees` was
    // deleted (2026-08-16), `inference_cfg_bandit_blend_ratio` was deleted (D-426),
    // `environment_meta` was never listed, and the real total had moved. It survived BOTH
    // deletions' sweeps because a prose inventory is invisible to every guard.
    // Re-derive from the SSoT instead — `enum StampHasFlagBit` in
    // StampBoundModelConstRegistry.hpp, whose terminal `STAMP_BIT_COUNT` is the count.
    uint64_t has_flags;

    // === Architectural value fields — auto-generated from FOREACH_STAMP_BOUND_MODEL_CONST ===
    // 26 entries (v5.14.8.A.0.b restored 3 dropped fields; v5.14.8.A.merged added xgb_tree_method).
    #define X(name, group, presence, type, fmt, default_val, get_value, emit_when, doc) \
        type name;
    FOREACH_STAMP_BOUND_MODEL_CONST(X)
    #undef X

    // === Derived cfg fields — auto-generated from 4 master cfg registries (per-core + global + ml_cfg_flag + gate_cfg_flag) ===
    // v5.15.5.F.4d.1.B.3 Step 1.6.3 (Decision C Approach A; codified at v1.12 plan body) —
    // UNCONDITIONAL struct-gen replaces legacy FOREACH_STAMP_BOUND_CFG walker. Sister to
    // populate_stamp_cfg_from_derived + drift_check_from_derived + populate_inference_cfg_from_derived
    // (4-of-4 cfg-derived consumer family) — per option (e) framework consolidation.
    //
    // Generates ~163 fields (uint8_t has_<name> + STORAGE_T <name> per row across 4 registries).
    // Bounded .bss cost (~1-1.5KB per struct). Per-entry Surface G has_<name> semantic preserved.
    // Walkers (parse / emit / drift / inference_cfg populate) filter by STAMP_BOUND_CFG_DERIVED bit
    // at use sites — struct holds ALL fields; walkers process flagged subset.
    //
    // EXCLUSION REDIRECT (H18 SIDECAR; Pillar B13 cross-walker struct-field uniqueness):
    // 3 names (xgb_*) collide with FOREACH_STAMP_BOUND_MODEL_CONST walker above. Per
    // FOREACH_STAMP_RESULT_FIELD_EXCLUSION sidecar in CfgGateRegistry.hpp, redirect them to dead
    // `_stamp_result_excluded_<name>` fields during master walker expansion. Real fields come from
    // MODEL_CONST walker (training-time semantic preserved). ~96 bytes wasted; bounded.
    // CI tool check_struct_field_uniqueness.py catches future collisions automatically.
    #define xgb_min_child_weight _stamp_result_excluded_xgb_min_child_weight
    #define xgb_seed             _stamp_result_excluded_xgb_seed
    #define xgb_train_nthread    _stamp_result_excluded_xgb_train_nthread
    STAMP_RESULT_DERIVED_FIELDS_AUTO_GEN()
    #undef xgb_min_child_weight
    #undef xgb_seed
    #undef xgb_train_nthread

    // === Late-emit architectural fields auto-generated from FOREACH_STAMP_BOUND_MODEL_CONST_POST_CFG ===
    // (v5.14.8.A.merged.4 — TECH_DEBT-006 full closure). 6 entries:
    // expected_num_classes, expected_role, expected_num_features,
    // expected_feature_format_version, overlay_hash, effective_hash.
    // has_* flags bit-packed in has_flags (above); typed value fields
    // declared via X-macro below. Entries emit AFTER FOREACH_STAMP_BOUND_CFG
    // to preserve canonical wire format.
    //
    // Note: ModelStampResult includes ALL POST_CFG entries (parser side
    // sees everything). FOREACH_STAMP_BOUND_MODEL_CONST already walked
    // both halves above (union expansion).
};
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// v5.14.8.A.merged — ModelStampResult migrated to X-macro generation
// from FOREACH_STAMP_BOUND_MODEL_CONST registry. has_* flags bit-packed
// into uint64_t has_flags (v5.14.8.A.merged.1 bit allocation enum).
// All 26 architectural fields auto-flow from the registry; future field
// addition = 1 row in registry → struct field + parser + emitter +
// AUTOPOPULATE wiring all auto-derived. Closes TECH_DEBT-006.
//
// Caller migration: `r.has_<X>` → `STAMP_HAS(r, <group_or_entry>)`;
// field reads continue as `r.<canonical_name>`.
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-08-23]
//----------------------------------------------------------------------
// [SIZE]_[5296B]
// [ALIGN]_[16]
// [CACHE_LINES]_[83]
// [STRADDLE]_[unverified: scaler_sha256 overlay_hash effective_hash scaler_fit_data_hash removal_reasons_csv environment_pytorch_version environment_cpu_model]
//======================================================================
// [END_STRUCT]_[ModelStampResult]
//======================================================================

//======================================================================
// [FUNCTION]_[sha256_file_hex]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [DETERMINISM]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[verify helpers (stamp_parse_line rides) — in-process EVP SHA-256 file hash + the key=value line splitter]
//======================================================================
// [CODE]
//======================================================================
inline int sha256_file_hex(const char* path, char* hex_out, size_t hex_cap) {
    return tt::sha256_file_hex_inproc(path, hex_out, hex_cap);
}

// Parse a "key=value" line into key + value pointers. Returns 1 on success.
// Modifies `line` in place (NUL-terminates the key at '=').
inline int stamp_parse_line(char* line, const char** key_out, const char** val_out) {
    char* eq = strchr(line, '=');
    if (!eq) return 0;
    *eq = '\0';
    *key_out = line;
    *val_out = eq + 1;
    // Trim trailing newline from value
    char* nl = strchr((char*)*val_out, '\n');
    if (nl) *nl = '\0';
    return 1;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// sha256_file_hex: computes SHA-256 of a file. Reads in 64K chunks, safe
// for any size. Out parameter `hex` must be at least 65 bytes (64 hex
// digits + NUL).
//
// v5.3.0 Phase B: now an in-process EVP wrapper. Was a popen("sha256sum ...")
// shell-out in v5.2.0 — replaced for speed, shell-injection safety, and
// removing the dependency on /usr/bin/sha256sum being installed.
//======================================================================
// [END_FUNCTION]_[sha256_file_hex]
//======================================================================

// TOMBSTONE (H21 / TECH_DEBT-238, retired 2026-07-17) — FOREACH_LEGACY_PREFIXED_KEY.
// The v1→v2 wire-key back-compat dispatch (16 `inference_cfg_*` → unprefixed pairs, SOFT-bump
// Layer 6b, Step 1.6.7.4) lived here. The TECH_DEBT-237 pre-epoch floor (check 0c in
// verify_model_stamp) hard-refuses every stamp that could carry those keys, making the layer
// dead code by construction → deleted per H21 (remove dead code; never leave it compiled-in).
// The legacy `inference_cfg_*` wire keys are RETIRED IDENTIFIERS: never re-accept them with
// different semantics — a resurrected v1-era key means a new name, not this slot.
// (The cited "v1 LOAD test fixture" never existed — rg-verified at the fix ship; the floor
// tests in controller_test.cpp are the standing re-introduction guard.)

//======================================================================
// [FUNCTION]_[verify_model_stamp]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [DETERMINISM]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the stamp INGEST gate — parse key=value body, registry-driven field dispatch, drift checks (format/feature-hash/label-hash/feature-mask/gap), model-sha pin, H9 HMAC-SHA256 verify]
// [REFERENCE]_[INVARIANT]_[[H9] [H21] [H20]]
// [FUTURE_WORK]_[TECH_DEBT]_[TECH_DEBT-237]
// [REFERENCE]_[CLASS]_[18]
// [REFERENCE]_[DECISION]_[D-174]
// [REFERENCE]_[TECH_DEBT]_[[TECH_DEBT-3] [TECH_DEBT-237]]
//======================================================================
// [CODE]
//======================================================================
inline ModelStampResult verify_model_stamp(const char* model_path,
                                            const char* secret,
                                            double gap_threshold,
                                            int expected_format_version,
                                            uint64_t expected_feature_registry_hash = 0,
                                            uint64_t expected_label_registry_hash = 0,
                                            // v5.11.18a — feature_mask of the core
                                            // loading the model. 0 = skip check
                                            // (legacy callers + default mask).
                                            uint64_t expected_feature_mask = 0) {
    // v5.14.8.A.merged — Brace-init zero-fills the entire struct in one
    // statement. Replaces ~90 lines of explicit field zero-inits.
    // - has_flags = 0 (all bits clear; legacy stamps load with all
    //   group/standalone bits 0 → drift checks skip silently)
    // - All char arrays = "" (zero-filled)
    // - All numeric fields = 0
    // - FOREACH_STAMP_BOUND_CFG fields' has_* + value = 0
    // - Late-emit manual fields (expected_*, overlay_hash, effective_hash) = 0
    //
    // After brace-init, set the runtime-only fields that need non-zero
    // initial values (valid + gap_threshold from caller param).
    ModelStampResult r{};
    r.valid = -1;
    r.gap_threshold = gap_threshold;

    char stamp_path[512];
    snprintf(stamp_path, sizeof(stamp_path), "%s.stamp", model_path);
    FILE* f = fopen(stamp_path, "r");
    if (!f) {
        snprintf(r.reason, sizeof(r.reason), "stamp file missing: %s", stamp_path);
        return r;
    }

    // Read the whole stamp into a buffer (cap 4KB)
    char buf[4096] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) {
        r.valid = 0;
        snprintf(r.reason, sizeof(r.reason), "empty stamp file");
        return r;
    }
    buf[n] = '\0';

    // Parse line-by-line. Capture each key/value into r and detect the
    // signature line. Build the canonical "signed body" (everything before
    // signature= line) for HMAC verify.
    char canonical[4096] = {0};
    size_t canonical_len = 0;
    char model_sha[80] = {0};
    char stamp_sig[128] = {0};

    char* save = nullptr;
    char* line = strtok_r(buf, "\n", &save);
    while (line) {
        char line_copy[512] = {0};
        size_t lc = strlen(line);
        if (lc >= sizeof(line_copy)) lc = sizeof(line_copy) - 1;
        memcpy(line_copy, line, lc);
        line_copy[lc] = '\0';

        const char* key;
        const char* val;
        if (stamp_parse_line(line_copy, &key, &val)) {
            if (strcmp(key, "signature") == 0) {
                size_t vl = strlen(val);
                if (vl >= sizeof(stamp_sig)) vl = sizeof(stamp_sig) - 1;
                memcpy(stamp_sig, val, vl);
                stamp_sig[vl] = '\0';
                break;  // signature is the last line; stop accumulating canonical
            }
            // Add this line to canonical (in original "key=val\n" form)
            int wrote = snprintf(canonical + canonical_len,
                                  sizeof(canonical) - canonical_len,
                                  "%s=%s\n", key, val);
            if (wrote > 0 && (size_t)wrote < sizeof(canonical) - canonical_len) {
                canonical_len += wrote;
            }
            // Capture fields we care about
            if (strcmp(key, "model_format_version") == 0) {
                r.model_format_version = atoi(val);
            } else if (strcmp(key, "model_sha256") == 0) {
                size_t vl = strlen(val);
                if (vl >= sizeof(model_sha)) vl = sizeof(model_sha) - 1;
                memcpy(model_sha, val, vl);
                model_sha[vl] = '\0';
            } else if (strcmp(key, "gap") == 0) {
                r.generalization_gap = tt::parse_double_fast(val);
            } else if (strcmp(key, "gap_threshold") == 0) {
                r.gap_threshold = tt::parse_double_fast(val);
            } else if (strcmp(key, "feature_registry_hash") == 0) {
                // v5.8.1a: parse hex-encoded 64-bit hash. strtoull accepts
                // 0x-prefix or bare hex. Stamp emits %016lx (no prefix).
                r.feature_registry_hash = (uint64_t)strtoull(val, nullptr, 16);
            } else if (strcmp(key, "engine_version") == 0) {
                // v5.8.6: SemVer string captured at training time (e.g. "5.8.5").
                // Empty / missing for stamps written by pre-v5.8.6 callers.
                size_t vl = strlen(val);
                if (vl >= sizeof(r.engine_version)) vl = sizeof(r.engine_version) - 1;
                memcpy(r.engine_version, val, vl);
                r.engine_version[vl] = '\0';
            } else if (strcmp(key, "stamp_format_version") == 0) {
                // v5.9.0: stamp body schema version. 0 means absent (legacy);
                // current = 1. Future schema changes bump this. Verifier
                // could reject unknown versions in strict mode (deferred to
                // a future ship; for now we just record the value).
                r.stamp_format_version = atoi(val);
            }
            // v5.15.0.B — Registry-driven PRE_CFG parser dispatch.
            // Replaces ~120 LOC of manual if-else branches with one X-macro
            // walk over FOREACH_STAMP_BOUND_MODEL_CONST_PRE_CFG. Mirrors the
            // POST_CFG pattern below (v5.14.8.A.merged.4); both halves now
            // registry-driven. Closes the Class 18 parser/emit mirror;
            // closes TECH_DEBT-003.
            //
            // tt::stamp_parse_field<T> dispatches per-type via if-constexpr
            // (char[N] strncpy / double parse_double_fast / unsigned strtoull
            // / signed atoi). For unsigned types, base is auto-detected from
            // the registry's `fmt` column (DRY: fmt is the single source of
            // truth for emit AND parse format). Hex-encoded uint64 fields
            // (build_flags_hash, label_registry_hash, feature_mask) emit via
            // "%016lx" → parser auto-uses strtoull base 16. Future hex fields
            // auto-flow with no manual branches. Group has_* bit set via
            // STAMP_PARSER_SET_HAS_##group.
            //
            // EXCEPTION — feature_scaler_present manual branch retained:
            // defensive truthy normalization (any non-zero → 1) protects
            // against malformed stamps with non-0/1 values. Production
            // emit always produces 0 or 1, so the exception is defensive
            // coding; the X-macro chain would store the raw uint8_t value
            // instead. Bounded; keep as-is.
            else if (strcmp(key, "feature_scaler_present") == 0) {
                r.feature_scaler_present = (atoi(val) != 0) ? 1 : 0;
                STAMP_SET(r, scaler);
            }
            // === Registry-driven PRE_CFG dispatch (all entries auto-flow) ===
            // Future PRE_CFG additions = 1 row in FOREACH_STAMP_BOUND_MODEL_CONST_PRE_CFG;
            // parser auto-derives. Per-type dispatch via tt::stamp_parse_field<T>
            // (CLAUDE.md item 23 — type-trait dispatch via templated helper).
            // Hex-encoded fields auto-flow via fmt-based base detection.
            #define X(name, group, presence, type, fmt, default_val, get_value, emit_when, doc) \
                else if (strcmp(key, #name) == 0) { \
                    tt::stamp_parse_field(r.name, val, fmt); \
                    STAMP_PARSER_SET_HAS_##group(name); \
                }
            FOREACH_STAMP_BOUND_MODEL_CONST_PRE_CFG(X)
            #undef X
            // (The v1 legacy-prefixed-key dispatch walk lived here — RETIRED with the
            // TECH_DEBT-237 pre-epoch floor; see the FOREACH_LEGACY_PREFIXED_KEY tombstone
            // above. Pre-epoch stamps never reach field consumption: check 0c refuses them.)
            // v5.15.5.F.4d.1.B.3 Step 1.6.3 (Decision C Approach A; codified at v1.12 plan body) —
            // single-call framework dispatch replaces inline X-macro walker. PARSE_STAMP_CFG_TO_DERIVED
            // returns true when key matches any STAMP_BOUND_CFG_DERIVED-flagged row across 4 master
            // cfg registries (per-core + global + ml_cfg_flag + gate_cfg_flag); on match, parses value
            // + sets r.<name> + r.has_<name>. Walker filtered by metadata bit at template fn level.
            else if (PARSE_STAMP_CFG_TO_DERIVED(r, key, val)) {
                /* matched a STAMP_BOUND_CFG_DERIVED-flagged master cfg row; framework wrote to struct */
            }
            // v5.14.8.A.merged.4 — POST_CFG section parser branches (registry-driven).
            // Replaces manual if-else branches for expected_*, overlay_hash,
            // effective_hash (6 fields total). All POST_CFG entries are
            // standalone (group="_") so STAMP_SET(r, name) sets the entry's
            // own bit. tt::stamp_parse_field<T> templated helper handles
            // type-dispatch (char[N] strncpy / scalar cast); avoids the
            // non-template `if constexpr` cast-syntax issue (templated
            // function instantiates per-T, properly discarding branches).
            #define X(name, group, presence, type, fmt, default_val, get_value, emit_when, doc) \
                else if (strcmp(key, #name) == 0) { \
                    tt::stamp_parse_field(r.name, val, fmt); \
                    STAMP_PARSER_SET_HAS_##group(name); \
                }
            FOREACH_STAMP_BOUND_MODEL_CONST_POST_CFG(X)
            #undef X
        }
        line = strtok_r(nullptr, "\n", &save);
    }

    // v5.9.2b — cross-major engine version detection. Compare stamp's
    // engine_version major against current build's ENGINE_VERSION_STRING
    // major. Empty stamp engine_version (pre-v5.8.6) → skip (allow).
    // Major = atoi() of the prefix before first '.' — works for "5.9.2",
    // "5.9.2a", "v5.9.2", or any leading-int form.
    r.cross_major_engine = 0;
    if (r.engine_version[0] != '\0') {
        const char* sv = r.engine_version;
        if (sv[0] == 'v' || sv[0] == 'V') sv++;  // accept v-prefix
        int stamp_major = atoi(sv);
        const char* cur = ENGINE_VERSION_STRING;
        if (cur[0] == 'v' || cur[0] == 'V') cur++;
        int cur_major = atoi(cur);
        if (stamp_major != cur_major && stamp_major > 0 && cur_major > 0) {
            r.cross_major_engine = 1;
        }
    }

    // 0b. v5.15.5.F.4d.1.B.3 Step 1.6.7.2 — stamp_format_version upper-bound check.
    // Reject FUTURE versions — engine doesn't know how to interpret unknown shape.
    if ((uint32_t)r.stamp_format_version > MAX_SUPPORTED_STAMP_FORMAT_VERSION) {
        r.valid = 0;
        snprintf(r.reason, sizeof(r.reason),
            "stamp_format_version too new: stamp=%d engine_max=%u "
            "(retrain on an engine version that supports this stamp shape)",
            r.stamp_format_version, MAX_SUPPORTED_STAMP_FORMAT_VERSION);
        return r;
    }

    // 0c. TECH_DEBT-237 close (2026-07-17) — the Ship-B S-4/D-174 pre-epoch FLOOR, landed.
    // A stamp below the decimal epoch (stamp_format_version < 3 — INCLUDING 0 = field absent,
    // pre-v5.9.0) carries its ~30 money wire fields under the retired binary-FPN encoding;
    // parsing them into the decimal engine is the exact mis-decode S-4 forbids. UNCONDITIONAL
    // hard-invalid: valid=0 (NEVER -1), so held_out_gate_strict=0 cannot admit it — the strict
    // fork only softens MISSING stamps, never pre-epoch ones. Boot-time load path (H20
    // boot-time exception); MONEY_ENCODING_EPOCH is constexpr, so the guard constant-folds
    // away entirely on a binary-epoch build.
    if (MONEY_ENCODING_EPOCH != 0u &&
        (uint32_t)r.stamp_format_version < STAMP_FORMAT_VERSION_EPOCH_FLOOR) {
        r.valid = 0;
        snprintf(r.reason, sizeof(r.reason),
            "pre-epoch stamp (stamp_format_version=%d < %u): money fields carry the retired "
            "binary encoding; decimal engine refuses — retrain to re-stamp (TECH_DEBT-237)",
            r.stamp_format_version, STAMP_FORMAT_VERSION_EPOCH_FLOOR);
        return r;
    }

    // 1. Format version match
    if (r.model_format_version != expected_format_version) {
        r.valid = 0;
        snprintf(r.reason, sizeof(r.reason),
            "format-version mismatch: stamp=%d engine=%d",
            r.model_format_version, expected_format_version);
        return r;
    }

    // 1b. v5.8.1a — feature registry hash match. When caller passes
    // expected_feature_registry_hash != 0, the stamp's hash must match
    // (catches train-serve drift). Default 0 = "skip check" (caller
    // explicitly opts out). v5.8.6: when stamp has NO hash field
    // (pre-v5.8.1a stamps parse as 0), accept with stderr WARN rather
    // than reject — preserves back-compat with legacy models. Drift catch
    // fires only when BOTH sides have the data and they disagree.
    if (expected_feature_registry_hash != 0) {
        if (r.feature_registry_hash == 0) {
            fprintf(stderr,
                "[stamp] WARN: %s stamp lacks feature_registry_hash "
                "(pre-v5.8.1a) — drift NOT verified\n",
                stamp_path);
        } else if (r.feature_registry_hash != expected_feature_registry_hash) {
            r.valid = 0;
            snprintf(r.reason, sizeof(r.reason),
                "feature-registry-hash mismatch: stamp=%016lx engine=%016lx "
                "(retrain required)",
                (unsigned long)r.feature_registry_hash,
                (unsigned long)expected_feature_registry_hash);
            return r;
        }
    }

    // 1c. v5.10.0d — label registry hash match. Same shape as 1b but for
    // the LABEL_REGISTRY_HASH (FOREACH_TARGET X-macro). Caller passes
    // expected_label_registry_hash from LABEL_REGISTRY_HASH() at engine
    // boot. Default 0 = "skip check" (legacy callers + non-ML cores).
    // Pre-v5.10.0d stamps lack the field (parses as 0) → WARN, accept.
    // Drift catch fires only when both sides have the data and disagree.
    if (expected_label_registry_hash != 0) {
        if (r.label_registry_hash == 0) {
            fprintf(stderr,
                "[stamp] WARN: %s stamp lacks label_registry_hash "
                "(pre-v5.10.0d) — label drift NOT verified\n",
                stamp_path);
        } else if (r.label_registry_hash != expected_label_registry_hash) {
            r.valid = 0;
            snprintf(r.reason, sizeof(r.reason),
                "label-registry-hash mismatch: stamp=%016lx engine=%016lx "
                "(label set drift; retrain required)",
                (unsigned long)r.label_registry_hash,
                (unsigned long)expected_label_registry_hash);
            return r;
        }
    }

    // 1d. v5.11.18a — feature_mask match. Caller passes the runtime cfg's
    // per-core mask for the core loading this model. Default 0 = skip
    // check. Pre-v5.11.18a stamps lack the field (parses as 0) → caller
    // can decide WARN vs accept based on operator strictness; here we
    // WARN-and-accept by default (informational; behavior change is
    // v5.11.18 territory). When both sides have the data and disagree,
    // refuse — masked-feature drift is a parity-critical failure mode
    // (CRITICAL gap from /parity-check 2026-05-07).
    if (expected_feature_mask != 0) {
        if (!STAMP_HAS(r, feature_mask)) {
            fprintf(stderr,
                "[stamp] WARN: %s stamp lacks feature_mask "
                "(pre-v5.11.18a) — feature-mask drift NOT verified\n",
                stamp_path);
        } else if (r.feature_mask != expected_feature_mask) {
            r.valid = 0;
            snprintf(r.reason, sizeof(r.reason),
                "feature_mask mismatch: stamp=%016lx engine=%016lx "
                "(per-node feature subset drift; retrain or restore "
                "feature_mask cfg to training-time value)",
                (unsigned long)r.feature_mask,
                (unsigned long)expected_feature_mask);
            return r;
        }
    }

    // 2. Gap acceptable. v5.9.5j sentinel: gap_threshold == 0.0 + held_out
    // == 0.0 means "training-only stamp" (Train Model auto-stamp without
    // held-out). Skip the gap check for these stamps; they're info-grade
    // not deploy-grade. Operator wanting deploy validation runs Run Full
    // Validation which produces a full stamp.
    bool training_only_stamp = (r.gap_threshold == 0.0);
    if (!training_only_stamp && r.generalization_gap > r.gap_threshold) {
        r.valid = 0;
        snprintf(r.reason, sizeof(r.reason),
            "generalization gap %.4f exceeds threshold %.4f",
            r.generalization_gap, r.gap_threshold);
        return r;
    }

    // 3. Model file hasn't been swapped post-stamp
    char actual_sha[80] = {0};
    if (!sha256_file_hex(model_path, actual_sha, sizeof(actual_sha))) {
        r.valid = 0;
        snprintf(r.reason, sizeof(r.reason),
            "could not compute sha256 of %s", model_path);
        return r;
    }
    if (model_sha[0] && strcmp(model_sha, actual_sha) != 0) {
        r.valid = 0;
        snprintf(r.reason, sizeof(r.reason),
            "model file hash differs from stamp: actual=%.16s... stamp=%.16s...",
            actual_sha, model_sha);
        return r;
    }
    // s5 BT-8 — surface the (now file-verified) content hash to the caller.
    // Copied AFTER the equality check so a result carrying a sha is one whose
    // sha provably matches the bytes on disk.
    {
        size_t sl = strnlen(model_sha, sizeof(r.model_sha256) - 1);
        memcpy(r.model_sha256, model_sha, sl);
        r.model_sha256[sl] = '\0';
    }

    // 4. Signature verify (HMAC-SHA256 over canonical body, base64 or hex sig)
    //    Empty secret = accept-any (dev mode). Production should set secret.
    if (secret == nullptr || secret[0] == '\0') {
        // Dev mode — accept without sig check. Log a warning so this is
        // visible in stderr.
        fprintf(stderr,
            "[stamp] WARN: held_out_stamp_secret is empty — signature NOT verified for %s\n",
            stamp_path);
        r.valid = 1;
        snprintf(r.reason, sizeof(r.reason), "ok (dev mode, sig unchecked)");
        return r;
    }
    // v5.3.0 Phase B: in-process HMAC. Was a popen("openssl dgst -sha256 -hmac")
    // shell-out in v5.2.0 — replaced for shell-injection safety (canonical
    // body contained user-controlled fields like trained_on, secret was
    // single-quoted). RFC 4231 vectors and bash-compat regression test in
    // controller_test.cpp guard sig parity with the bash script's openssl
    // calls.
    char computed[80];
    if (!tt::hmac_sha256_hex(secret, canonical, computed)) {
        r.valid = 0;
        snprintf(r.reason, sizeof(r.reason), "HMAC-SHA256 computation failed");
        return r;
    }
    if (strcmp(computed, stamp_sig) == 0) {
        r.valid = 1;
        snprintf(r.reason, sizeof(r.reason), "ok (signature verified, gap %.4f ≤ %.4f)",
            r.generalization_gap, r.gap_threshold);
    } else {
        r.valid = 0;
        snprintf(r.reason, sizeof(r.reason),
            "signature mismatch: stamp=%.16s... computed=%.16s...",
            stamp_sig, computed);
    }
    return r;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// A `.stamp` file lives alongside each `.bin` model:
//
//   models/aggressive/buy_signal.bin
//   models/aggressive/buy_signal.stamp
//
// Stamp format (text, key=value lines, last line is signature):
//
//   model_format_version=12
//   model_sha256=<hex of binary>
//   trained_on=2026-04-28
//   wf_mean_val=0.55
//   held_out_metric=0.53
//   gap=0.02
//   gap_threshold=0.05
//   signature=<HMAC-SHA256(secret, all-prior-lines-concatenated)>
//
// Verifier returns:
//   1 = stamp present, signature valid, gap below threshold → safe to load
//   0 = stamp present but FAILED (sig mismatch, gap too wide, format-version
//       drift, or model_sha256 mismatch) → REJECT, log reason
//  -1 = stamp file missing entirely → caller decides via held_out_gate_strict
//
// Empty `secret` = "accept any signature" mode (dev convenience).
// Real production: set a non-empty secret + flip held_out_gate_strict=1.
//
// Safe from path traversal: caller passes the .bin path; we append ".stamp".
// File reads are bounded; stamp file > 4KB is treated as malformed.
//
// Parameters:
// model_path: path to the .bin file. `.stamp` is implied by appending.
// secret: HMAC secret. Empty string ("") = accept-any signature (dev mode).
// gap_threshold: max acceptable generalization gap. Stamp gap must be ≤ this.
// expected_format_version: caller passes MODEL_FORMAT_VERSION; mismatch fails.
//======================================================================
// [END_FUNCTION]_[verify_model_stamp]
//======================================================================

//----------------------------------------------------------------------
// [SECTION]_[v5.3.0 Phase B — stamp_write_for_model: sign + write a model stamp in-process]
//----------------------------------------------------------------------
// Inverse of verify_model_stamp. Computes SHA-256 of the model file,
// builds the same canonical body the verifier reads, signs with
// HMAC-SHA256, writes <model>.stamp atomically (write to .tmp, then
// rename — POSIX atomic within a filesystem). Refuses to write when
// |wf - held_out| > gap_threshold unless `force` is set.
//
// Field order in the canonical body MUST match the verifier byte-for-byte.
// (The retired tools/stamp_model.sh bash stamper defined the original
// format — deleted at v5.15.5.F.4d.1.B.3, stamping is in-process now;
// the bash-compat regression test in controller_test.cpp still locks the
// canonical-body format against drift.)
//
// Locale pinning: %g/%f honor LC_NUMERIC. A stamp signed under
// LC_NUMERIC=C wouldn't verify under LC_NUMERIC=de_DE because
// 0.55 → "0,55" in some locales. We pin LC_NUMERIC=C for the canonical
// body construction (per-thread via uselocale).

//======================================================================
// [STRUCT]_[StampWriteResult]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [PERSISTENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the stamp-write outcome — ok flag + human-readable error + the resolved stamp path (written or would-be)]
//======================================================================
// [CODE]
//======================================================================
struct StampWriteResult {
    int  ok;             // 1 = stamp written; 0 = refused (gap too wide, i/o, etc.)
    char error[256];     // human-readable failure reason
    char stamp_path[512]; // where it was written (or would have been)
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
// [SIZE]_[772B]
// [ALIGN]_[4]
// [CACHE_LINES]_[13]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[StampWriteResult]
//======================================================================

//======================================================================
// [STRUCT]_[StampInferenceCfgInputs]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [PERSISTENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the stamp EMIT-side input bundle — caller-filled fields gated by bit-packed has_flags; X-macro-generated to match ModelStampResult's canonical wire-key names]
// [REFERENCE]_[INVARIANT]_[[H9] [H18]]
//======================================================================
// [CODE]
//======================================================================
struct StampInferenceCfgInputs {
    // v5.15.5.F.4d.1.B.3 Step 1.6.3 — F=64 brought into struct scope for STAMP_RESULT_DERIVED_FIELDS_AUTO_GEN().
    static constexpr unsigned F = 64;

    // === Bit-packed has_* flags (matches ModelStampResult) ===
    uint64_t has_flags;

    // === Architectural value fields — auto-generated from FOREACH_STAMP_BOUND_MODEL_CONST ===
    // 26 entries; canonical wire-key names. Same expansion as ModelStampResult side.
    #define X(name, group, presence, type, fmt, default_val, get_value, emit_when, doc) \
        type name;
    FOREACH_STAMP_BOUND_MODEL_CONST(X)
    #undef X

    // === Derived cfg fields — auto-generated via STAMP_RESULT_DERIVED_FIELDS_AUTO_GEN ===
    // v5.15.5.F.4d.1.B.3 Step 1.6.3 (Decision C Approach A; codified at v1.12 plan body) —
    // sister site to ModelStampResult unconditional struct-gen. Same 4-walker framework primitive.
    // EXCLUSION REDIRECT: 3 xgb_* names redirect to dead-prefixed per H18 sidecar (see ModelStampResult site).
    #define xgb_min_child_weight _stamp_result_excluded_xgb_min_child_weight
    #define xgb_seed             _stamp_result_excluded_xgb_seed
    #define xgb_train_nthread    _stamp_result_excluded_xgb_train_nthread
    STAMP_RESULT_DERIVED_FIELDS_AUTO_GEN()
    #undef xgb_min_child_weight
    #undef xgb_seed
    #undef xgb_train_nthread

    // === Late-emit architectural fields — auto-generated via FOREACH_STAMP_BOUND_MODEL_CONST union (v5.14.8.A.merged.4) ===
    // POST_CFG section's entries declared above via X-macro walk over
    // FOREACH_STAMP_BOUND_MODEL_CONST = PRE_CFG + POST_CFG. has_* bits
    // bit-packed in has_flags; STAMP_HAS(inf, expected_num_classes) etc.
    // for accessor.
};
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// v5.9.2b — inference cfg fields bound to the stamp at training time.
// Caller fills only the fields it has; has_* flags gate emit. Nullptr
// passed for legacy callers means none of these fields emit (forward-
// compat with v5.9.0/.1/.2 stamps).
// v5.14.8.A.merged.3 — StampInferenceCfgInputs migrated to X-macro
// generation from FOREACH_STAMP_BOUND_MODEL_CONST registry (matches
// ModelStampResult; Option 1 unification). Both structs now use canonical
// wire-key field names + uint64_t has_flags bit-packed via STAMP_HAS aliases.
//
// Production caller migration: replace manual population blocks with
// `STAMP_MODEL_CONST_AUTOPOPULATE(inf, src)` macro call. Future field
// additions auto-flow via the registry (extinguishes v5.9.5b production-
// caller class for stamp body fields).
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-08-22]
// [SIZE]_[4896B]
// [ALIGN]_[16]
// [CACHE_LINES]_[77]
// [STRADDLE]_[unverified: scaler_sha256 overlay_hash effective_hash run_name scaler_fit_data_hash removal_reasons_csv environment_tf_version environment_cuda_version environment_cpu_model]
//======================================================================
// [END_STRUCT]_[StampInferenceCfgInputs]
//======================================================================

// D-426 — opt this struct INTO the STAMP_SET refusal. Declared in
// StampBoundModelConstRegistry.hpp (which this file includes, so the primary
// template is already visible); specialized here because that header precedes
// this type's definition.
//
// EFFECT TODAY: **LIVE.** This is the opt-in marker for the `STAMP_SET` guard,
// read at the `static_assert` in StampBoundModelConstRegistry.hpp's STAMP_SET.
// `StampInferenceCfgInputs` is the ONLY struct that opts in, so `STAMP_SET` on a
// name that IS a member of THIS struct is a compile error (use `STAMP_PUT`),
// while a GROUP bit — which has no field of its own — stays legal.
//
// ⚠️ THIS COMMENT HAS NOW BEEN FALSE IN BOTH DIRECTIONS ON CONSECUTIVE DAYS, which
// is why it is worth reading slowly. Draft 1 claimed the refusal was live while the
// guard had been reverted. That was corrected to "EFFECT TODAY: NONE — this
// specialization is INERT, nothing reads it", which was true for a few hours and
// then became false when the guard landed — and it was NOT swept, because the
// commit that armed the guard touched exactly one file. Both errors were caught by
// an independent close-out review, neither by the author.
//
// That is the whole lesson of this arc, reproduced twice on its own opt-in marker:
// a comment asserting the presence or absence of a guard is exactly why the `fees`
// sibling survived its own sweep. Class 58 sub-shape A′. **If you change the guard,
// sweep THIS comment in the same commit — it has a 2-for-2 record of being missed.**
//
// ⚠️ SCOPE — and do NOT repeat the too-broad claim the earlier drafts made here.
// The parse-side `ModelStampResult` legitimately stays out: its presence bits are
// set inside the parser's macro walk, structurally paired with the value. The
// runtime `ModelHandle` is a DIFFERENT case and the earlier phrasing lumped them
// together wrongly: its sr->handle copies are HAND-WRITTEN pairs, many of them
// bit-before-value, and site #4 of this arc's four-site pattern lived in exactly
// that block. It is unguarded because it is not opted in — not because it is safe.
// Extending the opt-in there is tracked follow-up, not a settled exemption.
namespace tt {
    template <>
    inline constexpr bool is_stamp_emit_inputs_v<StampInferenceCfgInputs> = true;
}

//======================================================================
// [FUNCTION]_[stamp_write_for_model]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [DETERMINISM]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the stamp EMIT side — LC_NUMERIC=C-pinned canonical body (PRE_CFG -> cfg-derived -> POST_CFG registry order), H9 HMAC-SHA256 sign, atomic tmp+rename write; refuses on gap > threshold]
// [REFERENCE]_[INVARIANT]_[[H9] [H21]]
// [REFERENCE]_[TECH_DEBT]_[[TECH_DEBT-6] [TECH_DEBT-237]]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F = 64>
inline StampWriteResult stamp_write_for_model(const char* model_path,
                                                const char* secret,
                                                int   format_version,
                                                const char* trained_on_iso,  // YYYY-MM-DD
                                                double wf_mean_val,
                                                double held_out_metric,
                                                double gap_threshold,
                                                int   force,
                                                uint64_t feature_registry_hash = 0,
                                                const char* engine_version = nullptr,
                                                // v5.9.2b — inference cfg binding.
                                                // Optional; nullptr = skip emit (legacy callers).
                                                const StampInferenceCfgInputs* inf = nullptr,
                                                // v5.15.5.F.4d.1.B.3 Step 1.6.4 — cfg pointer for
                                                // cfg-driven canonical body emit via framework.
                                                // nullptr = skip cfg-derived emit (legacy callers).
                                                const ControllerConfig<F>* cfg_ptr = nullptr) {
    StampWriteResult r;
    r.ok = 0;
    r.error[0] = '\0';
    r.stamp_path[0] = '\0';

    if (!model_path || !trained_on_iso) {
        snprintf(r.error, sizeof(r.error), "NULL inputs (model_path/trained_on_iso)");
        return r;
    }

    // 1. SHA-256 of the model file (in-process)
    char model_sha[80] = {0};
    if (!tt::sha256_file_hex_inproc(model_path, model_sha, sizeof(model_sha))) {
        snprintf(r.error, sizeof(r.error), "could not sha256 %s", model_path);
        return r;
    }

    // 2. Compute |wf - held_out|
    double gap = wf_mean_val - held_out_metric;
    if (gap < 0) gap = -gap;

    // 3. Refuse on gap > threshold (unless force)
    if (gap > gap_threshold && !force) {
        snprintf(r.error, sizeof(r.error),
            "REFUSE: gap %.4f > threshold %.4f (use force=1 to override)",
            gap, gap_threshold);
        return r;
    }

    // 4. Pin LC_NUMERIC=C for canonical body construction. uselocale() is
    //    per-thread — doesn't disturb the rest of the process.
    locale_t pinned = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
    locale_t prev = (locale_t)0;
    if (pinned) prev = uselocale(pinned);

    // 5. Canonical body — must match bash script + verifier byte-for-byte.
    //    Field order: format-version, sha256, trained_on, wf_mean_val,
    //    held_out_metric, gap, gap_threshold, [feature_registry_hash],
    //    [engine_version].
    //    feature_registry_hash is appended ONLY when format_version >= 5
    //    AND a non-zero hash was supplied. engine_version is appended
    //    ONLY when format_version >= 5 AND a non-empty string was supplied.
    //    v4 stamps and dev-mode invocations omit both fields — verifier
    //    handles missing fields as "skip check" / empty.
    //    Each line ends with \n.
    int has_hash    = (format_version >= 5 && feature_registry_hash != 0);
    int has_engver  = (format_version >= 5 && engine_version && engine_version[0] != '\0');
    // v5.9.0: stamp_format_version emitted whenever format_version >= 5
    // (the v5.8.1a+ wire-format era — the era that has feature_registry_hash
    // and engine_version). Schema version of the stamp body itself,
    // distinct from MODEL_FORMAT_VERSION (which versions the model file
    // shape, not the stamp). Bumped on future stamp body schema changes.
    // v5.15.5.F.4d.1.B.3 Step 1.6.7.1-3 — version literal extracted to
    // STAMP_FORMAT_VERSION_CURRENT constant; SOFT bump 1 → 2 (15 cfg-derived
    // cohort wire keys lose `inference_cfg_` prefix). The v1 dual-recognition
    // layer was RETIRED with the TECH_DEBT-237 pre-epoch floor (2026-07-17).
    int has_stamp_ver = (format_version >= 5);
    // v5.9.2b — bumped from 2048 → 4096. Original ~700 bytes; 9 new
    // inference_cfg_* + training_poll_interval fields × ~50 bytes each
    // = +450 bytes worst-case, well under the new ceiling. Leaves
    // headroom for v5.9.3 scaler fields too.
    char canonical[4096];
    int n = snprintf(canonical, sizeof(canonical),
        "model_format_version=%d\n"
        "model_sha256=%s\n"
        "trained_on=%s\n"
        "wf_mean_val=%g\n"
        "held_out_metric=%g\n"
        "gap=%.6f\n"
        "gap_threshold=%g\n",
        format_version, model_sha, trained_on_iso,
        wf_mean_val, held_out_metric, gap, gap_threshold);
    if (has_hash && n > 0 && (size_t)n < sizeof(canonical)) {
        int wrote = snprintf(canonical + n, sizeof(canonical) - n,
            "feature_registry_hash=%016lx\n",
            (unsigned long)feature_registry_hash);
        if (wrote > 0) n += wrote;
    }
    if (has_engver && n > 0 && (size_t)n < sizeof(canonical)) {
        int wrote = snprintf(canonical + n, sizeof(canonical) - n,
            "engine_version=%s\n", engine_version);
        if (wrote > 0) n += wrote;
    }
    if (has_stamp_ver && n > 0 && (size_t)n < sizeof(canonical)) {
        int wrote = snprintf(canonical + n, sizeof(canonical) - n,
            "stamp_format_version=%u\n", STAMP_FORMAT_VERSION_CURRENT);
        if (wrote > 0) n += wrote;
    }
    // v5.14.8.A.merged.3 — Registry-driven emit walk. Replaces ~152 lines
    // of manual `if (inf->has_X) snprintf(...)` blocks with single X-macro
    // walk over FOREACH_STAMP_BOUND_MODEL_CONST. Per-entry has_* dispatch
    // via STAMP_EMIT_CHECK_HAS_<group> token paste. Wire format byte-for-byte
    // preserved (same registry order as canonical emit sequence v5.14.7).
    //
    // Type handling: printf format string in registry's `fmt` column matches
    // C type promotion rules on x86_64:
    //   double + "%g" / "%.6g" — direct
    //   int + "%d" — direct
    //   uint8_t + "%d" — integer promotion to int
    //   uint32_t + "%u" — same as unsigned int on x86_64
    //   uint64_t + "%016lx" — same as unsigned long on x86_64
    //   tt::stamp_str_N (char[N]) + "%s" — array decay to char*
    // Compiler may emit -Wformat warnings for non-x86_64; if porting to
    // 32-bit, add explicit casts via a templated stamp_emit_field<T> helper.
    #define X(name, group, presence, type, fmt, default_val, get_value, emit_when, doc) \
        if (inf && STAMP_EMIT_CHECK_HAS_##group(name) && n > 0 && (size_t)n < sizeof(canonical)) { \
            int wrote = snprintf(canonical + n, sizeof(canonical) - n, \
                #name "=" fmt "\n", inf->name); \
            if (wrote > 0) n += wrote; \
        }
    FOREACH_STAMP_BOUND_MODEL_CONST_PRE_CFG(X)
    #undef X

    // v5.15.5.F.4d.1.B.3 Step 1.6.4 — Production canonical body emit migration.
    // Replaces legacy FOREACH_STAMP_BOUND_CFG(X) walker with framework call
    // `cfg_derived::populate_stamp_cfg_from_derived<F>(buf, cap, cfg)`. Framework walks all 4
    // cfg-derived cohort registries (per-core + global + ml_cfg_flag + gate_cfg_flag) filtered
    // by STAMP_BOUND_CFG_DERIVED bit; per-row cohort gate from cfg_gate::lookup_populate; per-row
    // emit via tt::cfg_emit_field<T> (handles FPN_Binary<F> ↔ %.17g + bitmap-bool ↔ %d correctly).
    //
    // Source-of-truth shift: cfg-derived wire keys now read from cfg AT EMIT TIME (not from
    // caller-populated inf). Wire keys lose `inference_cfg_` prefix at v2 wire format per
    // SOFT bump (Step 1.6.7); pre-epoch (v1/v2) stamps now REFUSE at check 0c (TECH_DEBT-237).
    //
    // cfg_ptr=nullptr → skip cfg-derived emit (legacy callers; equivalent to all-inf-has-zero
    // pre-migration). Stamp_AssembleAndEmit passes &cfg → cfg-derived rows emit per gate.
    if (cfg_ptr && n > 0 && (size_t)n < sizeof(canonical)) {
        size_t added = cfg_derived::populate_stamp_cfg_from_derived<F>(
            canonical + n, sizeof(canonical) - (size_t)n, *cfg_ptr);
        if (added > 0) n += (int)added;
    }

    // v5.14.8.A.merged.4 — POST_CFG section emit walk (registry-driven).
    // Closes TECH_DEBT-006: 6 late-emit fields (expected_*, overlay_hash,
    // effective_hash) auto-flow via the same X-macro pattern as PRE_CFG.
    // Wire format byte-for-byte preserved (canonical order: PRE_CFG → cfg
    // → POST_CFG; same as v5.14.7 manual emit).
    #define X(name, group, presence, type, fmt, default_val, get_value, emit_when, doc) \
        if (inf && STAMP_EMIT_CHECK_HAS_##group(name) && n > 0 && (size_t)n < sizeof(canonical)) { \
            int wrote = snprintf(canonical + n, sizeof(canonical) - n, \
                #name "=" fmt "\n", inf->name); \
            if (wrote > 0) n += wrote; \
        }
    FOREACH_STAMP_BOUND_MODEL_CONST_POST_CFG(X)
    #undef X

    // Restore prior locale ASAP — every subsequent return must NOT undo this twice
    if (pinned) {
        uselocale(prev);
        freelocale(pinned);
    }

    if (n <= 0 || (size_t)n >= sizeof(canonical)) {
        snprintf(r.error, sizeof(r.error), "canonical body overflow (n=%d)", n);
        return r;
    }

    // 6. HMAC-SHA256(secret, canonical). Empty secret = dev-mode placeholder
    //    so the file is well-formed but the engine knows to skip sig check.
    char sig[80];
    const char* effective_secret = (secret && secret[0]) ? secret : "";
    if (effective_secret[0] == '\0') {
        memcpy(sig, "devmode-no-secret-no-signature", 31);
        sig[31] = '\0';
    } else {
        if (!tt::hmac_sha256_hex(effective_secret, canonical, sig)) {
            snprintf(r.error, sizeof(r.error), "HMAC-SHA256 computation failed");
            return r;
        }
    }

    // 7. Atomic write: write to <stamp>.tmp, then rename. POSIX rename()
    //    is atomic within the same filesystem, so a reader can never
    //    observe a partially-written stamp.
    snprintf(r.stamp_path, sizeof(r.stamp_path), "%s.stamp", model_path);
    char tmp_path[520];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", r.stamp_path);

    FILE* f = fopen(tmp_path, "w");
    if (!f) {
        snprintf(r.error, sizeof(r.error), "fopen failed: %s", tmp_path);
        return r;
    }
    fputs(canonical, f);
    fprintf(f, "signature=%s\n", sig);
    if (fclose(f) != 0) {
        unlink(tmp_path);
        snprintf(r.error, sizeof(r.error), "fclose failed: %s", tmp_path);
        return r;
    }

    if (rename(tmp_path, r.stamp_path) != 0) {
        unlink(tmp_path);
        snprintf(r.error, sizeof(r.error), "rename failed: %s -> %s",
                 tmp_path, r.stamp_path);
        return r;
    }

    r.ok = 1;
    return r;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// v5.15.5.F.4d.1.B.3 Step 1.6.4 — templated on F + cfg_ptr param for cfg-driven canonical body
// emit via cfg_derived::populate_stamp_cfg_from_derived<F>. Default F=64 preserves call shape
// for non-cfg callers (cfg_ptr=nullptr skips cfg-derived emit; matches Stamp_AssembleAndEmit ↔
// stamp_write_for_model contract). Source-of-truth shift: cfg-derived wire keys read from cfg
// at emit time (not from caller-populated inf). Cohort gating per-row from cfg_gate::lookup_populate.
//======================================================================
// [END_FUNCTION]_[stamp_write_for_model]
//======================================================================

#endif // MODEL_INFERENCE_HPP
