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
#include "../ML_Headers/CostModel.hpp"     // v5.5.0 Class 8 — cost gate
#include "../ML_Headers/ModelInference.hpp"
#include "../ML_Headers/FeatureRegistry.hpp"  // v5.8.1b: Features_PackAll replaces ModelFeatures_Pack
#include "../ML_Headers/RollingStats.hpp"
#include "../ML_Headers/VolScaler.hpp"     // v5.5.0 Class 8 — vol scaling
#include "../MemHeaders/HealthLog.hpp"     // v5.9.0b — Health_LogCriticalRateLimited
#include "../Strategies/RegimeDetector.hpp"
#include "SimpleDip.hpp"          // SimpleDipState<F> — Phase 2.1 state-aware BuildParameters
#include "MeanReversion.hpp"      // MeanReversionState<F> — Phase 2.2 state-aware BuildParameters
#include "Momentum.hpp"           // MomentumState<F>     — Phase 2.3 state-aware BuildParameters
// v5.8.0 Phase 0: conditional include. Public release snapshots can drop
// Strategies/private/ and the build still compiles. The X-macro registry
// (v5.8.1) uses the same __has_include guard to omit EMA_CROSS from
// FOREACH_STRATEGY when private/EmaCross.hpp is missing.
#if __has_include("private/EmaCross.hpp")
#  include "private/EmaCross.hpp"   // EmaCrossState<F>     — Phase 2.4 state-aware BuildParameters
#endif
#include "StrategyInterface.hpp"

