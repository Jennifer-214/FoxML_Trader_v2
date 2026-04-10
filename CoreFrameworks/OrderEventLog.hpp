// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [ORDER EVENT LOG]
//
// Append-only in-memory log of order lifecycle events. Every state
// transition in the OMS (submit, ack, fill, reject, cancel, reconcile)
// gets appended here. The portfolio can be reconstructed from the log
// at any time via Portfolio_FromEventLog, making the log the canonical
// source of truth for what happened — the portfolio is a derived view.
//
// Phase 03: in-memory only, ring buffer sized for ~1 day of trading.
// Phase 07 adds disk persistence (write-ahead to a file, truncate the
// in-memory ring after flush). The fold function exists from day 1 so
// tests and replay can verify determinism without needing persistence.
//
// Design rules:
//   - Append-only. Never modify or delete entries. Corrections go in as
//     new OEVT_RECONCILED events that override previous fills.
//   - Trivially copyable entries. No pointers, no strings longer than
//     the fixed reason[] buffer. Enables direct memcpy to disk in phase 07.
//   - Monotonic event_id. Gaps are allowed (a dropped event from a full
//     buffer is detectable by comparing event_ids). Never reused.
//   - Insertion order == market-time order within a single thread.
//     Cross-thread ordering is NOT guaranteed by this log — the OMS
//     serializes through the command queue, so arrival-order at the
//     OMS IS the canonical ordering.
//======================================================================================================

#pragma once

#include "../FixedPoint/FixedPointN.hpp"
#include "../Limits.hpp"
#include "Order.hpp"
#include "Portfolio.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace tt {

//======================================================================================================
// [EVENT TYPES]
//======================================================================================================
enum OrderEventType : uint8_t {
    OEVT_SUBMITTED     = 0,   // order entered the OMS table
    OEVT_ACKNOWLEDGED  = 1,   // exchange confirmed receipt (future, phase 04+)
    OEVT_PARTIAL_FILL  = 2,   // partial fill (future, phase 06+)
    OEVT_FULL_FILL     = 3,   // fully filled — the only type the fold uses today
    OEVT_REJECTED      = 4,   // exchange or OMS rejected
    OEVT_CANCELED      = 5,   // canceled by us or by exchange
    OEVT_RECONCILED    = 6,   // reconciler override (phase 05+)
};

//======================================================================================================
// [ORDER EVENT]
//======================================================================================================
// One lifecycle event for one order. Self-contained — the fold function
// can reconstruct the portfolio from a stream of these without consulting
// any external state (no CoreContext lookup needed, no Order table needed).
//
// For OEVT_FULL_FILL buy events, tp and sl carry the intended take-profit
// and stop-loss at fill time. For sells and non-fill events they're zero.
// This makes the event log self-contained for replay — the fold doesn't
// need to know what the controller's intended TP/SL was at the time.
//======================================================================================================
template <unsigned F>
struct OrderEvent {
    uint64_t       event_id;       // monotonic across the log
    uint64_t       order_id;       // local order id from the OMS
    uint64_t       timestamp_us;   // market time of the originating event
    OrderEventType type;           // OEVT_*
    OrderType      order_type;     // ORDER_MARKET_BUY / ORDER_MARKET_SELL
    int16_t        core_id;        // which executor core (-1 for non-core)
    uint8_t        _pad[4];
    FPN<F>         price;          // fill price (zero for non-fill events)
    FPN<F>         qty;            // fill qty (zero for non-fill events)
    FPN<F>         tp;             // intended TP at fill time (entry only)
    FPN<F>         sl;             // intended SL at fill time (entry only)
    char           reason[32];     // short description for REJECTED/RECONCILED
};

//======================================================================================================
// [EVENT LOG STRUCT]
//======================================================================================================
// Growable array (malloc + realloc). Capacity is the allocated size,
// count is the number of valid entries. Grows 2x when full, matching the
// BacktestResults growth pattern in the codebase (see Backtest dynamic
// sizing rules in CLAUDE.md).
//
// Phase 03: sized for ~1 day of trading. SimpleDip fires ~50-100 entries
// per day at current BTC rates. 16384 initial capacity is ~160 days of
// headroom. Growth handles longer runs or faster strategies.
//======================================================================================================
constexpr size_t ORDER_EVENT_LOG_INIT_CAPACITY = 16384;

