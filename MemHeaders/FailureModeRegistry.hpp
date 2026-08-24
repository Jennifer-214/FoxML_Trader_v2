// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[MemHeaders/FailureModeRegistry.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [MONITORING_PLANE] [FRAMEWORK_DISCIPLINE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the ML failure-mode SSoT — 15 rows (11 BIT_FLAG in failure_flags uint16 / 3 COUNTER_U32 / 1 PERCENT_U8) with severity + panel format + tooltip + display group per row]
// [CONTAINS]
//   - [REGISTRY]_[FOREACH_FAILURE_MODE]   (storage-class/severity/group tokens + bit/mask gen + FAILURE_* accessors + count ride as sections)
// [REFERENCE]_[DESIGN_SPEC]_[[x-macro-registry-with-presence-dispatch] [bitmap-flag-api]]
// [REFERENCE]_[INVARIANT]_[H14]
//======================================================================================================
// Pseudo-registry for ML observability failure modes that surface to
// PerNodeSnap + ML Status panel. Auto-generates 3 mechanical sites:
//   1. PerNodeSnap field / bit declaration (storage_class-aware)
//   2. Populator helper (Health_Log rate-limit static + setter)
//   3. Health_Log rate-limit per-call-site static (uint64_t last_emit_us)
//
// Drives 1 hand-placed site:
//   4. ML Status panel branch — placement contextual; reads registry
//      constants (SEVERITY_COLOR, FAILURE_MODE_LABEL, FAILURE_MODE_TOOLTIP)
//
// CLOSES recurring "add failure mode requires N-site update" pattern:
// before this registry, adding a failure mode required:
//   - PerNodeSnap field (DataStream/EngineTUI.hpp)
//   - Populator at slow path (CoreFrameworks/EngineSharded.hpp)
//   - Rate-limit static + Health_Log call (per failure mode)
//   - ML Status panel branch (GUI/MLStatusPanel.hpp)
//   - cfg field + parser (if cfg-toggleable)
//   - Tests (controller_test.cpp)
// 6 sites per addition. Now: 1 registry line + 1 (smaller) hand-placed
// panel branch. 4-of-6 sites mechanically generated.
//
// STORAGE CLASSES (data-oriented design + compile-time-elision disciplines):
//   BIT_FLAG    — 1 bit in PerNodeSnap.failure_flags uint16_t bitmap.
//                 Auto-allocates bit position via __COUNTER__ at struct
//                 generation. Up to 16 BIT_FLAG entries (uint16_t cap).
//                 Wins: branchless multi-flag check (failure_flags &
//                 (MASK_X | MASK_Y)); atomic multi-flag updates via
//                 __atomic_fetch_or; "any failure?" check is single
//                 uint16_t compare.
//   COUNTER_U32 — Standalone uint32_t field. Incrementing event counter.
//                 Bumped per event via populator helper.
//   PERCENT_U8  — Standalone uint8_t field. 0-100 percentage indicator
//                 (e.g., warmup_progress_pct).
//
// GROUP_ID (combined-display):
//   Entries with group_id != 0 share a panel display row (e.g.,
//   nan_feature_events + nan_prediction_events both shown on one
//   "nan: feat=N, pred=M" row). group_id == 0 means standalone display.
//   Panel-side iterates entries by group_id; auto-generates the
//   combined render call.
//
// SEVERITY:
//   SEV_RED    — engine cannot proceed; operator must intervene
//   SEV_YELLOW — engine continues degraded; operator should investigate
//   SEV_SAND   — informational; operator awareness only (warmup, etc.)
//
// CROSS-REFERENCE: this registry is the SECOND application of the
// X-macro registry pattern documented in
// `tick-trader-percore-workspace/DESIGN_SPECS/x-macro-registry-with-presence-dispatch.md`
// + uses the BITMAP_* API from `bitmap-flag-api.md`. See those docs
// for the deeper pattern + future extension shape.
//======================================================================================================
#ifndef FAILURE_MODE_REGISTRY_HPP
#define FAILURE_MODE_REGISTRY_HPP

#include <stdint.h>
#include "BitmapMacros.hpp"   // BITMAP_* primitives (v5.14.8.A.0.b.1)

