// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [CORE CONTEXT INIT/RESET REGISTRY — v5.15.5.B.7]
//======================================================================================================
// X-macro registries + AUTOPOPULATE companion macros for per-core init
// and paper-reset paths. Closes a Class-18 mirror class (CLAUDE.md item
// 19): pre-.B.7, adding a new CoreContext field that needed boot-time
// init required touching `EventLoopState_Init` (~50 line loop body);
// adding a per-session counter also required touching the paper-reset
// path in `EngineSharded.hpp:2237+`. Forgetting either → silently wrong
// state on boot or stale state across paper-reset sessions. v5.4.3
// caught the SL-cooldown leak (`recurring-bugs Class 5`); v4.7.26 caught
// the partner-pairing + gross-accumulator leak; Phase 2.1 caught the
// P&L leak; Phase 3 caught the kill-switch leak. All instances of the
// same recurring class.
//
// Two registries (intentional separation — semantic distinction):
//
//   FOREACH_CORE_CTX_INIT_FIELD(X)
//     ~40 entries. ALL boot-init fields on CoreContext that take a
//     value-init (FPN<F>=Zero, scalar=0/HALT_OK/STRATEGY_NONE/-1,
//     pointer=nullptr, etc.). Walked at boot only.
//
//   FOREACH_CORE_CTX_RESET_FIELD(X)
//     ~15 entries. STRICT SUBSET of init fields — only the "per-session
//     state that should reset between operator-initiated session
//     boundaries" (paper reset). Carefully selected (NOT a blanket
//     `core_state_flags = 0` or `pnl_feeder = Init()` which would
//     destroy load-bearing state like loaded models, IC history,
//     regression feedback, regime hysteresis). v4.7.21 / v4.7.26 /
//     v5.4.3 / Phase 2.1 / Phase 3 anchor each membership decision.
//
// AUTOPOPULATE macros (multi-target dispatch):
//
//   CORE_CTX_INIT_AUTOPOPULATE(state_ptr, i)
//     One-line per-slot boot init. Covers value-init registry walk +
//     6 helper-Init calls (sub-structs) + 4 sp_telemetry atomic stores +
//     slow_state arena allocation + CoreContextDisplayMeta_Init sibling
//     init. The ONLY thing left at the call site is the for-loop.
//
//   CORE_CTX_RESET_AUTOPOPULATE(state_ref, c)
//     One-line per-slot paper-reset. Covers reset-subset registry walk +
//     selective KILL_TRIPPED bitmap clear + engine-wide partner_pending
//     bitmap clear for core c. The ONLY thing left at the call site is
//     the for-loop.
//
// Templated helper functions encapsulate the FOREACH walks per
// CLAUDE.md item 23 (tt::stamp_parse_field<T> precedent). This avoids
// re-expanding the registry at every AUTOPOPULATE call site + gives
// cleaner error diagnostics + lets the helpers be called independently
// from outside the macros if ever needed.
//
// MUST be included AFTER CoreContext<F>, EventLoopState<F>,
// CoreContextDisplayMeta<F>, SlowPathTelemetry, and all helper-Init
// function declarations are visible (otherwise the inline helpers in
// this header fail to compile).
//
// Cross-references:
//   CLAUDE.md item 13 (X-macro registry pattern)
//   CLAUDE.md item 19 (structural fix preferred when bug class can recur)
//   CLAUDE.md item 21 (AUTOPOPULATE companion macro pattern)
//   CLAUDE.md item 23 (templated helper for type-trait dispatch)
//   DESIGN_SPECS/autopopulate-pattern-for-production-caller-class.md
//   DESIGN_SPECS/x-macro-registry-with-presence-dispatch.md
//======================================================================================================
#ifndef CORE_CTX_INIT_REGISTRY_HPP
#define CORE_CTX_INIT_REGISTRY_HPP

#include <atomic>
#include <cstdint>

