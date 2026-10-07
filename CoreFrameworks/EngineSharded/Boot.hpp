// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[CoreFrameworks/EngineSharded/Boot.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [BOOT_TIME] [CONCURRENCY] [MONITORING_PLANE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[boot-time shared globals + the SIGINT/SIGTERM handler — C++17 inline single-storage discipline; the synthetic feed's announcement (CS-282)]
// [REFERENCE]_[DESIGN_SPEC]_[cpp17-inline-variable-for-header-shared-state]
// [CONTAINS]
//   - [FUNCTION]_[EngineSharded_SignalHandler]
//   - [FUNCTION]_[EngineSharded_SyntheticFeedWarn]
//   - [FUNCTION]_[EngineSharded_SyntheticFeedReminder]
//======================================================================================================
// Sub-file of CoreFrameworks/EngineSharded.hpp (split per file-size-split-discipline.md
// at v5.15.5.F.4d.1.B.6; subfolder pattern first canonical).
//
// Contains:
//   - g_engine_sharded_shutdown — file-shared shutdown flag set by SIGINT handler
//   - g_engine_sharded_gui_quit_ptr — pointer to GUI's quit_requested flag (signal lockstep)
//   - EngineSharded_SignalHandler — SIGINT handler installed by EngineSharded_Run
//   - EngineSharded_SyntheticFeedWarn / _SyntheticFeedReminder — a synthetic feed says so, loudly (CS-282)
//
// **C++17 inline-variable discipline (Decision C of .B.6 plan body):**
// Both globals declared `inline` (not `static`) for single shared storage across all TUs
// that include this header within the same program. CRITICAL: do NOT refactor back to
// `static` — that would give each TU its own private copy → signal handler + drainer +
// producer would see DIFFERENT instances → silent shared-state corruption.
//
// Per NEW DESIGN_SPEC `cpp17-inline-variable-for-header-shared-state.md` (Stage 3 first
// canonical at .B.6 ship close; sister to test_common.hpp pattern from .B.5 WIP-B1).
//======================================================================================================

#pragma once

#include <csignal>     // sig_atomic_t / SIGINT
#include <cstdint>     // uint64_t — the reminder's tick count
#include <cstdio>      // FILE / fprintf — the synthetic feed's lines

#include "../../MemHeaders/HealthLog.hpp"   // Health_Log — the synthetic feed's ONE health.jsonl record

// parent_index: CoreFrameworks/EngineSharded.hpp