//======================================================================
// [REGISTRY]_[FOREACH_FAILURE_MODE]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [MONITORING_PLANE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[15 failure-mode rows -> storage-class-dispatched PerNodeSnap fields/bits + FAILURE_BIT_/MASK_ constants + FAILURE_* accessors; 11 BIT_FLAGs (overflow assert at 16)]
// [COLUMN]_[name]_[PerNodeSnap field/bit name + populator suffix (valid C identifier)]
// [COLUMN]_[storage_class]_[BIT_FLAG / COUNTER_U32 / PERCENT_U8 token — drives field type + populator semantics]
// [COLUMN]_[severity]_[SEV_RED / SEV_YELLOW / SEV_SAND — maps to color via SEVERITY_COLOR at panel render]
// [COLUMN]_[format_str]_[printf-style label for panel display]
// [COLUMN]_[tooltip_str]_[multi-line operator guidance (panel hover)]
// [COLUMN]_[group_id]_[tt::GROUP_STANDALONE (0) or tt::GROUP_<X> combined-display row]
// [REFERENCE]_[DECISION]_[D-221]
// [REFERENCE]_[TECH_DEBT]_[TECH_DEBT-15]
//======================================================================
// [CODE]
//======================================================================
//------------------------------------------------------------------
// [SECTION]_[STORAGE CLASS TOKENS]
//------------------------------------------------------------------
// Each entry's storage_class column is one of these tokens. Token-paste
// dispatch generates the right struct field declaration + populator
// helper at expansion time. See FAILURE_MODE_GEN_FIELD_<class> +
// FAILURE_MODE_GEN_SETTER_<class> macros below.
// (No #defines needed for token names themselves — they're literal
// tokens used in registry entries + dispatched via ##.)

//------------------------------------------------------------------
// [SECTION]_[SEVERITY TOKENS]
//------------------------------------------------------------------
// Map to FoxmlColors for ML Status panel rendering. Hand-placed panel
// branch reads SEVERITY_COLOR(sev_token) at render time.

#define FAILURE_MODE_SEVERITY_RED    /* operator-must-intervene */
#define FAILURE_MODE_SEVERITY_YELLOW /* degraded-investigate */
#define FAILURE_MODE_SEVERITY_SAND   /* informational */

// At panel render: `SEVERITY_COLOR(SEV_RED)` returns the right ImVec4.
// Provided by GUI/FoxmlTheme.hpp; this header just enumerates names.

//------------------------------------------------------------------
// [SECTION]_[GROUP_ID TOKENS — combined-display panel groups]
//------------------------------------------------------------------
// Entries with the same non-zero group_id share a panel display row.
// Add new GROUP_<NAME> = next-available-id when introducing a new
// combined-display group.

namespace tt {
enum FailureModeGroupId : int {
    GROUP_STANDALONE  = 0,  // sentinel; standalone display
    GROUP_NAN_EVENTS  = 1,  // nan_feature_events + nan_prediction_events
    GROUP_DRIFT       = 2,  // v5.15.1 — Model Health drift surface (8 entries since cfg_cross_binary_drift joined at v5.15.5.A.7)
    // Future combined-display groups append here.
};
}

