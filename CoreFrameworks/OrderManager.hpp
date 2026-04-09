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
    CMD_FILL_RESULT = 0,
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

    // === BANK STATE (moved from EventLoopState in phase 03 chunk 1) ===
    // Canonical portfolio + balance. After phase 03 mode 1 ships, these
    // are derived from the order event log. In mode 0 (legacy) and during
    // chunk 1 itself, EventLoop_OnEvent still mutates them directly.
    Portfolio<F> portfolio;
    FPN<F>       balance;
    FPN<F>       realized_pnl;
    FPN<F>       fee_rate;

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

    // Phase 03 chunk 1B: bank state lives here now.
    Portfolio_Init(&oms->portfolio);
    oms->balance             = starting_balance;
    oms->realized_pnl        = FPN_Zero<F>();
    oms->fee_rate            = fee_rate;
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
    OrderEventLog_Init(&oms->event_log);
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
                                    FPN<F> event_price = FPN_Zero<F>()) {
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
    oms->orders[slot].requested_qty = qty;
    oms->orders[slot].intended_tp   = intended_tp;
    oms->orders[slot].intended_sl   = intended_sl;
    oms->orders[slot].strategy_id   = strategy_id;
    oms->orders[slot].event_price   = event_price;
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
// [TICK — drain the result queue]
//======================================================================================================
// Drainer thread calls this on every drain pass. Pops every CMD_FILL_RESULT
// from the result queue and applies it to the matching order. The matching
// uses linear search across MAX_INFLIGHT_ORDERS slots, which is fine for
// 16 slots — O(16) per result is trivial.
//
// On a successful fill: mark FILLED, copy exchange_id and fill price/qty
// out of the result, free the slot.
// On a rejection: log, mark REJECTED, free the slot. The "TODO reset path"
// problem (executor still thinks it's active after a failure) is documented
// at EngineSharded.hpp's drainer comment and is properly fixed in phase 03
// (event log) which routes events through the OMS instead of optimistically
// updating portfolio in OnEvent.
//======================================================================================================
template <unsigned F>
inline void OrderManager_Tick(OrderManagerState<F>* oms) {
    Command cmd;
    while (SPSCRing_TryPop(&oms->result_queue, &cmd)) {
        if (cmd.type != (uint8_t)CMD_FILL_RESULT) continue;

        // Linear search for the matching order. 16 slots max.
        int slot = -1;
        for (int i = 0; i < MAX_INFLIGHT_ORDERS; ++i) {
            if ((oms->order_bitmap & (uint16_t)(1u << i)) == 0) continue;
            if (oms->orders[i].id == cmd.order_id) { slot = i; break; }
        }
        if (slot < 0) {
            std::fprintf(stderr,
                         "[OMS] result for unknown order %llu (slot freed already?), ignoring\n",
                         (unsigned long long)cmd.order_id);
            continue;
        }

        Order<F>* o = &oms->orders[slot];
        if (cmd.result.success) {
            std::strncpy(o->exchange_id, cmd.result.exchange_id,
                         sizeof(o->exchange_id) - 1);
            o->exchange_id[sizeof(o->exchange_id) - 1] = '\0';
            o->avg_fill_price = FPN_FromDouble<F>(cmd.result.avg_fill_price);
            o->filled_qty     = FPN_FromDouble<F>(cmd.result.fill_qty);
            o->state          = ORDER_FILLED;
            oms->total_filled.fetch_add(1, std::memory_order_relaxed);
        } else {
            std::fprintf(stderr,
                         "[OMS] order %llu FAIL core=%d code=%d msg=%s — TODO reset path\n",
                         (unsigned long long)o->id,
                         (int)o->core_id,
                         cmd.result.error_code,
                         cmd.result.error_message);
            o->state = ORDER_REJECTED;
            oms->total_rejected.fetch_add(1, std::memory_order_relaxed);
        }

        // === EVENT LOG MODE 1: fill handler ===
        // In mode 1 the OMS owns portfolio mutation. On a successful fill
        // we open/close the portfolio slot and update balance. On rejection
        // we append a rejection event to the log for the audit trail.
        if (oms->event_log_mode == 1) {
            if (cmd.result.success) {
                // Append fill event to the audit log.
                FPN<F> fill_price = FPN_FromDouble<F>(cmd.result.avg_fill_price);
                FPN<F> fill_qty   = FPN_FromDouble<F>(cmd.result.fill_qty);
                OrderEventLog_Append(&oms->event_log,
                    OrderEvent_MakeFill<F>(
                        o->id, o->submitted_at_us,
                        (OrderType)o->type, o->core_id,
                        fill_price, fill_qty,
                        o->intended_tp, o->intended_sl));

                if (o->type == (uint8_t)ORDER_MARKET_BUY) {
                    // Entry fill: open portfolio slot.
                    FPN<F> notional  = FPN_Mul(fill_price, fill_qty);
                    FPN<F> entry_fee = FPN_Mul(notional, oms->fee_rate);
                    Portfolio_OpenSlot(&oms->portfolio, (int)o->core_id,
                                      fill_price, fill_qty,
                                      o->intended_tp, o->intended_sl, entry_fee);
                    // Trade log CSV — construct a synthetic TradeEvent so
                    // the existing RecordEntry function works unchanged.
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
                    FPN<F> entry_price_snap = oms->portfolio.positions[pslot].entry_price;
                    FPN<F> entry_fee = oms->portfolio.positions[pslot].entry_fee;
                    FPN<F> qty_snap  = oms->portfolio.positions[pslot].quantity;
                    FPN<F> gross     = Portfolio_CloseSlot(&oms->portfolio, pslot, fill_price);
                    FPN<F> exit_notional = FPN_Mul(fill_price, qty_snap);
                    FPN<F> exit_fee      = FPN_Mul(exit_notional, oms->fee_rate);
                    FPN<F> total_fee     = FPN_Add(entry_fee, exit_fee);
                    FPN<F> net           = FPN_Sub(gross, total_fee);
                    oms->balance      = FPN_Add(oms->balance, net);
                    oms->realized_pnl = FPN_Add(oms->realized_pnl, net);
                    if (FPN_GreaterThan(oms->balance, oms->ks_peak_balance)) {
                        oms->ks_peak_balance = oms->balance;
                    }
                    // Trade log CSV.
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
            } else {
                // Rejection: append to event log for the audit trail.
                OrderEventLog_Append(&oms->event_log,
                    OrderEvent_MakeRejection<F>(
                        o->id, o->submitted_at_us,
                        (OrderType)o->type, o->core_id,
                        cmd.result.error_message));
            }
        }

        // Free the slot on terminal transition.
        oms->order_bitmap &= (uint16_t)~(1u << slot);
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
