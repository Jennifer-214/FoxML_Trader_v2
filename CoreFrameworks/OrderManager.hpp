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
// of FPN<F> width.
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
// SubmitCommand is templated on F so the FPN<F> fields are sized correctly.
template <unsigned F>
struct SubmitCommand {
    int16_t  core_id;       // execution-core id; routes the resulting fill
    uint8_t  order_type;    // OrderType enum (uint8 for compact storage)
    uint8_t  strategy_id;   // strategy that produced this order
    uint8_t  leg;           // 0 = leg A or single, 1 = leg B (partials)
    uint8_t  _pad[3];
    FPN<F>   qty;
    FPN<F>   intended_tp;
    FPN<F>   intended_sl;
    FPN<F>   event_price;   // for the trade log + fill price
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
template <unsigned F>
struct OrderManagerState {
    // ════════════════════════════════════════════════════════════════════
    // HOT CLUSTER — drainer reads every cycle
    // (orders, rings, event_log_mode, event_log)
    // ════════════════════════════════════════════════════════════════════
    Order<F> orders[MAX_INFLIGHT_ORDERS];
    uint16_t order_bitmap;       // 1 = slot in use, 0 = free. uint16_t caps at 16 slots.
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

    // v4.7.37: per-core submit queues. Producer threads (today: producer
    // slow-path; future: per-core slow-path threads in engine_arch=
    // per_core_slow) push SubmitCommands here. The drainer thread pops
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
    int event_log_mode;

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
    FPN<F>       balance;
    FPN<F>       realized_pnl;
    FPN<F>       fee_rate;
    // Phase 8 — maker/taker rates. Init sets both = fee_rate for backward
    // compat; engine main.cpp sets them from cfg.fee_rate_maker/taker before
    // any fills arrive. HandleFill picks per Order's is_maker field.
    FPN<F>       fee_rate_maker;
    FPN<F>       fee_rate_taker;
    // v4.2.1 — paper-mode slippage simulation. Adjusts fill prices to model
    // realistic worst-case execution: BUY fills above gate price, SELL fills
    // below trigger price. ONLY applied in paper mode (live=0); in live the
    // exchange already returns the real post-slippage price. Engine sets
    // this from cfg.slippage_pct after Init. Default zero = no simulation.
    FPN<F>       slippage_pct;
    // Phase 8 (post-coding c10) — maker/taker accounting counters parallel
    // to PortfolioController's (which only fire in legacy mode). HandleFill
    // increments these per fill so sharded mode has correct accounting.
    // Sanity invariant: total_fees == total_maker_fees + total_taker_fees
    // after every fill.
    uint32_t     maker_fills_count;
    uint32_t     taker_fills_count;
    FPN<F>       total_maker_fees;
    FPN<F>       total_taker_fees;
    FPN<F>       total_fees;       // mirrors PortfolioController.total_fees
                                    // for sanity invariant; OMS-side aggregate.

