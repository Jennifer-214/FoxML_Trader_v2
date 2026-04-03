#pragma once
// CandleAccumulator — aggregates raw trade ticks into OHLCV candles
// fed from the engine's existing Binance websocket (no second connection)
// thread-safe: engine thread writes, GUI thread reads via snapshot
//
// port of tools/chart.py CandleAccumulator with same bucketing logic

#include <cstring>
#include <ctime>
#include <cstdint>
#include <pthread.h>
#include "../Limits.hpp"

static constexpr int CANDLE_MAX = CANDLE_HISTORY_MAX;

struct Candle {
    double time_sec;  // bucket start (unix timestamp)
    double open, high, low, close;
    double volume, buy_vol, sell_vol;
};

struct CandleAccumulator {
    int interval_sec;       // candle width (default 60)
    Candle candles[CANDLE_MAX];
    int head;               // next write position in ring buffer
    int count;              // total candles accumulated (capped at CANDLE_MAX)
    Candle current;         // candle being built (not yet in ring)
    int has_current;        // 0 until first tick arrives
    double vwap_pv;         // running price*volume sum
    double vwap_vol;        // running volume sum
    pthread_mutex_t lock;
};

static inline void CandleAccumulator_Init(CandleAccumulator *ca, int interval_sec = 60) {
    memset(ca, 0, sizeof(*ca));
    ca->interval_sec = interval_sec;
    pthread_mutex_init(&ca->lock, NULL);
}

// called from engine thread on every tick
static inline void CandleAccumulator_Push(CandleAccumulator *ca,
                                           double price, double volume, int is_seller) {
    double now = (double)time(NULL);
    double bucket = (double)((int64_t)(now / ca->interval_sec) * ca->interval_sec);

    pthread_mutex_lock(&ca->lock);

    // new candle bucket?
    if (!ca->has_current || bucket > ca->current.time_sec) {
        // flush current candle to ring buffer
        if (ca->has_current) {
            ca->candles[ca->head] = ca->current;
            ca->head = (ca->head + 1) % CANDLE_MAX;
            if (ca->count < CANDLE_MAX) ca->count++;
        }
        // start new candle
        ca->current.time_sec = bucket;
        ca->current.open = price;
        ca->current.high = price;
        ca->current.low  = price;
        ca->current.close = price;
        ca->current.volume = 0.0;
        ca->current.buy_vol = 0.0;
        ca->current.sell_vol = 0.0;
        ca->has_current = 1;
    }

    // update current candle
    ca->current.close = price;
    if (price > ca->current.high) ca->current.high = price;
    if (price < ca->current.low)  ca->current.low  = price;
    ca->current.volume += volume;
    if (is_seller)
        ca->current.sell_vol += volume;
    else
        ca->current.buy_vol += volume;

    ca->vwap_pv  += price * volume;
    ca->vwap_vol += volume;

    pthread_mutex_unlock(&ca->lock);
}

// variant for backtest replay: caller provides the tick timestamp
// instead of using wall-clock time(NULL)
static inline void CandleAccumulator_PushWithTime(CandleAccumulator *ca,
                                                    double price, double volume,
                                                    int is_seller, double tick_time_sec) {
    double bucket = (double)((int64_t)(tick_time_sec / ca->interval_sec) * ca->interval_sec);

    pthread_mutex_lock(&ca->lock);

    if (!ca->has_current || bucket > ca->current.time_sec) {
        if (ca->has_current) {
            ca->candles[ca->head] = ca->current;
            ca->head = (ca->head + 1) % CANDLE_MAX;
            if (ca->count < CANDLE_MAX) ca->count++;
        }
        ca->current.time_sec = bucket;
        ca->current.open = price;
        ca->current.high = price;
        ca->current.low  = price;
        ca->current.close = price;
        ca->current.volume = 0.0;
        ca->current.buy_vol = 0.0;
        ca->current.sell_vol = 0.0;
        ca->has_current = 1;
    }

    ca->current.close = price;
    if (price > ca->current.high) ca->current.high = price;
    if (price < ca->current.low)  ca->current.low  = price;
    ca->current.volume += volume;
    if (is_seller)
        ca->current.sell_vol += volume;
    else
        ca->current.buy_vol += volume;

    ca->vwap_pv  += price * volume;
    ca->vwap_vol += volume;

    pthread_mutex_unlock(&ca->lock);
}

// snapshot for GUI thread — copies ring buffer + current candle
struct CandleSnapshot {
    Candle candles[CANDLE_MAX + 1];  // +1 for the in-progress candle
    int count;                        // total candles in snapshot
    double vwap;
};

static inline void CandleAccumulator_Snapshot(CandleAccumulator *ca, CandleSnapshot *out) {
    pthread_mutex_lock(&ca->lock);

    // copy completed candles in chronological order
    int n = 0;
    if (ca->count > 0) {
        int start = (ca->count >= CANDLE_MAX)
            ? ca->head   // ring buffer full — head is the oldest
            : 0;         // ring buffer not full — oldest is at 0
        int total = (ca->count >= CANDLE_MAX) ? CANDLE_MAX : ca->count;
        for (int i = 0; i < total; i++) {
            out->candles[n++] = ca->candles[(start + i) % CANDLE_MAX];
        }
    }
    // append current in-progress candle
    if (ca->has_current) {
        out->candles[n++] = ca->current;
    }
    out->count = n;
    out->vwap = (ca->vwap_vol > 0.0) ? ca->vwap_pv / ca->vwap_vol : 0.0;

    pthread_mutex_unlock(&ca->lock);
}

// reset accumulator with new interval (clears all candle data)
static inline void CandleAccumulator_SetInterval(CandleAccumulator *ca, int interval_sec) {
    pthread_mutex_lock(&ca->lock);
    ca->interval_sec = interval_sec;
    ca->head = 0;
    ca->count = 0;
    ca->has_current = 0;
    ca->vwap_pv = 0;
    ca->vwap_vol = 0;
    pthread_mutex_unlock(&ca->lock);
}

static inline void CandleAccumulator_Destroy(CandleAccumulator *ca) {
    pthread_mutex_destroy(&ca->lock);
}
