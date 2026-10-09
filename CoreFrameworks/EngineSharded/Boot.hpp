// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[CoreFrameworks/EngineSharded/Boot.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [BOOT_TIME] [CONCURRENCY] [MONITORING_PLANE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[boot-time shared globals + the SIGINT/SIGTERM handler — C++17 inline single-storage discipline; the synthetic feed's announcement (CS-282) and its two refusals (D-518)]
// [REFERENCE]_[DESIGN_SPEC]_[cpp17-inline-variable-for-header-shared-state]
// [CONTAINS]
//   - [FUNCTION]_[EngineSharded_SignalHandler]
//   - [FUNCTION]_[EngineSharded_SyntheticFeedWarn]
//   - [FUNCTION]_[EngineSharded_SyntheticFeedReminder]
//   - [FUNCTION]_[EngineSharded_SyntheticLiveRefuse]
//   - [FUNCTION]_[EngineSharded_PaperResetRefusedAtRequester]
//   - [FUNCTION]_[EngineSharded_PaperResetRefusedAtExecutor]
//======================================================================================================
// Sub-file of CoreFrameworks/EngineSharded.hpp (split per file-size-split-discipline.md
// at v5.15.5.F.4d.1.B.6; subfolder pattern first canonical).
//
// Contains:
//   - g_engine_sharded_shutdown — file-shared shutdown flag set by SIGINT handler
//   - g_engine_sharded_gui_quit_ptr — pointer to GUI's quit_requested flag (signal lockstep)
//   - EngineSharded_SignalHandler — SIGINT handler installed by EngineSharded_Run
//   - EngineSharded_SyntheticFeedWarn / _SyntheticFeedReminder — a synthetic feed says so, loudly (CS-282)
//   - EngineSharded_SyntheticLiveRefuse — what a synthetic session refuses (D-518)
//   - EngineSharded_PaperResetRefusedAtRequester / _AtExecutor — the paper reset refused outside a PAPER session, at
//     the GUI's request and at the composer (D-518 call 4, D-526 call 4)
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
// synthetic session has: a WARN line (logging/<log_file>), ONE health.jsonl WARN record, and a reminder
// while it lasts. Notify — the one other channel a headless engine has — is OFF in a synthetic session
// (D-522 call 2: a fabricated kill trip must not send a real-looking alert), and the WARN names it. They
// take the line's sink so a cell pins the words through fmemopen; which arm calls them is Run.hpp's (the
// offline boot smoke pins that). Interim by construction: SYN removes the synthetic source and these with
// it (deleting `use_synthetic` red-builds each caller). The paper-reset refusals below are NOT synthetic-only
// — a live boot reaches the GUI's too (CS-300 (b)) — so they stay when SYN lands.
//
// The session they announce is EPHEMERAL (D-514 call 2 / D-515 B12 / D-516 / D-518): each durable sink
// takes its existing off-mode at boot — the OMS's through its session (D-526) — so nothing derived from the
// fabricated ticks is kept but the two logs: engine.log and health.jsonl, whose per-fill entry / exit records
// still write (D-516 call 2); the depth recorder stays on (it records only real market data); notify sends
// nothing (D-522 call 2). Live never runs on synthetic ticks — refused
// before any live init, so `use_synthetic` implies paper for every sink keyed on it.
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
    // the consequence reads the same either way: the session is EPHEMERAL — the line names what it does
    // NOT keep and what it DOES (D-515 B12 (c); CS-300 (c): the health log keeps its per-fill records), and
    // that notify sends nothing (D-522 call 2) — the logs by ROLE, never by file name: both paths are cfg keys
    // (log_file, health_log_path)
    if (forced) {
        // never "unavailable" — the stream was not even tried (I-1 F-2)
        fprintf(out, "[sharded] WARN: SYNTHETIC ticks by request (sharded_force_synthetic=1) — a ~$60,100 "
                     "sawtooth, NOT market data; this run keeps no OMS ledger, snapshot, trade CSVs, calibration "
                     "log, learned state or tick tape and sends no notify alert — it keeps only its two logs (the "
                     "engine log and the health log, its fill records too) and the depth tape (real market data "
                     "only) — set sharded_force_synthetic=0 and restart to trade on market data\n");
    } else {
        fprintf(out, "[sharded] WARN: market data stream UNAVAILABLE (the [BINANCE] line above names the "
                     "failed step) — trading on SYNTHETIC ticks, a ~$60,100 sawtooth that is NOT market data; "
                     "this run keeps no OMS ledger, snapshot, trade CSVs, calibration log, learned state or tick "
                     "tape and sends no notify alert — it keeps only its two logs (the engine log and the health "
                     "log, its fill records too) and the depth tape (real market data only) — fix the network and "
                     "restart\n");
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

//======================================================================
// [FUNCTION]_[EngineSharded_SyntheticLiveRefuse]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BOOT_TIME] [CAPITAL_BEARING] [MONITORING_PLANE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[live NEVER runs on synthetic ticks (D-518 call 3): the boot's FATAL line, worded by the feed's cause with the remedy, plus ONE health.jsonl CRITICAL boot_abort record; the caller returns before any live init]
// [REFERENCE]_[DECISION]_[[D-518]]
//======================================================================
// [CODE]
//======================================================================
inline void EngineSharded_SyntheticLiveRefuse(FILE* out, bool forced) {
    // real orders priced off a fabricated sawtooth would move real money — and the ephemeral session keys every
    // durable sink on the synthetic feed, so live would also lose the ledger for its real fills
    if (forced) {
        fprintf(out, "[sharded] FATAL: trading_mode=live with SYNTHETIC ticks by request "
                     "(sharded_force_synthetic=1) — refusing to start: live trading never runs on fabricated "
                     "prices; set sharded_force_synthetic=0, or trading_mode=paper\n");
    } else {
        fprintf(out, "[sharded] FATAL: trading_mode=live but the market data stream is UNAVAILABLE (the "
                     "[BINANCE] line above names the failed step) — refusing to start: live trading never runs "
                     "on SYNTHETIC ticks; fix the network and restart, or use trading_mode=paper\n");
    }
    // a refusal to boot is operator-blocking — CRITICAL, like the sister boot_abort (D-514 STATUS (iii));
    // free text, never parsed — no identifier coined (H21)
    Health_Log(HEALTH_CRITICAL, "engine", -1, "boot_abort reason=synthetic_feed_live cause=%s",
               forced ? "sharded_force_synthetic" : "market_stream_unavailable");
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[EngineSharded_SyntheticLiveRefuse]
//======================================================================

//======================================================================
// [FUNCTION]_[EngineSharded_PaperResetRefusedAtRequester]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [PRODUCER] [MONITORING_PLANE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the GUI's Reset Paper, refused outside a PAPER session (D-518 call 4; worded at D-526 call 4 / CS-300 (b)): ONE line naming BOTH causes its boot-latched `paper_persist_off` cannot tell apart — a synthetic feed, or a live boot whose running cfg a reload turned to paper; no health record (it prints on the producer thread); its sister at the composer refuses the same request (D-526)]
// [REFERENCE]_[DECISION]_[[D-518] [D-526]]
//======================================================================
// [CODE]
//======================================================================
inline void EngineSharded_PaperResetRefusedAtRequester(FILE* out) {
    fprintf(out, "[sharded] WARN: paper reset REFUSED — this session keeps no paper state: it runs on SYNTHETIC "
                 "ticks (a reset would archive its fabricated state into data/paper_resets/ beside your real "
                 "sessions), or it booted LIVE (a reload to a paper cfg does not make it a paper session) — "
                 "restart as a paper session on the market data stream to reset paper state\n");
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[EngineSharded_PaperResetRefusedAtRequester]
//======================================================================

//======================================================================
// [FUNCTION]_[EngineSharded_PaperResetRefusedAtExecutor]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [OMS_DRAINER] [CAPITAL_BEARING] [MONITORING_PLANE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the paper reset's EXECUTOR refused it — the session is not a PAPER one (D-526 call 4, CS-311 + CS-298's archive half): ONE line, worded by the session kind; in LIVE a defect (no requester asks in a live session), in an ephemeral session nothing to reset; no health record (the composer prints it; its requester-side sister above prints on the producer)]
// [REFERENCE]_[DECISION]_[[D-526] [D-481]]
//======================================================================
// [CODE]
//======================================================================
inline void EngineSharded_PaperResetRefusedAtExecutor(FILE* out, bool live) {
    if (live) {
        // D-481: the OMS-wide kill is restart-only in LIVE, and the reset's OMS_RESET_AUTOPOPULATE would clear it
        fprintf(out, "[sharded] CRITICAL: paper reset REFUSED at the composer — this is a LIVE session: a reset would "
                     "wipe the live OMS and clear a tripped kill switch (restart-only, D-481); no requester asks in a "
                     "live session, so a request that reached the composer is a defect — report it\n");
    } else {
        fprintf(out, "[sharded] WARN: paper reset REFUSED at the composer — this session keeps no paper state (an "
                     "ephemeral session: a synthetic feed): there is nothing to archive or reset — restart on the "
                     "market data stream to reset paper state\n");
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[EngineSharded_PaperResetRefusedAtExecutor]
//======================================================================

} // namespace tt
