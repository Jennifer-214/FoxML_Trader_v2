// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[Strategies/StrategyLifecycle.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [FRAMEWORK_DISCIPLINE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the lifecycle dispatchers (v5.4.0) — Init/Adapt/ExitAdjust/Free by strategy_id via the FOREACH_STRATEGY registry; plus the shared ratchet write helpers (SL fee-floor capped)]
// [CONTAINS]
//   - [FUNCTION]_[Strategy_InitPerCore]      (+ SeedFromCfg overloads)
//   - [FUNCTION]_[Strategy_AdaptPerCore]
//   - [FUNCTION]_[Strategy_WriteRatchetSL]   (+ WriteRatchetTP twin)
//   - [FUNCTION]_[Strategy_ExitAdjustPerCore]
//   - [FUNCTION]_[Strategy_FreePerCore]
//======================================================================================================
// Dispatches Strategy_<Action> calls by strategy_id. Wraps the per-strategy
// `_Init`, `_Adapt`, `_ExitAdjust`, etc. so the engine's slow-path
// (per-core thread or producer in centralized) can drive the lifecycle
// without knowing strategy-specific types.
//
// Why a separate file: the dispatchers must include every strategy header
// (Momentum.hpp, MeanReversion.hpp, SimpleDip.hpp, EmaCross.hpp,
// MLStrategy.hpp) to instantiate state structs. Putting this in
// StrategyParameters.hpp would couple the dispatch surface to the
// build-parameters surface; better to keep them separate.
//
// Lifecycle stages (per DOCS/STRATEGY_INTERFACE.md):
//   1. Init           — at boot per-core. Allocate state.
//   2. Adapt          — per-cadence. Update state from market.
//   3. BuildParameters — per-cadence. Read state → emit GateParameters.
//                        (Lives in StrategyParameters.hpp, not here.)
//   4. ExitAdjust     — per-cadence on open positions. Write ratchet_sl.
//   5. RegimeAdjust   — on regime transition. Retune positions.
//
// This file ships Stages 1-4 (Init, Adapt, ExitAdjust, plus FreePerCore
// for symmetric cleanup). Stage 5 (RegimeAdjust) is wired in Phase 3.
//
// Pre-v5.4 status: Stages 1-2 + 4-5 were ORPHANED in the sharded path.
// See DOCS/v5.4-regression-postmortem.md F7-F10. This file's introduction
// is the foundation for restoring them across all strategies.
//======================================================================================================
#ifndef STRATEGY_LIFECYCLE_HPP
#define STRATEGY_LIFECYCLE_HPP

#include "../FixedPoint/FixedPointN.hpp"
#include "../ML_Headers/RollingStats.hpp"
#include "../CoreFrameworks/ControllerConfig.hpp"
#include "../CoreFrameworks/ControllerEventLoop.hpp"  // NodeContext, EventLoopState
#include "Momentum.hpp"
#include "MeanReversion.hpp"
#include "SimpleDip.hpp"
#include "MLStrategy.hpp"
// v5.8.0 Phase 0: conditional include — see StrategyParameters.hpp comment.
#if __has_include("private/EmaCross.hpp")
#  include "private/EmaCross.hpp"
#endif
#include "StrategyInterface.hpp"  // STRATEGY_* enum, BuySideGateConditions

