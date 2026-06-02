// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
#include "../Limits.hpp"
// [PORTFOLIO MANAGER]
//======================================================================================================
// this is basically just gonna track positions and stuff and be the core portfolio managment system, im not sure if ill actually add the rebaalncing logic and stuff here, but it should eventually just serve as the API call to get position deltas and stuff, it will be more robust that just the simple pool allocator i was attempting earlier
//======================================================================================================
// [INCLUDE]
//======================================================================================================
#ifndef PORTFOLIO_HPP
#define PORTFOLIO_HPP

#include "../FixedPoint/FixedPointN.hpp"
#include "../MemHeaders/PositionFieldRegistry.hpp"  // v5.15.5.C.4 Phase POS — FOREACH_POSITION_FIELD + PERSIST_KIND dispatch
#include <stdio.h>
#include <time.h>      // v5.11.65 — clock_gettime(CLOCK_REALTIME) for entry_timestamp_us
#include <cstddef>     // v5.15.5.C.4 Phase POS — offsetof for static_assert layout locks
//======================================================================================================
// [STRUCTS]
//======================================================================================================
// Im not sure how many positions i really want to track here but for now im just gonna leave it at like 16 i think, there will probably be more advanced logic added later to have a model that watches performace and dynamically updates or something like i attempted to do in FoxML core, but this is a deepr dive so i can actually learn and understand the logic behind stuff, and i just think its cool as shit, like why learn java when stuff lke this exists lmao, also i get to make my own library so im not functioning off blackbox implementations where the end of the documentation is lke "Trust me bro", and i hate reading documentation, so id rather build my own
//======================================================================================================
// v5.15.5.C.5 — Position struct generated from FOREACH_POSITION_FIELD registry
// + `alignas(64)` per `DESIGN_SPECS/hot-side-array-element-alignment-for-sparse-access.md`.
//
// Field order (controlled by FOREACH_POSITION_FIELD in PositionFieldRegistry.hpp)
// preserves the prior manual struct layout:
//   - Hot-path fields first (TP/SL — ExitGate reads every tick; fit in 1st cache line)
//   - Warm fields middle (quantity, entry_price, entry_fee — read at fill / P&L)
//   - Cold fields last (originals, timestamp, pair_index — slow-path-only)
//
// v5.15.5.C.5 changes vs C.4:
//   - SKIP_PERSIST fields (exit_fill_price, is_maker) REVERTED to OMS sibling arrays
//     (per `slot-state-foreach-registry-with-storage-routing.md` decision tree for
//     sparse-access ephemeral state)
//   - `alignas(64)` on Position struct → sizeof = 192B = 3 cache lines exact
//   - Per-slot hot-path access: guaranteed 1 cache line (TP+SL fit in first 64B
//     of each Position; each Position[N] starts on 64B boundary)
//   - 8B trailing alignas pad (cost: 128B per OMS); savings: ~50% reduction in
//     hot-path cache misses at sparse-iteration access patterns
//
// Wire format: PORTFOLIO_SNAPSHOT_VERSION=5 byte-identical (PERSIST_BYTES=184
// unchanged; alignas pad NOT in wire format; Save/Load writes 184B per position).
//
// POS.2's SKIP_PERSIST infrastructure (FOREACH_POSITION_FIELD_SKIP_PERSIST registry +
// PERSIST_KIND filter dispatch in Portfolio_Save/Load) RETAINED as future-extension
// capacity. Empty registry today; available for future fields that warrant Position-
// locality co-access with PERSIST fields.
template <unsigned F> struct alignas(64) Position {
    // Auto-generated fields from FOREACH_POSITION_FIELD (PositionFieldRegistry.hpp):
    //   take_profit_price (FPN<F>)   ← hot; offset 0
    //   stop_loss_price   (FPN<F>)   ← hot; offset 24
    //   quantity          (FPN<F>)
    //   entry_price       (FPN<F>)
    //   entry_fee         (FPN<F>)
    //   original_tp       (FPN<F>)
    //   original_sl       (FPN<F>)
    //   entry_timestamp_us (uint64_t)
    //   pair_index        (int8_t)
    #define POSITION_EMIT_FIELD(name, type, init, persist_kind, doc) type name = init;
    FOREACH_POSITION_FIELD(POSITION_EMIT_FIELD)
    #undef POSITION_EMIT_FIELD

    // Manual padding to align Position to 8 bytes after int8_t pair_index.
    // Part of wire format (PORTFOLIO_SNAPSHOT_VERSION=5 byte layout).
    // POSITION_PERSIST_BYTES = offsetof(_pad_pos) + sizeof(_pad_pos) = 184.
    uint8_t _pad_pos[7];

    // SKIP_PERSIST fields would expand here. Empty today per C.5 revert.
    // The `alignas(64)` decorator above implicitly adds 8B trailing pad
    // to round sizeof(Position) to 192B = 3 cache lines exact.
};

