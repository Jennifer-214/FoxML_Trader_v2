// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[Strategies/SimpleDip.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the simplest strategy — buy X% below the recent high, fixed TP/SL, no regression/adaptation/regime; the 4-lifecycle stubs keep the dispatcher uniform]
// [CONTAINS]
//   - [STRUCT]_[SimpleDipState]
//   - [FUNCTION]_[SimpleDip_BuySignal]   (Init / Adapt / ExitAdjustSharded stubs share the file)
//======================================================================================================
// buy when price drops X% below the recent high. fixed TP/SL. no regression,
// no adaptation, no regime dependency. just price action.
//
// the idea: prices bounce. if BTC drops 0.15% from its recent high in the last
// 30 minutes, buy the dip and take profit at 0.10% or cut at 0.15%.
//
// uses rolling stats for the high (already computed), config for thresholds.
//======================================================================================================
#pragma once

#include "../CoreFrameworks/OrderGates.hpp"
#include "../ML_Headers/RollingStats.hpp"
#include "../CoreFrameworks/ControllerConfig.hpp"
#include "StrategyInterface.hpp"

//======================================================================
// [STRUCT]_[SimpleDipState]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[rolling recent-high + init flag — the whole strategy state]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F> struct SimpleDipState {
    FPN_Binary<F> recent_high;      // rolling max price (updated every slow path)
    int initialized;
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]   (tool-refreshed — do NOT hand-edit; check_cache_layout --fix owns these)
// [SIZE]_[32B]
// [ALIGN]_[16]
// [CACHE_LINES]_[1]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[SimpleDipState]
//======================================================================

//======================================================================================================
// INIT — called once after warmup
//======================================================================================================
template <unsigned F>
inline void SimpleDip_Init(SimpleDipState<F> *state, const RollingStats<F> *rolling,
                            BuySideGateConditions<F> *buy_conds) {
    state->recent_high = rolling->price_max;
    state->initialized = 1;
    (void)buy_conds;
}

//======================================================================================================
// ADAPT — update rolling high. no regression, no filter shifting.
//======================================================================================================
template <unsigned F>
inline void SimpleDip_Adapt(SimpleDipState<F> *state, FPN_Binary<F> current_price,
                             FPN_Binary<F> portfolio_delta, uint16_t active_bitmap,
                             const BuySideGateConditions<F> *buy_conds,
                             const ControllerConfig<F> *cfg) {
    // track the rolling high — just use the max from rolling stats
    // (passed in via BuySignal, not here — Adapt has no rolling stats access)
    (void)current_price; (void)portfolio_delta; (void)active_bitmap;
    (void)buy_conds; (void)cfg;
}

//======================================================================================================
// BUY SIGNAL — buy when price is X% below the recent high
//======================================================================
// [FUNCTION]_[SimpleDip_BuySignal]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[buy gate = recent_high * (1 - entry_offset_pct); volume gate = avg * mult; buys below]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline BuySideGateConditions<F> SimpleDip_BuySignal(
    SimpleDipState<F> *state, const RollingStats<F> *rolling,
    const RollingStats<F, 512> *rolling_long, const ControllerConfig<F> *cfg,
    FPN_Binary<F> ema_price = FPN_Zero<F>()) {

    BuySideGateConditions<F> conds;

    // update recent high from rolling stats
    // use the higher of short and long window max for a broader view
    state->recent_high = rolling->price_max;
    if (rolling_long && FPN_GreaterThan(rolling_long->price_max, state->recent_high))
        state->recent_high = rolling_long->price_max;

    // buy price = recent_high * (1 - dip_pct)
    // dip_pct comes from entry_offset_pct (reuse existing config field)
    // e.g. 0.15% dip from high → buy
    FPN_Binary<F> dip_offset = FPN_Mul(state->recent_high, Money_ToBinary(cfg->entry_offset_pct));  // feature-side ingress
    conds.price = FPN_Sub(state->recent_high, dip_offset);

    // volume gate: same as MR — require minimum volume
    conds.volume = FPN_Mul(rolling->volume_avg, cfg->volume_multiplier);

    // buy below (dip buying)
    conds.gate_direction = 0;

    (void)ema_price;  // not used — we use recent_high directly
    (void)rolling_long;

    return conds;
}

//======================================================================================================
// EXIT ADJUST — none. fixed TP/SL from config, no trailing.
//======================================================================================================
// intentionally empty — TP/SL are set at fill time from config and never modified.
// this is the simplest possible exit: hit TP or hit SL, nothing else.
//
// v5.8.0: stub function so the X-macro registry has a uniform 4-lifecycle
// signature across all strategies. The dispatcher calls it but it does
// nothing.
namespace tt {
template <unsigned F> struct EventLoopState;
template <unsigned F, unsigned W>
inline void SimpleDip_ExitAdjustSharded(
    EventLoopState<F>* state, int slot,
    SimpleDipState<F>* strat_state,
    Money current_price,
    const RollingStats<F, W>* rolling,
    const ControllerConfig<F>* cfg) {
    (void)state; (void)slot; (void)strat_state;
    (void)current_price; (void)rolling; (void)cfg;
}
} // namespace tt
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[SimpleDip_BuySignal]
//======================================================================
