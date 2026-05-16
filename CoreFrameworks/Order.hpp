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
#include <cstdio>  // std::fprintf for Order_WarnIfNotPreResolved (WIP2d-1.B.1)

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
static constexpr uint32_t MASK_ORDER_TYPE          = 0x00000003u;  // bits 0-1
static constexpr uint32_t SHIFT_ORDER_TYPE         = 0;
static constexpr uint32_t MASK_ORDER_STATE         = 0x0000003Cu;  // bits 2-5
static constexpr uint32_t SHIFT_ORDER_STATE        = 2;
static constexpr uint32_t MASK_ORDER_IS_MAKER      = 0x00000040u;  // bit 6
static constexpr uint32_t SHIFT_ORDER_IS_MAKER     = 6;
static constexpr uint32_t MASK_ORDER_LEG           = 0x00000080u;  // bit 7
static constexpr uint32_t SHIFT_ORDER_LEG          = 7;
static constexpr uint32_t MASK_ORDER_RETRY_COUNT   = 0x0000FF00u;  // bits 8-15
static constexpr uint32_t SHIFT_ORDER_RETRY_COUNT  = 8;
// v5.15.5.F.4c.3 WIP2d-1.B.1 — pre-resolved bind discipline bit (Class 27 + silent-zero-fee closure).
// Set by Order_BindPreResolved; checked at HandleFill via Order_WarnIfNotPreResolved.
// Production: OrderManager_Submit sig requires core_cfg → BindPreResolved always called → bit always set.
// Test fixtures constructing Order directly must call Order_BindPreResolved explicitly OR set
// pre_resolved.* fields directly + Order_MarkPreResolvedBound.
static constexpr uint32_t MASK_ORDER_PRE_RESOLVED  = 0x00010000u;  // bit 16
static constexpr uint32_t SHIFT_ORDER_PRE_RESOLVED = 16;

// v5.15.5.F.4d — bandit-emit context bits for at-decision-time binding.
// Sister to MASK_ORDER_PRE_RESOLVED at the same field — same bind site
// (Order_BindPreResolved). Per § N.1 of the .F.4d merged plan body +
// multi-bit-state-encoding-pattern.md INVARIANT (5th canonical application
// on Order::flags_packed alongside MASK_ORDER_PRE_RESOLVED at bit 16).
//
// Bit allocation post-.F.4d:
//   Bits 17-19: bandit_active_state (3 bits for ≤ 8 states; 5 currently per FOREACH_BANDIT_ALGORITHM)
//   Bits 20-22: bandit_regime       (3 bits for ≤ 8 regimes; 5 currently per FOREACH_REGIME)
//   Bits 23-25: bandit_chosen_arm   (3 bits for ≤ 8 arms; ENSEMBLE_HORIZON_MAX)
//   Bits 26-31: 6 bits free headroom for future Order metadata
//
// Bit-width invariants (FOREACH_BANDIT_ALGORITHM_COUNT ≤ 8 / NUM_REGIMES ≤ 8 /
// ENSEMBLE_HORIZON_MAX ≤ 8) static_asserted in ML_Headers/bandit_dispatch_table.hpp
// where those ML-side symbols are visible. Order.hpp deliberately stays include-light;
// keeping ML-side deps out of OMS headers preserves dep direction OMS → ML.
//
// Set at Order_BindPreResolved (sister to fee_rate/slippage_pct binding) via
// MBS_OrderSetBanditContext below — bandit context flows with Order through trade lifecycle.
// Read at calib emit / reward attribution via MBS_OrderBandit* accessors below.
static constexpr uint32_t SHIFT_ORDER_BANDIT_ACTIVE_STATE = 17;
static constexpr uint32_t SHIFT_ORDER_BANDIT_REGIME       = 20;
static constexpr uint32_t SHIFT_ORDER_BANDIT_CHOSEN_ARM   = 23;
static constexpr uint32_t MASK_ORDER_BANDIT_3BIT          = 0x7u;

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
    // Bit-packed flags: type[2] + state[4] + is_maker[1] + leg[1] + retry_count[8] + pre_resolved_bound[1] @bit16.
    // Access via Order_GetType / Order_SetType / etc. inline fns; NEVER direct bit-twiddle.
    // v5.15.5.F.4c.3 WIP2d-1.B.1: widened uint16_t → uint32_t for pre_resolved_bound bit; padding _pad_hot1
    // absorbs the 2 B widening (3 B → 1 B). Cache-neutral: Order @ 320 B unchanged, FPN field offsets unchanged.
    uint32_t              flags_packed;   // 4 B  @ 16
    int16_t               core_id;        // 2 B  @ 20   which executor core, -1 for non-core orders
    uint8_t               strategy_id;    // 1 B  @ 22   STRATEGY_* constant, for trade log CSV
    uint8_t               _pad_hot1[1];   // 1 B  @ 23   pad to FPN alignment (FPN<64> alignof = 8); shrunk 3→1 by flags_packed widening
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
    o->flags_packed = (uint32_t)((o->flags_packed & ~MASK_ORDER_TYPE)
                                 | (((uint32_t)t << SHIFT_ORDER_TYPE) & MASK_ORDER_TYPE));
}

