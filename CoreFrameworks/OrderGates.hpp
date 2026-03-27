// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [Main Logic]
//======================================================================================================
// [EDIT [19-03-26 09:09pm]]
//======================================================================================================
// v1 has like this thing where the gate adjustment uses the innger regression lose, rather than th ROR header information, like just direct pnl trend, the ROR is still computed and stored but its not using this right now, because it was easier to set up the other thing first
//======================================================================================================
#ifndef ORDER_GATES_H
#define ORDER_GATES_H

#include <stdint.h>
#include "../MemHeaders/PoolAllocator.hpp"
#include "../FixedPoint/FixedPointN.hpp"
//======================================================================================================
// [STRUCTS]
//======================================================================================================
// FPN throughout - no float-to-int conversion boundaries, no precision surprises
// Packed gate trick is dropped - FPN comparisons are already branchless so packing buys nothing
//======================================================================================================
// [EDIT [16-03-26 12:08pm]]
//======================================================================================================
//i need to make a feature store thats branchless probably unless the relationships between price and volume can be extrapolated to the actual raw data, otherwise the features pribably need to be branchless as well, to reduce inference time, those would stack up fast having mispredcitons for every single tick
//======================================================================================================
template <unsigned F> struct DataStream {
    FPN<F> price;
    FPN<F> volume;
    double price_d;    // stashed from parse for TUI display (no FPN_ToDouble on hot path)
    double volume_d;
};

template <unsigned F> struct ProfitTarget {
    FPN<F> profit_target;
};

template <unsigned F> struct BuySideGateConditions {
    FPN<F> price;
    FPN<F> volume;
    int gate_direction = 0;  // 0 = buy below price (mean reversion), 1 = buy above price (momentum)
};

template <unsigned F> struct SellSideGateConditions {
    FPN<F> price;
    FPN<F> volume;
};

//======================================================================================================
//[ORDER GATES]
//======================================================================================================
// no more packing/unpacking - compare FPN fields directly (already branchless)
//======================================================================================================
// [EDIT [16-03-26 08:55am]]
//======================================================================================================
// im not sure if im gonna add the outputs to a portfolio management system here or not, we'll see, im not even sure if im actually gonna put out anything thats actually useable for like large scale or systemic trading, like sure, for individuals using it yeah, thats not ahuge concern, but something that someone could pick up and start a hedge fund from idk, well See,
//
// anyways, i was saying im not sure if ill add like order tracking here or not, like it has tracking, but what i mean is like, viewing through a UI or something or in a .parquet file or something, like a human readable record, that may be a speerate header file
//======================================================================================================
// [EDIT [16-03-26 02:39pm]]
//======================================================================================================
// these probably need to be reworked to actually allow different strategies and stuff, maybe passing and array or struct with multiple different strategies packed would work, or a single core per strategy idk, because at most this current set up is simply a buy when conditions are below or at a set point, and sell when above or meet a set point, so really just an extremely basic strategy
//======================================================================================================
// [EDIT [16-03-26 11:31pm]]
//======================================================================================================
// so im probably gonna keep this as a struct to import conditions to actually trigger placing and selling orders, after thinking about it some more, it would probably be best to create like a standardized struct for a model to pass outputs to as a conditional price and volume threshhold, like, not the current way where its buy when below x value, but something thats more like a gradient or something, it probably wont be a rework of the Condition structs, probably more like code thats within the actual gates that parses the data stream and finds favorable conditions based on the target profit and Condition structs, this only works because the features and stuff are just about optimizing the relationships between raw inputs and target conditions, its just an easier way for the model to learn patters, so based on that it can essentially be boiled down to something as simple as raw OHCLV inference, but that would be handled by a watcher header file, and that would set the conditions, so maybe strict conditions are the correct approach, idk more testing is needed, this will probably be lke the watcher header or module analyzes the data stream, and protfolio performance and dynamically updates as needed based on current microstructure trends, because the inference for parsing raw ohclv and making decisions at run time is way too heavy of a compute cost, so this is probably the correct way, like if drawdone exceeds x% over y time, then update conditions to z0 and z1 price and volume as a basic sketched out idea, im pretty sure i referenced this ina  different file, but it never hurts to rethink through the actual architectural decisions and overall design, because when i said it there i was probably thinking about making it a main function within the same file, but it should probably run on a seperate core, or ideally a seperate server, and the decisions and conditions should be sent over netwrok, idk, i have no formal training or mentoring or whatever so i could be wrong and there are probably better ways of doing this, idk, keeping the gates this simple is still probably a better idea, because it reduces hotpath cycle counts heavily
//======================================================================================================
template <unsigned F> inline void BuyGate(const BuySideGateConditions<F> *conditions, const DataStream<F> *stream, OrderPool<F> *pool) {
    // direction-aware price gate: branchless select between buy-below (MR) and buy-above (momentum)
    int below = FPN_LessThanOrEqual(stream->price, conditions->price);
    int above = FPN_GreaterThanOrEqual(stream->price, conditions->price);
    int price_pass  = (below & !conditions->gate_direction) | (above & conditions->gate_direction);
    int volume_pass = FPN_GreaterThanOrEqual(stream->volume, conditions->volume);

    int pass = price_pass & volume_pass;

    // conditional write: fills are rare (~1/1000 ticks), branch predictor handles this
    // better than unconditional 48-byte write every tick
    if (pass) {
        uint32_t index = __builtin_ctzll(~pool->bitmap);
        pool->bitmap |= (1ULL << index);
        pool->slots[index].price    = stream->price;
        pool->slots[index].quantity = stream->volume;
    }
}

template <unsigned F>
inline void SellGate(const SellSideGateConditions<F> *conditions, const DataStream<F> *stream, OrderPool<F> *pool,
                     const ProfitTarget<F> *profit_target) {
    int price_pass  = FPN_GreaterThanOrEqual(stream->price, conditions->price);
    int volume_pass = FPN_LessThanOrEqual(stream->volume, conditions->volume);

    int pass = price_pass & volume_pass;

    uint64_t active = pool->bitmap;
    while (active) {
        uint32_t idx            = __builtin_ctzll(active);
        FPN<F> entry_price  = pool->slots[idx].price;
        FPN<F> target_price = FPN_AddSat(entry_price, profit_target->profit_target);
        int exit_pass           = FPN_GreaterThanOrEqual(stream->price, target_price);
        uint64_t clear_mask     = (uint64_t)(-(int64_t)exit_pass) & (1ULL << idx);
        pool->bitmap &= ~clear_mask;
        active &= active - 1;
    }
}
//======================================================================================================
#endif // ORDER_GATES_H
