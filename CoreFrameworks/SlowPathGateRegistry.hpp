// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [SLOW-PATH GATE REGISTRY — FOREACH_SLOW_PATH_GATE + AUTOPOPULATE]
//======================================================================================================
// Single source of truth for cfg-toggleable slow-path-adjacent gates. Each
// gate is a cfg-driven boolean predicate. Scope column dispatches to the
// appropriate state struct + AUTOPOPULATE walk:
//
//   PER_CORE     — checked in per-core ML rebuild body; uses RESOLVED cfg
//                  (already merged with per-core overrides). Cached on
//                  CoreContext<F>.gate_state (SlowPathGateState).
//   ENGINE_WIDE  — checked in engine-wide outer / function-entry; uses
//                  GLOBAL cfg (no per-core override). Cached on
//                  EventLoopState.global_gate_state (GlobalGateState).
//
// Pattern documented in workspace DESIGN_SPECS/slow-path-gate-registry-pattern.md.
// Composes 3 existing patterns: x-macro-registry-with-presence-dispatch
// (scope column), bitmap-flag-api (bit-pack via uint16_t),
// autopopulate-pattern (production-caller class extinction).
//
// Adding a new gate (1 row):
//   1. Append X(scope, NAME, predicate, "doc") to FOREACH_SLOW_PATH_GATE
//   2. Auto-generated:
//      - GATE_<NAME> bit position (sequential across all scopes)
//      - MASK_<NAME> uint16_t mask constant
//      - State bit on appropriate struct (SlowPathGateState or GlobalGateState)
//      - AUTOPOPULATE walks the new entry in matching scope variant
//      - Other-scope variant SKIPs the entry
//   3. Add use site reading via BITMAP_IS_SET(state.flags, MASK_<NAME>)
//
// Adding a new SCOPE: 1 dispatch macro variant (~5 lines) + 1 AUTOPOPULATE
// variant (~10 lines). Existing registry entries untouched.
//
// Concurrency: per-core slow-path thread is the single writer + reader for
// SlowPathGateState; engine-wide slow-path thread for GlobalGateState. No
// atomics needed (matches v5.14.8.B FailureModeRegistry's failure_flags).
// GUI display reads PerCoreSnap, not gate_state directly.
//
// Established: v5.14.9.B.0 (2026-05-10)
//======================================================================================================

#ifndef SLOW_PATH_GATE_REGISTRY_HPP
#define SLOW_PATH_GATE_REGISTRY_HPP

#include <stdint.h>
#include "../MemHeaders/BitmapMacros.hpp"
#include "../ML_Headers/ConfidenceScore.hpp"  // CURVE_OFF enum (LADDER_ACTIVE predicate)