namespace tt {

//------------------------------------------------------------------------------------------------------
// [SECTION]_[shutdown flag — set by SIGINT handler; polled by all engine threads]
//------------------------------------------------------------------------------------------------------
// File-shared shutdown flag the SIGINT handler flips. The handler is installed
// only while EngineSharded_Run is active, so this flag is only set when the
// sharded engine is the one that wants to know about it.
//------------------------------------------------------------------------------------------------------
inline volatile std::sig_atomic_t g_engine_sharded_shutdown = 0;

//------------------------------------------------------------------------------------------------------
// [SECTION]_[GUI quit pointer — signal-lockstep with SDL GUI thread]
//------------------------------------------------------------------------------------------------------
// Pointer to the GUI's quit_requested flag, set by EngineSharded_Run after
// g_shared is constructed. The signal handler writes through it so SDL's GUI
// thread (which loops on quit_requested) exits in lockstep with the engine
// threads (which loop on g_engine_sharded_shutdown). Without this, Ctrl+C
// flips g_engine_sharded_shutdown but the GUI thread keeps running until the
// main thread reaches its post-join cleanup — which can hang if SDL's event
// dispatch holds resources the joiner is waiting on. Two flags, one signal.
//------------------------------------------------------------------------------------------------------
inline volatile sig_atomic_t* g_engine_sharded_gui_quit_ptr = nullptr;

//======================================================================
// [FUNCTION]_[EngineSharded_SignalHandler]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BOOT_TIME] [CONCURRENCY]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[SIGINT/SIGTERM -> set both flags — engine threads + the SDL GUI exit in lockstep off one signal]
//======================================================================
// [CODE]
//======================================================================
extern "C" inline void EngineSharded_SignalHandler(int sig) {
    (void)sig;
    g_engine_sharded_shutdown = 1;
    if (g_engine_sharded_gui_quit_ptr) {
        *g_engine_sharded_gui_quit_ptr = 1;
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[EngineSharded_SignalHandler]
//======================================================================

//------------------------------------------------------------------------------------------------------
// [SECTION]_[a synthetic feed says so, loudly — CS-282 (D-513 / D-514 call 1)]
//------------------------------------------------------------------------------------------------------
// When the trade stream does not connect — or the cfg turns it off — the producer fans out a
// fabricated ~$60,100 sawtooth instead. That used to be ONE unmarked line inside the STARTING
// banner, and paper sessions ran on it with nothing calling it out. These say it on every channel a
// headless engine has today: a WARN line (logging/<log_file>), ONE health.jsonl WARN record, and a
// reminder while it lasts. They take the line's sink so a cell pins the words through fmemopen;
// which arm calls them is Run.hpp's (the offline boot smoke pins that). Interim by construction:
// SYN removes the synthetic source and these with it (deleting `use_synthetic` red-builds each caller).
//------------------------------------------------------------------------------------------------------

//======================================================================
// [FUNCTION]_[EngineSharded_SyntheticFeedWarn]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BOOT_TIME] [MONITORING_PLANE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the boot's WARN for a synthetic feed, worded by its cause (the stream failed vs the cfg asked) with the consequence and the remedy, plus ONE health.jsonl WARN record naming the cause]
//======================================================================
// [CODE]
//======================================================================
inline void EngineSharded_SyntheticFeedWarn(FILE* out, bool forced) {
    // the consequence reads the same either way: a synthetic run's fills reach the paper ledger,
    // the snapshot and the learned state exactly like a market run's
    if (forced) {
        // never "unavailable" — the stream was not even tried (I-1 F-2)
        fprintf(out, "[sharded] WARN: SYNTHETIC ticks by request (sharded_force_synthetic=1) — a ~$60,100 "
                     "sawtooth, NOT market data; this run's fills, P&L, snapshot and learned state are "
                     "fabricated — and kept, like a market run's — set sharded_force_synthetic=0 and restart "
                     "to trade on market data\n");
    } else {
        fprintf(out, "[sharded] WARN: market data stream UNAVAILABLE (the [BINANCE] line above names the "
                     "failed step) — trading on SYNTHETIC ticks, a ~$60,100 sawtooth that is NOT market data; "
                     "this run's fills, P&L, snapshot and learned state are fabricated — and kept, like a "
                     "market run's — fix the network and restart\n");
    }
    // free text, never parsed — the cause is the line's own wording key; no identifier coined (H21)
    Health_Log(HEALTH_WARN, "engine", -1, "boot_warn feed=synthetic cause=%s",
               forced ? "sharded_force_synthetic" : "market_stream_unavailable");
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[EngineSharded_SyntheticFeedWarn]
//======================================================================

//======================================================================
// [FUNCTION]_[EngineSharded_SyntheticFeedReminder]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [PRODUCER] [MONITORING_PLANE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the synthetic feed again, once per sawtooth period on the producer — the boot WARN scrolls away, and a reader who starts tailing the log mid-run needs to see this one]
//======================================================================
// [CODE]
//======================================================================
inline void EngineSharded_SyntheticFeedReminder(FILE* out, uint64_t produced) {
    fprintf(out, "[sharded] WARN: still on SYNTHETIC ticks (%llu produced) — NOT market data\n",
            (unsigned long long)produced);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[EngineSharded_SyntheticFeedReminder]
//======================================================================

} // namespace tt
