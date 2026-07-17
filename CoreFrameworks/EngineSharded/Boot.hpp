// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[CoreFrameworks/EngineSharded/Boot.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [BOOT_TIME] [CONCURRENCY]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[boot-time shared globals + the SIGINT/SIGTERM handler — C++17 inline single-storage discipline]
// [REFERENCE]_[DESIGN_SPEC]_[cpp17-inline-variable-for-header-shared-state]
// [CONTAINS]
//   - [FUNCTION]_[EngineSharded_SignalHandler]
//======================================================================================================
// Sub-file of CoreFrameworks/EngineSharded.hpp (split per file-size-split-discipline.md
// at v5.15.5.F.4d.1.B.6; subfolder pattern first canonical).
//
// Contains:
//   - g_engine_sharded_shutdown — file-shared shutdown flag set by SIGINT handler
//   - g_engine_sharded_gui_quit_ptr — pointer to GUI's quit_requested flag (signal lockstep)
//   - EngineSharded_SignalHandler — SIGINT handler installed by EngineSharded_Run
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

} // namespace tt
