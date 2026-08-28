// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[CoreFrameworks/OrderEventLog.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [PERSISTENCE] [CAPITAL_BEARING] [CONCURRENCY]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the append-only order audit log — the canonical source of truth; portfolio = a derived fold of it]
// [CONTAINS]
//   - [ENUM]_[OrderEventType]
//   - [STRUCT]_[OrderEvent]
//   - [STRUCT]_[OrderEventLogFileHeader]
//   - [STRUCT]_[OrderEventLog]
//   - [FUNCTION]_[OrderEventLog_Init]
//   - [FUNCTION]_[OrderEventLog_Free]
//   - [FUNCTION]_[OrderEventLog_ApplyEvent]
//   - [FUNCTION]_[OrderEventLog_Append]
//   - [FUNCTION]_[OrderEventLog_AsyncWriterRoutine]
//   - [FUNCTION]_[OrderEventLog_StartAsyncWriter]
//   - [FUNCTION]_[OrderEventLog_StopAsyncWriter]
//   - [FUNCTION]_[OrderEventLog_InitWithFile]
//   - [FUNCTION]_[OrderEventLog_Reset]
//   - [FUNCTION]_[OrderEventLog_LoadFromDisk]
//   - [FUNCTION]_[OrderEvent_MakeFill]
//   - [FUNCTION]_[Portfolio_FromEventLog]
//======================================================================================================
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
#include <cerrno>
#include <ctime>      // time() for D-175a rotate suffix      // errno - made explicit (was transitively included; header self-sufficiency, .E.1-reshuffle-protective)
#include <atomic>      // v5.11.3.C — async writer thread coordination
#include <pthread.h>   // v5.11.3.C — async writer pthread
#include <unistd.h>    // v5.11.3.C — usleep in writer idle path
#include <sys/mman.h>  // v5.11.5.C — mmap(MAP_POPULATE) pre-alloc

#include "SPSCRing.hpp"  // v5.11.3.C — drainer → writer queue

namespace tt {

//======================================================================
// [ENUM]_[OrderEventType]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [PERSISTENCE]]
// [REFERENCE]_[INVARIANT]_[H21]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[order lifecycle event codes — disk-persisted in every OrderEvent record; append-only, never reuse]
//======================================================================
// [CODE]
//======================================================================
enum OrderEventType : uint8_t {
    OEVT_SUBMITTED     = 0,   // order entered the OMS table
    OEVT_ACKNOWLEDGED  = 1,   // exchange confirmed receipt (future, phase 04+)
    OEVT_PARTIAL_FILL  = 2,   // partial fill (future, phase 06+)
    OEVT_FULL_FILL     = 3,   // fully filled — the only type the fold uses today
    OEVT_REJECTED      = 4,   // exchange or OMS rejected
    OEVT_CANCELED      = 5,   // canceled by us or by exchange
    OEVT_RECONCILED    = 6,   // reconciler override (phase 05+)
    OEVT_CTRL_TRUNCATE = 7,   // E.1.3 P3-c (D-445): IN-BAND reset control — the writer thread
                              // intercepts + truncates; NEVER persisted to disk (rides the ring
                              // only, so FIFO guarantees every pre-reset event lands in the OLD
                              // file first — the ack-free property). H21: append-only code; the
                              // value is enum-space-reserved even though no record carries it.
};
//======================================================================
// [END_CODE]
//======================================================================
// [END_ENUM]_[OrderEventType]
//======================================================================

//======================================================================
// [STRUCT]_[OrderEvent]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [PERSISTENCE] [CAPITAL_BEARING] [DECIMAL]]
// [REFERENCE]_[INVARIANT]_[[H9] [H12] [H21]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[one self-contained lifecycle event — fwritten RAW to the OMSEL02 disk log; the fold replays these alone]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
struct OrderEvent {
    uint64_t       event_id;       // monotonic across the log
    uint64_t       order_id;       // local order id from the OMS
    uint64_t       timestamp_us;   // market time of the originating event
    OrderEventType type;           // OEVT_*
    OrderType      order_type;     // ORDER_MARKET_BUY / ORDER_MARKET_SELL
    int16_t        node_id;        // the portfolio SLOT (Order::node_id; -1 = non-node event).
                                   // NOT the executor node — read back as `int slot` below.
                                   // H21: the -1 sentinel is wire-visible; never reassign it.
    uint8_t        _pad[4];
    Money          price;          // fill price (DECIMAL money — Ship B P2b epoch)
    Money          qty;            // fill qty (decimal)
    Money          tp;             // intended TP at fill time (entry only; decimal)
    Money          sl;             // intended SL at fill time (entry only; decimal)
    Money          fee;            // booked fee for fill events (S-3; semantics land at P3
                                   // fill-lifecycle rework — zero until then; wire slot settles NOW)
    char           reason[32];     // short description for REJECTED/RECONCILED
};
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// One lifecycle event for one order. Self-contained — the fold function
// can reconstruct the portfolio from a stream of these without consulting
// any external state (no NodeContext lookup needed, no Order table needed).
//
// For OEVT_FULL_FILL buy events, tp and sl carry the intended take-profit
// and stop-loss at fill time. For sells and non-fill events they're zero.
// This makes the event log self-contained for replay — the fold doesn't
// need to know what the controller's intended TP/SL was at the time.
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-08-10]
//----------------------------------------------------------------------
// [SIZE]_[144B]
// [ALIGN]_[16]
// [CACHE_LINES]_[3]
// [STRADDLE]_[reason@112]
//======================================================================
// [END_STRUCT]_[OrderEvent]
//======================================================================

