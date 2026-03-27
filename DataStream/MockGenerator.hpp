// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [MOCK DATA GENERATOR]
//======================================================================================================
// generates fake market data ticks as FIX messages for testing the full pipeline
// uses a simple LCG random number generator so its deterministic given a seed - same seed same
// price series every time, which makes debugging way easier than actual random data
//
// the price model is a random walk with drift: each tick the price moves by a small random amount
// biased slightly upward or downward depending on the trend parameter, volume is randomized
// independently with a base level and random spikes
//======================================================================================================
#ifndef MOCK_GENERATOR_HPP
#define MOCK_GENERATOR_HPP

#include "FauxFIX.hpp"

//======================================================================================================
// [LCG RANDOM]
//======================================================================================================
// linear congruential generator - fast, deterministic, not cryptographic but we dont need that
// constants from Numerical Recipes, period is 2^64
//======================================================================================================
struct MockRNG {
    uint64_t state;
};

static inline void MockRNG_Seed(MockRNG *rng, uint64_t seed) {
    rng->state = seed;
}

// returns a pseudo-random uint64_t
static inline uint64_t MockRNG_Next(MockRNG *rng) {
    rng->state = rng->state * 6364136223846793005ULL + 1442695040888963407ULL;
    return rng->state;
}

// returns a double in [0.0, 1.0)
static inline double MockRNG_Double(MockRNG *rng) {
    return (double)(MockRNG_Next(rng) >> 11) / (double)(1ULL << 53);
}

// returns a double in [lo, hi)
static inline double MockRNG_Range(MockRNG *rng, double lo, double hi) {
    return lo + MockRNG_Double(rng) * (hi - lo);
}

//======================================================================================================
// [GENERATOR CONFIG]
//======================================================================================================
struct MockGeneratorConfig {
    double start_price;     // initial price (e.g. 150.0)
    double volatility;      // per-tick price movement scale (e.g. 0.5 means +/-$0.50)
    double drift;           // bias per tick (e.g. 0.01 for slight uptrend, -0.01 for downtrend)
    double base_volume;     // average volume per tick (e.g. 1000.0)
    double volume_spike;    // max random volume spike multiplier (e.g. 3.0 means up to 3x base)
    double min_price;       // floor price, wont go below this (e.g. 1.0)
    const char *symbol;     // ticker symbol (e.g. "AAPL")
    uint64_t seed;          // RNG seed for reproducibility
};

//======================================================================================================
// [GENERATOR STATE]
//======================================================================================================
struct MockGenerator {
    MockGeneratorConfig config;
    MockRNG rng;
    double current_price;
    uint32_t seq_num;
};

static inline void MockGenerator_Init(MockGenerator *gen, MockGeneratorConfig config) {
    gen->config = config;
    MockRNG_Seed(&gen->rng, config.seed);
    gen->current_price = config.start_price;
    gen->seq_num = 1;
}

//======================================================================================================
// [GENERATE NEXT TICK]
//======================================================================================================
// advances the price by one random step and builds a FIX message for it
// returns the length written to buf, and fills out the parsed message for convenience
//======================================================================================================
static inline int MockGenerator_NextTick(MockGenerator *gen, char *buf, int buf_size, FIX_ParsedMessage *parsed_out) {
    // random walk step: drift + random noise centered around 0
    double noise = MockRNG_Range(&gen->rng, -gen->config.volatility, gen->config.volatility);
    gen->current_price += gen->config.drift + noise;

    // floor clamp
    if (gen->current_price < gen->config.min_price)
        gen->current_price = gen->config.min_price;

    // random volume with occasional spikes
    double vol_mult = 1.0 + MockRNG_Double(&gen->rng) * (gen->config.volume_spike - 1.0);
    double volume = gen->config.base_volume * vol_mult;

    // build the FIX message
    int len = FIX_BuildMarketDataMsg(buf, buf_size,
                                      gen->seq_num, gen->config.symbol,
                                      2, // entry_type = trade
                                      gen->current_price, volume);
    gen->seq_num++;

    // parse it back so the caller gets both the raw message and the parsed struct
    if (parsed_out) {
        *parsed_out = FIX_Parse(buf, len);
    }

    return len;
}

//======================================================================================================
// [GENERATE BATCH]
//======================================================================================================
// generates count ticks into an array of parsed messages, useful for filling regression buffers
// buf is scratch space for building FIX messages (reused each tick)
//======================================================================================================
static inline void MockGenerator_Batch(MockGenerator *gen, FIX_ParsedMessage *messages, int count,
                                        char *scratch_buf, int scratch_size) {
    for (int i = 0; i < count; i++) {
        MockGenerator_NextTick(gen, scratch_buf, scratch_size, &messages[i]);
    }
}

//======================================================================================================
//======================================================================================================
#endif // MOCK_GENERATOR_HPP
