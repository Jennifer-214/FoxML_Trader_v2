// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [LEGACY REFERENCE DRIVER]
//
// Phase 13 of the per-core sharded engine. Single-threaded reference driver
// that mimics the production single-threaded engine's hot path WITHOUT any of
// the per-core machinery (SPSC, seqlock, events, fan-out). Used as the
// baseline for the head-to-head comparison test.
//
// Why a reference driver instead of using the actual production engine:
//   The experiment lives in its own worktree and doesn't link against the
//   full production PortfolioController + RegimeDetector + RollingStats +
//   RegressionFeederX stack. Building the reference here lets us validate
//   that the SHARDED path produces identical trade decisions to a STRAIGHT
//   single-threaded implementation of the same gate logic, using the same
//   gate functions (BG_Evaluate / SG_Evaluate) and the same fee model.
//
//   This is a structural validation. It doesn't replace the eventual real
//   head-to-head test against production legacy (that happens in the
//   production migration PR), but it catches divergence in our own code
//   before that PR ever gets written.
//
// What it does per tick:
//   1. Push tick into RollingStats (for slow-path parameter rebuilds)
//   2. Walk all "positions" (in this stripped-down ref, just the slots)
//   3. For inactive slots: evaluate BG using the SAME BG_Evaluate function
//      the per-core path uses. If fires, open the position.
//   4. For active slots: evaluate SG using SG_Evaluate. If fires, close
//      and book P&L.
//   5. On slow path cadence: rebuild parameters via Strategy_BuildParameters
//      (the SAME dispatcher the per-core path uses) and apply.
//
// What it does NOT do (intentionally):
//   - Trade events (the legacy path mutates state directly)
//   - SPSC rings
//   - Seqlocks
//   - Branchless masking (uses straight if/else for clarity)
//
// The KEY invariant: for the same input ticks + same starting params + same
// fee model, this MUST produce the same trade decisions as the sharded
// path. If they differ, there's a bug in the sharded path.
//======================================================================================================

#pragma once

#include "../FixedPoint/FixedPointN.hpp"
#include "../ML_Headers/RollingStats.hpp"
#include "../Strategies/StrategyParameters.hpp"
#include "GateParameters.hpp"
#include "Tick.hpp"

#include <cstdint>

