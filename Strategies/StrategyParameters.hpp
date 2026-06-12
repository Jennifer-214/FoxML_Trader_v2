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
// its inputs (RollingStats, core_cfg, allocated_balance). No globals, no
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
#include "../CoreFrameworks/SlowPathGateRegistry.hpp"  // v5.14.9.B.0 — FOREACH_SLOW_PATH_GATE + MASK_* + BITMAP_IS_SET
// Note: STATE_FLAG_LADDER_BOTTOM_HIT is set in ShardedSnapshot copy via
// inference (gate_state.LADDER_ACTIVE && last_confidence_factor == 0.0)
// — not set directly here. No PerCoreStateFlagsRegistry include needed.
#include "../FixedPoint/FixedPointN.hpp"
#include "../ML_Headers/BarrierGate.hpp"
#include "../ML_Headers/ConfidenceScore.hpp"
#include "../ML_Headers/RollingTurnover.hpp"  // v5.14.1.G — portfolio turnover populator
#include <ctime>  // v5.14.1.B — clock_gettime for composite confidence freshness
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
    // v5.13.0.B — sell-side ML prediction. ML_BuildParameters writes the
    // blended exit_predictor probability here when BITMAP_IS_SET(cfg.ml_cfg_flags, MASK_ML_CFG_USE_EXIT_MODEL) &&
    // ezoo->exit_predictor_count > 0. Slow-path body post-RebuildOneCore
    // reads + acts on it (fires OMS submit if above cfg.exit_threshold and
    // any positions are open). nullptr-safe: legacy callers without sell-
    // side wiring leave this null; ML_BuildParameters skips the inference.
    double*             out_exit_prediction;
    int*                out_exit_dominant_horizon;
    // v5.15.5.A.6 — buy-side per-horizon barrier dispatch observability.
    // Written by ML_BuildParameters when per-horizon barrier feature
    // active. Mirrors exit-side pattern.
    int*                out_buy_dominant_horizon;       // -1 = no dispatch this cycle
    uint8_t*            out_barrier_mode_used;          // FOREACH_BARRIER_BLEND_MODE enum
    uint32_t*           barrier_shadow_event_count;     // incremented on shadow ring write
    // v5.9.0b — ML observability pass-through. ML_BuildParameters writes
    // these for the entry log + ML Status panel to read. nullptr-safe
    // (legacy/test callers can omit).
    // v5.15.5.B.3 — `model_load_failed` migrated from `int*` to plain
    // `int`-by-value. The bitmap bit on CoreContext (core_state_flags bit
    // MASK_CORE_STATE_MODEL_LOAD_FAILED) is not addressable, so the call
    // site copies-in the current bit state. Read-only field from ML body's
    // perspective; semantics preserved (0 = no failure; non-zero = failure).
    int                 model_load_failed;            // read-only: BIT state at call time (0 = ok, 1 = load failed)
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
    void*               ema_price;       // const FPN_Binary<F>*
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
    double              current_spread;     // BookSnapshot::spread (FPN_Binary→double)
    double              current_mid_price;  // BookSnapshot::mid_price (FPN_Binary→double)
    // v5.10.0a.G.5 — multi-horizon ensemble dispatch. nullptr default =
    // single-model path (CoreModelZoo via model_handle, existing); when
    // engine boot auto-detects N horizon siblings on disk, populates this
    // pointer to the per-core EnsembleModelZoo. ML_BuildParameters checks
    // ensemble_zoo first; if active, dispatches through Model_Predict_Ensemble;
    // else falls through to single-zoo path bytewise-identical to pre-G.5.
    void*               ensemble_zoo;        // EnsembleModelZoo<F>*  (nullptr = inactive)
    // v5.10.0a.G.7 — current regime classification. Used by ensemble
    // weighted-blend dispatch to select the correct per-regime bandit's
    // weights. Updated by slow-path regime classifier (RegimeDetector)
    // before each ML_BuildParameters call. Default 0 = REGIME_RANGING
    // (safe fallback when classifier hasn't run yet).
    int                 current_regime_id;
    // v5.11.18 main — per-core feature mask. Threaded from
    // ControllerConfig::core_feature_mask[core_id] (set up in
    // v5.11.18a's cfg parser). When non-null, Features_PackAll
    // checks the bit-per-feature mask and writes 0.0f to the
    // corresponding output slot for any unset bit (sparse-zero,
    // not dense compression — caller still gets
    // NUM_REGISTERED_FEATURES floats out, but unset features are
    // zeroed before compute). nullptr = legacy path = no masking
    // (every feature computed normally; bytewise-identical to
    // pre-v5.11.18 builds).
    //
    // Stamp parity: when this points to a non-default mask, the
    // model's stamp body MUST carry has_feature_mask=1 +
    // feature_mask_train matching. v5.11.18a's stamp_write +
    // verify_model_stamp pipeline already supports this; load-time
    // refusal fires on mismatch.
    const uint64_t*     feature_mask;

    // v5.14.1.G — portfolio turnover diagnostic state. Pointer to
    // CoreContext.turnover (per-core EventLoopState; sharded-only).
    // ML_BuildParameters populates the ring with top-K weights mask
    // each cycle when non-null. nullptr = legacy path / non-ML cores
    // (no-op). Surfaced via PerCoreSnap.ml_portfolio_turnover.
    void*               turnover_state;  // RollingTurnover* (void* avoids include cycle)
    int                 turnover_topk;   // matched to cfg.confidence_turnover_topk at boot

    // v5.14.9.B — soft risk degradation ladder factor observability.
    // ML_BuildParameters writes the per-cycle ladder factor (composite
    // confidence × degradation curve) here when non-null. 1.0 = full
    // size; (0, 1) = soft scale; 0.0 = ladder bottom (entry blocked
    // with SHALT_LOW_CONFIDENCE). nullptr-safe: legacy/test callers
    // can omit. Read by ML Status panel + PerCoreSnap.ml_confidence_factor.
    double*             out_confidence_factor;
    // v5.14.9.B.0 — pointer to the per-core slow-path gate cache
    // (FOREACH_SLOW_PATH_GATE PER_CORE entries). Populated by the
    // slow-path caller after ControllerConfig_ResolveForCore +
    // SLOW_PATH_GATE_AUTOPOPULATE_PER_CORE; ML_BuildParameters reads
    // gate predicates via BITMAP_IS_SET(gate_state->flags, MASK_<NAME>).
    // nullptr-safe: legacy/test callers without slow-path setup can
    // omit (use sites fall back to inline cfg reads when null).
    // void* (not SlowPathGateState*) avoids an include cycle —
    // ML_BuildParameters casts at use sites.
    void*               gate_state;  // SlowPathGateState* — cast at use site
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
inline bool Strategy_SpacingOk(Money proposed_entry,
                                Money last_entry,
                                const RollingStats<F, W>* rolling,
                                const PerCoreCfg<F>* core_cfg) {
    // No prior entry → spacing irrelevant.
    if (Money_IsZero(last_entry)) return true;
    // Spacing disabled or zero stddev → can't compute meaningful threshold.
    if (FPN_IsZero(core_cfg->spacing_multiplier)) return true;
    if (FPN_IsZero(rolling->price_stddev))      return true;
    // Required min distance between entries
    FPN_Binary<F> min_dist = FPN_Mul(rolling->price_stddev, core_cfg->spacing_multiplier);
    // |proposed - last|
    Money diff = Money_Ge(proposed_entry, last_entry)
        ? Money_Sub(proposed_entry, last_entry)
        : Money_Sub(last_entry, proposed_entry);
    return Money_Ge(diff, Money_FromBinary(min_dist));  // feature-spacing vs money distance
}

template <unsigned F>
inline Money Strategy_TpFloor(Money entry_price,
                                Money tp_amount,
                                const PerCoreCfg<F>* core_cfg) {
    if (Money_IsZero(core_cfg->fee_floor_mult) || Money_IsZero(core_cfg->fee_rate))
        return tp_amount;
    // Required floor = entry × fee_rate × fee_floor_mult
    Money fee_per_side = Money_Mul(entry_price, core_cfg->fee_rate);
    Money floor = Money_Mul(fee_per_side, core_cfg->fee_floor_mult);
    return Money_Ge(tp_amount, floor) ? tp_amount : floor;
}

