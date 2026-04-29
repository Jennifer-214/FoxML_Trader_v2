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
template <unsigned F>
struct OrderManagerState {
    Order<F> orders[MAX_INFLIGHT_ORDERS];
    uint16_t order_bitmap;       // 1 = slot in use, 0 = free. uint16_t caps at 16 slots.
    uint16_t _pad0;
    uint32_t _pad1;
    uint64_t next_order_id;      // monotonic id counter; 0 reserved for "no id"

    // Exchange adapter (by value). In paper mode all function pointers
    // are null and the OMS short-circuits before touching them. In live
    // mode the OMS calls submit_market_buy / submit_market_sell from
    // OrderManager_Submit and the adapter callback fires later from a
    // worker thread.
    ExchangeAdapter<F> adapter;

    int live_trading;            // 0 = paper, 1 = live (adapter required)

    // Result queue: adapter worker thread (single producer with
    // worker_count==1) pushes CMD_FILL_RESULT here when an order
    // completes. Drainer thread (single consumer) drains it from
    // OrderManager_Tick. SPSC contract relies on worker_count==1.
    SPSCRing<Command, OMS_RESULT_QUEUE_SIZE> result_queue;

    // WS fill queue (phase 04): user data websocket thread is the sole
    // producer, drainer is the sole consumer. Separate ring preserves
    // the SPSC contract — no MPSC needed. OrderManager_Tick drains
    // this after the REST result_queue.
    SPSCRing<Command, OMS_RESULT_QUEUE_SIZE> ws_result_queue;

    // Reconcile queue (phase 05): reconciler thread is the sole producer,
    // drainer is the sole consumer. Carries CMD_RECONCILE commands with
    // drift amounts. OrderManager_Tick drains this third.
    SPSCRing<Command, 64> reconcile_queue;

    // v4.7.37: per-core submit queues. Producer threads (today: producer
    // slow-path; future: per-core slow-path threads in engine_arch=
    // per_core_slow) push SubmitCommands here. The drainer thread pops
    // them in OMS_DrainSubmit and calls OrderManager_Submit serially —
    // preserving the documented "drainer is sole Submit caller" contract.
    //
    // Why per-core (not one queue): when per-core slow-paths spawn (Phase C),
    // each thread is the sole producer for its own ring. SPSC contract
    // holds. With one shared queue, multiple producers would need MPSC.
    SPSCRing<SubmitCommand<F>, OMS_SUBMIT_QUEUE_SIZE> submit_queues[MAX_EXECUTION_CORES];

    // === BANK STATE (moved from EventLoopState in phase 03 chunk 1) ===
    // Canonical portfolio + balance. After phase 03 mode 1 ships, these
    // are derived from the order event log. In mode 0 (legacy) and during
    // chunk 1 itself, EventLoop_OnEvent still mutates them directly.
    Portfolio<F> portfolio;
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
    uint16_t   last_opened_mask;
    uint8_t    _pad_lof[6];

    // === PARTIALS GEOMETRY (mirrored from cfg at engine init) ===
    // partial_exit_enabled = 1 → portfolio slot 2c is core c's leg A,
    // slot 2c+1 is core c's leg B (mapping via Sharded_LegSlot in
    // ControllerEventLoop.hpp). The drainer needs this to map slot→core
    // when applying FillRecords to per-core stats. Set at sharded init,
    // not changed at runtime (toggle requires snapshot v3 reload anyway).
    uint8_t partial_exit_enabled;
    uint8_t _pad_pe[7];

    // === KILL SWITCH STATE (moved from EventLoopState in phase 03 chunk 1) ===
    // Configured by EventLoopState_ConfigureKillSwitch (which now writes
    // here through the OMS pointer). Disabled by default (both thresholds
    // zero). Tripping clears every registered core's permission with
    // RELEASE; resume via EventLoop_Unpause.
    FPN<F>  ks_min_balance;       // trip if balance < this
    FPN<F>  ks_max_drawdown_pct;  // trip if (peak - balance) / peak > this (0 = disabled)
    FPN<F>  ks_peak_balance;      // running max of balance, updated on exits
    uint8_t kill_switch_tripped;  // 1 once tripped (idempotent)
    uint8_t _pad_ks[7];
    uint64_t ks_trips_total;      // count of trip events (observability)

    // === TRADE LOG (moved from EventLoopState in phase 03 chunk 1) ===
    // Optional CSV trade log. nullptr → no logging (default). Not owned —
    // the engine main owns the ShardedTradeLog object and passes a pointer
    // here via EventLoopState_AttachTradeLog (which now writes through to
    // the OMS).
    ShardedTradeLog* trade_log;

    // === EVENT LOG MODE (phase 03 chunk 3) ===
    // 0 = legacy: OMS_Tick only marks FILLED/REJECTED and frees slots.
    //     OnEvent in ControllerEventLoop does the portfolio mutation.
    // 1 = event log: OMS_Tick runs the fill handler which opens/closes
    //     portfolio slots, updates balance, appends to the event log.
    //     OnEvent just bumps counters.
    int event_log_mode;

