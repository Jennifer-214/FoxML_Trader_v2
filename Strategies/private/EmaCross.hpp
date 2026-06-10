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
#include "../../MemHeaders/OmsStateFlagRegistry.hpp"  // v5.15.5.C.2 (S3a) — MASK_OMS_STATE_*
#include "../StrategyInterface.hpp"

template <unsigned F> struct EmaCrossState {
    FPN_Binary<F> prev_ema;        // previous slow-path EMA value (for slope)
    FPN_Binary<F> last_ema_slope;  // ema - prev_ema (positive = rising)
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
inline void EmaCross_Adapt(EmaCrossState<F> *state, FPN_Binary<F> current_price,
                            FPN_Binary<F> portfolio_delta, uint16_t active_bitmap,
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
    FPN_Binary<F> ema_price = FPN_Zero<F>()) {

    BuySideGateConditions<F> conds;

    // use EMA if available, fall back to rolling avg
    FPN_Binary<F> ref = FPN_IsZero(ema_price) ? rolling->price_avg : ema_price;

    // update EMA slope (for ExitAdjust trailing)
    state->last_ema_slope = FPN_Sub(ref, state->prev_ema);
    state->prev_ema = ref;

    // crossover check: EMA must be above short SMA (128-tick)
    // use absolute difference vs stddev instead of normalized spread
    // (normalized spread is too tiny when EMA and SMA converge in ranging markets)
    FPN_Binary<F> short_sma = rolling->price_avg;
    int short_cross = 0;
    {
        // branchless compute-guard: compute always (zero divisor → safe saturate), gate the result by `valid`.
        int valid = !FPN_IsZero(short_sma) & !FPN_IsZero(rolling->price_stddev);
        FPN_Binary<F> diff = FPN_Sub(ref, short_sma);
        int ema_above = (diff.v > 0);   // EMA above SMA: positive diff (was sign==0 && !IsZero; 16B two's-comp)
        // spread as fraction of stddev — more meaningful than % of price
        FPN_Binary<F> spread_stddevs = FPN_DivNoAssert(diff, rolling->price_stddev);
        short_cross = valid & ema_above & FPN_GreaterThan(spread_stddevs, cfg->emacross_crossover_min);
    }

    int uptrend = short_cross;

    // buy price = EMA - (stddev * dip_mult)
    FPN_Binary<F> dip = FPN_Mul(rolling->price_stddev, cfg->emacross_dip_mult);
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
inline void EmaCross_ExitAdjust(Portfolio<F> *portfolio, Money current_price,
                                 const RollingStats<F> *rolling,
                                 EmaCrossState<F> *state,
                                 const ControllerConfig<F> *cfg) {
    FPN_Binary<F> stddev = rolling->price_stddev;
    if (FPN_IsZero(stddev)) return;

    // is EMA rising? (strictly-positive slope = trend continuation)
    // Ship-A 16B two's-comp: (sign==0 && !IsZero) [non-negative AND non-zero] == strictly positive == v > 0.
    int ema_rising = (state->last_ema_slope.v > 0);

    uint16_t active = portfolio->active_bitmap;
    while (active) {
        int idx = __builtin_ctz(active);
        active &= active - 1;

        auto *pos = &portfolio->positions[idx];

        // only trail if price is above original TP (in profit territory)
        if (!Money_Gt(current_price, pos->take_profit_price)) continue;

        if (ema_rising) {
            // EMA confirms uptrend — trail with wider multiplier (let it run)
            FPN_Binary<F> trail_dist = FPN_Mul(stddev, FPN_Mul(cfg->tp_trail_mult,
                                                          cfg->emacross_trail_mult));
            Money new_tp = Money_Sub(current_price, Money_FromBinary(trail_dist));
            pos->take_profit_price = Money_Max(pos->take_profit_price, new_tp);

            // trail SL up too — but cap at fee-floor so we don't ratchet
            // into a guaranteed-net-loss exit.
            FPN_Binary<F> sl_dist = FPN_Mul(stddev, cfg->sl_trail_mult);
            Money new_sl = Money_Sub(current_price, Money_FromBinary(sl_dist));

            // v5.1.7: fee-floor on the SL ratchet. entry × (1 - 3 × fee_rate)
            // is the floor below which any SG-fired exit would be net-
            // negative after round-trip fees.
            Money fee_rate = !Money_IsZero(cfg->fee_rate_taker)
                ? cfg->fee_rate_taker : cfg->fee_rate;
            Money fee_floor_dist = Money_Mul(pos->entry_price,
                Money_Mul(fee_rate, Money_FromInt(3)));
            Money sl_floor = Money_Sub(pos->entry_price, fee_floor_dist);
            new_sl = Money_Min(new_sl, sl_floor);

            // only ratchet SL up, never down
            if (Money_Lt(pos->stop_loss_price, pos->entry_price)) {
                pos->stop_loss_price = Money_Max(pos->stop_loss_price, new_sl);
            }
        }

        // enforce SL floor invariant (2:1 min reward/risk)
        if (Money_Lt(pos->stop_loss_price, pos->entry_price)) {
            Money tp_dist = Money_Sub(pos->take_profit_price, pos->entry_price);
            Money min_sl_dist = Money_Mul(tp_dist, Money{ 50000000 });  // exact 0.5
            Money sl_floor = Money_Sub(pos->entry_price, min_sl_dist);
            pos->stop_loss_price = Money_Max(pos->stop_loss_price, sl_floor);
        }
    }
}

//======================================================================================================
// [EXIT ADJUST — sharded, ratchet_sl path]
//======================================================================================================
// v5.4.0 Phase 2.4: sharded equivalent of EmaCross_ExitAdjust above.
// Trail SL only when EMA is rising (state->last_ema_slope > 0). Routes
// through Strategy_WriteRatchetSL so the v5.1.7 fee-floor cap applies.
// TP trailing deferred to Phase 3.
//======================================================================================================
namespace tt {
template <unsigned F> struct EventLoopState;
template <unsigned F>
bool Strategy_WriteRatchetSL(EventLoopState<F>* state, int slot,
                              FPN_Binary<F> proposed_sl, FPN_Binary<F> entry_price,
                              const ControllerConfig<F>* cfg);

template <unsigned F, unsigned W>
inline void EmaCross_ExitAdjustSharded(
    EventLoopState<F>* state,
    int slot,
    EmaCrossState<F>* es,
    Money current_price,
    const RollingStats<F, W>* rolling,
    const ControllerConfig<F>* cfg
) {
    if (FPN_IsZero(rolling->price_stddev)) return;

    // EmaCross-specific gate: only trail when EMA is rising (uptrend
    // continuation). Same geometry as the legacy EmaCross_ExitAdjust.
    int ema_rising = (es->last_ema_slope.v > 0);   // EMA rising: positive slope (was sign==0 && !IsZero; 16B)
    if (!ema_rising) return;

    // v5.15.5.C.2 (S3a) — bit-packed in oms_state_flags.
    int partial_on = BITMAP_IS_SET(state->oms->oms_state_flags, tt::MASK_OMS_STATE_PARTIAL_EXIT_ENABLED);
    uint16_t my_mask = partial_on
        ? (uint16_t)((1u << (slot * 2)) | (1u << (slot * 2 + 1)))
        : (uint16_t)(1u << slot);
    uint16_t bm = (uint16_t)(state->oms->portfolio.active_bitmap & my_mask);

    FPN_Binary<F> sl_offset   = FPN_Mul(rolling->price_stddev, cfg->sl_trail_mult);
    Money trailing_sl = Money_Sub(current_price, Money_FromBinary(sl_offset));

    while (bm) {
        int pidx = __builtin_ctz(bm);
        bm &= (uint16_t)(bm - 1);
        Money entry = state->oms->portfolio.positions[pidx].entry_price;
        if (Money_IsZero(entry)) continue;
        // Only trail positions that are above their original TP — same
        // gate as the legacy EmaCross_ExitAdjust.
        Money orig_tp = state->oms->portfolio.positions[pidx].original_tp;
        if (!Money_IsZero(orig_tp) && !Money_Gt(current_price, orig_tp)) continue;
        Strategy_WriteRatchetSL(state, slot, trailing_sl, entry, cfg);
    }
}
} // namespace tt