// TECH_DEBT-228 — compile-time size pin on the LIVE OMSEL02 raw-struct wire image.
// Mirrors the Position pin (Portfolio.hpp). Until now the ONLY guard was the RUNTIME
// `entry_size` header check, which fails SAFE (reject-rotate, never misread) but lets a
// layout change through as a silent format break instead of a build error. With this pin a
// layout change is a compile error, forcing a DELIBERATE ORDER_EVENT_LOG_FORMAT_VERSION
// bump (H21) rather than a silently-rotated log.
static_assert(sizeof(OrderEvent<64>) == 144,
              "OrderEvent<64> is the OMSEL02 wire image — a size change is a WIRE FORMAT "
              "change: bump ORDER_EVENT_LOG_FORMAT_VERSION + update this pin in the same "
              "commit (H21 append-only; never reuse a retired version number).");

// Ship-B P2 epoch guard (S-4/D-175a): the header's magic/fpn_width/entry_size ALL stay unchanged
// at a 16B->16B decimal re-encoding — old logs would replay binary-scaled ints as decimals.
// Tripwire: the flip commit must bump the magic OMSEL01->OMSEL02 + claim a reserved[] word as
// format_version + switch the reject path to ROTATE-not-append (the loader's swallowed -1 +
// "ab" reopen otherwise appends decimal events under the OLD magic — lost at next boot) + add
// the booked-fee field (S-3) in the same stroke.
// Format version 2 = the decimal-money epoch (OMSEL02). Version 1 = the 16B-binary
// OMSEL01 era — H21 TOMBSTONE: never reuse the OMSEL01 magic or version 1.
#define ORDER_EVENT_LOG_FORMAT_VERSION 2u
// [ASSERT]_[EPOCH_TRIPWIRE]_[is_fp_decimal_v<OrderEvent money> => ORDER_EVENT_LOG_FORMAT_VERSION >= 2]
// [WHY]_[a 16B-to-16B encoding flip leaves magic/width/entry_size unchanged — the trait-keyed guard forces the OMSEL02 version bump in the same commit]
static_assert(!is_fp_decimal_v<decltype(OrderEvent<64>::price)> || ORDER_EVENT_LOG_FORMAT_VERSION >= 2u,
              "Ship-B epoch: decimal OrderEvent money requires format version >= 2 (OMSEL02).");

//------------------------------------------------------------------------------------------------------
// [SECTION]_[capacity constants + file header]
//------------------------------------------------------------------------------------------------------
// Phase-03 design (the growable era — SUPERSEDED by the v5.11.5.C fixed mmap below):
// Growable array (malloc + realloc). Capacity is the allocated size,
// count is the number of valid entries. Grows 2x when full, matching the
// BacktestResults growth pattern in the codebase (see Backtest dynamic
// sizing rules in CLAUDE.md).
//
// Phase 03: sized for ~1 day of trading. SimpleDip fires ~50-100 entries
// per day at current BTC rates. 16384 initial capacity is ~160 days of
// headroom. Growth handles longer runs or faster strategies.
constexpr size_t ORDER_EVENT_LOG_INIT_CAPACITY = 16384;   // growable-era constant; no live consumers (TECH_DEBT-192 cluster)

// v5.11.5.C — fixed mmap-allocated capacity. Replaces malloc + realloc
// growth pattern with a single mmap(MAP_POPULATE) at boot. Trade-off:
// - Pre-faults all pages at boot so first-write never hits a page-fault tail.
// - Capacity is FIXED at boot — overflow drops events with a counter
//   bump (silent failure → operator sees the counter via the GUI).
// - Eliminates realloc-mid-trading from the writer thread (which v5.11.3.C
//   moved off the drainer; v5.11.5.C now eliminates it entirely).
//
// v5.11.5.D — bumped to 32768 (~5.6 MB pre-touched at F=64) per
// parity-check 2026-05-07 Section N finding: 16384 truncates on load
// for any pre-v5.11.5.C log file with > 16384 events (roughly 30 days
// at SimpleDip rates × 4 cores). 32768 gives ~320 days headroom,
// which covers all realistic continuous-deployment scenarios while
// keeping the mmap'd buffer comfortably small (5.6 MB << page cache).
constexpr size_t ORDER_EVENT_LOG_MAX_CAPACITY = 32768;

//======================================================================
// [STRUCT]_[OrderEventLogFileHeader]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [PERSISTENCE]]
// [REFERENCE]_[INVARIANT]_[[H9] [H21]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the OMSEL02 on-disk header — magic + width + entry size gate every load; OMSEL01 is tombstoned]
//======================================================================
// [CODE]
//======================================================================
struct OrderEventLogFileHeader {
    char     magic[8];       // "OMSEL02\0" (OMSEL01 = pre-epoch binary era, H21 tombstone)
    uint32_t fpn_width;      // F template parameter (e.g. 64)
    uint32_t entry_size;     // sizeof(OrderEvent<F>) for this build
    uint64_t format_version; // ORDER_EVENT_LOG_FORMAT_VERSION (claimed from reserved[0] at the epoch)
    uint64_t reserved;       // future: checksum
};

