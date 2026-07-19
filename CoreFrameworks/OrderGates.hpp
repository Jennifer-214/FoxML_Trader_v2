// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[CoreFrameworks/OrderGates.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [DATA_PLANE] [CAPITAL_BEARING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the v1 gate apparatus — DataStream parse record + branchless gate-zero helpers + legacy buy/sell gates]
// [CONTAINS]
//   - [STRUCT]_[DataStream]
//   - [FUNCTION]_[BuyGate]
//   - [FUNCTION]_[SellGate]
//======================================================================================================
// [EDIT]_[[19-03-26 09:09pm]]
// v1 has like this thing where the gate adjustment uses the innger regression lose, rather than th ROR header information, like just direct pnl trend, the ROR is still computed and stored but its not using this right now, because it was easier to set up the other thing first
//======================================================================================================
#ifndef ORDER_GATES_H
#define ORDER_GATES_H

#include <stdint.h>
#include "../MemHeaders/PoolAllocator.hpp"
#include "../FixedPoint/FixedPointN.hpp"
//------------------------------------------------------------------------------------------------------
// [SECTION]_[structs]
//------------------------------------------------------------------------------------------------------
// FPN_Binary throughout - no float-to-int conversion boundaries, no precision surprises
// Packed gate trick is dropped - FPN_Binary comparisons are already branchless so packing buys nothing
//------------------------------------------------------------------------------------------------------
// [EDIT]_[[16-03-26 12:08pm]]
// i need to make a feature store thats branchless probably unless the relationships between price and volume can be extrapolated to the actual raw data, otherwise the features pribably need to be branchless as well, to reduce inference time, those would stack up fast having mispredcitons for every single tick
//======================================================================================================

//======================================================================
// [STRUCT]_[DataStream]
//----------------------------------------------------------------------
// [TAG]_[[DATA_PLANE] [HOT_PATH] [DECIMAL]]
// [THREAD]_[[PRODUCER_WRITER]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the per-tick parse record — DECIMAL Money price/volume + aggressor flag + display-only doubles]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F> struct DataStream {
    Money price;
    Money volume;
    int is_buyer_maker; // 1 = buyer was maker (seller-initiated), 0 = buyer was taker (buyer-initiated)
    int _pad0;          // align doubles to 8 bytes
    double price_d;     // stashed from parse for TUI display (no FPN_ToDouble on hot path)
    double volume_d;
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
//----------------------------------------------------------------------
// [SIZE]_[64B]
// [ALIGN]_[16]
// [CACHE_LINES]_[1]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[DataStream]
//======================================================================

template <unsigned F> struct ProfitTarget {
    FPN_Binary<F> profit_target;
};

//======================================================================
// [STRUCT]_[BuySideGateConditions]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [HOT_PATH] [BINARY_FP]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the buy-gate inputs — price + volume conditions + gate_direction (0=buy-below/mean-reversion, 1=buy-above/momentum); Gate_Zero branchlessly masks .price on a failed gate]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F> struct BuySideGateConditions {
    FPN_Binary<F> price;
    FPN_Binary<F> volume;
    int gate_direction = 0;  // 0 = buy below price (mean reversion), 1 = buy above price (momentum)
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
// [SIZE]_[48B]
// [ALIGN]_[16]
// [CACHE_LINES]_[1]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[BuySideGateConditions]
//======================================================================

template <unsigned F> struct SellSideGateConditions {
    FPN_Binary<F> price;
    FPN_Binary<F> volume;
};

//------------------------------------------------------------------------------------------------------
// [SECTION]_[gate helpers]
//------------------------------------------------------------------------------------------------------
// branchless gate: zeros buy conditions when gate fails (pass=0)
// replaces the 5-line mask pattern used across all strategy buy signals
//------------------------------------------------------------------------------------------------------
// Gate_Zero: zeros price condition when gate fails (pass=0)
// volume is left intact — the BuyGate checks price first, so zeroed price blocks fills.
// keeping volume lets the TUI display the actual volume threshold for diagnostics.
template <unsigned F>
inline void Gate_Zero(BuySideGateConditions<F> *conds, int pass) {
    unsigned __int128 mask = -(unsigned __int128)(unsigned)pass;   // 16B: mask the whole .v (no word loop / sign field)
    conds->price.v &= (__int128)mask;
}

// Gate_ZeroAll: zeros both price AND volume (used by momentum where both must block)
template <unsigned F>
inline void Gate_ZeroAll(BuySideGateConditions<F> *conds, int pass) {
    unsigned __int128 mask = -(unsigned __int128)(unsigned)pass;   // 16B: mask the whole .v (no word loop / sign field)
    conds->price.v  &= (__int128)mask;
    conds->volume.v &= (__int128)mask;
}

//------------------------------------------------------------------------------------------------------
// [SECTION]_[order gates]
//------------------------------------------------------------------------------------------------------
// no more packing/unpacking - compare FPN_Binary fields directly (already branchless)