namespace tt {

//======================================================================================================
// [REFERENCE STATE]
//======================================================================================================
// One "core equivalent" — holds active flag + entry price + per-slot params.
// Sized to MAX_LEGACY_REF_SLOTS so we can run with N parallel-ish slots and
// compare against the sharded path with N execution cores.
//======================================================================================================
constexpr int MAX_LEGACY_REF_SLOTS = 16;

template <unsigned F>
struct LegacyRefSlot {
    GateParameters<F> params;       // gate thresholds
    FPN<F>            entry_price;  // 0 when inactive
    FPN<F>            quantity;     // size of the open position
    FPN<F>            entry_fee;    // fee paid at entry
    uint8_t           active;       // 1 = position open
    uint8_t           permission;   // 1 = allowed to take new entries
    uint8_t           strategy_id;
    uint8_t           _pad[5];
    uint64_t          entries;      // counter
    uint64_t          exits;        // counter
};

template <unsigned F>
struct LegacyReferenceState {
    LegacyRefSlot<F> slots[MAX_LEGACY_REF_SLOTS];
    int num_slots;
    FPN<F> balance;
    FPN<F> realized_pnl;
    FPN<F> fee_rate;
    FPN<F> allocated_balance_per_slot;  // for parameter rebuild sizing
    uint64_t total_entries;
    uint64_t total_exits;
};

template <unsigned F>
inline void LegacyReference_Init(LegacyReferenceState<F>* state,
                                  FPN<F> starting_balance,
                                  FPN<F> fee_rate,
                                  FPN<F> allocated_balance_per_slot) {
    state->num_slots = 0;
    state->balance = starting_balance;
    state->realized_pnl = FPN_Zero<F>();
    state->fee_rate = fee_rate;
    state->allocated_balance_per_slot = allocated_balance_per_slot;
    state->total_entries = 0;
    state->total_exits = 0;
    for (int i = 0; i < MAX_LEGACY_REF_SLOTS; ++i) {
        GateParameters_Init(&state->slots[i].params);
        state->slots[i].entry_price = FPN_Zero<F>();
        state->slots[i].quantity    = FPN_Zero<F>();
        state->slots[i].entry_fee   = FPN_Zero<F>();
        state->slots[i].active      = 0;
        state->slots[i].permission  = 0;
        state->slots[i].strategy_id = STRATEGY_NONE;
        state->slots[i].entries     = 0;
        state->slots[i].exits       = 0;
    }
}

template <unsigned F>
inline int LegacyReference_AddSlot(LegacyReferenceState<F>* state, uint8_t strategy_id) {
    if (state->num_slots >= MAX_LEGACY_REF_SLOTS) return -1;
    int slot = state->num_slots++;
    state->slots[slot].strategy_id = strategy_id;
    state->slots[slot].permission = 1;  // armed by default in the reference
    return slot;
}

//======================================================================================================
// [REFERENCE TICK]
//======================================================================================================
// Walk every slot. For each one: evaluate BG (if not active) or SG (if
// active), update state accordingly. The gate evaluations use the SAME
// BG_Evaluate / SG_Evaluate functions the sharded path uses, so any
// divergence in trade decisions has to come from state management, not gate
// logic.
//======================================================================================================
template <unsigned F>
inline void LegacyReference_Tick(LegacyReferenceState<F>* state, const Tick<F>& tick) {
    for (int i = 0; i < state->num_slots; ++i) {
        LegacyRefSlot<F>* slot = &state->slots[i];

        // Inactive slot: check buy gate
        if (!slot->active) {
            if (slot->permission && BG_Evaluate(tick, &slot->params)) {
                // open position
                slot->entry_price = tick.price;
                slot->quantity    = slot->params.trade_size;
                FPN<F> notional = FPN_Mul(tick.price, slot->params.trade_size);
                slot->entry_fee   = FPN_Mul(notional, state->fee_rate);
                slot->active      = 1;
                slot->entries++;
                state->total_entries++;
            }
            continue;
        }

        // Active slot: check sell gate
        if (SG_Evaluate(tick.price, slot->entry_price, &slot->params)) {
            // close position, book P&L
            FPN<F> diff  = FPN_Sub(tick.price, slot->entry_price);
            FPN<F> gross = FPN_Mul(diff, slot->quantity);
            FPN<F> exit_notional = FPN_Mul(tick.price, slot->quantity);
            FPN<F> exit_fee = FPN_Mul(exit_notional, state->fee_rate);
            FPN<F> total_fee = FPN_Add(slot->entry_fee, exit_fee);
            FPN<F> net = FPN_Sub(gross, total_fee);
            state->balance = FPN_Add(state->balance, net);
            state->realized_pnl = FPN_Add(state->realized_pnl, net);
            slot->active = 0;
            slot->exits++;
            state->total_exits++;
        }
    }
}

//======================================================================================================
// [REFERENCE SLOW PATH]
//======================================================================================================
// Rebuild parameters from RollingStats via the same Strategy_BuildParameters
// dispatcher the sharded path uses. Apply directly to each slot's params
// (no seqlock — single threaded, no race).
//======================================================================================================
template <unsigned F, unsigned W>
inline void LegacyReference_SlowPath(LegacyReferenceState<F>* state,
                                      const RollingStats<F, W>* rolling,
                                      const ControllerConfig<F>* config) {
    for (int i = 0; i < state->num_slots; ++i) {
        if (state->slots[i].strategy_id == STRATEGY_NONE) continue;
        // v5.15.5.F.4c.3 WIP2c.2 — per-core single-param sig; legacy single-core
        // path uses cores[0]. poll_interval pre-resolved from global cfg.
        Strategy_BuildParameters(state->slots[i].strategy_id,
                                  rolling, &config->cores[0],
                                  state->allocated_balance_per_slot,
                                  &state->slots[i].params,
                                  /*rolling_long*/ nullptr,
                                  /*model_ctx*/ nullptr,
                                  /*strategy_state*/ nullptr,
                                  /*strategy_halt_reason*/ nullptr,
                                  /*now_us*/ 0,
                                  /*poll_interval_ticks*/ (int)config->poll_interval);
    }
}

//======================================================================================================
// [REFERENCE RUN]
//======================================================================================================
// Convenience wrapper: run a tick stream end to end with slow path on cadence.
// Mirrors ShardedBacktest_Run so the comparison harness can call them
// symmetrically.
//======================================================================================================
template <unsigned F, unsigned W = 128>
inline void LegacyReference_Run(LegacyReferenceState<F>* state,
                                 RollingStats<F, W>* rolling,
                                 const ControllerConfig<F>* config,
                                 const Tick<F>* ticks,
                                 int num_ticks,
                                 int slow_path_interval) {
    for (int i = 0; i < num_ticks; ++i) {
        if (rolling) RollingStats_Push(rolling, ticks[i].price, ticks[i].volume);
        LegacyReference_Tick(state, ticks[i]);
        if (((i + 1) % slow_path_interval) == 0 && rolling && config) {
            LegacyReference_SlowPath(state, rolling, config);
        }
    }
}

}  // namespace tt
