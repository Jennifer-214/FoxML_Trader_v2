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

    // --- Sizing (controller-set, not used by gate evaluation directly) ---
    FPN<F> trade_size;               // size for the next entry, written to Position by controller

    // --- Identification ---
    uint8_t strategy_id;             // STRATEGY_* constant
    uint8_t flags;                   // GATE_FLAG_* bitmask
    uint8_t _pad[6];                 // explicit padding for layout stability
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
    return (price_ok & volume_check) != 0;
}

template <unsigned F>
__attribute__((always_inline))
static inline bool SG_Evaluate(const FPN<F>& current_price, const FPN<F>& entry_price, const GateParameters<F>* params) {
    // Stub: TP hit OR SL hit (each gated by its enable flag)
    (void)entry_price;  // unused in stub; real implementation may use for trailing
    uint64_t tp_enabled = (uint64_t)((params->flags & GATE_FLAG_TP_ENABLED) != 0);
    uint64_t sl_enabled = (uint64_t)((params->flags & GATE_FLAG_SL_ENABLED) != 0);
    uint64_t tp_hit = (uint64_t)FPN_GreaterThanOrEqual(current_price, params->sg_take_profit_price);
    uint64_t sl_hit = (uint64_t)FPN_LessThanOrEqual(current_price, params->sg_stop_loss_price);
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
    params->trade_size = FPN_Zero<F>();
    params->strategy_id = STRATEGY_NONE;
    params->flags = 0;
}

}  // namespace tt