namespace tt {

//------------------------------------------------------------------------------
// [SECTION]_[SEED FROM CFG — overload-resolution per state type]
//------------------------------------------------------------------------------
// The X-macro Init dispatcher allocates state generically + calls _Init
// uniformly. Strategies that need cfg-derived seeding (MR, Momentum)
// override this template via overload. Default = no-op.
//
// Why overload not specialization: function-template specialization +
// template parameter inference is fragile across compilers. Overloading
// on the state-type pointer is unambiguous and selects the right
// override at the X-macro call site.
//======================================================================================================
template <typename StateT, unsigned F>
inline void Strategy_SeedFromCfg(StateT* s, const ControllerConfig<F>* cfg) {
    (void)s; (void)cfg;  // default: no seed
}

template <unsigned F>
inline void Strategy_SeedFromCfg(MomentumState<F>* s, const ControllerConfig<F>* cfg) {
    // v5.4.0 Phase 2.3 — seed adaptive filter values from cfg.
    // Mirrors the legacy PortfolioController init path; without this
    // seed Momentum_Adapt has nothing to ratchet against and
    // breakout_mult collapses to breakout_min.
    if (!cfg) return;
    FPN_Binary<F> seed_mult = !FPN_IsZero(cfg->momentum_breakout_mult)
        ? cfg->momentum_breakout_mult
        : FPN_FromDouble<F>(2.0);  // 2σ default
    s->live_breakout_mult = seed_mult;
    s->live_vol_mult      = cfg->volume_multiplier;
}

template <unsigned F>
inline void Strategy_SeedFromCfg(MeanReversionState<F>* s, const ControllerConfig<F>* cfg) {
    // v5.4.0 Phase 2.2 — seed adaptive filter values from cfg.
    // Without this seed, MR_Adapt clamps to cfg.offset_min /
    // cfg.vol_mult_min — collapsing the adaptive range entirely.
    if (!cfg) return;
    s->live_offset_pct  = Money_ToBinary(cfg->entry_offset_pct);  // strategy-internal feature copy
    s->live_vol_mult    = cfg->volume_multiplier;
    s->live_stddev_mult = cfg->offset_stddev_mult;  // 0 = pct mode, >0 = stddev mode
}

//======================================================================
// [FUNCTION]_[Strategy_InitPerCore]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BOOT_TIME] [SLOW_PATH]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[allocate + seed per-node strategy state by strategy_id (X-macro dispatch); sets strategy_state_kind — the ONLY site that does (with FreePerCore)]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline void Strategy_FreePerCore(EventLoopState<F>* state, int slot);

template <unsigned F, unsigned W = 128>
inline void Strategy_InitPerCore(EventLoopState<F>* state, int slot,
                                  uint8_t strategy_id,
                                  const RollingStats<F, W>* rolling,
                                  const ControllerConfig<F>* cfg) {
    if (slot < 0 || slot >= MAX_EXECUTION_NODES) return;
    auto& ctx = state->nodes[slot];
    if (ctx.strategy_state) {
        Strategy_FreePerCore(state, slot);
    }

    // Stack-local scratch for the legacy _Init signature. Discarded after
    // the call — sharded engine reads gate parameters via _BuildParameters,
    // not via BuySideGateConditions.
    BuySideGateConditions<F> buy_conds_scratch{};

    // v5.11.6.A — InitArena-backed strategy state allocation when the
    // engine has set the global arena (production path); placement-new
    // on the arena slot. Tests + non-engine consumers fall back to
    // standard `new`.
    switch (strategy_id) {
#define X(id, short_name, full_name, state_t, init_fn, build_fn, adapt_fn, exit_fn) \
        case STRATEGY_##id: { \
            state_t<F>* s; \
            if (auto* arena = tt::InitArena_Global()) { \
                void* mem = tt::InitArena_Alloc(arena, sizeof(state_t<F>), \
                                                 alignof(state_t<F>)); \
                s = mem ? new (mem) state_t<F>{} : new state_t<F>{}; \
            } else { \
                s = new state_t<F>{}; \
            } \
            Strategy_SeedFromCfg(s, cfg); \
            init_fn(s, rolling, &buy_conds_scratch); \
            ctx.strategy_state = s; \
            break; \
        }
        FOREACH_STRATEGY(X)
#undef X
        case STRATEGY_AUTO:
        case STRATEGY_NONE:
        default:
            // AUTO / NONE / unknown: no per-strategy state.
            ctx.strategy_state = nullptr;
            break;
    }
    ctx.strategy_state_kind = strategy_id;
}

//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Allocates the right state struct on the heap, calls the strategy's
// `_Init` to populate initial values from rolling stats + cfg.
// Idempotent: if state already exists, free it first (used during
// hot-swap, where one strategy is replaced with another).
//
// `buy_conds_scratch` is a stack-local that the legacy `_Init` signature
// fills in. Sharded engine doesn't use BuySideGateConditions (it uses
// GateParameters via _BuildParameters); the scratch is just a sink so
// the signature works without modifying legacy code.
//
// v5.8.0: switch dispatch generated from FOREACH_STRATEGY(X). Each row
// emits one case that allocates the registered state type, runs the
// optional cfg seed, then calls the registered _Init function.
//======================================================================
// [END_FUNCTION]_[Strategy_InitPerCore]
//======================================================================

