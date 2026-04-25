// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [ORDER]
//
// First-class order entity for the OMS (Order Management System).
//
// Why this exists:
//   The pre-OMS sharded engine treated orders as side effects of trade
//   events. The drainer optimistically updated the portfolio, then maybe
//   submitted to Binance. When the order failed or partially filled or
//   filled at a different price, local state was wrong with no clean
//   recovery. This struct lifts orders to first-class status — every
//   submission gets its own Order with a state machine, an idempotency
//   key, fill tracking, and an audit trail (event log lands in phase 03).
//
// State machine:
//   PENDING --> SUBMITTED --> ACKNOWLEDGED --> PARTIAL --> FILLED   (terminal)
//      |                                          |
//      |                                          +--> FILLED       (terminal)
//      |
//      +--> REJECTED        (terminal)
//      +--> CANCELED        (terminal)
//      +--> TIMEOUT         (terminal)
//      +--> UNKNOWN         (lost tracking, needs reconciliation)
//
// Phase 01 only uses PENDING and FILLED/REJECTED — the synchronous
// BinanceOrderAPI path either succeeds and goes straight to FILLED, or
// fails and goes to REJECTED. Phase 02+ adds the intermediate states
// once the adapter callbacks become async.
//
// Naming clarification:
//   This Order is NOT the same as MemHeaders/PoolAllocator.hpp:OrderPool.
//   That OrderPool is the buy gate's intent pool (slots that the BG fires
//   into when it wants to enter a position). This Order is the live
//   exchange order lifecycle. They don't overlap conceptually or in
//   field names — different abstractions, the prefix collision is
//   unfortunate but harmless.
//======================================================================================================

#pragma once

#include "../FixedPoint/FixedPointN.hpp"
#include <cstdint>

namespace tt {

enum OrderType : uint8_t {
    ORDER_MARKET_BUY  = 0,
    ORDER_MARKET_SELL = 1,
    ORDER_LIMIT_BUY   = 2,    // future, phase 08
    ORDER_LIMIT_SELL  = 3,    // future, phase 08
};

enum OrderState : uint8_t {
    ORDER_PENDING        = 0,  // submitted to OMS, not yet on exchange
    ORDER_SUBMITTED      = 1,  // adapter.submit returned, awaiting ack
    ORDER_ACKNOWLEDGED   = 2,  // exchange has it, working
    ORDER_PARTIAL        = 3,  // partially filled, still working
    ORDER_FILLED         = 4,  // fully filled (terminal)
    ORDER_REJECTED       = 5,  // exchange rejected (terminal)
    ORDER_CANCELED       = 6,  // we canceled or exchange canceled (terminal)
    ORDER_TIMEOUT        = 7,  // never got an ack within deadline (terminal)
    ORDER_UNKNOWN        = 8,  // lost tracking, needs reconciliation
};

template <unsigned F>
struct Order {
    uint64_t  id;                  // local monotonic id, assigned by OMS
    uint64_t  client_id;           // idempotency key for retries (== id for first attempt)
    char      exchange_id[64];     // assigned by exchange on ack, "" until then
    int16_t   core_id;             // which executor core, -1 for non-core orders
    uint8_t   type;                // OrderType
    uint8_t   state;               // OrderState
    // implicit padding here for FPN alignment
    FPN<F>    requested_qty;
    FPN<F>    requested_price;     // limit only, ignored for MARKET
    FPN<F>    filled_qty;          // running total across partials
    FPN<F>    avg_fill_price;      // weighted across partials
    // phase 03 chunk 3: context fields for the OMS fill handler. when
    // event_log_mode == 1, the OMS opens portfolio slots on fill and
    // needs the TP/SL/strategy the controller intended at entry time.
    // also carries event_price for paper mode fills (no adapter callback
    // to supply a fill price, so we use the market price at submit time).
    FPN<F>    intended_tp;         // TP to apply when this order fills (entry only)
    FPN<F>    intended_sl;         // SL to apply when this order fills (entry only)
    FPN<F>    event_price;         // market price at submit time (paper fill price)
    uint64_t  submitted_at_us;     // wall-clock microseconds since epoch
    uint64_t  last_update_us;      // last state transition timestamp
    uint8_t   retry_count;         // bumped on each retry attempt
    uint8_t   strategy_id;         // STRATEGY_* constant, for trade log CSV
    // Phase 8 — fill type for maker/taker fee accounting. Set from Binance
    // executionReport "m" field by ud_parse_execution_report (c3). Valid
    // only when state == ORDER_FILLED or ORDER_PARTIAL; 0 (taker) until
    // first fill event arrives. Backtest path keeps is_maker=0 always.
    uint8_t   is_maker;
    uint8_t   _pad[5];             // adjusted from 6 to keep struct size stable
};

// Initialize an order to PENDING state with the given identifying fields.
// FPN amounts are zeroed; the caller fills in requested_qty (and optionally
// requested_price for limit orders) BEFORE calling OrderManager_Submit.
//
// id and client_id are set equal — phase 01 uses the local monotonic id as
// the idempotency key. Phase 06 (production hardening) may decouple them
// if retry semantics require a stable client_id across retries.
template <unsigned F>
inline void Order_Init(Order<F>* o, uint64_t id, int16_t core_id, OrderType type) {
    o->id              = id;
    o->client_id       = id;
    o->exchange_id[0]  = '\0';
    o->core_id         = core_id;
    o->type            = (uint8_t)type;
    o->state           = ORDER_PENDING;
    o->requested_qty   = FPN_Zero<F>();
    o->requested_price = FPN_Zero<F>();
    o->filled_qty      = FPN_Zero<F>();
    o->avg_fill_price  = FPN_Zero<F>();
    o->intended_tp     = FPN_Zero<F>();
    o->intended_sl     = FPN_Zero<F>();
    o->event_price     = FPN_Zero<F>();
    o->submitted_at_us = 0;
    o->last_update_us  = 0;
    o->retry_count     = 0;
    o->strategy_id     = 0xFF;  // STRATEGY_NONE
    o->is_maker        = 0;     // assume taker until executionReport says otherwise
}

// Phase 8 anti-drift guard: pin Order<F> size to catch silent ABI breakage
// from future field additions. If you add a field and this fails, decide
// CONSCIOUSLY whether the change is acceptable (and update the constant)
// or pack into existing _pad. OrderPool slots are sized for this struct;
// growing it changes the pool's memory footprint.
//
// Per-instantiation: F=64 is the live-engine + suite default. Other widths
// would have different sizes and don't get a static_assert (yet).
static_assert(sizeof(Order<64>) == 280,
              "Order<64> size changed — verify OrderPool slot assumptions, "
              "then update this assertion to the new size.");

// Predicate: is this order in a terminal state (no further transitions)?
// Used by OrderManager_Tick to decide whether to free the slot. Phase 03
// (event log) keeps terminal orders around longer for audit; phase 01
// frees them immediately on terminal transition.
template <unsigned F>
inline bool Order_IsTerminal(const Order<F>* o) {
    uint8_t s = o->state;
    return s == ORDER_FILLED
        || s == ORDER_REJECTED
        || s == ORDER_CANCELED
        || s == ORDER_TIMEOUT;
}

}  // namespace tt
