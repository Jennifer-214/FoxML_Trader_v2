// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [TRADE LOG]
//======================================================================================================
// simple CSV append logger for recording buy/sell activity with gate conditions at time of fill
// filename is {symbol}_order_history.csv so the symbol is implicit, no symbol column needed
// fflush after every write so nothing is lost if the program crashes
//======================================================================================================
#ifndef TRADE_LOG_HPP
#define TRADE_LOG_HPP

#include "../Strategies/StrategyInterface.hpp"

// string helpers for CSV output (also used by MetricsLog)
static inline const char *_strategy_str(int sid) {
    return (sid == STRATEGY_MOMENTUM) ? "MOMENTUM" : "MEAN_REVERSION";
}
static inline const char *_regime_str(int rid) {
    switch (rid) { case 1: return "TRENDING"; case 2: return "VOLATILE"; case 3: return "TRENDING_DOWN"; default: return "RANGING"; }
}

#include <stdio.h>
#include <stdint.h>

//======================================================================================================
// [STRUCT]
//======================================================================================================
struct TradeLog {
    FILE *file;
    uint64_t trade_count;
};

struct TradeLogRecord {
    uint64_t tick;
    double price, quantity, entry_price, delta_pct;
    double tp, sl, buy_cond_p, buy_cond_v;
    double stddev, avg, balance, fee_cost;
    double spacing, gate_dist_pct;
    int is_buy;          // 1=BUY, 0=SELL
    int strategy_id;     // which strategy entered/exited
    int regime;          // regime at time of trade
    char reason[16];     // "TP", "SL", "TIME", "SESSION_CLOSE"
};

//======================================================================================================
// [FUNCTIONS]
//======================================================================================================
static inline int TradeLog_Init(TradeLog *log, const char *symbol) {
    char filename[128];
    int pos = 0;
    while (*symbol && pos < 119) {
        filename[pos++] = *symbol++;
    }
    const char *suffix = "_order_history.csv";
    while (*suffix && pos < 127) {
        filename[pos++] = *suffix++;
    }
    filename[pos] = '\0';

    // check if file already has content (so we don't double-write the header)
    log->file = fopen(filename, "r");
    int has_content = 0;
    if (log->file) {
        has_content = (fgetc(log->file) != EOF);
        fclose(log->file);
    }

    log->file = fopen(filename, "a");
    if (!log->file) return 0;

    log->trade_count = 0;

    if (!has_content) {
        fprintf(log->file, "tick,side,price,quantity,entry_price,delta_pct,exit_reason,"
                "buy_price_cond,buy_vol_cond,take_profit,stop_loss,"
                "stddev,avg,balance,fee_cost,spacing,gate_dist_pct,strategy,regime\n");
        fflush(log->file);
    }

    return 1;
}
//======================================================================================================
static inline void TradeLog_Buy(TradeLog *log, TradeLogRecord *r) {
    if (!log->file) return;
    fprintf(log->file, "%lu,BUY,%.4f,%.6f,,,,"
            "%.4f,%.4f,%.4f,%.4f,"
            "%.4f,%.2f,%.4f,%.4f,%.2f,%.4f,%s,%s\n",
            (unsigned long)r->tick, r->price, r->quantity,
            r->buy_cond_p, r->buy_cond_v, r->tp, r->sl,
            r->stddev, r->avg, r->balance, r->fee_cost, r->spacing, r->gate_dist_pct,
            _strategy_str(r->strategy_id), _regime_str(r->regime));
    fflush(log->file);
    log->trade_count++;
}
//======================================================================================================
static inline void TradeLog_Sell(TradeLog *log, TradeLogRecord *r) {
    if (!log->file) return;
    fprintf(log->file, "%lu,SELL,%.4f,%.6f,%.4f,%.4f,%s,"
            ",,,,,"
            ",,,%.4f,%.4f,,,%s,%s\n",
            (unsigned long)r->tick, r->price, r->quantity, r->entry_price, r->delta_pct, r->reason,
            r->balance, r->fee_cost,
            _strategy_str(r->strategy_id), _regime_str(r->regime));
    fflush(log->file);
    log->trade_count++;
}
//======================================================================================================
static inline void TradeLog_Close(TradeLog *log) {
    if (log->file) {
        fclose(log->file);
        log->file = 0;
    }
}
//======================================================================================================
// [BUFFERED TRADE LOG]
//======================================================================================================
// hot-path version: accumulates trade records in a ring buffer (~10ns per push).
// slow-path drains the buffer to the CSV file (fprintf happens off the hot path).
// NOTE: if engine crashes between fill and next slow-path drain, CSV entry is lost
// (but position is preserved in binary snapshot).
//======================================================================================================
#define TRADE_LOG_BUF_SIZE 64

