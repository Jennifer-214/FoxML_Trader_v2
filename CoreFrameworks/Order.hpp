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

// Forward decl for Order_BindPreResolved (defined in CoreFrameworks/ControllerConfig.hpp).
// Callers of Order_BindPreResolved MUST include ControllerConfig.hpp; the forward decl
// allows Order.hpp to remain include-light. PerCoreCfg is in the global namespace.
template <unsigned F> struct PerCoreCfg;

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

//======================================================================================================
// [BIT-PACKED FLAGS] (v5.15.5.F.4c.3 WIP2d-1.B.1)
//======================================================================================================
// type[2] + state[4] + is_maker[1] + leg[1] + retry_count[8] packed into uint16_t flags_packed.
// Access via Order_GetType / Order_SetType / etc. accessor inline fns; never via direct
// flags_packed bit-twiddling at consumer sites. Per multi-bit-state-encoding-pattern.md
// (multi-field extension — flags packed are independent fields sharing a register-sized word).
//======================================================================================================
static constexpr uint16_t MASK_ORDER_TYPE         = 0x0003;  // bits 0-1
static constexpr uint16_t SHIFT_ORDER_TYPE        = 0;
static constexpr uint16_t MASK_ORDER_STATE        = 0x003C;  // bits 2-5
static constexpr uint16_t SHIFT_ORDER_STATE       = 2;
static constexpr uint16_t MASK_ORDER_IS_MAKER     = 0x0040;  // bit 6
static constexpr uint16_t SHIFT_ORDER_IS_MAKER    = 6;
static constexpr uint16_t MASK_ORDER_LEG          = 0x0080;  // bit 7
static constexpr uint16_t SHIFT_ORDER_LEG         = 7;
static constexpr uint16_t MASK_ORDER_RETRY_COUNT  = 0xFF00;  // bits 8-15
static constexpr uint16_t SHIFT_ORDER_RETRY_COUNT = 8;

//======================================================================================================
// [PRE-RESOLVED SUB-STRUCT] (v5.15.5.F.4c.3 WIP2d-1.B.1)
//======================================================================================================
// Decision-time-bound values pre-resolved at Order submit time via Order_BindPreResolved().
// HandleFill (drainer thread) reads o->pre_resolved.fee_rate directly — zero OMS cache
// lookup. Per DESIGN_SPECS/decision-time-data-binding-pattern.md § Sub-struct refinement
// (closes Class 27 — scalar cfg-mirror flattens per-instance distinction).
//
// Future per-resolved fields extend HERE (single-line addition) + Order_BindPreResolved
// extends concurrently. Consumer sites unchanged.
//
// Sized at 48 B (2 × FPN<F=64> = 24 B each). Placed at end of Order<F> HOT cluster.
//======================================================================================================
template <unsigned F>
struct OrderPreResolved {
    FPN<F> fee_rate;       // pre-resolved at submit: is_maker ? maker_rate : taker_rate
    FPN<F> slippage_pct;   // pre-resolved per-core
    // Future per-resolved fields (extend in lockstep with Order_BindPreResolved):
    //   - effective_kill_switch_threshold (per-core risk envelope at submit time)
    //   - effective_min_holding_ticks (per-core time-exit floor)
    //   - effective_intended_strategy_dispatch (pre-resolved dispatch arm)
};
static_assert(sizeof(OrderPreResolved<64>) == 48,
              "OrderPreResolved<64> size locked at 48 B; if changing, update Order<64> size_assert.");

