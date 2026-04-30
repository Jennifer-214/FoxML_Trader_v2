// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [STRATEGY LIFECYCLE DISPATCHERS — v5.4.0 Phase 1.2]
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
#include "../CoreFrameworks/ControllerEventLoop.hpp"  // CoreContext, EventLoopState
#include "Momentum.hpp"
#include "MeanReversion.hpp"
#include "SimpleDip.hpp"
#include "MLStrategy.hpp"
#include "private/EmaCross.hpp"
#include "StrategyInterface.hpp"  // STRATEGY_* enum, BuySideGateConditions

namespace tt {

//======================================================================================================
// [INIT — allocate per-strategy state]
//======================================================================================================
// Allocates the right state struct on the heap, calls the strategy's
// `_Init` to populate initial values from rolling stats + cfg.
// Idempotent: if state already exists, free it first (used during
// hot-swap, where one strategy is replaced with another).
//
// `buy_conds_scratch` is a stack-local that the legacy `_Init` signature
// fills in. Sharded engine doesn't use BuySideGateConditions (it uses
// GateParameters via _BuildParameters); the scratch is just a sink so
// the signature works without modifying legacy code.
//======================================================================================================
template <unsigned F>
inline void Strategy_FreePerCore(EventLoopState<F>* state, int slot);

template <unsigned F, unsigned W = 128>
inline void Strategy_InitPerCore(EventLoopState<F>* state, int slot,
                                  uint8_t strategy_id,
                                  const RollingStats<F, W>* rolling,
                                  const ControllerConfig<F>* cfg) {
    if (slot < 0 || slot >= MAX_EXECUTION_CORES) return;
    auto& ctx = state->cores[slot];
    if (ctx.strategy_state) {
        Strategy_FreePerCore(state, slot);
    }

    // Stack-local scratch for the legacy _Init signature. Discarded after
    // the call — sharded engine reads gate parameters via _BuildParameters,
    // not via BuySideGateConditions.
    BuySideGateConditions<F> buy_conds_scratch{};

    switch (strategy_id) {
        case STRATEGY_MOMENTUM: {
            auto* s = new MomentumState<F>{};
            // v5.4.0 Phase 2.3 — seed adaptive filter values from cfg.
            // Mirrors the legacy PortfolioController init path; without
            // this seed Momentum_Adapt has nothing to ratchet against and
            // breakout_mult collapses to breakout_min.
            if (cfg) {
                // Prefer momentum_breakout_mult (per-strategy cfg field) when
                // set; otherwise fall back to a sensible default that the
                // squeeze + regression can adapt away from.
                FPN<F> seed_mult = !FPN_IsZero(cfg->momentum_breakout_mult)
                    ? cfg->momentum_breakout_mult
                    : FPN_FromDouble<F>(2.0);  // 2σ default
                s->live_breakout_mult = seed_mult;
                s->live_vol_mult      = cfg->volume_multiplier;
            }
            Momentum_Init(s, rolling, &buy_conds_scratch);
            ctx.strategy_state = s;
            break;
        }
        case STRATEGY_MEAN_REVERSION: {
            auto* s = new MeanReversionState<F>{};
            // v5.4.0 Phase 2.2 — seed adaptive filter values from cfg.
            // Legacy PortfolioController did this at line ~309 before
            // calling MR_Init; sharded missed it because MR_Init itself
            // doesn't write live_offset_pct/live_vol_mult/live_stddev_mult
            // (it reads them to pick mode + compute initial buy_conds).
            // Without this seed, MR_Adapt clamps to cfg.offset_min and
            // cfg.vol_mult_min — collapsing the adaptive range entirely.
            if (cfg) {
                s->live_offset_pct  = cfg->entry_offset_pct;
                s->live_vol_mult    = cfg->volume_multiplier;
                s->live_stddev_mult = cfg->offset_stddev_mult;  // 0 = pct mode, >0 = stddev mode
            }
            MeanReversion_Init(s, rolling, &buy_conds_scratch);
            ctx.strategy_state = s;
            break;
        }
        case STRATEGY_SIMPLE_DIP: {
            auto* s = new SimpleDipState<F>{};
            SimpleDip_Init(s, rolling, &buy_conds_scratch);
            ctx.strategy_state = s;
            break;
        }
        case STRATEGY_EMA_CROSS: {
            auto* s = new EmaCrossState<F>{};
            EmaCross_Init(s, rolling, &buy_conds_scratch);
            ctx.strategy_state = s;
            break;
        }
        case STRATEGY_ML: {
            auto* s = new MLStrategyState<F>{};
            MLStrategy_Init(s, rolling, &buy_conds_scratch);
            ctx.strategy_state = s;
            break;
        }
        case STRATEGY_AUTO:
        case STRATEGY_NONE:
        default:
            // AUTO / NONE / unknown: no per-strategy state.
            ctx.strategy_state = nullptr;
            break;
    }
    ctx.strategy_state_kind = strategy_id;

    (void)cfg;  // not currently consumed by any _Init; reserved for future strategies
}

//======================================================================================================
// [ADAPT — per-cadence dispatch]
//======================================================================================================
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
//======================================================================================================
template <unsigned F>
inline void Strategy_AdaptPerCore(
    EventLoopState<F>* state,
    int slot,
    uint8_t effective_strategy_id,
    FPN<F> current_price,
    FPN<F> portfolio_delta,
    uint16_t active_bitmap,
    const ControllerConfig<F>* cfg
) {
    if (slot < 0 || slot >= MAX_EXECUTION_CORES) return;
    auto& ctx = state->cores[slot];
    if (!ctx.strategy_state) return;  // no allocated state → no Adapt

    // Stack-local scratch — legacy _Adapt signature wants buy_conds, sharded
    // doesn't use it (parameters flow via GateParameters seqlock, not
    // BuySideGateConditions).
    BuySideGateConditions<F> buy_conds_scratch{};

    switch (effective_strategy_id) {
        case STRATEGY_SIMPLE_DIP:
            if (ctx.strategy_state_kind == STRATEGY_SIMPLE_DIP) {
                SimpleDip_Adapt(static_cast<SimpleDipState<F>*>(ctx.strategy_state),
                                current_price, portfolio_delta, active_bitmap,
                                &buy_conds_scratch, cfg);
            }
            break;
        case STRATEGY_MEAN_REVERSION:
            // v5.4.0 Phase 2.2 — only invoke when allocated state matches.
            // Mismatched kind (e.g. AUTO core whose effective_strategy_id
            // resolved to MR but state was allocated for the original AUTO
            // sentinel) leaves state untouched; Phase 3 will handle AUTO
            // re-allocation on regime transitions.
            if (ctx.strategy_state_kind == STRATEGY_MEAN_REVERSION) {
                MeanReversion_Adapt(static_cast<MeanReversionState<F>*>(ctx.strategy_state),
                                     current_price, portfolio_delta, active_bitmap,
                                     &buy_conds_scratch, cfg);
            }
            break;
        case STRATEGY_MOMENTUM:
            // v5.4.0 Phase 2.3 — same kind-match guard as MR.
            if (ctx.strategy_state_kind == STRATEGY_MOMENTUM) {
                Momentum_Adapt(static_cast<MomentumState<F>*>(ctx.strategy_state),
                                current_price, portfolio_delta, active_bitmap,
                                &buy_conds_scratch, cfg);
            }
            break;
        // Phase 2.4-2.5 will add EmaCross / ML cases here.
        default:
            break;
    }
}

//======================================================================================================
// [WRITE RATCHET SL — shared helper, fee-floor capped]
//======================================================================================================
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
//======================================================================================================
template <unsigned F>
inline bool Strategy_WriteRatchetSL(EventLoopState<F>* state, int slot,
                                     FPN<F> proposed_sl, FPN<F> entry_price,
                                     const ControllerConfig<F>* cfg) {
    if (slot < 0 || slot >= MAX_EXECUTION_CORES) return false;
    if (FPN_IsZero(entry_price)) return false;

    // Cap proposal at entry × (1 - 3 × fee_rate_taker). Same formula as the
    // generic ratcheter (ControllerEventLoop.hpp:2244 v5.1.7 commentary).
    FPN<F> fee_taker = !FPN_IsZero(cfg->fee_rate_taker)
        ? cfg->fee_rate_taker : cfg->fee_rate;
    FPN<F> three     = FPN_FromDouble<F>(3.0);
    FPN<F> floor_pct = FPN_Mul(fee_taker, three);
    FPN<F> floor_mult = FPN_Sub(FPN_FromDouble<F>(1.0), floor_pct);
    FPN<F> sl_floor  = FPN_Mul(entry_price, floor_mult);
    FPN<F> capped    = FPN_LessThan(proposed_sl, sl_floor) ? proposed_sl : sl_floor;

    auto& ctx = state->cores[slot];
    if (FPN_GreaterThan(capped, ctx.pending_params.ratchet_sl)) {
        ctx.pending_params.ratchet_sl = capped;
        ctx.dirty = 1;
        return true;
    }
    return false;
}

//======================================================================================================
// [EXIT ADJUST — per-cadence dispatch]
//======================================================================================================
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
//======================================================================================================
template <unsigned F, unsigned W>
inline void Strategy_ExitAdjustPerCore(
    EventLoopState<F>* state,
    int slot,
    uint8_t effective_strategy_id,
    FPN<F> current_price,
    const RollingStats<F, W>* rolling,
    const ControllerConfig<F>* cfg
) {
    if (slot < 0 || slot >= MAX_EXECUTION_CORES) return;
    auto& ctx = state->cores[slot];
    if (!ctx.strategy_state) return;

    switch (effective_strategy_id) {
        case STRATEGY_MEAN_REVERSION:
            if (ctx.strategy_state_kind == STRATEGY_MEAN_REVERSION) {
                MeanReversion_ExitAdjustSharded(state, slot,
                    static_cast<MeanReversionState<F>*>(ctx.strategy_state),
                    current_price, rolling, cfg);
            }
            break;
        case STRATEGY_MOMENTUM:
            if (ctx.strategy_state_kind == STRATEGY_MOMENTUM) {
                Momentum_ExitAdjustSharded(state, slot,
                    static_cast<MomentumState<F>*>(ctx.strategy_state),
                    current_price, rolling, cfg);
            }
            break;
        // Phase 2.4 will add EmaCross.
        default:
            break;
    }
}

//======================================================================================================
// [FREE — destroy per-strategy state]
//======================================================================================================
// Idempotent: safe to call on slots where strategy_state == nullptr.
// Dispatches by strategy_state_kind to call the right `delete` (void*
// can't be deleted directly).
//======================================================================================================
template <unsigned F>
inline void Strategy_FreePerCore(EventLoopState<F>* state, int slot) {
    if (slot < 0 || slot >= MAX_EXECUTION_CORES) return;
    auto& ctx = state->cores[slot];
    if (!ctx.strategy_state) {
        ctx.strategy_state_kind = 0xFF;
        return;
    }
    switch (ctx.strategy_state_kind) {
        case STRATEGY_MOMENTUM:
            delete static_cast<MomentumState<F>*>(ctx.strategy_state);
            break;
        case STRATEGY_MEAN_REVERSION:
            delete static_cast<MeanReversionState<F>*>(ctx.strategy_state);
            break;
        case STRATEGY_SIMPLE_DIP:
            delete static_cast<SimpleDipState<F>*>(ctx.strategy_state);
            break;
        case STRATEGY_EMA_CROSS:
            delete static_cast<EmaCrossState<F>*>(ctx.strategy_state);
            break;
        case STRATEGY_ML:
            delete static_cast<MLStrategyState<F>*>(ctx.strategy_state);
            break;
        default:
            // Unknown kind: leak rather than miscast. 0xFF (uninitialized)
            // means strategy_state should already be nullptr — handled
            // above. This default catches genuinely unknown values which
            // should never occur in practice.
            fprintf(stderr,
                "[strategy-lifecycle] Strategy_FreePerCore: unknown kind %u "
                "on slot %d, leaking the state pointer rather than miscasting\n",
                ctx.strategy_state_kind, slot);
            break;
    }
    ctx.strategy_state = nullptr;
    ctx.strategy_state_kind = 0xFF;
}

} // namespace tt

#endif // STRATEGY_LIFECYCLE_HPP