#include <cstdint>
#include <cmath>  // v5.9.0: std::isnan/isinf for prediction validation

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
    // v5.9.0b — ML observability pass-through. ML_BuildParameters writes
    // these for the entry log + ML Status panel to read. nullptr-safe
    // (legacy/test callers can omit).
    int*                model_load_failed;            // read-only: set by CoreModelZoo, observed here
    uint64_t*           last_ml_critical_log_us;      // rate-limit gate for fall-through log
    double*             out_threshold;                // ml_buy_threshold at decision time
    double*             out_effective_threshold;      // post-damping threshold used
    uint32_t*           nan_feature_events_total;     // bumped on Features_PackAll -1
    uint32_t*           nan_prediction_events_total;  // bumped on Model_Predict NaN/Inf
    // v5.9.1 (V5_9_AUDIT-#21) — ML strategy writes SHALT_LOW_CONFIDENCE here
    // when raw confidence falls below confidence_hard_block_threshold. The
    // dispatcher's strategy_halt_reason pointer is wired to this slot so the
    // GUI Strategy Halt panel + entry log can attribute the block correctly.
    uint8_t*            out_strategy_halt_reason;
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
    // v5.10.0a.G.5 — multi-horizon ensemble dispatch. nullptr default =
    // single-model path (CoreModelZoo via model_handle, existing); when
    // engine boot auto-detects N horizon siblings on disk, populates this
    // pointer to the per-core EnsembleModelZoo. ML_BuildParameters checks
    // ensemble_zoo first; if active, dispatches through Model_Predict_Ensemble;
    // else falls through to single-zoo path bytewise-identical to pre-G.5.
    void*               ensemble_zoo;        // EnsembleModelZoo<F>*  (nullptr = inactive)
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
    GateParameters<F>* out,
    MeanReversionState<F>* state = nullptr   // v5.4.0 Phase 2.2 — adaptive filter state
) {
    // v5.4.0 Phase 2.2: when state is provided (sharded path post-Phase 1),
    // use state->live_offset_pct + state->live_vol_mult + state->live_stddev_mult
    // — these are seeded from cfg at boot and adapted by MR_Adapt on cadence
    // (P&L regression-driven). When state is nullptr (legacy callers, tests),
    // fall back to cfg defaults — preserves pre-Phase 2.2 numerics.
    FPN<F> live_offset = state ? state->live_offset_pct  : config->entry_offset_pct;
    FPN<F> live_vmult  = state ? state->live_vol_mult    : config->volume_multiplier;
    FPN<F> live_smult  = state ? state->live_stddev_mult : config->offset_stddev_mult;

    // BUG FIX (v4.0.3): pre-fix used `bg_threshold = rolling->price_avg`
    // with no depth requirement — gate fired on EVERY tick price < avg
    // (statistically half of all ticks during noise). Real MR buys on
    // meaningful DIPS below mean. Now matches SimpleDip's pattern: gate
    // sits at `avg - (avg × entry_offset_pct)` so it requires a true dip.
    FPN<F> avg = rolling->price_avg;
    if (FPN_IsZero(avg)) avg = rolling->price_max;

    // v5.4.0 Phase 2.2: dual-mode entry price like the legacy MR_BuySignal.
    // pct mode: entry = avg - (avg * live_offset_pct)
    // stddev mode: entry = avg - (stddev * live_stddev_mult)
    // Mode toggle is `live_smult > 0` (cfg.offset_stddev_mult is the global
    // toggle; state mirrors at boot, may drift via Adapt within bounds).
    FPN<F> entry_price;
    if (!FPN_IsZero(live_smult)) {
        FPN<F> stddev_offset = FPN_Mul(rolling->price_stddev, live_smult);
        entry_price = FPN_Sub(avg, stddev_offset);
    } else {
        FPN<F> dip_offset = FPN_Mul(avg, live_offset);
        entry_price = FPN_Sub(avg, dip_offset);
    }

    FPN<F> tp_pct = !FPN_IsZero(config->mr_tp_pct) ? config->mr_tp_pct : config->take_profit_pct;
    FPN<F> sl_pct = !FPN_IsZero(config->mr_sl_pct) ? config->mr_sl_pct : config->stop_loss_pct;
    FPN<F> tp_amount = FPN_Mul(entry_price, tp_pct);
    FPN<F> sl_amount = FPN_Mul(entry_price, sl_pct);
    FPN<F> volume_threshold = FPN_Mul(rolling->volume_avg, live_vmult);

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
    GateParameters<F>* out,
    MomentumState<F>* state = nullptr   // v5.4.0 Phase 2.3 — adaptive breakout state
) {
    // v5.4.0 Phase 2.3: when state is provided (sharded path post-Phase 1),
    // use state->live_breakout_mult + state->live_vol_mult — these are
    // seeded from cfg at boot and adapted by Momentum_Adapt on cadence.
    // When state is nullptr (legacy callers, tests), fall back to cfg
    // defaults — preserves pre-Phase 2.3 numerics. Note that the legacy
    // sharded stub used entry_offset_pct (a percentage); state-aware path
    // uses momentum_breakout_mult (a stddev multiplier), which is the
    // legacy single-core convention.
    FPN<F> live_breakout = state ? state->live_breakout_mult : FPN_Zero<F>();
    FPN<F> live_vmult    = state ? state->live_vol_mult      : config->volume_multiplier;

    // BUG FIX (v4.0.3): same family as MR — pre-fix used `bg_threshold = avg`
    // with no breakout depth. Gate fired on every tick price > avg (with the
    // BUY_ABOVE flag). Real momentum buys on confirmed BREAKOUTS above the
    // mean, not infinitesimal upticks. Now requires a breakout of
    // `entry_offset_pct` above the rolling mean before the gate arms.
    FPN<F> avg = rolling->price_avg;
    if (FPN_IsZero(avg)) avg = rolling->price_max;
    // v5.4.0 Phase 2.3: stddev * live_breakout_mult when state provides it
    // (matches legacy Momentum_BuySignal: avg + stddev * breakout_mult). When
    // state is null OR live_breakout is zero, fall back to entry_offset_pct
    // (the pre-Phase 2.3 sharded behavior).
    FPN<F> breakout_offset;
    if (state && !FPN_IsZero(live_breakout) && !FPN_IsZero(rolling->price_stddev)) {
        breakout_offset = FPN_Mul(rolling->price_stddev, live_breakout);
    } else {
        breakout_offset = FPN_Mul(avg, config->entry_offset_pct);
    }
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
    // v5.4.0 Phase 2.3: live_vmult from state when present (adaptive),
    // else cfg.volume_multiplier (legacy).
    FPN<F> volume_threshold = FPN_Mul(rolling->volume_avg, live_vmult);

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
    GateParameters<F>* out,
    EmaCrossState<F>* state = nullptr   // v5.4.0 Phase 2.4 — EMA tracking state
) {
    // v5.4.0 Phase 2.4: when state is provided (sharded path post-Phase 1),
    // use state->prev_ema (set by Adapt from the producer's per-tick EMA)
    // as the dip reference + crossover anchor. Mirrors legacy
    // EmaCross_BuySignal:
    //   ref = ema (or rolling avg fallback)
    //   crossover = ref > short_sma (by emacross_crossover_min stddevs)
    //   buy_price = ref - stddev * emacross_dip_mult
    //   gate zeroed when crossover not confirmed (no entries during downtrend).
    //
    // When state is nullptr (legacy callers, tests), keep the pre-Phase 2.4
    // SimpleDip-with-overrides behavior so existing snapshots/tests stay
    // numerically identical.
    if (!state) {
        SimpleDip_BuildParameters(rolling, config, allocated_balance, out);
        out->strategy_id = STRATEGY_EMA_CROSS;
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
        return;
    }

    // State-aware path. ref = state->prev_ema (Adapt populated this from
    // the producer's per-tick EMA replication). Fall back to rolling avg
    // if the state hasn't been seeded yet (cold-start, ema=0).
    FPN<F> ref = !FPN_IsZero(state->prev_ema) ? state->prev_ema : rolling->price_avg;

    // Crossover gate: ref must sit above short SMA by at least
    // emacross_crossover_min stddevs. Same geometry as the legacy
    // EmaCross_BuySignal.
    int uptrend = 0;
    if (!FPN_IsZero(rolling->price_avg) && !FPN_IsZero(rolling->price_stddev)) {
        FPN<F> diff = FPN_Sub(ref, rolling->price_avg);
        int ema_above = (diff.sign == 0) && !FPN_IsZero(diff);
        FPN<F> spread_stddevs = FPN_DivNoAssert(diff, rolling->price_stddev);
        uptrend = ema_above & FPN_GreaterThan(spread_stddevs, config->emacross_crossover_min);
    }

    // Buy price = ref - stddev * emacross_dip_mult (dip below EMA)
    FPN<F> dip = FPN_Mul(rolling->price_stddev, config->emacross_dip_mult);
    FPN<F> entry_price = FPN_Sub(ref, dip);

    // TP/SL: use EMA-specific cfg overrides; fall through to shared.
    FPN<F> tp_pct = !FPN_IsZero(config->emacross_tp_pct)
        ? config->emacross_tp_pct : config->take_profit_pct;
    FPN<F> sl_pct = !FPN_IsZero(config->emacross_sl_pct)
        ? config->emacross_sl_pct : config->stop_loss_pct;

    FPN<F> tp_amount = FPN_Mul(entry_price, tp_pct);
    FPN<F> sl_amount = FPN_Mul(entry_price, sl_pct);
    FPN<F> volume_threshold = FPN_Mul(rolling->volume_avg, config->volume_multiplier);

    FPN<F> trade_size = FPN_Zero<F>();
    if (!FPN_IsZero(entry_price)) {
        trade_size = FPN_DivNoAssert(allocated_balance, entry_price);
    }

    out->bg_price_threshold   = uptrend ? entry_price : FPN_Zero<F>();
    out->bg_volume_threshold  = volume_threshold;
    out->sg_take_profit_price = FPN_Add(entry_price, tp_amount);
    out->sg_stop_loss_price   = FPN_Sub(entry_price, sl_amount);
    out->tp_pct               = tp_pct;
    out->sl_pct               = sl_pct;
    out->trade_size           = trade_size;
    out->strategy_id          = STRATEGY_EMA_CROSS;
    out->flags                = GATE_FLAG_TP_ENABLED | GATE_FLAG_SL_ENABLED;
    for (int i = 0; i < 6; ++i) out->_pad[i] = 0;
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
        // v5.9.0b — emit rate-limited CRITICAL log so the operator sees
        // ML→SimpleDip fall-through (V5_9_AUDIT-#2). Pre-v5.9.0b this
        // was silent; now visible in health log.
        if (mctx && mctx->last_ml_critical_log_us) {
            int load_failed = mctx->model_load_failed && *mctx->model_load_failed;
            tt::Health_LogCriticalRateLimited(
                mctx->last_ml_critical_log_us,
                /*gate_us=*/60000000ULL,  // 60s per-core gate
                /*core=*/-1,  // ML_BuildParameters doesn't have core_id; -1 = unknown
                "ml",
                "ML→SimpleDip fall-through: %s (zoo=%s)",
                load_failed ? "model load failed" : "no model configured",
                zoo ? "empty" : "null");
        }
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

    // pack features once — used by whichever model role is loaded.
    // v5.8.1b: registry-driven via Features_PackAll. Same contract as the
    // legacy ModelFeatures_Pack (validated bytewise in EXTENSIBILITY tests).
    float features[MODEL_MAX_FEATURES];
    FeatureComputeCtx<F> ctx{};
    ctx.signals       = &sig;
    ctx.short_rolling = rolling;
    int n = Features_PackAll(&ctx, features);
    // v5.9.0 — NaN/Inf in feature pack → fall through to SimpleDip.
    // Features_PackAll returns -1 sentinel; never feed garbage to XGBoost.
    // v5.9.0b — bump per-core NaN counter + emit rate-limited CRITICAL.
    if (n < 0) {
        if (mctx && mctx->nan_feature_events_total) {
            (*mctx->nan_feature_events_total)++;
        }
        if (mctx && mctx->last_ml_critical_log_us) {
            tt::Health_LogCriticalRateLimited(
                mctx->last_ml_critical_log_us, 60000000ULL, -1, "ml",
                "NaN/Inf in feature pack — fall-through to SimpleDip "
                "(nan_feature_events_total=%u)",
                mctx->nan_feature_events_total ? *mctx->nan_feature_events_total : 0);
        }
        SimpleDip_BuildParameters(rolling, config, allocated_balance, out, rolling_long);
        out->strategy_id = STRATEGY_ML;
        return;
    }

    // run inference, prefer 3-class barrier model when available
    double prediction = 0.5;   // neutral fallback
    double p_peak     = 0.5;
    double p_valley   = 0.5;
    int have_signal   = 0;

    if (zoo->loaded_mask & CORE_MODEL_BARRIER) {
        // v5.9.3b — apply scaler associated with barrier role. Identity
        // no-op when zoo->barrier.scaler.has_scaler=0 (legacy or absent).
        if (tt::FeatureStandardizer_Apply(&zoo->barrier.scaler, features, n) < 0) {
            fprintf(stderr, "[ML] dispatch: NaN/Inf post-scaler (barrier) — no signal\n");
            if (mctx && mctx->nan_feature_events_total) {
                (*mctx->nan_feature_events_total)++;
            }
            SimpleDip_BuildParameters(rolling, config, allocated_balance, out, rolling_long);
            out->strategy_id = STRATEGY_ML;
            return;
        }
        // 3-class softmax: [0]=stable, [1]=peak, [2]=valley
        float multi[3] = {0.0f, 0.0f, 0.0f};
        int got = Model_PredictMulti(&zoo->barrier, features, n, multi, 3);
        if (got >= 3) {
            // v5.9.0 — NaN/Inf in multiclass output → no signal (silent
            // miss class). XGBoost can return NaN on pathological inputs.
            if (std::isnan(multi[0]) || std::isinf(multi[0]) ||
                std::isnan(multi[1]) || std::isinf(multi[1]) ||
                std::isnan(multi[2]) || std::isinf(multi[2])) {
                fprintf(stderr, "[ML] dispatch: barrier multi-prediction NaN/Inf — no signal\n");
            } else {
                p_peak     = multi[1];
                p_valley   = multi[2];
                // entry signal = "valley imminent" — primary trade trigger
                prediction = p_valley;
                have_signal = 1;
            }
        } else if (got == 1) {
            // model was actually binary (mis-labeled as barrier role) — still usable
            if (std::isnan(multi[0]) || std::isinf(multi[0])) {
                fprintf(stderr, "[ML] dispatch: barrier-as-binary prediction NaN/Inf — no signal\n");
            } else {
                prediction = multi[0];
                p_peak     = 1.0 - prediction;
                p_valley   = prediction;
                have_signal = 1;
            }
        }
    } else if (zoo->loaded_mask & CORE_MODEL_BUY_SIGNAL) {
        // v5.9.3b — apply scaler associated with buy_signal role.
        if (tt::FeatureStandardizer_Apply(&zoo->buy_signal.scaler, features, n) < 0) {
            fprintf(stderr, "[ML] dispatch: NaN/Inf post-scaler (buy_signal) — no signal\n");
            if (mctx && mctx->nan_feature_events_total) {
                (*mctx->nan_feature_events_total)++;
            }
            SimpleDip_BuildParameters(rolling, config, allocated_balance, out, rolling_long);
            out->strategy_id = STRATEGY_ML;
            return;
        }
        // v5.10.0a.G.5 — ensemble dispatch when active. Uses
        // Model_Predict_Ensemble (G.4 selection logic — argmax-confidence)
        // until G.7 ships weighted blend variant.
        // ensemble_zoo->buy_signal[] all share the SAME scaler (per G.3
        // load-from-cfg invariant), so the standardize step above already
        // produced the correct features for every horizon (G.7 caching
        // optimization).
        double pred_raw = 0.0;
        EnsembleModelZoo<F>* ezoo = (EnsembleModelZoo<F>*)
            (mctx ? mctx->ensemble_zoo : nullptr);
        if (ezoo && ezoo->active && ezoo->buy_signal_count > 0) {
            int dominant_idx = -1;
            pred_raw = (double)Model_Predict_Ensemble(
                ezoo->buy_signal, ezoo->buy_signal_count,
                features, n, &dominant_idx);
        } else {
            // Single-zoo path (existing; bytewise unchanged from pre-G.5)
            pred_raw = (double)Model_Predict(&zoo->buy_signal, features, n);
        }
        if (std::isnan(pred_raw) || std::isinf(pred_raw)) {
            fprintf(stderr, "[ML] dispatch: buy_signal prediction NaN/Inf — no signal\n");
        } else {
            prediction = pred_raw;
            p_peak     = 1.0 - prediction;
            p_valley   = prediction;
            have_signal = 1;
        }
    }

    // if inference failed (no model loaded, or NaN/Inf prediction),
    // fall back to SimpleDip
    if (!have_signal) {
        // v5.9.0b — bump prediction-NaN counter + emit rate-limited
        // CRITICAL when fall-through is due to NaN (vs no model loaded).
        // The earlier NaN-fall-through paths above already log; this
        // catches the case where ALL prediction attempts produced NaN.
        if (mctx && mctx->nan_prediction_events_total) {
            (*mctx->nan_prediction_events_total)++;
        }
        if (mctx && mctx->last_ml_critical_log_us) {
            tt::Health_LogCriticalRateLimited(
                mctx->last_ml_critical_log_us, 60000000ULL, -1, "ml",
                "no signal — all model predictions NaN/Inf or model unloaded "
                "(nan_prediction_events_total=%u)",
                mctx->nan_prediction_events_total ? *mctx->nan_prediction_events_total : 0);
        }
        SimpleDip_BuildParameters(rolling, config, allocated_balance, out, rolling_long);
        out->strategy_id = STRATEGY_ML;
        return;
    }

    // v5.9.0b — record threshold + effective_threshold for entry log + ML
    // Status panel display. Threshold is read from cfg; effective is
    // post-confidence-damping (computed below in the gate decision).
    double ml_threshold_d = (double)FPN_ToDouble(config->ml_buy_threshold);
    if (mctx && mctx->out_threshold) *mctx->out_threshold = ml_threshold_d;

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
    // v5.9.0b — surface the effective threshold for entry log + ML Status
    // panel. Same value the gate decision uses below; single source of truth.
    if (mctx && mctx->out_effective_threshold) *mctx->out_effective_threshold = threshold;

    // v5.9.1 (V5_9_AUDIT-#21) — confidence hard-floor. Damping alone can
    // produce a pathological "always near zero" effective threshold when
    // confidence is in the noise floor; entries fire on essentially-random
    // predictions. Hard-block when raw confidence is below the operator-
    // configured floor. Default 0.0 = disabled (preserves pre-v5.9.1).
    double hard_floor = FPN_ToDouble(config->confidence_hard_block_threshold);
    if (config->confidence_enabled && hard_floor > 0.0 && conf_now < hard_floor) {
        out->bg_price_threshold   = FPN_Zero<F>();
        out->bg_volume_threshold  = FPN_Zero<F>();
        out->sg_take_profit_price = FPN_Zero<F>();
        out->sg_stop_loss_price   = FPN_Zero<F>();
        out->tp_pct               = FPN_Zero<F>();
        out->sl_pct               = FPN_Zero<F>();
        out->trade_size           = FPN_Zero<F>();
        out->strategy_id          = STRATEGY_ML;
        out->flags                = GATE_FLAG_BUY_BLOCKED;
        for (int i = 0; i < 6; ++i) out->_pad[i] = 0;
        if (mctx && mctx->out_strategy_halt_reason)
            *mctx->out_strategy_halt_reason = SHALT_LOW_CONFIDENCE;
        if (out_prediction) *out_prediction = prediction;
        if (out_confidence) *out_confidence = conf_now;
        return;
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
    void* strategy_state = nullptr,  // v5.4.0 Phase 2.x — typed-cast inside each branch
    uint8_t* strategy_halt_reason = nullptr  // v5.6.2 — when non-null, dispatcher
                                              // writes SHALT_* codes for BUY_BLOCKED
                                              // paths (fee-floor, cost-gate) and a
                                              // post-pass for strategy zero-gates
                                              // that didn't set a specific code.
                                              // Caller resets to SHALT_OK before call.
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
            // v5.4.0 Phase 2.2 — pass typed MR state through.
            MeanReversion_BuildParameters(rolling, config, allocated_balance, out,
                                           (MeanReversionState<F>*)strategy_state);
            break;
        case STRATEGY_MOMENTUM:
            // v5.4.0 Phase 2.3 — pass typed Momentum state through.
            Momentum_BuildParameters(rolling, config, allocated_balance, out,
                                      (MomentumState<F>*)strategy_state);
            // v5.7.5 — MOM quality filters. Each gated cfg-side (default
            // 0/off, preserving pre-v5.7 behavior). Operator opts in
            // after observing v5.7.6 quality dashboard data. Each filter
            // sets BUY_BLOCKED + a SHALT_MOM_* code visible in the v5.6
            // GUI. Order: TP-margin first (cheapest), then R², then
            // flow, then last-lost — first SHALT wins per the SHALT_OK
            // guard pattern so the most-load-bearing reason surfaces.
            if (rolling) {
                bool blocked = false;
                // Filter 1: minimum TP margin. Catches MOM trades where
                // the strategy's tp_pct is too thin to meaningfully clear
                // fees + slippage even after the dispatcher's fee-floor
                // (v5.1.10) passes — the dispatcher's check requires
                // 3 * fee, this filter enforces a stricter operator-set
                // floor (recommended 0.40%).
                if (!FPN_IsZero(config->momentum_min_tp_margin_pct) &&
                    !FPN_IsZero(out->tp_pct) &&
                    FPN_LessThan(out->tp_pct, config->momentum_min_tp_margin_pct)) {
                    out->flags |= GATE_FLAG_BUY_BLOCKED;
                    blocked = true;
                    if (strategy_halt_reason && *strategy_halt_reason == SHALT_OK) {
                        *strategy_halt_reason = SHALT_MOM_TP_TOO_TIGHT;
                    }
                }
                // Filter 2: R² minimum (short-window regression fit).
                // Low R² = noisy data = breakout is probably noise.
                if (!blocked && !FPN_IsZero(config->momentum_min_r2) &&
                    FPN_LessThan(rolling->price_r_squared, config->momentum_min_r2)) {
                    out->flags |= GATE_FLAG_BUY_BLOCKED;
                    blocked = true;
                    if (strategy_halt_reason && *strategy_halt_reason == SHALT_OK) {
                        *strategy_halt_reason = SHALT_MOM_LOW_R2;
                    }
                }
                // Filter 3: recent flow agreement (volume_delta).
                // Reject when recent flow opposes the breakout direction.
                if (!blocked && !FPN_IsZero(config->momentum_min_buy_delta_recent) &&
                    FPN_LessThan(rolling->volume_delta,
                                  config->momentum_min_buy_delta_recent)) {
                    out->flags |= GATE_FLAG_BUY_BLOCKED;
                    blocked = true;
                    if (strategy_halt_reason && *strategy_halt_reason == SHALT_OK) {
                        *strategy_halt_reason = SHALT_MOM_NO_FLOW;
                    }
                }
                // Filter 4: require last entry was a TP win. Aggressive —
                // disabled by default. Strategy-state owns the
                // last_entry_won flag (set in MomentumState by the
                // drainer post-fill); without state we can't enforce.
                // Reserved hook; full wiring needs MomentumState extension
                // and is deferred to a follow-up if measurement justifies.
                // (Keeping the cfg field + SHALT code defined now so the
                // table stays stable.)
                (void)config->momentum_require_last_win;  // not yet wired
            }
            break;
        case STRATEGY_EMA_CROSS:
            // v5.4.0 Phase 2.4 — pass typed EmaCross state through.
            EmaCross_BuildParameters(rolling, config, allocated_balance, out,
                                      (EmaCrossState<F>*)strategy_state);
            break;
        case STRATEGY_ML:
            ML_BuildParameters(rolling, rolling_long, config, allocated_balance, out, model_ctx);
            break;
        default:
            GateParameters_Init(out);
            // v5.6.2: STRATEGY_NONE / unknown strategy → no signal.
            // Early return bypasses the post-pass; set SHALT here.
            if (strategy_halt_reason && *strategy_halt_reason == SHALT_OK) {
                *strategy_halt_reason = SHALT_NO_SIGNAL;
            }
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
            // v5.6.2: SHALT visibility. Operator sees "blocked: fee-floor"
            // in the GUI Status column instead of plain "blocked" or
            // "READY".
            if (strategy_halt_reason && *strategy_halt_reason == SHALT_OK) {
                *strategy_halt_reason = SHALT_FEE_FLOOR;
            }
        }
    }

    // v5.5.0 (recurring-bugs Class 8): cost gate + vol scaling.
    // Both flags default off; opt-in via cfg. Defaults preserve pre-v5.5
    // numerics exactly.
    //
    // CostModel: estimate trade cost (timing + impact). When cost exceeds
    // a fraction of expected gain (tp_pct), set BUY_BLOCKED. Spread-cost
    // contribution is omitted in this first port — sharded path doesn't
    // yet plumb live spread bps to BuildParameters; timing + impact alone
    // is the conservative approximation. Future: thread spread_bps from
    // CoreSlowState::spread_state.
    if (config->cost_gate_enabled && !FPN_IsZero(out->tp_pct) &&
        !FPN_IsZero(out->bg_price_threshold) && rolling) {
        double price = FPN_ToDouble(rolling->price_avg);
        double rel_vol = (price > 0.01)
            ? FPN_ToDouble(rolling->price_stddev) / price : 0.0;
        if (rel_vol > 0.0) {
            // order size and ADV in dollars. ADV approximated as
            // volume_avg × price × 1440 (tick-rate samples-per-day proxy).
            double order_size_d = FPN_ToDouble(out->trade_size) *
                                  FPN_ToDouble(out->bg_price_threshold);
            double adv_d = FPN_ToDouble(rolling->volume_avg) * price * 1440.0;
            TradingCosts c = CostModel_Estimate(
                /*spread_bps=*/0.0,    // omitted — see comment above
                rel_vol,
                /*horizon_minutes=*/5.0,
                order_size_d, adv_d,
                COST_K1_DEFAULT, COST_K2_DEFAULT, COST_K3_DEFAULT);
            // Veto when total cost exceeds 50% of expected gain (tp_bps).
            // Conservative threshold; can be made cfg-tunable later.
            double tp_bps = FPN_ToDouble(out->tp_pct) * 10000.0;
            if (c.total_cost > tp_bps * 0.5) {
                out->flags |= GATE_FLAG_BUY_BLOCKED;
                // v5.6.2: SHALT visibility for cost-gate veto.
                if (strategy_halt_reason && *strategy_halt_reason == SHALT_OK) {
                    *strategy_halt_reason = SHALT_COST_GATE;
                }
            }
        }
    }

    // VolScaler: shrink trade_size in high-volatility regimes. Uses
    // alpha=tp_pct and vol=stddev/price; weight in [0, 1] that scales
    // the existing trade_size down (never up — never increases risk).
    if (config->foxml_vol_scaling_enabled && !FPN_IsZero(out->trade_size) &&
        !FPN_IsZero(out->tp_pct) && rolling) {
        double price = FPN_ToDouble(rolling->price_avg);
        double rel_vol = (price > 0.01)
            ? FPN_ToDouble(rolling->price_stddev) / price : 0.0;
        if (rel_vol > 0.0) {
            double alpha = FPN_ToDouble(out->tp_pct);
            double z_max = !FPN_IsZero(config->foxml_vol_scaling_z_max)
                ? FPN_ToDouble(config->foxml_vol_scaling_z_max)
                : VOL_SCALER_Z_MAX_DEFAULT;
            // max_weight=1.0 → VolScaler returns weight in [0, 1] which we
            // multiply trade_size by. Effectively: weight=1 means no
            // change; weight<1 means smaller position in high-vol regime.
            double weight = VolScaler_Size(alpha, rel_vol, z_max, /*max_weight=*/1.0);
            if (weight > 0.0 && weight < 1.0) {
                out->trade_size = FPN_Mul(out->trade_size,
                                           FPN_FromDouble<F>(weight));
            }
        }
    }

    // v5.6.2: SHALT post-pass. If the strategy zero-gated bg_price_threshold
    // for a strategy-internal reason (uptrend not confirmed, no mean-reversion
    // signal, etc) and didn't set BUY_BLOCKED via the dispatcher's fee-floor
    // or cost-gate paths, no specific SHALT code is set yet. Mark as
    // SHALT_NO_SIGNAL so the GUI shows "off: no-signal" instead of plain "off".
    //
    // A future ship per-strategy will assign more specific codes
    // (SHALT_NO_UPTREND, SHALT_NO_MEAN_REV, SHALT_NO_BREAKOUT, etc) by
    // having strategies take a SHALT pointer too. For now, no-signal
    // is a strict improvement over the silent "off" state.
    if (strategy_halt_reason && *strategy_halt_reason == SHALT_OK &&
        FPN_IsZero(out->bg_price_threshold) &&
        !(out->flags & GATE_FLAG_BUY_BLOCKED)) {
        *strategy_halt_reason = SHALT_NO_SIGNAL;
    }
}

}  // namespace tt