//======================================================================
// [FUNCTION]_[Strategy_AdaptPerCore]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[per-cadence Adapt dispatch by effective strategy_id (X-macro); restores the F7-F10 orphaned lifecycle on the sharded path]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline void Strategy_AdaptPerCore(
    EventLoopState<F>* state,
    int slot,
    uint8_t effective_strategy_id,
    FPN_Binary<F> current_price,
    FPN_Binary<F> portfolio_delta,
    uint16_t active_bitmap,
    const ControllerConfig<F>* cfg,
    const FPN_Binary<F>* ema_price = nullptr   // v5.4.0 Phase 2.4 — EmaCross uses this; others ignore
) {
    if (slot < 0 || slot >= MAX_EXECUTION_NODES) return;
    auto& ctx = state->nodes[slot];
    if (!ctx.strategy_state) return;  // no allocated state → no Adapt

    // Stack-local scratch — legacy _Adapt signature wants buy_conds, sharded
    // doesn't use it (parameters flow via GateParameters seqlock, not
    // BuySideGateConditions).
    BuySideGateConditions<F> buy_conds_scratch{};

    // v5.8.0: dispatch via FOREACH_STRATEGY(X). Each strategy's _Adapt
    // is invoked with the canonical signature (cast inside the case to
    // the registered state type). EmaCross has special pre-tracking
    // (last_ema_slope + prev_ema seeded from ema_price) handled
    // separately below — outside the X-macro because the canonical
    // _Adapt signature doesn't include ema_price. Per the v5.8 audit:
    // 4 of 5 strategies match canonical sig; EmaCross is the outlier.
    //
    // Kind-match guard: only invoke when allocated state matches the
    // effective strategy. Mismatched kind (e.g. AUTO core whose
    // effective_strategy_id resolved to MR but state was allocated for
    // the original AUTO sentinel) leaves state untouched. Phase 3 will
    // handle AUTO re-allocation on regime transitions.
    switch (effective_strategy_id) {
#define X(id, short_name, full_name, state_t, init_fn, build_fn, adapt_fn, exit_fn) \
        case STRATEGY_##id: \
            if (ctx.strategy_state_kind == STRATEGY_##id) { \
                adapt_fn(static_cast<state_t<F>*>(ctx.strategy_state), \
                          current_price, portfolio_delta, active_bitmap, \
                          &buy_conds_scratch, cfg); \
            } \
            break;
        FOREACH_STRATEGY(X)
#undef X
        default:
            break;
    }

    // v5.4.0 Phase 2.4 — EmaCross post-pass: update the state's EMA
    // tracking (last_ema_slope + prev_ema) from the current ema_price.
    // EmaCross_BuildParameters reads state->prev_ema for the crossover
    // reference; last_ema_slope drives ExitAdjust. This update lives
    // outside the X-macro because the canonical _Adapt sig doesn't
    // include ema_price (4 of 5 strategies don't need it). When ema_price
    // is null (cold start), skip the update — prev_ema stays at its
    // default zero and BuildParameters falls back to rolling avg.
#if __has_include("private/EmaCross.hpp")
    if (effective_strategy_id == STRATEGY_EMA_CROSS &&
        ctx.strategy_state_kind == STRATEGY_EMA_CROSS && ema_price) {
        auto* es = static_cast<EmaCrossState<F>*>(ctx.strategy_state);
        es->last_ema_slope = FPN_Sub(*ema_price, es->prev_ema);
        es->prev_ema       = *ema_price;
    }
#endif
}

//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Called from EventLoop_RebuildOneCore each slow-path cadence, BEFORE
// Strategy_BuildParameters. Updates per-strategy adaptive state from the
// latest market observations. Pure no-op when state is null (e.g. AUTO
// cores pre-Phase 3, STRATEGY_NONE cores) — preserves prior behavior.
//
// Phase 2.1 (SimpleDip): SimpleDip_Adapt is itself a no-op; the actual
// state update (recent_high) happens inside SimpleDip_BuildParameters
// where rolling is in scope. Wiring exists for symmetry with other
// strategies and to establish the dispatcher pattern Phase 2.2-2.5
// will fill in.
//======================================================================
// [END_FUNCTION]_[Strategy_AdaptPerCore]
//======================================================================

