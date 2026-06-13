// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [ORDER MANAGER]
//
// Single-owner OMS for the per-core sharded engine. Owns the in-flight
// order table and routes submissions through an ExchangeAdapter.
//
// Phase 02 architecture (this file):
//
//   drainer thread                          adapter worker thread(s)
//   ──────────────                          ────────────────────────
//   OrderManager_Submit                     pop from adapter queue
//      ├─> allocate slot                    ├─> call BinanceOrderAPI on
//      ├─> mark ORDER_SUBMITTED             │   per-thread instance
//      └─> adapter.submit_market_*          ├─> bracket with steady_clock
//          (returns immediately)            ├─> build OrderResult
//                                           └─> invoke callback
//                                                ├─> push CMD_FILL_RESULT
//                                                │   into oms->result_queue
//                                                └─> return immediately
//
//   OrderManager_Tick (drainer)             (worker keeps looping)
//      └─> drain result_queue
//          ├─> look up order by id
//          ├─> mark FILLED or REJECTED
//          └─> free slot
//
// The drainer never blocks on the network. The worker thread (or thread
// pool) handles all REST traffic. Drainer cycle drops from worst case
// ~800 ms (4 cores × 200 ms blocking REST in phase 01) to sub-µs
// (drainer just enqueues to the adapter and returns).
//
// CONCURRENCY MODEL:
//   The order table is owned by exactly one thread — the drainer thread,
//   which is the only caller of Submit and Tick. The result_queue is
//   SPSC: the adapter worker is the sole producer (with worker_count==1)
//   and the drainer's Tick is the sole consumer. No locks anywhere.
//
//   When scaling to worker_count > 1, replace result_queue with a real
//   MPSC ring or per-worker queues. The current SPSCRing breaks under
//   multiple producers. See plans/oms/02_async_submission/plan.md.
//
// PAPER MODE:
//   Submit short-circuits — bumps total_submitted and total_filled, never
//   touches the order table or the adapter, returns the next id. The
//   result_queue stays empty so Tick is a no-op. Zero adapter cost in
//   paper mode.
//
// Naming clarification:
//   This is the OMS for live exchange orders. It has nothing to do with
//   MemHeaders/PoolAllocator.hpp:OrderPool, which is the buy gate's order
//   intent pool from the legacy single-core engine.
//======================================================================================================

#pragma once