// v5.15.5.C.4 Phase POS — static_assert layout locks per the design spec
// `function-struct-alignment-for-single-mov-access.md` + wire-format byte
// preservation discipline. Catches accidental field-reorder that would
// invalidate PORTFOLIO_SNAPSHOT_VERSION=5 wire format.
//
// Reference layout (FPN<64> = 24B; PORTFOLIO_SNAPSHOT_VERSION=5):
//   offset 0:   take_profit_price   (24B)
//   offset 24:  stop_loss_price     (24B)
//   offset 48:  quantity            (24B)
//   offset 72:  entry_price         (24B)
//   offset 96:  entry_fee           (24B)
//   offset 120: original_tp         (24B)
//   offset 144: original_sl         (24B)
//   offset 168: entry_timestamp_us  (8B)
//   offset 176: pair_index          (1B)
//   offset 177: _pad_pos            (7B)
//   total:      184 bytes
// v5.15.5.C.5 — sizeof locked at 192B (alignas(64) padded to 3 cache lines).
// PERSIST prefix size stays at 184 — wire format unchanged.
//
// Per DESIGN_SPECS/hot-side-array-element-alignment-for-sparse-access.md:
// - sizeof(Position) % 64 == 0 → each Position[N] starts on cache-line boundary
// - Hot-path 48B read (TP + SL at offsets 0, 24) fits in first cache line of each
//   Position[N] — guaranteed 1 cache line per slot access, regardless of N
//
// Position layout (post-C.5):
//   offset 0-23:   take_profit_price (24B; HOT)
//   offset 24-47:  stop_loss_price   (24B; HOT)
//   offset 48-71:  quantity          (24B; warm)
//   offset 72-95:  entry_price       (24B; warm)
//   offset 96-119: entry_fee         (24B; warm)
//   offset 120-143: original_tp      (24B; cold)
//   offset 144-167: original_sl      (24B; cold)
//   offset 168-175: entry_timestamp_us (8B)
//   offset 176:    pair_index        (1B)
//   offset 177-183: _pad_pos          (7B; wire-format alignment pad)
//   offset 184-191: alignas(64) trailing pad (8B)
//   Total: 192B = 3 cache lines exact
static_assert(sizeof(Position<64>) == 192,
              "Position<64> size must be 192B (alignas(64) on 184B PERSIST struct = 3 cache lines exact); "
              "see DESIGN_SPECS/hot-side-array-element-alignment-for-sparse-access.md");
static_assert(alignof(Position<64>) == 64,
              "Position<64> must have 64B alignment for hot-path single-cache-line-per-slot access");
static_assert(offsetof(Position<64>, take_profit_price)  == 0,   "Position layout: take_profit_price offset (HOT)");
static_assert(offsetof(Position<64>, stop_loss_price)    == 24,  "Position layout: stop_loss_price offset (HOT)");
static_assert(offsetof(Position<64>, quantity)           == 48,  "Position layout: quantity offset");
static_assert(offsetof(Position<64>, entry_price)        == 72,  "Position layout: entry_price offset");
static_assert(offsetof(Position<64>, entry_fee)          == 96,  "Position layout: entry_fee offset");
static_assert(offsetof(Position<64>, original_tp)        == 120, "Position layout: original_tp offset");
static_assert(offsetof(Position<64>, original_sl)        == 144, "Position layout: original_sl offset");
static_assert(offsetof(Position<64>, entry_timestamp_us) == 168, "Position layout: entry_timestamp_us offset");
static_assert(offsetof(Position<64>, pair_index)         == 176, "Position layout: pair_index offset");

