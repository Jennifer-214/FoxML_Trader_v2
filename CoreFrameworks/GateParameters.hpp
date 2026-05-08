// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [GATE PARAMETERS]
//
// Pure parameter pack that the controller computes on its slow path and pushes
// to each execution core. Contains EVERYTHING the buy gate and sell gate need
// to evaluate. No global state, no Portfolio reads, no RollingStats access.
//
// The execution core reads this pack via the parameter slot (phase 05) and the
// hot path BG_Evaluate / SG_Evaluate functions take it as their only context.
// This is what makes the per-core architecture possible — the execution core
// can do its job without consulting any cross-core state.
//
// To add a new gate input:
//   1. Add the field here
//   2. Update Strategy_BuildParameters in the strategy that uses it
//   3. Update BG_Evaluate or SG_Evaluate to read it
//   4. Bump SNAPSHOT_VERSION (params are persisted in v11+)
//
// Flags field encodes which gate behaviors are active. Each bit is a named
// constant — never use numeric literals when checking flags.
//======================================================================================================

#pragma once

#include "../FixedPoint/FixedPointN.hpp"
#include "../Strategies/StrategyInterface.hpp"
#include "Tick.hpp"
#include <cstdint>
#include <type_traits>

namespace tt {

// Gate behavior flags. Each is one bit in GateParameters<F>::flags.
constexpr uint8_t GATE_FLAG_TP_ENABLED       = 0x01;
constexpr uint8_t GATE_FLAG_SL_ENABLED       = 0x02;
constexpr uint8_t GATE_FLAG_TRAILING_ENABLED = 0x04;
constexpr uint8_t GATE_FLAG_VOLUME_REQUIRED  = 0x08;
// v4.0: gate fires when tick.price >= bg_price_threshold (momentum buy-above)
// instead of <= (mean-reversion buy-below). Selected branchlessly in
// BG_Evaluate / ExecutionCore_Tick. Pre-v4.0 the hot path was buy-below only,
// silently breaking MOM strategy in sharded mode.
constexpr uint8_t GATE_FLAG_BUY_ABOVE        = 0x10;
// Track E.3 (2026-04-26): slow-path veto for the buy gate. When set, BG
// fires evaluate to false regardless of price/volume. Used by the
// book_imbalance gate (ControllerEventLoop.hpp) and any future slow-path
// "do not buy right now" decision that needs to mask without revoking
// permission (revoking would also disable kill-switch reset / warmup
// state machine — those are separate concerns).
//
// Why a flag and not a permission flip:
//   - permission is a single bit owned by the controller's lifecycle
//     (warmup → trading → kill-switch). Layering a third source on it
//     races during the kill-switch reset path.
//   - flag is a per-rebuild snapshot — recomputed every slow path. No
//     persistent state to corrupt; if the slow path forgets to set it,
//     the gate naturally re-opens.
constexpr uint8_t GATE_FLAG_BUY_BLOCKED      = 0x20;
// Partial exits P.2 (2026-04-27): when set, on entry the hot path opens
// BOTH leg A (TP=tp_pct, slot=core_id*2+0) AND leg B (TP=tp_pct_b,
// slot=core_id*2+1). Both share live_sl. Set by Strategy_BuildParameters
// when cfg.partial_exit_enabled=1 (P.4); cleared otherwise. With this
// flag set, ExecutionCore_Tick branchlessly evaluates SG on both legs;
// either or both can fire on a given tick. With it clear, leg-B fields
// are never written and active_b stays 0 → leg-B SG result is masked
// out, hot path costs ~1ns extra (the unused FPN comparisons pipeline
// into otherwise-idle CPU slots).
constexpr uint8_t GATE_FLAG_PAIR_ACTIVE      = 0x40;
// v5.12.1.B.3 (2026-05-08): hot-path staleness gate. When set, the
// execution core compares (tick.sequence - publish_tick) against
// GateParameters.param_max_age_ticks; if the gap exceeds the threshold,
// bg_fires is masked off and SHALT_PARAM_STALE is surfaced via
// strategy_halt_reason. publish_tick = 0 (warmup) skips the check
// regardless of this flag. Slow-path sets this flag based on
// cfg.param_staleness_gate_enabled in EventLoop_RebuildOneCore.
constexpr uint8_t GATE_FLAG_STALENESS_ENABLED = 0x80;

// Strategy IDs come from Strategies/StrategyInterface.hpp (single source of
// truth shared with the legacy strategies). STRATEGY_NONE = 0xFF means
// "this core has no assigned strategy, do not trade".

template <unsigned F>
struct alignas(64) GateParameters {
    // --- Buy gate inputs ---
    FPN<F> bg_price_threshold;       // tick.price must be < this to enter
    FPN<F> bg_volume_threshold;      // tick.volume must be > this (when GATE_FLAG_VOLUME_REQUIRED)