//======================================================================================================
// [EDIT]_[[16-03-26 08:55am]]
// im not sure if im gonna add the outputs to a portfolio management system here or not, we'll see, im not even sure if im actually gonna put out anything thats actually useable for like large scale or systemic trading, like sure, for individuals using it yeah, thats not ahuge concern, but something that someone could pick up and start a hedge fund from idk, well See,
//
// anyways, i was saying im not sure if ill add like order tracking here or not, like it has tracking, but what i mean is like, viewing through a UI or something or in a .parquet file or something, like a human readable record, that may be a speerate header file
//======================================================================================================
// [EDIT]_[[16-03-26 02:39pm]]
// these probably need to be reworked to actually allow different strategies and stuff, maybe passing and array or struct with multiple different strategies packed would work, or a single core per strategy idk, because at most this current set up is simply a buy when conditions are below or at a set point, and sell when above or meet a set point, so really just an extremely basic strategy
//======================================================================================================
// [EDIT]_[[16-03-26 11:31pm]]
// so im probably gonna keep this as a struct to import conditions to actually trigger placing and selling orders, after thinking about it some more, it would probably be best to create like a standardized struct for a model to pass outputs to as a conditional price and volume threshhold, like, not the current way where its buy when below x value, but something thats more like a gradient or something, it probably wont be a rework of the Condition structs, probably more like code thats within the actual gates that parses the data stream and finds favorable conditions based on the target profit and Condition structs, this only works because the features and stuff are just about optimizing the relationships between raw inputs and target conditions, its just an easier way for the model to learn patters, so based on that it can essentially be boiled down to something as simple as raw OHCLV inference, but that would be handled by a watcher header file, and that would set the conditions, so maybe strict conditions are the correct approach, idk more testing is needed, this will probably be lke the watcher header or module analyzes the data stream, and protfolio performance and dynamically updates as needed based on current microstructure trends, because the inference for parsing raw ohclv and making decisions at run time is way too heavy of a compute cost, so this is probably the correct way, like if drawdone exceeds x% over y time, then update conditions to z0 and z1 price and volume as a basic sketched out idea, im pretty sure i referenced this ina  different file, but it never hurts to rethink through the actual architectural decisions and overall design, because when i said it there i was probably thinking about making it a main function within the same file, but it should probably run on a seperate core, or ideally a seperate server, and the decisions and conditions should be sent over netwrok, idk, i have no formal training or mentoring or whatever so i could be wrong and there are probably better ways of doing this, idk, keeping the gates this simple is still probably a better idea, because it reduces hotpath cycle counts heavily
//======================================================================================================

//======================================================================
// [FUNCTION]_[BuyGate]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [CAPITAL_BEARING] [SUPPORTIVE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[v1 buy gate — branchless threshold compare + pool slot claim; the sharded path uses the inlined BG in ExecutionCore instead]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F> inline void BuyGate(const BuySideGateConditions<F> *conditions, const DataStream<F> *stream, OrderPool<F> *pool) {
    // inline positive-FPN_Binary comparisons: skip sign machinery (crypto prices always positive)
    // same pattern as PositionExitGate — saves ~70ns vs FPN_LessThanOrEqual
    // 16B two's-comp: crypto price/volume are always >= 0 and << 2^127, so .v compares NATIVELY — value-
    // equivalent to the old 2-word unsigned magnitude compare on non-negative values, and branchless (a single
    // __int128 cmp → setcc, no jump; also faster than the 2-word logic this replaces).
    // Ship-B P2b: the legacy gate apparatus stays FEATURE-domain (binary thresholds,
    // matching the ema/danger chain) — the money tick crosses ONCE here per eval.
    const FPN_Binary<F> price_b  = Money_ToBinary(stream->price);
    const FPN_Binary<F> volume_b = Money_ToBinary(stream->volume);
    int below = (conditions->price.v >= price_b.v);   // below: price <= gate  (gate >= price)
    int above = (price_b.v >= conditions->price.v);   // above: price >= gate

    int price_pass  = (below & !conditions->gate_direction) | (above & conditions->gate_direction);

    int volume_pass = (volume_b.v >= conditions->volume.v);   // volume: stream volume >= threshold

    int pass = price_pass & volume_pass;

    // conditional write: fills are rare (~1/1000 ticks), branch predictor handles this
    // better than unconditional 48-byte write every tick
    if (pass) {
        uint64_t free_mask = ~pool->bitmap;
        if (!free_mask) return; // pool full — drop order
        uint32_t index = __builtin_ctzll(free_mask);
        pool->bitmap |= (1ULL << index);
        pool->slots[index].price    = stream->price;
        pool->slots[index].quantity = stream->volume;
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[BuyGate]
//======================================================================

//======================================================================
// [FUNCTION]_[SellGate]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [CAPITAL_BEARING] [SUPPORTIVE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[v1 sell gate — bitmap walk of active pool slots, exit on profit-target hit; sharded path uses inlined SG instead]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline void SellGate(const SellSideGateConditions<F> *conditions, const DataStream<F> *stream, OrderPool<F> *pool,
                     const ProfitTarget<F> *profit_target) {
    int price_pass  = FPN_GreaterThanOrEqual(stream->price, conditions->price);
    int volume_pass = FPN_LessThanOrEqual(stream->volume, conditions->volume);

    int pass = price_pass & volume_pass;

    uint64_t active = pool->bitmap;
    while (active) {
        uint32_t idx            = __builtin_ctzll(active);
        FPN_Binary<F> entry_price  = pool->slots[idx].price;
        FPN_Binary<F> target_price = FPN_AddSat(entry_price, profit_target->profit_target);
        int exit_pass           = FPN_GreaterThanOrEqual(stream->price, target_price);
        uint64_t clear_mask     = (uint64_t)(-(int64_t)exit_pass) & (1ULL << idx);
        pool->bitmap &= ~clear_mask;
        active &= active - 1;
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[SellGate]
//======================================================================
#endif // ORDER_GATES_H