// TECH_DEBT-300a — compile-time size pin on the OMSEL02 FILE HEADER.
// The record (OrderEvent) was pinned at TECH_DEBT-228; its HEADER was not, and nothing noticed
// because check_struct_alignment could not see `fread(&phdr, sizeof(phdr), ...)` — its detector
// required sizeof(TYPE) and the idiom here is sizeof(VARIABLE). This header carries the magic,
// the FPN width, `entry_size` and the format version: a silent layout change here mis-frames
// EVERY record behind it, which is strictly worse than mis-sizing one record.
// A size change is a WIRE FORMAT change — bump ORDER_EVENT_LOG_FORMAT_VERSION and this pin in
// the same commit (H21 append-only; never reuse a retired version number).
static_assert(sizeof(OrderEventLogFileHeader) == 32,
              "OrderEventLogFileHeader is the OMSEL02 file header — a size change re-frames every "
              "record in the log: bump ORDER_EVENT_LOG_FORMAT_VERSION + update this pin together.");
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Phase 07 file header — written at the start of the event log file.
// Carries the FPN_Binary width and entry size for forward compatibility.
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
//----------------------------------------------------------------------
// [SIZE]_[32B]
// [ALIGN]_[8]
// [CACHE_LINES]_[1]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[OrderEventLogFileHeader]
//======================================================================

// v5.11.3.C — async writer SPSC ring size. 256 events × ~176B/event = ~45 KB.
// Default-strategy burst is far below this (SimpleDip ~50-100 events/day total
// across all cores); the ring exists to absorb micro-bursts (drainer cycles
// processing many fills at once) without blocking the drainer on fwrite.
constexpr size_t ORDER_EVENT_LOG_ASYNC_RING_SIZE = 256;

//======================================================================
// [STRUCT]_[OrderEventLog]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [PERSISTENCE] [CONCURRENCY] [CAPITAL_BEARING]]
// [THREAD]_[[DRAINER_WRITER] [WORKER_CONSUMER]]
// [SYNC]_[SPSC]
// [SYNC]_[ATOMIC]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the log state — mmap'd entries + disk write-through + the drainer->writer async SPSC seam]
// [REFERENCE]_[CLASS]_[50]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
struct OrderEventLog {
    OrderEvent<F>* entries;
    size_t         capacity;
    size_t         count;
    std::atomic<uint64_t> next_event_id;  // v5.11.3.C — atomic so the producer
                                           // (drainer) and writer (consumer) can
                                           // coexist without races on event_id
                                           // assignment.
    // Phase 07: optional disk persistence. When non-null, every Append
    // also fwrites the event to this file. Opened by InitWithFile,
    // flushed periodically, closed by Free.
    FILE*          disk_file;
    char           disk_path[256];

    // v5.11.3.C — async writer thread plumbing.
    //
    // Drainer thread calls OrderEventLog_Append → SPSCRing_TryPush onto
    // async_ring (no I/O, no realloc, ~5ns). The dedicated writer thread
    // sleeps when the ring is empty, wakes on usleep cadence, drains the
    // ring + applies each event (realloc + memcpy + fwrite). Disk-stall
    // isolation: a kernel page-cache flush blocking fwrite no longer pauses
    // the drainer.
    //
    // SPSC discipline: drainer is the SOLE producer (per OMS_DrainSubmit
    // single-caller invariant); writer thread is the SOLE consumer.
    //
    // Lifecycle:
    //   Init                 → ring initialized, writer NOT started
    //   StartAsyncWriter     → spawns pthread, sets writer_thread_active=1
    //   Append (active=1)    → push to ring; sync fallback if push fails (rare)
    //   Append (active=0)    → original sync path (test mode, init time)
    //   StopAsyncWriter      → signal stop, join, drain remaining events
    //   Free                 → calls Stop first if still active
    //
    // Fields below are touched by both threads — careful with ordering.
    SPSCRing<OrderEvent<F>, ORDER_EVENT_LOG_ASYNC_RING_SIZE> async_ring;
    pthread_t      writer_thread;        // pthread handle (only valid when active)
    std::atomic<int> writer_thread_active{0};  // 1 = writer thread is running. In-class init {0} (.E.0.10 TD-202/RBP Class 50): the quiesce-first OrderEventLog_Init reads this BEFORE its own store, and many OMS sites default-init (`OrderManagerState<64> oms;`) → without {0} the pre-Init read is indeterminate (UB).
    std::atomic<int> writer_should_stop{0};    // writer polls this; set on Stop. {0} for the same construction-time-determinism reason (lifecycle pair).
    std::atomic<uint64_t> ring_full_spins;  // total pause/usleep iterations spent waiting for ring slots
    std::atomic<uint64_t> writer_realloc_failed_count;  // writer realloc failures (legacy — should stay 0 post-v5.11.5.C)
    std::atomic<uint64_t> log_full_drops;   // v5.11.5.C — events dropped because mmap'd capacity is exhausted
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
//----------------------------------------------------------------------
// [SIZE]_[37376B]
// [ALIGN]_[64]
// [CACHE_LINES]_[584]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[OrderEventLog]
//======================================================================

// Forward-decl so OrderEventLog_Init (below) can quiesce a running writer before
// re-initializing. StopAsyncWriter is DEFINED further down; the forward-decl keeps
// the dependent call well-formed under two-phase lookup. (.E.0.10 TD-202/RBP Class 50.)
template <unsigned F> inline void OrderEventLog_StopAsyncWriter(OrderEventLog<F>* log);

//======================================================================
// [FUNCTION]_[OrderEventLog_Init]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BOOT_TIME] [CONCURRENCY]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[quiesce-first init — stop any live writer, then mmap(MAP_POPULATE) the fixed-capacity entries buffer]
// [REFERENCE]_[CLASS]_[50]
// [REFERENCE]_[MEMORY]_[feedback_fix_toward_future_trajectory_not_static_state]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline void OrderEventLog_Init(OrderEventLog<F>* log) {
    // .E.0.10 (TD-202 / RBP Class 50 — re-init-defeats-join lifecycle): QUIESCE-FIRST.
    // If a writer is already running (re-Init on a live log — the double-init that
    // defeats OrderEventLog_Free's guarded join → heap-use-after-free on async_ring),
    // stop+join it BEFORE re-initializing the ring / entries[] / the active flag.
    // Safe on a fresh log: writer_thread_active is in-class-init'd to 0, so Stop no-ops.
    // This is the forward-compatible .E.0.10 increment of the Init/Free/Start/Stop
    // lifecycle-idempotency discipline the .E.1 SPSC/event-log rework OWNS (single-owner
    // disk_file + a fully idempotent Init that also reclaims entries[]). The full rework
    // is folded into the .E.1 foundation plan; this guard is the stone it builds on.
    // See feedback_fix_toward_future_trajectory_not_static_state.
    OrderEventLog_StopAsyncWriter(log);