// PERSIST byte count — first 184 bytes of Position go to wire format.
// Save/Load writes exactly POSITION_PERSIST_BYTES per position (16 positions × 184 = 2944 bytes
// payload; matches PORTFOLIO_SNAPSHOT_VERSION=5 byte-identical to all prior versions).
// The trailing 8B alignas pad is NOT written/read (sizeof(Position) - POSITION_PERSIST_BYTES = 8).
template <unsigned F>
constexpr size_t POSITION_PERSIST_BYTES() {
    // 9 PERSIST value fields (168B FPN + 8B uint64 + 1B int8) + 7B _pad_pos = 184B
    return offsetof(Position<F>, _pad_pos) + 7;
}
static_assert(POSITION_PERSIST_BYTES<64>() == 184,
              "Position PERSIST byte count must equal 184 — wire format (PORTFOLIO_SNAPSHOT_VERSION=5) byte-identical");
static_assert(sizeof(Position<64>) - POSITION_PERSIST_BYTES<64>() == 8,
              "alignas(64) trailing pad must be 8B (rounds 184B to 192B = 3 cache lines)");
//======================================================================================================
// [PORTFOLIO]
//======================================================================================================
// bitmap-based like OrderPool - same __builtin_ctz pattern, no array shifting on removal,
// hot-path exit gate only walks set bits so cleared positions are skipped automatically
//======================================================================================================
template <unsigned F> struct Portfolio {
    uint16_t active_bitmap; // hot: read first every tick by ExitGate (cache line 0)
    uint16_t _pad0;
    uint32_t _pad1;         // align positions to 8 bytes
    Position<F> positions[MAX_PORTFOLIO_POSITIONS];
    static_assert(MAX_PORTFOLIO_POSITIONS <= 16,
                  "active_bitmap is uint16_t (16 bits); MAX_PORTFOLIO_POSITIONS must fit it — raising it "
                  "past 16 silently truncates the bitmap (bitmap-overflow-protection-discipline; H21-sibling).");
};
//======================================================================================================
// [EXIT STRUCTS]
//======================================================================================================
// ExitRecord snapshots all position data at exit time — slot may be reused by a
// new fill before DrainExits runs, so nothing downstream should read from the slot
//======================================================================================================
template <unsigned F> struct ExitRecord {
    uint32_t position_index;    // slot index (for entry_ticks/entry_strategy lookup only)
    int reason;                 // 0 = take profit, 1 = stop loss
    uint64_t tick;
    FPN<F> exit_price;
    // position data snapshot — captured at exit time, immune to slot reuse
    FPN<F> entry_price;
    FPN<F> quantity;
    FPN<F> entry_fee;
    int8_t pair_index;
    uint8_t _pad_rec[7];
};

template <unsigned F> struct ExitBuffer {
    uint32_t count;             // hot: read first every tick (cache line 0 with records[0])
    uint32_t _pad0;             // align records to 8 bytes
    ExitRecord<F> records[16];
};

template <unsigned F> inline void ExitBuffer_Init(ExitBuffer<F> *buf) {
    buf->count = 0;
}

template <unsigned F> inline void ExitBuffer_Clear(ExitBuffer<F> *buf) {
    buf->count = 0;
}