namespace tt {

//======================================================================================================
// [BOOT-INIT FIELD REGISTRY]
//======================================================================================================
// Tuple: X(NAME, TYPE, INIT_VALUE)
//   NAME       — field name on CoreContext (lowercase snake_case)
//   TYPE       — C++ field type; cast applied at write site via (TYPE)(INIT_VALUE)
//   INIT_VALUE — initial value at boot; FPN<F>=FPN_Zero<F>(), scalar=numeric, ptr=nullptr
//
// Order matches the historical EventLoopState_Init body to preserve
// review-readability + match documentation. Field grouping reflects the
// CoreContext HOT/WARM/COLD cluster layout from v5.15.5.B.1.
//
// Adding a new boot-init field: append ONE row here. AUTOPOPULATE picks
// it up at next compile.
//======================================================================================================
#define FOREACH_CORE_CTX_INIT_FIELD(X)                                                              \
    /* HOT cluster — dispatch metadata + handles */                                                 \
    X(core,                       ExecutionCore<F>*,  nullptr)                                      \
    X(model_handle,               void*,              nullptr)                                      \
    X(ensemble_handle,            void*,              nullptr)                                      \
    X(strategy_state,             void*,              nullptr)                                      \
    X(strategy_id,                uint8_t,            STRATEGY_NONE)                                \
    X(resolved_strategy_id,       uint8_t,            STRATEGY_NONE)                                \
    X(strategy_state_kind,        uint8_t,            0xFF)                                         \
    X(core_state_flags,           uint8_t,            0)                                            \
    /* HOT cluster — intended trade values */                                                       \
    X(intended_tp,                FPN<F>,             FPN_Zero<F>())                                \
    X(intended_sl,                FPN<F>,             FPN_Zero<F>())                                \
    X(intended_qty,               FPN<F>,             FPN_Zero<F>())                                \
    X(allocated_balance,          FPN<F>,             FPN_Zero<F>())                                \
    X(halt_reason,                uint8_t,            HALT_OK)                                      \
    X(strategy_halt_reason,       uint8_t,            SHALT_OK)                                     \
    /* HOT cluster — ML decision state (reset every cycle by RebuildOneCore; init for cold-boot safety) */ \
    X(staged_prediction,          double,             0.0)                                          \
    X(active_prediction,          double,             0.0)                                          \
    X(last_confidence,            double,             0.0)                                          \
    X(last_confidence_factor,     double,             0.0)                                          \
    X(last_exit_prediction,       double,             0.0)                                          \
    X(last_exit_dominant_horizon, int,                -1)                                           \
    X(last_buy_dominant_horizon,  int,                -1)                                           \
    X(last_barrier_mode_used,     uint8_t,            0)                                            \
    /* WARM cluster — per-event accounting */                                                       \
    X(entries_processed,          uint64_t,           0)                                            \
    X(exits_processed,            uint64_t,           0)                                            \
    X(last_entry_price,           FPN<F>,             FPN_Zero<F>())                                \
    X(last_entry_tick,            uint64_t,           0)                                            \
    X(last_entry_wall_us,         uint64_t,           0)                                            \
    X(sl_cooldown_remaining,      uint32_t,           0)                                            \
    X(idle_cycles,                uint32_t,           0)                                            \
    /* WARM cluster — per-core P&L (v4.0.4) */                                                      \
    X(core_realized,              FPN<F>,             FPN_Zero<F>())                                \
    X(core_fees,                  FPN<F>,             FPN_Zero<F>())                                \
    X(core_wins,                  uint32_t,           0)                                            \
    X(core_losses,                uint32_t,           0)                                            \
    /* WARM cluster — partner pairing + gross accumulators (v4.7.21/26) */                          \
    X(partner_pending_pnl,        FPN<F>,             FPN_Zero<F>())                                \
    X(core_gross_wins,            FPN<F>,             FPN_Zero<F>())                                \
    X(core_gross_losses,          FPN<F>,             FPN_Zero<F>())                                \
    X(core_open_notional,         FPN<F>,             FPN_Zero<F>())                                \
    /* WARM cluster — per-core kill switch (Phase 3) */                                             \
    X(core_peak_balance,          FPN<F>,             FPN_Zero<F>())                                \
    X(core_dd_pct,                FPN<F>,             FPN_Zero<F>())                                \
    X(core_ks_trips_total,        uint32_t,           0)

//======================================================================================================
// [PAPER-RESET FIELD REGISTRY]
//======================================================================================================
// Tuple: same shape X(NAME, TYPE, INIT_VALUE) as init registry.
//
// STRICT SUBSET of init fields — only state that MUST reset between
// operator-initiated paper-reset boundaries (vs boot init which resets
// EVERYTHING). Membership decisions anchored in:
//   - Phase 2.1 (P&L + budget leak): core_realized, core_fees, core_wins,
//     core_losses, core_open_notional
//   - Phase 3 (kill switch leak): core_peak_balance, core_dd_pct,
//     core_ks_trips_total + KILL_TRIPPED bitmap clear (in AUTOPOPULATE)
//   - v4.7.21/26 (partner pairing + gross accumulator leak):
//     partner_pending_pnl, core_gross_wins, core_gross_losses +
//     partner_pending_bitmap clear (in AUTOPOPULATE)
//   - v5.4.3 (recurring-bugs Class 5): sl_cooldown_remaining, idle_cycles
//   - Counter reset: entries_processed, exits_processed
//
// EXCLUDED (load-bearing — do NOT reset):
//   - core (ExecutionCore* pointer; registration persists)
//   - model_handle / ensemble_handle (loaded models persist)
//   - confidence (ConfidenceScorer history; resetting would destroy
//     drift detection state)
//   - pnl_feeder (RegressionFeederX history; resetting would destroy
//     adaptive feedback signal)
//   - regime_state (hysteresis; resetting would cause regime thrash
//     post-reset until RegimeDetector recomputes)
//   - turnover, drift_history (similar accumulator-history concerns)
//
// Adding a new field to the reset subset: append ONE row. AUTOPOPULATE
// picks it up at next compile.
//======================================================================================================
#define FOREACH_CORE_CTX_RESET_FIELD(X)                                                             \
    /* Counter resets */                                                                            \
    X(entries_processed,          uint64_t,           0)                                            \
    X(exits_processed,            uint64_t,           0)                                            \
    /* Phase 2.1 — P&L + budget leak prevention */                                                  \
    X(core_realized,              FPN<F>,             FPN_Zero<F>())                                \
    X(core_fees,                  FPN<F>,             FPN_Zero<F>())                                \
    X(core_wins,                  uint32_t,           0)                                            \
    X(core_losses,                uint32_t,           0)                                            \
    X(core_open_notional,         FPN<F>,             FPN_Zero<F>())                                \
    /* Phase 3 — kill switch leak prevention */                                                     \
    X(core_peak_balance,          FPN<F>,             FPN_Zero<F>())                                \
    X(core_dd_pct,                FPN<F>,             FPN_Zero<F>())                                \
    X(core_ks_trips_total,        uint32_t,           0)                                            \
    /* v4.7.21/26 — partner pairing + gross accumulator leak prevention */                          \
    X(partner_pending_pnl,        FPN<F>,             FPN_Zero<F>())                                \
    X(core_gross_wins,            FPN<F>,             FPN_Zero<F>())                                \
    X(core_gross_losses,          FPN<F>,             FPN_Zero<F>())                                \
    /* v5.4.3 (recurring-bugs Class 5) — cooldown + idle-cycle leak prevention */                   \
    X(sl_cooldown_remaining,      uint32_t,           0)                                            \
    X(idle_cycles,                uint32_t,           0)

//======================================================================================================
// [TEMPLATED HELPERS — registry walk encapsulation]
//======================================================================================================
// Per CLAUDE.md item 23 — templated helper functions encapsulate the
// FOREACH walks. Each is instantiated once per F (only F=64 today);
// AUTOPOPULATE macros below delegate to these helpers.
//======================================================================================================

template <unsigned F>
inline void _core_ctx_init_value_fields(CoreContext<F>& ctx) {
#define X(NAME, TYPE, INIT_VAL) ctx.NAME = (TYPE)(INIT_VAL);
    FOREACH_CORE_CTX_INIT_FIELD(X)
#undef X
}

template <unsigned F>
inline void _core_ctx_reset_value_fields(CoreContext<F>& ctx) {
#define X(NAME, TYPE, INIT_VAL) ctx.NAME = (TYPE)(INIT_VAL);
    FOREACH_CORE_CTX_RESET_FIELD(X)
#undef X
}

// Allocator policy helper: InitArena_Global → fallback to `new`.
// Encapsulates the 2-branch dispatch so future allocator changes
// (e.g., NUMA-aware arenas, huge-page pool selection) update ONE site.
template <unsigned F>
inline void _alloc_and_init_slow_state(CoreContext<F>& ctx) {
    if (auto* arena = tt::InitArena_Global()) {
        void* mem = tt::InitArena_Alloc(arena,
                                         sizeof(CoreSlowState<F>),
                                         alignof(CoreSlowState<F>));
        ctx.slow_state = mem
            ? new (mem) CoreSlowState<F>()
            : new CoreSlowState<F>();
    } else {
        ctx.slow_state = new CoreSlowState<F>();
    }
    CoreSlowState_Init(ctx.slow_state);
}

//======================================================================================================
// [COMPILE-TIME COUNT SENTINELS]
//======================================================================================================
// Public counts via `>=` style (per /readiness Check 21) — useful for
// downstream consumers + sanity checks.
//======================================================================================================
#define _CORE_CTX_INIT_COUNT_ONE(NAME, TYPE, INIT_VAL) +1
constexpr int CORE_CTX_INIT_FIELD_COUNT =
    0 FOREACH_CORE_CTX_INIT_FIELD(_CORE_CTX_INIT_COUNT_ONE);
#undef _CORE_CTX_INIT_COUNT_ONE
static_assert(CORE_CTX_INIT_FIELD_COUNT >= 30,
              "FOREACH_CORE_CTX_INIT_FIELD must keep at least the v5.15.5.B.7 "
              "set (30+ fields). Removing entries needs explicit justification.");

#define _CORE_CTX_RESET_COUNT_ONE(NAME, TYPE, INIT_VAL) +1
constexpr int CORE_CTX_RESET_FIELD_COUNT =
    0 FOREACH_CORE_CTX_RESET_FIELD(_CORE_CTX_RESET_COUNT_ONE);
#undef _CORE_CTX_RESET_COUNT_ONE
static_assert(CORE_CTX_RESET_FIELD_COUNT >= 15,
              "FOREACH_CORE_CTX_RESET_FIELD must keep at least the v5.15.5.B.7 "
              "set (15 fields anchored by Phase 2.1/3 + v4.7.21/26 + v5.4.3).");

}  // namespace tt

