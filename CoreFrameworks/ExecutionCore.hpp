// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[CoreFrameworks/ExecutionCore.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [HOT_PATH] [DATA_ORIENTED_DESIGN] [CONCURRENCY]]
// [SCOPE]_[CORE]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[per-core trading state machine + the branchless tick kernel — one ExecutionCore<F> per pinned CPU core]
// [CONTAINS]
//   - [STRUCT]_[ExecutionCore]
//   - [FUNCTION]_[ExecutionCore_Init]
//   - [FUNCTION]_[ExecutionCore_SetParameters]
//   - [FUNCTION]_[ExecutionCore_SetPermission]
//   - [FUNCTION]_[ExecutionCore_Tick_Impl]
//   - [FUNCTION]_[ExecutionCore_Tick]
//======================================================================================================

#pragma once

#include "../FixedPoint/FixedPointN.hpp"
#include "NodeLatencyStats.hpp"
#include "GateParameters.hpp"
#include "ParameterSlot.hpp"
#include "SPSCRing.hpp"
#include "Tick.hpp"
#include "TradeEvent.hpp"

#include <cstdint>
#include <type_traits>  // v5.11.0.E — static_assert(!std::is_polymorphic<...>)

namespace tt {

// Sized for the i5-1035G4 microbenchmark. Production sizing tuned per use case.
//
// EDIT[FROM OPERATOR]: 7-05-2026
// we could probably make this defined at the build time, using the build.sh
// script, just an idea, but it allows for different sizing easier, we could
// ideally do this for anything thats set as a constexpr across the code base
// where we set defintions, especially considering weve moved on from the i5
// processor, back to an i7, just A THOUGHT,(like me)
constexpr size_t EXECUTION_NODE_TICK_RING_SIZE  = 4096;
constexpr size_t EXECUTION_NODE_EVENT_RING_SIZE = 1024;

//======================================================================
// [STRUCT]_[ExecutionCore]
//----------------------------------------------------------------------
// [TAG]_[[HOT_PATH] [DATA_ORIENTED_DESIGN] [CONCURRENCY]]
// [SCOPE]_[CORE]
// [THREAD]_[[HOT_WRITER] [SLOW_READER]]
// [SYNC]_[SEQ_LOCK]
// [SYNC]_[SPSC]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[per-core hot execution state — layout-by-access-pattern; hot path reads line 0 + the permission line only in steady state]
// [REGION]_[line 0 steady-state hot reads]_[0..63]
// [THREAD]_[[HOT_WRITER]]
// [REGION]_[entry + leg-B cold fields]_[64..127]
// [REGION]_[permission — cross-thread isolate]_[128..191]
// [THREAD]_[[SLOW_WRITER] [HOT_READER]]
// [SYNC]_[ATOMIC]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
struct alignas(64) ExecutionCore {
    //------------------------------------------------------------------
    // [SECTION]_[CACHE LINE 0 — steady-state HOT READS only]
    //------------------------------------------------------------------
    uint8_t  active;           // hot read: core writes and reads
    uint8_t  active_b;         // hot read: 0 in steady state when leg B inactive
    uint8_t  _pad_hot[6];
    Money   live_tp;          // hot read: steady CMOV reads every tick — line-0 resident
    Money   live_sl;          // hot read: line-0 resident (Finding A)
    uint8_t  _pad_line0[8];    // pad cache line 0 to 64

    //------------------------------------------------------------------
    // [SECTION]_[CACHE LINE 1+ — cold in steady state; only entry/leg-B paths touch]
    //------------------------------------------------------------------
    // entry_price is WRITE-only on entry events (not read in steady CMOV).
    // leg-B fields gated by `if (__builtin_expect(active_b, 0))` — line never
    // touched when leg B is inactive (steady-state default cfg).
    Money   entry_price;      // write-on-entry only; cold in steady state
    Money   entry_price_b;
    Money   live_tp_b;
    Money   live_sl_b;

    //------------------------------------------------------------------
    // [SECTION]_[permission — isolated to its OWN cache line (audit Part 1.5)]
    //------------------------------------------------------------------
    // Controller atomic-stores `permission` from a different CPU than the
    // hot path's read. Pre-v5.11.1.5: permission shared cache line 0 with
    // active/entry_price/live_tp/live_sl → controller writes invalidated the
    // line, causing ~30-50ns reload stall on the next hot-path tick.
    // Post-v5.11.1.5: permission alone on its own 64B line → controller
    // writes don't invalidate the hot-fields line.
    alignas(64) uint8_t permission;
    uint8_t  _pad_perm[63];

    //------------------------------------------------------------------
    // [SECTION]_[controller-pushed parameters + cached snapshot]
    //------------------------------------------------------------------
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
    // v5.12.1.B.3 — paired with cached_params; refreshed every time
    // ParameterSlot_Read fires (cached_seq miss). Used by hot-path
    // staleness gate: gap = tick.sequence - cached_publish_tick.
    // Single-writer (this core's hot path); no atomic needed.
    uint64_t cached_publish_tick;