// exact net proceeds pending in exit buffer — matches what DrainExits/RecordExit will credit
// reads ALL data from ExitRecord (not position slots — those may have been reused)
template <unsigned F>
inline FPN<F> ExitBuffer_PendingProceeds(const ExitBuffer<F> *buf,
                                          FPN<F> fee_rate, FPN<F> slippage_pct) {
    FPN<F> total = FPN_Zero<F>();
    for (uint32_t i = 0; i < buf->count; i++) {
        FPN<F> exit_price = buf->records[i].exit_price;
        if (!FPN_IsZero(slippage_pct)) {
            FPN<F> slip = FPN_Mul(exit_price, slippage_pct);
            exit_price = FPN_SubSat(exit_price, slip);
        }
        FPN<F> gross = FPN_Mul(exit_price, buf->records[i].quantity);
        FPN<F> fee = FPN_Mul(gross, fee_rate);
        total = FPN_AddSat(total, FPN_SubSat(gross, fee));
    }
    return total;
}
//======================================================================================================
// [FUNCTIONS]
//======================================================================================================
// similar to the pool allocator, will need more work and im not sure if i want the rebalancing adn stuff here or in another header, probably another header for the actual managment, because these are just the basic functions to add and manipulate the actual opsitions, tnd are dependent on the buy/sell gates
//======================================================================================================
template <unsigned F> inline void Portfolio_Init(Portfolio<F> *portfolio) {
    for (int i = 0; i < 16; i++) {
        portfolio->positions[i].quantity          = FPN_Zero<F>();
        portfolio->positions[i].entry_price       = FPN_Zero<F>();
        portfolio->positions[i].entry_fee         = FPN_Zero<F>();
        portfolio->positions[i].take_profit_price = FPN_Zero<F>();
        portfolio->positions[i].stop_loss_price   = FPN_Zero<F>();
    }
    portfolio->active_bitmap = 0;
}
//======================================================================================================
template <unsigned F> inline int Portfolio_IsFull(const Portfolio<F> *portfolio) {
    return portfolio->active_bitmap == 0xFFFF;
}
//======================================================================================================
template <unsigned F> inline int Portfolio_CountActive(const Portfolio<F> *portfolio) {
    return __builtin_popcount(portfolio->active_bitmap);
}
//======================================================================================================
// find position by entry price - walks active bits, returns index or -1
//======================================================================================================
template <unsigned F> inline int Portfolio_FindByPrice(const Portfolio<F> *portfolio, FPN<F> entry_price) {
    uint16_t active = portfolio->active_bitmap;
    while (active) {
        int idx = __builtin_ctz(active);
        if (FPN_Equal(portfolio->positions[idx].entry_price, entry_price)) {
            return idx;
        }
        active &= active - 1;
    }
    return -1;
}
//======================================================================================================
// add quantity to existing position at index (consolidation)
//======================================================================================================
template <unsigned F> inline void Portfolio_AddQuantity(Portfolio<F> *portfolio, int index, FPN<F> quantity) {
    portfolio->positions[index].quantity = FPN_AddSat(portfolio->positions[index].quantity, quantity);
}
//======================================================================================================
// add new position with pre-computed exit prices, returns slot index or -1 if full
//======================================================================================================
template <unsigned F>
inline int Portfolio_AddPositionWithExits(Portfolio<F> *portfolio, FPN<F> quantity, FPN<F> entry_price,
                                          FPN<F> take_profit_price, FPN<F> stop_loss_price,
                                          FPN<F> entry_fee = FPN_Zero<F>()) {
    if (portfolio->active_bitmap == 0xFFFF) return -1;
    int idx                                       = __builtin_ctz(~portfolio->active_bitmap);
    portfolio->positions[idx].quantity             = quantity;
    portfolio->positions[idx].entry_price          = entry_price;
    portfolio->positions[idx].entry_fee            = entry_fee;
    portfolio->positions[idx].take_profit_price    = take_profit_price;
    portfolio->positions[idx].stop_loss_price      = stop_loss_price;
    portfolio->positions[idx].pair_index           = -1;
    portfolio->active_bitmap |= (1 << idx);
    return idx;
}
//======================================================================================================
// legacy add - for backward compatibility with existing tests, no exit prices
//======================================================================================================
template <unsigned F> inline void Portfolio_AddPosition(Portfolio<F> *portfolio, FPN<F> quantity, FPN<F> entry_price) {
    if (portfolio->active_bitmap == 0xFFFF) return;
    int idx                                       = __builtin_ctz(~portfolio->active_bitmap);
    portfolio->positions[idx].quantity             = quantity;
    portfolio->positions[idx].entry_price          = entry_price;
    portfolio->positions[idx].entry_fee            = FPN_Zero<F>();
    portfolio->positions[idx].take_profit_price    = FPN_Zero<F>();
    portfolio->positions[idx].stop_loss_price      = FPN_Zero<F>();
    portfolio->active_bitmap |= (1 << idx);
}
//======================================================================================================
// remove - just clears the bit, data stays in slot for controller to read
//======================================================================================================
template <unsigned F> inline void Portfolio_RemovePosition(Portfolio<F> *portfolio, int index) {
    portfolio->active_bitmap &= ~(1 << index);
}
//======================================================================================================
// [PER-CORE SHARDING SLOT HELPERS]
//======================================================================================================
// per-core sharding (phase 04+) binds each execution core to a fixed portfolio
// slot. slot index == core_id directly. these helpers open and close a slot by
// index instead of the auto-assigning __builtin_ctz path used by the legacy
// hot path. controller core calls these from PortfolioController_OnEvent when
// it processes a TradeEvent from a per-core event ring.
//
// open: writes the position fields and sets the active bit. caller must
//   ensure the slot isn't already active (assertion in debug, undefined in
//   release — controller's job to track per-core state).
//
// close: clears the active bit and returns gross P&L = (exit - entry) * qty.
//   does NOT touch balance or fees — controller's OnEvent does that, this
//   function just snapshots the gross.
//======================================================================================================
// v5.15.5.F.2 — Position entry args struct. Bundles ALL fields that
// Portfolio_OpenSlot writes to a Position so live-entry path + replay path
// populate from the same shape. Closes the Class-18 mirror between:
//   - Live entry: drainer's HandleFill builds args from a TradeEvent
//   - Replay:     Portfolio_FromEventLog builds args from an OrderEvent
//
// Adding a NEW Position field that needs preservation across replay =
// ONE line in this struct + populate at both sites. Eliminates the
// recurring "I forgot to preserve X across replay" bug class — see
// CLAUDE.md item 19 + DESIGN_SPECS/structural-fix-preferred-decision-
// framework.md.
//
// Field semantics:
//   - entry_price/quantity/take_profit_price/stop_loss_price/entry_fee:
//     always populated; no defaults safe (would compute wrong).
//   - entry_timestamp_us = 0 → Portfolio_OpenSlot calls clock_gettime(
//     CLOCK_REALTIME) (live-entry semantics). Non-zero → preserve as-is
//     (replay path: passes OrderEvent.timestamp_us so hold-time survives
//     engine restart).
//   - pair_index = -1 → no pairing (single-position mode or unpaired leg).
//     Non-negative → paired leg index for partial-exit semantics (.F.1.B
//     audit candidate when partial-exit replay needs leg-A/leg-B pairing
//     preservation).
template <unsigned F>
struct PositionEntryArgs {
    FPN<F>   entry_price;
    FPN<F>   quantity;
    FPN<F>   take_profit_price;
    FPN<F>   stop_loss_price;
    FPN<F>   entry_fee          = FPN_Zero<F>();
    uint64_t entry_timestamp_us = 0;   // 0 = use clock_gettime(REALTIME); non-zero = preserve
    int      pair_index         = -1;  // -1 = unpaired; >=0 = paired-leg slot index (partial-exit)
};

