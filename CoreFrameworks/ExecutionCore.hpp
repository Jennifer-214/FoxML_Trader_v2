// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [EXECUTION CORE]
//
// Per-core trading state machine for the per-core risk-sharded engine.
//
// One ExecutionCore<F> instance per pinned CPU core. Each instance owns:
//   - permission   (1 bit)   — controller-granted authorization to take new entries
//   - active       (1 bit)   — currently in a trade
//   - entry_price  (FPN<F>)  — price of the current trade entry, used by SG check
//   - gate_params  (struct)  — buy/sell gate parameter pack pushed by controller
//   - event_ring   (SPSC)    — outgoing trade events the controller drains on its slow path
//   - tick_ring*   (SPSC*)   — incoming tick stream from the market reader
//
// The execution core has NO concept of a position. No P&L. No balance. No portfolio.
// All of those live on the controller core, which learns about trades by reading
// the event ring and updates the canonical Position records on its own schedule.
//
// Per tick (target ~60ns flat on i5-1035G4 hardware, validated by phase 03 bench):
//   1. Compute BG_Evaluate AND SG_Evaluate unconditionally (CPU pipelines them
//      in parallel; no branch on active state).
//   2. Mask the results: can_enter = !active & permission & bg_fires
//                        can_exit  = active & sg_fires
//   3. If either fires, push a trade event to the event ring (rare branch,
//      predictable, almost always not-taken in steady state).
//   4. Update entry_price (CMOV) and active flag (mask op) branchlessly.
//
// The hot path is ALMOST entirely branchless. The only branch is the event push,
// which fires < 1% of ticks in normal operation and predicts perfectly. The gate
// evaluation, the state masks, and the active flag update are all branchless.
//
// To make it fully branchless: write the event slot fields unconditionally, then
// conditionally advance the ring head with `head += (can_enter | can_exit)`.
// This is what the phase 03 benchmark harness scenarios do for comparison.
// The production ExecutionCore uses SPSCRing_TryPush for correctness (overflow
// handling) and accepts the rare branch.
//======================================================================================================

#pragma once

#include "../FixedPoint/FixedPointN.hpp"
#include "CoreLatencyStats.hpp"
#include "GateParameters.hpp"
#include "ParameterSlot.hpp"
#include "SPSCRing.hpp"
#include "Tick.hpp"
#include "TradeEvent.hpp"

#include <cstdint>

namespace tt {

// Sized for the i5-1035G4 microbenchmark. Production sizing tuned per use case.
constexpr size_t EXECUTION_CORE_TICK_RING_SIZE  = 4096;
constexpr size_t EXECUTION_CORE_EVENT_RING_SIZE = 1024;

template <unsigned F>
struct alignas(64) ExecutionCore {
    // --- Hot fields (read every tick) ---
    // permission and active are single bytes; entry_price needs 8-byte alignment.
    // The explicit padding makes the layout deterministic across compilers.
    uint8_t  permission;       // controller writes (atomic), core reads
    uint8_t  active;           // core writes and reads
    uint8_t  _pad0[6];
    FPN<F>   entry_price;      // core writes on entry, reads on SG check
    // Phase 14: live TP/SL computed on actual fill, not on the controller's
    // expected entry. The execution core sets these when can_enter fires
    // using the percentage from the gate parameter pack. SG_Evaluate reads
    // them instead of params.sg_take_profit_price / sg_stop_loss_price when
    // the percentage path is active. Fixes the structural loss bias from
    // phase 13 head-to-head.
    FPN<F>   live_tp;
    FPN<F>   live_sl;

    // --- Parameter pack pushed by controller (phase 05) ---
    // Seqlock atomic slot. The controller calls ExecutionCore_SetParameters
    // (or ParameterSlot_Write) on its slow path; the hot path reads via
    // ParameterSlot_Read. The seqlock guarantees the consumer never reads
    // a torn value even if the producer writes back-to-back. See ParameterSlot.hpp
    // for the protocol details.
    ParameterSlot<GateParameters<F>> param_slot;

    // --- Cached parameter snapshot (phase 14 perf optimization) ---
    // The hot path skips the 192-byte memcpy of the parameter pack on every
    // tick by caching the last-read snapshot here. On each tick we do one
    // acquire-load of param_slot.seq and compare against cached_seq. If
    // they match (and cached_seq is even = "stable"), the cached_params are
    // still current and we use them directly. If they differ, we fall through
    // to the full ParameterSlot_Read to refresh the cache.
    //
    // Steady state: 1 acquire-load + 1 compare = ~1 ns
    // Param push:   1 full slot read = ~6 ns (one-time per push)
    //
    // Saves ~12 ns per tick vs the unconditional memcpy. Validated by
    // bench_batch_floor.cpp: cached_v2 ~35 ns vs original ~50 ns at the
    // batch floor. Single-writer (this core's hot path) so no atomics needed
    // on the cache fields themselves — they're only read by this core.
    uint64_t cached_seq;
    GateParameters<F> cached_params;