// Phase 07 file header — written at the start of the event log file.
// Carries the FPN width and entry size for forward compatibility.
struct OrderEventLogFileHeader {
    char     magic[8];       // "OMSEL01\0"
    uint32_t fpn_width;      // F template parameter (e.g. 64)
    uint32_t entry_size;     // sizeof(OrderEvent<F>) for this build
    uint64_t reserved[2];    // future: checksum, version
};

template <unsigned F>
struct OrderEventLog {
    OrderEvent<F>* entries;
    size_t         capacity;
    size_t         count;
    uint64_t       next_event_id;
    // Phase 07: optional disk persistence. When non-null, every Append
    // also fwrites the event to this file. Opened by InitWithFile,
    // flushed periodically, closed by Free.
    FILE*          disk_file;
    char           disk_path[256];
};

//======================================================================================================
// [INIT]
//======================================================================================================
template <unsigned F>
inline void OrderEventLog_Init(OrderEventLog<F>* log) {
    log->entries = (OrderEvent<F>*)std::malloc(
        ORDER_EVENT_LOG_INIT_CAPACITY * sizeof(OrderEvent<F>));
    log->capacity      = log->entries ? ORDER_EVENT_LOG_INIT_CAPACITY : 0;
    log->count         = 0;
    log->next_event_id = 1;
    log->disk_file     = nullptr;
    log->disk_path[0]  = '\0';
    if (!log->entries) {
        std::fprintf(stderr, "[OrderEventLog] WARN: initial malloc failed, "
                     "event logging disabled\n");
    }
}

//======================================================================================================
// [FREE]
//======================================================================================================
template <unsigned F>
inline void OrderEventLog_Free(OrderEventLog<F>* log) {
    if (log->disk_file) {
        std::fflush(log->disk_file);
        std::fclose(log->disk_file);
        log->disk_file = nullptr;
    }
    if (log->entries) {
        std::free(log->entries);
        log->entries = nullptr;
    }
    log->capacity      = 0;
    log->count         = 0;
    log->next_event_id = 0;
}

//======================================================================================================
// [APPEND]
//======================================================================================================
// Append one event. Grows the buffer 2x if full. Returns 1 on success,
// 0 on allocation failure (the event is lost — logged to stderr).
//
// The caller fills in all fields except event_id, which is assigned by
// this function from next_event_id.
//======================================================================================================
template <unsigned F>
inline int OrderEventLog_Append(OrderEventLog<F>* log, OrderEvent<F> event) {
    if (log->entries == nullptr) return 0;  // logging disabled (malloc failed)

    if (log->count >= log->capacity) {
        size_t new_cap = log->capacity * 2;
        if (new_cap < 256) new_cap = 256;  // safety floor
        OrderEvent<F>* new_buf = (OrderEvent<F>*)std::realloc(
            log->entries, new_cap * sizeof(OrderEvent<F>));
        if (!new_buf) {
            std::fprintf(stderr, "[OrderEventLog] WARN: realloc to %zu failed, "
                         "event %llu dropped\n",
                         new_cap, (unsigned long long)log->next_event_id);
            return 0;
        }
        log->entries  = new_buf;
        log->capacity = new_cap;
    }

    event.event_id = log->next_event_id++;
    log->entries[log->count++] = event;

    // Phase 07: write-through to disk. Best-effort — a failed fwrite
    // loses the disk copy but the in-memory log is still correct.
    if (log->disk_file) {
        if (std::fwrite(&event, sizeof(event), 1, log->disk_file) != 1) {
            std::fprintf(stderr, "[OrderEventLog] WARN: disk write failed for "
                         "event %llu\n", (unsigned long long)event.event_id);
        }
        // Flush every 16 events to balance durability vs throughput.
        if ((log->count & 15) == 0) std::fflush(log->disk_file);
    }
    return 1;
}