// v5.15.5.F.2 — Portfolio_OpenSlot now takes a const-ref PositionEntryArgs.
// Live-entry path + replay path populate identically (struct-shape parity).
// Old multi-arg signature shim (next overload) preserves backward compat
// for existing call sites; deprecated but functional.
template <unsigned F>
inline void Portfolio_OpenSlot(Portfolio<F>* portfolio, int slot,
                                const PositionEntryArgs<F>& args) {
    portfolio->positions[slot].entry_price       = args.entry_price;
    portfolio->positions[slot].quantity          = args.quantity;
    portfolio->positions[slot].entry_fee         = args.entry_fee;
    portfolio->positions[slot].take_profit_price = args.take_profit_price;
    portfolio->positions[slot].stop_loss_price   = args.stop_loss_price;
    portfolio->positions[slot].original_tp       = args.take_profit_price;
    portfolio->positions[slot].original_sl       = args.stop_loss_price;
    portfolio->positions[slot].pair_index        = args.pair_index;
    // v5.11.65 — wall-clock entry timestamp for cross-restart hold tracking.
    // CLOCK_REALTIME (not _MONOTONIC) so the value survives engine restart.
    if (args.entry_timestamp_us != 0) {
        portfolio->positions[slot].entry_timestamp_us = args.entry_timestamp_us;
    } else {
        struct timespec ts;
        if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
            portfolio->positions[slot].entry_timestamp_us =
                (uint64_t)ts.tv_sec * 1000000ULL +
                (uint64_t)ts.tv_nsec / 1000ULL;
        } else {
            portfolio->positions[slot].entry_timestamp_us = 0;
        }
    }
    portfolio->active_bitmap |= (uint16_t)(1 << slot);
}