    // --- Identity ---
    uint16_t core_id;
    uint8_t  _pad1[6];

    // --- Communication rings ---
    // Event ring: this core's outbound trade events. The controller is the consumer.
    SPSCRing<TradeEvent<F>, EXECUTION_CORE_EVENT_RING_SIZE> event_ring;

    // Tick ring: this core's inbound tick stream. The market reader is the producer.
    // Pointer (not embedded) because the ring is owned by the market reader, which
    // creates one per execution core at startup.
    SPSCRing<Tick<F>, EXECUTION_CORE_TICK_RING_SIZE>* tick_ring;

    // --- Latency monitoring (Phase 13 / per-core observability) ---
    // Lives at the tail of the struct so it doesn't pollute the hot fields
    // in cache line 0. Single writer (this core's hot path), single reader
    // (the controller's snapshot helper). Disabled by default; flip the
    // enable flag from the controller to start sampling.
    CoreLatencyStats latency_stats;
};

// Initialize an execution core. Defaults: permission=0 (controller must explicitly
// grant), active=0, entry_price=0, parameter slot installed with safe defaults
// (STRATEGY_NONE so the gate evaluators won't fire even if a tick comes in
// before the controller pushes real parameters).
//
// The controller calls this once per core at startup, then later sets permission=1
// after pushing a valid strategy assignment via ExecutionCore_SetParameters.
template <unsigned F>
static inline void ExecutionCore_Init(
    ExecutionCore<F>* core,
    uint16_t core_id,
    SPSCRing<Tick<F>, EXECUTION_CORE_TICK_RING_SIZE>* tick_ring
) {
    core->permission = 0;
    core->active     = 0;
    core->entry_price = FPN_Zero<F>();
    core->live_tp    = FPN_Zero<F>();
    core->live_sl    = FPN_Zero<F>();
    core->core_id    = core_id;
    core->tick_ring  = tick_ring;
    GateParameters<F> initial;
    GateParameters_Init(&initial);
    ParameterSlot_Init(&core->param_slot, initial);
    // Cache starts as a copy of the initial pack with cached_seq = sentinel
    // that forces a real ParameterSlot_Read on the first tick (so we pick up
    // any controller writes that landed between Init and the first tick).
    core->cached_params = initial;
    core->cached_seq    = (uint64_t)-1;
    SPSCRing_Init(&core->event_ring);
    CoreLatencyStats_Init(&core->latency_stats);
}

// Atomic parameter push from the controller. Wraps ParameterSlot_Write so the
// caller doesn't need to know the slot's internals. Call this from the
// controller's slow path or from the parameter-push step of the event loop.
// Wait-free on the consumer side (execution core can keep ticking through
// the swap).
template <unsigned F>
static inline void ExecutionCore_SetParameters(
    ExecutionCore<F>* core,
    const GateParameters<F>& new_params
) {
    ParameterSlot_Write(&core->param_slot, new_params);
}

// Atomic permission set. Phase 09 (kill switch) calls this from the controller
// core to enable or revoke trading authorization on a specific execution core.
//
// Uses RELEASE semantics so the cleared (or set) permission becomes visible to
// the execution core's next ACQUIRE load on the next tick. On x86 this compiles
// to a plain store, but the explicit memory order is required for correctness
// on weakly ordered architectures and to prevent the compiler from reordering
// the store across other instructions in the controller's slow path.
//
// Pitfall P9.3: never use ATOMIC_RELAXED here. Pitfall P9.4: never call this
// with value=1 unless the core has a valid strategy assignment AND fresh gate
// parameters in its slot. The safe pattern is: SetParameters → SetPermission(1).
template <unsigned F>
static inline void ExecutionCore_SetPermission(ExecutionCore<F>* core, uint8_t value) {
    __atomic_store_n(&core->permission, value, __ATOMIC_RELEASE);
}

// The hot path. Process one tick. Branchless on the gate evaluation and state
// update; the only branch is the rare event push when a trade actually fires.
//
// MUST be force-inlined or the BG/SG evaluators won't inline either, which kills
// the latency target. Verified by phase 03 disassembly.
//
// Phase 13: optional per-core latency sampling. When core->latency_stats.enabled
// is true, the function brackets its body with rdtsc and records the cycle
// delta. The check is one relaxed atomic load plus a branch (predicted
// not-taken in the common case), so disabled cost is ~1ns. Enabled cost adds
// ~25-30ns for the rdtsc + sample, which roughly doubles the hot path — use
// for diagnostics, not always-on monitoring.
template <unsigned F>
__attribute__((always_inline))
static inline void ExecutionCore_Tick(ExecutionCore<F>* core, const Tick<F>& tick) {
    // Latency sampling — read enable flag with relaxed ordering. The enable
    // flip is rare (controller flips it once when diagnostics start), so the
    // branch predictor pins this to "not taken" in steady state.
    uint8_t lat_enabled = core->latency_stats.enabled.load(std::memory_order_relaxed);
    uint64_t lat_t0 = 0;
    if (__builtin_expect(lat_enabled, 0)) {
        uint32_t hi, lo;
        asm volatile("mfence\n\tlfence\n\trdtsc\n\t" : "=a"(lo), "=d"(hi));
        lat_t0 = ((uint64_t)hi << 32) | lo;
    }

    // Cached parameter slot read (phase 14 perf optimization).
    // Steady state: 1 acquire-load of seq + 1 compare. Skip the 192-byte memcpy
    // entirely when the seq counter is unchanged since our last refresh. The
    // miss path falls through to the full ParameterSlot_Read protocol below.
    //
    // Saves ~12 ns per tick vs unconditional memcpy. Validated by
    // experiments/per_core_sharding/bench_batch_floor.cpp: cached_v2 ~35 ns
    // vs the original ~50 ns at the batch floor on i5-1035G4.
    uint64_t s_now = core->param_slot.seq.load(std::memory_order_acquire);
    if (__builtin_expect(s_now != core->cached_seq || (s_now & 1ULL), 0)) {
        // Cache miss or producer mid-write. Run the full retry-protected
        // read into the cache. The miss path is rare (one per slow-path
        // parameter push, ~once per ~256 ticks).
        ParameterSlot_Read(&core->param_slot, &core->cached_params);
        core->cached_seq = core->param_slot.seq.load(std::memory_order_acquire) & ~1ULL;
    }

    // Read the active flag once. Used in multiple places below.
    uint8_t active = core->active;

    // Phase 14 active override: when the core is currently in a trade, the SG
    // gate uses the live TP/SL computed at the actual fill price (which may
    // differ from the controller's expected entry due to slippage). When NOT
    // active, the SG gate uses the cached params' precomputed prices. We use
    // a CMOV-style ternary so both loads are issued in parallel and the right
    // one is selected — measurably faster than a real branch (verified in
    // bench_batch_floor v2 vs v3 where the branch was 4 ns slower).
    FPN<F> tp = active ? core->live_tp : core->cached_params.sg_take_profit_price;
    FPN<F> sl = active ? core->live_sl : core->cached_params.sg_stop_loss_price;

    // Read frequently used fields. The compiler keeps them in registers across
    // the body since cached_params is read-only in the steady path.
    uint8_t flags = core->cached_params.flags;

    // === Inlined BG_Evaluate ===
    // Branchless direction select via GATE_FLAG_BUY_ABOVE (v4.0 — momentum
    // strategy buys above breakout, mean-reversion / dip / ml / ema buy below).
    // Both comparisons computed unconditionally so the CPU pipelines them;
    // mask picks the active one. Adds ~1ns vs single-direction; ignorable.
    uint64_t price_below   = (uint64_t)FPN_LessThan(tick.price, core->cached_params.bg_price_threshold);
    uint64_t price_above   = (uint64_t)FPN_GreaterThan(tick.price, core->cached_params.bg_price_threshold);
    uint64_t buy_above     = (uint64_t)((flags & GATE_FLAG_BUY_ABOVE) != 0);
    uint64_t price_ok      = (price_above & buy_above) | (price_below & ~buy_above);
    uint64_t volume_ok     = (uint64_t)FPN_GreaterThan(tick.volume, core->cached_params.bg_volume_threshold);
    uint64_t volume_req    = (uint64_t)((flags & GATE_FLAG_VOLUME_REQUIRED) != 0);
    uint64_t volume_check  = (volume_req & volume_ok) | (~volume_req & 1ULL);
    // Track E.3: slow-path veto via GATE_FLAG_BUY_BLOCKED. Branchless mask
    // — blocked_mask is ALL_ONES when blocked, 0 when open. AND with
    // ~blocked_mask drops bg_fires when vetoed. Same shape as the
    // standalone BG_Evaluate in GateParameters.hpp. ~1ns added.
    uint64_t blocked       = (uint64_t)((flags & GATE_FLAG_BUY_BLOCKED) != 0);
    uint64_t blocked_mask  = -blocked;
    uint64_t bg_fires      = (price_ok & volume_check) & ~blocked_mask;

    // === Inlined SG_Evaluate (using selected tp/sl from above) ===
    uint64_t tp_enabled    = (uint64_t)((flags & GATE_FLAG_TP_ENABLED) != 0);
    uint64_t sl_enabled    = (uint64_t)((flags & GATE_FLAG_SL_ENABLED) != 0);
    // v4.0.3 D9: trailing SL via ratchet field. Branchless FPN_Max selects
    // the higher of original SL and the ratchet floor. When ratchet_sl is
    // FPN_Zero (default, no ratchet), FPN_Max(sl, 0) = sl so behavior is
    // unchanged. When controller has ratcheted up, the ratchet wins → exit
    // fires when price drops to the trailing level. ~5ns added per tick.
    FPN<F> effective_sl = FPN_Max(sl, core->cached_params.ratchet_sl);
    uint64_t tp_hit        = (uint64_t)FPN_GreaterThanOrEqual(tick.price, tp);
    uint64_t sl_hit        = (uint64_t)FPN_LessThanOrEqual(tick.price, effective_sl);
    uint64_t sg_fires      = (tp_enabled & tp_hit) | (sl_enabled & sl_hit);

    // Mask events. ~active means "not currently in a trade". permission means
    // "controller has authorized this slot to take new entries". The permission
    // read is the LAST thing in the can_enter chain so it provides freshness up
    // to the moment of decision (see P2.6 in pitfalls).
    //
    // Phase 09 (kill switch): permission is loaded with ACQUIRE so the cleared
    // value from the controller's RELEASE store becomes visible no later than
    // the next tick. On x86 this is a plain mov; the memory order is a fence
    // for the compiler reorder barrier and a contract for weakly ordered ISAs.
    uint8_t  perm = __atomic_load_n(&core->permission, __ATOMIC_ACQUIRE);
    uint64_t can_enter = ((uint64_t)(~active & 1) & (uint64_t)perm & bg_fires) & 1ULL;
    uint64_t can_exit  = ((uint64_t)active & sg_fires) & 1ULL;

    // Rare branch: push event when something actually fired. Almost always
    // not-taken in steady state, predicts perfectly. Entry-time TP/SL recompute
    // is folded into the same rare branch since it only matters on can_enter.
    if (__builtin_expect(can_enter | can_exit, 0)) {
        TradeEvent<F> event;
        event.price     = tick.price;
        event.timestamp = tick.timestamp;
        event.core_id   = core->core_id;
        event.type      = (uint8_t)(can_enter | (can_exit << 1));
        SPSCRing_TryPush(&core->event_ring, event);

        // Phase 14: stash the live TP/SL computed from the actual fill price.
        // If params.tp_pct is non-zero we use the percentage path; otherwise
        // we fall back to the precomputed absolute price from the parameter
        // pack (legacy path, used by tests that haven't been ported).
        if (can_enter) {
            core->entry_price = tick.price;
            FPN<F> tp_pct = core->cached_params.tp_pct;
            if (!FPN_IsZero(tp_pct)) {
                core->live_tp = FPN_Add(tick.price, FPN_Mul(tick.price, tp_pct));
            } else {
                core->live_tp = core->cached_params.sg_take_profit_price;
            }
            FPN<F> sl_pct = core->cached_params.sl_pct;
            if (!FPN_IsZero(sl_pct)) {
                core->live_sl = FPN_Sub(tick.price, FPN_Mul(tick.price, sl_pct));
            } else {
                core->live_sl = core->cached_params.sg_stop_loss_price;
            }
        }
    }

    // Branchless flag update: set bit on enter, clear on exit. Same tick can't
    // both enter and exit because they're mutually exclusive (can_enter requires
    // ~active, can_exit requires active).
    core->active = (uint8_t)((active | can_enter) & ~can_exit);

    // Latency sample close — only when enabled (predicted not-taken).
    if (__builtin_expect(lat_enabled, 0)) {
        uint32_t hi, lo;
        asm volatile("rdtscp\n\tlfence\n\t" : "=a"(lo), "=d"(hi) : : "rcx");
        uint64_t lat_t1 = ((uint64_t)hi << 32) | lo;
        CoreLatencyStats_Sample(&core->latency_stats, lat_t1 - lat_t0, lat_t1);
    }
}

}  // namespace tt