//------------------------------------------------------------------
// [SECTION]_[THE REGISTRY ROWS]
//------------------------------------------------------------------
#define FOREACH_FAILURE_MODE(X)                                                                        \
    X(ml_model_load_failed,     BIT_FLAG,    SEV_RED,    "model: LOAD FAILED",                          \
      "ML strategy was selected but no model could be loaded.\n"                                        \
      "Stamp validation failed, role file missing under any name "                                      \
      "(buy_signal/barrier/regime), or held_out_gate_strict=1\n"                                        \
      "with mismatched registry hash. Check the boot log for details.\n"                                \
      "Operator action: verify cfg path + retrain if needed.",                                          \
      tt::GROUP_STANDALONE)                                                                            \
    X(ml_scaler_load_failed,    BIT_FLAG,    SEV_YELLOW, "scaler: LOAD FAILED",                         \
      "Scaler sidecar present in stamp but failed to load.\n"                                           \
      "Check sidecar file exists at stamp's claimed path + scaler_sha256\n"                             \
      "matches. Engine falls back to identity scaler (predictions degraded).",                          \
      tt::GROUP_STANDALONE)                                                                            \
    X(ml_walker_parity_failed,  BIT_FLAG,    SEV_YELLOW, "walker: PARITY REFUSED",                      \
      "ml_backend=4 (flat-SoA walker) was requested but did NOT activate.\n"                            \
      "Either the artifact hit a walker parse REFUSE, or the load-time\n"                               \
      "bit-parity oracle found the walker disagreeing with the XGBoost\n"                               \
      "library on a designed probe (missing-sentinel / NaN / split-boundary).\n"                        \
      "YELLOW not RED on purpose: predictions are UNCHANGED — the engine fell\n"                        \
      "back to the library C API, so this costs speed, never correctness.\n"                            \
      "Operator action: read the [ML] walker REFUSED boot line; it names the\n"                         \
      "space (margin vs transformed), the class and the feature that diverged.", \
      tt::GROUP_STANDALONE)                                                                            \
    X(ml_role_mismatch,         BIT_FLAG,    SEV_RED,    "role: MISMATCH",                              \
      "Stamp expected_role does not match the slot this model loaded into\n"                            \
      "(or an exit-slot stamp has no role key). A buy-trained model serving\n"                          \
      "as an exit signal is semantically INVERTED. Retrain with the correct\n"                          \
      "Training Side or restore the right file (E.1.2.C, PARITY-044).",                                 \
      tt::GROUP_STANDALONE)                                                                            \
    X(warmup_progress_pct,      PERCENT_U8,  SEV_SAND,   "warmup: %u%%",                                \
      "Per-node slow path is still gathering rolling samples.\n"                                        \
      "Model + features won't fire predictions until rolling\n"                                         \
      "count reaches min_warmup_samples. Once 100%%, expect a\n"                                        \
      "boot-time stderr line confirming readiness.",                                                    \
      tt::GROUP_STANDALONE)                                                                            \
    X(ml_nan_feature_events,    COUNTER_U32, SEV_YELLOW, "feat: %u",                                    \
      "Total feature-pack NaN/Inf events on this node.\n"                                               \
      "Triggers when Features_PackAll's two-layer guard catches NaN/Inf\n"                              \
      "(FPN_Binary saturation past 1e15 OR IEEE-754 isnan/isinf post-cast).\n"                                 \
      "Counter increments per skipped prediction cycle.",                                               \
      tt::GROUP_NAN_EVENTS)                                                                             \
    X(ml_nan_prediction_events, COUNTER_U32, SEV_YELLOW, "pred: %u",                                    \
      "Total prediction NaN/Inf events on this node.\n"                                                 \
      "Triggers when Model_Predict returns NaN/Inf (degenerate model OR\n"                              \
      "post-scaler-apply NaN propagation).\n"                                                           \
      "Counter increments per skipped prediction cycle.",                                               \
      tt::GROUP_NAN_EVENTS)                                                                             \
    /* v5.14.8.E — stale-feature failure mode. Wiring deferred to TECH_DEBT-015 */                       \
    /* (Features_PackAll consumes feature_last_update_us per-feature; bumps     */                       \
    /* this counter when feature data exceeds max_staleness_minutes threshold). */                       \
    X(stale_feature_events,     COUNTER_U32, SEV_YELLOW, "stale: %u feat",                              \
      "Total events where a feature's source data exceeded its\n"                                       \
      "max_staleness_minutes threshold and the feature was skipped\n"                                   \
      "(zero sentinel written). Indicates feature pipeline drift\n"                                     \
      "or stalled data source. Investigate if counter grows steadily.",                                  \
      tt::GROUP_STANDALONE)                                                                             \
    /* === v5.15.1 — Model Health drift surface (7 BIT_FLAG entries; tt::GROUP_DRIFT) === */            \
    /* Set at NodeModelZoo_TryLoadRole post-verify_model_stamp chokepoint; read by   */                 \
    /* MLStatusPanel.hpp Model Health CollapsingHeader + (future) v5.15.2 boot gate. */                 \
    X(feature_hash_drift,       BIT_FLAG,    SEV_RED,    "feat: HASH DRIFT",                            \
      "Model's stamp-bound feature_registry_hash does not match the\n"                                  \
      "current FEATURE_REGISTRY_HASH at runtime.\n"                                                     \
      "Indicates schema drift since training (feature added / removed /\n"                              \
      "reordered).\n"                                                                                   \
      "Operator action: retrain with current feature set OR document drift.",                           \
      tt::GROUP_DRIFT)                                                                                  \
    X(label_hash_drift,         BIT_FLAG,    SEV_RED,    "label: HASH DRIFT",                           \
      "Model's stamp-bound label_registry_hash does not match the\n"                                    \
      "current LABEL_REGISTRY_HASH at runtime.\n"                                                       \
      "Indicates label-kind schema drift since training.\n"                                             \
      "Operator action: retrain with current label set.",                                               \
      tt::GROUP_DRIFT)                                                                                  \
    X(build_flags_drift,        BIT_FLAG,    SEV_YELLOW, "build: FLAG DRIFT",                           \
      "Model's stamp-bound build_flags_hash does not match the current\n"                               \
      "build's compile-time flags hash. Predictions may diverge if a build\n"                           \
      "flag (LATENCY_PROFILING, USE_XGBOOST, etc.) affects feature compute.\n"                          \
      "Operator action: rebuild engine with matching flags OR retrain.",                                \
      tt::GROUP_DRIFT)                                                                                  \
    X(scaler_drift,             BIT_FLAG,    SEV_RED,    "scaler: BIND DRIFT",                          \
      "Loaded scaler's feature_registry_hash does not match the model\n"                                \
      "handle's feature_registry_hash. Scaler binding broke between\n"                                  \
      "training-time and runtime (e.g., sidecar copied from different\n"                                \
      "training session).\n"                                                                            \
      "Operator action: retrain scaler with current model + features.",                                 \
      tt::GROUP_DRIFT)                                                                                  \
    X(cfg_binding_drift,        BIT_FLAG,    SEV_YELLOW, "cfg: INFERENCE DRIFT",                        \
      "One or more stamp-bound inference_cfg fields diverge between training-time\n"                    \
      "and runtime cfg.* values. Examples: confidence_threshold_scale,\n"                               \
      "barrier_gate_enabled, bandit_blend_ratio, ml_tp_pct, ml_sl_pct,\n"                               \
      "barrier_blend_mode, per_horizon_barrier_blend (v5.15.5.A.7+).\n"                                 \
      "Tier 1 fields REFUSE in strict mode (model_verify_strict=1);                                 \n" \
      "Tier 2 fields WARN regardless. Set by FOREACH_CFG_DRIFT_CHECK walker                          \n" \
      "at NodeModelZoo_ValidateAgainstCfg post-v5.15.5.A.7 chokepoint.                                \n"\
      "Operator action: review boot log for per-field WARN; retrain if\n"                               \
      "intentional or revert cfg to training-time values; or ack via\n"                                 \
      "cfg.acknowledge_inference_cfg_drift=1 (now ops_cfg_flags bit).",                                 \
      tt::GROUP_DRIFT)                                                                                  \
    /* v5.15.5.A.7 — Cross-binary-category drift bit (sister of cfg_binding_drift).             */     \
    /* Set when stamp-bound xgb hyperparams / build_flags / training_poll_interval diverge from   */    \
    /* cfg at load time. Cross-binary drift = forensic indicator (model was trained under         */    \
    /* different cfg; cannot be retrained at load; bytewise prediction divergence possible if     */    \
    /* operator retrains). All cross-binary checks are WARN-only (never REFUSE) — bit set when    */    \
    /* operator hasn't ack'd via cfg.acknowledge_cross_binary_version_drift=1 (now ops_cfg_flags  */    \
    /* bit). Closes ArchField↔CfgDrift bitmap asymmetry — ArchField sets per-entry bits; cfg-     */    \
    /* drift now sets per-category bits via FOREACH_CFG_DRIFT_CHECK Y3 category dispatch.         */    \
    X(cfg_cross_binary_drift,   BIT_FLAG,    SEV_YELLOW, "cfg: CROSS-BINARY DRIFT",                     \
      "One or more cross-binary stamp-bound fields diverge from cfg at load:\n"                         \
      "xgb_subsample, xgb_colsample_bytree, xgb_min_child_weight, xgb_seed,\n"                          \
      "xgb_tree_method, xgb_train_nthread, training_poll_interval,\n"                                   \
      "build_flags_hash (when not covered by ArchFieldDrift).\n"                                        \
      "Cross-binary drift = bytewise model divergence if retrained under current\n"                     \
      "cfg. Forensic only at load (cannot retrain at boot); operator notification.\n"                   \
      "Set by FOREACH_CFG_DRIFT_CHECK walker (v5.15.5.A.7+).\n"                                         \
      "Operator action: review boot log; ack via cfg.acknowledge_cross_binary_version_drift=1\n"        \
      "(now ops_cfg_flags bit) if intentional, OR retrain with current cfg values.",                    \
      tt::GROUP_DRIFT)                                                                                  \
    X(stamp_hmac_not_verified,  BIT_FLAG,    SEV_YELLOW, "stamp: HMAC NOT VERIFIED",                    \
      "Stamp body's HMAC signature was not verified at load\n"                                          \
      "(cfg.held_out_stamp_secret empty OR cfg.model_verify_strict=skip).\n"                            \
      "Live trading should always run with non-empty secret + strict\n"                                 \
      "verification (REFUSE in v5.15.2 boot gate when trading_mode=live).\n"                            \
      "Operator action: set held_out_stamp_secret + model_verify_strict=1.",                            \
      tt::GROUP_DRIFT)                                                                                  \
    X(model_age_warn,           BIT_FLAG,    SEV_YELLOW, "model: AGE WARN",                             \
      "Model's training_timestamp_us indicates age beyond\n"                                            \
      "cfg.model_max_age_hours. Stale model may produce predictions\n"                                  \
      "detached from current market regime.\n"                                                          \
      "Operator action: retrain on recent data OR adjust\n"                                             \
      "model_max_age_hours.",                                                                           \
      tt::GROUP_DRIFT)                                                                                  \
    /* v5.15.5.E.0.10 A6 ingress (D-221) — ML model BARRIER corruption; sticky (latches until a       */ \
    /* valid reload clears MODEL_CORRUPT). Node REFUSES new trades. Distinct from ml_model_load_failed */ \
    /* (missing→SimpleDip degrade). 1 hand-placed MLStatusPanel render branch (sibling pattern).       */ \
    X(ml_model_corrupt,         BIT_FLAG,    SEV_RED,    "model: CORRUPT — RETRAIN",                    \
      "ML model's stamp-bound barrier (label_tp_pct / label_sl_pct) failed\n"                            \
      "ingress validation at load (negative / NaN / +Inf / out-of-range) for\n"                          \
      "the MAJORITY of ensemble arms (or all arms). The node REFUSES to trade\n"                         \
      "(new entries gate-zeroed) until a valid model is loaded — DISTINCT from\n"                        \
      "a MISSING model (which degrades to SimpleDip): a corrupt capital artifact\n"                      \
      "is more alarming than an absent one.\n"                                                           \
      "Operator action: RETRAIN the model (the on-disk stamp is corrupt).",                             \
      tt::GROUP_STANDALONE)

