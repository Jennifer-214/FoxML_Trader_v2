// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[MemHeaders/NodeCtxInitRegistry.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [FRAMEWORK_DISCIPLINE] [BOOT_TIME]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the unified NodeContext init/reset SSoT — 40 rows with a RESET column (15 RST); init walks ALL, paper-reset walks RST-only; AUTOPOPULATE macros make call sites one-liners]
// [CONTAINS]
//   - [REGISTRY]_[FOREACH_NODE_CTX_FIELD]
//   - [FUNCTION]_[_node_ctx_init_value_fields]   (+ reset walk + _alloc_and_init_slow_state + count sentinels ride)
//   - [MACRO]_[NODE_CTX_*_AUTOPOPULATE]
// [REFERENCE]_[DESIGN_SPEC]_[[autopopulate-pattern-for-production-caller-class] [x-macro-registry-with-presence-dispatch]]
// [REFERENCE]_[CLASS]_[18]
//======================================================================================================
// X-macro registries + AUTOPOPULATE companion macros for per-core init
// and paper-reset paths. Closes a Class-18 mirror class (structural-fix-
// preferred): pre-.B.7, adding a new NodeContext field that needed boot-time
// init required touching `EventLoopState_Init` (~50 line loop body);
// adding a per-session counter also required touching the paper-reset
// path in `EngineSharded.hpp:2237+`. Forgetting either → silently wrong
// state on boot or stale state across paper-reset sessions. v5.4.3
// caught the SL-cooldown leak (`recurring-bugs Class 5`); v4.7.26 caught
// the partner-pairing + gross-accumulator leak; Phase 2.1 caught the
// P&L leak; Phase 3 caught the kill-switch leak. All instances of the
// same recurring class.
//
// ONE unified registry (v5.15.5.F.4d.1.E.1.2 / D-297 — was two parallel lists,
// FOREACH_NODE_CTX_INIT_FIELD + FOREACH_NODE_CTX_RESET_FIELD):
//
//   FOREACH_NODE_CTX_FIELD(X)  →  X(NAME, TYPE, INIT_VALUE, RESET)
//     ~40 boot-init scalar fields on NodeContext that take a value-init
//     (FPN_Binary<F>=Zero, scalar=0/HALT_OK/STRATEGY_NONE/-1, pointer=nullptr,
//     etc.). The RESET column marks the strict subset (RST) that ALSO clears on
//     operator paper-reset — the "per-session state that resets between session
//     boundaries." NORST = boot-init only (kept load-bearing across reset: loaded
//     models, IC history, regression feedback, regime hysteresis; a blanket
//     `node_state_flags = 0` / `pnl_feeder = Init()` would destroy those).
//     v4.7.21/26 / v5.4.3 / Phase 2.1 / Phase 3 anchor each RST membership.
//     Init view walks ALL rows; reset view walks RST-flagged only — both from
//     this ONE list, so the two can never drift (the former class-18 mirror).
//
// AUTOPOPULATE macros (multi-target dispatch):
//
//   NODE_CTX_INIT_AUTOPOPULATE(state_ptr, i)
//     One-line per-slot boot init. Covers value-init registry walk +
//     6 helper-Init calls (sub-structs) + 4 sp_telemetry atomic stores +
//     slow_state arena allocation + NodeContextDisplayMeta_Init sibling
//     init. The ONLY thing left at the call site is the for-loop.
//
//   NODE_CTX_RESET_AUTOPOPULATE(state_ref, c)
//     One-line per-slot paper-reset. Covers reset-subset registry walk +
//     selective KILL_TRIPPED bitmap clear + engine-wide partner_pending
//     bitmap clear for core c. The ONLY thing left at the call site is
//     the for-loop.
//
// Templated helper functions encapsulate the FOREACH walks per
// the templated-helpers discipline (tt::stamp_parse_field<T> precedent). This avoids
// re-expanding the registry at every AUTOPOPULATE call site + gives
// cleaner error diagnostics + lets the helpers be called independently
// from outside the macros if ever needed.
//
// MUST be included AFTER NodeContext<F>, EventLoopState<F>,
// NodeContextDisplayMeta<F>, SlowPathTelemetry, and all helper-Init
// function declarations are visible (otherwise the inline helpers in
// this header fail to compile).
//
// Cross-references (the X-macro-registry / structural-fix-preferred /
// AUTOPOPULATE-companion / templated-helpers disciplines):
//   DESIGN_SPECS/autopopulate-pattern-for-production-caller-class.md
//   DESIGN_SPECS/x-macro-registry-with-presence-dispatch.md
//======================================================================================================
#ifndef NODE_CTX_INIT_REGISTRY_HPP
#define NODE_CTX_INIT_REGISTRY_HPP

