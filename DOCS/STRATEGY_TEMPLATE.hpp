// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [<Name> STRATEGY — TEMPLATE]
//======================================================================================================
//
// This is a copy-paste template for a new public-tier strategy.
// Replace <Name> / <name> / <NAME> throughout, fill in your logic.
//
// HOW TO USE:
//   1. cp DOCS/STRATEGY_TEMPLATE.hpp Strategies/<Name>.hpp
//      (or Strategies/private/<Name>.hpp for alpha-flavored)
//   2. Replace <Name> token everywhere in the new file
//   3. Implement the 4 lifecycle functions (Init / Adapt / BuildParameters / ExitAdjustSharded)
//   4. Append the X-macro line in Strategies/StrategyInterface.hpp:
//        X(<NAME>, "<short>", "<full>", <Name>State, \
//          <Name>_Init, <Name>_BuildParameters, \
//          <Name>_Adapt, <Name>_ExitAdjustSharded)
//   5. Append the strategy color in GUI/DashboardPanels.hpp's strat_colors[]
//   6. ./build.sh test   — should compile + dispatch correctly
//
// CONVENTIONS:
//   - <Name> is PascalCase: MeanReversion, Momentum, etc.
//   - <name> is lowercase: mean_reversion, momentum
//   - <NAME> is upper snake: MEAN_REVERSION, MOMENTUM
//   - File goes in Strategies/<Name>.hpp (public) or Strategies/private/<Name>.hpp
//
// CANONICAL SIGNATURES (do not deviate):
//   See DOCS/STRATEGY_INTERFACE.md and DOCS/EASY_ADDITIONS_INVARIANTS.md
//   for the full contract and the X-macro variant-correctness rules.
//
//======================================================================================================
#pragma once

#include "../CoreFrameworks/OrderGates.hpp"
#include "../ML_Headers/RollingStats.hpp"
#include "../CoreFrameworks/ControllerConfig.hpp"
#include "StrategyInterface.hpp"

//======================================================================================================
// [STATE]
//======================================================================================================
// Per-core state. Allocated once per registered core during _InitPerCore.
// Hold any mutable state your Adapt + BuildParameters logic needs across
// slow-path cycles. Hot path NEVER reads this struct — it's slow-path-only.
//
// Replace fields with what your strategy needs. Common patterns:
//   FPN<F> live_offset_pct;     // adaptive offset that Adapt mutates
//   FPN<F> live_vol_mult;       // adaptive volume multiplier
//   FPN<F> last_signal;         // for hysteresis / debounce
//   int    initialized;         // 1 after _Init has run
//======================================================================================================
template <unsigned F> struct <Name>State {
    int    initialized;
    // ... your fields here ...
};

//======================================================================================================
// [INIT]
//======================================================================================================
// Called once per core after warmup completes. Set initial state from rolling
// stats. Do NOT touch buy_conds — leave that to BuildParameters.
//======================================================================================================
template <unsigned F>
inline void <Name>_Init(<Name>State<F> *state,
                         const RollingStats<F> *rolling,
                         BuySideGateConditions<F> *buy_conds) {
    state->initialized = 1;
    // ... initialize your state from rolling stats ...
    (void)buy_conds;
}

//======================================================================================================
// [ADAPT]
//======================================================================================================
// Called every slow-path cycle. Update adaptive state based on current price,
// portfolio P&L delta, and config. Return nothing — mutations land in `state`.
//
// If your strategy has no adaptive behavior (e.g. SimpleDip is purely cfg-
// driven), make this a no-op and document the reason. Don't delete the
// function — the X-macro registry expects it. Use NULL in the X-macro line
// if you want to skip the call entirely (matches SimpleDip's pattern).
//======================================================================================================
template <unsigned F>
inline void <Name>_Adapt(<Name>State<F> *state,
                          FPN<F> current_price,
                          FPN<F> portfolio_delta,
                          uint16_t active_bitmap,
                          const BuySideGateConditions<F> *buy_conds,
                          const ControllerConfig<F> *cfg) {
    (void)state; (void)current_price; (void)portfolio_delta;
    (void)active_bitmap; (void)buy_conds; (void)cfg;
    // ... your adaptive logic here ...
    // Common patterns:
    //   - if portfolio_delta > 0: tighten filters (we're winning, get hungry)
    //   - if portfolio_delta < 0: widen filters (we're losing, be picky)
    //   - update regression-driven offsets
    //   - apply idle squeeze if no entries fired in N cycles
}