    // === EXIT-FILL FEEDBACK (Phase 6prep sharded c14) ===
    // HandleFill on ORDER_MARKET_SELL sets one bit per closed core in
    // last_closed_mask and writes the realized return into the parallel array.
    // The drainer reads after OrderManager_Tick, calls ConfidenceScorer_Update
    // for ML cores, then clears the mask. Single-threaded by construction —
    // drainer is the sole reader, OMS_Tick (same thread) is the sole writer.
    // realized_return = (exit_price - entry_price) / entry_price as a double
    // (ConfidenceScorer is double-only — see CLAUDE.md FPN-Only invariant).
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
    struct FillRecord {
        FPN<F>  entry_notional;       // entry: fill_price × fill_qty
        FPN<F>  entry_fee;            // entry fee (maker or taker)
        FPN<F>  exit_net_pnl;         // exit: gross − total_fees (signed)
        FPN<F>  exit_entry_notional;  // exit: entry_price_snap × qty_snap
        FPN<F>  exit_total_fees;      // exit: entry_fee + exit_fee booked at close
        int8_t  was_win;              // exit only: 1 if exit_net_pnl > 0
        int8_t  _pad[7];
    };
    FillRecord last_fill[MAX_PORTFOLIO_POSITIONS];

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
    uint16_t _pad_lepb[3];  // align next field on 8-byte boundary

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
    // pad locking the COLD-cluster alignment gap before FPN<F> ks_min_balance.
    // FPN<F> needs 8-byte alignment (uint64 internally + CLAUDE.md item 27
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
    FPN<F>  ks_min_balance;       // trip if balance < this
    FPN<F>  ks_max_drawdown_pct;  // trip if (peak - balance) / peak > this (0 = disabled)
    FPN<F>  ks_peak_balance;      // running max of balance, updated on exits
    uint64_t ks_trips_total;      // count of trip events (observability)

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
    ~OrderManagerState() {
        OrderManager_Shutdown(this);
    }
};

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
// lock for the COLD-cluster post-bitmap gap. FPN<F> ks_min_balance needs
// 8-byte alignment per CLAUDE.md item 27 (struct padding determinism). The
// _pad_osf[7] field is the explicit pad; this assert confirms the offset
// remains 8-byte-aligned after future field additions to the COLD cluster.
// Compile-time check; zero runtime cost (offsetof + % 8 fold to constant).
static_assert(offsetof(OrderManagerState<64>, ks_min_balance) % 8 == 0,
              "ks_min_balance (FPN<F>) MUST be 8-byte aligned. If this trips, "
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
// Phase 03 chunk 1B: the OMS now owns the bank state. OrderManager_Init
// takes starting_balance + fee_rate so the OMS is fully self-contained
// from init onwards. EventLoopState_Init takes an OMS pointer instead of
// its own balance/fee_rate and forwards all financial reads through the
// OMS.
//
// Phase 03 chunk 3: event_log_mode parameter (default 0):
//   0 = legacy mode. OMS_Tick only marks orders FILLED/REJECTED and frees
//       slots. Portfolio mutation happens in EventLoop_OnEvent (unchanged).
//   1 = event log mode. OMS_Tick runs a fill handler that opens/closes
//       portfolio slots, updates balance, and appends to the event log.
//       EventLoop_OnEvent just bumps counters.
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
template <unsigned F>
inline void OrderManager_Init(OrderManagerState<F>* oms,
                              const ExchangeAdapter<F>& adapter,
                              int live_trading,
                              FPN<F> starting_balance,
                              FPN<F> fee_rate,
                              int event_log_mode = 0,
                              const char* event_log_path = "logging/order_events.bin") {
    for (int i = 0; i < MAX_INFLIGHT_ORDERS; ++i) {
        Order_Init(&oms->orders[i], 0, -1, ORDER_MARKET_BUY);
        oms->orders[i].state = ORDER_FILLED;  // mark as inactive (terminal)
    }
    oms->order_bitmap   = 0;
    oms->next_order_id  = 1;
    oms->adapter        = adapter;
    // v5.15.5.C.2 (S3a) — COLD-cluster boolean cohort bit-pack. All 3 flags
    // start cleared; live_trading bit set conditionally below. Engine init
    // sets PARTIAL_EXIT_ENABLED per cfg after Init (line ~665 in EngineSharded.hpp).
    oms->oms_state_flags = 0;
    if (live_trading) {
        OMS_STATE_FLAG_SET(*oms, LIVE_TRADING);
    }
    oms->last_seen_trade_id = 0;  // v5.14.4.0 — high-watermark for replay-safe boot reconcile

    SPSCRing_Init(&oms->result_queue);
    SPSCRing_Init(&oms->ws_result_queue);
    SPSCRing_Init(&oms->reconcile_queue);
    // v4.7.37: per-core submit queues (Phase B reordered)
    for (int i = 0; i < MAX_EXECUTION_CORES; ++i) {
        SPSCRing_Init(&oms->submit_queues[i]);
    }

    // Phase 03 chunk 1B: bank state lives here now.
    Portfolio_Init(&oms->portfolio);
    oms->balance             = starting_balance;
    oms->realized_pnl        = FPN_Zero<F>();
    oms->fee_rate            = fee_rate;
    oms->fee_rate_maker      = fee_rate; // Phase 8: legacy default = same rate
    oms->fee_rate_taker      = fee_rate; // engine sets per-cfg after Init
    oms->slippage_pct        = FPN_Zero<F>(); // v4.2.1: engine sets per-cfg after Init
    // Phase 8 (post-coding c10) — counter init
    oms->maker_fills_count   = 0;
    oms->taker_fills_count   = 0;
    oms->total_maker_fees    = FPN_Zero<F>();
    oms->total_taker_fees    = FPN_Zero<F>();
    oms->total_fees          = FPN_Zero<F>();
    // Phase 6prep sharded c14 — exit-fill feedback channel
    oms->last_closed_mask    = 0;
    for (int i = 0; i < MAX_PORTFOLIO_POSITIONS; ++i) {
        oms->last_realized_return[i] = 0.0;
    }
    // Mode 1 per-fill bookkeeping
    oms->last_opened_mask    = 0;
    // v5.15.5.C.2 (S3a) — partial_exit_enabled bit cleared in oms_state_flags above.
    // v5.15.5.C.2 (S3b) — bitmap clear replaces per-slot byte writes.
    oms->last_exit_predicted_bitmap = 0;
    for (int i = 0; i < MAX_PORTFOLIO_POSITIONS; ++i) {
        oms->last_fill[i].entry_notional      = FPN_Zero<F>();
        oms->last_fill[i].entry_fee           = FPN_Zero<F>();
        oms->last_fill[i].exit_net_pnl        = FPN_Zero<F>();
        oms->last_fill[i].exit_entry_notional = FPN_Zero<F>();
        oms->last_fill[i].exit_total_fees     = FPN_Zero<F>();
        oms->last_fill[i].was_win             = 0;
        // v5.13.0.B — per-slot exit-predictor attribution + calibration
        oms->last_exit_predicted_p[i]         = 0.0;
        // v5.13.4 / v5.15.5.C.2.1 (LOW-2) — per-slot bandit arm + regime
        // capture, bit-packed in last_exit_predicted_meta[i]. OMS_META_CLEAR
        // sets all slots (regime, arm, valid) to 0; valid=0 replaces the
        // pre-LOW-2 int8_t = -1 sentinel.
        OMS_META_CLEAR(oms->last_exit_predicted_meta[i]);
    }
    // v5.13.0.B — calibration log file lazy-opened by engine boot via
    // OrderManager_OpenCalibrationLog when cfg.calibration_log_path set.
    oms->calibration_log_file = nullptr;
    oms->ks_min_balance      = FPN_Zero<F>();
    oms->ks_max_drawdown_pct = FPN_Zero<F>();
    oms->ks_peak_balance     = starting_balance;  // initial peak = start
    // v5.15.5.C.2 (S3a) — kill_switch_tripped bit cleared in oms_state_flags above.
    oms->ks_trips_total      = 0;
    oms->trade_log           = nullptr;
    // v5.12.1.A.2 — emergency-flatten flag init.
    oms->flatten_pending.store(0, std::memory_order_relaxed);
    // v5.12.1.A.3 — recovery deadline init (0 = inactive).
    oms->recovery_until_us.store(0, std::memory_order_relaxed);

    oms->total_submitted.store(0, std::memory_order_relaxed);
    oms->total_filled.store(0, std::memory_order_relaxed);
    oms->total_rejected.store(0, std::memory_order_relaxed);

    // Phase 03 chunk 3: event log mode + log allocation.
    oms->event_log_mode = event_log_mode;
    // Phase 07: disk persistence. In mode 1, load previous events from
    // disk (reconstructs next_event_id), then open the file for append
    // so new events write through. On first run the load returns 0 (no
    // file) and InitWithFile creates a fresh one with a header.
    // v5.9.5e — disk persistence only when caller passes a non-empty
    // event_log_path. Backtest passes nullptr/"" → mode=1 in-memory-only:
    // fill+drain pipeline still active (parity with live), but no
    // load-from-disk + no append-to-disk. Live engine still gets full
    // restart-recovery via the default "logging/order_events.bin".
    int has_disk_path = (event_log_path && event_log_path[0]);
    if (event_log_mode == 1 && has_disk_path) {
        OrderEventLog_Init(&oms->event_log);  // allocate buffer first
        int loaded = OrderEventLog_LoadFromDisk(&oms->event_log, event_log_path);
        if (loaded > 0) {
            // replay the loaded events to reconstruct portfolio + balance
            FoldResult<F> fold = Portfolio_FromEventLog(&oms->event_log,
                                                         starting_balance, fee_rate);
            oms->portfolio    = fold.portfolio;
            oms->balance      = fold.balance;
            oms->realized_pnl = fold.realized_pnl;
            if (FPN_GreaterThan(oms->balance, oms->ks_peak_balance))
                oms->ks_peak_balance = oms->balance;
            std::fprintf(stderr, "[OMS] replayed %d events from disk, "
                         "balance=$%.2f\n", loaded, FPN_ToDouble(oms->balance));
        }
        // open for append (writes new events through to disk)
        OrderEventLog_InitWithFile(&oms->event_log, event_log_path);
    } else {
        // mode=0 OR mode=1+no-path → in-memory only.
        OrderEventLog_Init(&oms->event_log);
    }
    // v5.11.3.C — start the async writer thread. From here on, the drainer's
    // OrderEventLog_Append calls enqueue + return; the writer thread does
    // realloc + fwrite + fflush off the drainer's tail-latency path. If
    // pthread_create fails (rare), Append falls back to inline sync apply
    // — same correctness guarantees, just no isolation. Tests that don't
    // want the thread can call OrderEventLog_StopAsyncWriter immediately.
    OrderEventLog_StartAsyncWriter(&oms->event_log);
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
// qty is FPN<F> per the FPN-only-accounting rule in CLAUDE.md.
//
// Phase 03 chunk 3: extra context parameters for the fill handler.
//   intended_tp / intended_sl: TP/SL to apply at fill time (entry only).
//   strategy_id: STRATEGY_* constant for trade log CSV.
//   event_price: market price at submit time; used as the fill price in
//     paper mode (no adapter callback to supply one).
//======================================================================================================
template <unsigned F>
inline uint64_t OrderManager_Submit(OrderManagerState<F>* oms,
                                    int16_t core_id,
                                    OrderType type,
                                    FPN<F> qty,
                                    FPN<F> intended_tp = FPN_Zero<F>(),
                                    FPN<F> intended_sl = FPN_Zero<F>(),
                                    uint8_t strategy_id = 0xFF,
                                    FPN<F> event_price = FPN_Zero<F>(),
                                    uint8_t leg = 0) {  // P.3: 0 = leg A / single, 1 = leg B
    uint64_t id = oms->next_order_id++;

    // Paper mode + legacy (mode 0): count and return. Never touch the
    // table or the adapter. Mode 1 paper falls through to the slot
    // allocation path below so the fill handler runs in OMS_Tick.
    if (!BITMAP_IS_SET(oms->oms_state_flags, tt::MASK_OMS_STATE_LIVE_TRADING) && oms->event_log_mode == 0) {
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
    oms->orders[slot].leg           = leg;  // P.3: 0/1 for partial exits
    oms->orders[slot].state         = ORDER_SUBMITTED;
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
        cmd.result.avg_fill_price = FPN_ToDouble(event_price);
        cmd.result.fill_qty       = FPN_ToDouble(qty);
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
        oms->orders[slot].state = ORDER_REJECTED;
        oms->total_rejected.fetch_add(1, std::memory_order_relaxed);
        oms->order_bitmap &= (uint16_t)~(1u << slot);
        return 0;
    }

    // Async submit to the adapter. The callback fires later from the
    // worker thread once the REST round trip completes.
    double qty_d = FPN_ToDouble(qty);
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
        oms->orders[slot].state = ORDER_REJECTED;
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
inline bool OMS_PushSubmit(OrderManagerState<F>* oms,
                            int16_t core_id,
                            OrderType type,
                            FPN<F> qty,
                            FPN<F> intended_tp = FPN_Zero<F>(),
                            FPN<F> intended_sl = FPN_Zero<F>(),
                            uint8_t strategy_id = 0xFF,
                            FPN<F> event_price = FPN_Zero<F>(),
                            uint8_t leg = 0) {
    if (core_id < 0 || core_id >= MAX_EXECUTION_CORES) {
        std::fprintf(stderr,
                     "[OMS] PushSubmit: invalid core_id=%d (max=%d)\n",
                     (int)core_id, MAX_EXECUTION_CORES);
        return false;
    }
    SubmitCommand<F> cmd{};
    cmd.core_id      = core_id;
    cmd.order_type   = (uint8_t)type;
    cmd.strategy_id  = strategy_id;
    cmd.leg          = leg;
    cmd.qty          = qty;
    cmd.intended_tp  = intended_tp;
    cmd.intended_sl  = intended_sl;
    cmd.event_price  = event_price;
    bool pushed = SPSCRing_TryPush(&oms->submit_queues[core_id], cmd);
    if (!pushed) {
        std::fprintf(stderr,
                     "[OMS] PushSubmit: queue full for core=%d type=%u "
                     "(drainer starved?)\n",
                     (int)core_id, (unsigned)type);
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
            OrderManager_Submit(oms,
                                cmd.core_id,
                                (OrderType)cmd.order_type,
                                cmd.qty,
                                cmd.intended_tp,
                                cmd.intended_sl,
                                cmd.strategy_id,
                                cmd.event_price,
                                cmd.leg);
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
template <unsigned F>
inline void OrderManager_AccountMakerTakerFee(
    OrderManagerState<F>* oms, int is_maker, FPN<F> fee) {
    oms->total_fees = FPN_AddSat(oms->total_fees, fee);
    if (is_maker) {
        oms->maker_fills_count++;
        oms->total_maker_fees = FPN_AddSat(oms->total_maker_fees, fee);
    } else {
        oms->taker_fills_count++;
        oms->total_taker_fees = FPN_AddSat(oms->total_taker_fees, fee);
    }
}

//======================================================================================================
// [FILL HANDLER — single source of truth for portfolio mutation]
//======================================================================================================
// Extracted from OrderManager_Tick to eliminate duplication across REST fill,
// WS fill, and any future fill source. Handles:
//   - core_id bounds check
//   - event log append (OEVT_FULL_FILL)
//   - portfolio open (entry) / close + P&L (exit)
//   - fee computation (entry_fee + exit_fee)
//   - balance + realized_pnl update
//   - kill switch peak tracking
//   - trade log CSV
//
// This is the ONE place to update when the fee model or portfolio mutation
// logic changes. Portfolio_FromEventLog in OrderEventLog.hpp is the only
// other fee computation site (the replay fold), and it should mirror this.
//======================================================================================================
template <unsigned F>
inline void OrderManager_HandleFill(OrderManagerState<F>* oms, Order<F>* o,
                                     FPN<F> fill_price, FPN<F> fill_qty) {
    // Guard: core_id must be a valid portfolio slot index.
    if (o->core_id < 0 || o->core_id >= MAX_PORTFOLIO_POSITIONS) {
        std::fprintf(stderr,
                     "[OMS] fill handler: core_id %d out of range [0,%d), "
                     "skipping order %llu\n",
                     (int)o->core_id, MAX_PORTFOLIO_POSITIONS,
                     (unsigned long long)o->id);
        return;
    }

    // Append fill event to the audit log.
    OrderEventLog_Append(&oms->event_log,
        OrderEvent_MakeFill<F>(
            o->id, o->submitted_at_us,
            (OrderType)o->type, o->core_id,
            fill_price, fill_qty,
            o->intended_tp, o->intended_sl));

    if (o->type == (uint8_t)ORDER_MARKET_BUY) {
        // Entry fill: open portfolio slot.
        FPN<F> notional  = FPN_Mul(fill_price, fill_qty);
        // Phase 8: maker/taker fee on entry. o->is_maker comes from Binance
        // executionReport "m" field (parsed in c3, written by the OMS dispatch).
        // For legacy / backtest paths, fee_rate_maker == fee_rate_taker → same rate.
        FPN<F> entry_rate = o->is_maker ? oms->fee_rate_maker : oms->fee_rate_taker;
        FPN<F> entry_fee  = FPN_Mul(notional, entry_rate);
        // Phase 8 (post-coding c10) — accounting counters; v5.15.5.C.2 (S5)
        // extracted to OrderManager_AccountMakerTakerFee helper.
        OrderManager_AccountMakerTakerFee(oms, (int)o->is_maker, entry_fee);
        Portfolio_OpenSlot(&oms->portfolio, (int)o->core_id,
                           fill_price, fill_qty,
                           o->intended_tp, o->intended_sl, entry_fee);
        // Mode 1 per-core bookkeeping: stash entry data for the drainer to
        // apply to CoreContext (core_open_notional += notional, core_fees
        // += entry_fee). Slot is the portfolio slot — drainer maps to
        // core_id via Sharded_LegSlot under partials.
        oms->last_fill[(int)o->core_id].entry_notional = notional;
        oms->last_fill[(int)o->core_id].entry_fee      = entry_fee;
        oms->last_opened_mask |= (uint16_t)(1u << (int)o->core_id);
        if (oms->trade_log) {
            TradeEvent<F> synth{};
            synth.price     = fill_price;
            synth.timestamp = o->submitted_at_us;
            synth.core_id   = (uint16_t)o->core_id;
            synth.type      = TRADE_EVENT_ENTRY;
            ShardedTradeLog_RecordEntry(oms->trade_log, synth,
                                        o->strategy_id,
                                        fill_price, fill_qty,
                                        entry_fee, oms->balance);
        }
    } else if (o->type == (uint8_t)ORDER_MARKET_SELL) {
        // Exit fill: close portfolio slot, compute P&L, update balance.
        int pslot = (int)o->core_id;
        // v4.7.19: guard against double-close. Portfolio_CloseSlot doesn't
        // zero entry_price/quantity on close — it just clears the bitmap
        // bit. So if a SECOND SELL fill arrives for an already-closed slot
        // (e.g., manual-close racing with hot-path SG, or stale event in
        // ring), the read below picks up STALE entry_price + quantity,
        // computes a ghost gross/fee, and writes a phantom CSV row + bumps
        // total_filled. Result: trade history rows that never happened.
        // Skip the entire SELL branch when the slot bit is already clear.
        if ((oms->portfolio.active_bitmap & (uint16_t)(1u << pslot)) == 0) {
            std::fprintf(stderr,
                "[OMS] HandleFill: slot %d SELL on already-closed slot — "
                "no-op (likely race between manual close and hot-path SG)\n",
                pslot);
            return;
        }
        FPN<F> entry_price_snap = oms->portfolio.positions[pslot].entry_price;
        FPN<F> entry_fee = oms->portfolio.positions[pslot].entry_fee;
        FPN<F> qty_snap  = oms->portfolio.positions[pslot].quantity;
        // v5.1.6 (diagnostic logging): capture position TP/SL BEFORE close
        // so we can infer which gate fired this exit. Logged below after
        // gross/fee/net are computed.
        FPN<F> tp_snap = oms->portfolio.positions[pslot].take_profit_price;
        FPN<F> sl_snap = oms->portfolio.positions[pslot].stop_loss_price;
        // Phase 6prep sharded c14: compute realized return as double for the
        // ConfidenceScorer feedback channel BEFORE CloseSlot wipes entry_price.
        // The scorer uses (prediction, return) pairs to estimate IC. Skip the
        // signal if entry_price is degenerate (paper-mode dust, etc.).
        double entry_price_d = FPN_ToDouble(entry_price_snap);
        double exit_price_d  = FPN_ToDouble(fill_price);
        if (entry_price_d > 0.0 &&
            pslot >= 0 && pslot < MAX_PORTFOLIO_POSITIONS) {
            oms->last_realized_return[pslot] =
                (exit_price_d - entry_price_d) / entry_price_d;
            oms->last_closed_mask |= (uint16_t)(1u << pslot);
        }
        FPN<F> gross     = Portfolio_CloseSlot(&oms->portfolio, pslot, fill_price);
        FPN<F> exit_notional = FPN_Mul(fill_price, qty_snap);
        // Phase 8: maker/taker fee on exit. ORDER_MARKET_SELL is taker by
        // exchange definition, but read o->is_maker for forward-compat with
        // hybrid execution (Phase 9 POST_ONLY limit sells = potential maker).
        FPN<F> exit_rate = o->is_maker ? oms->fee_rate_maker : oms->fee_rate_taker;
        FPN<F> exit_fee  = FPN_Mul(exit_notional, exit_rate);
        // Phase 8 (post-coding c10) — accounting counters on exit; v5.15.5.C.2
        // (S5) extracted to OrderManager_AccountMakerTakerFee helper.
        OrderManager_AccountMakerTakerFee(oms, (int)o->is_maker, exit_fee);
        FPN<F> total_fee     = FPN_Add(entry_fee, exit_fee);
        FPN<F> net           = FPN_Sub(gross, total_fee);
        oms->balance      = FPN_Add(oms->balance, net);
        oms->realized_pnl = FPN_Add(oms->realized_pnl, net);
        if (FPN_GreaterThan(oms->balance, oms->ks_peak_balance)) {
            oms->ks_peak_balance = oms->balance;
        }
        // Mode 1 per-core bookkeeping: stash exit data for the drainer to
        // apply to CoreContext (core_realized += net, core_open_notional
        // -= entry_notional_snap, core_fees += total_fee, core_wins/losses).
        // entry_notional uses the SAME entry_price × qty snapshot the
        // mode-0 path subtracts — symmetric round-trip leaves no residue.
        oms->last_fill[pslot].exit_net_pnl        = net;
        oms->last_fill[pslot].exit_entry_notional = FPN_Mul(entry_price_snap, qty_snap);
        oms->last_fill[pslot].exit_total_fees     = total_fee;
        oms->last_fill[pslot].was_win             = FPN_GreaterThan(net, FPN_Zero<F>()) ? 1 : 0;
        // v5.13.0.B — calibration log row. Captures EVERY exit fill (not
        // just predicted ones) so operator can compute calibration metrics
        // (Brier, ROC AUC) offline. nullptr-safe: most runs leave the
        // FILE* null. After write, clears per-slot exit-prediction flags
        // (single-use per trade). Using std::chrono here to avoid coupling
        // HandleFill to OS-specific clocks; same precision as the rest of
        // the engine's wall-clock fields.
        if (oms->calibration_log_file) {
            uint64_t ts_us = (uint64_t)
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
            double entry_d_calib = FPN_ToDouble(entry_price_snap);
            double exit_d_calib  = FPN_ToDouble(fill_price);
            double gain_pct = entry_d_calib > 0.0
                ? (exit_d_calib - entry_d_calib) / entry_d_calib * 100.0
                : 0.0;
            double notional_d = entry_d_calib > 0.0
                ? entry_d_calib * FPN_ToDouble(qty_snap) : 0.0;
            double pnl_bps = notional_d > 0.0
                ? FPN_ToDouble(net) / notional_d * 10000.0 : 0.0;
            // v5.15.5.C.2 (S3b) — bit-packed in last_exit_predicted_bitmap.
            uint8_t pred_flag = (pslot >= 0 && pslot < MAX_PORTFOLIO_POSITIONS)
                ? (uint8_t)BITMAP_IS_SET(oms->last_exit_predicted_bitmap, BITMAP_BIT_U16(pslot)) : 0;
            double pred_p = (pslot >= 0 && pslot < MAX_PORTFOLIO_POSITIONS)
                ? oms->last_exit_predicted_p[pslot] : 0.0;
            // v5.14.10.D — registry-driven row emit via FOREACH_CALIB_LOG_COL
            // (DataStream/CalibLogColRegistry.hpp). Byte-identical output to
            // the prior hand-coded fprintf; closes TECH_DEBT-010 structurally
            // (future column additions = 1 row in the registry, not 3-site touch).
            CALIB_LOG_EMIT_ROW(oms->calibration_log_file);
            // Don't fflush every row — let stdio buffer (drainer is the
            // single writer; flush happens on file close at shutdown).
            // Operator can `tail -f` if needed (line-buffered when stdout
            // is a TTY; full-buffered for files — that's a tail -F caveat,
            // not a correctness concern for offline calibration analysis).
        }
        // v5.13.4 — per-slot exit-prediction state DELIBERATELY NOT
        // cleared here. Calibration log row above already captured what
        // it needs. Clear is moved to EventLoop_DrainPostFillOneCore
        // (post-bandit-attribution) so the bandit Update has stable
        // arm + regime + flag values to read. Clear there is also where
        // the buy-side ConfidenceScorer + ensemble bandit attribution
        // run, keeping per-leg exit-side accounting symmetric.
        // v5.1.6 (diagnostic logging): infer exit reason from the relationship
        // between fill_price and the position's TP/SL at close-time.
        //   - TP_HIT:  exit ≥ pos.take_profit_price (hot-path SG TP fired)
        //   - SL_HIT:  exit ≤ pos.stop_loss_price (hot-path SG SL fired —
        //              note: this includes ratchet_sl effect which RAISES sl
        //              via FPN_Max in ExecutionCore::SG_Evaluate)
        //   - INSIDE:  exit between TP and SL — the exit was forced by
        //              something OTHER than the hot-path SG (TimeExit force-
        //              close, manual close from GUI drag, kill switch, etc.)
        // The "INSIDE" case is what we care about most — the +0.097% bleed
        // pattern we saw in v5.1.2 soak (exits inside the TP/SL band that
        // shouldn't have fired the hot path). After v5.1.6 lands, grep
        // engine.log for "[exit-diag]" with reason=INSIDE to find every
        // mystery close.
        {
            double entry_d = FPN_ToDouble(entry_price_snap);
            double exit_d  = FPN_ToDouble(fill_price);
            double tp_d    = FPN_ToDouble(tp_snap);
            double sl_d    = FPN_ToDouble(sl_snap);
            double gain    = entry_d > 0.0 ? (exit_d - entry_d) / entry_d : 0.0;
            double net_d   = FPN_ToDouble(net);
            double fee_d   = FPN_ToDouble(total_fee);
            const char* reason = "INSIDE";
            if (tp_d > 0.0 && exit_d >= tp_d - 1e-6) reason = "TP_HIT";
            else if (sl_d > 0.0 && exit_d <= sl_d + 1e-6) reason = "SL_HIT";
            std::fprintf(stderr,
                "[exit-diag] slot=%d strat=%u reason=%s entry=%.2f exit=%.2f "
                "tp=%.2f sl=%.2f gain=%+.4f%% gross=%+.4f fees=%.4f net=%+.4f\n",
                pslot, (unsigned)o->strategy_id, reason,
                entry_d, exit_d, tp_d, sl_d, gain * 100.0,
                FPN_ToDouble(gross), fee_d, net_d);
        }
        // last_closed_mask bit was set above (line 590) — same producer.
        if (oms->trade_log) {
            TradeEvent<F> synth{};
            synth.price     = fill_price;
            synth.timestamp = o->submitted_at_us;
            synth.core_id   = (uint16_t)o->core_id;
            synth.type      = TRADE_EVENT_EXIT;
            ShardedTradeLog_RecordExit(oms->trade_log, synth,
                                       o->strategy_id,
                                       entry_price_snap, fill_price,
                                       qty_snap, net, total_fee,
                                       oms->balance);
        }
    }
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
    if (o->state == ORDER_FILLED) return 0;

    if (cmd.result.success) {
        std::strncpy(o->exchange_id, cmd.result.exchange_id,
                     sizeof(o->exchange_id) - 1);
        o->exchange_id[sizeof(o->exchange_id) - 1] = '\0';

        // ACK-only results (from REST when WS is active): fill_qty == 0.
        // Transition to ACKNOWLEDGED, keep the slot open for the WS fill.
        if (cmd.result.fill_qty == 0.0 && cmd.result.avg_fill_price == 0.0) {
            o->state = ORDER_ACKNOWLEDGED;
            return 1;  // slot stays open — don't free
        }

        o->avg_fill_price = FPN_FromDouble<F>(cmd.result.avg_fill_price);
        o->filled_qty     = FPN_FromDouble<F>(cmd.result.fill_qty);
        // Phase 8: maker/taker flag from Binance executionReport, parsed in c3.
        // Fee_Compute reads this for entry-fee math when the controller books
        // the fill. is_maker stays at Order_Init's 0 (taker) for synchronous
        // REST fills (Phase 02 path) — Binance market orders are taker by def.
        o->is_maker = cmd.result.is_maker;
        // Phase 8: pick FILLED vs PARTIAL based on Binance "X" field (parsed
        // in c3 as order_complete). For ACK-only paths above we already
        // returned with ORDER_ACKNOWLEDGED, so reaching here means an actual
        // fill (full or partial) happened. order_complete=0 = PARTIALLY_FILLED.
        // Defensive: if order_complete is missing from the event (parser sets
        // 0 in that case), we err toward PARTIAL — keeps the order alive in
        // the OMS, won't lose track. Subsequent fill events resolve to FILLED.
        o->state = cmd.result.order_complete ? ORDER_FILLED : ORDER_PARTIAL;
        if (o->state == ORDER_FILLED) {
            oms->total_filled.fetch_add(1, std::memory_order_relaxed);
        }

        // Mode 1 fill handler: portfolio mutation + event log.
        if (oms->event_log_mode == 1) {
            FPN<F> fill_price = o->avg_fill_price;
            FPN<F> fill_qty   = o->filled_qty;
            OrderManager_HandleFill(oms, o, fill_price, fill_qty);
        }
    } else {
        std::fprintf(stderr,
                     "[OMS] order %llu FAIL core=%d code=%d msg=%s\n",
                     (unsigned long long)o->id,
                     (int)o->core_id,
                     cmd.result.error_code,
                     cmd.result.error_message);
        o->state = ORDER_REJECTED;
        oms->total_rejected.fetch_add(1, std::memory_order_relaxed);

        // Mode 1: append rejection to event log for the audit trail.
        if (oms->event_log_mode == 1) {
            OrderEventLog_Append(&oms->event_log,
                OrderEvent_MakeRejection<F>(
                    o->id, o->submitted_at_us,
                    (OrderType)o->type, o->core_id,
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

    oms->balance = FPN_FromDouble<F>(exchange_balance);
    if (FPN_GreaterThan(oms->balance, oms->ks_peak_balance)) {
        oms->ks_peak_balance = oms->balance;
    }

    if (oms->event_log_mode == 1) {
        OrderEvent<F> recon_event;
        std::memset(&recon_event, 0, sizeof(recon_event));
        recon_event.type       = OEVT_RECONCILED;
        recon_event.order_type = ORDER_MARKET_BUY;  // placeholder
        recon_event.core_id    = -1;
        recon_event.price      = FPN_FromDouble<F>(drift);
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