#include <atomic>
#include <cstdint>

namespace tt {

//======================================================================
// [REGISTRY]_[FOREACH_NODE_CTX_FIELD]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [FRAMEWORK_DISCIPLINE] [BOOT_TIME]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[40 NodeContext scalar rows in HOT/WARM cluster order — init view walks ALL, paper-reset view walks the 15 RST-flagged; the two can never drift (D-297 unification)]
// [COLUMN]_[NAME]_[field name on NodeContext (lowercase snake_case)]
// [COLUMN]_[type]_[C++ field type; cast applied at write site via (TYPE)(INIT_VALUE)]
// [COLUMN]_[INIT_VALUE]_[boot value; Money_Zero() / numeric / nullptr / sentinel]
// [COLUMN]_[RESET]_[RST = also cleared on operator paper-reset; NORST = boot-init only (load-bearing across reset)]
//======================================================================
// [CODE]
//======================================================================
#define FOREACH_NODE_CTX_FIELD(X)                                                                    \
    /* HOT cluster — dispatch metadata + handles */                                                 \
    X(core,                       ExecutionCore<F>*,  nullptr,        NORST)                         \
    X(model_handle,               void*,              nullptr,        NORST)                         \
    X(ensemble_handle,            void*,              nullptr,        NORST)                         \
    X(strategy_state,             void*,              nullptr,        NORST)                         \
    X(strategy_id,                uint8_t,            STRATEGY_NONE,  NORST)                         \
    X(resolved_strategy_id,       uint8_t,            STRATEGY_NONE,  NORST)                         \
    X(strategy_state_kind,        uint8_t,            0xFF,           NORST)                         \
    X(node_state_flags,           uint8_t,            0,              NORST)                         \
    /* HOT cluster — intended trade values */                                                       \
    X(intended_tp,                Money,              Money_Zero(),   NORST)                         \
    X(intended_sl,                Money,              Money_Zero(),   NORST)                         \
    X(intended_qty,               Money,              Money_Zero(),   NORST)                         \
    X(allocated_balance,          Money,              Money_Zero(),   NORST)                         \
    X(halt_reason,                uint8_t,            HALT_OK,        NORST)                         \
    X(strategy_halt_reason,       uint8_t,            SHALT_OK,       NORST)                         \
    /* HOT cluster — ML decision state (reset every cycle by RebuildOneCore; init for cold-boot safety) */ \
    X(staged_prediction,          double,             0.0,            NORST)                         \
    X(active_prediction,          double,             0.0,            NORST)                         \
    X(last_confidence,            double,             0.0,            NORST)                         \
    X(last_confidence_factor,     double,             0.0,            NORST)                         \
    X(last_exit_prediction,       double,             0.0,            NORST)                         \
    X(last_exit_dominant_horizon, int,                -1,             NORST)                         \
    X(last_buy_dominant_horizon,  int,                -1,             NORST)                         \
    X(last_barrier_mode_used,     uint8_t,            0,              NORST)                         \
    /* WARM cluster — per-event accounting */                                                       \
    X(entries_processed,          uint64_t,           0,              RST)                           \
    X(exits_processed,            uint64_t,           0,              RST)                           \
    X(last_entry_price,           Money,              Money_Zero(),   NORST)                         \
    X(last_entry_tick,            uint64_t,           0,              NORST)                         \
    X(last_entry_wall_us,         uint64_t,           0,              NORST)                         \
    X(sl_cooldown_remaining,      uint32_t,           0,              RST)                           \
    X(idle_cycles,                uint32_t,           0,              RST)                           \
    /* WARM cluster — per-core P&L (v4.0.4) */                                                      \
    X(node_realized,              Money,              Money_Zero(),   RST)                           \
    X(node_fees,                  Money,              Money_Zero(),   RST)                           \
    X(node_wins,                  uint32_t,           0,              RST)                           \
    X(node_losses,                uint32_t,           0,              RST)                           \
    /* WARM cluster — partner pairing + gross accumulators (v4.7.21/26) */                          \
    X(partner_pending_pnl,        Money,              Money_Zero(),   RST)                           \
    X(node_gross_wins,            Money,              Money_Zero(),   RST)                           \
    X(node_gross_losses,          Money,              Money_Zero(),   RST)                           \
    X(node_open_notional,         Money,              Money_Zero(),   RST)                           \
    /* WARM cluster — per-core kill switch (Phase 3) */                                             \
    X(node_peak_balance,          Money,              Money_Zero(),   RST)                           \
    X(node_dd_pct,                Money,              Money_Zero(),   RST)                           \
    X(node_ks_trips_total,        uint32_t,           0,              RST)
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Tuple: X(NAME, TYPE, INIT_VALUE, RESET)
//
// v5.15.5.F.4d.1.E.1.2 (D-297/D2) — UNIFIED into ONE registry (was FOREACH_NODE_CTX_INIT_FIELD
// + FOREACH_NODE_CTX_RESET_FIELD, two parallel lists). Init + reset views now generate from
// this single source (the persist view + wire metadata land in the E.1.2 serializer step —
// Steps 2-4). `RESET` column: RST = also cleared on operator paper-reset (the strict subset —
// per-session state); NORST = boot-init only (load-bearing across reset: handles, models,
// intended trade values, IC/regime/feeder history). The RST set is byte-for-byte the former
// FOREACH_NODE_CTX_RESET_FIELD (Phase 2.1 + Phase 3 + v4.7.21/26 + v5.4.3 anchored).
//
// Order matches the historical EventLoopState_Init body to preserve
// review-readability + match documentation. Field grouping reflects the
// NodeContext HOT/WARM/COLD cluster layout from v5.15.5.B.1.
//
// Adding a new boot-init field: append ONE row here. AUTOPOPULATE picks
// it up at next compile.
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// [[v5.15.5.F.4d.1.E.1.2 D-297] [PAPER-RESET VIEW — the RST-flagged subset]]
//----------------------------------------------------------------------
// Reset is NO LONGER a separate registry (D-297/D2) — it is the `RST`-flagged
// subset of the unified FOREACH_NODE_CTX_FIELD above, generated by presence
// dispatch in `_node_ctx_reset_value_fields`. The RST membership is byte-for-byte
// the former FOREACH_NODE_CTX_RESET_FIELD (a `== 15` static_assert pins it),
// anchored by:
//   - Phase 2.1 (P&L + budget leak): node_realized/fees/wins/losses/open_notional
//   - Phase 3 (kill switch leak): node_peak_balance/dd_pct/ks_trips_total
//     (+ KILL_TRIPPED bitmap clear in AUTOPOPULATE)
//   - v4.7.21/26 (partner pairing + gross): partner_pending_pnl/gross_wins/gross_losses
//     (+ partner_pending_bitmap clear in AUTOPOPULATE)
//   - v5.4.3 (recurring-bugs Class 5): sl_cooldown_remaining, idle_cycles
//   - counter reset: entries_processed, exits_processed
// EXCLUDED (NORST — load-bearing across reset): core, model/ensemble handles,
// and the sub-struct history (confidence / pnl_feeder / regime_state / turnover /
// drift_history) — resetting those would destroy drift-detection / adaptive-feedback /
// regime-hysteresis state (sub-structs aren't in this scalar registry anyway; they're
// helper-Init'd at AUTOPOPULATE Layer 2).
// Adding/removing a reset field: flip a NORST↔RST on its row above. ONE edit, ONE list.
//======================================================================
// [END_REGISTRY]_[FOREACH_NODE_CTX_FIELD]
//======================================================================

//======================================================================
// [FUNCTION]_[_node_ctx_init_value_fields]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [FRAMEWORK_DISCIPLINE] [BOOT_TIME]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the registry-walk helper family (reset walk + _alloc_and_init_slow_state + count sentinels ride) — one instantiation per F; AUTOPOPULATE delegates here]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline void _node_ctx_init_value_fields(NodeContext<F>& ctx) {
#define X(NAME, TYPE, INIT_VAL, RESET) ctx.NAME = (TYPE)(INIT_VAL);
    FOREACH_NODE_CTX_FIELD(X)
#undef X
}