    // --- Sell gate inputs (PRECOMPUTED — backward compatibility / tests) ---
    // The execution core uses these absolute prices directly when both
    // tp_pct AND sl_pct are zero. When either pct is non-zero, the core
    // computes TP/SL from the actual fill price using the percentages
    // instead — this is the "per-fill" path from phase 14.
    FPN<F> sg_take_profit_price;     // legacy absolute TP
    FPN<F> sg_stop_loss_price;       // legacy absolute SL

    // --- Sell gate inputs (PER-FILL, phase 14 — the right way) ---
    // Strategies set tp_pct and sl_pct as percentages of fill price. The
    // execution core computes absolute TP/SL on entry against the actual
    // fill price, not the controller's expected entry. Fixes the structural
    // loss bias from phase 13 head-to-head.
    FPN<F> tp_pct;                   // 0.005 = 0.5% TP. zero → use sg_take_profit_price
    FPN<F> sl_pct;                   // 0.0025 = 0.25% SL. zero → use sg_stop_loss_price
    // Partial exits P.2: leg-B TP percentage (used only when
    // GATE_FLAG_PAIR_ACTIVE is set). Strategy_BuildParameters typically
    // sets tp_pct_b = tp_pct * cfg.tp2_mult (TP2 farther than TP1). Leg B
    // shares the same live_sl as leg A. Zero-defaults when partials
    // disabled, in which case the entry path won't activate leg B
    // regardless.
    FPN<F> tp_pct_b;

    // --- Sizing (controller-set, not used by gate evaluation directly) ---
    FPN<F> trade_size;               // size for the next entry, written to Position by controller

    // --- v4.0.3 D9 trailing SL ratchet ---
    // Controller writes via standard slow-path PushParameters (seqlock-protected).
    // Hot path uses effective_sl = FPN_Max(active_sl, ratchet_sl) — branchless,
    // FPN-pure. When zero (default), FPN_Max(sl, 0) = sl so no behavior change.
    // When non-zero, acts as a floor for SL — exit fires when price drops to
    // this level even if the original live_sl was lower. Mirrors legacy
    // PortfolioController trailing behavior; reaction time bounded by
    // slow_path_interval (~100-200ms) for the SET, microseconds for the FIRE.
    FPN<F> ratchet_sl;

    // --- v5.4.0 Phase 3.3 trailing TP ratchet ---
    // Parallel channel to ratchet_sl. For LONG positions, ratcheting TP UP
    // locks in a higher exit target as the trade runs. Hot path uses
    // effective_tp = FPN_Max(active_tp, ratchet_tp) — same FPN_Max pattern
    // as SL. When zero (default), no behavior change. Used by Regime_AdjustPositions
    // (Phase 3.1) to widen TP on RANGING→TRENDING transitions and by future
    // strategy-specific TP trailing in Phase 4+. Pre-v5.4 sharded had no TP
    // ratchet field; the legacy Regime_AdjustPositions writes to pos->take_profit_price
    // were dead — postmortem F4.
    FPN<F> ratchet_tp;

    // --- Identification ---
    uint8_t strategy_id;             // STRATEGY_* constant
    uint8_t flags;                   // GATE_FLAG_* bitmask
    uint8_t _pad[6];                 // pad to 8-byte alignment for the field below