//------------------------------------------------------------------
// [SECTION]_[STORAGE-CLASS-AWARE FIELD GENERATION]
//------------------------------------------------------------------
// Token-paste dispatch on storage_class column. Used by PerNodeSnap
// struct generation in v5.14.8.C migration to declare the right
// underlying field type.
//
// BIT_FLAG entries DON'T declare a struct field directly — they're
// auto-allocated bit positions in the per-snap failure_flags uint16_t.
// At struct generation time:
//   - PerNodeSnap.failure_flags is declared ONCE (uint16_t)
//   - BIT_FLAG entries' MASK_<name> constants generated separately
//
// COUNTER_U32 / PERCENT_U8 entries each declare a standalone field.

#define FAILURE_MODE_GEN_FIELD_BIT_FLAG(name)    /* skip — packed in failure_flags bitmap */
#define FAILURE_MODE_GEN_FIELD_COUNTER_U32(name) uint32_t name;
#define FAILURE_MODE_GEN_FIELD_PERCENT_U8(name)  uint8_t name;

// Per-storage-class struct gen dispatcher:
#define FAILURE_MODE_FIELD_DECL(name, storage_class, sev, fmt, tooltip, group_id) \
    FAILURE_MODE_GEN_FIELD_##storage_class(name)

//------------------------------------------------------------------
// [SECTION]_[BIT_FLAG MASK ALLOCATION]
//------------------------------------------------------------------
// For BIT_FLAG entries, generate MASK_<name> = bit position. Use a
// per-entry counter via enumeration. Up to 16 BIT_FLAG entries fit in
// uint16_t failure_flags.