    // === ORDER EVENT LOG (phase 03 chunk 3) ===
    // append-only log of order lifecycle events. populated in mode 1 by
    // the fill handler inside OMS_Tick. the portfolio can be reconstructed
    // from this log at any time via Portfolio_FromEventLog.
    OrderEventLog<F> event_log;

    // Observability counters. Atomic so the TUI render loop on a different
    // core can read them without locks. Relaxed ordering throughout —
    // these are display-only.
    std::atomic<uint64_t> total_submitted;
    std::atomic<uint64_t> total_filled;
    std::atomic<uint64_t> total_rejected;
};

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
template <unsigned F>
inline void OrderManager_Init(OrderManagerState<F>* oms,
                              const ExchangeAdapter<F>& adapter,
                              int live_trading,
                              FPN<F> starting_balance,
                              FPN<F> fee_rate,
                              int event_log_mode = 0) {
    for (int i = 0; i < MAX_INFLIGHT_ORDERS; ++i) {
        Order_Init(&oms->orders[i], 0, -1, ORDER_MARKET_BUY);
        oms->orders[i].state = ORDER_FILLED;  // mark as inactive (terminal)
    }
    oms->order_bitmap   = 0;
    oms->next_order_id  = 1;
    oms->adapter        = adapter;
    oms->live_trading   = live_trading;
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
    oms->partial_exit_enabled = 0;  // engine sets per-cfg after Init
    for (int i = 0; i < MAX_PORTFOLIO_POSITIONS; ++i) {
        oms->last_fill[i].entry_notional      = FPN_Zero<F>();
        oms->last_fill[i].entry_fee           = FPN_Zero<F>();
        oms->last_fill[i].exit_net_pnl        = FPN_Zero<F>();
        oms->last_fill[i].exit_entry_notional = FPN_Zero<F>();
        oms->last_fill[i].exit_total_fees     = FPN_Zero<F>();
        oms->last_fill[i].was_win             = 0;
    }
    oms->ks_min_balance      = FPN_Zero<F>();
    oms->ks_max_drawdown_pct = FPN_Zero<F>();
    oms->ks_peak_balance     = starting_balance;  // initial peak = start
    oms->kill_switch_tripped = 0;
    oms->ks_trips_total      = 0;
    oms->trade_log           = nullptr;

    oms->total_submitted.store(0, std::memory_order_relaxed);
    oms->total_filled.store(0, std::memory_order_relaxed);
    oms->total_rejected.store(0, std::memory_order_relaxed);

    // Phase 03 chunk 3: event log mode + log allocation.
    oms->event_log_mode = event_log_mode;
    // Phase 07: disk persistence. In mode 1, load previous events from
    // disk (reconstructs next_event_id), then open the file for append
    // so new events write through. On first run the load returns 0 (no
    // file) and InitWithFile creates a fresh one with a header.
    if (event_log_mode == 1) {
        OrderEventLog_Init(&oms->event_log);  // allocate buffer first
        int loaded = OrderEventLog_LoadFromDisk(&oms->event_log, "logging/order_events.bin");
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
        OrderEventLog_InitWithFile(&oms->event_log, "logging/order_events.bin");
    } else {
        OrderEventLog_Init(&oms->event_log);
    }
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
    if (!oms->live_trading && oms->event_log_mode == 0) {
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

    Order_Init(&oms->orders[slot], id, core_id, type);
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
    if (!oms->live_trading) {
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
        // Phase 8 (post-coding c10) — accounting counters
        oms->total_fees = FPN_AddSat(oms->total_fees, entry_fee);
        if (o->is_maker) {
            oms->maker_fills_count++;
            oms->total_maker_fees = FPN_AddSat(oms->total_maker_fees, entry_fee);
        } else {
            oms->taker_fills_count++;
            oms->total_taker_fees = FPN_AddSat(oms->total_taker_fees, entry_fee);
        }
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
        // Phase 8 (post-coding c10) — accounting counters on exit
        oms->total_fees = FPN_AddSat(oms->total_fees, exit_fee);
        if (o->is_maker) {
            oms->maker_fills_count++;
            oms->total_maker_fees = FPN_AddSat(oms->total_maker_fees, exit_fee);
        } else {
            oms->taker_fills_count++;
            oms->total_taker_fees = FPN_AddSat(oms->total_taker_fees, exit_fee);
        }
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
    // Find the matching order by id.
    int slot = -1;
    for (int i = 0; i < MAX_INFLIGHT_ORDERS; ++i) {
        if ((oms->order_bitmap & (uint16_t)(1u << i)) == 0) continue;
        if (oms->orders[i].id == cmd.order_id) { slot = i; break; }
    }

    // WS surprise fill (order_id == 0): log and skip.
    if (cmd.order_id == 0 && cmd.type == (uint8_t)CMD_WS_FILL) {
        std::fprintf(stderr,
                     "[OMS] WS surprise fill (no clientOrderId), ignoring — "
                     "reconciliation will catch it\n");
        return 0;
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
