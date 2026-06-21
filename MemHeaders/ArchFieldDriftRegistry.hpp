// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [ARCHITECTURAL FIELD DRIFT REGISTRY — v5.15.1]
//======================================================================================================
// X-macro registry for non-CFG-bound drift detection at model load time.
// Sister registry to the existing CFG drift loop at
// `NodeModelZoo.hpp` (post-verify_model_stamp; walks FOREACH_STAMP_BOUND_CFG
// + compares stamp_value vs cfg_value + increments
// sr.inference_cfg_drift_count).
//
// This registry covers ARCHITECTURAL drift sources that are NOT cfg-bound:
//   - feature_hash:    FOREACH_FEATURE registry hash drift (schema change)
//   - label_hash:      FOREACH_TARGET registry hash drift
//   - build_flags:     compile-time build flag hash drift
//   - scaler_binding:  loaded scaler bound to different feature set than model
//
// Adding a new architectural drift check = 1 row in this registry. Engine
// boot's TryLoadRole walks both registries (CFG + ARCH) + sets the
// corresponding FOREACH_FAILURE_MODE BIT_FLAG drift entries on the
// ModelHandle.drift_flags_at_load uint16_t. ShardedSnapshot_Publish
// OR-aggregates across all 4 zoo roles into PerNodeSnap.failure_flags
// for GUI Model Health rendering + boot-gate consumption.
//
// CLOSES recurring "add new arch-field drift check requires touching the
// drift detection site" pattern. Same Class 18 mirror shape that
// FOREACH_STAMP_BOUND_CFG closed on the cfg side; same shape that v5.15.0
// extinguished on the parser side. /merge-scan HIGH-1 consolidation: this
// is the NEW chokepoint for arch-field drift; the existing CFG loop stays
// as the chokepoint for cfg drift; cfg_binding_drift bit is OR-set once
// after the CFG loop runs (preserves existing logic; no parallel detection).
//
// CROSS-REFERENCE: extends `FailureModeRegistry.hpp` BIT_FLAG entries
// (FOREACH_FAILURE_MODE adds 7 drift entries in v5.15.1; this registry
// drives 4 of them — the other 3 (CFG_BINDING_DRIFT, STAMP_HMAC_NOT_VERIFIED,
// MODEL_AGE_WARN) are single-fact checks not driven by this registry).
//======================================================================================================
#ifndef ARCH_FIELD_DRIFT_REGISTRY_HPP
#define ARCH_FIELD_DRIFT_REGISTRY_HPP

#include <stdint.h>
#include "BitmapMacros.hpp"        // BITMAP_* primitives
#include "FailureModeRegistry.hpp" // FAILURE_MASK_<name> constants

//======================================================================================================
// [REGISTRY ENTRY SHAPE]
//======================================================================================================
// X(name, stamp_field_expr, runtime_value_expr, fail_mask)
//
//   name              — diagnostic label (used in WARN messages).
//   stamp_field_expr  — expression returning the stamp's recorded value
//                       (e.g., `sr.feature_registry_hash`). Evaluated at
//                       caller scope; caller's variable name for the
//                       ModelStampResult must be `sr`.
//   runtime_value_expr — expression returning the current runtime value
//                       (e.g., `FEATURE_REGISTRY_HASH()`). Evaluated at
//                       caller scope.
//   fail_mask         — FAILURE_MASK_<name> constant from FOREACH_FAILURE_MODE.
//                       Bit set on handle->drift_flags_at_load when stamp
//                       value diverges from runtime value.
//
// Caller usage (in NodeModelZoo_TryLoadRole post-verify):
//
//   #define X(name, stamp_field, runtime_value, fail_mask) \
//       if ((stamp_field) != (runtime_value)) { \
//           BITMAP_SET(handle->drift_flags_at_load, fail_mask); \
//       }
//   FOREACH_ARCH_FIELD_DRIFT(X)
//   #undef X
//======================================================================================================

#define FOREACH_ARCH_FIELD_DRIFT(X)                                                                       \
    /* FOREACH_FEATURE registry hash — schema change (feature added / removed / reordered).         */    \
    /* Match required for prediction integrity; RED severity (set in FOREACH_FAILURE_MODE entry).   */    \
    X(feature_hash,        sr.feature_registry_hash,        FEATURE_REGISTRY_HASH(),                      \
                           FAILURE_MASK_feature_hash_drift)                                               \
    /* FOREACH_TARGET registry hash — label kind change. Same severity as feature drift.            */    \
    X(label_hash,          sr.label_registry_hash,          LABEL_REGISTRY_HASH(),                        \
                           FAILURE_MASK_label_hash_drift)                                                 \
    /* Compile-time build flag hash — diverged when engine rebuilt with different LATENCY_PROFILING */    \
    /* / USE_XGBOOST / etc. since training; YELLOW (predictions may diverge but not necessarily).  */    \
    X(build_flags_hash,    sr.build_flags_hash,             tt::BUILD_FLAGS_HASH(),                       \
                           FAILURE_MASK_build_flags_drift)                                                \
    /* Scaler binding integrity — loaded scaler's training-time registry_hash must match the model's */   \
    /* training-time feature_registry_hash. Diverged when sidecar was copied from a different        */   \
    /* training session. RED severity (scaler applies wrong normalization → silent prediction drift).*/   \
    X(scaler_binding,      sr.feature_registry_hash,        handle->scaler.registry_hash,                 \
                           FAILURE_MASK_scaler_drift)

//======================================================================================================
// [REGISTRY ENTRY COUNT — test instrumentation]
//======================================================================================================
#define ARCH_FIELD_DRIFT_COUNT_ONE(name, stamp_field, runtime_value, fail_mask) +1
#define FOREACH_ARCH_FIELD_DRIFT_COUNT (0 FOREACH_ARCH_FIELD_DRIFT(ARCH_FIELD_DRIFT_COUNT_ONE))

#endif // ARCH_FIELD_DRIFT_REGISTRY_HPP