    //------------------------------------------------------------------
    // [SECTION]_[identity + rings + tail observability]
    //------------------------------------------------------------------
    // --- Identity ---
    uint16_t node_id;
    uint8_t  _pad1[6];

    // --- Communication rings ---
    // Event ring: this core's outbound trade events. The controller is the consumer.
    SPSCRing<TradeEvent<F>, EXECUTION_NODE_EVENT_RING_SIZE> event_ring;

    // Tick ring: this core's inbound tick stream. The market reader is the producer.
    // Pointer (not embedded) because the ring is owned by the market reader, which
    // creates one per execution core at startup.
    SPSCRing<Tick<F>, EXECUTION_NODE_TICK_RING_SIZE>* tick_ring;

    // --- Latency monitoring (Phase 13 / per-core observability) ---
    // Lives at the tail of the struct so it doesn't pollute the hot fields
    // in cache line 0. Single writer (this core's hot path), single reader
    // (the controller's snapshot helper). Disabled by default; flip the
    // enable flag from the controller to start sampling.
    NodeLatencyStats latency_stats;

    // --- v5.11.0.1: Hot-path failure counters (no I/O on hot path) ---
    // Replaces inline fprintf on the rare ring-push-failure branch
    // (cascading libc-mutex stall risk under degraded conditions).
    // Single writer (this core's hot path), relaxed-load by slow path
    // for surfacing via TUISnapshot. No false sharing — accessed only
    // by this core's threads. Position at struct tail keeps the cold
    // failure-path cache line out of line 0.
    // See plans/2026-05-06-hot-path-discipline.md Rule 2 for the pattern.
    uint64_t ring_push_failures;
};
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]_[unit model]
//----------------------------------------------------------------------
// Per-core trading state machine for the per-core risk-sharded engine.
//
// One ExecutionCore<F> instance per pinned CPU core. Each instance owns:
//   - permission   (1 bit)   — controller-granted authorization to take new entries
//   - active       (1 bit)   — currently in a trade
//   - entry_price  (Money)  — price of the current trade entry, used by SG check
//   - gate_params  (struct)  — buy/sell gate parameter pack pushed by controller
//   - event_ring   (SPSC)    — outgoing trade events the controller drains on its slow path
//   - tick_ring*   (SPSC*)   — incoming tick stream from the market reader
//
// The execution core has NO concept of a position. No P&L. No balance. No portfolio.
// All of those live on the controller core, which learns about trades by reading
// the event ring and updates the canonical Position records on its own schedule.
//======================================================================
// [COMMENT]_[per-tick algorithm]
//----------------------------------------------------------------------
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
//======================================================================
// [COMMENT]_[layout history]
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// [[2026-05] [v5.11.1.5]]
//----------------------------------------------------------------------
// v5.11.1.5 layout reorder: the per-tick CMOV at hot path lines ~287-288
// reads `live_tp` + `live_sl` every tick. Pre-v5.11.1.5 layout had
// `live_sl` spanning offsets 56-80 (cache line 0 → 1) due to Money=24B
// sizing — every tick loaded 2 cache lines instead of 1.
//
// entry_price MOVED to line 1 (write-only on entry events; not in steady
// CMOV). leg-B fields gated by `if (active_b)` → line never touched in
// steady state when leg B is inactive (the common case).
//
// Audit: LATENCY_OPTIMIZATION_AUDIT.md Part 1.5 (permission isolation)
//      + plans/2026-05-06-latency-path-discipline.md Rule 1 (Finding A)
// [SUPPORTING_DOCS]
//   - [DESIGN_SPEC]_[cache-line-discipline]
//   - [INVARIANT]_[H6]
//   - [PLAN]_[2026-05-06-latency-path-discipline]
//======================================================================
// [DERIVED]   (tool-refreshed — do NOT hand-edit; check_cache_layout --fix owns these)
//----------------------------------------------------------------------
// [SIZE]_[68352B]
// [ALIGN]_[64]
// [CACHE_LINES]_[1068]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[ExecutionCore]
//======================================================================

// [ASSERT]_[LAYOUT_LOCK]_[!std::is_polymorphic<ExecutionCore<64>>::value]
// [WHY]_[v5.11.0.E — Part 3 architectural invariant (LATENCY_OPTIMIZATION_AUDIT.md §3.1 Zero-VTable Architecture). ExecutionCore must remain non-polymorphic — virtual functions introduce vtable lookups + indirect jumps that bypass the CPU branch predictor, causing pipeline stalls on hot path. Future PR adding a virtual function fails at compile.]
static_assert(!std::is_polymorphic<ExecutionCore<64>>::value,
              "ExecutionCore must remain non-polymorphic — Part 3 invariant");

// v5.11.1.5 — Cache layout invariants. Future field reorders must preserve
// these to avoid regressing the hot-path single-cache-line load.
// See plans/2026-05-06-latency-path-discipline.md Rule 1.
// [ASSERT]_[LAYOUT_LOCK]_[offsetof(live_sl) + sizeof(Money) <= 64]
static_assert(offsetof(ExecutionCore<64>, live_sl) + sizeof(Money) <= 64,
              "live_sl must fit entirely in cache line 0 — see latency-path-discipline.md Rule 1");