namespace tt {

//======================================================================================================
// [REGISTRY DEFINITION]
//======================================================================================================
// Tuple: X(scope, name, predicate_expr, doc_string)
//   scope          — PER_CORE | ENGINE_WIDE; selects target state struct + cfg source
//   name           — UPPERCASE token; produces GATE_<name> bit + MASK_<name> constant
//   predicate_expr — bool expression; uses bare `cfg` reference bound by AUTOPOPULATE
//                    caller. PER_CORE gets resolved_cfg; ENGINE_WIDE gets global cfg
//   doc_string     — human-readable description for audits + cfg.example
//
// Adding a new gate to an existing scope: 1 row. Adding a new scope:
// add a new SCOPE_<NAME>_DISPATCH macro pair + a new AUTOPOPULATE variant
// (see "ADDING A SCOPE" section at file bottom).
//======================================================================================================

#define FOREACH_SLOW_PATH_GATE(X)                                                                   \
    /* === PER_CORE — checked in ML_BuildParameters body; uses resolved_cfg === */                  \
    /* v5.14.9.A — soft risk degradation ladder. Composite must be on */                            \
    X(PER_CORE,    LADDER_ACTIVE,                                                                    \
      ((_gate_cfg).risk_degradation_curve != CURVE_OFF) && BITMAP_IS_SET((_gate_cfg).ml_cfg_flags, MASK_ML_CFG_CONFIDENCE_COMPOSITE_ENABLED), \
      "soft risk degradation ladder (curve != OFF AND composite required)")                         \
    /* Pre-v5.14.x — confidence-damped threshold (v5.14.9.F.2 migrated to ml_cfg_flags) */          \
    X(PER_CORE,    CONFIDENCE_ENABLED,                                                               \
      BITMAP_IS_SET((_gate_cfg).ml_cfg_flags, MASK_ML_CFG_CONFIDENCE_ENABLED),                       \
      "scale entry threshold by confidence")                                                         \
    /* v5.14.1.B — composite (4-factor) vs legacy (3-factor) confidence (v5.14.9.F.2 migrated) */   \
    X(PER_CORE,    COMPOSITE_ENABLED,                                                                \
      BITMAP_IS_SET((_gate_cfg).ml_cfg_flags, MASK_ML_CFG_CONFIDENCE_COMPOSITE_ENABLED),             \
      "use 4-factor composite confidence formula (vs legacy 3-factor)")                             \
    /* v5.14.0 — Ridge within-horizon blend */                                                      \
    X(PER_CORE,    RIDGE_WITHIN_ACTIVE,                                                              \
      (_gate_cfg).ridge_within_horizon != 0,                                                               \
      "Ridge blend across role-arms within a horizon")                                              \
    /* v5.14.1.E — Ridge exit-side blend */                                                          \
    X(PER_CORE,    EXIT_BLENDER_ACTIVE,                                                              \
      (_gate_cfg).exit_blender_mode != 0,                                                                  \
      "Ridge blend across exit_predictor handles")                                                  \
    /* v5.14.10.B — Thompson sampling active (cfg.bandit_algorithm in {1, 2}) */                    \
    X(PER_CORE,    THOMPSON_ACTIVE,                                                                  \
      (_gate_cfg).bandit_algorithm != 0,                                                             \
      "Thompson sampling bandit dispatched (cfg=1 THOMPSON or cfg=2 BOTH)")                         \
    /* v5.14.10.B — cfg=2 dual-mode (parallel-training A/B telemetry) */                            \
    X(PER_CORE,    BANDIT_BOTH_ACTIVE,                                                               \
      (_gate_cfg).bandit_algorithm == 2,                                                             \
      "Both Exp3 + Thompson run per cycle (cfg=2 calib log telemetry)")                             \
    /* === ENGINE_WIDE — checked in engine-wide outer / function-entry; uses global cfg === */     \
    /* v5.12.2.B — lazy slow-path rebuild predicate (function-entry of EventLoop_RebuildOneCore) */ \
    X(ENGINE_WIDE, LAZY_REBUILD_ACTIVE,                                                              \
      BITMAP_IS_SET((_gate_cfg).ml_cfg_flags, MASK_ML_CFG_LAZY_REBUILD_ENABLED),                     \
      "skip RebuildOneCore body when state hasn't changed materially")                              \
    /* v5.12.1.A — WS staleness emergency-flatten (engine-wide outer) */                            \
    X(ENGINE_WIDE, WS_FLATTEN_ACTIVE,                                                                \
      BITMAP_IS_SET((_gate_cfg).risk_cfg_flags, MASK_RISK_CFG_WS_DEAD_TIME_FLATTEN_ENABLED),         \
      "fire OMS_FlattenAll when WS dead longer than threshold")

//======================================================================================================
// [SCOPE-DISPATCH HELPER MACROS — same shape as STAMP_HANDLE_GEN_INCLUDE/SKIP]
//======================================================================================================
// Each AUTOPOPULATE variant uses a dispatch chain:
//   X_AUTOPOP_<VARIANT>_DISPATCH_<SCOPE>(name, predicate, doc) →
//     either INCLUDE (emit OR-mask code) or SKIP (emit nothing)
//
// Pattern: when the variant matches the entry's scope, dispatch INCLUDE.
// Otherwise SKIP. Token-paste resolves at preprocessor expansion.
//======================================================================================================

// PER_CORE variant — INCLUDEs PER_CORE entries; SKIPs ENGINE_WIDE
#define X_AUTOPOP_PER_CORE_INCLUDE(name, predicate, doc)                                             \
    _new_flags |= ((predicate) ? MASK_##name : 0u);
#define X_AUTOPOP_PER_CORE_SKIP(name, predicate, doc) /* skip; not in this variant's scope */

#define X_AUTOPOP_PER_CORE_DISPATCH_PER_CORE     X_AUTOPOP_PER_CORE_INCLUDE
#define X_AUTOPOP_PER_CORE_DISPATCH_ENGINE_WIDE  X_AUTOPOP_PER_CORE_SKIP

#define X_AUTOPOP_PER_CORE_WALK(scope, name, predicate, doc)                                         \
    X_AUTOPOP_PER_CORE_DISPATCH_##scope(name, predicate, doc)

// ENGINE_WIDE variant — INCLUDEs ENGINE_WIDE entries; SKIPs PER_CORE
#define X_AUTOPOP_ENGINE_WIDE_INCLUDE(name, predicate, doc)                                          \
    _new_flags |= ((predicate) ? MASK_##name : 0u);
#define X_AUTOPOP_ENGINE_WIDE_SKIP(name, predicate, doc) /* skip */

#define X_AUTOPOP_ENGINE_WIDE_DISPATCH_PER_CORE     X_AUTOPOP_ENGINE_WIDE_SKIP
#define X_AUTOPOP_ENGINE_WIDE_DISPATCH_ENGINE_WIDE  X_AUTOPOP_ENGINE_WIDE_INCLUDE

#define X_AUTOPOP_ENGINE_WIDE_WALK(scope, name, predicate, doc)                                      \
    X_AUTOPOP_ENGINE_WIDE_DISPATCH_##scope(name, predicate, doc)

// === Bit / mask / count generation (walks all scopes, no dispatch) ===
// All entries get a sequential GATE_<NAME> + MASK_<NAME> regardless of scope.
// State structs use only bits matching their scope; other bits stay 0.

#define X_GEN_GATE_BIT(scope, name, predicate, doc)  GATE_##name,

enum SlowPathGate {
    FOREACH_SLOW_PATH_GATE(X_GEN_GATE_BIT)
    GATE_SLOW_PATH_TOTAL_COUNT  // sentinel; total bits across all scopes
};

#undef X_GEN_GATE_BIT

#define X_GEN_GATE_MASK(scope, name, predicate, doc) \
    static constexpr uint16_t MASK_##name = BITMAP_BIT_U16(GATE_##name);
FOREACH_SLOW_PATH_GATE(X_GEN_GATE_MASK)
#undef X_GEN_GATE_MASK

static_assert(GATE_SLOW_PATH_TOTAL_COUNT <= 16,
              "Total slow-path gate bits exceed uint16_t; expand state structs to uint32_t");

// === Per-scope counts (for tests; uses >= per /readiness Check 21) ===

#define X_GEN_PER_CORE_COUNT_DISPATCH_PER_CORE(name, predicate, doc) +1
#define X_GEN_PER_CORE_COUNT_DISPATCH_ENGINE_WIDE(name, predicate, doc) /* skip */
#define X_GEN_PER_CORE_COUNT(scope, name, predicate, doc) \
    X_GEN_PER_CORE_COUNT_DISPATCH_##scope(name, predicate, doc)
#define FOREACH_SLOW_PATH_GATE_PER_CORE_COUNT (0 FOREACH_SLOW_PATH_GATE(X_GEN_PER_CORE_COUNT))

#define X_GEN_ENGINE_WIDE_COUNT_DISPATCH_PER_CORE(name, predicate, doc) /* skip */
#define X_GEN_ENGINE_WIDE_COUNT_DISPATCH_ENGINE_WIDE(name, predicate, doc) +1
#define X_GEN_ENGINE_WIDE_COUNT(scope, name, predicate, doc) \
    X_GEN_ENGINE_WIDE_COUNT_DISPATCH_##scope(name, predicate, doc)
#define FOREACH_SLOW_PATH_GATE_ENGINE_WIDE_COUNT (0 FOREACH_SLOW_PATH_GATE(X_GEN_ENGINE_WIDE_COUNT))

//======================================================================================================
// [PER-SCOPE STATE STRUCTS]
//======================================================================================================
// Two structs keep the cache surfaces explicit. Use sites pick the right
// one + read via BITMAP_IS_SET(state.flags, MASK_<NAME>).
//
// uint16_t flags both: bits matching the struct's scope are set; other-scope
// bits stay 0 (always). Total bits ≤ 16 per static_assert above.

// Per-core gate cache. Lives on CoreContext<F>.gate_state. AUTOPOPULATE
// called per-core after ControllerConfig_ResolveForCore (so cfg has
// per-core overrides merged).
struct SlowPathGateState {
    uint16_t flags;
};

// Engine-wide gate cache. Lives on EventLoopState.global_gate_state.
// AUTOPOPULATE called once per slow-path entry with the global cfg.
struct GlobalGateState {
    uint16_t flags;
};

//======================================================================================================
// [AUTOPOPULATE COMPANION MACROS]
//======================================================================================================
// SLOW_PATH_GATE_AUTOPOPULATE_PER_CORE(state, _resolved_cfg)
//   Walks PER_CORE entries; sets each bit via mask OR-reduction.
//   Caller invokes ControllerConfig_ResolveForCore upstream so the cfg
//   already has per-core overrides merged.
//
// SLOW_PATH_GATE_AUTOPOPULATE_ENGINE_WIDE(state, _global_cfg)
//   Walks ENGINE_WIDE entries; sets each bit via mask OR-reduction.
//   Uses global cfg (no per-core resolution needed).
//
// Branchless: each predicate emits cmov; OR-reduction across N predicates;
// single store at the end. No conditional stores; no per-gate branches.
//
// Pattern: same shape as STAMP_CFG_AUTOPOPULATE (v5.14.1.B.3).
//======================================================================================================

#define SLOW_PATH_GATE_AUTOPOPULATE_PER_CORE(state, _cfg_arg)                                        \
    do {                                                                                             \
        const auto& _gate_cfg = (_cfg_arg);                                                          \
        uint16_t _new_flags = 0;                                                                     \
        FOREACH_SLOW_PATH_GATE(X_AUTOPOP_PER_CORE_WALK)                                              \
        (state).flags = _new_flags;                                                                  \
        (void)_gate_cfg;                                                                             \
    } while (0)

#define SLOW_PATH_GATE_AUTOPOPULATE_ENGINE_WIDE(state, _cfg_arg)                                     \
    do {                                                                                             \
        const auto& _gate_cfg = (_cfg_arg);                                                          \
        uint16_t _new_flags = 0;                                                                     \
        FOREACH_SLOW_PATH_GATE(X_AUTOPOP_ENGINE_WIDE_WALK)                                           \
        (state).flags = _new_flags;                                                                  \
        (void)_gate_cfg;                                                                             \
    } while (0)

}  // namespace tt

//======================================================================================================
// [ADDING A NEW SCOPE — instructions for future contributors]
//======================================================================================================
// Steps to add a new scope (e.g., HOT_PATH or BOOT) to FOREACH_SLOW_PATH_GATE:
//
// 1. Add scope-dispatch macros for EXISTING variants (PER_CORE, ENGINE_WIDE):
//      #define X_AUTOPOP_PER_CORE_DISPATCH_<NEW_SCOPE>     X_AUTOPOP_PER_CORE_SKIP
//      #define X_AUTOPOP_ENGINE_WIDE_DISPATCH_<NEW_SCOPE>  X_AUTOPOP_ENGINE_WIDE_SKIP
//      #define X_GEN_PER_CORE_COUNT_DISPATCH_<NEW_SCOPE>(...)     /* skip */
//      #define X_GEN_ENGINE_WIDE_COUNT_DISPATCH_<NEW_SCOPE>(...)  /* skip */
//
// 2. Add the new variant's dispatch chain (mirroring PER_CORE pattern):
//      #define X_AUTOPOP_<NEW_SCOPE>_INCLUDE(name, predicate, doc) \
//          _new_flags |= ((predicate) ? MASK_##name : 0u);
//      #define X_AUTOPOP_<NEW_SCOPE>_SKIP(name, predicate, doc) /* skip */
//      #define X_AUTOPOP_<NEW_SCOPE>_DISPATCH_PER_CORE     X_AUTOPOP_<NEW_SCOPE>_SKIP
//      #define X_AUTOPOP_<NEW_SCOPE>_DISPATCH_ENGINE_WIDE  X_AUTOPOP_<NEW_SCOPE>_SKIP
//      #define X_AUTOPOP_<NEW_SCOPE>_DISPATCH_<NEW_SCOPE>  X_AUTOPOP_<NEW_SCOPE>_INCLUDE
//      #define X_AUTOPOP_<NEW_SCOPE>_WALK(scope, name, predicate, doc) \
//          X_AUTOPOP_<NEW_SCOPE>_DISPATCH_##scope(name, predicate, doc)
//
// 3. Add the new state struct + AUTOPOPULATE macro:
//      struct <Scope>GateState { uint16_t flags; };
//      #define SLOW_PATH_GATE_AUTOPOPULATE_<NEW_SCOPE>(state, cfg) ...
//
// 4. Append entries to FOREACH_SLOW_PATH_GATE with the new scope token.
//
// EXISTING entries do not change. Existing AUTOPOPULATE variants do not
// change (they SKIP the new scope automatically via dispatch).
//======================================================================================================

#endif  // SLOW_PATH_GATE_REGISTRY_HPP
