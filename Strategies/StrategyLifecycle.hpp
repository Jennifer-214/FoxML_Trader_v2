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
            Momentum_Init(s, rolling, &buy_conds_scratch);
            ctx.strategy_state = s;
            break;
        }
        case STRATEGY_MEAN_REVERSION: {
            auto* s = new MeanReversionState<F>{};
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