// v5.15.5.F.2 — Back-compat shim for existing call sites that pass individual
// args. New code should construct a PositionEntryArgs + call the primary
// overload directly. Deprecation path: convert all callers, then delete this.
template <unsigned F>
inline void Portfolio_OpenSlot(Portfolio<F>* portfolio, int slot,
                                FPN<F> entry_price, FPN<F> quantity,
                                FPN<F> take_profit_price, FPN<F> stop_loss_price,
                                FPN<F> entry_fee = FPN_Zero<F>()) {
    PositionEntryArgs<F> args;
    args.entry_price        = entry_price;
    args.quantity           = quantity;
    args.take_profit_price  = take_profit_price;
    args.stop_loss_price    = stop_loss_price;
    args.entry_fee          = entry_fee;
    // entry_timestamp_us + pair_index = struct defaults (0 / -1; live-entry semantics)
    Portfolio_OpenSlot(portfolio, slot, args);
}

template <unsigned F>
inline FPN<F> Portfolio_CloseSlot(Portfolio<F> *portfolio, int slot, FPN<F> exit_price) {
    FPN<F> diff = FPN_Sub(exit_price, portfolio->positions[slot].entry_price);
    FPN<F> gross = FPN_Mul(diff, portfolio->positions[slot].quantity);
    portfolio->active_bitmap &= ~(uint16_t)(1 << slot);
    return gross;
}

