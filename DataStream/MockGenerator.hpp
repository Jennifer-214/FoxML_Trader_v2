// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[DataStream/MockGenerator.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [BACKTEST] [DETERMINISM]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[deterministic mock tick generator (LCG random walk + drift -> FauxFIX messages) — test substrate; consumed by tests/test_common.hpp + tests/integration_test.cpp]
// [CONTAINS]
//   - [FUNCTION]_[MockRNG_Next]   (MockRNG struct + Seed / Double / Range ride)
//   - [STRUCT]_[MockGenerator]   (MockGeneratorConfig + Init ride)
//   - [FUNCTION]_[MockGenerator_NextTick]   (+ Batch rides)
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

//======================================================================
// [FUNCTION]_[MockRNG_Next]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [DETERMINISM]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[LCG random family (MockRNG struct + Seed / Double / Range ride) — Numerical Recipes constants, period 2^64; deterministic given a seed]
//======================================================================
// [CODE]
//======================================================================
// linear congruential generator - fast, deterministic, not cryptographic but we dont need that
// constants from Numerical Recipes, period is 2^64
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
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[MockRNG_Next]
//======================================================================

//======================================================================
// [STRUCT]_[MockGeneratorConfig]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the mock price-feed knobs — start price / volatility / drift / base volume + spike / floor price / symbol / RNG seed]
//======================================================================
// [CODE]
//======================================================================
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
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
// [SIZE]_[64B]
// [ALIGN]_[8]
// [CACHE_LINES]_[1]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[MockGeneratorConfig]
//======================================================================

//======================================================================
// [STRUCT]_[MockGenerator]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[generator state — config + RNG + current price + FIX seq_num (Init rides)]
//======================================================================
// [CODE]
//======================================================================

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
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
// [SIZE]_[88B]
// [ALIGN]_[8]
// [CACHE_LINES]_[2]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[MockGenerator]
//======================================================================

//======================================================================
// [FUNCTION]_[MockGenerator_NextTick]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BACKTEST] [DETERMINISM]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[advance the walk one step and build + parse-back a FIX message (Batch rides) — drift + noise, floor clamp, randomized volume with spikes]
//======================================================================
// [CODE]
//======================================================================
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

// generates count ticks into an array of parsed messages, useful for filling regression buffers
// buf is scratch space for building FIX messages (reused each tick)
static inline void MockGenerator_Batch(MockGenerator *gen, FIX_ParsedMessage *messages, int count,
                                        char *scratch_buf, int scratch_size) {
    for (int i = 0; i < count; i++) {
        MockGenerator_NextTick(gen, scratch_buf, scratch_size, &messages[i]);
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[MockGenerator_NextTick]
//======================================================================
#endif // MOCK_GENERATOR_HPP