//======================================================================
// [FUNCTION]_[Strategy_WriteRatchetSL]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [CAPITAL_BEARING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[THE ratchet-SL chokepoint — fee-floor cap (entry x (1 - 3x per-node fee_taker)) uniform across all 5 callers; max-only + DIRTY on advance; WriteRatchetTP (no floor needed) shares the section]
// [REFERENCE]_[CLASS]_[26]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline bool Strategy_WriteRatchetSL(EventLoopState<F>* state, int slot,
                                     Money proposed_sl, Money entry_price,
                                     const ControllerConfig<F>* cfg) {
    if (slot < 0 || slot >= MAX_EXECUTION_NODES) return false;
    if (Money_IsZero(entry_price)) return false;

    // Cap proposal at entry × (1 - 3 × fee_rate_taker). Same formula as the
    // generic ratcheter (EventLoop_TrailingSLRatchetOneCore's v5.1.7 fee-floor).
    // v5.15.5.F.4d.1.B.8 — Class 26 sub-shape B fix: per-core fee_rate_taker
    // (UNINDEXED-GLOBAL closure). 5 callers (MeanReversion + MLStrategy +
    // EmaCross + Momentum + ControllerEventLoop) all pass per-core slot.
    Money fee_taker = !Money_IsZero(cfg->nodes[slot].fee_rate_taker)
        ? cfg->nodes[slot].fee_rate_taker : cfg->nodes[slot].fee_rate;
    Money three     = Money_FromInt(3);
    Money floor_pct = Money_Mul(fee_taker, three);
    Money floor_mult = Money_Sub(Money_FromInt(1), floor_pct);
    Money sl_floor  = Money_Mul(entry_price, floor_mult);
    Money capped    = Money_Lt(proposed_sl, sl_floor) ? proposed_sl : sl_floor;

    auto& ctx = state->nodes[slot];
    if (Money_Gt(capped, ctx.pending_params.ratchet_sl)) {
        ctx.pending_params.ratchet_sl = capped;
        NODE_STATE_FLAG_SET(ctx, DIRTY);
        return true;
    }
    return false;
}

//------------------------------------------------------------------------------
// [SECTION]_[WRITE RATCHET TP — shared helper, no fee-floor]
//------------------------------------------------------------------------------
// Companion to Strategy_WriteRatchetSL, parallel channel for trailing TP.
// LONG geometry: TP ratchets UP (FPN_Max wins) → locks in higher exit
// targets. No fee-floor cap is needed: a higher TP can never produce a
// net-negative exit. Used by Regime_AdjustPositions on RANGING→TRENDING
// transitions and by future strategy-specific TP trailing.
//
// Returns true iff the write actually advanced ratchet_tp.
//------------------------------------------------------------------------------
template <unsigned F>
inline bool Strategy_WriteRatchetTP(EventLoopState<F>* state, int slot,
                                     Money proposed_tp) {
    if (slot < 0 || slot >= MAX_EXECUTION_NODES) return false;
    if (Money_IsZero(proposed_tp)) return false;

    auto& ctx = state->nodes[slot];
    if (Money_Gt(proposed_tp, ctx.pending_params.ratchet_tp)) {
        ctx.pending_params.ratchet_tp = proposed_tp;
        NODE_STATE_FLAG_SET(ctx, DIRTY);
        return true;
    }
    return false;
}

//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Strategies that ratchet SL upward (MR, Momentum, EmaCross trailing logic)
// route writes through this helper so the fee-floor cap is uniform: the
// ratchet can never push effective_sl above entry × (1 - 3 × fee_rate_taker).
// Without the cap, an over-tight ratchet would exit on the first tiny pullback
// at near-breakeven gross which becomes net-negative after round-trip fees.
//
// Mirrors the cap that EventLoop_TrailingSLRatchetOneCore (the generic
// ratcheter, v5.1.7) applies. Both can run concurrently and write to the
// same field — the higher value wins via FPN_GreaterThan check.
//
// `entry_price` is the position entry — needed to compute the fee floor.
// Caller passes the relevant slot's entry_price (under partials, leg A and
// leg B share the same entry).
//
// Returns true iff the write actually advanced ratchet_sl. Side effect:
// dirty=1 set on advance (so seqlock pushes next cycle).
//======================================================================
// [END_FUNCTION]_[Strategy_WriteRatchetSL]
//======================================================================

//======================================================================
// [FUNCTION]_[Strategy_ExitAdjustPerCore]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [CAPITAL_BEARING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[per-cadence ExitAdjust dispatch (only for nodes with open slots) — strategy-specific ratchets route through WriteRatchetSL; higher value wins vs the generic ratcheter]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F, unsigned W>
inline void Strategy_ExitAdjustPerCore(
    EventLoopState<F>* state,
    int slot,
    uint8_t effective_strategy_id,
    Money current_price,
    const RollingStats<F, W>* rolling,
    const ControllerConfig<F>* cfg
) {
    if (slot < 0 || slot >= MAX_EXECUTION_NODES) return;
    auto& ctx = state->nodes[slot];
    if (!ctx.strategy_state) return;

    // v5.8.0: dispatch via FOREACH_STRATEGY(X). SimpleDip's
    // _ExitAdjustSharded is a no-op stub (added in v5.8.0 for X-macro
    // signature uniformity); the call still happens but does nothing.
    switch (effective_strategy_id) {
#define X(id, short_name, full_name, state_t, init_fn, build_fn, adapt_fn, exit_fn) \
        case STRATEGY_##id: \
            if (ctx.strategy_state_kind == STRATEGY_##id) { \
                exit_fn(state, slot, \
                         static_cast<state_t<F>*>(ctx.strategy_state), \
                         current_price, rolling, cfg); \
            } \
            break;
        FOREACH_STRATEGY(X)
#undef X
        default:
            break;
    }
}