//======================================================================================================
// [SIMPLEDIP — full port]
//======================================================================================================
//
// SimpleDip buys when price drops `entry_offset_pct` below the recent high
// in the rolling window. TP and SL are fixed percentage offsets from the
// expected entry price (core_cfg: take_profit_pct, stop_loss_pct).
//
// inputs read from rolling stats:
//   - price_max:  recent high used as the dip reference
//   - volume_avg: rolling mean volume, used for the volume gate threshold
//
// inputs read from core_cfg:
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
// ───────── H22 single-source per-fill TP/SL pct (.E.0.10 A1) ─────────
// Effective per-fill TP/SL pct = the strategy override (simpledip/mr/emacross_*_pct)
// ?: the shared take_profit_pct / stop_loss_pct. ONE source for BOTH the fresh-entry
// dispatcher (compile-time strategy id → the switch folds; slow-path, H7/H20-clean) and
// snapshot restore (runtime resolved_strategy_id; boot-time-only, the H20 exception), so a
// restored position exits at the SAME TP/SL it had while live. (A1 fix: restore previously
// read the GLOBAL take_profit_pct, dropping the per-node override → SimpleDip/MR/EmaCross
// positions survived a warm-restart at the wrong exit price.) Templated on the cfg view so
// PerCoreCfg<F> (dispatcher) and the resolved ControllerConfig<F> (restore) both call it.
// MOMENTUM → flat global (no override field); ML → global (its TP is the barrier blend at
// entry, unreproducible at restore — a separate tracked sibling, NOT closed by A1).
template <typename CfgT>
inline Money ResolvePerFillTpPct(uint8_t strategy_id, const CfgT& cfg) {
    switch (strategy_id) {
        case STRATEGY_SIMPLE_DIP:
            return !Money_IsZero(cfg.simpledip_tp_pct) ? cfg.simpledip_tp_pct : cfg.take_profit_pct;
        case STRATEGY_MEAN_REVERSION:
            return !Money_IsZero(cfg.mr_tp_pct) ? cfg.mr_tp_pct : cfg.take_profit_pct;
#if __has_include("private/EmaCross.hpp")
        case STRATEGY_EMA_CROSS:
            return !Money_IsZero(cfg.emacross_tp_pct) ? cfg.emacross_tp_pct : cfg.take_profit_pct;
#endif
        default:  // MOMENTUM (flat) / ML (blend at entry; restore→global) / AUTO / NONE
            return cfg.take_profit_pct;
    }
}
template <typename CfgT>
inline Money ResolvePerFillSlPct(uint8_t strategy_id, const CfgT& cfg) {
    switch (strategy_id) {
        case STRATEGY_SIMPLE_DIP:
            return !Money_IsZero(cfg.simpledip_sl_pct) ? cfg.simpledip_sl_pct : cfg.stop_loss_pct;
        case STRATEGY_MEAN_REVERSION:
            return !Money_IsZero(cfg.mr_sl_pct) ? cfg.mr_sl_pct : cfg.stop_loss_pct;
#if __has_include("private/EmaCross.hpp")
        case STRATEGY_EMA_CROSS:
            return !Money_IsZero(cfg.emacross_sl_pct) ? cfg.emacross_sl_pct : cfg.stop_loss_pct;
#endif
        default:
            return cfg.stop_loss_pct;
    }
}