template <unsigned F>
inline int Portfolio_SlotActive(const Portfolio<F> *portfolio, int slot) {
    return (portfolio->active_bitmap >> slot) & 1;
}
//======================================================================================================
template <unsigned F> inline void Portfolio_ClearPositions(Portfolio<F> *portfolio) {
    for (int i = 0; i < 16; i++) {
        portfolio->positions[i].quantity          = FPN_Zero<F>();
        portfolio->positions[i].entry_price       = FPN_Zero<F>();
        portfolio->positions[i].entry_fee         = FPN_Zero<F>();
        portfolio->positions[i].take_profit_price = FPN_Zero<F>();
        portfolio->positions[i].stop_loss_price   = FPN_Zero<F>();
    }
    portfolio->active_bitmap = 0;
}
//======================================================================================================
template <unsigned F>
inline void Portfolio_UpdatePosition(Portfolio<F> *portfolio, int index, FPN<F> new_quantity, FPN<F> new_entry_price) {
    portfolio->positions[index].quantity    = new_quantity;
    portfolio->positions[index].entry_price = new_entry_price;
}
//======================================================================================================
// [P&L FUNCTIONS]
//======================================================================================================
// unrealized P&L: for each active position, (current_price - entry_price) * quantity
// this is the signal the controller feeds to regression - measures whether current
// gate conditions are producing positions that are making money
//======================================================================================================
template <unsigned F> inline FPN<F> Portfolio_ComputePnL(const Portfolio<F> *portfolio, FPN<F> current_price) {
    FPN<F> total = FPN_Zero<F>();
    uint16_t active  = portfolio->active_bitmap;
    while (active) {
        int idx        = __builtin_ctz(active);
        FPN<F> diff = FPN_Sub(current_price, portfolio->positions[idx].entry_price);
        FPN<F> pnl  = FPN_Mul(diff, portfolio->positions[idx].quantity);
        total           = FPN_AddSat(total, pnl);
        active &= active - 1;
    }
    return total;
}
//======================================================================================================
// total portfolio value: sum of current_price * quantity across active positions
//======================================================================================================
template <unsigned F> inline FPN<F> Portfolio_ComputeValue(const Portfolio<F> *portfolio, FPN<F> current_price) {
    FPN<F> total = FPN_Zero<F>();
    uint16_t active  = portfolio->active_bitmap;
    while (active) {
        int idx        = __builtin_ctz(active);
        FPN<F> val = FPN_Mul(current_price, portfolio->positions[idx].quantity);
        total          = FPN_AddSat(total, val);
        active &= active - 1;
    }
    return total;
}
//======================================================================================================
// [POSITION EXIT GATE - HOT PATH]
//======================================================================================================
// runs every tick - walks active bitmap, checks each position's TP/SL against current price
// branchless comparisons, writes to exit buffer using count += should_exit pattern
// clears position bit immediately on exit so next tick's gate skips it
//======================================================================================================
template <unsigned F>
inline void PositionExitGate(Portfolio<F> *portfolio, FPN<F> current_price, ExitBuffer<F> *exit_buf, uint64_t tick) {
    uint16_t active = portfolio->active_bitmap;
    while (active) {
        int idx = __builtin_ctz(active);

        // full multi-word positive-FPN comparison (MSW to LSW, short-circuit on first difference)
        // crypto prices always positive so sign check skipped
        constexpr unsigned NW = FPN<F>::N;
        const FPN<F> &tp = portfolio->positions[idx].take_profit_price;
        const FPN<F> &sl = portfolio->positions[idx].stop_loss_price;
        // hit_tp: price >= TP, hit_sl: price <= SL
        int hit_tp = 0, hit_sl = 0;
        {
          int decided_tp = 0, decided_sl = 0;
          for (int w = NW - 1; w >= 0; w--) {
            if (!decided_tp) {
              if (current_price.w[w] > tp.w[w]) { hit_tp = 1; decided_tp = 1; }
              else if (current_price.w[w] < tp.w[w]) { decided_tp = 1; }
            }
            if (!decided_sl) {
              if (current_price.w[w] < sl.w[w]) { hit_sl = 1; decided_sl = 1; }
              else if (current_price.w[w] > sl.w[w]) { decided_sl = 1; }
            }
            if (decided_tp & decided_sl) break;
          }
          // all words equal → price == TP/SL → triggers exit
          if (!decided_tp) hit_tp = 1;
          if (!decided_sl) hit_sl = 1;
        }

        // skip positions with no exit prices set (legacy adds, zero TP/SL)
        int has_exits = !FPN_IsZero(tp);
        int should_exit = (hit_tp | hit_sl) & has_exits;

        // conditional write: exits are rare (~1/1000 ticks), well-predicted branch
        // saves ~8ns/position vs unconditional 24-byte write every tick
        if (should_exit && exit_buf->count < 16) {
            ExitRecord<F> *rec = &exit_buf->records[exit_buf->count];
            rec->position_index = idx;
            rec->exit_price     = current_price;
            rec->tick            = tick;
            rec->reason          = hit_sl & (!hit_tp); // 0=TP, 1=SL (TP takes priority)
            // snapshot position data BEFORE clearing bitmap — slot may be reused
            rec->entry_price     = portfolio->positions[idx].entry_price;
            rec->quantity        = portfolio->positions[idx].quantity;
            rec->entry_fee       = portfolio->positions[idx].entry_fee;
            rec->pair_index      = portfolio->positions[idx].pair_index;
            exit_buf->count++;
            portfolio->active_bitmap &= ~(1 << idx);
        }

        active &= active - 1;
    }
}
//======================================================================================================
// [PERSISTENCE]
//======================================================================================================
// binary snapshot of portfolio state - written on slow path, read once at startup
// includes a magic number and version so we dont load garbage or stale formats
// also saves realized P&L and adaptive filter state alongside the portfolio
//
// file format:
//   [4 bytes] magic: "TICK"
//   [4 bytes] version: 1
//   [2 bytes] active_bitmap
//   [2 bytes] padding
//   [16 * sizeof(Position<F>)] positions array
//   [sizeof(FPN<F>)] realized_pnl
//   [sizeof(FPN<F>)] live_offset_pct
//   [sizeof(FPN<F>)] live_vol_mult
//   [sizeof(FPN<F>)] balance
//======================================================================================================
#define PORTFOLIO_SNAPSHOT_MAGIC 0x4B434954  // "TICK" in little-endian
#define PORTFOLIO_SNAPSHOT_VERSION 5

