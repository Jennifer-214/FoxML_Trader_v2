// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [PORTFOLIO MANAGER]
//======================================================================================================
// this is basically just gonna track positions and stuff and be the core portfolio managment system, im not sure if ill actually add the rebaalncing logic and stuff here, but it should eventually just serve as the API call to get position deltas and stuff, it will be more robust that just the simple pool allocator i was attempting earlier
//======================================================================================================
// [INCLUDE]
//======================================================================================================
#ifndef PORTFOLIO_HPP
#define PORTFOLIO_HPP

#include "../FixedPoint/FixedPointN.hpp"
#include <stdio.h>
//======================================================================================================
// [STRUCTS]
//======================================================================================================
// Im not sure how many positions i really want to track here but for now im just gonna leave it at like 16 i think, there will probably be more advanced logic added later to have a model that watches performace and dynamically updates or something like i attempted to do in FoxML core, but this is a deepr dive so i can actually learn and understand the logic behind stuff, and i just think its cool as shit, like why learn java when stuff lke this exists lmao, also i get to make my own library so im not functioning off blackbox implementations where the end of the documentation is lke "Trust me bro", and i hate reading documentation, so id rather build my own
//======================================================================================================
template <unsigned F> struct Position {
    FPN<F> quantity;          // positive for long, negative for short
    FPN<F> entry_price;
    FPN<F> take_profit_price; // LIVE — modified by trailing TP on slow path
    FPN<F> stop_loss_price;   // LIVE — modified by trailing SL on slow path
    FPN<F> original_tp;       // set at fill, never modified — used to detect "running" positions
    FPN<F> original_sl;       // set at fill, never modified — baseline for trailing SL
};
static_assert(sizeof(Position<64>) == 6 * sizeof(FPN<64>), "Position size mismatch");
//======================================================================================================
// [PORTFOLIO]
//======================================================================================================
// bitmap-based like OrderPool - same __builtin_ctz pattern, no array shifting on removal,
// hot-path exit gate only walks set bits so cleared positions are skipped automatically
//======================================================================================================
template <unsigned F> struct Portfolio {
    Position<F> positions[16];
    uint16_t active_bitmap; // one bit per slot, same pattern as OrderPool
};
// sizeof includes padding after uint16_t to align the struct - thats fine, the bitmap is what matters
//======================================================================================================
// [EXIT STRUCTS]
//======================================================================================================
// ExitRecord stores position index for O(1) lookup - the position data stays in the slot
// after the bit is cleared, so the controller can read entry_price/quantity directly
//======================================================================================================
template <unsigned F> struct ExitRecord {
    uint32_t position_index;
    FPN<F> exit_price;
    uint64_t tick;
    int reason; // 0 = take profit, 1 = stop loss
};

template <unsigned F> struct ExitBuffer {
    ExitRecord<F> records[16];
    uint32_t count;
};

template <unsigned F> inline void ExitBuffer_Init(ExitBuffer<F> *buf) {
    buf->count = 0;
}

template <unsigned F> inline void ExitBuffer_Clear(ExitBuffer<F> *buf) {
    buf->count = 0;
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
                                          FPN<F> take_profit_price, FPN<F> stop_loss_price) {
    if (portfolio->active_bitmap == 0xFFFF) return -1;
    int idx                                       = __builtin_ctz(~portfolio->active_bitmap);
    portfolio->positions[idx].quantity             = quantity;
    portfolio->positions[idx].entry_price          = entry_price;
    portfolio->positions[idx].take_profit_price    = take_profit_price;
    portfolio->positions[idx].stop_loss_price      = stop_loss_price;
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
template <unsigned F> inline void Portfolio_ClearPositions(Portfolio<F> *portfolio) {
    for (int i = 0; i < 16; i++) {
        portfolio->positions[i].quantity          = FPN_Zero<F>();
        portfolio->positions[i].entry_price       = FPN_Zero<F>();
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

        // inline positive-FPN comparison: skip sign check (crypto prices always positive)
        // compares raw words directly — saves ~7ns per comparison vs FPN_GreaterThanOrEqual
        constexpr unsigned NW = FPN<F>::N;
        const FPN<F> &tp = portfolio->positions[idx].take_profit_price;
        const FPN<F> &sl = portfolio->positions[idx].stop_loss_price;
        int hit_tp = (current_price.w[NW-1] > tp.w[NW-1]) ||
                     (current_price.w[NW-1] == tp.w[NW-1] && current_price.w[0] >= tp.w[0]);
        int hit_sl = (current_price.w[NW-1] < sl.w[NW-1]) ||
                     (current_price.w[NW-1] == sl.w[NW-1] && current_price.w[0] <= sl.w[0]);

        // skip positions with no exit prices set (legacy adds, zero TP/SL)
        int has_exits = (tp.w[NW-1] | tp.w[0]) != 0; // faster than FPN_IsZero (no sign check, normalized to 0/1)
        int should_exit = (hit_tp | hit_sl) & has_exits;

        // conditional write: exits are rare (~1/1000 ticks), well-predicted branch
        // saves ~8ns/position vs unconditional 24-byte write every tick
        if (should_exit && exit_buf->count < 16) {
            exit_buf->records[exit_buf->count].position_index = idx;
            exit_buf->records[exit_buf->count].exit_price     = current_price;
            exit_buf->records[exit_buf->count].tick            = tick;
            exit_buf->records[exit_buf->count].reason          = hit_sl & (!hit_tp); // 0=TP, 1=SL (TP takes priority)
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
#define PORTFOLIO_SNAPSHOT_VERSION 4

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
    fwrite(portfolio->positions, sizeof(Position<F>), 16, f);
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
    if (fread(portfolio->positions, sizeof(Position<F>), 16, f) != 16) { fclose(f); return 0; }

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
