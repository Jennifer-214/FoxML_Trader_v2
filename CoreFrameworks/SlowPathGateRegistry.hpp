// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[CoreFrameworks/SlowPathGateRegistry.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [CFG_FLOW] [FRAMEWORK_DISCIPLINE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the cfg-flag eligibility canon — scope-dispatched slow-path gate registry + branchless AUTOPOPULATE caches]
// [CONTAINS]
//   - [REGISTRY]_[FOREACH_SLOW_PATH_GATE]
//======================================================================================================

#ifndef SLOW_PATH_GATE_REGISTRY_HPP
#define SLOW_PATH_GATE_REGISTRY_HPP

#include <stdint.h>
#include "../MemHeaders/BitmapMacros.hpp"
#include "../ML_Headers/ConfidenceScore.hpp"  // CURVE_OFF enum (LADDER_ACTIVE predicate)

namespace tt {

//======================================================================
// [REGISTRY]_[FOREACH_SLOW_PATH_GATE]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [CFG_FLOW] [BITMAP_PACKED]]
// [REFERENCE]_[DESIGN_SPEC]_[slow-path-gate-registry-pattern]
// [REFERENCE]_[DESIGN_SPEC]_[x-macro-registry-with-presence-dispatch]
// [REFERENCE]_[DESIGN_SPEC]_[[bitmap-flag-api] [slow-path-gate-registry-pattern.md]]
// [REFERENCE]_[INVARIANT]_[H20]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[cfg-toggleable slow-path gates — scope column dispatches PER_NODE (resolved cfg) vs ENGINE_WIDE (global cfg)]
// [COLUMN]_[scope]_[PER_NODE | ENGINE_WIDE; selects target state struct + cfg source]
// [COLUMN]_[name]_[UPPERCASE token; produces GATE_<name> bit + MASK_<name> constant]
// [COLUMN]_[predicate_expr]_[bool expression over the AUTOPOPULATE-bound _gate_cfg; PER_NODE gets resolved_cfg, ENGINE_WIDE gets global cfg]
// [COLUMN]_[doc_string]_[human-readable description for audits + cfg.example]
// [REFERENCE]_[CLASS]_[[18] [28]]
// [REFERENCE]_[PARITY]_[PARITY-32]
//======================================================================
// [CODE]
//======================================================================
#define FOREACH_SLOW_PATH_GATE(X)                                                                   \
    /* === PER_NODE — checked in ML_BuildParameters body; uses resolved_cfg === */                  \
    /* v5.14.9.A — soft risk degradation ladder. Composite must be on */                            \
    X(PER_NODE,    LADDER_ACTIVE,                                                                    \
      ((_gate_cfg).risk_degradation_curve != CURVE_OFF) && BITMAP_IS_SET((_gate_cfg).ml_cfg_flags, MASK_ML_CFG_CONFIDENCE_COMPOSITE_ENABLED), \
      "soft risk degradation ladder (curve != OFF AND composite required)")                         \
    /* Pre-v5.14.x — confidence-damped threshold (v5.14.9.F.2 migrated to ml_cfg_flags) */          \
    X(PER_NODE,    CONFIDENCE_ENABLED,                                                               \
      BITMAP_IS_SET((_gate_cfg).ml_cfg_flags, MASK_ML_CFG_CONFIDENCE_ENABLED),                       \
      "scale entry threshold by confidence")                                                         \
    /* v5.14.1.B — composite (4-factor) vs legacy (3-factor) confidence (v5.14.9.F.2 migrated) */   \
    X(PER_NODE,    COMPOSITE_ENABLED,                                                                \
      BITMAP_IS_SET((_gate_cfg).ml_cfg_flags, MASK_ML_CFG_CONFIDENCE_COMPOSITE_ENABLED),             \
      "use 4-factor composite confidence formula (vs legacy 3-factor)")                             \
    /* v5.14.0 — Ridge within-horizon blend (v5.14.11.C migrated to ml_cfg_flags bitmap) */         \
    X(PER_NODE,    RIDGE_WITHIN_ACTIVE,                                                              \
      BITMAP_IS_SET((_gate_cfg).ml_cfg_flags, MASK_ML_CFG_RIDGE_WITHIN_HORIZON),                     \
      "Ridge blend across role-arms within a horizon")                                              \
    /* v5.14.1.E — Ridge exit-side blend (v5.14.11.C migrated to ml_cfg_flags bitmap) */            \
    X(PER_NODE,    EXIT_BLENDER_ACTIVE,                                                              \
      BITMAP_IS_SET((_gate_cfg).ml_cfg_flags, MASK_ML_CFG_EXIT_BLENDER_MODE),                        \
      "Ridge blend across exit_predictor handles")                                                  \
    /* v5.14.11.C — Ridge online correlation matrix (sliding-window incremental) gate */            \
    X(PER_NODE,    RIDGE_ONLINE_CORR_ACTIVE,                                                         \
      BITMAP_IS_SET((_gate_cfg).ml_cfg_flags, MASK_ML_CFG_RIDGE_ONLINE_CORR),                        \
      "use sliding-window incremental correlation matrix in Ridge (vs full recompute)")             \
    /* v5.15.5.F.4d — metadata-derived predicates per § I of merged plan body. Replaces former */ \
    /* cfg.bandit_algorithm == X if-chain with branchless mask-bit shift over auto-derived       */ \
    /* BANDIT_THOMPSON_UPDATE_MASK / BANDIT_EXP3_UPDATE_MASK (from FOREACH_BANDIT_ALGORITHM       */ \
    /* metadata column reductions in BanditAlgorithmRegistry.hpp). Adding a 6th bandit algorithm */ \
    /* with appropriate thompson_up/exp3_up bits → these predicates auto-extend correctness; no  */ \
    /* per-site rebind needed. Class 18 + Class 28 closure at slow-path gate surface.            */ \
    X(PER_NODE,    THOMPSON_ACTIVE,                                                                  \
      (((uint8_t)BANDIT_THOMPSON_UPDATE_MASK >> (_gate_cfg).bandit_algorithm) & 1u),                 \
      "Thompson posterior is being updated for the current bandit_algorithm (any state with thompson_up=1)") \
    /* v5.15.5.F.4d — RENAMED from BANDIT_BOTH_ACTIVE. Semantic: BOTH algos learning from rewards.  */ \
    /* True for any state where exp3_up=1 AND thompson_up=1 (cfg=2/3/4 post-.F.4d expansion).        */ \
    X(PER_NODE,    BANDIT_SHADOW_LEARNING,                                                           \
      ((((uint8_t)BANDIT_EXP3_UPDATE_MASK & (uint8_t)BANDIT_THOMPSON_UPDATE_MASK) >> (_gate_cfg).bandit_algorithm) & 1u), \
      "Both Exp3 + Thompson learning from rewards this cycle (any algo with exp3_up=1 AND thompson_up=1)") \
    /* === ENGINE_WIDE — checked in engine-wide outer / function-entry; uses global cfg === */     \
    /* v5.12.2.B — lazy slow-path rebuild predicate (function-entry of EventLoop_RebuildOneCore) */ \
    X(ENGINE_WIDE, LAZY_REBUILD_ACTIVE,                                                              \
      BITMAP_IS_SET((_gate_cfg).ml_cfg_flags, MASK_ML_CFG_LAZY_REBUILD_ENABLED),                     \
      "skip RebuildOneCore body when state hasn't changed materially")                              \
    /* v5.12.1.A — WS staleness emergency-flatten (engine-wide outer) */                            \
    X(ENGINE_WIDE, WS_FLATTEN_ACTIVE,                                                                \
      BITMAP_IS_SET((_gate_cfg).risk_cfg_flags, MASK_RISK_CFG_WS_DEAD_TIME_FLATTEN_ENABLED),         \
      "fire OMS_FlattenAll when WS dead longer than threshold")                                      \
    /* v5.15.5.F.4d.1.B.4 — PARITY-032 closure: cache lifecycle cfg flag engine-wide for branchless */ \
    /* gating inside SlowPathCycleOneCore. Sister consumer pattern at ControllerEventLoop.hpp:2344  */ \
    /* (MASK_LAZY_REBUILD_ACTIVE) + :3558 (MASK_WS_FLATTEN_ACTIVE). D1-B applies slow-path-gate     */ \
    /* cache pattern to breakeven for the first time (NOT cosmetic-extension of wrapper at :3799   */ \
    /* which reads cfg directly). Default cfg unset → cached bit 0 → branchless skip. Mask name    */ \
    /* MASK_BREAKEVEN_ON_PROFIT distinct from cfg-side MASK_LIFECYCLE_CFG_BREAKEVEN_ON_PROFIT by    */ \
    /* full-token compare + prefix discipline (slow-path-gate uses MASK_<name>; cfg-flag uses       */ \
    /* MASK_LIFECYCLE_CFG_<name>).                                                                  */ \
    X(ENGINE_WIDE, BREAKEVEN_ON_PROFIT,                                                              \
      BITMAP_IS_SET((_gate_cfg).lifecycle_cfg_flags, MASK_LIFECYCLE_CFG_BREAKEVEN_ON_PROFIT),        \
      "breakeven-on-profit lifecycle event (ratchet SL to fee-floored breakeven when in profit)")

//------------------------------------------------------------------------------
// [SECTION]_[scope-dispatch helper macros — same shape as STAMP_HANDLE_GEN_INCLUDE/SKIP]
//------------------------------------------------------------------------------
// Each AUTOPOPULATE variant uses a dispatch chain:
//   X_AUTOPOP_<VARIANT>_DISPATCH_<SCOPE>(name, predicate, doc) →
//     either INCLUDE (emit OR-mask code) or SKIP (emit nothing)
//
// Pattern: when the variant matches the entry's scope, dispatch INCLUDE.
// Otherwise SKIP. Token-paste resolves at preprocessor expansion.
//------------------------------------------------------------------------------

// PER_NODE variant — INCLUDEs PER_NODE entries; SKIPs ENGINE_WIDE
#define X_AUTOPOP_PER_NODE_INCLUDE(name, predicate, doc)                                             \
    _new_flags |= ((predicate) ? MASK_##name : 0u);
#define X_AUTOPOP_PER_NODE_SKIP(name, predicate, doc) /* skip; not in this variant's scope */

#define X_AUTOPOP_PER_NODE_DISPATCH_PER_NODE     X_AUTOPOP_PER_NODE_INCLUDE
#define X_AUTOPOP_PER_NODE_DISPATCH_ENGINE_WIDE  X_AUTOPOP_PER_NODE_SKIP

#define X_AUTOPOP_PER_NODE_WALK(scope, name, predicate, doc)                                         \
    X_AUTOPOP_PER_NODE_DISPATCH_##scope(name, predicate, doc)

// ENGINE_WIDE variant — INCLUDEs ENGINE_WIDE entries; SKIPs PER_NODE
#define X_AUTOPOP_ENGINE_WIDE_INCLUDE(name, predicate, doc)                                          \
    _new_flags |= ((predicate) ? MASK_##name : 0u);
#define X_AUTOPOP_ENGINE_WIDE_SKIP(name, predicate, doc) /* skip */

#define X_AUTOPOP_ENGINE_WIDE_DISPATCH_PER_NODE     X_AUTOPOP_ENGINE_WIDE_SKIP
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

#define X_GEN_PER_NODE_COUNT_DISPATCH_PER_NODE(name, predicate, doc) +1
#define X_GEN_PER_NODE_COUNT_DISPATCH_ENGINE_WIDE(name, predicate, doc) /* skip */
#define X_GEN_PER_NODE_COUNT(scope, name, predicate, doc) \
    X_GEN_PER_NODE_COUNT_DISPATCH_##scope(name, predicate, doc)
#define FOREACH_SLOW_PATH_GATE_PER_NODE_COUNT (0 FOREACH_SLOW_PATH_GATE(X_GEN_PER_NODE_COUNT))

#define X_GEN_ENGINE_WIDE_COUNT_DISPATCH_PER_NODE(name, predicate, doc) /* skip */
#define X_GEN_ENGINE_WIDE_COUNT_DISPATCH_ENGINE_WIDE(name, predicate, doc) +1
#define X_GEN_ENGINE_WIDE_COUNT(scope, name, predicate, doc) \
    X_GEN_ENGINE_WIDE_COUNT_DISPATCH_##scope(name, predicate, doc)
#define FOREACH_SLOW_PATH_GATE_ENGINE_WIDE_COUNT (0 FOREACH_SLOW_PATH_GATE(X_GEN_ENGINE_WIDE_COUNT))

//------------------------------------------------------------------------------
// [SECTION]_[per-scope state structs]
//------------------------------------------------------------------------------
// Two structs keep the cache surfaces explicit. Use sites pick the right
// one + read via BITMAP_IS_SET(state.flags, MASK_<NAME>).
//
// uint16_t flags both: bits matching the struct's scope are set; other-scope
// bits stay 0 (always). Total bits ≤ 16 per static_assert above.

// Per-core gate cache. Lives on NodeContext<F>.gate_state. AUTOPOPULATE
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

//------------------------------------------------------------------------------
// [SECTION]_[autopopulate companion macros]
//------------------------------------------------------------------------------
// SLOW_PATH_GATE_AUTOPOPULATE_PER_NODE(state, _resolved_cfg)
//   Walks PER_NODE entries; sets each bit via mask OR-reduction.
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

#define SLOW_PATH_GATE_AUTOPOPULATE_PER_NODE(state, _cfg_arg)                                        \
    do {                                                                                             \
        const auto& _gate_cfg = (_cfg_arg);                                                          \
        uint16_t _new_flags = 0;                                                                     \
        FOREACH_SLOW_PATH_GATE(X_AUTOPOP_PER_NODE_WALK)                                              \
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
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]_[SSoT story + add-a-gate recipe + concurrency]
//----------------------------------------------------------------------
// Single source of truth for cfg-toggleable slow-path-adjacent gates. Each
// gate is a cfg-driven boolean predicate. Scope column dispatches to the
// appropriate state struct + AUTOPOPULATE walk:
//
//   PER_NODE     — checked in per-core ML rebuild body; uses RESOLVED cfg
//                  (already merged with per-core overrides). Cached on
//                  NodeContext<F>.gate_state (SlowPathGateState).
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
// GUI display reads PerNodeSnap, not gate_state directly.
//
// Established: v5.14.9.B.0 (2026-05-10)
//======================================================================
// [COMMENT]_[adding a new scope — instructions for future contributors]
//----------------------------------------------------------------------
// Steps to add a new scope (e.g., HOT_PATH or BOOT) to FOREACH_SLOW_PATH_GATE:
//
// 1. Add scope-dispatch macros for EXISTING variants (PER_NODE, ENGINE_WIDE):
//      #define X_AUTOPOP_PER_NODE_DISPATCH_<NEW_SCOPE>     X_AUTOPOP_PER_NODE_SKIP
//      #define X_AUTOPOP_ENGINE_WIDE_DISPATCH_<NEW_SCOPE>  X_AUTOPOP_ENGINE_WIDE_SKIP
//      #define X_GEN_PER_NODE_COUNT_DISPATCH_<NEW_SCOPE>(...)     /* skip */
//      #define X_GEN_ENGINE_WIDE_COUNT_DISPATCH_<NEW_SCOPE>(...)  /* skip */
//
// 2. Add the new variant's dispatch chain (mirroring PER_NODE pattern):
//      #define X_AUTOPOP_<NEW_SCOPE>_INCLUDE(name, predicate, doc) \
//          _new_flags |= ((predicate) ? MASK_##name : 0u);
//      #define X_AUTOPOP_<NEW_SCOPE>_SKIP(name, predicate, doc) /* skip */
//      #define X_AUTOPOP_<NEW_SCOPE>_DISPATCH_PER_NODE     X_AUTOPOP_<NEW_SCOPE>_SKIP
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
//======================================================================
// [DERIVED]   (tool-refreshed — ROW_COUNT/CONSUMERS generators land with the drift-gate generalization; empty skeleton is correct, D-327)
//======================================================================
// [END_REGISTRY]_[FOREACH_SLOW_PATH_GATE]
//======================================================================

}  // namespace tt

#endif  // SLOW_PATH_GATE_REGISTRY_HPP