//======================================================================================================
// [BUILD PARAMETERS — sharded]
//======================================================================================================
// THE CONTRACT WITH THE HOT PATH. Called per slow-path cycle. Emits the
// GateParameters pack that the hot path's BG_Evaluate / SG_Evaluate consume.
// Pure function of inputs — no globals, no statics, no Portfolio reads.
// The dispatcher in StrategyParameters.hpp wires this in via the X-macro.
//
// MUST set on `out`:
//   - bg_price_threshold (entry price)
//   - bg_volume_threshold (volume gate)
//   - sg_take_profit_price + sg_stop_loss_price (exit gates)
//   - tp_pct (used by post-cap fee-floor check)
//   - sl_pct
//   - trade_size (FPN — qty in base asset)
//   - flags (TP_ENABLED | SL_ENABLED at minimum; BUY_ABOVE for momentum)
//   - strategy_id = STRATEGY_<NAME>
//
// Should set if any reason to halt:
//   - Gate_Zero(out, false) zeros the threshold
//   - Or set BUY_BLOCKED flag for marginal-but-not-zero gates
//   - The dispatcher post-pass writes SHALT_NO_SIGNAL for unset reasons
//======================================================================================================
template <unsigned F, unsigned W = 128>
inline void <Name>_BuildParameters(
    const RollingStats<F, W> *rolling,
    const ControllerConfig<F> *config,
    FPN<F> allocated_balance,
    GateParameters<F> *out,
    <Name>State<F> *state = nullptr)
{
    (void)state;  // remove if you use state

    // Compute entry price, TP, SL from rolling + config.
    // ... your BuildParameters logic here ...

    out->bg_price_threshold   = /* entry price */;
    out->bg_volume_threshold  = FPN_Mul(rolling->volume_avg, config->volume_multiplier);
    out->sg_take_profit_price = /* TP price */;
    out->sg_stop_loss_price   = /* SL price */;
    out->tp_pct               = config->take_profit_pct;
    out->sl_pct               = config->stop_loss_pct;
    out->trade_size           = /* qty: FPN_DivNoAssert(allocated_balance, entry_price) */;
    out->flags                = GATE_FLAG_TP_ENABLED | GATE_FLAG_SL_ENABLED;
    out->strategy_id          = STRATEGY_<NAME>;
}

//======================================================================================================
// [EXIT ADJUST — sharded]
//======================================================================================================
// Called per slow-path cycle for active positions on this core. Trail SL
// and/or TP via Strategy_WriteRatchetSL / _WriteRatchetTP helpers (which
// apply the v5.1.7 fee-floor cap automatically).
//
// Don't write pos->stop_loss_price / pos->take_profit_price directly —
// the hot path doesn't read them. Use the ratchet helpers.
//
// Per-leg under partials: caller (StrategyLifecycle) hands you the leg slot
// directly; iterate active positions yourself if you need to.
//======================================================================================================
namespace tt {
template <unsigned F> struct EventLoopState;
template <unsigned F>
bool Strategy_WriteRatchetSL(EventLoopState<F>* state, int slot,
                              FPN<F> proposed_sl, FPN<F> entry_price,
                              const ControllerConfig<F>* cfg);

template <unsigned F, unsigned W>
inline void <Name>_ExitAdjustSharded(
    EventLoopState<F>* state,
    int slot,
    <Name>State<F>* strat_state,
    FPN<F> current_price,
    const RollingStats<F, W>* rolling,
    const ControllerConfig<F>* cfg)
{
    if (FPN_IsZero(rolling->price_stddev)) return;

    int partial_on = state->oms->partial_exit_enabled ? 1 : 0;
    uint16_t my_mask = partial_on
        ? (uint16_t)((1u << (slot * 2)) | (1u << (slot * 2 + 1)))
        : (uint16_t)(1u << slot);
    uint16_t bm = (uint16_t)(state->oms->portfolio.active_bitmap & my_mask);

    FPN<F> sl_offset   = FPN_Mul(rolling->price_stddev, cfg->sl_trail_mult);
    FPN<F> trailing_sl = FPN_Sub(current_price, sl_offset);

    while (bm) {
        int pidx = __builtin_ctz(bm);
        bm &= (uint16_t)(bm - 1);
        FPN<F> entry = state->oms->portfolio.positions[pidx].entry_price;
        if (FPN_IsZero(entry)) continue;

        // ... your trailing logic here. Common pattern:
        //     - only trail when in profit (current_price > original_tp)
        //     - only ratchet upward (Strategy_WriteRatchetSL handles this)
        Strategy_WriteRatchetSL(state, slot, trailing_sl, entry, cfg);
    }

    (void)strat_state;
}
} // namespace tt
