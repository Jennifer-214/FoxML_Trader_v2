// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [STRATEGY PARAMETERS — pure functions that build GateParameters packs]
//======================================================================================================
//
// Phase 06 of the per-core sharding port. Each strategy provides a pure
// `_BuildParameters` function that takes the current slow-path state
// (RollingStats, ControllerConfig, allocated balance) and produces a
// GateParameters<F> pack the execution core can read on its hot path.
//
// Architecture:
//
//   slow path (controller core, every ~3 sim seconds):
//     1. RollingStats_Push absorbs the latest tick into the rolling window
//     2. for each registered execution core, dispatch to the strategy's
//        _BuildParameters via Strategy_BuildParameters(strategy_id, ...)
//     3. write the resulting pack into CoreContext::pending_params, mark dirty
//     4. EventLoop_PushParameters atomically swaps the pack into the core's
//        ParameterSlot via the seqlock from phase 05
//
//   hot path (execution core, every tick):
//     1. ParameterSlot_Read snapshots the active pack into stack-local storage
//     2. BG_Evaluate / SG_Evaluate consume the pack
//     3. branchless decision, optional event push
//
// Pure invariant: each _BuildParameters function MUST be a pure function of
// its inputs (RollingStats, config, allocated_balance). No globals, no
// statics, no Portfolio reads. The controller is the source of truth for
// per-core state and passes the parameters explicitly. Pitfall P6.3 (hidden
// statics) is the failure mode this invariant protects against.
//
// Phase 06 minimum viable scope:
//   - SimpleDip_BuildParameters: full port. SimpleDip is the simplest strategy
//     and the current default in production, so it's the priority.
//   - MeanReversion / Momentum / EMACross: minimal stubs that produce safe
//     defaults. Full ports happen in followup work — they have complex
//     adaptive feedback loops that need separate attention.
//   - Strategy_BuildParameters dispatcher routes by strategy_id.
//
//======================================================================================================

#pragma once

#include "../CoreFrameworks/ControllerConfig.hpp"
#include "../CoreFrameworks/GateParameters.hpp"
#include "../FixedPoint/FixedPointN.hpp"
#include "../ML_Headers/RollingStats.hpp"
#include "StrategyInterface.hpp"

#include <cstdint>