template <unsigned F>
inline void _node_ctx_reset_value_fields(NodeContext<F>& ctx) {
    // reset = the RST-flagged subset (presence dispatch on the RESET column)
#define _NCF_RESET_APPLY_RST(NAME, TYPE, INIT_VAL)   ctx.NAME = (TYPE)(INIT_VAL);
#define _NCF_RESET_APPLY_NORST(NAME, TYPE, INIT_VAL)
#define X(NAME, TYPE, INIT_VAL, RESET) _NCF_RESET_APPLY_##RESET(NAME, TYPE, INIT_VAL)
    FOREACH_NODE_CTX_FIELD(X)
#undef X
#undef _NCF_RESET_APPLY_RST
#undef _NCF_RESET_APPLY_NORST
}

// Allocator policy helper: InitArena_Global → fallback to `new`.
// Encapsulates the 2-branch dispatch so future allocator changes
// (e.g., NUMA-aware arenas, huge-page pool selection) update ONE site.
template <unsigned F>
inline void _alloc_and_init_slow_state(NodeContext<F>& ctx) {
    if (auto* arena = tt::InitArena_Global()) {
        void* mem = tt::InitArena_Alloc(arena,
                                         sizeof(NodeSlowState<F>),
                                         alignof(NodeSlowState<F>));
        ctx.slow_state = mem
            ? new (mem) NodeSlowState<F>()
            : new NodeSlowState<F>();
    } else {
        ctx.slow_state = new NodeSlowState<F>();
    }
    NodeSlowState_Init(ctx.slow_state);
}