template <unsigned F>
static inline int Portfolio_Save(const Portfolio<F> *portfolio, FPN<F> realized_pnl,
                                  FPN<F> live_offset_pct, FPN<F> live_vol_mult,
                                  FPN<F> live_stddev_mult, FPN<F> balance,
                                  const char *filepath) {
    FILE *f = fopen(filepath, "wb");
    if (!f) {
        fprintf(stderr, "[SNAPSHOT] failed to open %s for writing\n", filepath);
        return 0;
    }

    uint32_t magic   = PORTFOLIO_SNAPSHOT_MAGIC;
    uint32_t version = PORTFOLIO_SNAPSHOT_VERSION;

    fwrite(&magic, 4, 1, f);
    fwrite(&version, 4, 1, f);
    fwrite(&portfolio->active_bitmap, 2, 1, f);
    uint16_t pad = 0;
    fwrite(&pad, 2, 1, f);
    // v5.15.5.C.4 Phase POS.2 — write only PERSIST prefix (184 bytes per position).
    // SKIP_PERSIST fields (exit_fill_price, is_maker) live in the Position struct
    // for cache locality but are NOT in the wire format. Per-position loop with
    // explicit POSITION_PERSIST_BYTES() count preserves PORTFOLIO_SNAPSHOT_VERSION=5
    // byte-identity with pre-POS.2 snapshots.
    constexpr size_t pos_persist_bytes = POSITION_PERSIST_BYTES<F>();
    for (int i = 0; i < 16; i++) {
        fwrite(&portfolio->positions[i], pos_persist_bytes, 1, f);
    }
    fwrite(&realized_pnl, sizeof(FPN<F>), 1, f);
    fwrite(&live_offset_pct, sizeof(FPN<F>), 1, f);
    fwrite(&live_vol_mult, sizeof(FPN<F>), 1, f);
    fwrite(&live_stddev_mult, sizeof(FPN<F>), 1, f);
    fwrite(&balance, sizeof(FPN<F>), 1, f);

    fflush(f);
    fclose(f);
    return 1;
}

template <unsigned F>
static inline int Portfolio_Load(Portfolio<F> *portfolio, FPN<F> *realized_pnl,
                                  FPN<F> *live_offset_pct, FPN<F> *live_vol_mult,
                                  FPN<F> *live_stddev_mult, FPN<F> *balance,
                                  const char *filepath) {
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        // no snapshot file is normal on first run
        return 0;
    }

    uint32_t magic, version;
    if (fread(&magic, 4, 1, f) != 1 || magic != PORTFOLIO_SNAPSHOT_MAGIC) {
        fprintf(stderr, "[SNAPSHOT] bad magic in %s - ignoring\n", filepath);
        fclose(f);
        return 0;
    }
    if (fread(&version, 4, 1, f) != 1 || version != PORTFOLIO_SNAPSHOT_VERSION) {
        fprintf(stderr, "[SNAPSHOT] version mismatch in %s - ignoring\n", filepath);
        fclose(f);
        return 0;
    }

    uint16_t bitmap;
    uint16_t pad;
    if (fread(&bitmap, 2, 1, f) != 1) { fclose(f); return 0; }
    if (fread(&pad, 2, 1, f) != 1) { fclose(f); return 0; }
    // v5.15.5.C.4 Phase POS.2 — read only PERSIST prefix (184 bytes per position).
    // SKIP_PERSIST fields (exit_fill_price, is_maker) are NOT in wire format;
    // they stay at default-init (zero) after load. Per-position loop matches
    // Save's PERSIST-filtered write; legacy snapshots load byte-identical.
    constexpr size_t pos_persist_bytes = POSITION_PERSIST_BYTES<F>();
    for (int i = 0; i < 16; i++) {
        if (fread(&portfolio->positions[i], pos_persist_bytes, 1, f) != 1) {
            fclose(f);
            return 0;
        }
    }

    portfolio->active_bitmap = bitmap;

    if (fread(realized_pnl, sizeof(FPN<F>), 1, f) != 1) { fclose(f); return 0; }
    if (fread(live_offset_pct, sizeof(FPN<F>), 1, f) != 1) { fclose(f); return 0; }
    if (fread(live_vol_mult, sizeof(FPN<F>), 1, f) != 1) { fclose(f); return 0; }
    if (fread(live_stddev_mult, sizeof(FPN<F>), 1, f) != 1) { fclose(f); return 0; }
    if (fread(balance, sizeof(FPN<F>), 1, f) != 1) { fclose(f); return 0; }

    fclose(f);

    int count = __builtin_popcount(bitmap);
    fprintf(stderr, "[SNAPSHOT] loaded %d positions from %s\n", count, filepath);
    return 1;
}

//======================================================================================================
//======================================================================================================
//======================================================================================================
#endif