template <unsigned F, unsigned W = 128, unsigned WL = 512>
inline void SimpleDip_BuildParameters(
    const RollingStats<F, W>* rolling,
    const PerCoreCfg<F>* core_cfg,
    Money allocated_balance,
    GateParameters<F>* out,
    const RollingStats<F, WL>* rolling_long = nullptr,
    SimpleDipState<F>* state = nullptr
) {
    // v5.4.0 Phase 2.1 — when a per-core SimpleDipState is supplied (sharded
    // production path post-Phase 1), publish recent_high into it so the
    // legacy `_BuySignal` contract is preserved (state is the source of truth
    // for "where the dip reference is"). Tests + legacy paths still pass
    // nullptr and get the inline behavior — same numerics as pre-Phase 2.1.
    FPN_Binary<F> recent_high = rolling->price_max;
    if (rolling_long && FPN_GreaterThan(rolling_long->price_max, recent_high)) {
        recent_high = rolling_long->price_max;
    }
    if (state) {
        state->recent_high = recent_high;
    }

    // expected_entry = recent_high * (1 - entry_offset_pct)
    Money high_m = Money_FromBinary(recent_high);  // D-170: ONE feature->money ingress per build
    Money dip_offset = Money_Mul(high_m, core_cfg->entry_offset_pct);
    Money expected_entry = Money_Sub(high_m, dip_offset);

    // Per-strategy TP/SL via the H22 single-source resolver (shared with snapshot restore — A1).
    Money tp_pct = ResolvePerFillTpPct(STRATEGY_SIMPLE_DIP, *core_cfg);
    Money sl_pct = ResolvePerFillSlPct(STRATEGY_SIMPLE_DIP, *core_cfg);

    // tp = expected_entry * (1 + tp_pct)
    Money tp_amount = Money_Mul(expected_entry, tp_pct);
    Money take_profit_price = Money_Add(expected_entry, tp_amount);

    // sl = expected_entry * (1 - sl_pct)
    Money sl_amount = Money_Mul(expected_entry, sl_pct);
    Money stop_loss_price = Money_Sub(expected_entry, sl_amount);

    // volume gate: tick volume must exceed rolling avg * multiplier
    FPN_Binary<F> volume_threshold = FPN_Mul(rolling->volume_avg, core_cfg->volume_multiplier);

    // sizing: allocated_balance / expected_entry_price (notional ÷ price = qty)
    // guard against zero entry to avoid divide-by-zero (rolling stats uninit)
    Money trade_size = Money_Zero();
    if (!Money_IsZero(expected_entry)) {
        trade_size = Money_Div(allocated_balance, expected_entry);
    }

    out->bg_price_threshold   = expected_entry;
    out->bg_volume_threshold  = Money_FromBinary(volume_threshold);  // D-170 egress
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
    const PerCoreCfg<F>* core_cfg,
    Money allocated_balance,
    GateParameters<F>* out,
    MeanReversionState<F>* state = nullptr   // v5.4.0 Phase 2.2 — adaptive filter state
) {
    // v5.4.0 Phase 2.2: when state is provided (sharded path post-Phase 1),
    // use state->live_offset_pct + state->live_vol_mult + state->live_stddev_mult
    // — these are seeded from cfg at boot and adapted by MR_Adapt on cadence
    // (P&L regression-driven). When state is nullptr (legacy callers, tests),
    // fall back to cfg defaults — preserves pre-Phase 2.2 numerics.
    FPN_Binary<F> live_offset = state ? state->live_offset_pct  : Money_ToBinary(core_cfg->entry_offset_pct);
    FPN_Binary<F> live_vmult  = state ? state->live_vol_mult    : core_cfg->volume_multiplier;
    FPN_Binary<F> live_smult  = state ? state->live_stddev_mult : core_cfg->offset_stddev_mult;

    // BUG FIX (v4.0.3): pre-fix used `bg_threshold = rolling->price_avg`
    // with no depth requirement — gate fired on EVERY tick price < avg
    // (statistically half of all ticks during noise). Real MR buys on
    // meaningful DIPS below mean. Now matches SimpleDip's pattern: gate
    // sits at `avg - (avg × entry_offset_pct)` so it requires a true dip.
    FPN_Binary<F> avg = rolling->price_avg;
    if (FPN_IsZero(avg)) avg = rolling->price_max;

    // v5.4.0 Phase 2.2: dual-mode entry price like the legacy MR_BuySignal.
    // pct mode: entry = avg - (avg * live_offset_pct)
    // stddev mode: entry = avg - (stddev * live_stddev_mult)
    // Mode toggle is `live_smult > 0` (cfg.offset_stddev_mult is the global
    // toggle; state mirrors at boot, may drift via Adapt within bounds).
    FPN_Binary<F> entry_price;
    if (!FPN_IsZero(live_smult)) {
        FPN_Binary<F> stddev_offset = FPN_Mul(rolling->price_stddev, live_smult);
        entry_price = FPN_Sub(avg, stddev_offset);
    } else {
        FPN_Binary<F> dip_offset = FPN_Mul(avg, live_offset);
        entry_price = FPN_Sub(avg, dip_offset);
    }

    Money tp_pct = ResolvePerFillTpPct(STRATEGY_MEAN_REVERSION, *core_cfg);  // H22 single-source (A1)
    Money sl_pct = ResolvePerFillSlPct(STRATEGY_MEAN_REVERSION, *core_cfg);
    Money entry_m = Money_FromBinary(entry_price);  // D-170 ingress
    Money tp_amount = Money_Mul(entry_m, tp_pct);
    Money sl_amount = Money_Mul(entry_m, sl_pct);
    FPN_Binary<F> volume_threshold = FPN_Mul(rolling->volume_avg, live_vmult);

    Money trade_size = Money_Zero();
    if (!Money_IsZero(entry_m)) {
        trade_size = Money_Div(allocated_balance, entry_m);
    }

    out->bg_price_threshold   = entry_m;
    out->bg_volume_threshold  = Money_FromBinary(volume_threshold);
    out->sg_take_profit_price = Money_Add(entry_m, tp_amount);
    out->sg_stop_loss_price   = Money_Sub(entry_m, sl_amount);
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
    const PerCoreCfg<F>* core_cfg,
    Money allocated_balance,
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
    FPN_Binary<F> live_breakout = state ? state->live_breakout_mult : FPN_Zero<F>();
    FPN_Binary<F> live_vmult    = state ? state->live_vol_mult      : core_cfg->volume_multiplier;

    // BUG FIX (v4.0.3): same family as MR — pre-fix used `bg_threshold = avg`
    // with no breakout depth. Gate fired on every tick price > avg (with the
    // BUY_ABOVE flag). Real momentum buys on confirmed BREAKOUTS above the
    // mean, not infinitesimal upticks. Now requires a breakout of
    // `entry_offset_pct` above the rolling mean before the gate arms.
    FPN_Binary<F> avg = rolling->price_avg;
    if (FPN_IsZero(avg)) avg = rolling->price_max;
    // v5.4.0 Phase 2.3: stddev * live_breakout_mult when state provides it
    // (matches legacy Momentum_BuySignal: avg + stddev * breakout_mult). When
    // state is null OR live_breakout is zero, fall back to entry_offset_pct
    // (the pre-Phase 2.3 sharded behavior).
    FPN_Binary<F> breakout_offset;
    if (state && !FPN_IsZero(live_breakout) && !FPN_IsZero(rolling->price_stddev)) {
        breakout_offset = FPN_Mul(rolling->price_stddev, live_breakout);
    } else {
        breakout_offset = FPN_Mul(avg, Money_ToBinary(core_cfg->entry_offset_pct));
    }
    FPN_Binary<F> entry_price = FPN_Add(avg, breakout_offset);

    // STDDEV-floor guard: in early warmup or dead-flat markets,
    // rolling->price_stddev can be near-zero, which made tp_amount basically
    // zero and produced TP=SL=entry positions (caught visually in v4.0.2).
    // Require a minimum stddev relative to price (1bp) before trusting the
    // stddev-mult path; otherwise fall back to percentage.
    FPN_Binary<F> min_stddev_floor = FPN_Mul(avg, FPN_FromDouble<F>(0.0001));
    int stddev_usable = FPN_GreaterThan(rolling->price_stddev, min_stddev_floor);
    FPN_Binary<F> tp_amount, sl_amount;
    if (!FPN_IsZero(core_cfg->momentum_tp_mult) && stddev_usable) {
        tp_amount = FPN_Mul(rolling->price_stddev, core_cfg->momentum_tp_mult);
        sl_amount = FPN_Mul(rolling->price_stddev, core_cfg->momentum_sl_mult);
    } else {
        tp_amount = FPN_Mul(entry_price, Money_ToBinary(core_cfg->take_profit_pct));
        sl_amount = FPN_Mul(entry_price, Money_ToBinary(core_cfg->stop_loss_pct));
    }
    // v5.4.0 Phase 2.3: live_vmult from state when present (adaptive),
    // else cfg.volume_multiplier (legacy).
    FPN_Binary<F> volume_threshold = FPN_Mul(rolling->volume_avg, live_vmult);

    Money trade_size = Money_Zero();
    Money entry_mm = Money_FromBinary(entry_price);  // D-170 ingress
    if (!Money_IsZero(entry_mm)) {
        trade_size = Money_Div(allocated_balance, entry_mm);
    }

    out->bg_price_threshold   = entry_mm;
    out->bg_volume_threshold  = Money_FromBinary(volume_threshold);  // D-170 egress
    out->sg_take_profit_price = Money_Add(entry_mm, Money_FromBinary(tp_amount));
    out->sg_stop_loss_price   = Money_Sub(entry_mm, Money_FromBinary(sl_amount));
    out->tp_pct               = ResolvePerFillTpPct(STRATEGY_MOMENTUM, *core_cfg);  // flat (no MOM override); H22 single-source (A1)
    out->sl_pct               = ResolvePerFillSlPct(STRATEGY_MOMENTUM, *core_cfg);
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
            double sl = Money_ToDouble(out->sg_stop_loss_price);
            double tp = Money_ToDouble(out->sg_take_profit_price);
            const char* geom = (sl < e) ? "OK_long" : (sl > e ? "INVERTED" : "ZERO");
            fprintf(stderr,
                "[mom-sl-build] entry=%.2f tp=%.2f sl=%.2f sl_amount=%.4f "
                "stddev=%.4f mom_sl_mult=%.4f geom=%s\n",
                e, tp, sl, FPN_ToDouble(sl_amount),
                FPN_ToDouble(rolling->price_stddev),
                FPN_ToDouble(core_cfg->momentum_sl_mult), geom);
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
    const PerCoreCfg<F>* core_cfg,
    Money allocated_balance,
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
    // .E.0.10 A1/H22 EXEMPTION (documented per H16-style): this stateless shim is
    // DELIBERATELY not routed through ResolvePerFillTpPct — it preserves the legacy
    // simpledip-fallback for old snapshots/tests. It is UNREACHABLE in production: the
    // sharded dispatcher always passes a non-null EmaCrossState, taking the state-aware
    // helper path below, so a real warm-restart never diverges (the A1 refute confirmed
    // this is test/legacy-only). A future single-source CI-check must EXEMPT this branch.
    if (!state) {
        SimpleDip_BuildParameters(rolling, core_cfg, allocated_balance, out);
        out->strategy_id = STRATEGY_EMA_CROSS;
        if (!Money_IsZero(core_cfg->emacross_tp_pct)) {
            out->tp_pct = core_cfg->emacross_tp_pct;
            Money entry = out->bg_price_threshold;
            out->sg_take_profit_price = Money_Add(entry, Money_Mul(entry, core_cfg->emacross_tp_pct));
        }
        if (!Money_IsZero(core_cfg->emacross_sl_pct)) {
            out->sl_pct = core_cfg->emacross_sl_pct;
            Money entry = out->bg_price_threshold;
            out->sg_stop_loss_price = Money_Sub(entry, Money_Mul(entry, core_cfg->emacross_sl_pct));
        }
        return;
    }

    // State-aware path. ref = state->prev_ema (Adapt populated this from
    // the producer's per-tick EMA replication). Fall back to rolling avg
    // if the state hasn't been seeded yet (cold-start, ema=0).
    FPN_Binary<F> ref = !FPN_IsZero(state->prev_ema) ? state->prev_ema : rolling->price_avg;

    // Crossover gate: ref must sit above short SMA by at least
    // emacross_crossover_min stddevs. Same geometry as the legacy
    // EmaCross_BuySignal.
    // branchless compute-guard: compute always (zero divisor → safe saturate), gate by `valid`.
    int uptrend;
    {
        int valid = !FPN_IsZero(rolling->price_avg) & !FPN_IsZero(rolling->price_stddev);
        FPN_Binary<F> diff = FPN_Sub(ref, rolling->price_avg);
        int ema_above = (diff.v > 0);   // positive diff (was sign==0 && !IsZero; 16B two's-comp)
        FPN_Binary<F> spread_stddevs = FPN_DivNoAssert(diff, rolling->price_stddev);
        uptrend = valid & ema_above & FPN_GreaterThan(spread_stddevs, core_cfg->emacross_crossover_min);
    }

    // Buy price = ref - stddev * emacross_dip_mult (dip below EMA)
    FPN_Binary<F> dip = FPN_Mul(rolling->price_stddev, core_cfg->emacross_dip_mult);
    FPN_Binary<F> entry_price = FPN_Sub(ref, dip);

    // TP/SL: use EMA-specific cfg overrides; fall through to shared.
    Money tp_pct = ResolvePerFillTpPct(STRATEGY_EMA_CROSS, *core_cfg);  // H22 single-source (A1)
    Money sl_pct = ResolvePerFillSlPct(STRATEGY_EMA_CROSS, *core_cfg);

    Money entry_m = Money_FromBinary(entry_price);  // D-170 ingress
    Money tp_amount = Money_Mul(entry_m, tp_pct);
    Money sl_amount = Money_Mul(entry_m, sl_pct);
    FPN_Binary<F> volume_threshold = FPN_Mul(rolling->volume_avg, core_cfg->volume_multiplier);

    Money trade_size = Money_Zero();
    Money entry_me = Money_FromBinary(entry_price);  // D-170 ingress
    if (!Money_IsZero(entry_me)) {
        trade_size = Money_Div(allocated_balance, entry_me);
    }

    out->bg_price_threshold   = uptrend ? entry_me : Money_Zero();
    out->bg_volume_threshold  = Money_FromBinary(volume_threshold);  // D-170 egress
    out->sg_take_profit_price = Money_Add(entry_me, tp_amount);
    out->sg_stop_loss_price   = Money_Sub(entry_me, sl_amount);
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
// BarrierGate modulation (when BITMAP_IS_SET(core_cfg->gate_cfg_flags, MASK_GATE_CFG_BARRIER_GATE_ENABLED)):
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
    const PerCoreCfg<F>* core_cfg,
    Money allocated_balance,
    GateParameters<F>* out,
    void* ml_ctx_ptr,
    uint64_t now_us = 0,  // v5.14.1.B.2 (PARITY-001) — passed by caller for
                          // replay-determinism. Live: clock_gettime at slow-
                          // path entry. Backtest: tick.timestamp (deterministic).
                          // Default 0 = legacy/test path (composite stays in
                          // cold-start freshness=0 if enabled; harmless when
                          // composite_enabled=0 which is the default).
    int poll_interval_ticks = 100  // v5.15.5.F.4c.3 WIP2c.2 (Class 25 closure) —
                                    // caller-resolved scalar arg for tick→time
                                    // conversion. poll_interval is engine-wide
                                    // global; per-core consumer reads it as
                                    // scalar to preserve single-param discipline
                                    // (cfg-scope-discipline.md § "Consumer
                                    // function signatures"). Default 100 matches
                                    // the global cfg default.
) {
    MLBuildContext* mctx = (MLBuildContext*)ml_ctx_ptr;
    // v5.15.5.A.4 — function-scope locals for per-horizon barrier dispatch.
    // Computed INSIDE the weighted-block scope (weights_buf is local to
    // that scope; trace-deps audit RED finding). Consumed at the cfg-
    // fallback site at ~:1259-1260 below. Defaults preserve LEGACY mode
    // bytewise-identical behavior when ensemble inactive or dispatch
    // mode disabled.
    int blend_dominant_h = -1;
    double blend_tp_d = 0.0, blend_sl_d = 0.0;
    double dominant_tp_d = 0.0, dominant_sl_d = 0.0;  // captured at compute time; ezoo not in dispatch-site scope
    bool blend_dispatch_ready = false;  // 1 = weights+barriers populated
    CoreModelZoo<F>* zoo = nullptr;
    ConfidenceScorer* conf_scorer = nullptr;
    double* out_prediction = nullptr;
    double* out_confidence = nullptr;
    const RORRegressor<F>* ror_in = nullptr;
    const FPN_Binary<F>* ema_in = nullptr;
    // v5.14.9.B.0 — per-core slow-path gate cache (FOREACH_SLOW_PATH_GATE
    // PER_CORE entries). Wired by EventLoop_RebuildOneCore upstream.
    // Null when caller didn't populate (legacy/test path) — use sites
    // fall back to inline cfg-flag reads for compatibility.
    const SlowPathGateState* gate_state = nullptr;
    if (mctx) {
        zoo = (CoreModelZoo<F>*)mctx->model_handle;
        conf_scorer = mctx->confidence;
        out_prediction = mctx->out_prediction;
        out_confidence = mctx->out_confidence;
        ror_in = (const RORRegressor<F>*)mctx->ror_regressor;
        ema_in = (const FPN_Binary<F>*)mctx->ema_price;
        gate_state = (const SlowPathGateState*)mctx->gate_state;
    }

    // if no zoo or no models loaded, fall back to SimpleDip
    if (!zoo || !CoreModelZoo_HasAny(zoo)) {
        // v5.9.0b — emit rate-limited CRITICAL log so the operator sees
        // ML→SimpleDip fall-through (V5_9_AUDIT-#2). Pre-v5.9.0b this
        // was silent; now visible in health log.
        if (mctx && mctx->last_ml_critical_log_us) {
            // v5.15.5.B.3 — model_load_failed is now a plain `int` value
            // (was `int*` pre-.B.3; bitmap bit is not addressable).
            int load_failed = mctx->model_load_failed;
            tt::Health_LogCriticalRateLimited(
                mctx->last_ml_critical_log_us,
                /*gate_us=*/60000000ULL,  // 60s per-core gate
                /*core=*/-1,  // ML_BuildParameters doesn't have core_id; -1 = unknown
                "ml",
                "ML→SimpleDip fall-through: %s (zoo=%s)",
                load_failed ? "model load failed" : "no model configured",
                zoo ? "empty" : "null");
        }
        SimpleDip_BuildParameters(rolling, core_cfg, allocated_balance, out, rolling_long);
        out->strategy_id = STRATEGY_ML;
        return;
    }

    // compute entry price (same as SimpleDip for sizing)
    FPN_Binary<F> recent_high = rolling->price_max;
    if (rolling_long && FPN_GreaterThan(rolling_long->price_max, recent_high)) {
        recent_high = rolling_long->price_max;
    }
    FPN_Binary<F> entry_price = rolling->price_avg;
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
    // v5.11.18 main: thread per-core feature_mask from MLBuildContext.
    // When mctx->feature_mask is null (default; legacy callers), pack all
    // enabled features identically to pre-v5.11.18. When non-null, packs
    // sparse-zero (zeroed slot for any unset bit). The model's stamp must
    // have feature_mask_train matching the runtime mask — verified at
    // load-time by verify_model_stamp's expected_feature_mask param.
    float features[MODEL_MAX_FEATURES];
    FeatureComputeCtx<F> ctx{};
    ctx.signals       = &sig;
    ctx.short_rolling = rolling;
    const uint64_t* eff_mask = (mctx && mctx->feature_mask) ? mctx->feature_mask : nullptr;
    int n = Features_PackAll(&ctx, features, eff_mask);
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
        SimpleDip_BuildParameters(rolling, core_cfg, allocated_balance, out, rolling_long);
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
            SimpleDip_BuildParameters(rolling, core_cfg, allocated_balance, out, rolling_long);
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
            SimpleDip_BuildParameters(rolling, core_cfg, allocated_balance, out, rolling_long);
            out->strategy_id = STRATEGY_ML;
            return;
        }
        // v5.10.0a.G.5/G.7 — ensemble dispatch when active.
        // Two modes (cfg.ensemble_blend_mode):
        //   "selection" → G.4 argmax-confidence (single horizon per tick)
        //   "weighted"  → G.7 Bandit-Exp3 per-regime weighted blend (default)
        // ensemble_zoo->buy_signal[] all share the SAME scaler (per G.3
        // load-from-cfg invariant), so the standardize step above already
        // produced the correct features for every horizon (G.7 perf opt #1
        // — caching across horizons).
        double pred_raw = 0.0;
        EnsembleModelZoo<F>* ezoo = (EnsembleModelZoo<F>*)
            (mctx ? mctx->ensemble_zoo : nullptr);
        // v5.11.62 — read primary_handles + primary_count instead of
        // buy_signal_*. Loader picks role at boot (priority: buy_signal
        // > barrier > regime); per-handle buy_class_idx makes
        // Model_Predict transparently extract the right class.
        if (ezoo && BITMAP_IS_SET(ezoo->init_flags, MASK_EZOO_ACTIVE) && ezoo->primary_count > 0 &&
            ezoo->primary_handles) {
            int dominant_idx = -1;
            // Mode dispatch: weighted (default) uses bandit weights;
            // selection falls back to G.4 argmax-confidence.
            bool use_weighted = (strcmp(ezoo->blend_mode, "weighted") == 0);
            // v5.10.0a.G.8 — buffer for per-arm predictions written by the
            // weighted helper; used to populate the reward ring record so
            // slow-path lookback can attribute rewards correctly later.
            float per_arm_preds[ENSEMBLE_HORIZON_MAX];
            for (int a = 0; a < ENSEMBLE_HORIZON_MAX; ++a)
                per_arm_preds[a] = 0.5f;
            if (use_weighted && BITMAP_IS_SET(ezoo->init_flags, MASK_EZOO_BANDITS_READY)) {
                // G.7 path: per-regime bandit weights drive blend.
                int regime_id = mctx ? mctx->current_regime_id : 0;
                if (regime_id < 0 || regime_id >= NUM_REGIMES) regime_id = 0;
                ezoo->last_predicted_regime_id = regime_id;
                double weights_buf[ENSEMBLE_HORIZON_MAX];

                // v5.14.10.B — bandit algorithm dispatch via FOREACH_BANDIT_ALGORITHM
                // registry (ML_Headers/BanditAlgorithmRegistry.hpp). cfg.bandit_algorithm
                // enum: 0=EXP3 (default; bytewise-identical to pre-v5.14.10), 1=THOMPSON,
                // 2=BOTH (Exp3 drives action; Thompson chosen_arm logged for cfg=2 telemetry).
                //
                // EXP3 path branch is INTENTIONALLY duplicated (not routed through
                // BanditAlgorithm_Apply) to preserve bytewise-identical behavior of the
                // regime hysteresis blend. Hysteresis only fires for Exp3 because Thompson's
                // one-hot output weights have no natural alpha-blend semantic
                // (alpha-blending one-hot with probability distribution is mathematically
                // undefined; would corrupt Thompson's intended argmax-of-posterior dynamics).
                if (core_cfg->bandit_algorithm == 0) {
                    // G.7 #7 — regime hysteresis dampening (Exp3 only). When regime just
                    // changed, blend OLD bandit's weights with NEW for hysteresis cycles.
                    // Otherwise use current bandit directly. Bytewise-identical to pre-v5.14.10.
                    if (ezoo->regime_transition_cycles_remaining > 0) {
                        double w_curr[ENSEMBLE_HORIZON_MAX];
                        double w_prev[ENSEMBLE_HORIZON_MAX];
                        Bandit_GetProbabilities(&ezoo->bandits[regime_id], w_curr);
                        Bandit_GetProbabilities(&ezoo->bandits[ezoo->prev_regime_id], w_prev);
                        int hyst = core_cfg->regime_hysteresis > 0
                                 ? (int)core_cfg->regime_hysteresis : 5;
                        double alpha = (double)(hyst - ezoo->regime_transition_cycles_remaining) /
                                       (double)hyst;
                        if (alpha < 0.0) alpha = 0.0;
                        if (alpha > 1.0) alpha = 1.0;
                        for (int h = 0; h < ezoo->primary_count; ++h) {
                            weights_buf[h] = alpha * w_curr[h] + (1.0 - alpha) * w_prev[h];
                        }
                        ezoo->regime_transition_cycles_remaining--;
                    } else {
                        Bandit_GetProbabilities(&ezoo->bandits[regime_id], weights_buf);
                    }
                } else {
                    // THOMPSON (cfg=1) or BOTH (cfg=2) — single registry dispatch.
                    // Hysteresis SKIPPED (see comment above). Hysteresis counter
                    // decremented anyway so a future cfg-flip back to EXP3 sees clean state.
                    int chosen_arm = -1;
                    // v5.15.5.F.4d Step 4 complete (§ C of merged plan body) — blend_alpha now sourced
                    // from per-core resolved cfg field `thompson_exp3_blend_alpha` (cfg field added at
                    // CfgFieldRegistry.hpp + STAMP_BOUND for replay determinism + drift-check row in
                    // CfgDriftCheckRegistry.hpp gated to BLENDED state). For non-BLENDED algos (cfg=0/1/2/3),
                    // BanditAlgorithm_Apply ignores blend_alpha entirely; only BLENDED state-4
                    // (`BanditAlgo_Blended_Apply`) reads it. Per-core resolution avoids Class 27 scalar
                    // mirror (alpha varies per-core; each core's BLENDED dispatch reads its own value).
                    BanditAlgorithm_Apply(core_cfg->bandit_algorithm,
                                          &ezoo->bandits[regime_id],
                                          BITMAP_IS_SET(ezoo->init_flags, MASK_EZOO_BUY_THOMPSON_READY)
                                              ? &ezoo->buy_thompson_bandits[regime_id] : nullptr,
                                          ezoo->primary_count,
                                          /*blend_alpha=*/FPN_ToDouble(core_cfg->thompson_exp3_blend_alpha),
                                          weights_buf,
                                          &chosen_arm);
                    // Capture Thompson's chosen_arm for cfg=2 telemetry. .D's
                    // FOREACH_CALIB_LOG_COL writer reads this per fill.
                    if (chosen_arm >= 0) {
                        ezoo->last_predicted_buy_thompson_arm = chosen_arm;
                    }
                    if (ezoo->regime_transition_cycles_remaining > 0) {
                        ezoo->regime_transition_cycles_remaining--;
                    }
                }
                // v5.14.0.B — Ridge risk-parity blending OVERRIDE.
                // When cfg.ridge_within_horizon=1, supersede bandit weights
                // with Ridge weights computed from prediction-correlation
                // matrix + cost-aware IC. Default 0: bytewise-identical to
                // pre-v5.14 bandit-only path (this block is skipped).
                //
                // Ridge complements bandit: bandit selects/weights via
                // exponential-update on per-arm rewards; Ridge accounts
                // for correlation BETWEEN arms (penalizes double-counting
                // of correlated alpha sources). Both run; Ridge wins when
                // flag is on.
                //
                // Cost: ~3µs/cycle when enabled (BuildCorr ~1µs +
                // Cholesky ~2µs at N=8). Default off pays ~5ns flag check.
                // v5.14.9.B.0 — read ridge_within_horizon gate from cached
                // per-core state when wired; fall back to inline cfg-flag.
                // v5.14.11.C — branchless multi-flag mask check (CLAUDE.md item
                // 18). "Ridge ON AND Thompson OFF" collapses to a single
                // mask-AND-compare when gate_state is present (1 cycle vs
                // 2-branch scalar). Fallback keeps scalar form for backtest
                // paths that don't wire gate_state. v5.14.10.B — Ridge override
                // mutually-exclusive with Thompson. Thompson's one-hot weights
                // would feed degenerate correlation history into Ridge's
                // BuildCorr → singular Σ → fallback_to_uniform (ineffective).
                bool _ridge_dispatch;
                if (gate_state) {
                    constexpr uint16_t ridge_only_mask = MASK_RIDGE_WITHIN_ACTIVE | MASK_THOMPSON_ACTIVE;
                    _ridge_dispatch = (gate_state->flags & ridge_only_mask) == MASK_RIDGE_WITHIN_ACTIVE;
                } else {
                    _ridge_dispatch = BITMAP_IS_SET(core_cfg->ml_cfg_flags, MASK_ML_CFG_RIDGE_WITHIN_HORIZON)
                                     && core_cfg->bandit_algorithm == 0;
                }
                if (_ridge_dispatch &&
                    ezoo->primary_count >= 2) {
                    // v5.14.11.A — OnlineCycleStep helper consolidates ring-walk
                    // + BuildCorr at this site + the exit-side mirror (below at
                    // ~:1195). C1 helper extraction eliminates the Class 18
                    // mirror per CLAUDE.md item 19.
                    // v5.14.11.C — use_online wired from cfg.ridge_online_corr
                    // (default 0 = full recompute; bytewise-identical). Pattern:
                    // DESIGN_SPECS/sliding-window-online-statistics-pattern.md.
                    const bool use_online = gate_state
                        ? BITMAP_IS_SET(gate_state->flags, MASK_RIDGE_ONLINE_CORR_ACTIVE)
                        : BITMAP_IS_SET(core_cfg->ml_cfg_flags, MASK_ML_CFG_RIDGE_ONLINE_CORR);
                    int rc_corr = RidgeBlender_OnlineCycleStep<F>(
                        &ezoo->ridge_state,
                        ezoo->reward_ring,
                        ezoo->reward_ring_head,
                        EnsembleModelZoo<F>::REWARD_RING_SIZE,
                        ezoo->predict_call_count,
                        ezoo->primary_count,
                        use_online);
                    if (rc_corr == 0) {
                        // corr_matrix populated; proceed to Cholesky + weights.
                        // IC per arm from existing drift watchdog tracker
                        // (v5.10.0a.G.8 — already populated post-trade-close).
                        double ic_per_arm[MAX_RIDGE_MODELS];
                        double cost_per_arm[MAX_RIDGE_MODELS];
                        for (int i = 0; i < ezoo->primary_count; ++i) {
                            ic_per_arm[i] = (double)ezoo->drift[i].ic_avg;
                            // Cost tracking deferred to v5.15+; default 0.
                            cost_per_arm[i] = 0.0;
                        }
                        // Solve. On Cholesky failure, RidgeBlender_Compute
                        // sets fallback_to_uniform=1 + writes uniform 1/N
                        // weights — safe fallback (no exception/abort).
                        int rc = RidgeBlender_Compute<F>(
                            &ezoo->ridge_state,
                            ic_per_arm, cost_per_arm, ezoo->primary_count,
                            FPN_ToDouble(core_cfg->ridge_lambda),
                            FPN_ToDouble(core_cfg->ridge_cost_penalty),
                            FPN_ToDouble(core_cfg->ridge_min_ic_floor));
                        (void)rc;  // diagnostic only via fallback_to_uniform
                        for (int i = 0; i < ezoo->primary_count; ++i) {
                            weights_buf[i] = FPN_ToDouble(ezoo->ridge_state.w[i]);
                        }
                    }
                    // rc_corr == -1: not enough history; bandit weights
                    // stay in weights_buf (Ridge needs ≥2 samples).
                }
                // v5.14.1.G — push top-K mask to turnover ring for diagnostic.
                // weights_buf is now finalized (post-bandit OR post-Ridge).
                // Reads ezoo->primary_count + weights_buf; writes to per-core
                // turnover ring via mctx->turnover_state pointer (CoreContext-
                // owned). No-op when state nullptr (legacy / non-ML).
                if (mctx && mctx->turnover_state && ezoo->primary_count > 0) {
                    uint8_t topk_mask = topk_mask_from_weights(
                        weights_buf, ezoo->primary_count,
                        mctx->turnover_topk);
                    RollingTurnover_Push(
                        (RollingTurnover*)mctx->turnover_state, topk_mask);
                }
                pred_raw = (double)Model_Predict_Ensemble_Weighted(
                    ezoo->primary_handles, ezoo->primary_count,
                    features, n,
                    weights_buf,
                    ezoo->disabled_horizon_mask,
                    core_cfg->ensemble_min_agreement_pct,
                    &dominant_idx,
                    per_arm_preds);
                // v5.15.5.A.4 — per-horizon barrier dispatch compute. INSIDE
                // the weighted-block scope (weights_buf is local here per
                // trace-deps RED finding). Computes BOTH blend (Σ wᵢ · barrierᵢ)
                // AND dominant (argmax weights) so the mode-dispatch site below
                // at the cfg-fallback can pick either via branchless MODE_FLAGS[]
                // mask. Constant-iter inner loop (CLAUDE.md item 26); active mask
                // (i < primary_count) zeroed via multiply for branchless.
                // Gated by arms_with_barriers_mask: only arms with stamp barriers
                // contribute; missing arms zero out via the mask AND.
                double max_w_v5155 = -1.0;
                for (int i = 0; i < ENSEMBLE_HORIZON_MAX; i++) {
                    double active = (i < ezoo->primary_count) ? 1.0 : 0.0;
                    double has_b  = BITMAP_IS_SET(ezoo->arms_with_barriers_mask,
                                                   BITMAP_BIT_U8(i)) ? 1.0 : 0.0;
                    double gate = active * has_b;
                    blend_tp_d += gate * weights_buf[i] *
                                  (double)ezoo->per_arm_barriers[i].tp;
                    blend_sl_d += gate * weights_buf[i] *
                                  (double)ezoo->per_arm_barriers[i].sl;
                    bool is_greater = (gate > 0.0) && (weights_buf[i] > max_w_v5155);
                    max_w_v5155       = is_greater ? weights_buf[i] : max_w_v5155;
                    blend_dominant_h  = is_greater ? i               : blend_dominant_h;
                }
                blend_dispatch_ready = (blend_dominant_h >= 0);
                // Capture dominant arm's barriers as function-scope doubles
                // (ezoo is local to this block; dispatch site at ~:1290 below
                // can't reference ezoo directly).
                if (blend_dispatch_ready) {
                    dominant_tp_d = (double)ezoo->per_arm_barriers[blend_dominant_h].tp;
                    dominant_sl_d = (double)ezoo->per_arm_barriers[blend_dominant_h].sl;
                }
            } else {
                // Selection path (G.4 argmax-confidence). Bandit-uninit
                // ensembles also fall here (cold-start before _InitBandits).
                // We still want per-arm predictions for G.8 reward records;
                // run them inline.
                for (int a = 0; a < ezoo->primary_count; ++a) {
                    if (Model_IsLoaded(&ezoo->primary_handles[a])) {
                        per_arm_preds[a] = Model_Predict(&ezoo->primary_handles[a],
                                                          features, n);
                    } else {
                        per_arm_preds[a] = 0.5f;
                    }
                }
                pred_raw = (double)Model_Predict_Ensemble(
                    ezoo->primary_handles, ezoo->primary_count,
                    features, n, &dominant_idx);
            }
            ezoo->last_predicted_horizon_idx = dominant_idx;
            // v5.10.0a.G.8 — record this prediction for later reward
            // attribution (slow-path lookback + trade-close hooks).
            // Use rolling->price_avg as a stable proxy for "current price"
            // (per-tick price isn't directly available in mctx).
            float current_price = (float)FPN_ToDouble(rolling->price_avg);
            if (current_price > 0.0f) {
                EnsembleModelZoo_RecordPrediction(
                    ezoo,
                    ezoo->last_predicted_regime_id,
                    per_arm_preds,
                    ezoo->primary_count,
                    current_price);
                // Process any old-enough records: compute reward + Bandit_Update.
                // Forward horizon = 1000 ticks (matches training label
                // default; live cfg has no `label_forward_ticks` field —
                // that's a BacktestRunConfig-only setting). When per-arm
                // horizons differ in v5.10.0a.next, replace with arm-
                // specific lookback walking ezoo->barrier_horizons[].
                // ic_floor 0.02 keeps drift watchdog safely inert at low
                // sample counts; v5.10.0e will pull it from cfg.
                // v5.15.5.F.4d — pass core_cfg for per-core bandit_algorithm dispatch (Step 3 +
                // § H Class 25 sweep). Replaces former per-call cfg branches at attribution sites
                // with metadata-driven g_buy_reward_dispatch[algo] inside the fn body. Class 24
                // sister + Class 25 + Class 28 closure at this attribution surface.
                EnsembleModelZoo_TickRewardsFromLookback(
                    ezoo,
                    current_price,
                    /*forward_ticks=*/1000,
                    poll_interval_ticks,   // v5.15.5.F.4c.3 WIP2c.2 — caller-resolved scalar
                    /*ic_floor=*/0.02,
                    core_cfg);             // v5.15.5.F.4d — per-core bandit_algorithm source
            }
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

    // v5.13.0.B — sell-side prediction (Path 3 architecture). Runs once
    // per slow-path rebuild when:
    //   1. BITMAP_IS_SET(cfg.ml_cfg_flags, MASK_ML_CFG_USE_EXIT_MODEL) = 1 (operator opt-in)
    //   2. ezoo->exit_predictor_count > 0 (models loaded)
    //   3. mctx->out_exit_prediction != nullptr (slow-path body wired)
    //
    // Reuses already-standardized features from the buy-side path (scaler
    // is shared across roles via sibling-scaler load-time check). Writes
    // blended exit probability to *out_exit_prediction; slow-path body
    // post-RebuildOneCore reads it + fires OMS submit if above threshold.
    //
    // Hot path UNTOUCHED. Default cfg (use_exit_model=0): ~5ns flag check.
    EnsembleModelZoo<F>* ezoo_ex = (EnsembleModelZoo<F>*)
        (mctx ? mctx->ensemble_zoo : nullptr);
    if (BITMAP_IS_SET(core_cfg->ml_cfg_flags, MASK_ML_CFG_USE_EXIT_MODEL)
        && ezoo_ex
        && ezoo_ex->exit_predictor_count > 0
        && mctx
        && mctx->out_exit_prediction) {
        // v5.14.1.E — collect per-handle predictions (used by both the
        // existing uniform/Ridge blend AND the exit_reward_ring populate
        // for Ridge correlation history).
        float per_handle_pred[ENSEMBLE_HORIZON_MAX] = {0};
        int n_loaded = 0;
        int dominant = -1;
        float max_p = -1.0f;
        for (int h = 0; h < ezoo_ex->exit_predictor_count; ++h) {
            ModelHandle<F>* mh = &ezoo_ex->exit_predictor[h];
            if (!Model_IsLoaded(mh)) continue;
            float p = Model_Predict_Normalized(mh, features, n);
            if (std::isnan(p) || std::isinf(p)) continue;
            per_handle_pred[h] = p;
            n_loaded++;
            if (p > max_p) { max_p = p; dominant = h; }
        }
        if (n_loaded > 0) {
            // v5.14.1.E — push per-handle predictions into exit_reward_ring
            // BEFORE blending (mirrors buy-side reward_ring populate at
            // CoreModelZoo.hpp:1002+). Used by Ridge solver to compute
            // correlation matrix from prediction history. Cheap: ~50 bytes
            // memcpy per cycle into a 256-slot ring.
            int slot = ezoo_ex->exit_reward_ring_head %
                       EnsembleModelZoo<F>::REWARD_RING_SIZE;
            auto& rec = ezoo_ex->exit_reward_ring[slot];
            rec.predict_call = ezoo_ex->exit_predict_call_count;
            for (int h = 0; h < ezoo_ex->exit_predictor_count; ++h) {
                rec.predictions[h] = per_handle_pred[h];
            }
            ezoo_ex->exit_predict_call_count++;
            ezoo_ex->exit_reward_ring_head =
                (ezoo_ex->exit_reward_ring_head + 1) %
                EnsembleModelZoo<F>::REWARD_RING_SIZE;

            // v5.14.1.E — blend computation. Default cfg.exit_blender_mode=0
            // → uniform average (pre-v5.14.1.E behavior, bytewise unchanged).
            // When =1: Ridge override using exit_ridge_state + exit_reward_ring
            // history. Mirrors v5.14.0 buy-side ridge_within_horizon block at
            // StrategyParameters.hpp:891-947.
            double weights[ENSEMBLE_HORIZON_MAX];
            for (int i = 0; i < ezoo_ex->exit_predictor_count; ++i) {
                weights[i] = 1.0 / (double)n_loaded;  // uniform default
            }
            // v5.14.9.B.0 — read exit_blender gate from cached state when wired
            // v5.14.11.C — cfg.exit_blender_mode migrated to ml_cfg_flags bitmap
            bool _exit_blender_gate = gate_state
                ? BITMAP_IS_SET(gate_state->flags, MASK_EXIT_BLENDER_ACTIVE)
                : BITMAP_IS_SET(core_cfg->ml_cfg_flags, MASK_ML_CFG_EXIT_BLENDER_MODE);
            if (_exit_blender_gate &&
                ezoo_ex->exit_predictor_count >= 2) {
                // v5.14.11.A — OnlineCycleStep helper (mirrors buy-side dispatch
                // at ~:996). C1 helper extraction eliminates Class 18 mirror.
                // v5.14.11.C — use_online wired from cfg.ridge_online_corr
                // (default 0 = full recompute; bytewise-identical).
                const bool use_online = gate_state
                    ? BITMAP_IS_SET(gate_state->flags, MASK_RIDGE_ONLINE_CORR_ACTIVE)
                    : BITMAP_IS_SET(core_cfg->ml_cfg_flags, MASK_ML_CFG_RIDGE_ONLINE_CORR);
                int rc_corr = RidgeBlender_OnlineCycleStep<F>(
                    &ezoo_ex->exit_ridge_state,
                    ezoo_ex->exit_reward_ring,
                    ezoo_ex->exit_reward_ring_head,
                    EnsembleModelZoo<F>::REWARD_RING_SIZE,
                    ezoo_ex->exit_predict_call_count,
                    ezoo_ex->exit_predictor_count,
                    use_online);
                if (rc_corr == 0) {
                    // corr_matrix populated; proceed to Cholesky + weights.
                    // IC + cost arrays. For exit side, reuse drift[] (per-arm
                    // IC tracker; populated from exit prediction outcomes
                    // post-fill via existing v5.13.4 path). Cost stays 0.0
                    // until v5.15+ live cost-aware tracking lands.
                    double ic_per_arm[MAX_RIDGE_MODELS]   = {0};
                    double cost_per_arm[MAX_RIDGE_MODELS] = {0};
                    for (int i = 0; i < ezoo_ex->exit_predictor_count; ++i) {
                        // Buy-side uses ezoo->drift[i].ic_avg; exit side has
                        // its own per-handle drift via ic_avg_exit[] when
                        // available. Default to 0 if not yet tracked
                        // (Ridge will floor to ridge_min_ic_floor anyway).
                        ic_per_arm[i] = 0.0;
                    }
                    int rc = RidgeBlender_Compute<F>(
                        &ezoo_ex->exit_ridge_state,
                        ic_per_arm, cost_per_arm, ezoo_ex->exit_predictor_count,
                        FPN_ToDouble(core_cfg->ridge_lambda),
                        FPN_ToDouble(core_cfg->ridge_cost_penalty),
                        FPN_ToDouble(core_cfg->ridge_min_ic_floor));
                    (void)rc;  // diagnostic via fallback_to_uniform
                    for (int i = 0; i < ezoo_ex->exit_predictor_count; ++i) {
                        weights[i] = FPN_ToDouble(ezoo_ex->exit_ridge_state.w[i]);
                    }
                }
                // rc_corr == -1: not enough history; uniform weights stay
            }
            // Weighted blend
            double blended = 0.0;
            for (int h = 0; h < ezoo_ex->exit_predictor_count; ++h) {
                blended += weights[h] * (double)per_handle_pred[h];
            }
            *mctx->out_exit_prediction = blended;
            if (mctx->out_exit_dominant_horizon)
                *mctx->out_exit_dominant_horizon = dominant;
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
        SimpleDip_BuildParameters(rolling, core_cfg, allocated_balance, out, rolling_long);
        out->strategy_id = STRATEGY_ML;
        return;
    }

    // v5.9.0b — record threshold + effective_threshold for entry log + ML
    // Status panel display. Threshold is read from cfg; effective is
    // post-confidence-damping (computed below in the gate decision).
    double ml_threshold_d = (double)FPN_ToDouble(core_cfg->ml_buy_threshold);
    if (mctx && mctx->out_threshold) *mctx->out_threshold = ml_threshold_d;

    // v5.15.5.A.4 — TP/SL mode-dispatch via FOREACH_BARRIER_BLEND_MODE.
    // Branchless dispatch using MODE_FLAGS[] bit-packed lookup (Rule 8
    // Pattern 8b from cache-layout-discipline DESIGN_SPEC). Modes:
    //   LEGACY               — cfg.ml_tp_pct direct (pre-v5.15.5 behavior)
    //   BLEND                — Σ wᵢ · barrierᵢ from per_arm_barriers
    //   DOMINANT             — argmax(weights) picks one arm's barriers
    //   BOTH_BLEND_DRIVES    — blend drives trade; dominant logged for shadow
    //   BOTH_DOMINANT_DRIVES — dominant drives trade; blend logged for shadow
    //
    // Per-core override: barrier_blend_mode reads from cfg directly today;
    // future v5.15.6 wires through gate_state cache + per-core resolution.
    // Master ON/OFF: per_horizon_barrier_blend cohort bit in ml_cfg_flags
    // gates the entire feature; cleared → falls back to LEGACY regardless
    // of mode value.
    Money tp_pct, sl_pct;
    bool feature_enabled = BITMAP_IS_SET(core_cfg->ml_cfg_flags,
                                          MASK_ML_CFG_PER_HORIZON_BARRIER_BLEND);
    int active_mode = feature_enabled ? core_cfg->barrier_blend_mode
                                      : MODE_BARRIER_BLEND_LEGACY;
    uint8_t mode_flags = (active_mode >= 0 && active_mode < MODE_BARRIER_BLEND_COUNT)
                             ? MODE_FLAGS[active_mode]
                             : MODE_F_LEGACY;
    bool blend_drives    = (mode_flags & MODE_F_BLEND_DRIVES)    != 0;
    bool dominant_drives = (mode_flags & MODE_F_DOMINANT_DRIVES) != 0;
    // Dispatch resolution (branchless ternary chain):
    if (blend_dispatch_ready && blend_drives) {
        tp_pct = Money{ money_from_double_payload(blend_tp_d) };
        sl_pct = Money{ money_from_double_payload(blend_sl_d) };
    } else if (blend_dispatch_ready && dominant_drives) {
        tp_pct = Money{ money_from_double_payload(dominant_tp_d) };
        sl_pct = Money{ money_from_double_payload(dominant_sl_d) };
    } else {
        // LEGACY fallback: cfg-direct (bytewise-identical to pre-v5.15.5).
        tp_pct = core_cfg->ml_tp_pct;
        sl_pct = core_cfg->ml_sl_pct;
    }
    // v5.15.5.A.6 — observability writes for the per-horizon barrier
    // dispatch. Mirrors exit-side pattern. Surfaces to MLStatusPanel via
    // PerCoreSnap (ShardedSnapshot.hpp:597-602 area).
    if (mctx) {
        if (mctx->out_buy_dominant_horizon)
            *mctx->out_buy_dominant_horizon = blend_dispatch_ready ? blend_dominant_h : -1;
        if (mctx->out_barrier_mode_used)
            *mctx->out_barrier_mode_used = (uint8_t)active_mode;
        // Shadow event counter: increment when mode is BOTH_*_DRIVES
        // (MODE_F_SHADOW_ACTIVE set). Counter is monotonic; operator
        // gauges shadow-data accumulation for offline A/B analysis.
        // Full shadow ring write (records[]) deferred to v5.15.6 — for
        // v5.15.5.A we just count events; ring infra lands when the
        // first telemetry consumer needs it.
        if (mctx->barrier_shadow_event_count &&
            (mode_flags & MODE_F_SHADOW_ACTIVE) && blend_dispatch_ready) {
            (*mctx->barrier_shadow_event_count)++;
        }
    }
    Money entry_m = Money_FromBinary(entry_price);  // D-170 ingress
    Money tp_amount = Money_Mul(entry_m, tp_pct);
    Money sl_amount = Money_Mul(entry_m, sl_pct);

    // volume gate
    FPN_Binary<F> volume_threshold = FPN_Mul(rolling->volume_avg, core_cfg->volume_multiplier);

    // sizing — base trade size before barrier modulation
    Money trade_size = Money_Zero();
    Money entry_ml = Money_FromBinary(entry_price);  // D-170 ingress
    if (!Money_IsZero(entry_ml)) {
        trade_size = Money_Div(allocated_balance, entry_ml);
    }

    // Phase 6prep sharded c15: confidence-damped threshold. When confidence_enabled,
    // the effective entry threshold is base * (scale - conf), clamped to <= 1.0.
    // A noise-floor model (conf ≈ CONFIDENCE_MIN_IC_DEFAULT) gives effective ≈
    // base * (2.0 - 0.01) ≈ 2*base — gate stays cold. Real signal pushes conf
    // toward 1.0 → effective approaches 0 → gate fires more readily. The
    // unconditional clamp to 1.0 prevents a perverse "always blocked" state if
    // base+scale combine to >1.0 with low conf. Mirrors the legacy formula at
    // PortfolioController.hpp:~1614.
    double base_threshold = FPN_ToDouble(core_cfg->ml_buy_threshold);
    double conf_now = 0.0;
    double threshold = base_threshold;
    // v5.14.9.B.0 — read confidence_enabled gate from cached per-core state
    // when wired; fall back to inline cfg-flag for legacy/test callers.
    bool _conf_gate = gate_state
        ? BITMAP_IS_SET(gate_state->flags, MASK_CONFIDENCE_ENABLED)
        : (BITMAP_IS_SET(core_cfg->ml_cfg_flags, MASK_ML_CFG_CONFIDENCE_ENABLED) != 0);
    if (_conf_gate && conf_scorer) {
        // v5.14.1.B — cfg-gated swap to 4-factor composite confidence.
        // Default (composite_enabled=0) preserves bytewise-identical
        // pre-v5.14.1 behavior. Composite path uses wall-clock now_us
        // for freshness; data_age=0 in legacy path keeps freshness=1.0.
        // v5.14.9.B.0 — read composite_enabled gate from cached state when wired
        bool _comp_gate = gate_state
            ? BITMAP_IS_SET(gate_state->flags, MASK_COMPOSITE_ENABLED)
            : (BITMAP_IS_SET(core_cfg->ml_cfg_flags, MASK_ML_CFG_CONFIDENCE_COMPOSITE_ENABLED) != 0);
        if (_comp_gate) {
            // v5.14.1.B.2 (PARITY-001) — now_us passed in by caller. Live:
            // clock_gettime at slow-path entry (non-deterministic OK; live
            // has no determinism contract). Backtest: tick.timestamp via
            // per-core RebuildOneCore call chain (deterministic; same
            // CSV → same now_us across replays).
            // Composite formula reads freshness from last UpdateAndMark
            // + capacity from current_adv + stability normalized vs
            // cfg.confidence_rmse_baseline. Hot-cfg fields are pushed
            // into the scorer at boot via ConfidenceScorer_BindCompositeCfg
            // (EngineSharded_Init / PortfolioController init in v5.14.1.B.1).
            conf_now = ConfidenceScorer_ComputeComposite(conf_scorer, now_us);
        } else {
            conf_now = ConfidenceScorer_Compute(conf_scorer, 0.0);  // data_age=0 (live)
        }
        double scale = FPN_ToDouble(core_cfg->confidence_threshold_scale);
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
    double hard_floor = FPN_ToDouble(core_cfg->confidence_hard_block_threshold);
    // v5.14.9.B.0 — same cached gate as the threshold-damping check above.
    if (_conf_gate && hard_floor > 0.0 && conf_now < hard_floor) {
        out->bg_price_threshold   = Money_Zero();
        out->bg_volume_threshold  = Money_Zero();
        out->sg_take_profit_price = Money_Zero();
        out->sg_stop_loss_price   = Money_Zero();
        out->tp_pct               = Money_Zero();
        out->sl_pct               = Money_Zero();
        out->trade_size           = Money_Zero();
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
    Money gate_price = Money_Zero();  // default: zero-gate (no entry)

    if (BITMAP_IS_SET(core_cfg->gate_cfg_flags, MASK_GATE_CFG_BARRIER_GATE_ENABLED)) {
        BarrierGateResult bg = BarrierGate_Compute(p_peak, p_valley);
        // hard block if either: barrier says "imminent peak" OR prediction below threshold
        if (!bg.blocked && prediction >= threshold) {
            gate_price = entry_m;
            // soft modulation: scale position by gate strength [g_min, 1.0]
            trade_size = Money_Mul(trade_size, Money{ money_from_double_payload(bg.gate) });
        }
    } else {
        // legacy binary threshold path (backward compat when barrier_gate_enabled=0)
        if (prediction >= threshold) {
            gate_price = entry_m;
        }
    }

    // Phase 6prep sharded c13/c15: outparams for snapshot + drainer.
    // out_prediction is snapshotted to ctx->active_prediction at entry submit.
    // out_confidence is read by ShardedSnapshot for ML observability.
    if (out_prediction)  *out_prediction  = prediction;
    if (out_confidence)  *out_confidence  = conf_now;

    // v5.14.9.B — soft risk degradation ladder (replaces v5.12.1.D broken math).
    //
    // The v5.12.1.D math compared conf_now (composite scale ∈ [0.001, 0.3])
    // against ml_buy_threshold (∈ [0.5, 0.7]) and silently blocked entries
    // when composite_enabled=1 (factor=0 always). v5.14.9 ships
    // FOREACH_DEGRADATION_CURVE registry + per-core gate_state cache so the
    // ladder operates on composite confidence's actual scale via operator-
    // tunable thresholds.
    //
    // Read MASK_LADDER_ACTIVE from gate_state (populated upstream by
    // SLOW_PATH_GATE_AUTOPOPULATE_PER_CORE). MASK_LADDER_ACTIVE = (curve != OFF
    // AND composite_enabled). When inactive: factor=1.0 (preserves pre-v5.14.9
    // behavior bytewise). When active: dispatch to FOREACH_DEGRADATION_CURVE
    // compute fn (branchless; cmov + fma). Factor=0 (ladder bottom) emits
    // SHALT_LOW_CONFIDENCE + early-return for entry-log/ML-Status attribution
    // (same shape as v5.9.1 confidence_hard_block_threshold path above).
    double factor = 1.0;
    if (gate_state && BITMAP_IS_SET(gate_state->flags, MASK_LADDER_ACTIVE)) {
        factor = Confidence_DegradationScale(
            core_cfg->risk_degradation_curve,
            conf_now,
            FPN_ToDouble(core_cfg->risk_full_size_threshold),
            FPN_ToDouble(core_cfg->risk_min_size_threshold),
            FPN_ToDouble(core_cfg->risk_min_size_pct));
    }
    // Surface the per-cycle factor for PerCoreSnap.ml_confidence_factor
    // observability. nullptr-safe: legacy callers without the wiring skip.
    if (mctx && mctx->out_confidence_factor) {
        *mctx->out_confidence_factor = factor;
    }
    // Ladder-bottom emission: factor=0 means "operator policy floor breached"
    // (composite confidence below cfg.risk_min_size_threshold). Same gate-
    // zeroing + SHALT path as the v5.9.1 hard-floor block above so the entry
    // log + ML Status panel attribute the block correctly. Returns early
    // (preserves the explicit SHALT pattern; readability beats marginal
    // branchless gain at a single slow-path site per CLAUDE.md item 18(b)).
    if (factor == 0.0 && gate_state && BITMAP_IS_SET(gate_state->flags, MASK_LADDER_ACTIVE)) {
        out->bg_price_threshold   = Money_Zero();
        out->bg_volume_threshold  = Money_Zero();
        out->sg_take_profit_price = Money_Zero();
        out->sg_stop_loss_price   = Money_Zero();
        out->tp_pct               = Money_Zero();
        out->sl_pct               = Money_Zero();
        out->trade_size           = Money_Zero();
        out->strategy_id          = STRATEGY_ML;
        out->flags                = GATE_FLAG_BUY_BLOCKED;
        for (int i = 0; i < 6; ++i) out->_pad[i] = 0;
        if (mctx && mctx->out_strategy_halt_reason)
            *mctx->out_strategy_halt_reason = SHALT_LOW_CONFIDENCE;
        return;
    }
    // Normal ladder path: scale trade_size by factor (∈ [min_pct, 1.0] when
    // ladder active; 1.0 when inactive). Hard cap at original size; never
    // upsize. FPN_Binary multiply preserves accounting precision.
    trade_size = Money_Mul(trade_size, Money{ money_from_double_payload(factor) });

    out->bg_price_threshold   = gate_price;
    out->bg_volume_threshold  = Money_FromBinary(volume_threshold);  // D-170 egress
    out->sg_take_profit_price = Money_Add(entry_ml, tp_amount);
    out->sg_stop_loss_price   = Money_Sub(entry_ml, sl_amount);
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
    const PerCoreCfg<F>* core_cfg,
    Money allocated_balance,
    GateParameters<F>* out,
    const RollingStats<F, WL>* rolling_long = nullptr,
    void* model_ctx = nullptr,
    void* strategy_state = nullptr,  // v5.4.0 Phase 2.x — typed-cast inside each branch
    uint8_t* strategy_halt_reason = nullptr,  // v5.6.2 — when non-null, dispatcher
                                              // writes SHALT_* codes for BUY_BLOCKED
                                              // paths (fee-floor, cost-gate) and a
                                              // post-pass for strategy zero-gates
                                              // that didn't set a specific code.
                                              // Caller resets to SHALT_OK before call.
    uint64_t now_us = 0,  // v5.14.1.B.2 (PARITY-001) — passed through to
                          // ML_BuildParameters for composite confidence
                          // freshness. Live: clock_gettime at slow-path
                          // entry. Backtest: deterministic tick.timestamp.
                          // Default 0 keeps non-ML strategies + legacy
                          // callers unchanged.
    int poll_interval_ticks = 100  // v5.15.5.F.4c.3 WIP2c.2 (Class 25 closure) —
                                    // caller-resolved scalar arg for tick→time
                                    // conversion in ML_BuildParameters. Caller
                                    // pre-resolves from cfg.poll_interval (global)
                                    // + passes here. Default 100 matches the
                                    // global cfg default; preserves backward-
                                    // compat for non-ML strategies + legacy
                                    // callers.
) {
    switch (strategy_id) {
        case STRATEGY_SIMPLE_DIP:
            // v5.4.0 Phase 2.1 — pass typed state through. nullptr is the
            // legacy contract (test paths, AUTO cores pre-Phase 3) and
            // produces identical numerics to pre-Phase 2.1.
            SimpleDip_BuildParameters(rolling, core_cfg, allocated_balance, out, rolling_long,
                                       (SimpleDipState<F>*)strategy_state);
            break;
        case STRATEGY_MEAN_REVERSION:
            // v5.4.0 Phase 2.2 — pass typed MR state through.
            MeanReversion_BuildParameters(rolling, core_cfg, allocated_balance, out,
                                           (MeanReversionState<F>*)strategy_state);
            break;
        case STRATEGY_MOMENTUM:
            // v5.4.0 Phase 2.3 — pass typed Momentum state through.
            Momentum_BuildParameters(rolling, core_cfg, allocated_balance, out,
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
                if (!Money_IsZero(core_cfg->momentum_min_tp_margin_pct) &&
                    !Money_IsZero(out->tp_pct) &&
                    Money_Lt(out->tp_pct, core_cfg->momentum_min_tp_margin_pct)) {
                    out->flags |= GATE_FLAG_BUY_BLOCKED;
                    blocked = true;
                    if (strategy_halt_reason && *strategy_halt_reason == SHALT_OK) {
                        *strategy_halt_reason = SHALT_MOM_TP_TOO_TIGHT;
                    }
                }
                // Filter 2: R² minimum (short-window regression fit).
                // Low R² = noisy data = breakout is probably noise.
                if (!blocked && !FPN_IsZero(core_cfg->momentum_min_r2) &&
                    FPN_LessThan(rolling->price_r_squared, core_cfg->momentum_min_r2)) {
                    out->flags |= GATE_FLAG_BUY_BLOCKED;
                    blocked = true;
                    if (strategy_halt_reason && *strategy_halt_reason == SHALT_OK) {
                        *strategy_halt_reason = SHALT_MOM_LOW_R2;
                    }
                }
                // Filter 3: recent flow agreement (volume_delta).
                // Reject when recent flow opposes the breakout direction.
                if (!blocked && !FPN_IsZero(core_cfg->momentum_min_buy_delta_recent) &&
                    FPN_LessThan(rolling->volume_delta,
                                  core_cfg->momentum_min_buy_delta_recent)) {
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
                (void)core_cfg->momentum_require_last_win;  // not yet wired
            }
            break;
        case STRATEGY_EMA_CROSS:
            // v5.4.0 Phase 2.4 — pass typed EmaCross state through.
            EmaCross_BuildParameters(rolling, core_cfg, allocated_balance, out,
                                      (EmaCrossState<F>*)strategy_state);
            break;
        case STRATEGY_ML:
            // v5.14.1.B.2 (PARITY-001) — now_us threaded through for
            // composite confidence replay-determinism.
            ML_BuildParameters(rolling, rolling_long, core_cfg, allocated_balance, out, model_ctx, now_us, poll_interval_ticks);
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
    if (BITMAP_IS_SET(core_cfg->lifecycle_cfg_flags, MASK_LIFECYCLE_CFG_PARTIAL_EXIT_ENABLED)) {
        out->flags |= GATE_FLAG_PAIR_ACTIVE;
        // tp_pct_b = tp_pct * tp2_mult. Falls back to tp_pct (TP1 distance,
        // i.e. leg B duplicates leg A) when tp2_mult is zero (defensive —
        // strategy ought to have set tp_pct, but if not, leg B is a no-op).
        if (!Money_IsZero(core_cfg->tp2_mult) && !Money_IsZero(out->tp_pct)) {
            out->tp_pct_b = Money_Mul(out->tp_pct, core_cfg->tp2_mult);
        } else {
            out->tp_pct_b = out->tp_pct;
        }
    } else {
        // Explicit clear when disabled — guarantees pre-P.4 callers
        // (or a re-used GateParameters instance) see tp_pct_b == 0 +
        // GATE_FLAG_PAIR_ACTIVE clear, regardless of prior state.
        out->flags &= ~GATE_FLAG_PAIR_ACTIVE;
        out->tp_pct_b = Money_Zero();
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
        Money fee_taker = !Money_IsZero(core_cfg->fee_rate_taker)
            ? core_cfg->fee_rate_taker : core_cfg->fee_rate;
        Money three = Money_FromInt(3);
        Money floor_pct = Money_Mul(fee_taker, three);
        // out->tp_pct may be zero if strategy didn't set it (e.g.
        // STRATEGY_NONE fallthrough). Skip the gate in that case —
        // the strategy itself has already produced a no-op result.
        if (!Money_IsZero(out->tp_pct) && Money_Lt(out->tp_pct, floor_pct)) {
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
    if (BITMAP_IS_SET(core_cfg->gate_cfg_flags, MASK_GATE_CFG_COST_GATE_ENABLED) && !Money_IsZero(out->tp_pct) &&
        !Money_IsZero(out->bg_price_threshold) && rolling) {
        double price = FPN_ToDouble(rolling->price_avg);
        double rel_vol = (price > 0.01)
            ? FPN_ToDouble(rolling->price_stddev) / price : 0.0;
        if (rel_vol > 0.0) {
            // order size and ADV in dollars. ADV approximated as
            // volume_avg × price × 1440 (tick-rate samples-per-day proxy).
            double order_size_d = Money_ToDouble(out->trade_size) *
                                  Money_ToDouble(out->bg_price_threshold);
            double adv_d = FPN_ToDouble(rolling->volume_avg) * price * 1440.0;
            TradingCosts c = CostModel_Estimate(
                /*spread_bps=*/0.0,    // omitted — see comment above
                rel_vol,
                /*horizon_minutes=*/5.0,
                order_size_d, adv_d,
                COST_K1_DEFAULT, COST_K2_DEFAULT, COST_K3_DEFAULT);
            // Veto when total cost exceeds 50% of expected gain (tp_bps).
            // Conservative threshold; can be made cfg-tunable later.
            double tp_bps = Money_ToDouble(out->tp_pct) * 10000.0;
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
    if (BITMAP_IS_SET(core_cfg->ml_cfg_flags, MASK_ML_CFG_FOXML_VOL_SCALING_ENABLED) && !Money_IsZero(out->trade_size) &&
        !Money_IsZero(out->tp_pct) && rolling) {
        double price = FPN_ToDouble(rolling->price_avg);
        double rel_vol = (price > 0.01)
            ? FPN_ToDouble(rolling->price_stddev) / price : 0.0;
        if (rel_vol > 0.0) {
            double alpha = Money_ToDouble(out->tp_pct);
            double z_max = !FPN_IsZero(core_cfg->foxml_vol_scaling_z_max)
                ? FPN_ToDouble(core_cfg->foxml_vol_scaling_z_max)
                : VOL_SCALER_Z_MAX_DEFAULT;
            // max_weight=1.0 → VolScaler returns weight in [0, 1] which we
            // multiply trade_size by. Effectively: weight=1 means no
            // change; weight<1 means smaller position in high-vol regime.
            double weight = VolScaler_Size(alpha, rel_vol, z_max, /*max_weight=*/1.0);
            if (weight > 0.0 && weight < 1.0) {
                out->trade_size = Money_Mul(out->trade_size,
                                           Money{ money_from_double_payload(weight) });
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
        Money_IsZero(out->bg_price_threshold) &&
        !(out->flags & GATE_FLAG_BUY_BLOCKED)) {
        *strategy_halt_reason = SHALT_NO_SIGNAL;
    }
}

}  // namespace tt