//------------------------------------------------------------------
// [SECTION]_[COMPILE-TIME COUNT SENTINELS]
//------------------------------------------------------------------
// Public counts via `>=` style (per /readiness Check 21) — useful for
// downstream consumers + sanity checks.
#define _NODE_CTX_INIT_COUNT_ONE(NAME, TYPE, INIT_VAL, RESET) +1
constexpr int NODE_CTX_INIT_FIELD_COUNT =
    0 FOREACH_NODE_CTX_FIELD(_NODE_CTX_INIT_COUNT_ONE);
#undef _NODE_CTX_INIT_COUNT_ONE
// [ASSERT]_[REGISTRY_COVERAGE]_[NODE_CTX_INIT_FIELD_COUNT >= 30]
static_assert(NODE_CTX_INIT_FIELD_COUNT >= 30,
              "FOREACH_NODE_CTX_FIELD must keep at least the v5.15.5.B.7 "
              "set (30+ fields). Removing entries needs explicit justification.");

// reset count = the RST-flagged rows only (presence dispatch on the RESET column)
#define _NODE_CTX_RESET_COUNT_RST   +1
#define _NODE_CTX_RESET_COUNT_NORST +0
#define _NODE_CTX_RESET_COUNT_ONE(NAME, TYPE, INIT_VAL, RESET) _NODE_CTX_RESET_COUNT_##RESET
constexpr int NODE_CTX_RESET_FIELD_COUNT =
    0 FOREACH_NODE_CTX_FIELD(_NODE_CTX_RESET_COUNT_ONE);
#undef _NODE_CTX_RESET_COUNT_ONE
#undef _NODE_CTX_RESET_COUNT_RST
#undef _NODE_CTX_RESET_COUNT_NORST
// [ASSERT]_[REGISTRY_COVERAGE]_[NODE_CTX_RESET_FIELD_COUNT == 15]
static_assert(NODE_CTX_RESET_FIELD_COUNT == 15,
              "The RST-flagged subset of FOREACH_NODE_CTX_FIELD is EXACTLY the 15 former "
              "FOREACH_NODE_CTX_RESET_FIELD rows (Phase 2.1/3 + v4.7.21/26 + v5.4.3). A "
              "byte-identical-reset pin — flipping a NORST↔RST changes this count deliberately.");
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[_node_ctx_init_value_fields]
//======================================================================

}  // namespace tt