namespace tt {

//======================================================================================================
// [SIMPLEDIP — full port]
//======================================================================================================
//
// SimpleDip buys when price drops `entry_offset_pct` below the recent high
// in the rolling window. TP and SL are fixed percentage offsets from the
// expected entry price (config: take_profit_pct, stop_loss_pct).
//
// inputs read from rolling stats:
//   - price_max:  recent high used as the dip reference
//   - volume_avg: rolling mean volume, used for the volume gate threshold
//
// inputs read from config:
//   - entry_offset_pct: dip depth (e.g. 0.0015 = 0.15% below high)
//   - volume_multiplier: volume gate (tick volume must be > avg * multiplier)
//   - take_profit_pct: TP offset (e.g. 0.03 = 3%)
//   - stop_loss_pct: SL offset (e.g. 0.015 = 1.5%)
//
// allocated_balance is the per-core capital share. trade_size is computed as
// allocated_balance / expected_entry_price (i.e. spend the full allocation
// on a single position). this matches the legacy single-slot mode.
//
// Note on absolute vs relative TP/SL:
//   The execution core's SG_Evaluate uses absolute TP and SL prices. We don't
//   know the actual entry price ahead of time — we only know the threshold
//   that BG will fire on. So we compute TP/SL relative to the threshold price
//   (the expected entry). If the actual entry is slightly different, TP/SL
//   are off by the same amount. The controller's slow-path rebuild (every
//   ~3 sim seconds) refreshes them as the rolling stats move, so the drift
//   is bounded.
//======================================================================================================
template <unsigned F, unsigned W = 128, unsigned WL = 512>
inline void SimpleDip_BuildParameters(
    const RollingStats<F, W>* rolling,
    const ControllerConfig<F>* config,
    FPN<F> allocated_balance,
    GateParameters<F>* out,
    const RollingStats<F, WL>* rolling_long = nullptr
) {
    // Match production SimpleDip_BuySignal: use MAX(short_max, long_max) when
    // a long-window stats is provided, otherwise fall back to the short max.
    // Without this the sharded path produces a tighter entry threshold than
    // the legacy path and misses trades that legacy takes.
    FPN<F> recent_high = rolling->price_max;
    if (rolling_long && FPN_GreaterThan(rolling_long->price_max, recent_high)) {
        recent_high = rolling_long->price_max;
    }

    // expected_entry = recent_high * (1 - entry_offset_pct)
    FPN<F> dip_offset = FPN_Mul(recent_high, config->entry_offset_pct);
    FPN<F> expected_entry = FPN_Sub(recent_high, dip_offset);

    // tp = expected_entry * (1 + take_profit_pct)
    FPN<F> tp_amount = FPN_Mul(expected_entry, config->take_profit_pct);
    FPN<F> take_profit_price = FPN_Add(expected_entry, tp_amount);

    // sl = expected_entry * (1 - stop_loss_pct)
    FPN<F> sl_amount = FPN_Mul(expected_entry, config->stop_loss_pct);
    FPN<F> stop_loss_price = FPN_Sub(expected_entry, sl_amount);

    // volume gate: tick volume must exceed rolling avg * multiplier
    FPN<F> volume_threshold = FPN_Mul(rolling->volume_avg, config->volume_multiplier);

    // sizing: allocated_balance / expected_entry_price (notional ÷ price = qty)
    // guard against zero entry to avoid divide-by-zero (rolling stats uninit)
    FPN<F> trade_size = FPN_Zero<F>();
    if (!FPN_IsZero(expected_entry)) {
        trade_size = FPN_DivNoAssert(allocated_balance, expected_entry);
    }

    out->bg_price_threshold   = expected_entry;
    out->bg_volume_threshold  = volume_threshold;
    // Legacy absolute prices kept for back-compat with tests that don't
    // exercise the per-fill path.
    out->sg_take_profit_price = take_profit_price;
    out->sg_stop_loss_price   = stop_loss_price;
    // Phase 14 per-fill: the execution core will compute live TP/SL from
    // the actual fill price using these percentages, overriding the legacy
    // absolute prices above. Removes the structural loss bias.
    out->tp_pct               = config->take_profit_pct;
    out->sl_pct               = config->stop_loss_pct;
    out->trade_size           = trade_size;
    out->strategy_id          = STRATEGY_SIMPLE_DIP;
    out->flags                = GATE_FLAG_TP_ENABLED | GATE_FLAG_SL_ENABLED;
    for (int i = 0; i < 6; ++i) out->_pad[i] = 0;
}

//======================================================================================================
// [MEAN REVERSION — STUB, full port deferred]
//======================================================================================================
// Real MeanReversion uses a regression feedback loop to adapt entry_offset_pct
// and volume_multiplier based on recent P&L. The state lives in
// `MeanReversionState<F>` and is mutated by `MeanReversion_Adapt`. Porting it
// to a pure function requires moving the regression state out of the strategy
// and into the controller (or accepting that the controller calls _Adapt
// before each _BuildParameters cycle).
//
// For phase 06, this stub produces a safe deterministic pack: same entry
// price as SimpleDip but with the MR strategy_id so dispatcher routing works
// for tests. Full port is followup work.
//======================================================================================================
template <unsigned F, unsigned W = 128>
inline void MeanReversion_BuildParameters(
    const RollingStats<F, W>* rolling,
    const ControllerConfig<F>* config,
    FPN<F> allocated_balance,
    GateParameters<F>* out
) {
    // TODO(phase06-followup): full port of the regression feedback loop.
    // For now, use the rolling mean ± stddev as the base entry/exit prices.
    FPN<F> entry_price = rolling->price_avg;
    if (FPN_IsZero(entry_price)) entry_price = rolling->price_max;

    FPN<F> tp_amount = FPN_Mul(entry_price, config->take_profit_pct);
    FPN<F> sl_amount = FPN_Mul(entry_price, config->stop_loss_pct);
    FPN<F> volume_threshold = FPN_Mul(rolling->volume_avg, config->volume_multiplier);

    FPN<F> trade_size = FPN_Zero<F>();
    if (!FPN_IsZero(entry_price)) {
        trade_size = FPN_DivNoAssert(allocated_balance, entry_price);
    }

    out->bg_price_threshold   = entry_price;
    out->bg_volume_threshold  = volume_threshold;
    out->sg_take_profit_price = FPN_Add(entry_price, tp_amount);
    out->sg_stop_loss_price   = FPN_Sub(entry_price, sl_amount);
    out->trade_size           = trade_size;
    out->strategy_id          = STRATEGY_MEAN_REVERSION;
    out->flags                = GATE_FLAG_TP_ENABLED | GATE_FLAG_SL_ENABLED;
    for (int i = 0; i < 6; ++i) out->_pad[i] = 0;
}

//======================================================================================================
// [MOMENTUM — STUB, full port deferred]
//======================================================================================================
// Real Momentum uses ROR (rate-of-rate, slope-of-slope) and R² gates from
// the rolling regression. It buys ABOVE the price (gate_direction = 1) when
// momentum is rising. Like MeanReversion, the adaptive feedback loop needs
// to be relocated to controller-side state before this becomes a pure
// function.
//======================================================================================================
template <unsigned F, unsigned W = 128>
inline void Momentum_BuildParameters(
    const RollingStats<F, W>* rolling,
    const ControllerConfig<F>* config,
    FPN<F> allocated_balance,
    GateParameters<F>* out
) {
    // TODO(phase06-followup): full port with ROR and R² gates.
    // For phase 06 stub: buy above the rolling avg, fixed TP/SL offsets.
    FPN<F> entry_price = rolling->price_avg;
    if (FPN_IsZero(entry_price)) entry_price = rolling->price_max;

    FPN<F> tp_amount = FPN_Mul(entry_price, config->take_profit_pct);
    FPN<F> sl_amount = FPN_Mul(entry_price, config->stop_loss_pct);
    FPN<F> volume_threshold = FPN_Mul(rolling->volume_avg, config->volume_multiplier);

    FPN<F> trade_size = FPN_Zero<F>();
    if (!FPN_IsZero(entry_price)) {
        trade_size = FPN_DivNoAssert(allocated_balance, entry_price);
    }

    out->bg_price_threshold   = entry_price;
    out->bg_volume_threshold  = volume_threshold;
    out->sg_take_profit_price = FPN_Add(entry_price, tp_amount);
    out->sg_stop_loss_price   = FPN_Sub(entry_price, sl_amount);
    out->trade_size           = trade_size;
    out->strategy_id          = STRATEGY_MOMENTUM;
    out->flags                = GATE_FLAG_TP_ENABLED | GATE_FLAG_SL_ENABLED;
    for (int i = 0; i < 6; ++i) out->_pad[i] = 0;
}

//======================================================================================================
// [EMA CROSS — STUB, full port deferred]
//======================================================================================================
// Real EMA Cross uses an EMA computed by the controller on the slow path,
// then buys dips when fast EMA is above slow EMA. The EMA state lives in
// the controller, not the strategy. Once the controller exposes a clean
// "current EMA" output, the port becomes straightforward.
//======================================================================================================
template <unsigned F, unsigned W = 128>
inline void EmaCross_BuildParameters(
    const RollingStats<F, W>* rolling,
    const ControllerConfig<F>* config,
    FPN<F> allocated_balance,
    GateParameters<F>* out
) {
    // TODO(phase06-followup): full port with EMA crossover trigger.
    // For phase 06 stub: behave like SimpleDip but with EMA_CROSS strategy_id
    // so the dispatcher routes correctly.
    SimpleDip_BuildParameters(rolling, config, allocated_balance, out);
    out->strategy_id = STRATEGY_EMA_CROSS;
}

//======================================================================================================
// [STRATEGY DISPATCHER]
//======================================================================================================
// the slow path calls this once per registered execution core. dispatches to
// the strategy's _BuildParameters by strategy_id. STRATEGY_NONE produces a
// zero-init pack so the core has safe defaults but won't trade (combined
// with permission=0 enforced by the run loop, this is bulletproof).
//
// the dispatcher is the integration point between the per-core sharded
// architecture and the strategy library. adding a new strategy:
//   1. define STRATEGY_FOO constant in StrategyInterface.hpp (or wherever)
//   2. write Foo_BuildParameters following the pattern above
//   3. add a case to this switch
//   4. done — Strategy_BuildParameters dispatches to it automatically
//======================================================================================================
template <unsigned F, unsigned W = 128, unsigned WL = 512>
inline void Strategy_BuildParameters(
    uint8_t strategy_id,
    const RollingStats<F, W>* rolling,
    const ControllerConfig<F>* config,
    FPN<F> allocated_balance,
    GateParameters<F>* out,
    const RollingStats<F, WL>* rolling_long = nullptr
) {
    switch (strategy_id) {
        case STRATEGY_SIMPLE_DIP:
            SimpleDip_BuildParameters(rolling, config, allocated_balance, out, rolling_long);
            return;
        case STRATEGY_MEAN_REVERSION:
            MeanReversion_BuildParameters(rolling, config, allocated_balance, out);
            return;
        case STRATEGY_MOMENTUM:
            Momentum_BuildParameters(rolling, config, allocated_balance, out);
            return;
        case STRATEGY_EMA_CROSS:
            EmaCross_BuildParameters(rolling, config, allocated_balance, out);
            return;
        default:
            // STRATEGY_NONE and any unknown id: safe zero pack, no trading.
            GateParameters_Init(out);
            return;
    }
}

}  // namespace tt