//======================================================================================================
// [AUTOPOPULATE COMPANION MACROS — multi-target dispatch]
//======================================================================================================
// CORE_CTX_INIT_AUTOPOPULATE(state_ptr, i)
//   One-line per-slot boot init. Covers:
//     1. ~40 value-init fields via templated helper _core_ctx_init_value_fields
//     2. 6 helper-Init calls (sub-structs)
//     3. 4 sp_telemetry atomic stores
//     4. slow_state arena allocation + CoreSlowState_Init via _alloc_and_init_slow_state
//     5. CoreContextDisplayMeta_Init for the sibling display_meta[i] entry
//
// CORE_CTX_RESET_AUTOPOPULATE(state_ref, c)
//   One-line per-slot paper-reset. Covers:
//     1. ~15 reset-subset value-init via templated helper _core_ctx_reset_value_fields
//     2. Selective CORE_STATE_FLAG_CLR(KILL_TRIPPED) bitmap op
//     3. Engine-wide partner_pending_bitmap clear for core c
//
// Multi-target dispatch: same registry + helpers serve BOTH boot init
// AND paper reset. Future "per-session counter" additions touch ONE row
// (init registry) + ONE row (reset registry if appropriate) — no manual
// touch of the AUTOPOPULATE macro body unless adding a new helper-Init
// or atomic cluster.
//======================================================================================================

