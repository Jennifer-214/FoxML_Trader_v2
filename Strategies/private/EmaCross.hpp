// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// PRIVATE — do not publish to public repositories.

//======================================================================================================
// [EMA CROSS STRATEGY]
//======================================================================================================
// buy dips below EMA during confirmed uptrends (EMA > SMA crossover).
// no regression adaptation, no idle squeeze, no death spiral.
//
// the EMA is updated every tick on the hot path (~2ns). this strategy uses it
// as a dynamic reference price — faster than rolling avg, no lag from regression.
//
// entry: price dips below EMA by (stddev * dip_mult) while EMA > short SMA AND long SMA
// exit: trail TP/SL when EMA slope is positive (trend confirmed), fixed otherwise
//======================================================================================================
#pragma once

#include "../../CoreFrameworks/OrderGates.hpp"
#include "../../ML_Headers/RollingStats.hpp"
#include "../../CoreFrameworks/ControllerConfig.hpp"
#include "../StrategyInterface.hpp"

template <unsigned F> struct EmaCrossState {
    FPN<F> prev_ema;        // previous slow-path EMA value (for slope)
    FPN<F> last_ema_slope;  // ema - prev_ema (positive = rising)
    int initialized;
};

//======================================================================================================
// INIT — called once after warmup
//======================================================================================================
template <unsigned F>
inline void EmaCross_Init(EmaCrossState<F> *state, const RollingStats<F> *rolling,
                           BuySideGateConditions<F> *buy_conds) {
    state->prev_ema = rolling->price_avg;  // seed with rolling avg until EMA warms up
    state->last_ema_slope = FPN_Zero<F>();
    state->initialized = 1;
    (void)buy_conds;
}

//======================================================================================================
// ADAPT — no-op. no regression, no idle squeeze, no filter shifting.
//======================================================================================================
template <unsigned F>
inline void EmaCross_Adapt(EmaCrossState<F> *state, FPN<F> current_price,
                            FPN<F> portfolio_delta, uint16_t active_bitmap,
                            const BuySideGateConditions<F> *buy_conds,
                            const ControllerConfig<F> *cfg) {
    (void)state; (void)current_price; (void)portfolio_delta;
    (void)active_bitmap; (void)buy_conds; (void)cfg;
}

//======================================================================================================
// BUY SIGNAL — buy dips below EMA when uptrend confirmed via crossover
//======================================================================================================
template <unsigned F>
inline BuySideGateConditions<F> EmaCross_BuySignal(
    EmaCrossState<F> *state, const RollingStats<F> *rolling,
    const RollingStats<F, 512> *rolling_long, const ControllerConfig<F> *cfg,
    FPN<F> ema_price = FPN_Zero<F>()) {

    BuySideGateConditions<F> conds;

    // use EMA if available, fall back to rolling avg
    FPN<F> ref = FPN_IsZero(ema_price) ? rolling->price_avg : ema_price;

    // update EMA slope (for ExitAdjust trailing)
    state->last_ema_slope = FPN_Sub(ref, state->prev_ema);
    state->prev_ema = ref;

    // crossover check: EMA must be above short SMA (128-tick)
    // use absolute difference vs stddev instead of normalized spread
    // (normalized spread is too tiny when EMA and SMA converge in ranging markets)
    FPN<F> short_sma = rolling->price_avg;
    int short_cross = 0;
    if (!FPN_IsZero(short_sma) && !FPN_IsZero(rolling->price_stddev)) {
        FPN<F> diff = FPN_Sub(ref, short_sma);
        // EMA must be above SMA (positive diff = sign==0 and not zero)
        int ema_above = (diff.sign == 0) && !FPN_IsZero(diff);
        // spread as fraction of stddev — more meaningful than % of price
        FPN<F> spread_stddevs = FPN_DivNoAssert(diff, rolling->price_stddev);
        short_cross = ema_above & FPN_GreaterThan(spread_stddevs, cfg->emacross_crossover_min);
    }

    int uptrend = short_cross;

    // buy price = EMA - (stddev * dip_mult)
    FPN<F> dip = FPN_Mul(rolling->price_stddev, cfg->emacross_dip_mult);
    conds.price = FPN_Sub(ref, dip);

    // volume gate
    conds.volume = FPN_Mul(rolling->volume_avg, cfg->volume_multiplier);

    // buy below (dip buying in uptrend)
    conds.gate_direction = 0;

    // zero the gate if uptrend not confirmed — no fills, no death spiral
    Gate_Zero(&conds, uptrend);

    return conds;
}

//======================================================================================================
// EXIT ADJUST — trail TP/SL when EMA slope is positive
//======================================================================================================
template <unsigned F>
inline void EmaCross_ExitAdjust(Portfolio<F> *portfolio, FPN<F> current_price,
                                 const RollingStats<F> *rolling,
                                 EmaCrossState<F> *state,
                                 const ControllerConfig<F> *cfg) {
    FPN<F> stddev = rolling->price_stddev;
    if (FPN_IsZero(stddev)) return;

    // is EMA rising? (positive slope = trend continuation)
    int ema_rising = (state->last_ema_slope.sign == 0) && !FPN_IsZero(state->last_ema_slope);

    uint16_t active = portfolio->active_bitmap;
    while (active) {
        int idx = __builtin_ctz(active);
        active &= active - 1;

        auto *pos = &portfolio->positions[idx];

        // only trail if price is above original TP (in profit territory)
        if (!FPN_GreaterThan(current_price, pos->take_profit_price)) continue;

        if (ema_rising) {
            // EMA confirms uptrend — trail with wider multiplier (let it run)
            FPN<F> trail_dist = FPN_Mul(stddev, FPN_Mul(cfg->tp_trail_mult,
                                                          cfg->emacross_trail_mult));
            FPN<F> new_tp = FPN_Sub(current_price, trail_dist);
            pos->take_profit_price = FPN_Max(pos->take_profit_price, new_tp);

            // trail SL up too
            FPN<F> sl_dist = FPN_Mul(stddev, cfg->sl_trail_mult);
            FPN<F> new_sl = FPN_Sub(current_price, sl_dist);
            // only ratchet SL up, never down
            if (FPN_LessThan(pos->stop_loss_price, pos->entry_price)) {
                pos->stop_loss_price = FPN_Max(pos->stop_loss_price, new_sl);
            }
        }

        // enforce SL floor invariant (2:1 min reward/risk)
        if (FPN_LessThan(pos->stop_loss_price, pos->entry_price)) {
            FPN<F> tp_dist = FPN_Sub(pos->take_profit_price, pos->entry_price);
            FPN<F> min_sl_dist = FPN_Mul(tp_dist, FPN_FromDouble<F>(0.5));
            FPN<F> sl_floor = FPN_SubSat(pos->entry_price, min_sl_dist);
            pos->stop_loss_price = FPN_Max(pos->stop_loss_price, sl_floor);
        }
    }
}