template <unsigned F>
inline OrderState Order_GetState(const Order<F>* o) {
    return (OrderState)((o->flags_packed & MASK_ORDER_STATE) >> SHIFT_ORDER_STATE);
}
template <unsigned F>
inline void Order_SetState(Order<F>* o, OrderState s) {
    o->flags_packed = (uint32_t)((o->flags_packed & ~MASK_ORDER_STATE)
                                 | (((uint32_t)s << SHIFT_ORDER_STATE) & MASK_ORDER_STATE));
}

template <unsigned F>
inline bool Order_GetIsMaker(const Order<F>* o) {
    return (o->flags_packed & MASK_ORDER_IS_MAKER) != 0;
}
template <unsigned F>
inline void Order_SetIsMaker(Order<F>* o, bool is_maker) {
    o->flags_packed = (uint32_t)((o->flags_packed & ~MASK_ORDER_IS_MAKER)
                                 | (is_maker ? MASK_ORDER_IS_MAKER : (uint32_t)0));
}

template <unsigned F>
inline uint8_t Order_GetLeg(const Order<F>* o) {
    return (uint8_t)((o->flags_packed & MASK_ORDER_LEG) >> SHIFT_ORDER_LEG);
}
template <unsigned F>
inline void Order_SetLeg(Order<F>* o, uint8_t leg) {
    o->flags_packed = (uint32_t)((o->flags_packed & ~MASK_ORDER_LEG)
                                 | (((uint32_t)leg << SHIFT_ORDER_LEG) & MASK_ORDER_LEG));
}

template <unsigned F>
inline uint8_t Order_GetRetryCount(const Order<F>* o) {
    return (uint8_t)((o->flags_packed & MASK_ORDER_RETRY_COUNT) >> SHIFT_ORDER_RETRY_COUNT);
}
template <unsigned F>
inline void Order_SetRetryCount(Order<F>* o, uint8_t retry) {
    o->flags_packed = (uint32_t)((o->flags_packed & ~MASK_ORDER_RETRY_COUNT)
                                 | (((uint32_t)retry << SHIFT_ORDER_RETRY_COUNT) & MASK_ORDER_RETRY_COUNT));
}

// v5.15.5.F.4c.3 WIP2d-1.B.1 — pre-resolved bind discipline accessors.
// `Order_BindPreResolved` sets the bit; `Order_MarkPreResolvedBound` is the explicit-set path
// for test fixtures that manually wire `pre_resolved.*` fields (bypassing the cfg-driven bind).
template <unsigned F>
inline bool Order_GetPreResolvedBound(const Order<F>* o) {
    return (o->flags_packed & MASK_ORDER_PRE_RESOLVED) != 0;
}
template <unsigned F>
inline void Order_MarkPreResolvedBound(Order<F>* o) {
    o->flags_packed = (uint32_t)(o->flags_packed | MASK_ORDER_PRE_RESOLVED);
}