#include "../FixedPoint/FixedPointN.hpp"
#include "../Limits.hpp"
#include "ExchangeAdapter.hpp"
#include "Order.hpp"
#include "OrderEventLog.hpp"
#include "Portfolio.hpp"
#include "ShardedTradeLog.hpp"
#include "SPSCRing.hpp"
#include "../DataStream/CalibLogColRegistry.hpp"   // v5.14.10.D — FOREACH_CALIB_LOG_COL registry (closes TECH_DEBT-010)
#include "../MemHeaders/OmsStateFlagRegistry.hpp"  // v5.15.5.C.2 (S3a) — FOREACH_OMS_STATE_FLAG bitmap cohort
#include "../MemHeaders/OmsExitPredictorMetaRegistry.hpp"  // v5.15.5.C.2.1 (LOW-2) — FOREACH_OMS_META_SLOT multi-bit cohort
#include "../MemHeaders/OmsFieldRegistry.hpp"              // v5.15.5.C.3 (Phase 3) — canonical FOREACH_OMS_FIELD + projections
#include "../ML_Headers/CoreModelZoo.hpp"                  // v5.15.5.F.4d Step 7 § F — EnsembleModelZoo<F>* for real_on_exit_calibration ezoo_ref cast (transitively pulls PerCoreCfg<F> + BANDIT_MAX_ARMS + NUM_REGIMES + Bandit_GetProbabilities)

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace tt {

//======================================================================================================
// [COMMAND QUEUE]
//======================================================================================================
// External threads push state updates as Commands; OrderManager_Tick
// drains them on the drainer thread. Phase 02 only uses CMD_FILL_RESULT
// (from the adapter worker thread). Phase 03 will add CMD_RECONCILE
// (from the reconciliation poller). Phase 04 may add CMD_USER_FILL
// (from the user-data websocket).
//======================================================================================================
enum CommandType : uint8_t {
    CMD_FILL_RESULT = 0,   // from adapter worker (REST ACK or full fill)
    CMD_WS_FILL     = 1,   // from user data websocket (real-time fill)
    CMD_RECONCILE   = 2,   // from reconciliation poller (phase 05)
};

// Non-templated POD so the SPSCRing slots stay self-contained regardless
// of Money width.
struct Command {
    uint8_t      type;
    uint8_t      _pad0[7];
    uint64_t     order_id;
    OrderResult  result;
};

constexpr size_t OMS_RESULT_QUEUE_SIZE = 256;

// v4.7.37: SubmitCommand — payload for the per-core submit_queues. Producer
// threads (today: producer slow-path; future: per-core slow-path threads)
// push commands here instead of calling OrderManager_Submit directly. The
// drainer thread (sole Submit caller) pops from these queues each tick,
// preserving the documented "single Submit caller" OMS contract even when
// multiple producer-side threads need to submit orders.
//
// Sizing: 32 slots per queue. Slow-path submit events are rare (force-close
// on time-exit, manual close, drag, swap) — peak <1/sec. 32 is generous.
//
// SubmitCommand is templated on F so the Money fields are sized correctly.
// v5.15.5.F.4c.3 WIP2d-1.B.1 — POD args struct + SPSC wire-format unified (option l).
// SubmitCommand is BOTH the OMS submit-API arg shape AND the SPSC ring element. Producers
// construct one struct; drainer pops the same struct; OrderManager_Submit consumes the same
// struct. Eliminates the prior unpack/repack ceremony.
//
// All fields have default member init for safe value-initialization (required for OMS state
// aggregate init: `OrderManagerState<F> oms{};` value-inits the SPSC ring slot array, which
// in turn value-inits each SubmitCommand slot). C++17 friend-scope rules don't permit nested-
// aggregate-init-through-private-default-ctor, so compile-time core_cfg enforcement isn't
// cleanly achievable until C++20 concepts.
//
// DISCIPLINE: production callers MUST use the required-field ctor + set core_cfg = &cfg.cores[c].
// Test fixtures may use default-construct + field-by-field assignment with explicit nullptr.
// TT_ASSERT_PRE_RESOLVED_BOUND (Order.hpp) is the runtime backstop.
//
// Two public ctors:
//   1. Default ctor — for SPSC ring slot init + aggregate OMS state value-init compatibility
//   2. Required-field ctor — for production + test caller sites; signals which fields are
//      essential for a valid submit
//
// Per orchestration-helper-with-pod-args-pattern.md (2nd canonical application after
// Stamp_AssembleAndEmit) — POD args pattern promoted to CLAUDE.md item at ship close.
template <unsigned F>
struct SubmitCommand {
    // ─── REQUIRED (semantic; ctor-signaled) ───
    int16_t                 core_id      = 0;
    uint8_t                 order_type   = 0;
    Money                  qty          = Money_Zero();
    uint8_t                 leg          = 0;
    const ::PerCoreCfg<F>*  core_cfg     = nullptr;

    // ─── OPTIONAL (default-init; caller overrides as needed) ───
    uint8_t                 strategy_id  = 0xFF;
    uint8_t                 _pad[2]      = {0, 0};
    Money                  intended_tp  = Money_Zero();
    Money                  intended_sl  = Money_Zero();
    Money                  event_price  = Money_Zero();
    Money                  tp_pct       = Money_Zero();  // A25 (D-205): leg-effective per-fill TP fraction, resolved at submit; 0 → handle_buy_fill keeps the legacy intended_tp path (bytewise-identical)

    // Default ctor — needed for SPSC ring slot init + OMS state value-init compat.
    SubmitCommand() = default;

    // Required-field ctor — recommended form for production + test callers; signals
    // which fields a valid submit needs.
    SubmitCommand(int16_t cid, OrderType t, Money q, uint8_t lg, const ::PerCoreCfg<F>* cfg)
        : core_id(cid), order_type((uint8_t)t), qty(q), leg(lg), core_cfg(cfg) {}
};

constexpr size_t OMS_SUBMIT_QUEUE_SIZE = 32;  // power of 2

//======================================================================================================
// [ORDER MANAGER STATE]
//======================================================================================================
// Phase 03 (option D — facade): the OMS now owns all the financial state
// that used to live in EventLoopState. EventLoopState becomes a thin
// dispatcher with a pointer back to here.
//
// Field groups:
//   - order table (orders[], order_bitmap, next_order_id)
//   - exchange wiring (adapter, live_trading, result_queue)
//   - bank state (portfolio, balance, realized_pnl, fee_rate) — phase 03
//   - kill switch (ks_*, kill_switch_tripped, ks_trips_total) — phase 03
//   - trade log pointer (not owned, points at the engine's CSV writer) — phase 03
//   - observability counters (total_submitted/filled/rejected)
//
// The bank state and kill switch fields used to live in EventLoopState.
// Moved here in phase 03 chunk 1 so the OMS is the single source of truth
// for everything money-related. EventLoopState_Balance and friends are
// the forwarding accessors that let existing call sites keep working.
//======================================================================================================
// v5.15.5.C.1 — OrderManagerState HOT/WARM/COLD tier reorganization
// (cache-layout-discipline-for-hot-side-structs.md Rule 4) + cross-thread
// atomic cluster isolation via alignas(64) (cross-thread-snapshot-publish-
// cluster-isolation.md / ND1) + embedded-SPSCRing cluster discipline
// (spsc-ring-embedded-in-hot-struct-cluster-discipline.md / NC2) + RAII
// destructor preservation through tier reorg (raii-destructor-with-cluster-
// reorg-interaction.md / NC1).
//
// Pre-.C.1 the struct interleaved HOT (orders, rings) with COLD (adapter,
// live_trading) with WARM-on-fill (portfolio, fee state) with CROSS-THREAD
// (total_*, flatten_pending) — drainer per-cycle access pulled scattered
// cache lines across the entire ~150 KB struct. Post-.C.1 clusters are
// explicit + compile-time-locked via static_asserts.
//
// FIELD NAMES UNCHANGED — only field positions move. All ~46 consumer
// access sites (`oms->total_submitted` etc.) work without modification.
// Compiler resolves names at compile time; reorg changes resolved offsets
// only. RAII destructor (`~OrderManagerState()`) refs fields by name; reorg
// is bytewise-safe per NC1.
//
// Persistence: OMS state is NOT byte-persisted. OrderEventLog writes
// individual OrderEvent records (NOT struct bytes); Portfolio_FromEventLog
// replays on restart. Snapshot publisher reads individual fields. Reorg
// preserves all wire-format semantics.
//
// Bench-gate context: TECH_DEBT-012 flags this as PERFORMANCE-CRITICAL
// (drainer reads OMS every cycle). Tier reorg is pure data-layout change
// with compile-time-enforced sizeof/offsetof asserts — bytewise-safe + the
// 3027-test regression + parity_harness byte-equivalence checks act as the
// gate. The HIGH-RISK changes per TECH_DEBT-012 (FOREACH_OMS_INIT registry
// conversion) land in .C.3 where rdtsc-bracket bench instrumentation is
// added with a runtime cfg toggle.
// v5.15.5.F.4c.3 WIP2d-1.B.1 r-6 phase 2 — Pattern 5 sink-fn-pointer forward declarations.
// Per DESIGN_SPECS/sink-fn-pointer-for-optional-side-effect-pattern.md. Noop fn provides
// the always-call default; real fns wired at boot when respective subsystems enable.
template <unsigned F> struct OrderManagerState;
template <unsigned F>
inline void noop_fill_emit(OrderManagerState<F>*, Order<F>*, Money, Money, Money);

template <unsigned F>
struct OrderManagerState {
    // ════════════════════════════════════════════════════════════════════
    // HOT CLUSTER — drainer reads every cycle
    // (orders, rings, event_log). v5.15.5.C.3 (Finding A') — event_log_mode
    // removed from HOT cluster; now a 2-bit slot in oms_state_flags (COLD).
    // ════════════════════════════════════════════════════════════════════
    Order<F> orders[MAX_INFLIGHT_ORDERS];
    uint16_t order_bitmap;       // 1 = slot in use, 0 = free. uint16_t caps at 16 slots.
    static_assert(MAX_INFLIGHT_ORDERS <= 16,
                  "order_bitmap is uint16_t (16 bits) AND the order-id encodes the slot in 4 bits; "
                  "MAX_INFLIGHT_ORDERS must fit both — raising it past 16 silently overflows (bitmap-overflow-protection-discipline).");
    uint16_t _pad0;
    uint32_t _pad1;
    uint64_t next_order_id;      // monotonic id counter; 0 reserved for "no id"

    // Result queue: adapter worker thread (single producer with
    // worker_count==1) pushes CMD_FILL_RESULT here when an order
    // completes. Drainer thread (single consumer) drains it from
    // OrderManager_Tick. SPSC contract relies on worker_count==1.
    // v5.15.5.C.1 — alignas(64) at enclosing-struct level per NC2
    // (spsc-ring-embedded-in-hot-struct-cluster-discipline.md); ensures
    // preceding field's tail doesn't share line with ring head.
    alignas(64) SPSCRing<Command, OMS_RESULT_QUEUE_SIZE> result_queue;

    // WS fill queue (phase 04): user data websocket thread is the sole
    // producer, drainer is the sole consumer. Separate ring preserves
    // the SPSC contract — no MPSC needed. OrderManager_Tick drains
    // this after the REST result_queue.
    alignas(64) SPSCRing<Command, OMS_RESULT_QUEUE_SIZE> ws_result_queue;

    // Reconcile queue (phase 05): reconciler thread is the sole producer,
    // drainer is the sole consumer. Carries CMD_RECONCILE commands with
    // drift amounts. OrderManager_Tick drains this third.
    alignas(64) SPSCRing<Command, 64> reconcile_queue;

    // v4.7.37: per-core submit queues. Producer threads (producer slow-path
    // + per-core slow-path threads in SHARDED per_core_slow execution mode
    // since v5.0.0 default) push SubmitCommands here. The drainer thread pops
    // them in OMS_DrainSubmit and calls OrderManager_Submit serially —
    // preserving the documented "drainer is sole Submit caller" contract.
    //
    // Why per-core (not one queue): when per-core slow-paths spawn (Phase C),
    // each thread is the sole producer for its own ring. SPSC contract
    // holds. With one shared queue, multiple producers would need MPSC.
    alignas(64) SPSCRing<SubmitCommand<F>, OMS_SUBMIT_QUEUE_SIZE> submit_queues[MAX_EXECUTION_CORES];

    // === EVENT LOG MODE (phase 03 chunk 3) ===
    // 0 = legacy: OMS_Tick only marks FILLED/REJECTED and frees slots.
    //     OnEvent in ControllerEventLoop does the portfolio mutation.
    // 1 = event log: OMS_Tick runs the fill handler which opens/closes
    //     portfolio slots, updates balance, appends to the event log.
    //     OnEvent just bumps counters.
    // v5.15.5.C.1 — relocated to HOT cluster (read per drainer fill-process).
    // v5.15.5.C.3 (Finding A') — int field removed; event_log_mode is now a
    // 2-bit slot in oms_state_flags (FOREACH_OMS_STATE_MULTI_BIT registry
    // in MemHeaders/OmsStateFlagRegistry.hpp). Saves 4 bytes from HOT cluster
    // + closes the byte-per-low-cardinality-int pattern at OMS struct level
    // (per multi-bit-state-encoding-pattern.md; promotes pattern to CLAUDE.md
    // item candidate alongside item 20 bitmap-flag-api per CLAUDE.local.md
    // "codify" rule 2026-05-13). Accessor: MBS_EQ_U8(oms->oms_state_flags,
    // tt::MASK_OMS_STATE_EVENT_LOG_MODE, tt::SHIFT_OMS_STATE_EVENT_LOG_MODE, N).

    // === ORDER EVENT LOG (phase 03 chunk 3) ===
    // append-only log of order lifecycle events. populated in mode 1 by
    // the fill handler inside OMS_Tick. the portfolio can be reconstructed
    // from this log at any time via Portfolio_FromEventLog.
    // v5.15.5.C.1 — relocated to HOT cluster (drainer writes per fill-process
    // via async_ring push). Cross-thread atomics INSIDE event_log
    // (ring_full_spins, writer_realloc_failed_count, log_full_drops) are
    // isolated by OrderEventLog's internal alignas discipline.
    OrderEventLog<F> event_log;

    // ════════════════════════════════════════════════════════════════════
    // WARM CLUSTER — read on fill burst (HandleFill + DrainPostFill)
    // === BANK STATE (moved from EventLoopState in phase 03 chunk 1) ===
    // ════════════════════════════════════════════════════════════════════
    // Canonical portfolio + balance. After phase 03 mode 1 ships, these
    // are derived from the order event log. In mode 0 (legacy) and during
    // chunk 1 itself, EventLoop_OnEvent still mutates them directly.
    alignas(64) Portfolio<F> portfolio;
    Money       balance;
    Money       realized_pnl;
    // v5.15.5.F.4c.3 WIP2d-1.B.1 — DELETED: fee_rate, fee_rate_maker, fee_rate_taker, slippage_pct.
    // Per Class 27 structural closure: scalar cfg-mirror caches eliminated. Per-Order fee_rate
    // now lives on Order::pre_resolved (set at submit via Order_BindPreResolved with cfg.cores[c]).
    // HandleFill reads o->pre_resolved.fee_rate directly; DrainPostFill reads stored last_exit_fee
    // (set by HandleFill SELL). slippage_pct migrated to cfg.cores[c].slippage_pct + read at
    // EventLoop_OnEvent. Per decision-time-data-binding-pattern.md.
    // Phase 8 (post-coding c10) — maker/taker accounting counters parallel
    // to PortfolioController's (which only fire in legacy mode). HandleFill
    // increments these per fill so sharded mode has correct accounting.
    // Sanity invariant: total_fees == total_maker_fees + total_taker_fees
    // after every fill.
    uint32_t     maker_fills_count;
    uint32_t     taker_fills_count;
    Money       total_maker_fees;
    Money       total_taker_fees;
    Money       total_fees;       // mirrors PortfolioController.total_fees
                                    // for sanity invariant; OMS-side aggregate.

    // === EXIT-FILL FEEDBACK (Phase 6prep sharded c14) ===
    // HandleFill on ORDER_MARKET_SELL sets one bit per closed core in
    // last_closed_mask and writes the realized return into the parallel array.
    // The drainer reads after OrderManager_Tick, calls ConfidenceScorer_Update
    // for ML cores, then clears the mask. Single-threaded by construction —
    // drainer is the sole reader, OMS_Tick (same thread) is the sole writer.
    // realized_return = (exit_price - entry_price) / entry_price as a double
    // (ConfidenceScorer is double-only — see CLAUDE.md FPN_Binary-Only invariant).
    uint16_t     last_closed_mask;
    uint16_t     last_opened_mask;   // v5.15.5.C.1 — relocated adjacent (both per-fill bookkeeping masks)
    uint8_t      _pad_lof[4];        // pad to 8B before double[16] array
    double       last_realized_return[MAX_PORTFOLIO_POSITIONS];

    // === PER-FILL BOOKKEEPING (mode 1 per-core accounting) ===
    // HandleFill writes one record per fill into last_fill[portfolio_slot].
    // The drainer reads after OrderManager_Tick and applies them to the
    // matching CoreContext (mapped via Sharded_LegSlot under partials).
    // Same producer/consumer contract as last_closed_mask:
    //   - producer: HandleFill (drainer thread, OMS_Tick callee)
    //   - consumer: drainer thread post-Tick
    //   single-threaded → no atomics needed.
    //
    // entry_notional / entry_fee — populated on BUY fills; mask bit set in
    //   last_opened_mask (separate from last_closed_mask so paired entry+
    //   exit on different slots in the same drain cycle don't lose data).
    //
    // exit_net_pnl / exit_entry_notional / exit_total_fees / was_win —
    //   populated on SELL fills; mask bit set in last_closed_mask
    //   (existing). Drainer uses these for core_realized accumulation,
    //   core_open_notional decrement, core_fees, core_wins/core_losses.
    // v5.15.5.C.4 Phase H — entry-side fields (entry_notional, entry_fee)
    // v5.15.5.C.4 Phase K — FillRecord struct + last_fill[] array DELETED
    // entirely. All 6 prior FillRecord fields are now Position-derived at
    // DrainPostFill time (Phase G derives exit_net_pnl + exit_entry_notional
    // + exit_total_fees from Position state in CLOSE form; Phase H derives
    // entry_notional + entry_fee from Position state in OPEN form; Phase J
    // moved was_win to OMS-level last_was_win_bitmap). FillRecord-as-snapshot
    // class permanently EXTINCT. Future per-slot scratch state goes through:
    //   - Position struct (FOREACH_POSITION_FIELD; PERSIST or SKIP_PERSIST)
    //   - OMS sibling SoA arrays (FOREACH_OMS_SLOT_SCALAR_ARRAY)
    //   - OMS cross-slot bitmaps (FOREACH_OMS_BITMAP)
    // Per DESIGN_SPECS/slot-state-foreach-registry-with-storage-routing.md.

    // v5.13.0.B / v5.15.5.C.2 (S3b) — per-slot bit set by MLStrategy when
    // exit_predictor fires the exit on this specific slot. Consumed by
    // v5.13.4's reward attribution (drainer post-fill in HandleFill) +
    // cleared after.
    //
    // PARTIALS-AWARE: bit index = portfolio slot (0..MAX_PORTFOLIO_POSITIONS-1)
    // not core_id. Under partials, slot 2c+0 and 2c+1 are independent legs;
    // each has its OWN bit.
    //
    // Single-writer (slow-path MLStrategy thread per-core), single-reader
    // (drainer thread). uint16_t reads are naturally atomic on x86; cross-
    // thread visibility via the SPSC ring's release-acquire fence on the
    // submit/fill path (set BEFORE OMS_PushSubmit returns; read AFTER
    // OMS_DrainSubmit observes the order). No explicit atomic needed.
    //
    // Pre-S3b: uint8_t[16] array + zero-size pad = 16 bytes. Post-S3b:
    // uint16_t bitmap = 2 bytes (+ 6 bytes implicit pad to align next field).
    // 8 bytes saved + mask iteration symmetric with last_closed_mask /
    // last_opened_mask (CLAUDE.md item 20 Variant 6 — bitmap-flag-api.md
    // 8th application).
    uint16_t last_exit_predicted_bitmap;
    // v5.15.5.C.4 Phase J — was_win extracted from FillRecord to cross-slot
    // bitmap (Technique 3 of aggressive-memory-reduction-techniques.md +
    // CLAUDE.md item 20). Saves 8B/record × 16 = 128B; 1B per slot in old
    // FillRecord layout (incl. alignment pad) → 2B total at OMS level.
    uint16_t last_was_win_bitmap;
    // v5.15.5.C.5 — is_maker reverted from Position SKIP_PERSIST to OMS-level
    // cross-slot bitmap per slot-state-foreach-registry-with-storage-routing.md
    // decision tree (sparse-access ephemeral state lives in sibling SoA on OMS).
    // Enables Position to be 184B PERSIST + alignas(64) → 192B = 3 cache lines
    // exact (hot-side-array-element-alignment-for-sparse-access.md). 1 bit/slot
    // captured at HandleFill SELL; consumed by Phase G derive at DrainPostFill.
    uint16_t last_is_maker_bitmap;
    // v5.15.5.C.5 — exit_fill_price reverted from Position SKIP_PERSIST to
    // OMS-level sibling SoA array (same reasoning as last_is_maker_bitmap).
    // Per-slot captured at HandleFill SELL; consumed by Phase G derive at
    // DrainPostFill. 384B (24 × 16); not persisted.
    Money last_exit_fill_price[MAX_PORTFOLIO_POSITIONS];
    // v5.15.5.F.4c.3 WIP2d-1.B.1 — exit_fee stored at HandleFill SELL time (from o->pre_resolved.fee_rate).
    // Consumed by EventLoop_DrainPostFillOneCore for per-core accounting. Replaces the prior pattern of
    // RE-COMPUTING exit_fee in DrainPostFill from cfg lookup — that was a Class 27 adjacent shape
    // (DrainPostFill's recompute lost the authoritative pre_resolved.fee_rate that HandleFill captured).
    // Per decision-time-data-binding-pattern.md: the Order's pre_resolved.fee_rate is the canonical
    // value; subsequent consumers READ it, never recompute from cfg. 384B (24 × 16); not persisted.
    // v5.15.5.F.4d Step 7 (§ N.2): enrolled in FOREACH_OMS_PER_SLOT_FIELD registry — closes Class 30
    // latent drift (existed since .F.4c.3 r-4 but wasn't in registry; init/reset hand-maintained).
    Money last_exit_fee[MAX_PORTFOLIO_POSITIONS];

    // v5.15.5.F.4d Step 7 (§ N.2) — NEW per-slot bandit reward attribution. Written at HandleFill SELL
    // (computed reward_bps per the dispatch's chosen leaf reward fn — for exit-side dispatch via
    // g_exit_reward_dispatch[algo]). Read at DrainPostFill body OR calib emit body for downstream
    // metric capture. Sibling-array carrier mechanism per decision-time-data-binding-pattern.md
    // Stage 3 amendment v1.2 — bandit reward stays bound to slot through trade lifecycle without
    // requiring re-resolution from cfg (no Class 27 cache-mirror). 128B (8 × 16); not persisted.
    double bandit_reward_bps[MAX_PORTFOLIO_POSITIONS];

    // v5.13.0.B — calibration logging. Populated by slow-path body when
    // the exit-model fires (mirrors last_exit_predicted_bitmap). HandleFill
    // reads + emits CSV row on exit fill (any exit reason — predicted or
    // natural TP/SL/time). Operator post-processes the CSV offline (ROC,
    // Brier, precision-recall). last_exit_predicted_p stores the actual
    // blended probability at submit time.
    double last_exit_predicted_p[MAX_PORTFOLIO_POSITIONS];

    // v5.13.4 / v5.15.5.C.2.1 (LOW-2) — per-slot exit-predictor arm + regime
    // capture, bit-packed via FOREACH_OMS_META_SLOT (first application of
    // DESIGN_SPECS/multi-bit-state-encoding-pattern.md).
    //
    // Slow-path body writes the dominant exit_predictor horizon idx + regime
    // at submit time; HandleFill reads at fill time for Bandit_Update on
    // exit_bandits[regime]. Captured per-slot (not per-core) so partials legs
    // A/B independently attributable + stable across subsequent slow-path
    // cycles (subsequent predicts on the same core can't overwrite this
    // slot's chosen arm because there's already a fill pending). Cleared
    // in HandleFill post-update (drainer; same thread as read).
    //
    // Per-slot byte layout (see OmsExitPredictorMetaRegistry.hpp):
    //   bits 0..1 = regime (2 bits, 4 states)
    //   bits 2..5 = arm (4 bits, 0..15)
    //   bit 6     = valid flag (1 = populated; 0 = unset, replaces -1 sentinel)
    //   bit 7     = reserved
    //
    // Pre-LOW-2: int8_t last_exit_predicted_arm[16] + int8_t last_exit_
    // predicted_regime[16] = 32 bytes. Post-LOW-2: uint8_t[16] = 16 bytes.
    // 16 bytes saved per OMS. Parallel decode of arm + regime via ILP at
    // consumer sites (no data dependency between extracts).
    uint8_t last_exit_predicted_meta[MAX_PORTFOLIO_POSITIONS];
    // MAX_PORTFOLIO_POSITIONS=16 → 16 bytes; 16 % 8 == 0, so no padding required.

    // ════════════════════════════════════════════════════════════════════
    // COLD CLUSTER — boot-set + reconcile-only (drainer hot path doesn't read)
    // ════════════════════════════════════════════════════════════════════

    // Exchange adapter (by value). In paper mode all function pointers
    // are null and the OMS short-circuits before touching them. In live
    // mode the OMS calls submit_market_buy / submit_market_sell from
    // OrderManager_Submit and the adapter callback fires later from a
    // worker thread.
    // v5.15.5.C.1 — relocated to COLD cluster (set at boot, read only on
    // live Submit path which is rare relative to drainer-cycle reads).
    alignas(64) ExchangeAdapter<F> adapter;

    // === COLD-CLUSTER BOOLEAN STATE (v5.15.5.C.2 / S3a) ===
    // Bit-packed cohort of 3 single-thread boolean state flags:
    //   LIVE_TRADING         (bit 0) — boot-set; gates Submit adapter dispatch
    //   PARTIAL_EXIT_ENABLED (bit 1) — boot-set; drainer slot→core_id mapping
    //   KILL_SWITCH_TRIPPED  (bit 2) — drainer-thread write; serialized as int
    //                                    in snapshot (wire format preserved)
    // Pre-.C.2 layout: int live_trading + uint8 + _pad_pe[7] + uint8 + _pad_ks[7]
    //                   = 20 bytes. Post-.C.2: uint8_t bitmap (+ implicit padding) = 1 byte.
    // Accessor: BITMAP_IS_SET(oms.oms_state_flags, tt::MASK_OMS_STATE_<NAME>)
    //           or OMS_STATE_FLAG_IS_SET(oms, <NAME>). See OmsStateFlagRegistry.hpp.
    // 5 bits headroom for future COLD-cluster booleans (static_assert catches overflow).
    uint8_t oms_state_flags;
    // v5.15.5.C.2.1 (MEDIUM-2 close from /dod-audit on 852a6e3): explicit 7-byte
    // pad locking the COLD-cluster alignment gap before Money ks_min_balance.
    // Money needs 8-byte alignment (uint64 internally + CLAUDE.md item 27
    // padding-determinism). The 7-byte gap is structurally inherent; explicit
    // pad matches the prior _pad_pe[7] + _pad_ks[7] discipline pre-S3a.
    // Offset-lock static_assert at the end of struct (line ~520).
    uint8_t _pad_osf[7];

    // === KILL SWITCH THRESHOLDS (moved from EventLoopState in phase 03 chunk 1) ===
    // Configured by EventLoopState_ConfigureKillSwitch (which now writes
    // here through the OMS pointer). Disabled by default (both thresholds
    // zero). Tripping clears every registered core's permission with
    // RELEASE; resume via EventLoop_Unpause.
    // kill_switch_tripped bit lives in oms_state_flags above (v5.15.5.C.2).
    Money  ks_min_balance;       // trip if balance < this
    Money  ks_max_drawdown_pct;  // trip if (peak - balance) / peak > this (0 = disabled)
    Money  ks_peak_balance;      // running max of balance, updated on exits
    uint64_t ks_trips_total;      // count of trip events (observability)

    // v5.15.5.C.3 — paper-session start time (microseconds). Set at OrderManager_Init
    // to now_us(); updated to now_us() at each paper-reset (end of previous session +
    // start of new session). Persisted via FOREACH_OMS_PERSIST_FIELD so paper-mode
    // restart resumes from the same session-start anchor. Used by the paper-reset
    // archive flow (Phase 6) to format `{start_iso}_to_{end_iso}.paper` directory
    // names — gives operator a date-range identifier per paper session for offline
    // analysis (per_strategy_per_regime comparison across sessions).
    uint64_t paper_session_start_us;

    // FILE* opened lazily by OrderManager_OpenCalibrationLog (engine boot
    // calls when cfg.calibration_log_path is non-empty). Drainer thread
    // (sole HandleFill caller) is the only writer. Closed in Shutdown.
    FILE* calibration_log_file;

    // === TRADE LOG (moved from EventLoopState in phase 03 chunk 1) ===
    // Optional CSV trade log. nullptr → no logging (default). Not owned —
    // the engine main owns the ShardedTradeLog object and passes a pointer
    // here via EventLoopState_AttachTradeLog (which now writes through to
    // the OMS).
    ShardedTradeLog* trade_log;

    // v5.14.4.0 — high-watermark trade_id we've seen via myTrades (boot
    // reconcile path). Used by Reconcile_ApplyMissedFills (v5.14.4.B) to
    // filter out trades we already saw, replaying ONLY trades with
    // trade_id > last_seen_trade_id. Monotonic; updated post-reconcile
    // to max(seen) so the next reconcile cycle skips replay-applied trades.
    //
    // Single source of truth for "what trades have we observed?" — sister
    // to total_filled (which counts observed-and-applied) but specifically
    // tracks the exchange-side trade_id high-watermark for replay-safety.
    //
    // Future-thinking: when WS-side fill stream is added (v5.14.x+ post-
    // boot reconcile), bump this on every WS fill so post-disconnect
    // reconcile only replays trades newer than the WS-stream high water.
    // Boot today = once at startup; not a per-cycle path.
    uint64_t last_seen_trade_id;

    // ════════════════════════════════════════════════════════════════════
    // CROSS-THREAD OBSERVABILITY COUNTERS — alignas(64) cluster (NC1 ND1)
    // Writer = drainer thread (atomic_fetch_add per fill).
    // Reader = snapshot publisher (GUI thread) at 60 Hz via .load(RELAXED).
    // Isolation prevents publisher reads from invalidating cache lines of
    // drainer's other write targets (warm fee_state / last_fill etc.).
    // ════════════════════════════════════════════════════════════════════
    // Observability counters. Atomic so the TUI render loop on a different
    // core can read them without locks. Relaxed ordering throughout —
    // these are display-only.
    alignas(64) std::atomic<uint64_t> total_submitted;
    std::atomic<uint64_t> total_filled;
    std::atomic<uint64_t> total_rejected;

    // ════════════════════════════════════════════════════════════════════
    // CROSS-THREAD SAFETY CAS CLUSTER — alignas(64) cluster (NC1 ND1)
    // Writer = N slow-path threads via CAS (compare_exchange_strong).
    // Reader = slow-path predicate in RebuildOneCore.
    // Isolation prevents N-thread CAS contention from invalidating
    // neighbor warm fields (kill_switch_tripped, ks_*, etc.).
    // ════════════════════════════════════════════════════════════════════
    // v5.12.1.A.2 — WS-staleness emergency-flatten flag. CAS-set by
    // EventLoop_CheckWsStaleness when the producer's last-tick gap exceeds
    // cfg.ws_dead_time_flatten_threshold_secs. Multiple slow-paths may
    // race for the CAS; only one wins and submits the flatten via
    // EventLoop_FlattenAll. Read by EventLoop_RebuildOneCore (.A.3) for
    // recovery-refusal gating during the post-flatten reconcile window.
    // std::atomic<int> for the compare_exchange_strong primitive.
    alignas(64) std::atomic<int> flatten_pending;  // 0 = normal, 1 = flatten fired
    // v5.12.1.A.3 — recovery refusal deadline. Set by CheckWsStaleness
    // alongside flatten_pending=1: deadline = now_us + recovery_delay_secs*1e6.
    // RebuildOneCore reads this; while now_us < deadline, it forces
    // BUY_BLOCKED + SHALT_RECOVERY on every core's pending_params.
    // Cleared together with flatten_pending after deadline expires
    // (auto-recovery; no manual reset required).
    std::atomic<uint64_t> recovery_until_us;  // 0 = no recovery window active

    // ════════════════════════════════════════════════════════════════════
    // RAII destructor (v5.11.26)
    // ════════════════════════════════════════════════════════════════════
    // Stops the OrderEventLog async writer thread (v5.11.3.B feature) +
    // closes the disk file + frees the mmap'd entry buffer when this
    // struct goes out of scope. Idempotent on default-init / never-Init'd
    // state.
    //
    // v5.15.5.C.1 — destructor body unchanged through tier reorg per NC1
    // (raii-destructor-with-cluster-reorg-interaction.md). Field references
    // resolve by name; reorg changes offsets but not names. The cleanup
    // pairing (every Init alloc matches a Shutdown free) is preserved
    // because the reorg adds/removes ZERO members — only reorders.
    //
    // Why RAII (vs explicit OrderManager_Shutdown at every site):
    //  - 113 OrderManagerState declarations across the codebase, ZERO
    //    callers of OrderManager_Shutdown today (production-side
    //    EngineSharded_Run never calls it; OS cleanup at exit only).
    //  - Tests: 9 sites call OrderManager_Init directly; none call
    //    Shutdown. Each previously leaked the writer thread; ASAN
    //    flagged stack-use-after-scope at SPSCRing_TryPop because the
    //    writer outlived the test scope and read stale stack data.
    //  - One destructor closes both classes of leak in one place.
    //
    // Why safe (no copy/move concerns):
    //  - grep verified zero copy-init / memcpy of OrderManagerState
    //    across the codebase. struct is always declared then used by
    //    pointer; never duplicated.
    //  - Compiler-generated copy/move constructors would be deleted by
    //    SPSCRing's deleted copy semantics anyway (mutex-like guarantee
    //    via std::atomic<uint64_t>).
    // v5.15.5.F.4c.3 WIP2d-1.B.1 r-6 phase 2 — Pattern 5 sink-fn-pointer dispatch.
    // Per DESIGN_SPECS/sink-fn-pointer-for-optional-side-effect-pattern.md. Default = noop fn
    // (always-call, deterministic no-op). Real fns wired at boot when trade_log /
    // calibration_log_file Init succeeds. Eliminates `if (oms->trade_log)` /
    // `if (oms->calibration_log_file)` callsite branches in handle_buy_fill / handle_sell_fill.
    // Indirect call cost ~3-5ns vs predicted-correctly branch ~0-1ns; branchless wins on
    // p99 + variance per H20 + Caramel principle ("branchless even when slightly slower").
    void (*on_entry_fill_emit)(OrderManagerState<F>*, Order<F>*, Money, Money, Money) = &noop_fill_emit<F>;
    void (*on_exit_fill_emit)(OrderManagerState<F>*, Order<F>*, Money, Money, Money)  = &noop_fill_emit<F>;
    void (*on_exit_calibration)(OrderManagerState<F>*, Order<F>*, Money, Money, Money) = &noop_fill_emit<F>;

    // v5.15.5.F.4d Step 7 § F — per-core ezoo + core_cfg lookup for calib log consumer.
    // Per-core ARRAYS indexed by Order::core_id at consumer (sister to per-slot last_exit_fee[]
    // + bandit_reward_bps[] sibling-array pattern; OmsState is engine-wide single instance, NOT
    // per-core). void* keeps OmsState ML-agnostic (sister to ctx.ensemble_handle on CoreContext);
    // cast to EnsembleModelZoo<F>* / const PerCoreCfg<F>* in real_on_exit_calibration via
    // oms->ezoo_refs[o->core_id]. Default nullptr (test fixtures + pre-boot state + non-ML cores);
    // wired at EngineSharded per-core init alongside state.cores[i].ensemble_handle.
    void*       ezoo_refs[MAX_EXECUTION_CORES]     = {nullptr};   // EnsembleModelZoo<F>* per-core (lazy-cast)
    const void* core_cfg_refs[MAX_EXECUTION_CORES] = {nullptr};   // const PerCoreCfg<F>* per-core (lazy-cast)

    ~OrderManagerState() {
        OrderManager_Shutdown(this);
    }
};

// v5.15.5.F.4c.3 WIP2d-1.B.1 r-6 phase 2 — Pattern 5 noop fn definition.
// Single noop shared across all 3 fn-pointer fields (same sig). Used as the "always-call"
// default when subsystems are disabled; production cost = 1 indirect call (~3-5ns deterministic).
template <unsigned F>
inline void noop_fill_emit(OrderManagerState<F>*, Order<F>*, Money, Money, Money) {}

// v5.15.5.F.4c.3 WIP2d-1.B.1 r-6 phase 2 — Pattern 5 real fn definitions.
// Wrap the previous `if (oms->trade_log) { ... }` / `if (oms->calibration_log_file) { ... }` bodies.
// Set at boot site (e.g., ShardedTradeLog_Init, calibration_log_open) when respective subsystem enables.
template <unsigned F>
inline void real_on_entry_fill_emit(OrderManagerState<F>* oms, Order<F>* o,
                                     Money fill_price, Money fill_qty, Money entry_fee) {
    TradeEvent<F> synth{};
    synth.price     = fill_price;
    synth.timestamp = o->submitted_at_us;
    synth.core_id   = (uint16_t)o->core_id;
    synth.type      = TRADE_EVENT_ENTRY;
    ShardedTradeLog_RecordEntry(oms->trade_log, synth, o->strategy_id,
                                fill_price, fill_qty, entry_fee, oms->balance);
}

template <unsigned F>
inline void real_on_exit_fill_emit(OrderManagerState<F>* oms, Order<F>* o,
                                    Money fill_price, Money net, Money total_fee) {
    // Re-read position state (preserved through CloseSlot per Phase F invariant — only the
    // active_bitmap bit was cleared; entry_price + quantity remain stored on Position).
    const int pslot = (int)o->core_id;
    const Money entry_price_snap = oms->portfolio.positions[pslot].entry_price;
    const Money qty_snap         = oms->portfolio.positions[pslot].quantity;
    TradeEvent<F> synth{};
    synth.price     = fill_price;
    synth.timestamp = o->submitted_at_us;
    synth.core_id   = (uint16_t)o->core_id;
    synth.type      = TRADE_EVENT_EXIT;
    ShardedTradeLog_RecordExit(oms->trade_log, synth, o->strategy_id,
                               entry_price_snap, fill_price,
                               qty_snap, net, total_fee, oms->balance);
}

template <unsigned F>
inline void real_on_exit_calibration(OrderManagerState<F>* oms, Order<F>* o,
                                      Money fill_price, Money net, Money total_fee) {
    const int pslot = (int)o->core_id;
    const Money entry_price_snap = oms->portfolio.positions[pslot].entry_price;
    const Money qty_snap         = oms->portfolio.positions[pslot].quantity;
    const uint64_t ts_us = (uint64_t)
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    const double entry_d_calib = Money_ToDouble(entry_price_snap);
    const double exit_d_calib  = Money_ToDouble(fill_price);
    const double gain_pct      = entry_d_calib > 0.0
        ? (exit_d_calib - entry_d_calib) / entry_d_calib * 100.0 : 0.0;
    const double notional_d    = entry_d_calib > 0.0
        ? entry_d_calib * Money_ToDouble(qty_snap) : 0.0;
    const double pnl_bps       = notional_d > 0.0
        ? Money_ToDouble(net) / notional_d * 10000.0 : 0.0;
    const uint8_t pred_flag    = (uint8_t)BITMAP_IS_SET(oms->last_exit_predicted_bitmap, BITMAP_BIT_U16(pslot));
    const double pred_p        = oms->last_exit_predicted_p[pslot];
    (void)total_fee;

    // v5.15.5.F.4d Step 7 § F — decode bandit context from Order::flags_packed bits 17-25
    // (Pattern 4 decision-time-bound; sister to MASK_ORDER_PRE_RESOLVED at bit 16).
    const int bandit_active_state = MBS_OrderBanditActiveState(*o);
    const int bandit_regime       = MBS_OrderBanditRegime(*o);
    const int bandit_chosen_arm   = MBS_OrderBanditChosenArm(*o);

    // v5.15.5.F.4d Step 7 § F — per-slot bandit reward from FOREACH_OMS_PER_SLOT_FIELD sibling
    // array (added Step 7 § N.2; written at HandleFill SELL).
    const double reward_bps_attributed = oms->bandit_reward_bps[pslot];

    // v5.15.5.F.4d Step 7 § F — cast per-core ezoo_refs[core_id] / core_cfg_refs[core_id] to
    // typed pointers for ML-side access. OmsState is engine-wide single instance (line 662 of
    // EngineSharded boot); per-core ezoo + cfg slice lookup indexed by Order::core_id (== pslot
    // since each core owns 1 portfolio position). Nullptr-defensive: test fixtures + non-ML cores
    // have wiring pointers nullptr; telemetry coalesces to 0/0.0 placeholders.
    auto* ezoo     = static_cast<EnsembleModelZoo<F>*>(oms->ezoo_refs[pslot]);
    auto* core_cfg = static_cast<const PerCoreCfg<F>*>(oms->core_cfg_refs[pslot]);

    // Telemetry — null-coalesced.
    const int    thompson_telemetry_arm    = ezoo     ? ezoo->last_predicted_buy_thompson_arm               : 0;
    const double thompson_exp3_blend_alpha = core_cfg ? FPN_ToDouble(core_cfg->thompson_exp3_blend_alpha) : 0.0;

    // Per-arm Exp3 probabilities (telemetry). bandit_regime bounds-clamped defensively
    // (cfg parser clamps but belt-and-suspenders for replay-from-old-stamp scenarios).
    const int regime_clamped = (bandit_regime >= 0 && bandit_regime < NUM_REGIMES) ? bandit_regime : 0;
    double exp3_probs[BANDIT_MAX_ARMS] = {0};
    if (ezoo) Bandit_GetProbabilities(&ezoo->bandits[regime_clamped], exp3_probs);

    CALIB_LOG_EMIT_ROW(oms->calibration_log_file);
}

// ════════════════════════════════════════════════════════════════════════
// v5.15.5.C.1 — Layout invariants. Compile-time-enforced cluster anchors
// per `cache-layout-discipline-for-hot-side-structs.md` Rule 3+4 +
// `cross-thread-snapshot-publish-cluster-isolation.md` (ND1) +
// `spsc-ring-embedded-in-hot-struct-cluster-discipline.md` (NC2).
// Catches future field-insertion that silently breaks alignment.
// ════════════════════════════════════════════════════════════════════════
static_assert(alignof(OrderManagerState<64>) >= 64,
              "OrderManagerState MUST be 64-byte aligned (cluster anchors + alignas(64) on result_queue).");
// HOT cluster — first SPSCRing anchor (preceding fields = orders[] + scalars).
static_assert(offsetof(OrderManagerState<64>, result_queue) % 64 == 0,
              "result_queue (HOT cluster ring 1) MUST start at a cache-line boundary. "
              "See spsc-ring-embedded-in-hot-struct-cluster-discipline.md.");
// WARM cluster — Portfolio anchor (per-fill bookkeeping cluster start).
static_assert(offsetof(OrderManagerState<64>, portfolio) % 64 == 0,
              "Portfolio (WARM cluster anchor) MUST start at a cache-line boundary "
              "(separates HOT event_log from WARM per-fill state).");
// COLD cluster — adapter anchor (boot-set fields cluster start).
static_assert(offsetof(OrderManagerState<64>, adapter) % 64 == 0,
              "ExchangeAdapter (COLD cluster anchor) MUST start at a cache-line boundary "
              "(separates WARM per-fill state from COLD boot-set state).");
// Cross-thread observability cluster — total_submitted anchor.
static_assert(offsetof(OrderManagerState<64>, total_submitted) % 64 == 0,
              "Observability atomics cluster (total_submitted/filled/rejected) MUST be "
              "alignas(64)-isolated. Snapshot publisher reads at 60 Hz; isolation "
              "prevents reads from invalidating drainer-written neighbor warm fields. "
              "See cross-thread-snapshot-publish-cluster-isolation.md (ND1).");
// Cross-thread safety CAS cluster — flatten_pending anchor.
static_assert(offsetof(OrderManagerState<64>, flatten_pending) % 64 == 0,
              "Safety CAS atomics cluster (flatten_pending/recovery_until_us) MUST be "
              "alignas(64)-isolated. N slow-path threads CAS-contend; isolation prevents "
              "RFO storms on neighbor cold fields. "
              "See cross-thread-snapshot-publish-cluster-isolation.md (ND1).");
// v5.15.5.C.2.1 (MEDIUM-2 close from /dod-audit) — explicit 8-byte alignment
// lock for the COLD-cluster post-bitmap gap. Money ks_min_balance needs
// 8-byte alignment per CLAUDE.md item 27 (struct padding determinism). The
// _pad_osf[7] field is the explicit pad; this assert confirms the offset
// remains 8-byte-aligned after future field additions to the COLD cluster.
// Compile-time check; zero runtime cost (offsetof + % 8 fold to constant).
static_assert(offsetof(OrderManagerState<64>, ks_min_balance) % 8 == 0,
              "ks_min_balance (Money) MUST be 8-byte aligned. If this trips, "
              "the COLD cluster gained a non-8-aligned field between oms_state_flags "
              "and ks_min_balance. Re-check _pad_osf[7] explicit pad.");

//======================================================================================================
// [FILL RESULT CALLBACK — invoked by the adapter worker thread]
//======================================================================================================
// Templated on F so it matches the OrderManagerState<F>* user_ctx. Each F
// instantiation produces a distinct function pointer, which the adapter
// stores type-erased as `OrderCallback`. The callback is the only path
// by which the worker thread mutates OMS state — it pushes a Command
// into the result_queue and returns. The drainer thread later picks it
// up via OrderManager_Tick.
//
// Pushing into the SPSC ring is wait-free. If the queue is full (should
// never happen with size 256 unless something is very wrong), the result
// is dropped with a log message.
//======================================================================================================
template <unsigned F>
static void OrderManager_FillResultCallback(void* user_ctx,
                                             uint64_t client_id,
                                             const OrderResult* result) {
    OrderManagerState<F>* oms = (OrderManagerState<F>*)user_ctx;
    Command cmd;
    cmd.type     = (uint8_t)CMD_FILL_RESULT;
    cmd.order_id = client_id;
    cmd.result   = *result;
    if (!SPSCRing_TryPush(&oms->result_queue, cmd)) {
        std::fprintf(stderr,
                     "[OMS] result queue full, dropping fill result for order %llu\n",
                     (unsigned long long)client_id);
    }
}

//======================================================================================================
// [INIT]
//======================================================================================================
// Zero the order table, clear the bitmap, install the adapter and the
// live_trading flag. The adapter is copied by value — the caller still
// owns whatever the adapter.ctx points at, but the function pointers
// and the ctx pointer are captured into the OMS for its lifetime.
//
// In paper mode the caller passes a default-constructed (zero-initialized)
// adapter — all function pointers null. The OMS will short-circuit Submit
// to FILLED before touching them.
//
// OMS owns the bank state since v4.x (originally Phase 03 chunk 1B).
// OrderManager_Init takes starting_balance + fee_rate so the OMS is fully
// self-contained from init onwards. EventLoopState_Init takes an OMS
// pointer and forwards all financial reads through the OMS.
//
// event_log_mode parameter (default 0):
//   0 = legacy mode. OMS_Tick only marks orders FILLED/REJECTED and frees
//       slots. Portfolio mutation happens in EventLoop_OnEvent (unchanged).
//   1 = event log mode. OMS_Tick runs a fill handler that opens/closes
//       portfolio slots, updates balance, and appends to the event log.
//       EventLoop_OnEvent just bumps counters.
//   2-3 = reserved for future modes. Stored as 2-bit slot in oms_state_flags
//         (v5.15.5.C.3 MULTI_BIT slot — see FOREACH_OMS_STATE_MULTI_BIT).
//
// partial_exit_enabled parameter (v5.15.5.C.3 Finding A):
//   0 = single-leg geometry; slot index == core_id (1:1 mapping).
//   1 = paired-leg geometry; slot index = 2*core_id + leg (legs A+B per core).
//   Set as BIT in oms_state_flags (MASK_OMS_STATE_PARTIAL_EXIT_ENABLED).
//   Pre-Finding A: engine boot called OMS_STATE_FLAG_SET(PARTIAL_EXIT_ENABLED)
//   externally after OrderManager_Init returned (Class-18 mirror at the
//   external SET site). Post-Finding A: passed as OrderManager_Init parameter;
//   the registry walk inside OMS_INIT_AUTOPOPULATE sets the bit via the
//   BIT-kind row for `partial_exit_enabled`.
//======================================================================================================
// v5.9.5e — `event_log_path` lets callers separate the in-memory event log
// infrastructure (single-writer mode=1 used by both live + backtest for
// fill+drain pipeline parity since v4.7.15) from the on-disk persistence
// (live engine only — restart-recovery via replay). Backtest passes nullptr
// or "" to use in-memory-only mode=1: no disk load, no append-on-write.
// Pre-v5.9.5e the path was hardcoded "logging/order_events.bin" and
// backtest_mode=1 silently inherited live OMS state across runs (stale
// balance, polluted next_event_id), breaking backtest hermeticity. The
// feature/label collection pipeline doesn't read OMS, so ML training
// output was unaffected — but Past Runs P&L + trade history started
// from the contaminated balance.
//
// v5.15.5.C.3 Phase 3b — body migration from ~140 LOC manual init to a
// single OMS_INIT_AUTOPOPULATE invocation. All scalar/BIT/MULTI_BIT/ATOMIC
// field init lives in FOREACH_OMS_FIELD (MemHeaders/OmsFieldRegistry.hpp);
// sub-struct inits (Portfolio, Order per-slot loop, FillRecord per-slot,
// SPSC rings, OrderEventLog conditional + StartAsyncWriter) live in the
// AUTOPOPULATE macro body. Adding a new OMS-level field is now ONE row in
// the canonical registry; INIT/RESET/PERSIST views auto-flow.
//======================================================================================================
// v5.15.5.F.4c.3 WIP2d-1.B.1 — `fee_rate` param DELETED. Per-Order fee_rate lives on
// Order::pre_resolved (set at submit via Order_BindPreResolved with cfg.cores[c]). OMS no
// longer holds a global fee_rate. Callers drop the arg.
template <unsigned F>
inline void OrderManager_Init(OrderManagerState<F>* oms,
                              const ExchangeAdapter<F>& adapter,
                              int live_trading,
                              int partial_exit_enabled,
                              Money starting_balance,
                              int event_log_mode = 0,
                              const char* event_log_path = "logging/order_events.bin") {
    OMS_INIT_AUTOPOPULATE(oms, adapter, live_trading, partial_exit_enabled,
                          starting_balance, event_log_mode, event_log_path);
}

//======================================================================================================
// [SUBMIT — async via adapter, returns immediately]
//======================================================================================================
// Paper mode:
//   Mode 0 (legacy): Bump total_submitted and total_filled, return the next
//   id. No slot allocation, no adapter call, no result queue traffic.
//   Symmetric with the legacy fire-and-forget pattern but routed through the
//   OMS so the counters are consistent across paper and live.
//
//   Mode 1 (event log): allocate a slot, mark ORDER_SUBMITTED, push a
//   synthetic CMD_FILL_RESULT into the result queue so OMS_Tick handles
//   it uniformly. The fill handler then opens/closes portfolio slots and
//   appends to the event log, just like in live mode.
//
// Live mode:
//   1. Allocate a free slot from the bitmap. Drop on full (logged).
//   2. Populate the Order with id, type, qty, mark ORDER_SUBMITTED.
//   3. Call adapter.submit_market_buy or _market_sell with the OMS
//      pointer as user_ctx and OrderManager_FillResultCallback<F> as
//      the callback. The adapter enqueues to its worker thread and
//      returns immediately — Submit does NOT block.
//   4. If the adapter rejects the enqueue (its queue is full), mark
//      the slot REJECTED and free it.
//
// qty is Money per the FPN_Binary-only-accounting rule in CLAUDE.md.
//
// Phase 03 chunk 3: extra context parameters for the fill handler.
//   intended_tp / intended_sl: TP/SL to apply at fill time (entry only).
//   strategy_id: STRATEGY_* constant for trade log CSV.
//   event_price: market price at submit time; used as the fill price in
//     paper mode (no adapter callback to supply one).
//======================================================================================================
template <unsigned F>
inline uint64_t OrderManager_Submit(OrderManagerState<F>* oms, const SubmitCommand<F>& cmd) {
    // v5.15.5.F.4c.3 WIP2d-1.B.1 — option (l): SubmitCommand POD is canonical arg.
    // Local extraction matches prior positional names so body internals are unchanged.
    // Production callers MUST set cmd.core_cfg = &cfg.cores[c] (discipline; runtime TT_ASSERT
    // backstop catches misses at HandleFill).
    const int16_t                core_id     = cmd.core_id;
    const OrderType              type        = (OrderType)cmd.order_type;
    const Money                 qty         = cmd.qty;
    const Money                 intended_tp = cmd.intended_tp;
    const Money                 intended_sl = cmd.intended_sl;
    const uint8_t                strategy_id = cmd.strategy_id;
    const Money                 event_price = cmd.event_price;
    const uint8_t                leg         = cmd.leg;
    const ::PerCoreCfg<F>* const core_cfg    = cmd.core_cfg;
    uint64_t id = oms->next_order_id++;

    // Paper mode + legacy (mode 0): count and return. Never touch the
    // table or the adapter. Mode 1 paper falls through to the slot
    // allocation path below so the fill handler runs in OMS_Tick.
    // v5.15.5.C.3 — event_log_mode is a 2-bit slot in oms_state_flags
    // (see MemHeaders/OmsStateFlagRegistry.hpp). Use MBS_EQ_U8 for K-state
    // slot semantics (consistent with the 4 other read sites — drainer
    // ProcessFillCommand uses MBS_EQ_U8(..., 1); this site checks ..., 0).
    // /dod-audit MEDIUM-3 close (consistency over BITMAP_NONE for K-state).
    if (!BITMAP_IS_SET(oms->oms_state_flags, tt::MASK_OMS_STATE_LIVE_TRADING) &&
        MBS_EQ_U8(oms->oms_state_flags, tt::MASK_OMS_STATE_EVENT_LOG_MODE,
                  tt::SHIFT_OMS_STATE_EVENT_LOG_MODE, 0)) {
        oms->total_submitted.fetch_add(1, std::memory_order_relaxed);
        oms->total_filled.fetch_add(1, std::memory_order_relaxed);
        return id;
    }

    // Live mode: allocate a slot.
    uint16_t free_mask = (uint16_t)~oms->order_bitmap;
    if (free_mask == 0) {
        std::fprintf(stderr,
                     "[OMS] order table full (%d slots), dropping submission "
                     "for core=%d type=%u\n",
                     MAX_INFLIGHT_ORDERS, (int)core_id, (unsigned)type);
        return 0;
    }
    int slot = __builtin_ctz((unsigned int)free_mask);
    oms->order_bitmap |= (uint16_t)(1u << slot);

    // v5.11.5.B — encode slot in the upper 4 bits of the order id. The wire
    // representation (clientOrderId on the exchange) and Order::id both
    // carry this encoded value. ProcessFillCommand decodes the slot
    // directly from cmd.order_id for O(1) lookup, replacing the prior
    // O(MAX_INFLIGHT_ORDERS) linear scan over order_bitmap.
    //
    // Encoding: bits 63..60 = slot (0-15, fits in 4 bits since
    // MAX_INFLIGHT_ORDERS=16); bits 59..0 = monotonic counter.
    // Lower 60 bits give 1.15e18 unique IDs — a million years at 1/μs.
    //
    // Audit: LATENCY_OPTIMIZATION_AUDIT.md Part 9. Plan: master plan
    // v5.11.5 item 3.
    uint64_t encoded_id = id | ((uint64_t)slot << 60);
    Order_Init(&oms->orders[slot], encoded_id, core_id, type);
    id = encoded_id;  // returned to caller + used in cmd.order_id below
    oms->orders[slot].submitted_at_us = (uint64_t)
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    oms->orders[slot].requested_qty = qty;
    oms->orders[slot].intended_tp   = intended_tp;
    oms->orders[slot].intended_sl   = intended_sl;
    oms->orders[slot].strategy_id   = strategy_id;
    oms->orders[slot].event_price   = event_price;
    Order_SetLeg(&oms->orders[slot], leg);  // P.3: 0/1 for partial exits
    Order_SetState(&oms->orders[slot], ORDER_SUBMITTED);
    // v5.15.5.F.4c.3 WIP2d-1.B.1 — decision-time data binding: pre-resolve fee_rate /
    // slippage_pct from per-core cfg onto Order before any fill can arrive. is_maker
    // defaults to false at Order_Init (set later by adapter executionReport for LIMIT
    // orders; MARKET orders are always taker — current engine is MARKET-only).
    // Per DESIGN_SPECS/decision-time-data-binding-pattern.md + branchless-dispatch-
    // discipline.md (branchless via cmov-to-stub; always-call Order_BindPreResolved
    // regardless of whether caller passed cfg). nullptr → zero-init stub → pre_resolved
    // values stay FPN_Zero (same effective behavior as skip-when-null, but no branch).
    static const ::PerCoreCfg<F> NULL_PER_CORE_CFG_STUB{};
    const ::PerCoreCfg<F>* effective_cfg = core_cfg ? core_cfg : &NULL_PER_CORE_CFG_STUB;
    Order_BindPreResolved(&oms->orders[slot], *effective_cfg);
    // A25 (D-205): tp_pct is resolved at the submit SITE (ResolvePerFillTpPct needs
    // StrategyParameters, not include-reachable here) + carried on the command; set it on
    // the Order's pre_resolved beside the bound fee_rate/slippage_pct. 0 (unwired/test
    // paths) → handle_buy_fill keeps the legacy intended_tp anchor (bytewise-identical).
    oms->orders[slot].pre_resolved.tp_pct = cmd.tp_pct;
    oms->total_submitted.fetch_add(1, std::memory_order_relaxed);

    // Paper mode + event log (mode 1): push a synthetic fill result so
    // OMS_Tick runs the fill handler uniformly. The fill price is the
    // event_price captured at submit time. No adapter call needed.
    if (!BITMAP_IS_SET(oms->oms_state_flags, tt::MASK_OMS_STATE_LIVE_TRADING)) {
        Command cmd;
        cmd.type     = (uint8_t)CMD_FILL_RESULT;
        cmd.order_id = id;
        std::memset(&cmd.result, 0, sizeof(cmd.result));
        cmd.result.success        = 1;
        // v5.15.5.F.4d.1.E.0.10 A9 — apply paper/backtest slippage HERE: the single production slip SSoT
        // (D-202 + adversarial-pessimistic-simulation-discipline.md). Paper-gated BY CONSTRUCTION — this is
        // the !LIVE_TRADING branch; live books the real executionReport price untouched. Consumes the bound
        // pre_resolved.slippage_pct (set at Order_BindPreResolved above) — closes the bound-but-unread orphan
        // (the old EventLoop_OnEvent slip was dead-in-mode-1, now deleted). Pessimistic-sim: entry fills WORSE
        // (higher), exit fills WORSE (lower); slip_pct=0 (default) -> Money_Mul=0 -> no-op, so no guard branch.
        // Branchless sign-select per H20: both sides computed; cmov-select on the value (not a branch).
        const Money a9_slip      = Money_Mul(event_price, oms->orders[slot].pre_resolved.slippage_pct);
        const Money a9_buy_fill  = Money_Add(event_price, a9_slip);   // entry (BUY): pay higher
        const Money a9_sell_fill = Money_Sub(event_price, a9_slip);   // exit (SELL): receive lower
        const Money a9_fill      = (type == ORDER_MARKET_SELL) ? a9_sell_fill : a9_buy_fill;
        cmd.result.avg_fill_price = Money_ToDouble(a9_fill);
        cmd.result.fill_qty       = Money_ToDouble(qty);
        std::strncpy(cmd.result.exchange_id, "PAPER",
                     sizeof(cmd.result.exchange_id) - 1);
        if (!SPSCRing_TryPush(&oms->result_queue, cmd)) {
            std::fprintf(stderr,
                         "[OMS] result queue full on paper submit for order %llu\n",
                         (unsigned long long)id);
        }
        return id;
    }

    // Defensive: live mode requires a wired adapter.
    if (oms->adapter.submit_market_buy == nullptr ||
        oms->adapter.submit_market_sell == nullptr) {
        std::fprintf(stderr,
                     "[OMS] live mode but adapter is not wired, rejecting order %llu\n",
                     (unsigned long long)id);
        Order_SetState(&oms->orders[slot], ORDER_REJECTED);
        oms->total_rejected.fetch_add(1, std::memory_order_relaxed);
        oms->order_bitmap &= (uint16_t)~(1u << slot);
        return 0;
    }

    // Async submit to the adapter. The callback fires later from the
    // worker thread once the REST round trip completes.
    double qty_d = Money_ToDouble(qty);
    int submit_ok = (type == ORDER_MARKET_BUY)
        ? oms->adapter.submit_market_buy(oms->adapter.ctx, id, qty_d,
                                          OrderManager_FillResultCallback<F>,
                                          (void*)oms)
        : oms->adapter.submit_market_sell(oms->adapter.ctx, id, qty_d,
                                           OrderManager_FillResultCallback<F>,
                                           (void*)oms);
    if (!submit_ok) {
        std::fprintf(stderr,
                     "[OMS] adapter rejected enqueue for order %llu, freeing slot\n",
                     (unsigned long long)id);
        Order_SetState(&oms->orders[slot], ORDER_REJECTED);
        oms->total_rejected.fetch_add(1, std::memory_order_relaxed);
        oms->order_bitmap &= (uint16_t)~(1u << slot);
        return 0;
    }
    return id;
}

//======================================================================================================
// [PUSH SUBMIT — funnel external Submit requests through SPSC queue]
//======================================================================================================
// v4.7.37 (Phase B reordered): producer threads call OMS_PushSubmit instead
// of OrderManager_Submit. Drainer thread pops from submit_queues and calls
// OrderManager_Submit serially, preserving the documented "drainer is sole
// Submit caller" OMS contract.
//
// Per-core SPSC: each core_id has its own queue. Today's caller (producer
// slow-path) is the sole producer for ALL queues — still SPSC per ring.
// When Phase C spawns per-core slow-paths, each thread is the sole producer
// for its own ring. SPSC contract holds in both modes.
//
// Returns false if the queue is full (caller should consider this an error
// — slow-path submission backlog suggests drainer is starved). Returns true
// on successful push.
//======================================================================================================
template <unsigned F>
inline bool OMS_PushSubmit(OrderManagerState<F>* oms, const SubmitCommand<F>& cmd) {
    // v5.15.5.F.4c.3 WIP2d-1.B.1 — option (l): SubmitCommand POD is canonical arg.
    // No internal assembly; caller constructs the cmd struct directly + we push it.
    // Eliminates the prior 9-field unpack/repack ceremony.
    if (cmd.core_id < 0 || cmd.core_id >= MAX_EXECUTION_CORES) {
        std::fprintf(stderr,
                     "[OMS] PushSubmit: invalid core_id=%d (max=%d)\n",
                     (int)cmd.core_id, MAX_EXECUTION_CORES);
        return false;
    }
    bool pushed = SPSCRing_TryPush(&oms->submit_queues[cmd.core_id], cmd);
    if (!pushed) {
        std::fprintf(stderr,
                     "[OMS] PushSubmit: queue full for core=%d type=%u "
                     "(drainer starved?)\n",
                     (int)cmd.core_id, (unsigned)cmd.order_type);
    }
    return pushed;
}

//======================================================================================================
// [DRAIN SUBMIT — drainer thread pops queues and calls Submit serially]
//======================================================================================================
// v4.7.37 (Phase B reordered): called from the drainer thread's loop, before
// OrderManager_Tick. Drains all per-core submit queues and calls
// OrderManager_Submit for each command. Single-threaded — preserves OMS
// contract.
//
// Returns the number of commands drained.
//======================================================================================================
template <unsigned F>
inline int OMS_DrainSubmit(OrderManagerState<F>* oms, int num_cores) {
    int drained = 0;
    int max = (num_cores > MAX_EXECUTION_CORES) ? MAX_EXECUTION_CORES : num_cores;
    for (int c = 0; c < max; ++c) {
        SubmitCommand<F> cmd;
        while (SPSCRing_TryPop(&oms->submit_queues[c], &cmd)) {
            // v5.15.5.F.4c.3 WIP2d-1.B.1 — option (l): pass POD directly; no unpack ceremony.
            OrderManager_Submit(oms, cmd);
            drained++;
        }
    }
    return drained;
}

//======================================================================================================
// [FEE ACCOUNTING — maker/taker counters + totals]
//======================================================================================================
// v5.15.5.C.2 (S5): single-source-of-truth for the 8-line fee bookkeeping
// duplicated byte-identical at both entry-fill and exit-fill sites in
// HandleFill (only differing in entry_fee vs exit_fee). Class 18 mirror
// close (CLAUDE.md item 19 + DESIGN_SPECS/structural-fix-preferred-
// decision-framework.md). Future fee categories (taker_rebate,
// partial_taker, etc.) extend this single helper rather than touching
// N call sites.
//======================================================================================================
// v5.15.5.F.4c.3 WIP2d-1.B.1 — branchless via mask-select counters + ternary FPN_Binary store.
// Pre-r-6: `if (is_maker) { maker++; maker_fees+= } else { taker++; taker_fees+= }` — 1 branch per fill.
// Post-r-6: always-compute both counter increments via mask (0 or 1); ternary FPN_AddSat-or-nop on each
// fee bucket lowers to cmov. Per branchless-dispatch-discipline.md Pattern 3 mask-select. ~3-5 cycles
// extra per fill (drainer slow path; ~0.005% of 100μs budget) for deterministic latency.
template <unsigned F>
inline void OrderManager_AccountMakerTakerFee(
    OrderManagerState<F>* oms, int is_maker, Money fee) {
    oms->total_fees = Money_Add(oms->total_fees, fee);
    const bool maker = is_maker != 0;
    oms->maker_fills_count   = (uint32_t)(oms->maker_fills_count + (uint32_t)maker);
    oms->taker_fills_count   = (uint32_t)(oms->taker_fills_count + (uint32_t)(!maker));
    oms->total_maker_fees    = maker ? Money_Add(oms->total_maker_fees, fee) : oms->total_maker_fees;
    oms->total_taker_fees    = maker ? oms->total_taker_fees : Money_Add(oms->total_taker_fees, fee);
}

// GUARD — maker/taker fee-desync (DORMANT; the reactivatable-assumption shape, Class-40 sibling).
// pre_resolved.fee_rate is bound at SUBMIT as TAKER (the engine is MARKET-only — OrderManager_Submit refuses
// non-MARKET, and MARKET fills are always taker). So a maker fill CANNOT occur today: this is a NEVER-TAKEN
// branch (zero behavior change now). If it ever fires, LIMIT orders were enabled WITHOUT re-resolving
// fee_rate from is_maker at fill time → the taker-rate fee is charged + bucketed as maker (a live fee
// over-charge + wrong P&L split). Fail LOUD so it can't ship silently. The real fix: re-resolve
// pre_resolved.fee_rate from is_maker BEFORE enabling LIMIT orders (TECH_DEBT-154). __builtin_expect-rare
// per H20 exception (cold capital-check; matches the file's existing FILE*-null guard convention).
template <unsigned F>
inline void OMS_GuardTakerBoundFeeBasis(const Order<F>* o) {
    if (__builtin_expect(Order_GetIsMaker(o) != 0, 0)) {
        fprintf(stderr, "[FATAL] OrderManager fee-desync: maker fill on a TAKER-bound fee_rate — LIMIT orders "
                        "were enabled without re-resolving fee_rate from is_maker at fill (TECH_DEBT-154).\n");
    }
}

//======================================================================================================
// [PATTERN 1 1D TYPE DISPATCH HANDLERS — v5.15.5.F.4c.3 WIP2d-1.B.1]
//======================================================================================================
// BUY/SELL dispatch via fn pointer table indexed by Order_GetType(o). Per branchless-dispatch-
// discipline.md Pattern 1 + H20 invariant. Class 28 first canonical. Inner bodies use Pattern 3
// mask-selects for all data-dependent dispatch (TP/SL/INSIDE reason, peak balance via FPN_Max,
// last_realized_return write mask-gated, last_is_maker / last_was_win bitmaps mask-select).
// FILE* null guards tagged __builtin_expect per H20 exception #4 (fprintf side effects can't be
// cheaply mask-gated; Phase 2 Portfolio/TradeLog mask-param refactor handles the remaining cases).
//======================================================================================================

// BUY handler — entry fill: open portfolio slot + record entry fee + bump counters + trade log.
template <unsigned F>
inline void handle_buy_fill(OrderManagerState<F>* oms, Order<F>* o, Money fill_price, Money fill_qty,
                            Money booked_fee) {
    // Ship-B P3: the fee is resolved ONCE in OrderManager_HandleFill (venue-reported
    // commission preferred per D-173, computed fallback) and threaded here — the
    // handler no longer derives its own copy (booking-rule SSoT).
    const Money entry_fee  = booked_fee;
    OMS_GuardTakerBoundFeeBasis(o);   // dormant fee-desync guard (TECH_DEBT-154); never-taken while MARKET-only
    OrderManager_AccountMakerTakerFee(oms, (int)Order_GetIsMaker(o), entry_fee);
    // A25 (D-205): arm the trail anchor (original_tp) relative to the ACTUAL fill, not the
    // expected-entry intended_tp — post-A9 they diverge under slippage, so the 4 sharded
    // *_ExitAdjustSharded trails + the exit-bandit counterfactual (ControllerEventLoop.hpp:1749)
    // were arming at the wrong price. tp_pct (leg-effective, resolved at submit) carries the
    // per-fill fraction; tp_pct==0 → fallback to intended_tp = the legacy path (bytewise-identical).
    const Money per_fill_tp = !Money_IsZero(o->pre_resolved.tp_pct)
        ? Money_Add(fill_price, Money_Mul(fill_price, o->pre_resolved.tp_pct))
        : o->intended_tp;
    Portfolio_OpenSlot(&oms->portfolio, (int)o->core_id,
                       fill_price, fill_qty,
                       per_fill_tp, o->intended_sl, entry_fee);
    oms->last_opened_mask |= (uint16_t)(1u << (int)o->core_id);
    // v5.15.5.F.4c.3 WIP2d-1.B.1 r-6 phase 2 — Pattern 5 sink-fn-pointer dispatch (branchless).
    // Default = noop_fill_emit (no-op); set to real_on_entry_fill_emit at boot when trade_log Init succeeds.
    // Per DESIGN_SPECS/sink-fn-pointer-for-optional-side-effect-pattern.md.
    oms->on_entry_fill_emit(oms, o, fill_price, fill_qty, entry_fee);
}

// SELL handler — exit fill: close portfolio slot, compute P&L, update balance, write trade log.
template <unsigned F>
inline void handle_sell_fill(OrderManagerState<F>* oms, Order<F>* o, Money fill_price, Money fill_qty,
                             Money booked_fee) {
    const int pslot = (int)o->core_id;
    // v4.7.19 race guard — H20 exception #4 (genuine predicate; alternative requires Portfolio refactor).
    // __builtin_expect-rare: production fires only on rare hot-path-SG / manual-close race.
    if (__builtin_expect((oms->portfolio.active_bitmap & (uint16_t)(1u << pslot)) == 0, 0)) {
        std::fprintf(stderr,
            "[OMS] handle_sell_fill: slot %d SELL on already-closed slot — "
            "no-op (race between manual close and hot-path SG)\n", pslot);
        return;
    }
    const Money entry_price_snap = oms->portfolio.positions[pslot].entry_price;
    const Money entry_fee        = oms->portfolio.positions[pslot].entry_fee;
    const Money qty_snap         = oms->portfolio.positions[pslot].quantity;
    const Money tp_snap          = oms->portfolio.positions[pslot].take_profit_price;
    const Money sl_snap          = oms->portfolio.positions[pslot].stop_loss_price;

    // v5.15.5.F.4c.3 WIP2d-1.B.1 — branchless last_realized_return + last_closed_mask update.
    // Pre-r-6: `if (entry_price_d > 0.0 && bounds) { write }` — 2 branches.
    // Post-r-6: always compute return value; mask-gate the WRITE via ternary store-select.
    // pslot bounds already enforced by HandleFill caller guard + active_bitmap check above.
    const double entry_price_d   = Money_ToDouble(entry_price_snap);
    const double exit_price_d    = Money_ToDouble(fill_price);
    const bool   valid_entry     = entry_price_d > 0.0;
    const double computed_ret    = valid_entry ? (exit_price_d - entry_price_d) / entry_price_d : 0.0;
    oms->last_realized_return[pslot] = valid_entry ? computed_ret : oms->last_realized_return[pslot];
    const uint16_t closed_bit    = (uint16_t)(1u << pslot);
    oms->last_closed_mask        = (uint16_t)(oms->last_closed_mask | (valid_entry ? closed_bit : (uint16_t)0));

    const Money gross         = Portfolio_CloseSlot(&oms->portfolio, pslot, fill_price);
    const Money exit_fee      = booked_fee;   // Ship-B P3: resolved once in HandleFill (D-173 rule)
    OMS_GuardTakerBoundFeeBasis(o);   // dormant fee-desync guard (TECH_DEBT-154); never-taken while MARKET-only
    OrderManager_AccountMakerTakerFee(oms, (int)Order_GetIsMaker(o), exit_fee);
    const Money total_fee     = Money_Add(entry_fee, exit_fee);
    const Money net           = Money_Sub(gross, total_fee);
    oms->balance               = Money_Add(oms->balance, net);
    oms->realized_pnl          = Money_Add(oms->realized_pnl, net);
    // v5.15.5.F.4c.3 WIP2d-1.B.1 — peak balance via FPN_Max (branchless mask-select replaces `if`).
    oms->ks_peak_balance       = Money_Max(oms->ks_peak_balance, oms->balance);

    // Exit-side scratch on OMS sibling arrays.
    oms->last_exit_fill_price[pslot] = fill_price;
    oms->last_exit_fee[pslot]        = exit_fee;

    // Branchless mask-select on last_is_maker_bitmap (Pattern 3).
    const uint16_t maker_bit       = BITMAP_BIT_U16(pslot);
    const uint16_t maker_mask_bits = Order_GetIsMaker(o) ? maker_bit : (uint16_t)0;
    oms->last_is_maker_bitmap      = (uint16_t)((oms->last_is_maker_bitmap & ~maker_bit) | maker_mask_bits);

    // Branchless mask-select on last_was_win_bitmap (Pattern 3).
    const uint16_t win_bit       = BITMAP_BIT_U16(pslot);
    const uint16_t was_win_mask  = Money_Gt(net, Money_Zero()) ? win_bit : (uint16_t)0;
    oms->last_was_win_bitmap     = (uint16_t)((oms->last_was_win_bitmap & ~win_bit) | was_win_mask);

    // v5.15.5.F.4c.3 WIP2d-1.B.1 r-6 phase 2 — Pattern 5 sink-fn-pointer dispatch (branchless).
    // Default = noop_fill_emit; set to real_on_exit_calibration when calibration_log_file fopen() succeeds.
    oms->on_exit_calibration(oms, o, fill_price, net, total_fee);

    // v5.1.6 exit reason diagnostic — branchless ternary chain (replaces if-else-if; cmov on pointer).
    {
        const double entry_d = Money_ToDouble(entry_price_snap);
        const double exit_d  = Money_ToDouble(fill_price);
        const double tp_d    = Money_ToDouble(tp_snap);
        const double sl_d    = Money_ToDouble(sl_snap);
        const double gain    = entry_d > 0.0 ? (exit_d - entry_d) / entry_d : 0.0;
        const double net_d   = Money_ToDouble(net);
        const double fee_d   = Money_ToDouble(total_fee);
        // Branchless: chained ternaries → cmov on const char*.
        const bool is_tp     = (tp_d > 0.0) && (exit_d >= tp_d - 1e-6);
        const bool is_sl     = (sl_d > 0.0) && (exit_d <= sl_d + 1e-6);
        const char* reason   = is_tp ? "TP_HIT" : (is_sl ? "SL_HIT" : "INSIDE");
        std::fprintf(stderr,
            "[exit-diag] slot=%d strat=%u reason=%s entry=%.2f exit=%.2f "
            "tp=%.2f sl=%.2f gain=%+.4f%% gross=%+.4f fees=%.4f net=%+.4f\n",
            pslot, (unsigned)o->strategy_id, reason,
            entry_d, exit_d, tp_d, sl_d, gain * 100.0,
            Money_ToDouble(gross), fee_d, net_d);
    }

    // v5.15.5.F.4c.3 WIP2d-1.B.1 r-6 phase 2 — Pattern 5 sink-fn-pointer dispatch (branchless).
    // Default = noop_fill_emit; set to real_on_exit_fill_emit at boot when trade_log Init succeeds.
    oms->on_exit_fill_emit(oms, o, fill_price, net, total_fee);
}

// Fn pointer table — indexed by OrderType (0=MARKET_BUY, 1=MARKET_SELL, 2=LIMIT_BUY, 3=LIMIT_SELL).
// 4 × 8B = 32B = 1 cache line; L1-hot on pinned drainer.
template <unsigned F>
using FillHandler = void (*)(OrderManagerState<F>*, Order<F>*, Money, Money, Money /*booked_fee*/);

template <unsigned F>
inline constexpr FillHandler<F> g_fill_dispatch[4] = {
    &handle_buy_fill<F>,    // ORDER_MARKET_BUY  = 0
    &handle_sell_fill<F>,   // ORDER_MARKET_SELL = 1
    &handle_buy_fill<F>,    // ORDER_LIMIT_BUY   = 2 (future)
    &handle_sell_fill<F>,   // ORDER_LIMIT_SELL  = 3 (future)
};

//======================================================================================================
// [FILL HANDLER — Pattern 1 dispatch entrypoint]
//======================================================================================================
// Slim entrypoint: bounds guard (H20 exception #4) + pre-resolve discipline warn + audit log append
// + branchless dispatch via fn pointer table. All BUY/SELL-specific logic lives in handle_buy_fill /
// handle_sell_fill above. Future LIMIT_BUY/LIMIT_SELL types extend mechanically — 2 rows in g_fill_dispatch.
//======================================================================================================
template <unsigned F>
inline void OrderManager_HandleFill(OrderManagerState<F>* oms, Order<F>* o,
                                     Money fill_price, Money fill_qty,
                                     double venue_commission = 0.0,
                                     const char* venue_commission_asset = nullptr) {
    // Bounds guard — H20 exception #4 (genuine predicate without alternative); __builtin_expect-rare.
    if (__builtin_expect(o->core_id < 0 || o->core_id >= MAX_PORTFOLIO_POSITIONS, 0)) {
        std::fprintf(stderr,
                     "[OMS] fill handler: core_id %d out of range [0,%d), "
                     "skipping order %llu\n",
                     (int)o->core_id, MAX_PORTFOLIO_POSITIONS,
                     (unsigned long long)o->id);
        return;
    }
    // Pre-resolve bind discipline runtime warn (cost ~1 cycle in production).
    Order_WarnIfNotPreResolved(o, "OrderManager_HandleFill");
    // ── Ship-B P3 fee booking rule (D-173, SSoT) ──────────────────────────────────
    // Venue-reported commission is AUTHORITATIVE when it arrives in the quote asset
    // (the engine's books are USDT-quoted; multi-quote rides the .E.1 venue registry).
    // Anything else (BNB-paid, base-asset, absent) books the COMPUTED fee
    // (pre_resolved rate × notional) and warns — the operator-visible signal that
    // venue fees and engine books have diverged (the D-173 degrade arm).
    const Money computed_fee = Money_Mul(Money_Mul(fill_price, fill_qty),
                                         o->pre_resolved.fee_rate);
    Money booked_fee = computed_fee;
    if (venue_commission > 0.0 && venue_commission_asset) {
        if (std::strcmp(venue_commission_asset, "USDT") == 0) {
            booked_fee = Money{ money_from_double_payload(venue_commission) };
        } else {
            std::fprintf(stderr,
                "[OMS] fill %llu: venue commission %.8f %s != quote asset — booking "
                "COMPUTED fee (D-173 fallback; check BNB-burn / fee-asset config)\n",
                (unsigned long long)o->id, venue_commission, venue_commission_asset);
        }
    }
    // Audit log append — common across BUY/SELL/future LIMIT. The S-3 fee slot makes
    // the event log fee-self-contained from this epoch on.
    OrderEventLog_Append(&oms->event_log,
        OrderEvent_MakeFill<F>(
            o->id, o->submitted_at_us,
            Order_GetType(o), o->core_id,
            fill_price, fill_qty,
            o->intended_tp, o->intended_sl,
            booked_fee));
    // Pattern 1 1D dispatch — branchless via fn pointer table. Deterministic latency regardless
    // of BUY/SELL access pattern. Closes Class 28 first canonical.
    g_fill_dispatch<F>[(uint8_t)Order_GetType(o)](oms, o, fill_price, fill_qty, booked_fee);
}



//======================================================================================================
// [PROCESS ONE FILL COMMAND — unified handler for REST and WS fills]
//======================================================================================================
// Looks up the order by id, applies the fill or rejection, runs the mode 1
// fill handler if applicable, frees the slot. Called from the unified drain
// loop in OrderManager_Tick for both CMD_FILL_RESULT and CMD_WS_FILL.
//
// Returns 1 if the command was processed, 0 if skipped (dedup, not found, etc.)
//======================================================================================================
template <unsigned F>
inline int OrderManager_ProcessFillCommand(OrderManagerState<F>* oms, const Command& cmd) {
    // WS surprise fill (order_id == 0): log and skip.
    if (cmd.order_id == 0 && cmd.type == (uint8_t)CMD_WS_FILL) {
        std::fprintf(stderr,
                     "[OMS] WS surprise fill (no clientOrderId), ignoring — "
                     "reconciliation will catch it\n");
        return 0;
    }

    // v5.11.5.B — O(1) slot lookup via encoded id.
    // Bits 63..60 of cmd.order_id carry the slot index assigned at submit
    // time. Decode + verify the slot still holds the expected order
    // (defends against late-arriving callbacks for an already-freed slot
    // that has been reused for a different order).
    int slot = (int)((cmd.order_id >> 60) & 0xFu);
    if ((oms->order_bitmap & (uint16_t)(1u << slot)) == 0 ||
        oms->orders[slot].id != cmd.order_id) {
        slot = -1;  // slot freed, or reused for a different order
    }

    if (slot < 0) {
        // Order not found. For REST: slot freed already. For WS: REST beat us
        // (order already processed and freed). Either way, skip silently.
        if (cmd.type == (uint8_t)CMD_FILL_RESULT) {
            std::fprintf(stderr,
                         "[OMS] result for unknown order %llu (slot freed already?), ignoring\n",
                         (unsigned long long)cmd.order_id);
        }
        return 0;
    }

    Order<F>* o = &oms->orders[slot];

    // Dedup: if order is already FILLED (another source beat us), skip.
    if (Order_GetState(o) == ORDER_FILLED) return 0;

    if (cmd.result.success) {
        std::strncpy(o->exchange_id, cmd.result.exchange_id,
                     sizeof(o->exchange_id) - 1);
        o->exchange_id[sizeof(o->exchange_id) - 1] = '\0';

        // ACK-only results (from REST when WS is active): fill_qty == 0.
        // Transition to ACKNOWLEDGED, keep the slot open for the WS fill.
        if (cmd.result.fill_qty == 0.0 && cmd.result.avg_fill_price == 0.0) {
            Order_SetState(o, ORDER_ACKNOWLEDGED);
            return 1;  // slot stays open — don't free
        }

        o->avg_fill_price = Money{ money_from_double_payload(cmd.result.avg_fill_price) };  // OrderResult ring bridge (scaled-i64 vehicle rides P3/S-8)
        o->filled_qty     = Money{ money_from_double_payload(cmd.result.fill_qty) };
        // Phase 8: maker/taker flag from Binance executionReport, parsed in c3.
        // Fee_Compute reads this for entry-fee math when the controller books
        // the fill. is_maker stays at Order_Init's 0 (taker) for synchronous
        // REST fills (Phase 02 path) — Binance market orders are taker by def.
        Order_SetIsMaker(o, (bool)cmd.result.is_maker);
        // Phase 8: pick FILLED vs PARTIAL based on Binance "X" field (parsed
        // in c3 as order_complete). For ACK-only paths above we already
        // returned with ORDER_ACKNOWLEDGED, so reaching here means an actual
        // fill (full or partial) happened. order_complete=0 = PARTIALLY_FILLED.
        // Defensive: if order_complete is missing from the event (parser sets
        // 0 in that case), we err toward PARTIAL — keeps the order alive in
        // the OMS, won't lose track. Subsequent fill events resolve to FILLED.
        Order_SetState(o, cmd.result.order_complete ? ORDER_FILLED : ORDER_PARTIAL);
        if (Order_GetState(o) == ORDER_FILLED) {
            oms->total_filled.fetch_add(1, std::memory_order_relaxed);
        }

        // Mode 1 fill handler: portfolio mutation + event log.
        if (MBS_EQ_U8(oms->oms_state_flags, tt::MASK_OMS_STATE_EVENT_LOG_MODE, tt::SHIFT_OMS_STATE_EVENT_LOG_MODE, 1)) {
            Money fill_price = o->avg_fill_price;
            Money fill_qty   = o->filled_qty;
            OrderManager_HandleFill(oms, o, fill_price, fill_qty,
                                     cmd.result.commission, cmd.result.commission_asset);
        }
    } else {
        std::fprintf(stderr,
                     "[OMS] order %llu FAIL core=%d code=%d msg=%s\n",
                     (unsigned long long)o->id,
                     (int)o->core_id,
                     cmd.result.error_code,
                     cmd.result.error_message);
        Order_SetState(o, ORDER_REJECTED);
        oms->total_rejected.fetch_add(1, std::memory_order_relaxed);

        // Mode 1: append rejection to event log for the audit trail.
        if (MBS_EQ_U8(oms->oms_state_flags, tt::MASK_OMS_STATE_EVENT_LOG_MODE, tt::SHIFT_OMS_STATE_EVENT_LOG_MODE, 1)) {
            OrderEventLog_Append(&oms->event_log,
                OrderEvent_MakeRejection<F>(
                    o->id, o->submitted_at_us,
                    Order_GetType(o), o->core_id,
                    cmd.result.error_message));
        }
    }

    // Free the slot on terminal transition.
    oms->order_bitmap &= (uint16_t)~(1u << slot);
    return 1;
}

//======================================================================================================
// [PROCESS ONE RECONCILE COMMAND]
//======================================================================================================
template <unsigned F>
inline void OrderManager_ProcessReconcile(OrderManagerState<F>* oms, const Command& cmd) {
    double drift = cmd.result.avg_fill_price;           // repurposed field
    double exchange_balance = cmd.result.fill_qty;      // repurposed field

    std::fprintf(stderr,
                 "[OMS] RECONCILE: drift=$%.4f, correcting balance to match "
                 "exchange ($%.4f)\n", drift, exchange_balance);

    oms->balance = Money{ money_from_double_payload(exchange_balance) };  // D-103 reconcile ingress (exact venue parse rides P3)
    if (Money_Gt(oms->balance, oms->ks_peak_balance)) {
        oms->ks_peak_balance = oms->balance;
    }

    if (MBS_EQ_U8(oms->oms_state_flags, tt::MASK_OMS_STATE_EVENT_LOG_MODE, tt::SHIFT_OMS_STATE_EVENT_LOG_MODE, 1)) {
        OrderEvent<F> recon_event;
        std::memset(&recon_event, 0, sizeof(recon_event));
        recon_event.type       = OEVT_RECONCILED;
        recon_event.order_type = ORDER_MARKET_BUY;  // placeholder
        recon_event.core_id    = -1;
        recon_event.price      = Money{ money_from_double_payload(drift) };
        std::strncpy(recon_event.reason, cmd.result.error_message,
                     sizeof(recon_event.reason) - 1);
        recon_event.reason[sizeof(recon_event.reason) - 1] = '\0';
        OrderEventLog_Append(&oms->event_log, recon_event);
    }
}

//======================================================================================================
// [TICK — drain all command queues]
//======================================================================================================
// Drainer thread calls this on every drain pass. Drains three SPSC rings
// sequentially (REST fills, WS fills, reconcile corrections) and dispatches
// each command to the appropriate handler. Adding a new command source is
// one new SPSCRing field + one drain call here + one handler function.
//======================================================================================================
template <unsigned F>
inline void OrderManager_Tick(OrderManagerState<F>* oms) {
    // Drain all three command queues through the unified dispatcher.
    // Adding a new command source: add one SPSCRing field, one drain
    // call here, one handler function. No duplication.
    Command cmd;

    // 1. REST fills (adapter worker thread)
    while (SPSCRing_TryPop(&oms->result_queue, &cmd)) {
        if (cmd.type == (uint8_t)CMD_FILL_RESULT)
            OrderManager_ProcessFillCommand(oms, cmd);
    }

    // 2. WS fills (user data websocket thread)
    while (SPSCRing_TryPop(&oms->ws_result_queue, &cmd)) {
        if (cmd.type == (uint8_t)CMD_WS_FILL)
            OrderManager_ProcessFillCommand(oms, cmd);
    }

    // 3. Reconciliation corrections (reconciler thread)
    while (SPSCRing_TryPop(&oms->reconcile_queue, &cmd)) {
        if (cmd.type == (uint8_t)CMD_RECONCILE)
            OrderManager_ProcessReconcile(oms, cmd);
    }
}

//======================================================================================================
// [SHUTDOWN]
//======================================================================================================
// The OMS itself owns no threads or sockets — adapter lifetime is managed
// externally (the caller calls adapter.shutdown after joining the drainer).
// This function is a no-op kept for symmetry with Init and so a future
// phase can hook in additional cleanup without breaking the API.
//======================================================================================================
template <unsigned F>
inline void OrderManager_Shutdown(OrderManagerState<F>* oms) {
    OrderEventLog_Free(&oms->event_log);
    // v5.13.0.B — calibration log cleanup. nullptr-safe: most runs leave
    // it null (cfg.calibration_log_path empty by default).
    if (oms->calibration_log_file) {
        std::fclose(oms->calibration_log_file);
        oms->calibration_log_file = nullptr;
    }
}

//======================================================================================================
// [v5.13.0.B — calibration log open]
//======================================================================================================
// Opens cfg.calibration_log_path in append mode. Writes a header row if
// the file is new (size == 0). Engine boot calls this AFTER OrderManager_Init
// when the cfg field is non-empty. Single-thread (boot); after this returns
// the FILE* is read-only on the drainer thread (sole writer in HandleFill).
//
// Returns 0 on success / -1 on failure (failure is non-fatal: log goes to
// stderr; engine continues without calibration logging this session).
//======================================================================================================
template <unsigned F>
inline int OrderManager_OpenCalibrationLog(OrderManagerState<F>* oms,
                                             const char* path) {
    if (!path || path[0] == '\0') return 0;  // disabled — not an error
    oms->calibration_log_file = std::fopen(path, "a");
    if (!oms->calibration_log_file) {
        std::fprintf(stderr,
            "[OMS] OpenCalibrationLog: fopen failed for '%s' — calibration "
            "logging disabled this session\n", path);
        return -1;
    }
    // v5.15.5.F.4c.3 WIP2d-1.B.1 r-6 phase 2 — Pattern 5 sink-fn-pointer wire-to-real.
    // Per DESIGN_SPECS/sink-fn-pointer-for-optional-side-effect-pattern.md. Default = noop;
    // set-to-real when fopen succeeds → handle_sell_fill dispatches to calibration row emit.
    oms->on_exit_calibration = &tt::real_on_exit_calibration<F>;
    // Header row only if file is empty (new). Use ftell after open.
    std::fseek(oms->calibration_log_file, 0, SEEK_END);
    long sz = std::ftell(oms->calibration_log_file);
    if (sz == 0) {
        // v5.14.10.D — registry-driven header via FOREACH_CALIB_LOG_COL.
        // Byte-identical to prior hand-coded literal; closes TECH_DEBT-010.
        CalibLog_EmitHeader(oms->calibration_log_file);
        std::fflush(oms->calibration_log_file);
    }
    return 0;
}

//======================================================================================================
// [INTROSPECTION HELPERS — for tests and TUI]
//======================================================================================================
template <unsigned F>
inline uint64_t OrderManager_TotalSubmitted(const OrderManagerState<F>* oms) {
    return oms->total_submitted.load(std::memory_order_relaxed);
}

template <unsigned F>
inline uint64_t OrderManager_TotalFilled(const OrderManagerState<F>* oms) {
    return oms->total_filled.load(std::memory_order_relaxed);
}

template <unsigned F>
inline uint64_t OrderManager_TotalRejected(const OrderManagerState<F>* oms) {
    return oms->total_rejected.load(std::memory_order_relaxed);
}

template <unsigned F>
inline int OrderManager_InflightCount(const OrderManagerState<F>* oms) {
    return __builtin_popcount((unsigned int)oms->order_bitmap);
}

}  // namespace tt