namespace tt {
enum FailureModeBit : uint16_t {
    // Auto-allocated bit positions for BIT_FLAG entries. The X-macro
    // walks FOREACH_FAILURE_MODE; for BIT_FLAG entries, declare a bit
    // here (entry name → enum value). Other storage classes skip.
    #define FAILURE_MODE_BIT_DECL_BIT_FLAG(name)    FAILURE_BIT_##name,
    #define FAILURE_MODE_BIT_DECL_COUNTER_U32(name) /* not bit-packed */
    #define FAILURE_MODE_BIT_DECL_PERCENT_U8(name)  /* not bit-packed */
    #define X(name, storage_class, sev, fmt, tooltip, group_id) \
        FAILURE_MODE_BIT_DECL_##storage_class(name)
    FOREACH_FAILURE_MODE(X)
    #undef X
    #undef FAILURE_MODE_BIT_DECL_BIT_FLAG
    #undef FAILURE_MODE_BIT_DECL_COUNTER_U32
    #undef FAILURE_MODE_BIT_DECL_PERCENT_U8

    FAILURE_BIT_COUNT  // sentinel; total BIT_FLAG entries
};
// [ASSERT]_[BITMAP_OVERFLOW]_[FAILURE_BIT_COUNT <= 16]
static_assert(FAILURE_BIT_COUNT <= 16,
              "FOREACH_FAILURE_MODE exceeds uint16_t failure_flags capacity");
}  // namespace tt