struct TradeLogBuffer {
    TradeLogRecord records[TRADE_LOG_BUF_SIZE];
    uint32_t head;
    uint32_t count;
};

static inline void TradeLogBuffer_Init(TradeLogBuffer *buf) {
    buf->head = 0;
    buf->count = 0;
}

// hot path: push a record to the ring buffer (~10ns, no file I/O)
static inline void TradeLogBuffer_PushBuy(TradeLogBuffer *buf, uint64_t tick,
                                           double price, double qty,
                                           double tp, double sl,
                                           double bc_p, double bc_v,
                                           double stddev, double avg, double balance,
                                           double fee_cost, double spacing, double gate_dist_pct,
                                           int strategy_id, int regime) {
    if (buf->count >= TRADE_LOG_BUF_SIZE) return; // overflow safety
    TradeLogRecord *r = &buf->records[buf->head];
    r->tick = tick; r->price = price; r->quantity = qty;
    r->tp = tp; r->sl = sl; r->buy_cond_p = bc_p; r->buy_cond_v = bc_v;
    r->stddev = stddev; r->avg = avg; r->balance = balance;
    r->fee_cost = fee_cost; r->spacing = spacing; r->gate_dist_pct = gate_dist_pct;
    r->is_buy = 1; r->strategy_id = strategy_id; r->regime = regime;
    r->entry_price = 0; r->delta_pct = 0; r->reason[0] = '\0';
    buf->head = (buf->head + 1) & (TRADE_LOG_BUF_SIZE - 1);
    buf->count++;
}

static inline void TradeLogBuffer_PushSell(TradeLogBuffer *buf, uint64_t tick,
                                            double price, double qty,
                                            double entry_price, double delta_pct,
                                            const char *reason,
                                            double balance, double fee_cost,
                                            int strategy_id, int regime) {
    if (buf->count >= TRADE_LOG_BUF_SIZE) return;
    TradeLogRecord *r = &buf->records[buf->head];
    r->tick = tick; r->price = price; r->quantity = qty;
    r->entry_price = entry_price; r->delta_pct = delta_pct;
    r->is_buy = 0; r->strategy_id = strategy_id; r->regime = regime;
    r->balance = balance; r->fee_cost = fee_cost;
    r->tp = 0; r->sl = 0; r->buy_cond_p = 0; r->buy_cond_v = 0;
    r->stddev = 0; r->avg = 0; r->spacing = 0; r->gate_dist_pct = 0;
    int i = 0;
    while (reason[i] && i < 15) { r->reason[i] = reason[i]; i++; }
    r->reason[i] = '\0';
    buf->head = (buf->head + 1) & (TRADE_LOG_BUF_SIZE - 1);
    buf->count++;
}

// slow path: drain all buffered records to the CSV file
static inline void TradeLogBuffer_Drain(TradeLogBuffer *buf, TradeLog *log) {
    if (buf->count == 0 || !log->file) return;
    uint32_t start = (buf->head - buf->count + TRADE_LOG_BUF_SIZE) & (TRADE_LOG_BUF_SIZE - 1);
    for (uint32_t i = 0; i < buf->count; i++) {
        uint32_t idx = (start + i) & (TRADE_LOG_BUF_SIZE - 1);
        TradeLogRecord *r = &buf->records[idx];
        if (r->is_buy) {
            TradeLog_Buy(log, r);
        } else {
            TradeLog_Sell(log, r);
        }
    }
    buf->count = 0;
}

//======================================================================================================
//======================================================================================================
#endif // TRADE_LOG_HPP