#define CORE_CTX_INIT_AUTOPOPULATE(_state_ptr, _i)                                                  \
    do {                                                                                            \
        auto* _autop_state = (_state_ptr);                                                          \
        int   _autop_idx   = (_i);                                                                  \
        auto& _autop_ctx   = _autop_state->cores[_autop_idx];                                       \
        /* Layer 1 — registry-driven value-init (~40 fields) */                                     \
        tt::_core_ctx_init_value_fields(_autop_ctx);                                                \
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
        /* Layer 4 — slow_state arena allocation + CoreSlowState_Init */                            \
        tt::_alloc_and_init_slow_state(_autop_ctx);                                                 \
        /* Layer 5 — sibling-struct init (CoreContextDisplayMeta on EventLoopState) */              \
        CoreContextDisplayMeta_Init(&_autop_state->display_meta[_autop_idx]);                       \
    } while (0)

#define CORE_CTX_RESET_AUTOPOPULATE(_state_ref, _c)                                                 \
    do {                                                                                            \
        auto& _autop_s   = (_state_ref);                                                            \
        int   _autop_c2  = (_c);                                                                    \
        auto& _autop_ctx = _autop_s.cores[_autop_c2];                                               \
        /* Layer 1 — registry-driven reset-subset value-init (~15 fields) */                        \
        tt::_core_ctx_reset_value_fields(_autop_ctx);                                               \
        /* Layer 2 — selective bitmap operations (Phase 3 KILL_TRIPPED + v5.14.9.G partner) */      \
        CORE_STATE_FLAG_CLR(_autop_ctx, KILL_TRIPPED);                                              \
        BITMAP_CLR(_autop_s.partner_pending_bitmap, BITMAP_BIT_U16(_autop_c2));                     \
    } while (0)

#endif  // CORE_CTX_INIT_REGISTRY_HPP