//======================================================================================================
// [INIT WITH FILE — phase 07 disk persistence]
//======================================================================================================
// Like Init, but also opens a binary file for write-through. Events are
// appended to disk on every OrderEventLog_Append call. The file carries a
// small header for forward compatibility (magic + FPN width + entry size).
//
// If the file already exists, LoadFromDisk should be called BEFORE this to
// replay the events into memory. This function opens the file in append
// mode so existing data is preserved.
//======================================================================================================
template <unsigned F>
inline void OrderEventLog_InitWithFile(OrderEventLog<F>* log, const char* path) {
    OrderEventLog_Init(log);
    std::strncpy(log->disk_path, path, sizeof(log->disk_path) - 1);
    log->disk_path[sizeof(log->disk_path) - 1] = '\0';

    // Check if the file exists (for the header write decision).
    bool file_exists = false;
    FILE* probe = std::fopen(path, "rb");
    if (probe) { file_exists = true; std::fclose(probe); }

    log->disk_file = std::fopen(path, "ab");
    if (!log->disk_file) {
        std::fprintf(stderr, "[OrderEventLog] WARN: could not open %s for writing, "
                     "disk persistence disabled\n", path);
        return;
    }

    // Write the header only if this is a new file.
    if (!file_exists) {
        OrderEventLogFileHeader hdr;
        std::memset(&hdr, 0, sizeof(hdr));
        std::memcpy(hdr.magic, "OMSEL01", 8);
        hdr.fpn_width  = F;
        hdr.entry_size = (uint32_t)sizeof(OrderEvent<F>);
        std::fwrite(&hdr, sizeof(hdr), 1, log->disk_file);
        std::fflush(log->disk_file);
    }

    std::fprintf(stderr, "[OrderEventLog] disk persistence: %s (%s)\n",
                 path, file_exists ? "appending" : "new file");
}

//======================================================================================================
// [LOAD FROM DISK — phase 07 replay on startup]
//======================================================================================================
// Reads events from a previously-written event log file and populates the
// in-memory buffer. Validates the file header for magic + FPN width match.
// Returns the number of events loaded, or -1 on error.
//
// Call this BEFORE InitWithFile if the file already exists. The loaded
// events are available for Portfolio_FromEventLog replay.
//======================================================================================================
template <unsigned F>
inline int OrderEventLog_LoadFromDisk(OrderEventLog<F>* log, const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return 0;  // no file = nothing to load (not an error)

    // Read and validate the header.
    OrderEventLogFileHeader hdr;
    if (std::fread(&hdr, sizeof(hdr), 1, f) != 1) {
        std::fprintf(stderr, "[OrderEventLog] WARN: %s too small for header\n", path);
        std::fclose(f);
        return -1;
    }
    if (std::memcmp(hdr.magic, "OMSEL01", 8) != 0) {
        std::fprintf(stderr, "[OrderEventLog] WARN: %s bad magic\n", path);
        std::fclose(f);
        return -1;
    }
    if (hdr.fpn_width != F) {
        std::fprintf(stderr, "[OrderEventLog] WARN: %s FPN width mismatch "
                     "(file=%u, build=%u)\n", path, hdr.fpn_width, F);
        std::fclose(f);
        return -1;
    }
    if (hdr.entry_size != (uint32_t)sizeof(OrderEvent<F>)) {
        std::fprintf(stderr, "[OrderEventLog] WARN: %s entry size mismatch "
                     "(file=%u, build=%u)\n", path, hdr.entry_size,
                     (uint32_t)sizeof(OrderEvent<F>));
        std::fclose(f);
        return -1;
    }

    // Read events one at a time until EOF.
    int loaded = 0;
    OrderEvent<F> event;
    while (std::fread(&event, sizeof(event), 1, f) == 1) {
        // Append to the in-memory buffer (bypasses disk write since
        // we're reading FROM disk). Manually grow if needed.
        if (log->count >= log->capacity) {
            size_t new_cap = log->capacity * 2;
            if (new_cap < 256) new_cap = 256;
            OrderEvent<F>* new_buf = (OrderEvent<F>*)std::realloc(
                log->entries, new_cap * sizeof(OrderEvent<F>));
            if (!new_buf) {
                std::fprintf(stderr, "[OrderEventLog] WARN: realloc failed during load\n");
                break;
            }
            log->entries  = new_buf;
            log->capacity = new_cap;
        }
        log->entries[log->count++] = event;
        if (event.event_id >= log->next_event_id) {
            log->next_event_id = event.event_id + 1;
        }
        loaded++;
    }

    std::fclose(f);
    std::fprintf(stderr, "[OrderEventLog] loaded %d events from %s\n", loaded, path);
    return loaded;
}

//======================================================================================================
// [HELPER — build a fill event]
//======================================================================================================
// Convenience builder for the common case: a full fill from the OMS
// callback. Fills all the fields so the caller doesn't have to zero-init
// and set each one individually.
//======================================================================================================
template <unsigned F>
inline OrderEvent<F> OrderEvent_MakeFill(uint64_t order_id,
                                          uint64_t timestamp_us,
                                          OrderType order_type,
                                          int16_t core_id,
                                          FPN<F> price,
                                          FPN<F> qty,
                                          FPN<F> tp,
                                          FPN<F> sl) {
    OrderEvent<F> e;
    std::memset(&e, 0, sizeof(e));
    e.event_id     = 0;  // assigned by Append
    e.order_id     = order_id;
    e.timestamp_us = timestamp_us;
    e.type         = OEVT_FULL_FILL;
    e.order_type   = order_type;
    e.core_id      = core_id;
    e.price        = price;
    e.qty          = qty;
    e.tp           = tp;
    e.sl           = sl;
    e.reason[0]    = '\0';
    return e;
}