// v5.15.5.F.4c.3 WIP2d-1.B.1 — Order<F> bit-packed flags + OrderPreResolved sub-struct.
// Closes Class 27 (OMS scalar cfg-mirror cluster) via Order pre-resolve at submit.
// Layout: HOT cluster exactly 4 cache lines (256 B); COLD cluster exactly 1 cache line (64 B).
// HOT/COLD perfectly aligned; no inter-cluster cache-line mixing. Per cache-layout-discipline-
// for-hot-side-structs.md + decision-first-cluster-layout-pattern.md.
//
// Previous (v5.15.5.C.1): Order<F> = 280 B with line-3 HOT/COLD mixing.
// New (v5.15.5.F.4c.3 WIP2d-1.B.1): Order<F> = 320 B with clean cluster boundaries.
// `exchange_id[64]` is touched only on terminal-state transitions + error/REJECTED
// logging — drainer + Submit hot paths never read it. Stays in COLD tail.
template <unsigned F>
struct Order {
    // ────────── HOT cluster — exactly 4 cache lines (256 B) ──────────
    uint64_t              id;             // 8 B  @ 0    local monotonic id, assigned by OMS
    uint64_t              client_id;      // 8 B  @ 8    idempotency key (== id for first attempt; phase 06 may decouple)
    // Bit-packed flags: type[2] + state[4] + is_maker[1] + leg[1] + retry_count[8].
    // Access via Order_GetType / Order_SetType / etc. inline fns; NEVER direct bit-twiddle.
    uint16_t              flags_packed;   // 2 B  @ 16
    int16_t               core_id;        // 2 B  @ 18   which executor core, -1 for non-core orders
    uint8_t               strategy_id;    // 1 B  @ 20   STRATEGY_* constant, for trade log CSV
    uint8_t               _pad_hot1[3];   // 3 B  @ 21   pad to FPN alignment (FPN<64> alignof = 8)
    FPN<F>                requested_qty;            // 24 B @ 24
    FPN<F>                requested_price;          // 24 B @ 48  limit only, ignored for MARKET (phase 08 forward-compat)
    FPN<F>                filled_qty;               // 24 B @ 72  running total across partials
    FPN<F>                avg_fill_price;           // 24 B @ 96  weighted across partials
    // phase 03 chunk 3: context fields for the OMS fill handler. when event_log_mode == 1,
    // the OMS opens portfolio slots on fill and needs the TP/SL/strategy the controller
    // intended at entry time. Also carries event_price for paper mode fills (no adapter
    // callback to supply a fill price, so we use the market price at submit time).
    FPN<F>                intended_tp;              // 24 B @ 120 TP to apply when this order fills (entry only)
    FPN<F>                intended_sl;              // 24 B @ 144 SL to apply when this order fills (entry only)
    FPN<F>                event_price;              // 24 B @ 168 market price at submit time (paper fill price)
    uint64_t              submitted_at_us;          // 8 B  @ 192 wall-clock microseconds since epoch
    uint64_t              last_update_us;           // 8 B  @ 200 last state transition timestamp
    // Decision-time-bound values, pre-resolved at Order submit via Order_BindPreResolved().
    // HandleFill reads o->pre_resolved.fee_rate directly — zero OMS cache lookup.
    // Per DESIGN_SPECS/decision-time-data-binding-pattern.md § Sub-struct refinement.
    OrderPreResolved<F>   pre_resolved;             // 48 B @ 208 — sub-struct, future extension point
    // HOT subtotal: 256 B (exactly 4 cache lines)

    // ────────── COLD cluster — exactly 1 cache line (64 B) ──────────
    // exchange_id is only set on adapter-side ACK (terminal-or-near-terminal) and read on
    // REJECTED logging / reconcile audit. Per-fill drainer hot path never touches this.
    char                  exchange_id[64];          // 64 B @ 256
};

//======================================================================================================
// [BIT-PACKED FLAG ACCESSORS] (v5.15.5.F.4c.3 WIP2d-1.B.1)
//======================================================================================================
// Branchless mask-select accessors over Order<F>::flags_packed. Compiler inlines; zero
// runtime overhead vs direct field access. ALL consumer sites use these accessors —
// direct `o->flags_packed` bit-twiddling FORBIDDEN outside Order.hpp.
//======================================================================================================
template <unsigned F>
inline OrderType Order_GetType(const Order<F>* o) {
    return (OrderType)((o->flags_packed & MASK_ORDER_TYPE) >> SHIFT_ORDER_TYPE);
}
template <unsigned F>
inline void Order_SetType(Order<F>* o, OrderType t) {
    o->flags_packed = (uint16_t)((o->flags_packed & ~MASK_ORDER_TYPE)
                                 | (((uint16_t)t << SHIFT_ORDER_TYPE) & MASK_ORDER_TYPE));
}

template <unsigned F>
inline OrderState Order_GetState(const Order<F>* o) {
    return (OrderState)((o->flags_packed & MASK_ORDER_STATE) >> SHIFT_ORDER_STATE);
}
template <unsigned F>
inline void Order_SetState(Order<F>* o, OrderState s) {
    o->flags_packed = (uint16_t)((o->flags_packed & ~MASK_ORDER_STATE)
                                 | (((uint16_t)s << SHIFT_ORDER_STATE) & MASK_ORDER_STATE));
}

template <unsigned F>
inline bool Order_GetIsMaker(const Order<F>* o) {
    return (o->flags_packed & MASK_ORDER_IS_MAKER) != 0;
}
template <unsigned F>
inline void Order_SetIsMaker(Order<F>* o, bool is_maker) {
    o->flags_packed = (uint16_t)((o->flags_packed & ~MASK_ORDER_IS_MAKER)
                                 | (is_maker ? MASK_ORDER_IS_MAKER : (uint16_t)0));
}

template <unsigned F>
inline uint8_t Order_GetLeg(const Order<F>* o) {
    return (uint8_t)((o->flags_packed & MASK_ORDER_LEG) >> SHIFT_ORDER_LEG);
}
template <unsigned F>
inline void Order_SetLeg(Order<F>* o, uint8_t leg) {
    o->flags_packed = (uint16_t)((o->flags_packed & ~MASK_ORDER_LEG)
                                 | (((uint16_t)leg << SHIFT_ORDER_LEG) & MASK_ORDER_LEG));
}

template <unsigned F>
inline uint8_t Order_GetRetryCount(const Order<F>* o) {
    return (uint8_t)((o->flags_packed & MASK_ORDER_RETRY_COUNT) >> SHIFT_ORDER_RETRY_COUNT);
}
template <unsigned F>
inline void Order_SetRetryCount(Order<F>* o, uint8_t retry) {
    o->flags_packed = (uint16_t)((o->flags_packed & ~MASK_ORDER_RETRY_COUNT)
                                 | (((uint16_t)retry << SHIFT_ORDER_RETRY_COUNT) & MASK_ORDER_RETRY_COUNT));
}