// MASK_<name> constants for BIT_FLAG entries (generated per-entry):
#define FAILURE_MODE_MASK_DECL_BIT_FLAG(name) \
    static constexpr uint16_t FAILURE_MASK_##name = \
        (uint16_t)(1u << tt::FAILURE_BIT_##name);
#define FAILURE_MODE_MASK_DECL_COUNTER_U32(name) /* not bit-packed */
#define FAILURE_MODE_MASK_DECL_PERCENT_U8(name)  /* not bit-packed */
#define X(name, storage_class, sev, fmt, tooltip, group_id) \
    FAILURE_MODE_MASK_DECL_##storage_class(name)
FOREACH_FAILURE_MODE(X)
#undef X

//------------------------------------------------------------------
// [SECTION]_[FAILURE_* ERGONOMIC ACCESSORS — alias to BITMAP_*]
//------------------------------------------------------------------
// BIT_FLAG accessors via BITMAP_* API. failure_flags is a uint16_t on
// PerNodeSnap (declared in v5.14.8.C migration).
//
// Atomic variants available via BITMAP_ATOMIC_* directly when slow path
// writes + display thread reads concurrently.

#define FAILURE_IS_SET(snap, name)  BITMAP_IS_SET((snap).failure_flags, FAILURE_MASK_##name)
#define FAILURE_SET(snap, name)     BITMAP_SET((snap).failure_flags, FAILURE_MASK_##name)
#define FAILURE_CLR(snap, name)     BITMAP_CLR((snap).failure_flags, FAILURE_MASK_##name)
#define FAILURE_ANY(snap, mask_set) BITMAP_ANY((snap).failure_flags, (mask_set))

// Atomic variants (cross-thread; e.g., slow-path writes failure flag,
// display thread reads):
#define FAILURE_ATOMIC_SET(snap, name) \
    BITMAP_ATOMIC_SET((snap).failure_flags, FAILURE_MASK_##name)
#define FAILURE_ATOMIC_IS_SET(snap, name) \
    BITMAP_ATOMIC_IS_SET((snap).failure_flags, FAILURE_MASK_##name)

//------------------------------------------------------------------
// [SECTION]_[TEST INSTRUMENTATION]
//------------------------------------------------------------------
// Compile-time count of total registry entries. Used by tests to assert
// shape correctness.

#define FAILURE_MODE_COUNT_ONE(name, storage_class, sev, fmt, tooltip, group_id) +1
#define FOREACH_FAILURE_MODE_COUNT  (0 FOREACH_FAILURE_MODE(FAILURE_MODE_COUNT_ONE))
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// X(name, storage_class, severity, format_str, tooltip_str, group_id)
//
//   name           — PerNodeSnap field/bit name + populator suffix.
//                    Must be a valid C identifier.
//   storage_class  — BIT_FLAG / COUNTER_U32 / PERCENT_U8 token.
//                    Drives field type + populator semantics.
//   severity       — SEV_RED / SEV_YELLOW / SEV_SAND token. Maps to
//                    color via SEVERITY_COLOR macro at panel render.
//   format_str     — printf-style label for panel display
//                    (e.g., "model: LOAD FAILED", "stale: %u feat").
//   tooltip_str    — multi-line operator guidance string. Shown on
//                    panel hover.
//   group_id       — tt::GROUP_STANDALONE (0) for own display row;
//                    tt::GROUP_<X> for combined-display.
//======================================================================
// [END_REGISTRY]_[FOREACH_FAILURE_MODE]
//======================================================================

#endif // FAILURE_MODE_REGISTRY_HPP