    // v5.11.5.C — mmap(MAP_POPULATE) for the entries[] buffer. Pre-faults
    // all pages at boot so the first-write path never hits a page-fault
    // tail. Capacity is fixed (no realloc); overflow increments
    // log_full_drops + drops the event silently (counter is GUI-surfaced).
    const size_t bytes = ORDER_EVENT_LOG_MAX_CAPACITY * sizeof(OrderEvent<F>);
    void* mem = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                        -1, 0);
    if (mem == MAP_FAILED) {
        // Fallback: malloc. Less ideal (no MAP_POPULATE pre-fault, no
        // alignment guarantee) but keeps the engine running.
        std::fprintf(stderr, "[OrderEventLog] WARN: mmap(MAP_POPULATE) "
                     "failed (%s), falling back to malloc\n",
                     std::strerror(errno));
        log->entries = (OrderEvent<F>*)std::malloc(bytes);
    } else {
        log->entries = (OrderEvent<F>*)mem;
    }
    log->capacity      = log->entries ? ORDER_EVENT_LOG_MAX_CAPACITY : 0;
    log->count         = 0;
    log->next_event_id.store(1, std::memory_order_relaxed);
    log->disk_file     = nullptr;
    log->disk_path[0]  = '\0';
    // v5.11.3.C — async writer state. Ring initialized, writer thread NOT
    // running until StartAsyncWriter is called. Pre-Start, Append falls back
    // to the original sync path (zero behavior change for tests).
    SPSCRing_Init(&log->async_ring);
    log->writer_thread_active.store(0, std::memory_order_relaxed);
    log->writer_should_stop.store(0, std::memory_order_relaxed);
    log->ring_full_spins.store(0, std::memory_order_relaxed);
    log->writer_realloc_failed_count.store(0, std::memory_order_relaxed);
    log->log_full_drops.store(0, std::memory_order_relaxed);
    if (!log->entries) {
        std::fprintf(stderr, "[OrderEventLog] WARN: entries[] allocation "
                     "failed, event logging disabled\n");
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[OrderEventLog_Init]
//======================================================================

//======================================================================
// [FUNCTION]_[OrderEventLog_Free]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BOOT_TIME]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[teardown — stop the writer first, flush + close disk, munmap (or free) the entries buffer]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline void OrderEventLog_Free(OrderEventLog<F>* log) {
    // v5.11.3.C — stop the async writer thread first so it stops touching
    // entries[] and disk_file before we free them. StopAsyncWriter is a
    // no-op if writer was never started (test path).
    OrderEventLog_StopAsyncWriter(log);
    if (log->disk_file) {
        std::fflush(log->disk_file);
        std::fclose(log->disk_file);
        log->disk_file = nullptr;
    }
    if (log->entries) {
        // v5.11.5.C — entries[] was allocated via mmap (or malloc fallback).
        // Try munmap first; if it fails, the buffer was the malloc fallback
        // and we free() it. munmap on a non-mmap'd region returns EINVAL
        // without modifying memory; safe probe.
        const size_t bytes = ORDER_EVENT_LOG_MAX_CAPACITY * sizeof(OrderEvent<F>);
        if (::munmap(log->entries, bytes) != 0) {
            std::free(log->entries);  // malloc fallback path
        }
        log->entries = nullptr;
    }
    log->capacity      = 0;
    log->count         = 0;
    log->next_event_id.store(0, std::memory_order_relaxed);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[OrderEventLog_Free]
//======================================================================

//======================================================================
// [FUNCTION]_[OrderEventLog_ApplyEvent]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [PERSISTENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[apply one event to memory + disk — fixed capacity: overflow drops + bumps the GUI-surfaced counter]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline int OrderEventLog_ApplyEvent(OrderEventLog<F>* log, const OrderEvent<F>& event) {
    // v5.11.5.C — fixed mmap'd capacity. Overflow drops the event + bumps a
    // counter (operator-visible via the GUI). Realloc-mid-trading is gone.
    if (log->count >= log->capacity) {
        log->log_full_drops.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
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
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Apply one event to the in-memory log + disk. The memcpy + fwrite pieces
// of the original Append. Caller is responsible for any
// thread-safety: this body assumes single-threaded access to log->entries
// and log->disk_file.
//
// Used by:
//   - OrderEventLog_Append in sync mode (caller is the drainer)
//   - OrderEventLog_AsyncWriterRoutine (caller is the writer thread)
//======================================================================
// [END_FUNCTION]_[OrderEventLog_ApplyEvent]
//======================================================================

//======================================================================
// [FUNCTION]_[OrderEventLog_Append]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [OMS_DRAINER] [CONCURRENCY]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[drainer-side append — assign event_id, push to the async ring (writer applies) or sync-apply pre-Start]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline int OrderEventLog_Append(OrderEventLog<F>* log, OrderEvent<F> event) {
    if (log->entries == nullptr) return 0;  // logging disabled (malloc failed)

    // Assign event_id atomically — works for both sync and async paths.
    // fetch_add(1) on uint64_t is contention-safe even though only the
    // drainer calls Append in production (reader threads never assign IDs).
    event.event_id = log->next_event_id.fetch_add(1, std::memory_order_relaxed);

    // v5.11.3.C — async path: enqueue + return. No realloc, no I/O on the
    // drainer thread. The writer thread dequeues and applies.
    if (log->writer_thread_active.load(std::memory_order_acquire)) {
        // Spin-wait if the ring is momentarily full. Writer thread drains
        // within its usleep cadence (~1ms), so spin time is bounded. We
        // CANNOT fall back to inline ApplyEvent here — that would race
        // with the writer thread's ApplyEvent on log->entries / disk_file
        // (they're SPSC-disciplined to be writer-thread-only post-Start).
        for (int spin = 0; !SPSCRing_TryPush(&log->async_ring, event); ++spin) {
            log->ring_full_spins.fetch_add(1, std::memory_order_relaxed);
            if (spin < 64) {
                __builtin_ia32_pause();   // tight spin while writer drains
            } else {
                usleep(100);              // back off to 0.1ms after 64 pauses
            }
        }
        return 1;
    }

    // Sync path (writer thread not active — test mode or pre-Start init).
    return OrderEventLog_ApplyEvent(log, event);
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// The caller fills in all fields except event_id, which is assigned by
// this function from next_event_id.
//======================================================================
// [END_FUNCTION]_[OrderEventLog_Append]
//======================================================================

//======================================================================
// [FUNCTION]_[OrderEventLog_AsyncWriterRoutine]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [CONCURRENCY] [PERSISTENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the writer thread body (v5.11.3.C) — drain ring, apply, drain-before-stop so shutdown flushes everything; P3-c: D-445 in-band truncate intercept]
//======================================================================
// [CODE]
//======================================================================
// fwd decl — the truncate body lives with Reset below; the D-445 intercept calls it.
template <unsigned F>
inline void OrderEventLog_TruncateDisk(OrderEventLog<F>* log);

template <unsigned F>
inline void* OrderEventLog_AsyncWriterRoutine(void* arg) {
    OrderEventLog<F>* log = (OrderEventLog<F>*)arg;
    OrderEvent<F> event;
    for (;;) {
        // Drain whatever's in the ring right now.
        int drained = 0;
        while (SPSCRing_TryPop(&log->async_ring, &event)) {
            // P3-c (D-445): in-band truncate intercept — every pre-reset event above already
            // applied to the OLD file (ring FIFO); truncate + zero the writer-owned count,
            // never persist the control record itself.
            if (event.type == OEVT_CTRL_TRUNCATE) {
                log->count = 0;
                OrderEventLog_TruncateDisk(log);
                drained++;
                continue;
            }
            if (!OrderEventLog_ApplyEvent(log, event)) {
                log->writer_realloc_failed_count.fetch_add(
                    1, std::memory_order_relaxed);
            }
            drained++;
        }
        // After draining, check stop signal. We drain BEFORE checking stop
        // so shutdown still flushes everything the drainer pushed before
        // signaling.
        if (log->writer_should_stop.load(std::memory_order_acquire)) {
            // Final drain pass to catch anything pushed between "ring empty"
            // and stop check (rare but possible under shutdown race).
            while (SPSCRing_TryPop(&log->async_ring, &event)) {
                if (event.type == OEVT_CTRL_TRUNCATE) {   // D-445: same intercept on the tail drain
                    log->count = 0;
                    OrderEventLog_TruncateDisk(log);
                    continue;
                }
                OrderEventLog_ApplyEvent(log, event);
            }
            // Final flush so the disk file is consistent post-shutdown.
            if (log->disk_file) std::fflush(log->disk_file);
            return nullptr;
        }
        // Empty + not stopping — sleep. 1ms is plenty given event cadence
        // (default strategy fires ~1 event every ~200 sec; even a 100ms
        // sleep would be fine, but 1ms minimizes shutdown latency).
        if (drained == 0) usleep(1000);
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[OrderEventLog_AsyncWriterRoutine]
//======================================================================

//======================================================================
// [FUNCTION]_[OrderEventLog_StartAsyncWriter]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BOOT_TIME] [CONCURRENCY]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[spawn the writer pthread — idempotent; failure degrades to sync Append, never fatal]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline int OrderEventLog_StartAsyncWriter(OrderEventLog<F>* log) {
    if (log->writer_thread_active.load(std::memory_order_acquire)) return 0;
    log->writer_should_stop.store(0, std::memory_order_relaxed);
    int rc = pthread_create(&log->writer_thread, nullptr,
                             OrderEventLog_AsyncWriterRoutine<F>, log);
    if (rc != 0) {
        std::fprintf(stderr, "[OrderEventLog] WARN: pthread_create failed (%d), "
                     "async writer disabled — falling back to sync Append\n", rc);
        return 0;
    }
    log->writer_thread_active.store(1, std::memory_order_release);
    return 1;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[OrderEventLog_StartAsyncWriter]
//======================================================================

//======================================================================
// [FUNCTION]_[OrderEventLog_StopAsyncWriter]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [CONCURRENCY]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[signal + join the writer — no-op when not running; the lifecycle pair of StartAsyncWriter]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline void OrderEventLog_StopAsyncWriter(OrderEventLog<F>* log) {
    if (!log->writer_thread_active.load(std::memory_order_acquire)) return;
    log->writer_should_stop.store(1, std::memory_order_release);
    pthread_join(log->writer_thread, nullptr);
    log->writer_thread_active.store(0, std::memory_order_release);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[OrderEventLog_StopAsyncWriter]
//======================================================================

//======================================================================
// [FUNCTION]_[OrderEventLog_InitWithFile]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [PERSISTENCE] [BOOT_TIME]]
// [REFERENCE]_[INVARIANT]_[H21]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[open disk write-through — D-175a ROTATE-not-append on any stale-format file; header written on fresh files]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline void OrderEventLog_InitWithFile(OrderEventLog<F>* log, const char* path) {
    // only init the buffer if not already allocated (LoadFromDisk may have
    // populated it before this call)
    if (!log->entries) OrderEventLog_Init(log);
    std::strncpy(log->disk_path, path, sizeof(log->disk_path) - 1);
    log->disk_path[sizeof(log->disk_path) - 1] = '\0';

    // Check if the file exists (for the header write decision) AND validate its
    // header. D-175a ROTATE-not-append: an existing file whose header is not the
    // current epoch (old magic / version / entry size) must NEVER be appended to —
    // pre-epoch binary events + new decimal events under one header would corrupt
    // replay silently. Rotate it aside and start fresh.
    bool file_exists = false;
    FILE* probe = std::fopen(path, "rb");
    if (probe) {
        file_exists = true;
        OrderEventLogFileHeader phdr;
        bool header_current =
            std::fread(&phdr, sizeof(phdr), 1, probe) == 1 &&
            std::memcmp(phdr.magic, "OMSEL02", 8) == 0 &&
            phdr.format_version == ORDER_EVENT_LOG_FORMAT_VERSION &&
            phdr.fpn_width == F &&
            phdr.entry_size == (uint32_t)sizeof(OrderEvent<F>);
        std::fclose(probe);
        if (!header_current) {
            char rotated[sizeof(log->disk_path) + 32];
            std::snprintf(rotated, sizeof(rotated), "%s.pre-epoch.%lld",
                          path, (long long)time(nullptr));
            if (std::rename(path, rotated) == 0) {
                std::fprintf(stderr, "[OrderEventLog] stale-format log ROTATED: %s -> %s (D-175a)\n",
                             path, rotated);
            } else {
                std::fprintf(stderr, "[OrderEventLog] WARN: could not rotate stale log %s "
                             "(errno=%d); disk persistence DISABLED to protect it\n",
                             path, errno);
                return;  // never append mixed-epoch events
            }
            file_exists = false;  // fresh file gets the new header below
        }
    }

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
        std::memcpy(hdr.magic, "OMSEL02", 8);
        hdr.fpn_width  = F;
        hdr.entry_size = (uint32_t)sizeof(OrderEvent<F>);
        hdr.format_version = ORDER_EVENT_LOG_FORMAT_VERSION;
        std::fwrite(&hdr, sizeof(hdr), 1, log->disk_file);
        std::fflush(log->disk_file);
    }

    std::fprintf(stderr, "[OrderEventLog] disk persistence: %s (%s)\n",
                 path, file_exists ? "appending" : "new file");
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Like Init, but also opens a binary file for write-through. Events are
// appended to disk on every OrderEventLog_Append call. The file carries a
// small header for forward compatibility (magic + FPN_Binary width + entry size).
//
// If the file already exists, LoadFromDisk should be called BEFORE this to
// replay the events into memory. This function opens the file in append
// mode so existing data is preserved.
//======================================================================
// [END_FUNCTION]_[OrderEventLog_InitWithFile]
//======================================================================

//======================================================================
// [FUNCTION]_[OrderEventLog_Reset]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [PERSISTENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[paper-reset truncate — zero memory state + recreate the disk file header-only (v4.7.18)]
//======================================================================
// [CODE]
//======================================================================
// P3-c (D-445): the disk half, extracted so the WRITER-thread truncate intercept and the
// single-threaded Reset share ONE body. Touches writer-owned state only (disk_file + the
// on-disk file) — callable from the writer thread (intercept) or a provably-single-threaded
// context (Reset pre-Start / tests).
template <unsigned F>
inline void OrderEventLog_TruncateDisk(OrderEventLog<F>* log) {
    if (!log->disk_file || log->disk_path[0] == '\0') return;

    // Close the current handle, recreate the file fresh (truncates), reopen
    // for append. Re-writes the header so the file remains valid.
    std::fclose(log->disk_file);
    log->disk_file = std::fopen(log->disk_path, "wb");
    if (!log->disk_file) {
        std::fprintf(stderr, "[OrderEventLog] WARN: reset failed to reopen %s\n",
                     log->disk_path);
        return;
    }
    OrderEventLogFileHeader hdr;
    std::memset(&hdr, 0, sizeof(hdr));
    std::memcpy(hdr.magic, "OMSEL02", 8);
    hdr.fpn_width  = F;
    hdr.entry_size = (uint32_t)sizeof(OrderEvent<F>);
    hdr.format_version = ORDER_EVENT_LOG_FORMAT_VERSION;
    std::fwrite(&hdr, sizeof(hdr), 1, log->disk_file);
    std::fflush(log->disk_file);
    // reopen in append mode so subsequent writes keep going
    std::fclose(log->disk_file);
    log->disk_file = std::fopen(log->disk_path, "ab");
    std::fprintf(stderr, "[OrderEventLog] reset: %s truncated + re-headered\n",
                 log->disk_path);
}

template <unsigned F>
inline void OrderEventLog_Reset(OrderEventLog<F>* log) {
    // DIRECT reset — single-threaded contexts ONLY (pre-Start boot, tests). With the writer
    // thread live, use OrderEventLog_RequestTruncate (D-445 in-band) — a direct Reset here
    // would race the writer's fwrite on disk_file (the Class-50 3-way race this closes).
    // In-memory: clear count + reset id sequence. Buffer stays allocated.
    log->count = 0;
    log->next_event_id.store(1, std::memory_order_relaxed);
    OrderEventLog_TruncateDisk(log);
}

// P3-c (D-445): the IN-BAND reset request — appender-thread-only (the composer), like Append.
// Pushes a control record through the SAME ring as data: FIFO guarantees every pre-reset
// event lands in the OLD file before the truncate (the ack-free property — a side-band flag
// would lose this ordering, which is exactly why the refuted design needed an ACK). The
// composer resets its own id sequence at push time; the writer owns count + disk.
template <unsigned F>
inline void OrderEventLog_RequestTruncate(OrderEventLog<F>* log) {
    if (log->entries == nullptr) return;   // logging disabled
    if (!log->writer_thread_active.load(std::memory_order_acquire)) {
        OrderEventLog_Reset(log);          // no writer = single-threaded context; direct is safe
        return;
    }
    OrderEvent<F> ctrl{};
    ctrl.type = OEVT_CTRL_TRUNCATE;        // never persisted — the writer intercepts
    for (int spin = 0; !SPSCRing_TryPush(&log->async_ring, ctrl); ++spin) {
        log->ring_full_spins.fetch_add(1, std::memory_order_relaxed);
        if (spin < 64) { __builtin_ia32_pause(); } else { usleep(100); }
    }
    log->next_event_id.store(1, std::memory_order_relaxed);
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Truncate the disk event log to header-only (or remove + recreate) and zero
// the in-memory state. Called from the engine's paper-reset handler so a
// fresh boot doesn't replay 40 zombie events from a prior session.
//
// Keeps the same file handle open in append mode after the truncation so
// subsequent appends keep working without reinitializing.
//======================================================================
// [END_FUNCTION]_[OrderEventLog_Reset]
//======================================================================

//======================================================================
// [FUNCTION]_[OrderEventLog_LoadFromDisk]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [PERSISTENCE] [BOOT_TIME]]
// [REFERENCE]_[INVARIANT]_[H21]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[startup replay load — header-gated (OMSEL01 refused loudly, H21); populates the buffer up to capacity]
//======================================================================
// [CODE]
//======================================================================
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
    if (std::memcmp(hdr.magic, "OMSEL01", 8) == 0) {
        // H21 tombstone: pre-epoch BINARY-encoded log — decimal replay would
        // misread every money field. Refuse loudly; InitWithFile rotates it away.
        std::fprintf(stderr, "[OrderEventLog] WARN: %s is a pre-epoch OMSEL01 (binary-money) "
                     "log — refused (H21); will be rotated, not appended\n", path);
        std::fclose(f);
        return -1;
    }
    if (std::memcmp(hdr.magic, "OMSEL02", 8) != 0) {
        std::fprintf(stderr, "[OrderEventLog] WARN: %s bad magic\n", path);
        std::fclose(f);
        return -1;
    }
    if (hdr.format_version != ORDER_EVENT_LOG_FORMAT_VERSION) {
        std::fprintf(stderr, "[OrderEventLog] WARN: %s format_version mismatch "
                     "(file=%llu, build=%u)\n", path,
                     (unsigned long long)hdr.format_version, ORDER_EVENT_LOG_FORMAT_VERSION);
        std::fclose(f);
        return -1;
    }
    if (hdr.fpn_width != F) {
        std::fprintf(stderr, "[OrderEventLog] WARN: %s FPN_Binary width mismatch "
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
    // v5.11.5.C — fixed mmap'd capacity; load up to capacity, then stop.
    // The disk file may have more events than the mmap can hold (long
    // operator history). In that case we load only the most recent
    // capacity-many events would be ideal — for now we load oldest-first
    // up to capacity and warn if the file is bigger. Acceptable since
    // ORDER_EVENT_LOG_MAX_CAPACITY covers months of typical
    // strategy rates (see the constant's v5.11.5.D sizing note).
    int loaded = 0;
    int truncated = 0;
    OrderEvent<F> event;
    while (std::fread(&event, sizeof(event), 1, f) == 1) {
        if (log->count >= log->capacity) {
            truncated++;
            continue;  // drain rest of file (advance next_event_id past it)
        }
        log->entries[log->count++] = event;
        uint64_t cur = log->next_event_id.load(std::memory_order_relaxed);
        if (event.event_id >= cur) {
            log->next_event_id.store(event.event_id + 1, std::memory_order_relaxed);
        }
        loaded++;
    }
    if (truncated > 0) {
        std::fprintf(stderr, "[OrderEventLog] WARN: %s has %d events past "
                     "MAX_CAPACITY=%zu; truncating in-memory log to oldest %zu "
                     "(disk file unchanged; bump ORDER_EVENT_LOG_MAX_CAPACITY "
                     "if older history is needed)\n",
                     path, truncated, ORDER_EVENT_LOG_MAX_CAPACITY,
                     ORDER_EVENT_LOG_MAX_CAPACITY);
    }

    std::fclose(f);
    std::fprintf(stderr, "[OrderEventLog] loaded %d events from %s\n", loaded, path);
    return loaded;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Reads events from a previously-written event log file and populates the
// in-memory buffer. Validates the file header for magic + FPN_Binary width match.
// Returns the number of events loaded, or -1 on error.
//
// Call this BEFORE InitWithFile if the file already exists. The loaded
// events are available for Portfolio_FromEventLog replay.
//======================================================================
// [END_FUNCTION]_[OrderEventLog_LoadFromDisk]
//======================================================================

//======================================================================
// [FUNCTION]_[OrderEvent_MakeFill]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [OMS_DRAINER] [HELPER]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[fill-event builder — zero-inits + fills every field incl. the S-3 booked fee; event_id assigned at Append]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline OrderEvent<F> OrderEvent_MakeFill(uint64_t order_id,
                                          uint64_t timestamp_us,
                                          OrderType order_type,
                                          int16_t node_id,
                                          Money price,
                                          Money qty,
                                          Money tp,
                                          Money sl,
                                          Money fee = Money_Zero()) {
    OrderEvent<F> e;
    std::memset(&e, 0, sizeof(e));
    e.event_id     = 0;  // assigned by Append
    e.order_id     = order_id;
    e.timestamp_us = timestamp_us;
    e.type         = OEVT_FULL_FILL;
    e.order_type   = order_type;
    e.node_id      = node_id;
    e.price        = price;
    e.qty          = qty;
    e.tp           = tp;
    e.sl           = sl;
    e.fee          = fee;          // Ship-B P3 (S-3): booked fee — the log is fee-self-contained
    e.reason[0]    = '\0';
    return e;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Convenience builder for the common case: a full fill from the OMS
// callback. Fills all the fields so the caller doesn't have to zero-init
// and set each one individually.
//======================================================================
// [END_FUNCTION]_[OrderEvent_MakeFill]
//======================================================================

template <unsigned F>
inline OrderEvent<F> OrderEvent_MakeRejection(uint64_t order_id,
                                               uint64_t timestamp_us,
                                               OrderType order_type,
                                               int16_t node_id,
                                               const char* reason_str) {
    OrderEvent<F> e;
    std::memset(&e, 0, sizeof(e));
    e.event_id     = 0;
    e.order_id     = order_id;
    e.timestamp_us = timestamp_us;
    e.type         = OEVT_REJECTED;
    e.order_type   = order_type;
    e.node_id      = node_id;
    if (reason_str) {
        std::strncpy(e.reason, reason_str, sizeof(e.reason) - 1);
        e.reason[sizeof(e.reason) - 1] = '\0';
    }
    return e;
}

//======================================================================
// [STRUCT]_[FoldResult]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [CAPITAL_BEARING] [DECIMAL]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the deterministic event-log fold result — final Money balance + realized P&L + the rebuilt Portfolio + fills-processed count]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
struct FoldResult {
    Money balance;
    Money realized_pnl;
    Portfolio<F> portfolio;
    int    fills_processed;
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
// [SIZE]_[2240B]
// [ALIGN]_[64]
// [CACHE_LINES]_[35]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[FoldResult]
//======================================================================

//======================================================================
// [FUNCTION]_[Portfolio_FromEventLog]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [CAPITAL_BEARING] [DETERMINISM] [BOOT_TIME]]
// [REFERENCE]_[CLASS]_[18]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the deterministic portfolio fold — same events in, same state out; the replay foundation]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline FoldResult<F> Portfolio_FromEventLog(const OrderEventLog<F>* log,
                                            Money starting_balance,
                                            Money fee_rate) {
    FoldResult<F> result;
    result.balance         = starting_balance;
    result.realized_pnl    = Money_Zero();
    result.fills_processed = 0;
    Portfolio_Init(&result.portfolio);

    for (size_t i = 0; i < log->count; ++i) {
        const OrderEvent<F>& e = log->entries[i];
        if (e.type != OEVT_FULL_FILL) continue;

        int slot = (int)e.node_id;
        if (slot < 0 || slot >= MAX_PORTFOLIO_POSITIONS) continue;

        if (e.order_type == ORDER_MARKET_BUY) {
            // Entry fill: open the slot. v5.15.5.F.2 — build a
            // PositionEntryArgs struct that preserves the original event
            // timestamp so hold-time display survives engine restart +
            // replay. Closes the Class-18 mirror between live-entry +
            // replay paths per CLAUDE.md item 19.
            Money notional  = Money_Mul(e.price, e.qty);
            Money entry_fee = Money_Mul(notional, fee_rate);
            PositionEntryArgs<F> args;
            args.entry_price        = e.price;
            args.quantity           = e.qty;
            args.take_profit_price  = e.tp;
            args.stop_loss_price    = e.sl;
            args.entry_fee          = entry_fee;
            args.entry_timestamp_us = e.timestamp_us;  // preserve original
            // pair_index left at default -1; partial-exit pairing across
            // replay is a future enhancement tracked under .F.1.B (would
            // require OrderEvent to carry pair_index too).
            Portfolio_OpenSlot(&result.portfolio, slot, args);
        } else if (e.order_type == ORDER_MARKET_SELL) {
            // Exit fill: close the slot, compute net P&L, update balance.
            // Same math as EventLoop_OnEvent in ControllerEventLoop.hpp.
            Money entry_fee     = result.portfolio.positions[slot].entry_fee;
            Money qty_snap      = result.portfolio.positions[slot].quantity;
            Money gross         = Portfolio_CloseSlot(&result.portfolio, slot, e.price);
            Money exit_notional = Money_Mul(e.price, qty_snap);
            Money exit_fee      = Money_Mul(exit_notional, fee_rate);
            Money total_fee     = Money_Add(entry_fee, exit_fee);
            Money net           = Money_Sub(gross, total_fee);
            result.balance       = Money_Add(result.balance, net);
            result.realized_pnl  = Money_Add(result.realized_pnl, net);
        }
        result.fills_processed++;
    }

    return result;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
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
//======================================================================
// [END_FUNCTION]_[Portfolio_FromEventLog]
//======================================================================

}  // namespace tt