//----------------------------------------------------------------------
// [MACRO]_[NODE_CTX_*_AUTOPOPULATE]
// [TAG]_[[ENGINE] [FRAMEWORK_DISCIPLINE] [BOOT_TIME]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the one-line per-slot call sites — INIT: 5 layers (registry walk / 6 helper-Inits / 4 atomic stores / arena slow_state / sibling display_meta); RESET: RST walk + 2 bitmap clears]
//----------------------------------------------------------------------
// NODE_CTX_INIT_AUTOPOPULATE(state_ptr, i)
//   One-line per-slot boot init. Covers:
//     1. ~40 value-init fields via templated helper _node_ctx_init_value_fields
//     2. 6 helper-Init calls (sub-structs)
//     3. 4 sp_telemetry atomic stores
//     4. slow_state arena allocation + NodeSlowState_Init via _alloc_and_init_slow_state
//     5. NodeContextDisplayMeta_Init for the sibling display_meta[i] entry
//
// NODE_CTX_RESET_AUTOPOPULATE(state_ref, c)
//   One-line per-slot paper-reset. Covers:
//     1. ~15 reset-subset value-init via templated helper _node_ctx_reset_value_fields
//     2. Selective NODE_STATE_FLAG_CLR(KILL_TRIPPED) bitmap op
//     3. Engine-wide partner_pending_bitmap clear for core c
//
// Multi-target dispatch: same registry + helpers serve BOTH boot init
// AND paper reset. Future "per-session counter" additions touch ONE row
// (append it RST-flagged; the D-297 unified registry has no separate
// reset list) — no manual touch of the AUTOPOPULATE macro body unless
// adding a new helper-Init or atomic cluster.

#define NODE_CTX_INIT_AUTOPOPULATE(_state_ptr, _i)                                                  \
    do {                                                                                            \
        auto* _autop_state = (_state_ptr);                                                          \
        int   _autop_idx   = (_i);                                                                  \
        auto& _autop_ctx   = _autop_state->nodes[_autop_idx];                                       \
        /* Layer 1 — registry-driven value-init (~40 fields) */                                     \
        tt::_node_ctx_init_value_fields(_autop_ctx);                                                \
        /* Layer 2 — helper-Init calls (sub-structs with boot defaults) */                          \
        GateParameters_Init(&_autop_ctx.pending_params);                                            \
        ConfidenceScorer_Init(&_autop_ctx.confidence,                                               \
                              CONFIDENCE_IC_WINDOW_DEFAULT,                                         \
                              CONFIDENCE_FRESHNESS_TAU_DEFAULT);                                    \
        DriftHistory_Init(&_autop_ctx.drift_history);                                               \
        RollingTurnover_Init(&_autop_ctx.turnover, 100, 3);                                         \
        Regime_Init(&_autop_ctx.regime_state, 5);                                                   \
        _autop_ctx.pnl_feeder = RegressionFeederX_Init<F>();                                        \
        /* Layer 3 — sp_telemetry alignas(64) cluster atomic stores */                              \
        _autop_ctx.sp_telemetry.last_tick_us.store(0, std::memory_order_relaxed);                   \
        _autop_ctx.sp_telemetry.cycles_total.store(0, std::memory_order_relaxed);                   \
        _autop_ctx.sp_telemetry.yield_count.store(0, std::memory_order_relaxed);                    \
        _autop_ctx.sp_telemetry.state.store(0, std::memory_order_relaxed);                          \
        /* Layer 4 — slow_state arena allocation + NodeSlowState_Init */                            \
        tt::_alloc_and_init_slow_state(_autop_ctx);                                                 \
        /* Layer 5 — sibling-struct init (NodeContextDisplayMeta on EventLoopState) */              \
        NodeContextDisplayMeta_Init(&_autop_state->display_meta[_autop_idx]);                       \
    } while (0)

#define NODE_CTX_RESET_AUTOPOPULATE(_state_ref, _c)                                                 \
    do {                                                                                            \
        auto& _autop_s   = (_state_ref);                                                            \
        int   _autop_c2  = (_c);                                                                    \
        auto& _autop_ctx = _autop_s.nodes[_autop_c2];                                               \
        /* Layer 1 — registry-driven reset-subset value-init (~15 fields) */                        \
        tt::_node_ctx_reset_value_fields(_autop_ctx);                                               \
        /* Layer 2 — selective bitmap operations (Phase 3 KILL_TRIPPED + v5.14.9.G partner) */      \
        NODE_STATE_FLAG_CLR(_autop_ctx, KILL_TRIPPED);                                              \
        BITMAP_CLR(_autop_s.partner_pending_bitmap, BITMAP_BIT_U16(_autop_c2));                     \
    } while (0)

#endif  // NODE_CTX_INIT_REGISTRY_HPP