template <unsigned F>
inline OrderEvent<F> OrderEvent_MakeRejection(uint64_t order_id,
                                               uint64_t timestamp_us,
                                               OrderType order_type,
                                               int16_t core_id,
                                               const char* reason_str) {
    OrderEvent<F> e;
    std::memset(&e, 0, sizeof(e));
    e.event_id     = 0;
    e.order_id     = order_id;
    e.timestamp_us = timestamp_us;
    e.type         = OEVT_REJECTED;
    e.order_type   = order_type;
    e.core_id      = core_id;
    if (reason_str) {
        std::strncpy(e.reason, reason_str, sizeof(e.reason) - 1);
        e.reason[sizeof(e.reason) - 1] = '\0';
    }
    return e;
}

//======================================================================================================
// [PORTFOLIO FOLD — deterministic reconstruction from the event log]
//======================================================================================================
// Walk the event log in order, replay every FULL_FILL, and produce the
// resulting Portfolio + balance + realized P&L. The output is
// deterministic: same events in, same state out, every time. This is the
// foundation for phase 07 replay and the phase 03 head-to-head test.
//
// The fold only processes OEVT_FULL_FILL events. Other event types
// (SUBMITTED, ACKNOWLEDGED, REJECTED, CANCELED) don't affect portfolio
// or balance. OEVT_PARTIAL_FILL is deferred to phase 06 — partial fills
// don't occur in phase 03 (market orders fill fully).
//
// Fee model: symmetric (entry fee + exit fee), each = price * qty * fee_rate.
// Matches the OnEvent computation at ControllerEventLoop.hpp.
//
// Returns the number of fill events processed (for verification).
//======================================================================================================
template <unsigned F>
struct FoldResult {
    FPN<F> balance;
    FPN<F> realized_pnl;
    Portfolio<F> portfolio;
    int    fills_processed;
};

template <unsigned F>
inline FoldResult<F> Portfolio_FromEventLog(const OrderEventLog<F>* log,
                                            FPN<F> starting_balance,
                                            FPN<F> fee_rate) {
    FoldResult<F> result;
    result.balance         = starting_balance;
    result.realized_pnl    = FPN_Zero<F>();
    result.fills_processed = 0;
    Portfolio_Init(&result.portfolio);

    for (size_t i = 0; i < log->count; ++i) {
        const OrderEvent<F>& e = log->entries[i];
        if (e.type != OEVT_FULL_FILL) continue;

        int slot = (int)e.core_id;
        if (slot < 0 || slot >= MAX_PORTFOLIO_POSITIONS) continue;

        if (e.order_type == ORDER_MARKET_BUY) {
            // Entry fill: open the slot.
            FPN<F> notional  = FPN_Mul(e.price, e.qty);
            FPN<F> entry_fee = FPN_Mul(notional, fee_rate);
            Portfolio_OpenSlot(&result.portfolio, slot,
                               e.price, e.qty, e.tp, e.sl, entry_fee);
        } else if (e.order_type == ORDER_MARKET_SELL) {
            // Exit fill: close the slot, compute net P&L, update balance.
            // Same math as EventLoop_OnEvent in ControllerEventLoop.hpp.
            FPN<F> entry_fee     = result.portfolio.positions[slot].entry_fee;
            FPN<F> qty_snap      = result.portfolio.positions[slot].quantity;
            FPN<F> gross         = Portfolio_CloseSlot(&result.portfolio, slot, e.price);
            FPN<F> exit_notional = FPN_Mul(e.price, qty_snap);
            FPN<F> exit_fee      = FPN_Mul(exit_notional, fee_rate);
            FPN<F> total_fee     = FPN_Add(entry_fee, exit_fee);
            FPN<F> net           = FPN_Sub(gross, total_fee);
            result.balance       = FPN_Add(result.balance, net);
            result.realized_pnl  = FPN_Add(result.realized_pnl, net);
        }
        result.fills_processed++;
    }

    return result;
}

}  // namespace tt
