// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [ENGINE — SHARDED MODE]
//======================================================================================================
//
// Phase 13 of the per-core sharding migration. The "experimental sharded
// engine" entry point that main.cpp dispatches to when engine_mode = sharded.
//
// What this is RIGHT NOW (be honest about it):
//   - A latency testbed that runs the per-core hot path under real
//     concurrency on the user's hardware
//   - Synthetic tick generator (sawtooth around $60k) feeds N execution
//     core threads via SPSC tick rings
//   - Per-core CoreLatencyStats are enabled, so every tick gets sampled
//   - On Ctrl+C the threads join cleanly and per-core latency is dumped
//
// What this is NOT yet:
//   - A real trading path (no Binance websocket integration)
//   - A complete strategy port (only SimpleDip is wired, and the parameter
//     pack in this runner uses fixed thresholds, not Strategy_BuildParameters)
//   - Production-grade in any way — strategy parameter rebuilds and
//     production gating logic are still pending follow-up
//
// Purpose of this file:
//   Lets Jennifer flip `engine_mode = sharded` in engine.cfg, run
//   `./build/engine`, and immediately get real per-core latency numbers
//   under multi-threaded execution on her actual hardware. Side-by-side
//   testable against the legacy live engine (which is reached by leaving
//   engine_mode at single_core, the default).
//
// Shutdown:
//   Catches SIGINT and SIGTERM via the existing main.cpp signal handler
//   pattern. The shutdown flag is checked by every thread loop. All threads
//   exit within ~10ms of the flag being raised.
//
//======================================================================================================
// [FILE SUITE — Subfolder split (v5.15.5.F.4d.1.B.6)]
//======================================================================================================
//
// At v5.15.5.F.4d.1.B.6 this file was split per file-size-split-discipline.md
// (first canonical subfolder application; previously this file was 2,769 lines).
// The bulk of the engine code now lives in CoreFrameworks/EngineSharded/ sub-files.
// This file is the INDEX shim — it pulls the sub-files in the correct order so
// existing callers (`#include "CoreFrameworks/EngineSharded.hpp"`) continue to
// resolve `EngineSharded_Run<F, BENCH>` + sister helpers + globals without source
// changes.
//
// Sub-files in CoreFrameworks/EngineSharded/:
//   - Boot.hpp: g_engine_sharded_shutdown + g_engine_sharded_gui_quit_ptr +
//     EngineSharded_SignalHandler. Both globals are C++17 `inline` (single
//     shared storage across TUs) per NEW DESIGN_SPEC
//     cpp17-inline-variable-for-header-shared-state.md.
//   - SlowPath.hpp: EngineSharded_SlowPath_DrainPostFill (hoist of
//     drain_post_fill lambda) + EngineSharded_SlowPath_DrainManualCloses
//     (MERGED hoist of drain_manual_closes LIVE+NO-OP variants per Decision H).
//   - Async.hpp: g_engine_drainer_cycle_hist inline global +
//     EngineSharded_Async_FanOut (hoist of producer-thread fan_out lambda) +
//     EngineSharded_Async_DrainWithSubmit (hoist of drainer-thread
//     drain_with_submit lambda). File-local-static refs (cores[], tick_rings[],
//     g_tick_rec, g_depth_shared, g_shared, g_candle_acc) passed explicitly
//     because they cannot be referenced from header scope.
//   - Run.hpp: g_sharded_order_lat inline global + EngineSharded_CalibrateTscGhz
//     + EngineSharded_PinThread + EngineSharded_GetSiblingCPU +
//     EngineSharded_SmartSlowPathPins + EngineSharded_DumpLatency<F> +
//     EngineSharded_Run<F, BENCH> (main orchestrator template).
//
// **Decision D — include order:** Boot → SlowPath → Async → Run.
// Run.hpp's EngineSharded_Run body references hoisted helpers + inline globals
// from the other three; Run.hpp also redundantly includes those sub-files for
// self-containment, but the canonical order here ensures availability for any
// future sub-file or external consumer.
//
// **B.5 (queued) will simplify this file further to a ~5-line INDEX shim** —
// just the 4 includes plus a single header comment. Current state preserves
// the original WHAT-THIS-IS narrative for cold-pickup context.
//======================================================================================================

#pragma once

#include "EngineSharded/Boot.hpp"
#include "EngineSharded/SlowPath.hpp"
#include "EngineSharded/Async.hpp"
#include "EngineSharded/Run.hpp"