//======================================================================================================
// [BANDIT CONTEXT MULTI-BIT ACCESSORS] (v5.15.5.F.4d)
//======================================================================================================
// MBS_* (multi-bit-state) branchless accessors for bandit context bits 17-25 on flags_packed.
// Naming per multi-bit-state-encoding-pattern.md (5th canonical INVARIANT application).
// Sister to Order_Get/SetPreResolvedBound above — same field, same bind site, same lifecycle
// (set at Order_BindPreResolved; flows with Order through trade lifecycle; read at calib emit
// + reward attribution sites). Per § N.1 of the .F.4d merged plan body.
//
// Branchless — pure shift + mask + bitwise-OR/AND; no data-dependent dispatch. H20 compliant.
//======================================================================================================
template <unsigned F>
inline int MBS_OrderBanditActiveState(const Order<F>& o) {
    return (int)((o.flags_packed >> SHIFT_ORDER_BANDIT_ACTIVE_STATE) & MASK_ORDER_BANDIT_3BIT);
}
template <unsigned F>
inline int MBS_OrderBanditRegime(const Order<F>& o) {
    return (int)((o.flags_packed >> SHIFT_ORDER_BANDIT_REGIME) & MASK_ORDER_BANDIT_3BIT);
}
template <unsigned F>
inline int MBS_OrderBanditChosenArm(const Order<F>& o) {
    return (int)((o.flags_packed >> SHIFT_ORDER_BANDIT_CHOSEN_ARM) & MASK_ORDER_BANDIT_3BIT);
}
template <unsigned F>
inline void MBS_OrderSetBanditContext(Order<F>* o, int state, int regime, int arm) {
    // Clear existing bandit bits then OR in new values. Sister to BITMAP_SET shape but
    // for a 3-slot multi-bit-state word; clear-and-set keeps the bit positions tidy when
    // called multiple times on the same Order (e.g., re-bind via Order_BindPreResolved).
    constexpr uint32_t BANDIT_CLEAR_MASK = ~(
        (MASK_ORDER_BANDIT_3BIT << SHIFT_ORDER_BANDIT_ACTIVE_STATE) |
        (MASK_ORDER_BANDIT_3BIT << SHIFT_ORDER_BANDIT_REGIME) |
        (MASK_ORDER_BANDIT_3BIT << SHIFT_ORDER_BANDIT_CHOSEN_ARM));
    o->flags_packed = (o->flags_packed & BANDIT_CLEAR_MASK)
        | (((uint32_t)state  & MASK_ORDER_BANDIT_3BIT) << SHIFT_ORDER_BANDIT_ACTIVE_STATE)
        | (((uint32_t)regime & MASK_ORDER_BANDIT_3BIT) << SHIFT_ORDER_BANDIT_REGIME)
        | (((uint32_t)arm    & MASK_ORDER_BANDIT_3BIT) << SHIFT_ORDER_BANDIT_CHOSEN_ARM);
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
    // v5.15.5.F.4c.3 WIP2d-1.B.1 — mark bit to satisfy Order_WarnIfNotPreResolved at HandleFill.
    Order_MarkPreResolvedBound(o);
}

//======================================================================================================
// [PRE-RESOLVED BIND DISCIPLINE GUARD] (v5.15.5.F.4c.3 WIP2d-1.B.1)
//======================================================================================================
// Called at HandleFill entry. Always-on runtime check; cost = 1 bit-test + predicted-not-taken branch
// (~0 cycles amortized in production). Production: bit always set because OrderManager_Submit sig
// requires core_cfg → BindPreResolved always called inside Submit. Test fixtures constructing Order
// directly via Order_Init + HandleFill bypass: bit not set → stderr warn. Tests asserting on fee
// accumulation will fail (visible canary); tests that don't care about fees get the warning but
// continue (cosmetic only).
//
// Why warn-not-abort: drainer thread; aborting kills the engine on the rare misuse. Production
// invariant catches this at compile time via sig; runtime check is a test-build belt-and-suspenders.
//
// Closes silent-zero-fee-rate class structurally (Class 27 sister).
//======================================================================================================
template <unsigned F>
inline void Order_WarnIfNotPreResolved(const Order<F>* o, const char* site) {
    if (__builtin_expect(!Order_GetPreResolvedBound(o), 0)) {
        std::fprintf(stderr,
            "[OMS] WARN: %s called on Order id=%llu core=%d without Order_BindPreResolved; "
            "pre_resolved.fee_rate=%f (expected explicit bind via core_cfg OR explicit "
            "Order_MarkPreResolvedBound after manual set). Production paths require core_cfg "
            "at OrderManager_Submit (sig-enforced); this Order bypassed Submit (test fixture path).\n",
            site, (unsigned long long)o->id, (int)o->core_id,
            FPN_ToDouble(o->pre_resolved.fee_rate));
    }
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