// Initialize an order to PENDING state with the given identifying fields.
// FPN amounts are zeroed; the caller fills in requested_qty (and optionally requested_price
// for limit orders) BEFORE calling OrderManager_Submit. Caller calls Order_SetIsMaker +
// Order_BindPreResolved BEFORE submit when fee/slippage accounting matters.
//
// id and client_id are set equal — phase 01 uses the local monotonic id as the idempotency
// key. Phase 06 (production hardening) may decouple them if retry semantics require a stable
// client_id across retries.
template <unsigned F>
inline void Order_Init(Order<F>* o, uint64_t id, int16_t core_id, OrderType type) {
    o->id              = id;
    o->client_id       = id;
    o->core_id         = core_id;
    o->strategy_id     = 0xFF;  // STRATEGY_NONE
    o->flags_packed    = 0;     // type/state/is_maker/leg/retry all zero; then set type+state via accessors
    Order_SetType(o, type);
    Order_SetState(o, ORDER_PENDING);
    // is_maker, leg, retry_count remain 0 (taker / leg A / 0 retries) per zero-init
    o->requested_qty             = FPN_Zero<F>();
    o->requested_price           = FPN_Zero<F>();
    o->filled_qty                = FPN_Zero<F>();
    o->avg_fill_price            = FPN_Zero<F>();
    o->intended_tp               = FPN_Zero<F>();
    o->intended_sl               = FPN_Zero<F>();
    o->event_price               = FPN_Zero<F>();
    o->submitted_at_us           = 0;
    o->last_update_us            = 0;
    o->pre_resolved.fee_rate     = FPN_Zero<F>();
    o->pre_resolved.slippage_pct = FPN_Zero<F>();
    o->exchange_id[0]            = '\0';
}

//======================================================================================================
// [DECISION-TIME DATA BINDING] (v5.15.5.F.4c.3 WIP2d-1.B.1)
//======================================================================================================
// Pre-resolve per-instance cfg values onto the Order. Call this AFTER Order_Init + AFTER
// caller has set is_maker (via Order_SetIsMaker), BEFORE OrderManager_Submit.
//
// Per DESIGN_SPECS/decision-time-data-binding-pattern.md § Sub-struct refinement (closes
// Class 27 — OMS scalar cfg-mirror flattens per-instance distinction). HandleFill (drainer
// thread) reads from o->pre_resolved directly; zero cross-thread cfg read; hot-swap safe
// (in-flight Orders keep their rate; new Orders get current rate).
//
// Future per-resolved fields extend OrderPreResolved + this fn body in lockstep. Consumer
// sites unchanged when adding new fields.
//
// Template note: caller MUST include CoreFrameworks/ControllerConfig.hpp for PerCoreCfg<F>
// definition; this fn body uses PerCoreCfg<F>'s field accessors.
//======================================================================================================
template <unsigned F>
inline void Order_BindPreResolved(Order<F>* o, const ::PerCoreCfg<F>& core_cfg) {
    bool is_maker = Order_GetIsMaker(o);
    o->pre_resolved.fee_rate = is_maker
        ? core_cfg.fee_rate_maker
        : core_cfg.fee_rate_taker;
    o->pre_resolved.slippage_pct = core_cfg.slippage_pct;
}

// Anti-drift guard: pin Order<F> size to catch silent ABI breakage from future field
// additions. If you add a field and this fails, decide CONSCIOUSLY whether the change is
// acceptable (and update the constant) or pack into existing _pad / use the OrderPreResolved
// sub-struct extension point. OrderPool slots are sized for this struct; growing it changes
// the pool's memory footprint.
//
// Per-instantiation: F=64 is the live-engine + suite default.
static_assert(sizeof(Order<64>) == 320,
              "Order<64> size locked at 320 B (HOT 256 B + COLD 64 B = exactly 5 cache lines, "
              "HOT/COLD cluster-aligned). v5.15.5.F.4c.3 WIP2d-1.B.1: bumped from 280 B for "
              "OrderPreResolved sub-struct + bit-packed flags. If changing, verify OrderPool "
              "slot assumptions + decision-time-data-binding-pattern.md sub-struct extension "
              "+ cache-layout-discipline-for-hot-side-structs.md HOT/COLD cluster boundaries.");

// Predicate: is this order in a terminal state (no further transitions)? Used by
// OrderManager_Tick to decide whether to free the slot. Phase 03 (event log) keeps terminal
// orders around longer for audit; phase 01 frees them immediately on terminal transition.
template <unsigned F>
inline bool Order_IsTerminal(const Order<F>* o) {
    OrderState s = Order_GetState(o);
    return s == ORDER_FILLED
        || s == ORDER_REJECTED
        || s == ORDER_CANCELED
        || s == ORDER_TIMEOUT;
}

}  // namespace tt