    // v5.12.1.B.3 — hot-path staleness gate threshold. When
    // GATE_FLAG_STALENESS_ENABLED is set on flags, the execution core checks
    // (tick.sequence - cached_publish_tick) > param_max_age_ticks and masks
    // off bg_fires when stale. Filled by EventLoop_RebuildOneCore from
    // cfg.param_max_age_ticks. Branchless mask compute on hot path; ~3-5ns.
    uint64_t param_max_age_ticks;
};

static_assert(std::is_trivially_copyable<GateParameters<64>>::value, "GateParameters<64> must be trivially copyable");
static_assert(alignof(GateParameters<64>) >= 64, "GateParameters<64> must be cache-line aligned");

//======================================================================================================
// Stub gate evaluators for phase 02. The real implementations come from phase 06
// (strategy parameter refactor) which extracts pure gate functions from the existing
// OrderGates.hpp. For phase 02 we just need the signatures + a working stub so
// ExecutionCore_Tick compiles and tests can verify branchlessness.
//
// These stubs are correct but minimal: BG fires when price < threshold, SG fires
// when price >= TP_price OR price <= SL_price. Real strategies will produce more
// sophisticated parameter packs but the same gate evaluators.
//======================================================================================================

template <unsigned F>
__attribute__((always_inline))
static inline bool BG_Evaluate(const Tick<F>& tick, const GateParameters<F>* params) {
    // Branchless price check — selects buy-below (price < threshold, MR/DIP/EMA/ML)
    // or buy-above (price > threshold, MOM) based on GATE_FLAG_BUY_ABOVE.
    // Both comparisons computed unconditionally; mask selects the active one.
    uint64_t price_below = (uint64_t)FPN_LessThan(tick.price, params->bg_price_threshold);
    uint64_t price_above = (uint64_t)FPN_GreaterThan(tick.price, params->bg_price_threshold);
    uint64_t buy_above   = (uint64_t)((params->flags & GATE_FLAG_BUY_ABOVE) != 0);
    uint64_t price_ok    = (price_above & buy_above) | (price_below & ~buy_above);
    uint64_t volume_ok   = (uint64_t)FPN_GreaterThan(tick.volume, params->bg_volume_threshold);
    uint64_t volume_required = (uint64_t)((params->flags & GATE_FLAG_VOLUME_REQUIRED) != 0);
    uint64_t volume_check = (volume_required & volume_ok) | (~volume_required & 1ULL);
    // Track E.3: slow-path veto. When GATE_FLAG_BUY_BLOCKED is set, force
    // bg_fires to 0. Branchless — blocked_mask is ALL_ONES when blocked,
    // 0 when open. AND with ~blocked_mask drops the gate when vetoed,
    // passes through when open. ~1ns added.
    uint64_t blocked      = (uint64_t)((params->flags & GATE_FLAG_BUY_BLOCKED) != 0);
    uint64_t blocked_mask = -blocked;  // 0 or ALL_ONES
    return ((price_ok & volume_check) & ~blocked_mask) != 0;
}

template <unsigned F>
__attribute__((always_inline))
static inline bool SG_Evaluate(const FPN<F>& current_price, const FPN<F>& entry_price, const GateParameters<F>* params) {
    // Stub: TP hit OR SL hit (each gated by its enable flag)
    (void)entry_price;  // unused in stub; real implementation may use for trailing
    uint64_t tp_enabled = (uint64_t)((params->flags & GATE_FLAG_TP_ENABLED) != 0);
    uint64_t sl_enabled = (uint64_t)((params->flags & GATE_FLAG_SL_ENABLED) != 0);
    // v5.4.0 Phase 3.3: ratchet_tp / ratchet_sl raise the effective exit
    // levels (max-only). Zero defaults preserve pre-v5.4 numerics.
    FPN<F> effective_tp = FPN_Max(params->sg_take_profit_price, params->ratchet_tp);
    FPN<F> effective_sl = FPN_Max(params->sg_stop_loss_price,   params->ratchet_sl);
    uint64_t tp_hit = (uint64_t)FPN_GreaterThanOrEqual(current_price, effective_tp);
    uint64_t sl_hit = (uint64_t)FPN_LessThanOrEqual(current_price, effective_sl);
    return ((tp_enabled & tp_hit) | (sl_enabled & sl_hit)) != 0;
}

// Initialize a GateParameters pack to safe defaults. Permission=0 semantics: with
// these params + permission=0 the execution core will not trade.
template <unsigned F>
static inline void GateParameters_Init(GateParameters<F>* params) {
    params->bg_price_threshold = FPN_Zero<F>();
    params->bg_volume_threshold = FPN_Zero<F>();
    params->sg_take_profit_price = FPN_Zero<F>();
    params->sg_stop_loss_price = FPN_Zero<F>();
    params->tp_pct = FPN_Zero<F>();
    params->sl_pct = FPN_Zero<F>();
    params->tp_pct_b = FPN_Zero<F>();  // P.2: leg-B TP%, set by strategy when partials enabled
    params->trade_size = FPN_Zero<F>();
    params->ratchet_sl = FPN_Zero<F>();  // v4.0.3 D9
    params->ratchet_tp = FPN_Zero<F>();  // v5.4.0 Phase 3.3 — TP ratchet channel
    params->strategy_id = STRATEGY_NONE;
    params->flags = 0;
}

}  // namespace tt