// [ASSERT]_[LAYOUT_LOCK]_[offsetof(permission) % 64 == 0]
static_assert((offsetof(ExecutionCore<64>, permission) % 64) == 0,
              "permission must be cache-line-aligned to prevent false sharing — audit Part 1.5");
// [ASSERT]_[LAYOUT_LOCK]_[offsetof(active) == 0]
static_assert(offsetof(ExecutionCore<64>, active) == 0,
              "active byte must sit at struct offset 0 (hot field, line 0)");
// [ASSERT]_[LAYOUT_LOCK]_[offsetof(active_b) == 1]
static_assert(offsetof(ExecutionCore<64>, active_b) == 1,
              "active_b byte must sit at struct offset 1 (hot field, line 0)");
// [ASSERT]_[LAYOUT_LOCK]_[offsetof(live_tp) >= 8]
static_assert(offsetof(ExecutionCore<64>, live_tp) >= 8,
              "live_tp must be 8-byte aligned (after byte flags + pad)");

//======================================================================
// [FUNCTION]_[ExecutionCore_Init]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BOOT_TIME]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[one-time per-core init — safe defaults (permission=0, STRATEGY_NONE) until the controller pushes real parameters]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
static inline void ExecutionCore_Init(
    ExecutionCore<F>* core,
    uint16_t node_id,
    SPSCRing<Tick<F>, EXECUTION_NODE_TICK_RING_SIZE>* tick_ring
) {
    core->permission = 0;
    core->active     = 0;
    core->entry_price = Money_Zero();
    core->live_tp    = Money_Zero();
    core->live_sl    = Money_Zero();
    // P.2 leg-B init — zero by default; opened only when can_enter fires
    // AND cached_params.flags carries GATE_FLAG_PAIR_ACTIVE (set by
    // Strategy_BuildParameters when cfg.partial_exit_enabled=1).
    core->active_b      = 0;
    core->entry_price_b = Money_Zero();
    core->live_tp_b     = Money_Zero();
    core->live_sl_b     = Money_Zero();
    core->node_id    = node_id;
    core->tick_ring  = tick_ring;
    GateParameters<F> initial;
    GateParameters_Init(&initial);
    ParameterSlot_Init(&core->param_slot, initial);
    // Cache starts as a copy of the initial pack with cached_seq = sentinel
    // that forces a real ParameterSlot_Read on the first tick (so we pick up
    // any controller writes that landed between Init and the first tick).
    core->cached_params = initial;
    core->cached_seq    = (uint64_t)-1;
    // v5.12.1.B.3 — warmup sentinel; hot-path treats 0 as "no stamp" → no gate fires.
    core->cached_publish_tick = 0;
    SPSCRing_Init(&core->event_ring);
    NodeLatencyStats_Init(&core->latency_stats);
    core->ring_push_failures = 0;  // v5.11.0.1: hot-path failure counter
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Initialize an execution core. Defaults: permission=0 (controller must explicitly
// grant), active=0, entry_price=0, parameter slot installed with safe defaults
// (STRATEGY_NONE so the gate evaluators won't fire even if a tick comes in
// before the controller pushes real parameters).
//
// The controller calls this once per core at startup, then later sets permission=1
// after pushing a valid strategy assignment via ExecutionCore_SetParameters.
//======================================================================
// [END_FUNCTION]_[ExecutionCore_Init]
//======================================================================

//======================================================================
// [FUNCTION]_[ExecutionCore_SetParameters]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [CONCURRENCY]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[atomic parameter push from the controller — seqlock write; wait-free on the consumer side]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
static inline void ExecutionCore_SetParameters(
    ExecutionCore<F>* core,
    const GateParameters<F>& new_params,
    uint64_t publish_tick = 0
) {
    ParameterSlot_Write(&core->param_slot, new_params, publish_tick);
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Atomic parameter push from the controller. Wraps ParameterSlot_Write so the
// caller doesn't need to know the slot's internals. Call this from the
// controller's slow path or from the parameter-push step of the event loop.
// Wait-free on the consumer side (execution core can keep ticking through
// the swap).
//
// v5.12.1.B.2 — optional publish_tick. Slow-path passes ticks_produced.load()
// at publish time so the hot path can detect stale params via the v5.12.1.B
// mask gate. Default 0 = back-compat (warmup sentinel; no gate fires).
//======================================================================
// [END_FUNCTION]_[ExecutionCore_SetParameters]
//======================================================================

//======================================================================
// [FUNCTION]_[ExecutionCore_SetPermission]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [CONCURRENCY] [LIVE_TRADING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[atomic permission set (kill switch) — RELEASE store from the controller; the core's next ACQUIRE load sees it]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
static inline void ExecutionCore_SetPermission(ExecutionCore<F>* core, uint8_t value) {
    __atomic_store_n(&core->permission, value, __ATOMIC_RELEASE);
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
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
//======================================================================
// [END_FUNCTION]_[ExecutionCore_SetPermission]
//======================================================================

//======================================================================
// [FUNCTION]_[ExecutionCore_Tick_Impl]
//----------------------------------------------------------------------
// [TAG]_[[HOT_PATH] [CAPITAL_BEARING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[THE hot path — process one tick; branchless gates + state update; the only branch is the rare event push]
// [REFERENCE]_[INVARIANT]_[[H7] [H8]]
// [REFERENCE]_[AUDIT]_[latency-optimization-part-1.1]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F, bool LAT_ENABLED, bool PAIR_BRANCHLESS>
__attribute__((always_inline))
static inline void ExecutionCore_Tick_Impl(ExecutionCore<F>* core, const Tick<F>& tick) {
    // Latency sampling — only loaded + checked in LAT_ENABLED=true builds.
    // The runtime gate (`core->latency_stats.enabled`) lets the controller
    // flip sampling on/off within an LAT_ENABLED=true binary; non-profiled
    // production builds (LAT_ENABLED=false) skip the entire block.
    uint8_t lat_enabled = 0;
    uint64_t lat_t0 = 0;
    if constexpr (LAT_ENABLED) {
        lat_enabled = core->latency_stats.enabled.load(std::memory_order_relaxed);
        if (__builtin_expect(lat_enabled, 0)) {
            uint32_t hi, lo;
            asm volatile("rdtscp\n\tlfence\n\t" : "=a"(lo), "=d"(hi) : : "rcx");
            lat_t0 = ((uint64_t)hi << 32) | lo;
        }
    }

    // Cached parameter slot read (phase 14 perf optimization).
    // Steady state: 1 acquire-load of seq + 1 compare. Skip the 192-byte memcpy
    // entirely when the seq counter is unchanged since our last refresh. The
    // miss path falls through to the full ParameterSlot_Read protocol below.
    //
    // Saves ~12 ns per tick vs unconditional memcpy. Validated by
    // experiments/per_node_sharding/bench_batch_floor.cpp: cached_v2 ~35 ns
    // vs the original ~50 ns at the batch floor on i5-1035G4.
    uint64_t s_now = core->param_slot.seq.load(std::memory_order_acquire);
    if (__builtin_expect(s_now != core->cached_seq || (s_now & 1ULL), 0)) {
        // Cache miss or producer mid-write. Run the full retry-protected
        // read into the cache. The miss path is rare (one per slow-path
        // parameter push, ~once per ~256 ticks).
        // v5.12.1.B.3 — refresh cached_publish_tick alongside cached_params
        // inside the same seqlock retry bracket (paired by ParameterSlot_Read).
        ParameterSlot_Read(&core->param_slot, &core->cached_params,
                           &core->cached_publish_tick);
        core->cached_seq = core->param_slot.seq.load(std::memory_order_acquire) & ~1ULL;
    }

    // Read the active flags once. Used in multiple places below.
    uint8_t active = core->active;
    uint8_t active_b = core->active_b;  // P.2: leg B (0 unless paired entry fired)

    // Phase 14 active override: when the core is currently in a trade, the SG
    // gate uses the live TP/SL computed at the actual fill price (which may
    // differ from the controller's expected entry due to slippage). When NOT
    // active, the SG gate uses the cached params' precomputed prices. We use
    // a CMOV-style ternary so both loads are issued in parallel and the right
    // one is selected — measurably faster than a real branch (verified in
    // bench_batch_floor v2 vs v3 where the branch was 4 ns slower).
    Money tp = active ? core->live_tp : core->cached_params.sg_take_profit_price;
    Money sl = active ? core->live_sl : core->cached_params.sg_stop_loss_price;
    // Note: leg-B TP/SL are loaded inside the branch-gated block below
    // (P.2 v2). The original P.2 design read them unconditionally; that
    // cost ~40ns per tick because Money compares pipeline less than
    // expected on the i5-1035G4. Moving the loads + compares behind
    // `if (__builtin_expect(active_b, 0))` returns steady-state latency
    // to the pre-P.2 baseline. Cost is paid only while a pair is open.

    // Read frequently used fields. The compiler keeps them in registers across
    // the body since cached_params is read-only in the steady path.
    uint8_t flags = core->cached_params.flags;

    //------------------------------------------------------------
    // [SECTION]_[Inlined BG_Evaluate]
    //------------------------------------------------------------
    // Branchless direction select via GATE_FLAG_BUY_ABOVE (v4.0 — momentum
    // strategy buys above breakout, mean-reversion / dip / ml / ema buy below).
    // Both comparisons computed unconditionally so the CPU pipelines them;
    // mask picks the active one. Adds ~1ns vs single-direction; ignorable.
    uint64_t price_below   = (uint64_t)Money_Lt(tick.price, core->cached_params.bg_price_threshold);
    uint64_t price_above   = (uint64_t)Money_Gt(tick.price, core->cached_params.bg_price_threshold);
    uint64_t buy_above     = (uint64_t)((flags & GATE_FLAG_BUY_ABOVE) != 0);
    uint64_t price_ok      = (price_above & buy_above) | (price_below & ~buy_above);
    uint64_t volume_ok     = (uint64_t)Money_Gt(tick.volume, core->cached_params.bg_volume_threshold);
    uint64_t volume_req    = (uint64_t)((flags & GATE_FLAG_VOLUME_REQUIRED) != 0);
    uint64_t volume_check  = (volume_req & volume_ok) | (~volume_req & 1ULL);
    // Track E.3: slow-path veto via GATE_FLAG_BUY_BLOCKED. Branchless mask
    // — blocked_mask is ALL_ONES when blocked, 0 when open. AND with
    // ~blocked_mask drops bg_fires when vetoed. Same shape as the
    // standalone BG_Evaluate in GateParameters.hpp. ~1ns added.
    uint64_t blocked       = (uint64_t)((flags & GATE_FLAG_BUY_BLOCKED) != 0);
    uint64_t blocked_mask  = -blocked;
    uint64_t bg_fires      = (price_ok & volume_check) & ~blocked_mask;

    //------------------------------------------------------------
    // [SECTION]_[v5.12.1.B.3 — branchless staleness gate]
    //------------------------------------------------------------
    // Predicate is true when ALL hold:
    //   - GATE_FLAG_STALENESS_ENABLED is set (cfg.param_staleness_gate_enabled=1
    //     was active when slow-path published)
    //   - cached_publish_tick != 0 (slow-path has published at least once)
    //   - tick.sequence > cached_publish_tick (catches counter-wrap defense;
    //     the very rare case where slow-path observed ticks_produced ahead
    //     of what hot-path has currently consumed → treat as gap=0)
    //   - (tick.sequence - cached_publish_tick) > cached_params.param_max_age_ticks
    //
    // When true, masks bg_fires off (entries blocked) and OR's
    // SHALT_PARAM_STALE into the staleness_signal field — surfaces in the
    // GUI strategy_halt_reason channel via the slow-path observability tap.
    //
    // Cost: 5 mask ops + 1 sub + 3 unsigned compares = ~5-7ns extra.
    // Default (cfg.param_staleness_gate_enabled=0 → flag bit 0): all three
    // predicates compute but stale_mask is 0 → bg_fires unchanged.
    uint64_t stale_enabled  = (uint64_t)((flags & GATE_FLAG_STALENESS_ENABLED) != 0);
    uint64_t has_stamp      = (uint64_t)(core->cached_publish_tick != 0);
    uint64_t tick_ge_stamp  = (uint64_t)(tick.sequence >= core->cached_publish_tick);
    // Branchless gap clamp: subtract unconditionally; mask to 0 if wrap.
    uint64_t param_gap      = (tick.sequence - core->cached_publish_tick)
                              & (uint64_t)(-(int64_t)tick_ge_stamp);
    uint64_t over_age       = (uint64_t)(param_gap > core->cached_params.param_max_age_ticks);
    uint64_t stale          = stale_enabled & has_stamp & over_age;
    uint64_t stale_mask     = (uint64_t)(-(int64_t)stale);
    bg_fires &= ~stale_mask;

    //------------------------------------------------------------
    // [SECTION]_[Inlined SG_Evaluate]
    //------------------------------------------------------------
    // Leg A: always evaluated (single-position case + when paired). Leg B:
    // BRANCH-GATED on active_b (P.2 v2 — 2026-04-27 measurement showed
    // unconditional leg-B compute cost ~40ns/tick on the i5-1035G4
    // because Money compares don't pipeline as cleanly as expected).
    // Branch is predicted not-taken in steady state; when no pair is
    // open (the common case, especially with partial_exit_enabled=0),
    // the leg-B FPN_Binary ops + memory loads are skipped entirely. When a
    // pair IS open (rare, only between leg A entry and final leg B
    // exit), branch is taken and we pay the +40ns to evaluate leg B.
    //
    // Net behavior:
    //   - partial_exit_enabled=0 (default): zero leg-B cost ever
    //   - partial_exit_enabled=1, no pair open: zero leg-B cost
    //   - partial_exit_enabled=1, pair open: +40ns until pair closes
    uint64_t tp_enabled    = (uint64_t)((flags & GATE_FLAG_TP_ENABLED) != 0);
    uint64_t sl_enabled    = (uint64_t)((flags & GATE_FLAG_SL_ENABLED) != 0);
    // v4.0.3 D9: trailing SL via ratchet field. Branchless FPN_Max selects
    // the higher of original SL and the ratchet floor. When ratchet_sl is
    // FPN_Zero (default, no ratchet), FPN_Max(sl, 0) = sl so behavior is
    // unchanged. When controller has ratcheted up, the ratchet wins → exit
    // fires when price drops to the trailing level.
    Money effective_sl = Money_Max(sl, core->cached_params.ratchet_sl);
    // v5.4.0 Phase 3.3: trailing TP via ratchet_tp — same FPN_Max pattern
    // as SL. When ratchet_tp is FPN_Zero (default), FPN_Max(tp, 0) = tp so
    // behavior is unchanged. When the controller has ratcheted TP up
    // (Regime_AdjustPositionsSharded, future strategy trailing), the higher
    // value wins — TP fires at the new lock-in level. Same ~1ns cost shape
    // as the SL ratchet.
    Money effective_tp = Money_Max(tp, core->cached_params.ratchet_tp);
    // Leg A SG (existing pattern, unchanged)
    uint64_t tp_hit_a   = (uint64_t)Money_Ge(tick.price, effective_tp);
    uint64_t sl_hit_a   = (uint64_t)Money_Le(tick.price, effective_sl);
    uint64_t sg_fires_a = (tp_enabled & tp_hit_a) | (sl_enabled & sl_hit_a);
    // v5.11.1.2 — Leg B SG. Two compile-time-selected paths via PAIR_BRANCHLESS:
    //   - PAIR_BRANCHLESS=true (always-pair deployments where active_b is set
    //     most ticks): unconditional compute + mask. CPU pipelines leg-A and
    //     leg-B compares together; no branch mispredict cost when leg B fires.
    //   - PAIR_BRANCHLESS=false (default-cfg deployments): original predicted-
    //     not-taken branch. ~0ns cost when active_b=0 (the common steady state).
    // Wrapper dispatches via cached_params.flags & GATE_FLAG_PAIR_ACTIVE — one
    // predicted runtime branch per tick (~0ns once predictor warms).
    // Both code paths produce bytewise-identical output for any (active_b, FPN_Binary)
    // input — the branchless path masks the unconditional compute with active_b.
    uint64_t sg_fires_b = 0;
    if constexpr (PAIR_BRANCHLESS) {
        // Unconditional leg-B compute. Loads live_tp_b + live_sl_b every tick;
        // for always-pair deployments these stay hot in L1d.
        Money effective_sl_b = Money_Max(core->live_sl_b, core->cached_params.ratchet_sl);
        Money effective_tp_b = Money_Max(core->live_tp_b, core->cached_params.ratchet_tp);
        uint64_t tp_hit_b     = (uint64_t)Money_Ge(tick.price, effective_tp_b);
        uint64_t sl_hit_b     = (uint64_t)Money_Le(tick.price, effective_sl_b);
        // Mask with active_b — when leg B isn't open, mask=0 zeros the result.
        sg_fires_b = ((tp_enabled & tp_hit_b) | (sl_enabled & sl_hit_b))
                     & -((uint64_t)active_b);
    } else {
        if (__builtin_expect(active_b, 0)) {
            Money tp_b           = core->live_tp_b;
            Money sl_b           = core->live_sl_b;
            Money effective_sl_b = Money_Max(sl_b, core->cached_params.ratchet_sl);
            Money effective_tp_b = Money_Max(tp_b, core->cached_params.ratchet_tp);
            uint64_t tp_hit_b     = (uint64_t)Money_Ge(tick.price, effective_tp_b);
            uint64_t sl_hit_b     = (uint64_t)Money_Le(tick.price, effective_sl_b);
            sg_fires_b            = (tp_enabled & tp_hit_b) | (sl_enabled & sl_hit_b);
        }
    }

    //------------------------------------------------------------
    // [SECTION]_[mask events + the rare push branch + flag updates]
    //------------------------------------------------------------
    // Mask events. ~(active_a | active_b) means "not currently in any leg".
    // This tighter gate prevents leg A from re-opening solo while leg B is
    // still running (would corrupt pair semantics post-partial-exit).
    // permission means "controller has authorized this core to enter."
    //
    // Phase 09 (kill switch): permission is loaded with ACQUIRE so the cleared
    // value from the controller's RELEASE store becomes visible no later than
    // the next tick. On x86 this is a plain mov; the memory order is a fence
    // for the compiler reorder barrier and a contract for weakly ordered ISAs.
    uint8_t  perm = __atomic_load_n(&core->permission, __ATOMIC_ACQUIRE);
    uint64_t any_active = (uint64_t)((active | active_b) & 1);
    uint64_t can_enter  = (~any_active & (uint64_t)perm & bg_fires) & 1ULL;
    uint64_t can_exit_a = ((uint64_t)active   & sg_fires_a) & 1ULL;
    uint64_t can_exit_b = ((uint64_t)active_b & sg_fires_b) & 1ULL;
    // P.2: pair_active flag from cached_params signals "open both legs on
    // entry." Branchless extraction; used in event push + flag update.
    uint64_t pair_active = (uint64_t)((flags & GATE_FLAG_PAIR_ACTIVE) != 0);

    // Rare branch: push event(s) when something actually fired. Almost
    // always not-taken in steady state, predicts perfectly. Entry-time
    // TP/SL recompute + leg-B activation folded into the same rare branch.
    //
    // v4.7.3: track each push's success. Pre-v4.7.3 the return value of
    // SPSCRing_TryPush was discarded — if the ring filled (drainer
    // briefly stalled), the event was silently dropped BUT the active
    // flag was still cleared on the unconditional mask update below.
    // Result: zombie position — Portfolio.active_bitmap stays set
    // (no Portfolio_CloseSlot ran on the lost exit), core->active=0,
    // hot path's `can_exit = active & sg_fires` evaluates to 0
    // forever, AND the active=0 fallback uses cached_params.sg_stop_loss_price
    // (slow-path-computed against the CURRENT bg threshold, not the
    // entry's SL) — which on a sustained price move tracks current
    // price closely → sl_hit never fires.
    //
    // Fix: only flip the active flag for legs whose event actually
    // queued. Failed pushes leave active unchanged → next tick retries
    // naturally (sg_fires_a still TRUE if SL still in range).
    uint8_t exit_a_pushed = 1, exit_b_pushed = 1;
    uint8_t entry_a_pushed = 1, entry_b_pushed = 1;
    if (__builtin_expect(can_enter | can_exit_a | can_exit_b, 0)) {
        // Leg A exit (or single-position exit when partials disabled)
        if (can_exit_a) {
            TradeEvent<F> event{};
            event.price     = tick.price;
            event.timestamp = tick.timestamp;
            event.node_id   = core->node_id;
            event.type      = TRADE_EVENT_EXIT;
            event.leg       = PARTIAL_LEG_A;
            exit_a_pushed = SPSCRing_TryPush(&core->event_ring, event) ? 1 : 0;
        }
        // Leg B exit (only when active_b=1)
        if (can_exit_b) {
            TradeEvent<F> event{};
            event.price     = tick.price;
            event.timestamp = tick.timestamp;
            event.node_id   = core->node_id;
            event.type      = TRADE_EVENT_EXIT;
            event.leg       = PARTIAL_LEG_B;
            exit_b_pushed = SPSCRing_TryPush(&core->event_ring, event) ? 1 : 0;
        }
        // Entry — opens leg A always, leg B too when GATE_FLAG_PAIR_ACTIVE
        if (can_enter) {
            // Leg A entry event
            TradeEvent<F> event_a{};
            event_a.price     = tick.price;
            event_a.timestamp = tick.timestamp;
            event_a.node_id   = core->node_id;
            event_a.type      = TRADE_EVENT_ENTRY;
            event_a.leg       = PARTIAL_LEG_A;
            entry_a_pushed = SPSCRing_TryPush(&core->event_ring, event_a) ? 1 : 0;

            // Only stash live TP/SL when entry actually queued. If the
            // push failed, leg-A's mask update below leaves active=0 →
            // hot path won't read these fields anyway.
            if (entry_a_pushed) {
                core->entry_price = tick.price;
                Money tp_pct = core->cached_params.tp_pct;
                if (!Money_IsZero(tp_pct)) {
                    core->live_tp = Money_Add(tick.price, Money_Mul(tick.price, tp_pct));
                } else {
                    core->live_tp = core->cached_params.sg_take_profit_price;
                }
                Money sl_pct = core->cached_params.sl_pct;
                if (!Money_IsZero(sl_pct)) {
                    core->live_sl = Money_Sub(tick.price, Money_Mul(tick.price, sl_pct));
                } else {
                    core->live_sl = core->cached_params.sg_stop_loss_price;
                }

                // P.2: leg B opens only when leg A succeeded AND
                // GATE_FLAG_PAIR_ACTIVE is set. Tied to leg A's success
                // so we never end up with a leg B floating without leg A.
                if (pair_active) {
                    TradeEvent<F> event_b{};
                    event_b.price     = tick.price;
                    event_b.timestamp = tick.timestamp;
                    event_b.node_id   = core->node_id;
                    event_b.type      = TRADE_EVENT_ENTRY;
                    event_b.leg       = PARTIAL_LEG_B;
                    entry_b_pushed = SPSCRing_TryPush(&core->event_ring, event_b) ? 1 : 0;

                    if (entry_b_pushed) {
                        core->entry_price_b = tick.price;
                        Money tp_pct_b = core->cached_params.tp_pct_b;
                        if (!Money_IsZero(tp_pct_b)) {
                            core->live_tp_b = Money_Add(tick.price, Money_Mul(tick.price, tp_pct_b));
                        } else {
                            // No leg-B TP configured — fall back to leg A's TP
                            // (effectively makes leg B a duplicate exit at TP1,
                            // which P.4 cfg validation guards against).
                            core->live_tp_b = core->live_tp;
                        }
                        core->live_sl_b = core->live_sl;  // shared SL
                    }
                }
            }
        }
        // v5.11.0.1: Surface push failures via counter (NO I/O on hot path).
        // Pre-fix this was an inline fprintf(stderr) — libc stdio mutex
        // acquisition during a ring-full condition (drainer already stalled)
        // could cascade-stall the hot path further. Now: single store to a
        // per-core counter; slow path picks it up via TUISnapshot for surfacing
        // (slow-path log/render landing in v5.11.3's async log thread).
        // See plans/2026-05-06-latency-path-discipline.md Rule 2.
        if (__builtin_expect(!(exit_a_pushed & exit_b_pushed &
                                entry_a_pushed & entry_b_pushed), 0)) {
            core->ring_push_failures++;
        }
    }

    // Branchless flag updates honoring per-leg push success. Failed pushes
    // mask out the flip, so active stays at its prior value and the next
    // tick retries naturally. Same-tick can't both enter and exit a leg
    // (mutually exclusive can_* masks) so OR-then-AND-NOT remains safe.
    uint64_t exit_a_eff  = can_exit_a & (uint64_t)exit_a_pushed;
    uint64_t exit_b_eff  = can_exit_b & (uint64_t)exit_b_pushed;
    uint64_t enter_a_eff = can_enter  & (uint64_t)entry_a_pushed;
    // Leg B activation requires BOTH leg-A and leg-B pushes succeeded.
    // If leg A's push failed, we never even tried leg B — leaves active_b
    // alone. If leg A succeeded but leg B failed, leg A's active flag
    // toggles correctly while leg B stays inactive (next tick: leg A is
    // already active, can_enter blocked by any_active=1, so we won't
    // retry leg B's missed entry — accept losing that one leg this trade).
    uint64_t enter_b_eff = enter_a_eff & (uint64_t)entry_b_pushed & pair_active;
    core->active   = (uint8_t)((active   | enter_a_eff) & ~exit_a_eff);
    core->active_b = (uint8_t)((active_b | enter_b_eff) & ~exit_b_eff);

    // Latency sample close — only when LAT_ENABLED at compile time + runtime gate set.
    if constexpr (LAT_ENABLED) {
        if (__builtin_expect(lat_enabled, 0)) {
            uint32_t hi, lo;
            asm volatile("rdtscp\n\tlfence\n\t" : "=a"(lo), "=d"(hi) : : "rcx");
            uint64_t lat_t1 = ((uint64_t)hi << 32) | lo;
            NodeLatencyStats_Sample(&core->latency_stats, lat_t1 - lat_t0, lat_t1);
        }
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
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
// v5.11.1.1 — Compile-time elision of latency sampling via template-bool
// LAT_ENABLED. When LAT_ENABLED=false (production builds without -DLATENCY_PROFILING),
// the entire rdtsc + sample block compiles out — zero runtime cost, zero loads
// of the lat_enabled atomic. When LAT_ENABLED=true (latency-profiled builds),
// runtime still gates on `core->latency_stats.enabled.load(...)` so the
// controller can flip sampling on/off at runtime within the profiled binary.
//
// RDTSC standardization (Finding D from pre-v5.11.1 review): both entry +
// exit now use `rdtscp; lfence`. rdtscp is serializing post-execution (no
// need for mfence+lfence pre-amble); gives a consistent measurement bracket.
//
// Audit: LATENCY_OPTIMIZATION_AUDIT.md Part 1.1
//======================================================================
// [END_FUNCTION]_[ExecutionCore_Tick_Impl]
//======================================================================

//======================================================================
// [FUNCTION]_[ExecutionCore_Tick]
//----------------------------------------------------------------------
// [TAG]_[[HOT_PATH]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[two-axis dispatch wrapper — LAT_ENABLED (compile-time) × PAIR_BRANCHLESS (runtime predicted branch); callers unchanged]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
__attribute__((always_inline))
static inline void ExecutionCore_Tick(ExecutionCore<F>* core, const Tick<F>& tick) {
    uint8_t flags = core->cached_params.flags;
    bool pair_branchless = (flags & GATE_FLAG_PAIR_ACTIVE) != 0;
#ifdef LATENCY_PROFILING
    if (pair_branchless) {
        ExecutionCore_Tick_Impl<F, /*LAT_ENABLED=*/true,  /*PAIR_BRANCHLESS=*/true>(core, tick);
    } else {
        ExecutionCore_Tick_Impl<F, /*LAT_ENABLED=*/true,  /*PAIR_BRANCHLESS=*/false>(core, tick);
    }
#else
    if (pair_branchless) {
        ExecutionCore_Tick_Impl<F, /*LAT_ENABLED=*/false, /*PAIR_BRANCHLESS=*/true>(core, tick);
    } else {
        ExecutionCore_Tick_Impl<F, /*LAT_ENABLED=*/false, /*PAIR_BRANCHLESS=*/false>(core, tick);
    }
#endif
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// v5.11.1.1 + v5.11.1.2 — Two-axis dispatch wrapper.
//   1. LAT_ENABLED: build-flag (LATENCY_PROFILING macro) — compile-time
//   2. PAIR_BRANCHLESS: cfg-flag (GATE_FLAG_PAIR_ACTIVE) — runtime predicted
//      branch on cached_params.flags. Selects branchless leg-B compute path
//      when operator runs partial-exit-always cfg (every entry pairs).
//
// 4 template instantiations live in the binary (LAT × PAIR = 2×2). The cfg
// branch is predicted nearly perfectly (cfg flag changes only via slow-path
// param push, ~once per ~256 ticks at most) → ~0ns steady-state cost.
//
// The wrapper reads `cached_params.flags` directly (single-byte load from
// line ~3 of struct). One-tick staleness vs the body's own cached_params
// read is benign: both template paths produce bytewise-identical output for
// any (active_b, threshold) input — the branchless path masks the result
// with active_b. Dispatch staleness affects perf path only, never correctness.
//
// Hot-swap: cfg.partial_exit_enabled flip → Strategy_BuildParameters →
// ParameterSlot_Write → next tick's cached_params has new flags → next
// tick dispatches to the other instantiation. Predictor takes ~2-3 ticks
// to relearn; transient cost negligible.
//
// Preserves callers (still call ExecutionCore_Tick<F>(core, tick)).
//======================================================================
// [END_FUNCTION]_[ExecutionCore_Tick]
//======================================================================

}  // namespace tt
