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
#include "../ML_Headers/BarrierGate.hpp"
#include "../ML_Headers/ConfidenceScore.hpp"
#include "../ML_Headers/CoreModelZoo.hpp"
#include "../ML_Headers/ModelInference.hpp"
#include "../ML_Headers/RollingStats.hpp"
#include "../Strategies/RegimeDetector.hpp"
#include "SimpleDip.hpp"          // SimpleDipState<F> — Phase 2.1 state-aware BuildParameters
#include "StrategyInterface.hpp"

#include <cstdint>

namespace tt {

//======================================================================================================
// [ML BUILD CONTEXT — Phase 6prep sharded c13/c15]
//======================================================================================================
// Bundle of ML-only extras that the dispatcher passes through to ML_BuildParameters
// as a single void*. Keeps the dispatcher signature stable when more ML inputs
// are added (cost model, vol scaler) — just add a field here.
//
// Non-ML strategies don't see this; the dispatcher's void* ml_ctx is opaque to
// them.
//
// Field semantics:
//   model_handle    — &CoreModelZoo<F>, populated by EngineSharded per core
//   confidence      — &CoreContext::confidence, fed (pred, return) at exit fill
//   out_prediction  — written by ML_BuildParameters with the prediction value
//                     used in the gate decision; drainer snapshots this into
//                     active_prediction at entry-submit time
//   out_confidence  — written with the conf used (so snapshot reads match the
//                     value that drove the gate decision; no recomputation)
//======================================================================================================
struct MLBuildContext {
    void*               model_handle;
    ConfidenceScorer*   confidence;
    double*             out_prediction;
    double*             out_confidence;
    // v4.0 train-serve parity: pass through the RORRegressor + EMA price
    // that the engine's slow path maintains. ML_BuildParameters uses these
    // with Regime_ComputeSignals so ALL features ModelFeatures_Pack reads
    // (ror_slope, ema_sma_spread, ema_above_sma) match what the legacy
    // backtest path produces during training.
    void*               ror_regressor;   // const RORRegressor<F>*
    void*               ema_price;       // const FPN<F>*
    // v4.3 — feature-pack expansion. Same pattern: engine maintains the
    // state, ML_BuildParameters threads it into Regime_ComputeSignals so
    // sharded live and legacy backtest produce identical features.
    void*               rolling_medium;  // const RollingStats<F, 256>*
    void*               rolling_baseline;// const RollingStats<F, 1024>*
    void*               cumdelta_state;  // const CumDeltaState<F>*
    void*               tick_rate_state; // const TickRateState*
    uint64_t            timestamp_us;    // current tick wall time for hour-of-day
    // v4.5 Wave 1 — D.1/D.2/D.4 state. Same threading pattern as v4.3.
    void*               book_imb_history;   // const BookImbalanceHistory<F, 1024>*
    void*               flow_state;         // const FlowState*
    void*               large_trade_state;  // const LargeTradeState<F, 1024>*
    // v4.6 Wave 2 — D.3 spread dynamics state + current spread/mid (from
    // depth state's BookSnapshot). FlowFeatures.hpp exposes SpreadState.
    void*               spread_state;       // const SpreadState<F, 1024>*
    double              current_spread;     // BookSnapshot::spread (FPN→double)
    double              current_mid_price;  // BookSnapshot::mid_price (FPN→double)
};

//======================================================================================================
// [SPACING + FEE-FLOOR HELPERS — v4.0.3]
//======================================================================================================
// Shared logic across all strategies' _BuildParameters. Pre-v4.0.3 these
// were silently ignored by sharded strategies — now applied uniformly.
//
// Spacing: zero-gate the entry if it's within `stddev × spacing_multiplier`
// of the last entry on this core. Prevents clustering N positions at
// near-identical prices (which produces correlated wins/losses, not
// independent diversification).
//
// Fee floor: ratchet TP up so it clears `entry × fee_rate × fee_floor_mult`.
// Round-trip fees on a position are 2 × fee_rate (entry + exit), so
// fee_floor_mult of 5 means TP must clear ~2.5× round-trip fees.
//======================================================================================================
template <unsigned F, unsigned W = 128>
inline bool Strategy_SpacingOk(FPN<F> proposed_entry,
                                FPN<F> last_entry,
                                const RollingStats<F, W>* rolling,
                                const ControllerConfig<F>* config) {
    // No prior entry → spacing irrelevant.
    if (FPN_IsZero(last_entry)) return true;
    // Spacing disabled or zero stddev → can't compute meaningful threshold.
    if (FPN_IsZero(config->spacing_multiplier)) return true;
    if (FPN_IsZero(rolling->price_stddev))      return true;
    // Required min distance between entries
    FPN<F> min_dist = FPN_Mul(rolling->price_stddev, config->spacing_multiplier);
    // |proposed - last|
    FPN<F> diff = FPN_GreaterThanOrEqual(proposed_entry, last_entry)
        ? FPN_Sub(proposed_entry, last_entry)
        : FPN_Sub(last_entry, proposed_entry);
    return FPN_GreaterThanOrEqual(diff, min_dist);
}

template <unsigned F>
inline FPN<F> Strategy_TpFloor(FPN<F> entry_price,
                                FPN<F> tp_amount,
                                const ControllerConfig<F>* config) {
    if (FPN_IsZero(config->fee_floor_mult) || FPN_IsZero(config->fee_rate))
        return tp_amount;
    // Required floor = entry × fee_rate × fee_floor_mult
    FPN<F> fee_per_side = FPN_Mul(entry_price, config->fee_rate);
    FPN<F> floor = FPN_Mul(fee_per_side, config->fee_floor_mult);
    return FPN_GreaterThanOrEqual(tp_amount, floor) ? tp_amount : floor;
}

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
    const RollingStats<F, WL>* rolling_long = nullptr,
    SimpleDipState<F>* state = nullptr
) {
    // v5.4.0 Phase 2.1 — when a per-core SimpleDipState is supplied (sharded
    // production path post-Phase 1), publish recent_high into it so the
    // legacy `_BuySignal` contract is preserved (state is the source of truth
    // for "where the dip reference is"). Tests + legacy paths still pass
    // nullptr and get the inline behavior — same numerics as pre-Phase 2.1.
    FPN<F> recent_high = rolling->price_max;
    if (rolling_long && FPN_GreaterThan(rolling_long->price_max, recent_high)) {
        recent_high = rolling_long->price_max;
    }
    if (state) {
        state->recent_high = recent_high;
    }

    // expected_entry = recent_high * (1 - entry_offset_pct)
    FPN<F> dip_offset = FPN_Mul(recent_high, config->entry_offset_pct);
    FPN<F> expected_entry = FPN_Sub(recent_high, dip_offset);

    // Per-strategy TP/SL: use simpledip-specific override if set, else shared
    FPN<F> tp_pct = !FPN_IsZero(config->simpledip_tp_pct) ? config->simpledip_tp_pct : config->take_profit_pct;
    FPN<F> sl_pct = !FPN_IsZero(config->simpledip_sl_pct) ? config->simpledip_sl_pct : config->stop_loss_pct;

    // tp = expected_entry * (1 + tp_pct)
    FPN<F> tp_amount = FPN_Mul(expected_entry, tp_pct);
    FPN<F> take_profit_price = FPN_Add(expected_entry, tp_amount);

    // sl = expected_entry * (1 - sl_pct)
    FPN<F> sl_amount = FPN_Mul(expected_entry, sl_pct);
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
    out->tp_pct               = tp_pct;
    out->sl_pct               = sl_pct;
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
    // BUG FIX (v4.0.3): pre-fix used `bg_threshold = rolling->price_avg`
    // with no depth requirement — gate fired on EVERY tick price < avg
    // (statistically half of all ticks during noise). Real MR buys on
    // meaningful DIPS below mean. Now matches SimpleDip's pattern: gate
    // sits at `avg - (avg × entry_offset_pct)` so it requires a true dip.
    FPN<F> avg = rolling->price_avg;
    if (FPN_IsZero(avg)) avg = rolling->price_max;
    FPN<F> dip_offset = FPN_Mul(avg, config->entry_offset_pct);
    FPN<F> entry_price = FPN_Sub(avg, dip_offset);

    FPN<F> tp_pct = !FPN_IsZero(config->mr_tp_pct) ? config->mr_tp_pct : config->take_profit_pct;
    FPN<F> sl_pct = !FPN_IsZero(config->mr_sl_pct) ? config->mr_sl_pct : config->stop_loss_pct;
    FPN<F> tp_amount = FPN_Mul(entry_price, tp_pct);
    FPN<F> sl_amount = FPN_Mul(entry_price, sl_pct);
    FPN<F> volume_threshold = FPN_Mul(rolling->volume_avg, config->volume_multiplier);

    FPN<F> trade_size = FPN_Zero<F>();
    if (!FPN_IsZero(entry_price)) {
        trade_size = FPN_DivNoAssert(allocated_balance, entry_price);
    }

    out->bg_price_threshold   = entry_price;
    out->bg_volume_threshold  = volume_threshold;
    out->sg_take_profit_price = FPN_Add(entry_price, tp_amount);
    out->sg_stop_loss_price   = FPN_Sub(entry_price, sl_amount);
    out->tp_pct               = tp_pct;
    out->sl_pct               = sl_pct;
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
    // BUG FIX (v4.0.3): same family as MR — pre-fix used `bg_threshold = avg`
    // with no breakout depth. Gate fired on every tick price > avg (with the
    // BUY_ABOVE flag). Real momentum buys on confirmed BREAKOUTS above the
    // mean, not infinitesimal upticks. Now requires a breakout of
    // `entry_offset_pct` above the rolling mean before the gate arms.
    FPN<F> avg = rolling->price_avg;
    if (FPN_IsZero(avg)) avg = rolling->price_max;
    FPN<F> breakout_offset = FPN_Mul(avg, config->entry_offset_pct);
    FPN<F> entry_price = FPN_Add(avg, breakout_offset);

    // STDDEV-floor guard: in early warmup or dead-flat markets,
    // rolling->price_stddev can be near-zero, which made tp_amount basically
    // zero and produced TP=SL=entry positions (caught visually in v4.0.2).
    // Require a minimum stddev relative to price (1bp) before trusting the
    // stddev-mult path; otherwise fall back to percentage.
    FPN<F> min_stddev_floor = FPN_Mul(avg, FPN_FromDouble<F>(0.0001));
    int stddev_usable = FPN_GreaterThan(rolling->price_stddev, min_stddev_floor);
    FPN<F> tp_amount, sl_amount;
    if (!FPN_IsZero(config->momentum_tp_mult) && stddev_usable) {
        tp_amount = FPN_Mul(rolling->price_stddev, config->momentum_tp_mult);
        sl_amount = FPN_Mul(rolling->price_stddev, config->momentum_sl_mult);
    } else {
        tp_amount = FPN_Mul(entry_price, config->take_profit_pct);
        sl_amount = FPN_Mul(entry_price, config->stop_loss_pct);
    }
    FPN<F> volume_threshold = FPN_Mul(rolling->volume_avg, config->volume_multiplier);

    FPN<F> trade_size = FPN_Zero<F>();
    if (!FPN_IsZero(entry_price)) {
        trade_size = FPN_DivNoAssert(allocated_balance, entry_price);
    }

    out->bg_price_threshold   = entry_price;
    out->bg_volume_threshold  = volume_threshold;
    out->sg_take_profit_price = FPN_Add(entry_price, tp_amount);
    out->sg_stop_loss_price   = FPN_Sub(entry_price, sl_amount);
    out->tp_pct               = config->take_profit_pct;  // fallback for per-fill
    out->sl_pct               = config->stop_loss_pct;
    out->trade_size           = trade_size;
    out->strategy_id          = STRATEGY_MOMENTUM;

    // v5.4.0 Phase A.3 diagnostic — gated on TT_SL_DEBUG env var.
    // Logs entry-time SL emission for MOM. If sg_stop_loss_price ends up
    // ABOVE entry_price, the bug is in BuildParameters. If it's correctly
    // BELOW entry but the live position shows SL above entry, the bug is
    // downstream (Momentum_ExitAdjust trailing ratchet, or Portfolio_OpenSlot
    // path, or the regime-change ratchet at ControllerEventLoop.hpp:1535).
    {
        static int sl_dbg = -1;
        if (sl_dbg == -1) {
            const char* e = getenv("TT_SL_DEBUG");
            sl_dbg = (e && e[0] && e[0] != '0') ? 1 : 0;
        }
        static thread_local uint32_t mom_dbg_cycle = 0;
        if (sl_dbg && (mom_dbg_cycle++ & 0x0F) == 0) {
            double e = FPN_ToDouble(entry_price);
            double sl = FPN_ToDouble(out->sg_stop_loss_price);
            double tp = FPN_ToDouble(out->sg_take_profit_price);
            const char* geom = (sl < e) ? "OK_long" : (sl > e ? "INVERTED" : "ZERO");
            fprintf(stderr,
                "[mom-sl-build] entry=%.2f tp=%.2f sl=%.2f sl_amount=%.4f "
                "stddev=%.4f mom_sl_mult=%.4f geom=%s\n",
                e, tp, sl, FPN_ToDouble(sl_amount),
                FPN_ToDouble(rolling->price_stddev),
                FPN_ToDouble(config->momentum_sl_mult), geom);
        }
    }
    // v4.0: GATE_FLAG_BUY_ABOVE — momentum buys breakouts above the threshold,
    // not dips below. Pre-v4.0 the hot path was hardcoded to buy-below, so MOM
    // in sharded silently traded like MR. Strategy logic is still a stub
    // (buy at rolling avg) but at least the direction is now correct.
    out->flags                = GATE_FLAG_TP_ENABLED | GATE_FLAG_SL_ENABLED |
                                GATE_FLAG_BUY_ABOVE;
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
    // For phase 06 stub: same entry logic as SimpleDip but uses EMA Cross
    // specific TP/SL overrides if set.
    SimpleDip_BuildParameters(rolling, config, allocated_balance, out);
    out->strategy_id = STRATEGY_EMA_CROSS;
    // Override TP/SL with EMA Cross specific values if set
    if (!FPN_IsZero(config->emacross_tp_pct)) {
        out->tp_pct = config->emacross_tp_pct;
        FPN<F> entry = out->bg_price_threshold;
        out->sg_take_profit_price = FPN_Add(entry, FPN_Mul(entry, config->emacross_tp_pct));
    }
    if (!FPN_IsZero(config->emacross_sl_pct)) {
        out->sl_pct = config->emacross_sl_pct;
        FPN<F> entry = out->bg_price_threshold;
        out->sg_stop_loss_price = FPN_Sub(entry, FPN_Mul(entry, config->emacross_sl_pct));
    }
}

