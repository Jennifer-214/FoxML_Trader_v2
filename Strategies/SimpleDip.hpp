// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [SIMPLE DIP STRATEGY]
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

template <unsigned F> struct SimpleDipState {
    FPN<F> recent_high;      // rolling max price (updated every slow path)
    int initialized;
};

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
inline void SimpleDip_Adapt(SimpleDipState<F> *state, FPN<F> current_price,
                             FPN<F> portfolio_delta, uint16_t active_bitmap,
                             const BuySideGateConditions<F> *buy_conds,
                             const ControllerConfig<F> *cfg) {
    // track the rolling high — just use the max from rolling stats
    // (passed in via BuySignal, not here — Adapt has no rolling stats access)
    (void)current_price; (void)portfolio_delta; (void)active_bitmap;
    (void)buy_conds; (void)cfg;
}

//======================================================================================================
// BUY SIGNAL — buy when price is X% below the recent high
//======================================================================================================
template <unsigned F>
inline BuySideGateConditions<F> SimpleDip_BuySignal(
    SimpleDipState<F> *state, const RollingStats<F> *rolling,
    const RollingStats<F, 512> *rolling_long, const ControllerConfig<F> *cfg,
    FPN<F> ema_price = FPN_Zero<F>()) {

    BuySideGateConditions<F> conds;

    // update recent high from rolling stats
    // use the higher of short and long window max for a broader view
    state->recent_high = rolling->price_max;
    if (rolling_long && FPN_GreaterThan(rolling_long->price_max, state->recent_high))
        state->recent_high = rolling_long->price_max;

    // buy price = recent_high * (1 - dip_pct)
    // dip_pct comes from entry_offset_pct (reuse existing config field)
    // e.g. 0.15% dip from high → buy
    FPN<F> dip_offset = FPN_Mul(state->recent_high, cfg->entry_offset_pct);
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