//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Called from EventLoop_RebuildOneCore each slow-path cadence, AFTER
// Strategy_BuildParameters, ONLY for cores with at least one open slot.
// Dispatches to per-strategy logic that writes ratchet_sl via the shared
// helper above.
//
// Pre-Phase 2.2 the strategy-specific exit-adjust logic was orphaned (per
// postmortem F8); the generic EventLoop_TrailingSLRatchetOneCore was the
// only ratchet path. With the dispatcher wired, both run — the higher
// ratchet_sl wins.
//
// `current_price` is the rolling price_avg (slow-path doesn't see live tick).
// Strategies that need a regression-based confidence gate read from their
// own state (which Adapt populated this cycle).
//======================================================================
// [END_FUNCTION]_[Strategy_ExitAdjustPerCore]
//======================================================================

//======================================================================
// [FUNCTION]_[Strategy_FreePerCore]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[kind-dispatched delete + kind=0xFF reset — the teardown half of the strategy_state_kind invariant]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline void Strategy_FreePerCore(EventLoopState<F>* state, int slot) {
    if (slot < 0 || slot >= MAX_EXECUTION_NODES) return;
    auto& ctx = state->nodes[slot];
    if (!ctx.strategy_state) {
        ctx.strategy_state_kind = 0xFF;
        return;
    }
    // v5.8.0: dispatch via FOREACH_STRATEGY(X). Each row generates one
    // case that deletes the registered state type. Unknown kinds leak
    // (defensive) — see default branch.
    //
    // v5.11.6.A — when arena-allocated, skip `delete` (arena owns the
    // memory; freed by InitArena_Destroy at engine shutdown). For the
    // arena path, also skip the destructor call since strategy state
    // structs are trivially destructible (POD-only fields verified by
    // construction at v5.4.0 strategy spec).
    switch (ctx.strategy_state_kind) {
#define X(id, short_name, full_name, state_t, init_fn, build_fn, adapt_fn, exit_fn) \
        case STRATEGY_##id: \
            if (!tt::InitArena_Owns(tt::InitArena_Global(), ctx.strategy_state)) { \
                delete static_cast<state_t<F>*>(ctx.strategy_state); \
            } \
            break;
        FOREACH_STRATEGY(X)
#undef X
        case STRATEGY_AUTO:
        case STRATEGY_NONE:
            // v5.11.11 (2026-05-07): symptom-quieted the WARN that this
            // branch used to fire (it lived in `default:` pre-v5.11.11).
            // v5.11.15 (2026-05-07): root cause found and fixed at
            // `CoreFrameworks/ShardedSnapshotPersist.hpp:498` — snapshot
            // Load was restoring `ctx.strategy_state_kind` from a
            // previous run's persisted byte, overwriting the kind that
            // Strategy_InitPerCore had just set correctly from
            // cfg.strategy_id. With that restore removed, the kind
            // invariant ("kind describes the C++ type of the
            // strategy_state pointer") holds and this branch should be
            // unreachable in normal operation.
            //
            // Kept defensively for two reasons: (1) tests can still
            // exercise unusual lifecycle orderings, (2) future paths
            // (hot-swap, AUTO regime re-allocation per Phase 3) could
            // re-introduce the mismatch and we'd rather null-out than
            // crash. Without knowing the concrete type, we cannot
            // safely delete (type-cast UB). Null the pointer — the
            // arena owns the memory (v5.11.6.A) so reclamation happens
            // at engine shutdown via InitArena_Destroy.
            break;
        default:
            // Genuinely unknown kind value (corrupted state). Worth
            // a WARN since this should never happen.
            fprintf(stderr,
                "[strategy-lifecycle] Strategy_FreePerCore: unknown kind %u "
                "on slot %d, leaking the state pointer rather than miscasting\n",
                ctx.strategy_state_kind, slot);
            break;
    }
    ctx.strategy_state = nullptr;
    ctx.strategy_state_kind = 0xFF;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Idempotent: safe to call on slots where strategy_state == nullptr.
// Dispatches by strategy_state_kind to call the right `delete` (void*
// can't be deleted directly).
//======================================================================
// [END_FUNCTION]_[Strategy_FreePerCore]
//======================================================================

} // namespace tt

#endif // STRATEGY_LIFECYCLE_HPP