//======================================================================================================
// [ML — model-driven buy signals via CoreModelZoo]
//======================================================================================================
// Packs features from rolling stats, runs inference on whichever role models
// are loaded in the per-core CoreModelZoo, computes BarrierGate modulation,
// and produces gate parameters.
//
// Model resolution priority (first match wins):
//   1. CORE_MODEL_BARRIER (3-class softmax: stable/peak/valley) — primary path
//   2. CORE_MODEL_BUY_SIGNAL (single-binary, complementary interpretation) — legacy
//   3. no models → fall back to SimpleDip behavior
//
// BarrierGate modulation (when config->barrier_gate_enabled):
//   - hard block when bg.blocked (p_peak > BARRIER_HARD_BLOCK)
//   - hard block when prediction < ml_buy_threshold (signal too cold)
//   - else soft modulation: scale trade_size by bg.gate ∈ [g_min, 1.0]
//
// ml_ctx_ptr is &MLBuildContext (Phase 6prep sharded c13/c15). Legacy callers
// (LegacyReferenceDriver, experiment tests) pass nullptr — no model, no
// confidence damping, fall back to SimpleDip. Sharded production path always
// passes a real MLBuildContext.
//======================================================================================================
template <unsigned F, unsigned W = 128, unsigned WL = 512>
inline void ML_BuildParameters(
    const RollingStats<F, W>* rolling,
    const RollingStats<F, WL>* rolling_long,
    const ControllerConfig<F>* config,
    FPN<F> allocated_balance,
    GateParameters<F>* out,
    void* ml_ctx_ptr
) {
    MLBuildContext* mctx = (MLBuildContext*)ml_ctx_ptr;
    CoreModelZoo<F>* zoo = nullptr;
    ConfidenceScorer* conf_scorer = nullptr;
    double* out_prediction = nullptr;
    double* out_confidence = nullptr;
    const RORRegressor<F>* ror_in = nullptr;
    const FPN<F>* ema_in = nullptr;
    if (mctx) {
        zoo = (CoreModelZoo<F>*)mctx->model_handle;
        conf_scorer = mctx->confidence;
        out_prediction = mctx->out_prediction;
        out_confidence = mctx->out_confidence;
        ror_in = (const RORRegressor<F>*)mctx->ror_regressor;
        ema_in = (const FPN<F>*)mctx->ema_price;
    }

    // if no zoo or no models loaded, fall back to SimpleDip
    if (!zoo || !CoreModelZoo_HasAny(zoo)) {
        SimpleDip_BuildParameters(rolling, config, allocated_balance, out, rolling_long);
        out->strategy_id = STRATEGY_ML;
        return;
    }

    // compute entry price (same as SimpleDip for sizing)
    FPN<F> recent_high = rolling->price_max;
    if (rolling_long && FPN_GreaterThan(rolling_long->price_max, recent_high)) {
        recent_high = rolling_long->price_max;
    }
    FPN<F> entry_price = rolling->price_avg;
    if (FPN_IsZero(entry_price)) entry_price = recent_high;

    // v4.0 train-serve parity: when the engine provides a RORRegressor + EMA
    // pointer (sharded production path), call Regime_ComputeSignals — same
    // path the legacy backtest uses. Otherwise (legacy/test callers without
    // mctx, or mctx without these pointers), fall back to the inline minimal
    // population (preserves prior behavior for those callers; ror_slope +
    // ema_sma_spread + ema_above_sma stay zero, matching pre-v4.0).
    RegimeSignals<F> sig;
    memset(&sig, 0, sizeof(sig));
    if (ror_in && ema_in && rolling_long) {
        // v4.3 — forward the expanded feature-pack state if mctx provides it
        const RollingStats<F, 256>* mid = mctx ? (const RollingStats<F, 256>*)mctx->rolling_medium : nullptr;
        const RollingStats<F, 1024>* base = mctx ? (const RollingStats<F, 1024>*)mctx->rolling_baseline : nullptr;
        const CumDeltaState<F>* cd = mctx ? (const CumDeltaState<F>*)mctx->cumdelta_state : nullptr;
        const TickRateState* tr = mctx ? (const TickRateState*)mctx->tick_rate_state : nullptr;
        uint64_t ts = mctx ? mctx->timestamp_us : 0;
        // v4.5 Wave 1 — D.1/D.2/D.4 state passthrough
        const void* bih = mctx ? mctx->book_imb_history  : nullptr;
        const void* fs  = mctx ? mctx->flow_state        : nullptr;
        const void* lts = mctx ? mctx->large_trade_state : nullptr;
        // v4.6 Wave 2 — D.3 spread state + current spread/mid
        const void* sst = mctx ? mctx->spread_state      : nullptr;
        double cur_spread = mctx ? mctx->current_spread    : 0.0;
        double cur_mid    = mctx ? mctx->current_mid_price : 0.0;
        Regime_ComputeSignals(&sig, rolling, rolling_long, ror_in, *ema_in,
                               mid, base, cd, tr, ts,
                               bih, fs, lts,
                               sst, cur_spread, cur_mid);
    } else {
        sig.short_slope    = FPN_IsZero(rolling->price_avg) ? FPN_Zero<F>()
                             : FPN_DivNoAssert(rolling->price_slope, rolling->price_avg);
        sig.short_r2       = rolling->price_r_squared;
        sig.short_variance = rolling->price_variance;
        sig.volume_slope   = rolling->volume_slope;
        sig.volume_delta   = rolling->volume_delta;
        if (rolling_long && rolling_long->count > 0) {
            sig.long_slope    = FPN_IsZero(rolling_long->price_avg) ? FPN_Zero<F>()
                                : FPN_DivNoAssert(rolling_long->price_slope, rolling_long->price_avg);
            sig.long_r2       = rolling_long->price_r_squared;
            sig.long_variance = rolling_long->price_variance;
            if (!FPN_IsZero(rolling_long->price_variance))
                sig.vol_ratio = FPN_DivNoAssert(rolling->price_variance, rolling_long->price_variance);
            else
                sig.vol_ratio = FPN_FromDouble<F>(1.0);
        }
    }

    // pack features once — used by whichever model role is loaded
    float features[MODEL_MAX_FEATURES];
    int n = ModelFeatures_Pack(features, &sig, rolling, rolling_long);

    // run inference, prefer 3-class barrier model when available
    double prediction = 0.5;   // neutral fallback
    double p_peak     = 0.5;
    double p_valley   = 0.5;
    int have_signal   = 0;

    if (zoo->loaded_mask & CORE_MODEL_BARRIER) {
        // 3-class softmax: [0]=stable, [1]=peak, [2]=valley
        float multi[3] = {0.0f, 0.0f, 0.0f};
        int got = Model_PredictMulti(&zoo->barrier, features, n, multi, 3);
        if (got >= 3) {
            p_peak     = multi[1];
            p_valley   = multi[2];
            // entry signal = "valley imminent" — primary trade trigger
            prediction = p_valley;
            have_signal = 1;
        } else if (got == 1) {
            // model was actually binary (mis-labeled as barrier role) — still usable
            prediction = multi[0];
            p_peak     = 1.0 - prediction;
            p_valley   = prediction;
            have_signal = 1;
        }
    } else if (zoo->loaded_mask & CORE_MODEL_BUY_SIGNAL) {
        // legacy single-binary: complementary interpretation
        prediction = Model_Predict(&zoo->buy_signal, features, n);
        p_peak     = 1.0 - prediction;
        p_valley   = prediction;
        have_signal = 1;
    }

    // if inference failed, fall back to SimpleDip
    if (!have_signal) {
        SimpleDip_BuildParameters(rolling, config, allocated_balance, out, rolling_long);
        out->strategy_id = STRATEGY_ML;
        return;
    }

    // TP/SL from ML-specific config
    FPN<F> tp_pct = config->ml_tp_pct;
    FPN<F> sl_pct = config->ml_sl_pct;
    FPN<F> tp_amount = FPN_Mul(entry_price, tp_pct);
    FPN<F> sl_amount = FPN_Mul(entry_price, sl_pct);

    // volume gate
    FPN<F> volume_threshold = FPN_Mul(rolling->volume_avg, config->volume_multiplier);

    // sizing — base trade size before barrier modulation
    FPN<F> trade_size = FPN_Zero<F>();
    if (!FPN_IsZero(entry_price)) {
        trade_size = FPN_DivNoAssert(allocated_balance, entry_price);
    }

    // Phase 6prep sharded c15: confidence-damped threshold. When confidence_enabled,
    // the effective entry threshold is base * (scale - conf), clamped to <= 1.0.
    // A noise-floor model (conf ≈ CONFIDENCE_MIN_IC_DEFAULT) gives effective ≈
    // base * (2.0 - 0.01) ≈ 2*base — gate stays cold. Real signal pushes conf
    // toward 1.0 → effective approaches 0 → gate fires more readily. The
    // unconditional clamp to 1.0 prevents a perverse "always blocked" state if
    // base+scale combine to >1.0 with low conf. Mirrors the legacy formula at
    // PortfolioController.hpp:~1614.
    double base_threshold = FPN_ToDouble(config->ml_buy_threshold);
    double conf_now = 0.0;
    double threshold = base_threshold;
    if (config->confidence_enabled && conf_scorer) {
        conf_now = ConfidenceScorer_Compute(conf_scorer, 0.0);  // data_age=0 (live)
        double scale = FPN_ToDouble(config->confidence_threshold_scale);
        double effective = base_threshold * (scale - conf_now);
        if (effective > 1.0) effective = 1.0;
        threshold = effective;
    }

    // gate decision: BarrierGate (continuous modulation) OR binary threshold
    FPN<F> gate_price = FPN_Zero<F>();  // default: zero-gate (no entry)

    if (config->barrier_gate_enabled) {
        BarrierGateResult bg = BarrierGate_Compute(p_peak, p_valley);
        // hard block if either: barrier says "imminent peak" OR prediction below threshold
        if (!bg.blocked && prediction >= threshold) {
            gate_price = entry_price;
            // soft modulation: scale position by gate strength [g_min, 1.0]
            trade_size = FPN_Mul(trade_size, FPN_FromDouble<F>(bg.gate));
        }
    } else {
        // legacy binary threshold path (backward compat when barrier_gate_enabled=0)
        if (prediction >= threshold) {
            gate_price = entry_price;
        }
    }

    // Phase 6prep sharded c13/c15: outparams for snapshot + drainer.
    // out_prediction is snapshotted to ctx->active_prediction at entry submit.
    // out_confidence is read by ShardedSnapshot for ML observability.
    if (out_prediction)  *out_prediction  = prediction;
    if (out_confidence)  *out_confidence  = conf_now;

    out->bg_price_threshold   = gate_price;
    out->bg_volume_threshold  = volume_threshold;
    out->sg_take_profit_price = FPN_Add(entry_price, tp_amount);
    out->sg_stop_loss_price   = FPN_Sub(entry_price, sl_amount);
    out->tp_pct               = tp_pct;
    out->sl_pct               = sl_pct;
    out->trade_size           = trade_size;
    out->strategy_id          = STRATEGY_ML;
    out->flags                = GATE_FLAG_TP_ENABLED | GATE_FLAG_SL_ENABLED;
    for (int i = 0; i < 6; ++i) out->_pad[i] = 0;
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
    const RollingStats<F, WL>* rolling_long = nullptr,
    void* model_ctx = nullptr,
    void* strategy_state = nullptr   // v5.4.0 Phase 2.x — typed-cast inside each branch
) {
    switch (strategy_id) {
        case STRATEGY_SIMPLE_DIP:
            // v5.4.0 Phase 2.1 — pass typed state through. nullptr is the
            // legacy contract (test paths, AUTO cores pre-Phase 3) and
            // produces identical numerics to pre-Phase 2.1.
            SimpleDip_BuildParameters(rolling, config, allocated_balance, out, rolling_long,
                                       (SimpleDipState<F>*)strategy_state);
            break;
        case STRATEGY_MEAN_REVERSION:
            MeanReversion_BuildParameters(rolling, config, allocated_balance, out);
            break;
        case STRATEGY_MOMENTUM:
            Momentum_BuildParameters(rolling, config, allocated_balance, out);
            break;
        case STRATEGY_EMA_CROSS:
            EmaCross_BuildParameters(rolling, config, allocated_balance, out);
            break;
        case STRATEGY_ML:
            ML_BuildParameters(rolling, rolling_long, config, allocated_balance, out, model_ctx);
            break;
        default:
            GateParameters_Init(out);
            return;
    }

    // Partial exits P.4 (2026-04-27): uniform post-dispatch cap. When
    // cfg.partial_exit_enabled=1, set GATE_FLAG_PAIR_ACTIVE on the param
    // pack so the hot path (ExecutionCore_Tick, P.2) opens both legs on
    // entry. Compute leg-B's TP percentage from cfg.tp2_mult — leg B's
    // TP is farther than leg A's by this multiplier (default 2.0 = TP2 is
    // 2× the TP1 distance). Leg B's SL is shared with leg A (set by
    // ExecutionCore_Tick on entry as live_sl_b = live_sl).
    //
    // Why here instead of inside each _BuildParameters: every strategy
    // has identical partial-exit semantics — same flag, same tp_pct_b
    // formula. Centralizing keeps the per-strategy code untouched and
    // makes future tweaks (e.g. asymmetric leg sizing) one-place changes.
    //
    // No-op when partial_exit_enabled=0: tp_pct_b stays at GateParameters_-
    // Init's zero, and GATE_FLAG_PAIR_ACTIVE is never set. Pre-P.4
    // behavior preserved exactly.
    if (config->partial_exit_enabled) {
        out->flags |= GATE_FLAG_PAIR_ACTIVE;
        // tp_pct_b = tp_pct * tp2_mult. Falls back to tp_pct (TP1 distance,
        // i.e. leg B duplicates leg A) when tp2_mult is zero (defensive —
        // strategy ought to have set tp_pct, but if not, leg B is a no-op).
        if (!FPN_IsZero(config->tp2_mult) && !FPN_IsZero(out->tp_pct)) {
            out->tp_pct_b = FPN_Mul(out->tp_pct, config->tp2_mult);
        } else {
            out->tp_pct_b = out->tp_pct;
        }
    } else {
        // Explicit clear when disabled — guarantees pre-P.4 callers
        // (or a re-used GateParameters instance) see tp_pct_b == 0 +
        // GATE_FLAG_PAIR_ACTIVE clear, regardless of prior state.
        out->flags &= ~GATE_FLAG_PAIR_ACTIVE;
        out->tp_pct_b = FPN_Zero<F>();
    }

    // v5.1.10 (Strategy P3 — runtime BUY_BLOCKED fee-floor gate):
    // After the strategy emits its TP target + post-cap leg-B math,
    // verify that TP1 leaves enough margin to clear round-trip fees.
    // If `out->tp_pct < 3 × fee_rate_taker`, set GATE_FLAG_BUY_BLOCKED
    // so the hot-path BG never fires. Catches the dynamic-TP-collapse
    // case (e.g. EMA stddev-based TP shrinking when stddev is low,
    // SimpleDip on a momentary recent_high spike) that the v5.1.3
    // boot-time warning misses (boot warning checks STATIC cfg, not
    // dynamic per-cycle TP).
    //
    // Why post-cap: tp_pct_b is derived from tp_pct, so checking tp_pct
    // alone covers both legs. If TP1 clears the floor, TP2 (which is
    // tp2_mult × TP1 ≥ TP1) will too.
    //
    // Why a gate, not a clamp: we don't want to "fix" a too-tight TP
    // by widening it — that would mask configuration mistakes. Refusing
    // to enter is the correct response.
    //
    // Logged once per cycle when the gate fires (not per tick — gate
    // is set on slow-path output, hot-path reads cached_params and
    // doesn't log). Caller sees the BUY_BLOCKED flag in pending_params
    // and can decide whether to log + how often.
    {
        FPN<F> fee_taker = !FPN_IsZero(config->fee_rate_taker)
            ? config->fee_rate_taker : config->fee_rate;
        FPN<F> three = FPN_FromDouble<F>(3.0);
        FPN<F> floor_pct = FPN_Mul(fee_taker, three);
        // out->tp_pct may be zero if strategy didn't set it (e.g.
        // STRATEGY_NONE fallthrough). Skip the gate in that case —
        // the strategy itself has already produced a no-op result.
        if (!FPN_IsZero(out->tp_pct) && FPN_LessThan(out->tp_pct, floor_pct)) {
            out->flags |= GATE_FLAG_BUY_BLOCKED;
        }
    }
}

}  // namespace tt
