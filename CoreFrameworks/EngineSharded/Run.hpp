// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [EngineSharded/Run.hpp — orchestrator + utility helpers + DumpLatency + ANSI TUI]
//======================================================================================================
// Sub-file of CoreFrameworks/EngineSharded.hpp (split per file-size-split-discipline.md
// at v5.15.5.F.4d.1.B.6; subfolder pattern first canonical).
//
// Contains (Phase B Step B.4 extraction; closes the subfolder-split scope):
//   - g_sharded_order_lat — inline ShardedOrderLatency singleton (moved here from
//     EngineSharded.hpp per Decision C placement; co-located with DumpLatency +
//     EngineSharded_Run final dump that reads it). C++17 inline-variable discipline
//     (single shared storage across TUs); sister to Boot.hpp + Async.hpp inline globals.
//   - EngineSharded_CalibrateTscGhz — TSC frequency calibration (~50ms busy work).
//   - EngineSharded_PinThread — best-effort core pinning via pthread_setaffinity_np.
//   - EngineSharded_GetSiblingCPU — SMT sibling read from /sys/devices/system/cpu.
//   - EngineSharded_SmartSlowPathPins — slow-path CPU pin assignment avoiding SMT
//     siblings of producer/hot/drainer threads.
//   - EngineSharded_DumpLatency<F> — per-core latency table dump (used at shutdown).
//   - EngineSharded_Run<F, BENCH> — main orchestrator template. The actual sharded
//     engine entry point that main.cpp dispatches to when engine_mode == ENGINE_MODE_SHARDED.
//     Holds ~30 function-scope `static` objects (BinanceStream / BinanceAdapterState /
//     InitArena / BinanceUserData / NotifyState / TickRecorder / DepthRecorder /
//     DepthSharedState / ReconciliationLoop / tick_rings[] / cores[] / TUISharedState /
//     CandleAccumulator + ShardedTradeLog); these block-scope statics travel with the
//     function definition (function-local storage; not accessible from outside this
//     translation unit). SH_* ANSI color macros + matching #undefs are inside the
//     EngineSharded_Run body (#else USE_IMGUI_GUI branch — ANSI TUI render loop).
//
// **Decision G — atomic extraction unit:** SH_* color macros + EngineSharded_Run body
// + DumpLatency move together (they all share the same file scope post-extract).
//
// **Decision C placement — g_sharded_order_lat:** Co-located with DumpLatency +
// EngineSharded_Run final dump that reads it. Pre-`.B.4` lived in EngineSharded.hpp
// proper; moved here at `.B.4` Phase B Step B.4 since callers all live in this file
// now. `inline` (C++17) discipline preserved — single shared storage across TUs.
//
// **Decision D — sub-file include order:** Run.hpp included LAST in parent shim
// (Boot → SlowPath → Async → Run) so all hoisted helpers (DrainPostFill /
// DrainManualCloses / FanOut / DrainWithSubmit) + globals (g_engine_sharded_shutdown /
// g_engine_sharded_gui_quit_ptr / g_engine_drainer_cycle_hist) referenced inside
// EngineSharded_Run body are available by the time Run.hpp is processed.
//
// **C++17 template explicit-args discipline (B.2 lesson):** ShardedSnapshot_Save<F>
// declared via header include (ShardedSnapshotPersist.hpp) BEFORE the call inside
// EngineSharded_Run body, satisfying C++17 two-phase lookup. Forward-decl-at-global-
// scope (B.3 lesson): TUISharedState + CandleAccumulator forward decls (if needed)
// go OUTSIDE namespace tt to avoid namespace-shadowing pitfall — but here we include
// full headers, so no forward-decls needed.
//======================================================================================================

#pragma once

// Match EngineSharded.hpp's full include set — Run.hpp owns the bulk of the engine
// orchestrator body and its dependencies are the union of all the engine's surfaces.
// (Boot/SlowPath/Async also include the subset each needs; double-include is safe
// via #pragma once.)
#include "../../DataStream/BinanceCrypto.hpp"
#include "../../DataStream/BinanceOrderAPI.hpp"
#include "../../DataStream/TickRecorder.hpp"  // Phase 8a (post-coding c7)
#include "../Notify.hpp"                      // Phase 8b (post-coding c8)
#include "../../FixedPoint/FixedPointN.hpp"
#include "../../ML_Headers/RollingStats.hpp"
#include "../../ML_Headers/BuildFlags.hpp"  // v5.9.5h: BUILD_FLAGS_HASH() for cross-build drift WARN
#include "../LiveReadiness.hpp"  // v5.15.2: LiveReadiness_Verify boot gate + FOREACH_LIVE_READINESS_CHECK
#include "../../Strategies/StrategyParameters.hpp"
#include "../../Strategies/StrategyLifecycle.hpp"  // v5.4.0 Phase 1.2: Strategy_InitPerCore / _FreePerCore
#include "../../DataStream/BinanceUserData.hpp"
#include "../BinanceAdapter.hpp"
#include "../ReconciliationLoop.hpp"
#include "../Reconcile.hpp"  // v5.2.1: boot-time exchange-truth reconcile (parse + decide)
#include "../../MemHeaders/HealthLog.hpp"  // v5.4.0 Phase 0.1: structured JSONL diagnostic log
#include "../ShardedLiveSafety.hpp"   // Phase 0: orphan recovery, force-close, reconcile
#include "../ShardedSnapshot.hpp"
#include "../ShardedSnapshotPersist.hpp"  // Phase 4: persistent state across restarts
#include "../PaperResetArchive.hpp"        // v5.15.5.C.3 Phase 6 — paper-reset archive helpers (Summary_WriteJson + dirname/mkdir)
#include "../../MemHeaders/LatencyHistogram.hpp"  // v5.15.5.C.3 Phase 7.B — drainer-cycle bench gate histogram
#include "../../MemHeaders/DrainerConstants.hpp"  // v5.15.5.C.4 Phase T1 — drainer-thread-stable POD struct (fee_rate_taker_d, drain_count, partial_on)
#include "../../MemHeaders/OmsPhasedDrain.hpp"    // v5.15.5.C.4 Phase F — phase-separated drainer foundation (DrainIntoBuckets + ProcessBucket_*)
#include "../ControllerEventLoop.hpp"
#include "../CoreLatencyStats.hpp"
#include "../ControllerConfig.hpp"
#include "../ExchangeAdapter.hpp"
#include "../ExecutionCore.hpp"
#include "../GateParameters.hpp"
#include "../OrderManager.hpp"
#include "../EngineCommon.hpp"  // v5.15.5.F.4d.1.B.4 — shared train-serve helpers (ApplyBnbDiscount + BootGlobal + BootPerCore + SlowPathCycle*)
#include "../../MemHeaders/OmsPushExitHelper.hpp"  // v5.15.5.C.4 Phase D5 — OMS_PushExitForSlot helper (Class-18 close)
#include "../ShardedOrderLatency.hpp"
#include "../SPSCRing.hpp"
#include "../Tick.hpp"

#ifdef USE_IMGUI_GUI
#include "../../GUI/CandleAccumulator.hpp"
#include "../../GUI/GuiThread.hpp"
#endif

#include <atomic>
#include <chrono>
#include <x86intrin.h>  // v4.7.42: __rdtsc for slow-path latency sampling
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif
#include <unistd.h>  // sysconf(_SC_NPROCESSORS_ONLN) — POSIX, also available on macOS

#include "../EnsembleHotSwap.hpp"   // v5.14.2 — EngineSharded_HotSwapEnsemble template (legacy in-place; superseded v5.15.4 by HotSwap.hpp)
#include "../HotSwap.hpp"           // v5.15.4 — HotSwap_ShadowLoad_{Ensemble,SingleZoo}
#include "../ModelValidation.hpp"   // v5.14.2.E.1 — CoreModelZoo_ValidateAgainstCfg (extracted; PARITY-012)
#include "../../ML_Headers/FeatureRegistryOverlay.hpp"  // v5.14.3.B — FeatureOverlay_PostLoadVerify

// Sister sub-files in CoreFrameworks/EngineSharded/. Pulled in here as well so
// Run.hpp's EngineSharded_Run body can reference the hoisted helpers + inline
// globals (g_engine_sharded_shutdown / g_engine_drainer_cycle_hist / etc.).
// Parent shim (EngineSharded.hpp) includes all 4 sub-files; this redundancy
// keeps Run.hpp self-contained for IDE navigation + future reuse.
#include "Boot.hpp"     // g_engine_sharded_shutdown / g_engine_sharded_gui_quit_ptr / EngineSharded_SignalHandler
#include "SlowPath.hpp" // EngineSharded_SlowPath_DrainPostFill / _DrainManualCloses
#include "Async.hpp"    // g_engine_drainer_cycle_hist / EngineSharded_Async_FanOut / _DrainWithSubmit

// v5.15.5.F.4d.1.B.6 Phase B Step B.4.1: sub-sub-files in Run/ subfolder.
// Second-tier subfolder split to bring Run.hpp under the 1,500-line source-header
// threshold per file-size-split-discipline.md. All sub-sub-files are pure helper
// translation units consumed exclusively by EngineSharded_Run body.
#include "Run/Utilities.hpp"   // EngineSharded_CalibrateTscGhz / _PinThread / _GetSiblingCPU / _SmartSlowPathPins
#include "Run/Latency.hpp"     // g_sharded_order_lat inline singleton + EngineSharded_DumpLatency<F>
#include "Run/AnsiTui.hpp"     // EngineSharded_AnsiTui_RenderLoop<F> — ANSI-build TUI render loop body
#include "Run/SlowPathThreads.hpp"  // EngineSharded_SpawnSlowPathThreads<F> — per-core slow-path thread spawn block
#include "Run/Shutdown.hpp"    // EngineSharded_Shutdown_PreJoin<F> / _PostJoin<F, BENCH> — engine shutdown helpers

// parent_index: CoreFrameworks/EngineSharded.hpp

namespace tt {

// v5.15.5.F.4d.1.B.6 Phase B Step B.4.1: extracted to Run/Utilities.hpp
// (EngineSharded_CalibrateTscGhz + EngineSharded_PinThread +
// EngineSharded_GetSiblingCPU + EngineSharded_SmartSlowPathPins).
// Atomic extraction unit: all four utilities are peers (boot-time topology +
// thread placement helpers). Run.hpp body still references them via this
// include; no caller-side changes needed.
//
// v5.15.5.F.4d.1.B.6 Phase B Step B.4.1: g_sharded_order_lat inline singleton +
// EngineSharded_DumpLatency<F> extracted to Run/Latency.hpp (atomic extraction
// unit: global + template that reads it). Same caller-shape preserved via
// include above; no migration step needed for EngineSharded_Run body.

//======================================================================================================
// [RUN]
//======================================================================================================
// The sharded engine main entry point. Called from main.cpp when
// engine_mode == ENGINE_MODE_SHARDED.
//
// Behavior:
//   1. Calibrate TSC for ns conversion
//   2. Build EventLoopState + N execution cores from config
//   3. Spawn 1 producer thread (synthetic ticks, sawtooth around $60k)
//   4. Spawn N executor threads (one per core, each pinned if possible)
//   5. Spawn 1 drainer thread on the controller core
//   6. Enable per-core CoreLatencyStats
//   7. Run until shutdown_flag is raised
//   8. Join all threads, dump per-core latency
//
// shutdown_flag is the same volatile int main.cpp uses for SIGINT/SIGTERM.
// Pass &g_shutdown_requested or whichever variable you have.
//======================================================================================================

// v5.14.2.E.1 — CoreModelZoo_ValidateAgainstCfg moved to its own header
// (CoreFrameworks/ModelValidation.hpp) so BacktestSharded.hpp can call it
// (closes PARITY-012). Same boundary-stable refactor pattern as
// EnsembleHotSwap.hpp from v5.14.2.A. Function definition unchanged.
// Header is included OUTSIDE namespace tt (at top of file, around line 90).
// v5.15.5.C.2 — old #if 0 definition removed (was kept "for context" since
// v5.14.2.E.1 extracted the function to ModelValidation.hpp). See git
// history pre-v5.15.5.C.2 if archeological reference needed.


// v5.14.2 — EngineSharded_HotSwapEnsemble template lives in its own
// header (CoreFrameworks/EnsembleHotSwap.hpp) so tests can exercise it
// without dragging in the full sharded engine. Definition is included
// once at the top of this file (above namespace tt opening); the call
// site is in EngineSharded_Run below.

//======================================================================================================
// [Phase 7.B — drainer-cycle bench gate histogram]
//======================================================================================================
// `g_engine_drainer_cycle_hist` is now declared in EngineSharded/Async.hpp (v5.15.5.F.4d.1.B.6
// Phase B Step B.2; co-located with drain_with_submit hoist that originated the per-cycle
// rdtsc bracket). Inline (C++17); same single-shared-storage semantics. References from this
// file (LatencyHistogram_Reset call below, drainer thread BENCH-accumulate, shutdown summary
// at end of EngineSharded_Run) all resolve via `tt::g_engine_drainer_cycle_hist`.
//======================================================================================================

template <unsigned F, bool BENCH = false>
static inline void EngineSharded_Run(ControllerConfig<F>& cfg,
                                      const BinanceConfig& bcfg) {
    // Install our own SIGINT/SIGTERM handler so threads can shut down cleanly.
    // Save the previous handlers so we can restore them on exit (in case the
    // legacy engine path runs after us in some test setup).
    g_engine_sharded_shutdown = 0;
    // v5.15.5.C.3 Phase 7.B — reset drainer-cycle bench histogram at boot.
    // Zero-cost when BENCH=false (compile-time elided); at BENCH=true the
    // reset ensures no stale stats from a prior run leak into this session.
    if constexpr (BENCH) {
        LatencyHistogram_Reset(&g_engine_drainer_cycle_hist);
        std::fprintf(stderr,
            "[OMS_BENCH] bench gate ENABLED — drainer cycle latencies will "
            "be recorded into g_engine_drainer_cycle_hist; summary line "
            "emitted at engine shutdown.\n");
    }
    auto prev_int  = std::signal(SIGINT,  EngineSharded_SignalHandler);
    auto prev_term = std::signal(SIGTERM, EngineSharded_SignalHandler);
    // Wire the Binance reconnect helper to our shutdown flag so its delay
    // sleep is interruptible. Without this, closing the GUI during a
    // reconnect window blocks for up to cfg.reconnect_delay seconds.
    g_binance_shutdown_flag = &g_engine_sharded_shutdown;

    // v5.4.0 Phase 0.1 — wire Health log per cfg. Empty path = disabled
    // (Health_Log calls become no-ops). Non-empty path = JSONL output;
    // every Health_Log call appends a line. See MemHeaders/HealthLog.hpp.
    tt::Health_LogConfigure(cfg.health_log_path, cfg.health_log_level);
    if (cfg.health_log_path[0]) {
        tt::Health_Log(tt::HEALTH_INFO, "engine", -1,
            "engine_start arch=sharded num_cores=%u health_log_level=%d",
            (unsigned)cfg.num_execution_cores, cfg.health_log_level);
    }

    // v5.14.9.B — composite-required-for-ladder boot REFUSE.
    // WIP2d-0 (.F.4c.3) — REFACTORED per cfg-scope-discipline.md § Anti-pattern 1
    // (global default + per-instance override FORBIDDEN). Pre-refactor used the
    // legacy global-default-with-override pattern (cfg.risk_degradation_curve flat
    // + cfg.core_overrides[c].risk_degradation_curve). Post-WIP2d-0 walks
    // cfg.cores[c].risk_degradation_curve exclusively over active cores —
    // per-core authoritative per Class 25 + Anti-pattern 1 discipline. The
    // resolution gap (override=0 means inherit-from-global) no longer exists;
    // each core's own value is THE value.
    //
    // Safety semantics preserved: refuse boot if ANY active core has ladder
    // enabled AND confidence_composite_enabled=0 engine-wide. Ladder thresholds
    // are tuned for composite confidence scale [0.001, 0.3]; legacy 3-factor IC
    // scale [0.05, 0.5] would silently misbehave.
    {
        bool any_ladder_active = false;
        int  active_core_id    = -1;
        int  active_curve      = (int)CURVE_OFF;
        for (uint16_t c = 0; c < cfg.num_execution_cores && c < MAX_EXECUTION_CORES; ++c) {
            int curve = cfg.cores[c].risk_degradation_curve;
            if (curve != (int)CURVE_OFF) {
                any_ladder_active = true;
                active_core_id    = (int)c;
                active_curve      = curve;
                break;
            }
        }
        if (any_ladder_active && BITMAP_IS_SET(cfg.ml_cfg_flags, MASK_ML_CFG_CONFIDENCE_COMPOSITE_ENABLED) == 0) {
            fprintf(stderr,
                "[boot] FATAL: core %d risk_degradation_curve=%s requires "
                "confidence_composite_enabled=1.\n"
                "  Ladder thresholds are tuned for composite confidence scale; "
                "legacy 3-factor IC scale would silently misbehave.\n"
                "  Set confidence_composite_enabled=1 OR set "
                "core_%d_risk_degradation_curve=OFF.\n",
                active_core_id,
                DegradationCurve_ToString(active_curve),
                active_core_id);
            if (cfg.health_log_path[0]) {
                tt::Health_Log(tt::HEALTH_CRITICAL, "boot", active_core_id,
                    "REFUSE: ladder requires composite (core=%d, curve=%s, composite=0)",
                    active_core_id, DegradationCurve_ToString(active_curve));
            }
            return;  // refuse boot; engine doesn't start
        }
    }

    // v5.7.2 — hardcoded strategy boot guard. AUTO (regime-gated) is the
    // recommended assignment for live/paper runs because it routes
    // strategy selection through the regime classifier. Hardcoded
    // strategies fire regardless of regime, which is fine for backtest
    // comparison but risky for capital. Per the v5.7 regime audit
    // (DOCS/changelogs/2026-04-30-regime-classifier-audit.md): the
    // 2026-04-30 "MOM enters in RANGING" symptom traced to Core 0
    // being hardcoded MOM, not an AUTO classifier bug.
    //
    // Behavior:
    //   paper run + hardcoded core → WARN to stderr + health log
    //   live run (use_real_money=1) + hardcoded core →
    //     ERROR + abort UNLESS acknowledge_hardcoded_strategy_in_live=1
    {
        int hardcoded_count = 0;
        char hardcoded_list[256] = {0};
        size_t pos = 0;
        for (uint16_t i = 0; i < cfg.num_execution_cores && i < 16; ++i) {
            uint8_t sid = cfg.core_strategies[i];
            if (sid != STRATEGY_AUTO && sid != STRATEGY_NONE) {
                hardcoded_count++;
                if (pos < sizeof(hardcoded_list) - 16) {
                    const char* sname = (sid < NUM_STRATEGIES)
                        ? STRATEGY_SHORT_NAMES[sid] : "?";
                    pos += (size_t)snprintf(hardcoded_list + pos,
                        sizeof(hardcoded_list) - pos,
                        "%score_%u=%s", pos > 0 ? ", " : "", i, sname);
                }
            }
        }
        if (hardcoded_count > 0) {
            const bool live = (cfg.use_real_money != 0);
            const bool ack  = (cfg.acknowledge_hardcoded_strategy_in_live != 0);
            if (live && !ack) {
                fprintf(stderr,
                    "[sharded] ERROR: live mode (use_real_money=1) with "
                    "%d hardcoded strategy core(s): %s\n"
                    "[sharded]        AUTO is recommended for live capital "
                    "(regime-gated strategy selection).\n"
                    "[sharded]        To override: set "
                    "acknowledge_hardcoded_strategy_in_live=1 in engine.cfg.\n",
                    hardcoded_count, hardcoded_list);
                tt::Health_Log(tt::HEALTH_INFO, "engine", -1,
                    "boot_abort reason=hardcoded_strategy_in_live cores=%s",
                    hardcoded_list);
                return;  // refuse to boot
            }
            // paper, or live+ack — WARN only
            fprintf(stderr,
                "[sharded] WARN: %d hardcoded strategy core(s): %s. "
                "AUTO is recommended for live/paper runs (regime-gated). "
                "Hardcoded is fine for backtest comparisons.\n",
                hardcoded_count, hardcoded_list);
            if (cfg.health_log_path[0]) {
                tt::Health_Log(tt::HEALTH_INFO, "engine", -1,
                    "boot_warn hardcoded_strategy_count=%d cores=%s "
                    "live=%d acknowledged=%d",
                    hardcoded_count, hardcoded_list,
                    (int)live, (int)ack);
            }
        }
    }

    // Try to open the real Binance stream. If it fails — or if the cfg
    // explicitly forces synthetic mode — fall back to the synthetic tick
    // generator so the latency testbed still runs.
    static BinanceStream bs;  // static so the producer thread can reach it
    bool use_synthetic;
    if (cfg.sharded_force_synthetic) {
        use_synthetic = true;  // explicit cfg override, don't even try Binance
    } else {
        use_synthetic = !BinanceStream_Init(&bs, &bcfg);
    }

    fprintf(stderr, "\n");
    fprintf(stderr, "================================================================\n");
    fprintf(stderr, "[sharded] STARTING in per-core sharded mode\n");
    fprintf(stderr, "[sharded] num_execution_cores = %u\n", (unsigned)cfg.num_execution_cores);

    // Partial exits P.1 (2026-04-27): validate cfg before allocating cores.
    // When partial_exit_enabled=1, refuses to start if num_execution_cores * 2
    // exceeds MAX_PORTFOLIO_POSITIONS (each core uses 2 portfolio slots in
    // pair mode). When disabled (default), returns 1 unconditionally.
    // Logs the activation line "[partial-exits] enabled: ..." so live engine
    // operators can see whether partials are armed at boot.
    //
    // NOTE: P.1 only validates + announces. Actual partial-exit BEHAVIOR
    // (hot-path TP1 detection, OMS leg-aware booking, strategy dual-TP
    // wiring) lands in P.2-P.4. With partial_exit_enabled=1 in this build,
    // the validation passes but trades still execute single-leg.
    if (!Sharded_ValidatePartialExitCfg(&cfg)) {
        fprintf(stderr, "[sharded] FATAL: partial-exit cfg validation failed. "
                        "Refusing to start.\n");
        return;
    }
    if (use_synthetic) {
        fprintf(stderr, "[sharded] Binance stream not available — using SYNTHETIC ticks\n");
    } else {
        fprintf(stderr, "[sharded] Binance stream connected — using REAL market ticks\n");
        fprintf(stderr, "[sharded] symbol: %s\n", bcfg.symbol);
    }

    // === Live trading setup (use_real_money) ===
    // Mirrors the legacy engine pattern in main.cpp:212-237. Loads secrets,
    // initializes the BinanceAdapter (which spawns one worker thread per
    // BinanceOrderAPI instance — currently 1 worker for phase 02), prints
    // a 10-second warning if running against PRODUCTION (not testnet).
    // The adapter state is static so the drainer lambda can capture it
    // across the function scope.
    //
    // Phase 02 changed this from a single BinanceOrderAPI to a
    // BinanceAdapterState because the underlying BinanceOrderAPI is not
    // thread-safe. The adapter owns one BinanceOrderAPI instance per
    // worker thread (per-thread, not shared). See BinanceAdapter.hpp.
    static BinanceAdapterState g_sharded_binance_adapter;
    bool live_trading = (cfg.use_real_money != 0);
    if (live_trading) {
        char api_key[128] = {}, api_secret[128] = {};
        if (!LoadSecrets("secrets.cfg", api_key, api_secret)) {
            fprintf(stderr, "[sharded] ERROR: use_real_money=1 but secrets.cfg missing or incomplete\n");
            std::signal(SIGINT, prev_int);
            std::signal(SIGTERM, prev_term);
            return;
        }
        const char *rest_host = bcfg.use_testnet ? "testnet.binance.vision" : "api.binance.us";
        // Phase 02 ships with worker_count = 1. Scale up to 2-4 in a
        // follow-on commit after the back-to-back stress test passes.
        if (!BinanceAdapter_Init(&g_sharded_binance_adapter, rest_host,
                                  api_key, api_secret, bcfg.symbol,
                                  &g_sharded_order_lat, /*worker_count=*/1)) {
            fprintf(stderr, "[sharded] ERROR: failed to init BinanceAdapter at %s\n", rest_host);
            std::signal(SIGINT, prev_int);
            std::signal(SIGTERM, prev_term);
            return;
        }
        fprintf(stderr, "[sharded] LIVE TRADING ENABLED on %s — real orders will be placed\n",
                bcfg.use_testnet ? "TESTNET" : "PRODUCTION");
        if (!bcfg.use_testnet) {
            fprintf(stderr, "[sharded] SAFETY: real money. Starting in 10 seconds... (Ctrl+C to abort)\n");
            for (int i = 0; i < 10 && !g_engine_sharded_shutdown; ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            if (g_engine_sharded_shutdown) {
                fprintf(stderr, "[sharded] aborted before start.\n");
                BinanceAdapter_ShutdownState(&g_sharded_binance_adapter);
                std::signal(SIGINT, prev_int);
                std::signal(SIGTERM, prev_term);
                return;
            }
        }
        fprintf(stderr, "[sharded] mode: LIVE — orders to %s\n",
                bcfg.use_testnet ? "TESTNET" : "PRODUCTION");
    } else {
        fprintf(stderr, "[sharded] mode: PAPER — internal state only, no orders submitted.\n");
    }
    fprintf(stderr, "[sharded] Press Ctrl+C to stop and dump per-core stats.\n");
    fprintf(stderr, "================================================================\n");

    ShardedOrderLatency_Reset(&g_sharded_order_lat);

    double tsc_ghz = EngineSharded_CalibrateTscGhz();
    fprintf(stderr, "[sharded] TSC calibrated at %.4f GHz\n", tsc_ghz);

    int num_cores = (int)cfg.num_execution_cores;
    if (num_cores < 1) num_cores = 1;
    if (num_cores > MAX_EXECUTION_CORES) num_cores = MAX_EXECUTION_CORES;

    // Phase 0.1 — orphan BTC recovery + live starting balance.
    // Mirrors legacy main.cpp behavior: query exchange balances at boot, and
    // if any BTC exists (sharded has no persistence yet, so BTC at boot is
    // by definition orphan from a prior session), market-sell to recover
    // USDT. Use the post-recovery USDT as the starting balance for the OMS
    // — the cfg's starting_balance is only used in paper mode.
    //
    // Notify isn't initialized yet (it's set up later, after OMS init), so
    // alerts at this point go to stderr only. That's acceptable: stderr is
    // line-buffered to a file (logging/engine.log), and any failure here
    // is fatal anyway — the user will see it on next start.
    FPN<F> live_starting_balance = cfg.starting_balance;
    if (live_trading) {
        double usdt_recovered = 0.0, btc_remaining = 0.0;
        if (!EngineSharded_OrphanRecovery(&g_sharded_binance_adapter,
                                           /*notify=*/nullptr,
                                           &usdt_recovered,
                                           &btc_remaining)) {
            fprintf(stderr, "[sharded] FATAL: orphan recovery failed — refusing to start with unknown exchange state\n");
            BinanceAdapter_ShutdownState(&g_sharded_binance_adapter);
            std::signal(SIGINT, prev_int);
            std::signal(SIGTERM, prev_term);
            return;
        }
        live_starting_balance = FPN_FromDouble<F>(usdt_recovered);
        fprintf(stderr, "[sharded] LIVE starting balance set from exchange: $%.2f\n",
                usdt_recovered);
    }

    // OMS phase 03 chunk 1B: construct the OMS first with the bank state,
    // then wire the EventLoopState to point at it. Financial state lives
    // in OrderManagerState; EventLoopState is a thin dispatcher.
    ExchangeAdapter<F> exchange_adapter{};  // value-init: all pointers null (paper mode default)
    if (live_trading) {
        exchange_adapter = BinanceAdapter_Get<F>(&g_sharded_binance_adapter);
    }
    OrderManagerState<F> oms;
    // v5.15.5.C.3 (Finding A) — partial_exit_enabled passed via OrderManager_Init.
    // Pre-Finding A: engine called BITMAP_SET(MASK_OMS_STATE_PARTIAL_EXIT_ENABLED)
    // externally after Init returned (Class-18 mirror at the external SET site).
    // Post-Finding A: passed as parameter; the registry walk inside
    // OMS_INIT_AUTOPOPULATE sets the bit via the BIT-kind row.
    int partial_exit_enabled =
        BITMAP_IS_SET(cfg.lifecycle_cfg_flags, MASK_LIFECYCLE_CFG_PARTIAL_EXIT_ENABLED) ? 1 : 0;
    // v5.15.5.F.4c.3 WIP2d-1.B.1 — `fee_rate` arg DELETED. Per-core fee_rate flows via Order pre_resolve.
    OrderManager_Init(&oms, exchange_adapter, live_trading ? 1 : 0,
                      partial_exit_enabled,
                      live_starting_balance,
                      (int)cfg.oms_event_log_mode);
    // v5.13.0.B — open calibration log if cfg.calibration_log_path set.
    // No-op when path is empty (default). Failure is non-fatal (logs to
    // stderr; calibration logging disabled this session).
    OrderManager_OpenCalibrationLog(&oms, cfg.calibration_log_path);
    // v5.15.5.F.4c.3 WIP2d-1.B.1 — Class 27 closure: OMS no longer holds scalar fee_rate /
    // fee_rate_maker / fee_rate_taker / slippage_pct fields. Order pre-resolves these
    // per-core via Order_BindPreResolved at submit time. See:
    //   - DESIGN_SPECS/decision-time-data-binding-pattern.md (the principle)
    //   - DOCS/RECURRING_BUG_PATTERNS.md Class 27 (the anti-pattern this closes)
    //   - CoreFrameworks/Order.hpp Order_BindPreResolved (the binding helper)
    //
    // v4.3.2 (Track C.1) — Binance BNB-pay 25% fee discount, applied per-core at boot
    // so cfg.cores[c].fee_rate_* reflect post-discount values uniformly. Order_BindPreResolved
    // reads cfg.cores[c].fee_rate_* (already discounted) → o->pre_resolved.fee_rate. User
    // must also enable BNB fee payment in Binance UI for this to actually apply on live fills.
    // v5.15.5.F.4d.1.B.4 Step C.1 — extracted to EngineCommon_ApplyBnbDiscount
    // (closes PARITY-030 by-construction; sister BACKTEST caller invokes same helper
    // at Step C.2). Body preserved verbatim from prior inline at LIVE :690-699 →
    // CoreFrameworks/EngineCommon.hpp:152-164. Non-const cfg mutation; ONE-SHOT
    // pre-loop; THE ONLY non-const-cfg helper in EngineCommon.
    EngineCommon_ApplyBnbDiscount(cfg);
    // v4.2.1 paper-mode slippage simulation — also per-core via cfg.cores[c].slippage_pct,
    // pre-resolved onto Order at submit via Order_BindPreResolved. Live mode reads exchange
    // fill prices directly (EventLoop_OnEvent gates on live_trading); slippage value ignored.
    // v5.15.5.C.3 (Finding A) — external PARTIAL_EXIT_ENABLED SET call dropped.
    // Bit is now set inside OMS_INIT_AUTOPOPULATE via the BIT-kind registry row
    // for `partial_exit_enabled` (driven by the parameter passed to OrderManager_Init
    // above). Adding a new cfg-derived boot bit flag = ONE row in
    // FOREACH_OMS_FIELD; no more external SET sites needed.

    // Trade log CSV — same pattern as legacy engine in main.cpp
    static ShardedTradeLog g_sharded_trade_log;
    ShardedTradeLog_Init(&g_sharded_trade_log, bcfg.symbol);
    oms.trade_log = &g_sharded_trade_log;
    // v5.15.5.F.4c.3 WIP2d-1.B.1 r-6 phase 2 — Pattern 5 sink-fn-pointer wire-to-real.
    // Per DESIGN_SPECS/sink-fn-pointer-for-optional-side-effect-pattern.md. Default = noop;
    // set-to-real when trade_log enables → handle_buy_fill / handle_sell_fill dispatch to log emit.
    oms.on_entry_fill_emit = &tt::real_on_entry_fill_emit<F>;
    oms.on_exit_fill_emit  = &tt::real_on_exit_fill_emit<F>;

    // v5.11.6.A — InitArena: single mmap'd region for all init-time
    // allocations (PortfolioController rolling stats × 3 + cumdelta_state +
    // per-core CoreSlowState + per-core strategy state). MAP_POPULATE
    // pre-faults all pages at boot so first slow-path cycle never page-faults.
    //
    // Sizing (measured at boot 2026-05-07):
    //   - CoreSlowState<64> ≈ 278 KB / core × 16 cores = 4.4 MB
    //   - RollingStats × 3 + CumDeltaState  ≈ 60 KB
    //   - Strategy state × 16 cores         ≈ 80 KB
    //   - Headroom for future growth        ≈ ~3 MB
    //   Total: 8 MB. Within the mlockall envelope (RLIMIT_MEMLOCK ≥ 256 MB
    //   per the deployment runbook).
    static tt::InitArena g_init_arena;
    // v5.11.22 — operator-gated MAP_HUGETLB. Default 0 = use 4 KB pages.
    // Set cfg.init_arena_use_hugepages=1 + reserve hugepages at the OS
    // level (sudo sysctl -w vm.nr_hugepages=4) for ~512× fewer TLB
    // entries on the 8 MB arena. InitArena_Create silently falls back
    // to non-HUGETLB on failure (with a stderr WARN) — never fatal.
    int arena_extra_flags = cfg.init_arena_use_hugepages ? MAP_HUGETLB : 0;
    g_init_arena = tt::InitArena_Create(8 * 1024 * 1024, arena_extra_flags);
    tt::InitArena_Global() = &g_init_arena;

    EventLoopState<F> state;
    // v5.15.5.F.4d.1.B.4 Step C.1 — extracted to EngineCommon_BootGlobal
    // (closes PARITY-026 hotfix sister-discipline as part of TECH_DEBT-119
    // structural fold; BACKTEST sister at Step C.2 uses same helper). Body
    // preserved verbatim from prior inline at LIVE :742 + :749-753 + :760-762 →
    // CoreFrameworks/EngineCommon.hpp:181-200 (Init + ConfigureKillSwitch +
    // Regime_Init loop with cfg-driven hysteresis per cfg.cores[i].regime_hysteresis).
    //
    // M5 LIVE-only persistence sinks (DepthRecorder + Notify + trade_log + TickRecorder
    // + BinanceUserData + ReconciliationLoop) stay post-helper at caller scope per
    // Decision G (process-lifetime + thread-shared semantics; not BACKTEST-mirrored).
    EngineCommon_BootGlobal(cfg, state, oms);

    // Phase 04: start the user data websocket for real-time fills.
    // Uses its own BinanceOrderAPI instance for listen key REST calls.
    // The ws_result_queue is a dedicated SPSC ring inside the OMS.
    static BinanceUserDataState g_user_data;
    if (live_trading) {
        char ud_api_key[128] = {}, ud_api_secret[128] = {};
        LoadSecrets("secrets.cfg", ud_api_key, ud_api_secret);
        const char* ws_host = bcfg.use_testnet
            ? "testnet.binance.vision" : "stream.binance.com";
        const char* rest_host = bcfg.use_testnet
            ? "testnet.binance.vision" : "api.binance.us";
        if (BinanceUserData_Init(&g_user_data, ws_host, rest_host,
                                  ud_api_key, ud_api_secret, bcfg.symbol,
                                  &oms.ws_result_queue)) {
            BinanceUserData_Start(&g_user_data);
            g_sharded_binance_adapter.ws_active.store(1, std::memory_order_release);
            fprintf(stderr, "[sharded] user data websocket started\n");
        } else {
            fprintf(stderr, "[sharded] user data websocket init failed, "
                             "falling back to REST-only fills\n");
        }
    }

    // Phase 8b (post-coding c8) — NotifyState for operational alerts.
    // Same setup as legacy path: stderr backend by default, command backend
    // (popen) for dunst/Discord/Slack/Telegram/etc when configured. Off by
    // default (notify_enabled=0). Notify_Send call sites in
    // PortfolioController + BinanceCrypto/Depth/UserData are shared headers
    // — they fire in both modes; g_notify being non-null is what gates
    // actual delivery.
    static NotifyState g_notify_state;
    static NotifyCommandState g_notify_cmd_state;
    if (BITMAP_IS_SET(cfg.ops_cfg_flags, MASK_OPS_CFG_NOTIFY_ENABLED)) {
        NotifyBackendFn backend = NotifyBackend_Stderr;
        void *backend_state = nullptr;
        if (cfg.notify_backend == 1) {
            if (cfg.notify_command[0] == '\0') {
                fprintf(stderr, "[sharded] notify backend=command but notify_command "
                                "is empty — falling back to stderr\n");
            } else {
                strncpy(g_notify_cmd_state.template_str, cfg.notify_command,
                        sizeof(g_notify_cmd_state.template_str) - 1);
                g_notify_cmd_state.template_str[sizeof(g_notify_cmd_state.template_str) - 1] = '\0';
                backend = NotifyBackend_Command;
                backend_state = &g_notify_cmd_state;
            }
        } else if (cfg.notify_backend != 0) {
            fprintf(stderr, "[sharded] notify backend=%d not recognized — "
                            "falling back to stderr\n", cfg.notify_backend);
        }
        NotifyState_Init(&g_notify_state, backend, backend_state,
                         (uint64_t)cfg.notify_cooldown_secs * 1000000ULL);
        if (g_notify_state.worker_started) {
            g_notify = &g_notify_state;
            fprintf(stderr, "[sharded] notify enabled (backend=%s, cooldown=%us)\n",
                    backend == NotifyBackend_Stderr ? "stderr" : "command",
                    cfg.notify_cooldown_secs);
        }
    }

    // Phase 8a (post-coding c7) — TickRecorder for raw market tick CSV audit.
    // Same pattern as legacy path. Off by default (record_ticks=0).
    static TickRecorder g_tick_rec;
    TickRecorder_Init(&g_tick_rec, bcfg.symbol, cfg.record_ticks, cfg.record_max_days);

    // Phase 8a (post-coding c6) — depth feed + DepthRecorder.
    // Same setup as main.cpp's legacy path, runs only when depth_enabled=1.
    // Track E.3 (2026-04-26): book_imbalance is now consumed in the slow
    // path below via EventLoop_RebuildAllParameters. Symmetric with
    // BacktestSharded_Run reading from DepthReplayState.
    static DepthRecorder g_depth_rec;
    static DepthSharedState<F> g_depth_shared;
    static pthread_t g_depth_tid = 0;
    DepthRecorder_Init(&g_depth_rec, bcfg.symbol, "data", cfg.record_max_days,
                       cfg.record_depth && BITMAP_IS_SET(cfg.gate_cfg_flags, MASK_GATE_CFG_DEPTH_ENABLED));
    if (BITMAP_IS_SET(cfg.gate_cfg_flags, MASK_GATE_CFG_DEPTH_ENABLED)) {
        const char *depth_host;
        int depth_port;
        if (bcfg.use_testnet)         { depth_host = "testnet.binance.vision";   depth_port = 443; }
        else if (bcfg.use_binance_us) { depth_host = "stream.binance.us";        depth_port = 9443; }
        else                          { depth_host = "data-stream.binance.vision"; depth_port = 443; }

        if (DepthStream_Init<F>(&g_depth_shared, bcfg.symbol,
                                 depth_host, depth_port,
                                 /*reconnect_delay=*/2) == 0) {
            g_depth_shared.recorder = cfg.record_depth ? &g_depth_rec : NULL;
            pthread_create(&g_depth_tid, NULL, depth_thread_fn<F>, &g_depth_shared);
            fprintf(stderr, "[sharded] depth feed active (%s:%d %s@depth5@100ms)%s\n",
                    depth_host, depth_port, bcfg.symbol,
                    cfg.record_depth ? " — recording" : "");
        } else {
            fprintf(stderr, "[sharded] depth feed init failed — continuing without depth\n");
        }
    }

    // Phase 05: reconciliation poller — own REST instance, periodic
    // balance check, pushes CMD_RECONCILE to oms.reconcile_queue.
    static ReconciliationLoopState<F> g_reconciler;
    if (live_trading) {
        char rc_key[128] = {}, rc_secret[128] = {};
        LoadSecrets("secrets.cfg", rc_key, rc_secret);
        const char* rc_host = bcfg.use_testnet
            ? "testnet.binance.vision" : "api.binance.us";
        if (ReconciliationLoop_Init(&g_reconciler, rc_host, rc_key, rc_secret,
                                     bcfg.symbol, &oms, 30, 0.01)) {
            ReconciliationLoop_Start(&g_reconciler);
        } else {
            fprintf(stderr, "[sharded] reconciler init failed\n");
        }
    }

    // Per-core resources. Static so they live in BSS, not the stack —
    // ExecutionCore is ~66KB and num_cores * size could blow the stack.
    static SPSCRing<Tick<F>, EXECUTION_CORE_TICK_RING_SIZE> tick_rings[MAX_EXECUTION_CORES];
    static ExecutionCore<F> cores[MAX_EXECUTION_CORES];
    // v5.1.2 — full per-core data plane. All rolling stats, regime, flow,
    // depth-history state lives in EACH engine's `state.cores[c].slow_state`
    // (heap-allocated by EventLoopState_Init). Three callers update them:
    //   - centralized:    producer's fan_out + slow-path body via
    //                     EventLoop_UpdateRollingStateAllCores helper
    //   - per_core_slow:  per-core slow-path lambda updates own slow_state
    //   - backtest:       same helper, single-thread, linear iteration
    //
    // Pre-v5.1.2 the producer maintained ONE shared copy at function scope.
    // Removed in v5.1.2; readers now use state.cores[c].slow_state pointers.
    // ema_price stays at producer scope since it's per-tick (replicated to
    // all N engines via EventLoop_UpdateEmaPriceAllCores in fan_out).
    FPN<F> ema_price = FPN_Zero<F>();
    // EMA alpha matches PortfolioController_Init's default (gate_ema_alpha
    // is the cfg key). Computed at boot from cfg; updated each tick.
    FPN<F> ema_alpha = !FPN_IsZero(cfg.gate_ema_alpha)
                       ? cfg.gate_ema_alpha
                       : FPN_FromDouble<F>(0.1);

    // Per-core risk allocation: split the configured starting balance across
    // cores. Each core gets its own mini-portfolio that's risk_pct of the
    // total. With risk_pct=10% and 4 cores starting at $10k, each core gets
    // $250 to risk on a single trade.
    double total_balance = FPN_ToDouble(cfg.starting_balance);
    double default_risk = FPN_ToDouble(cfg.risk_pct);
    if (default_risk <= 0.0) default_risk = 0.10;
    double default_per_core = (total_balance * default_risk) / (double)num_cores;
    if (default_per_core < 1.0) default_per_core = 1.0;

    for (int i = 0; i < num_cores; ++i) {
        // v5.15.5.F.4d.1.B.4 Step C.1 — per-core boot extracted to
        // EngineCommon_BootPerCore (TECH_DEBT-119 closure + closes PARITY-027/028/029
        // by-construction; BACKTEST sister at Step C.2 invokes same helper). Caller
        // owns: core_balance precompute (O2 bytewise-identical math) + ML zoo
        // aligned_alloc with null-check (LIVE-specific; BACKTEST uses Free+Init static
        // array) + post-helper LIVE-only wires (oms.ezoo_refs + CoreLatencyStats_Enable).
        // Helper body preserved verbatim from prior inline at LIVE :908-1177 →
        // CoreFrameworks/EngineCommon.hpp:233-427.

        // Per-core risk: use core-specific override if set, else shared/even split
        // (preserved verbatim per v1.6 O2 bytewise-identical math discipline).
        double core_balance = default_per_core;
        if (!FPN_IsZero(cfg.core_risk_pct[i])) {
            core_balance = total_balance * FPN_ToDouble(cfg.core_risk_pct[i]);
            if (core_balance < 1.0) core_balance = 1.0;
        }

        // ML zoo allocation (LIVE: aligned_alloc heap with null-check; BACKTEST uses
        // Free+Init static array at Step C.2). v5.15.4 — heap-allocate zoo containers
        // for lifecycle consistency with shadow-load (HotSwap_ShadowLoad_* unconditional
        // free(old_ptr) requires heap-resident containers). alignas(64) on container
        // struct (CoreModelZoo + EnsembleModelZoo; v5.15.4.B) means aligned_alloc(64,
        // sizeof(T)) gives the embedded alignment-sensitive members (ModelHandle,
        // RidgeWeights, etc.) correctly-aligned addresses. Process-exit leak acceptable
        // per existing static-array behavior (no shutdown cleanup of internal allocations).
        // Helper handles all load/init/validate/post-load paths internally.
        CoreModelZoo<F>* zoo_ptr = nullptr;
        EnsembleModelZoo<F>* ezoo_ptr = nullptr;
        if (cfg.core_strategies[i] == STRATEGY_ML) {
            zoo_ptr = (CoreModelZoo<F>*)aligned_alloc(64, sizeof(CoreModelZoo<F>));
            if (!zoo_ptr) {
                fprintf(stderr, "[sharded] core %d: aligned_alloc(CoreModelZoo) "
                                "failed; ML core cannot init\n", i);
                CORE_STATE_FLAG_SET(state.cores[i], MODEL_LOAD_FAILED);
                continue;
            }
            ezoo_ptr = (EnsembleModelZoo<F>*)aligned_alloc(64, sizeof(EnsembleModelZoo<F>));
            if (!ezoo_ptr) {
                fprintf(stderr, "[sharded] core %d: aligned_alloc(EnsembleModelZoo) "
                                "failed; ML core cannot init\n", i);
                free(zoo_ptr); zoo_ptr = nullptr;
                CORE_STATE_FLAG_SET(state.cores[i], MODEL_LOAD_FAILED);
                continue;
            }
        }

        // Helper call — closes PARITY-027 (exit-model bind) + PARITY-028 (Bind
        // CompositeCfg + RollingTurnover) + PARITY-029 (Strategy_InitPerCore) by
        // construction. Body covers: SPSCRing_Init + ExecutionCore_Init +
        // EventLoopState_RegisterCore + SetCoreStrategy + full ML branch (load/init/
        // post-load/validate/overlay/ConfidenceScorer/RollingTurnover) +
        // Strategy_InitPerCore + SetPermission. Internal logic identical to prior
        // inline at LIVE :908-1177.
        EngineCommon_BootPerCore(cfg, i, state, tick_rings[i], cores[i],
                                  zoo_ptr, ezoo_ptr,
                                  FPN_FromDouble<F>(core_balance));

        // Post-helper LIVE-only wires (M5 persistence + threading observability;
        // Decision B + Decision G — STAY in caller post-helper return).
        // v5.15.5.F.4d Step 7 § F — wire engine-wide oms->ezoo_refs[i] + core_cfg_refs[i]
        // alongside per-core ctx.ensemble_handle. OmsState is engine-wide single instance;
        // per-core arrays indexed by Order::core_id at calib log emit time
        // (real_on_exit_calibration). void* cast to EnsembleModelZoo<F>* /
        // const PerCoreCfg<F>* at consumer.
        if (state.cores[i].ensemble_handle != nullptr) {
            oms.ezoo_refs[i]     = (void*)ezoo_ptr;
            oms.core_cfg_refs[i] = (const void*)&cfg.cores[i];
        }
        // Per-core latency sampling — the whole point of this mode (LIVE-only;
        // backtest has no latency profiling at this scope per M5 LIVE-only discipline).
        CoreLatencyStats_Enable(&cores[i].latency_stats);
    }

    // Phase 4 — load persisted state. Cores are now registered + initialized
    // with cfg-derived values; this overlays any previous session's regime
    // hysteresis, pnl_feeder, kill switch peak/trips, P&L counters,
    // open positions. Refuses cleanly on bad magic / version / core-count
    // mismatch (logs + starts fresh — never crashes).
    //
    // Snapshot path: data/sharded_snapshot.dat. Live mode skips load —
    // orphan recovery already established the live balance from the
    // exchange, and persisted positions could be wrong if the user manually
    // traded between sessions. Paper mode is the safe-to-resume case.
    {
        const char* snapshot_path = "data/sharded_snapshot.dat";
        // Ensure data/ exists (mkdir is idempotent — silent if it does)
        mkdir("data", 0755);
        if (!live_trading) {
            // v5.15.5.C.2 (S3a + S4): canonical mirror via bit-packed
            // oms_state_flags (S3a); set at line 665 from cfg; drainer-path
            // single source of truth (S4).
            int loaded = ShardedSnapshot_Load<F>(&state, snapshot_path,
                                                  BITMAP_IS_SET(oms.oms_state_flags, tt::MASK_OMS_STATE_PARTIAL_EXIT_ENABLED),
                                                  &cfg);  // v5.5.5
            (void)loaded;  // logged inside; nothing else to do here
        } else {
            // v5.2.2 (live reconciliation Phase 1 wiring): exchange truth
            // is authoritative in live mode. Fetch account + open orders +
            // recent trades and reconcile against local state. Refuses
            // boot on critical disagreement (exchange has BTC, local has
            // 0 positions). Apply path is dry-run by default — flip
            // cfg.reconcile_dry_run=0 deliberately for production after
            // verifying logs.
            //
            // Phase 2 (deferred): actually CANCEL flagged orders, REPLAY
            // missed fills via OrderManager_ProcessFillCommand, FORCE-CLOSE
            // local slots that don't match exchange. Requires
            // BinanceOrderAPI_CancelOrder + Command synthesis helpers
            // that don't exist yet. v5.2.2 ships dry-run + critical
            // refusal — surfaces all the disagreements without applying.
            fprintf(stderr, "[snapshot] LIVE mode: reconciling with exchange truth\n");

            BinanceOrderAPI* api = &g_sharded_binance_adapter.workers_api[0];
            double exch_usdt = 0.0, exch_btc = 0.0;
            BinanceOrderAPI_GetBalances(api, &exch_usdt, &exch_btc);

            char open_buf[16384] = {0};
            char trades_buf[65536] = {0};
            BinanceOrderAPI_GetOpenOrders(api, open_buf, sizeof(open_buf));
            BinanceOrderAPI_GetMyTrades(api, /*since_trade_id=*/0,
                                         trades_buf, sizeof(trades_buf));

            tt::ReconcileOpenOrder orders[16];
            tt::ReconcileTrade     trades[256];
            int n_orders = tt::Reconcile_ParseOpenOrders(open_buf, orders, 16);
            int n_trades = tt::Reconcile_ParseMyTrades(trades_buf, trades, 256);

            int local_open = __builtin_popcount(oms.portfolio.active_bitmap);
            tt::ReconcileResult rr = tt::Reconcile_Decide(
                exch_usdt, exch_btc,
                orders, n_orders,
                trades, n_trades,
                local_open);

            tt::Reconcile_LogReport(rr, cfg.reconcile_dry_run);

            if (rr.refused_boot) {
                fprintf(stderr,
                    "[reconcile] CRITICAL — refusing to boot. "
                    "Resolve disagreement manually then restart.\n");
                BinanceAdapter_ShutdownState(&g_sharded_binance_adapter);
                std::signal(SIGINT,  prev_int);
                std::signal(SIGTERM, prev_term);
                return;
            }

            // v5.14.4.B.1 — 3-mode dispatch (FOREACH_RECONCILE_MODE).
            //
            // SHARDED-ONLY (deep audit 2026-05-09 / TECH_DEBT-002 alignment):
            // centralized engine main.cpp:362 does balance check only, NOT
            // reconciliation. This dispatch lives in EngineSharded boot ONLY.
            // When TECH_DEBT-002 (centralized removal) ships, no migration
            // step needed here. If v5.X+ adds Phase 3 heartbeat reconcile
            // (per Reconcile.hpp:19-24), verify the new dispatch is ALSO
            // sharded-only — DO NOT add to legacy path.
            //
            // FUTURE-THINKING: if mode dispatch goes per-cycle (e.g.,
            // AUTO_SYNC_CONTINUOUS in v5.X+), apply CLAUDE.md item 18(a):
            // template <bool ENABLED> + if constexpr for compile-time
            // elision. Boot dispatch today is operator-initiated +
            // I/O-dominated; branchless irrelevant.
            tt::ReconcileMode mode = (tt::ReconcileMode)cfg.reconcile_mode;
            switch (mode) {
                case tt::RECONCILE_STRICT:
                    // STRICT: refused_boot is the canonical CRITICAL refusal
                    // (already returned above). Additionally refuse if any
                    // cancel_actions surfaced — operator wants ZERO disagreement
                    // tolerance in strict mode (e.g. production deploy where
                    // any zombie order indicates a state-management bug to fix
                    // before continuing).
                    if (rr.cancel_actions > 0) {
                        fprintf(stderr,
                            "[reconcile] STRICT mode + %d cancel_action(s) "
                            "detected (zombie orders or position drift); "
                            "engine boot REFUSED. Set reconcile_mode=warn "
                            "to log + continue, OR reconcile_mode=auto_sync "
                            "to replay missed fills + cancel zombies.\n",
                            rr.cancel_actions);
                        BinanceAdapter_ShutdownState(&g_sharded_binance_adapter);
                        std::signal(SIGINT,  prev_int);
                        std::signal(SIGTERM, prev_term);
                        return;
                    }
                    break;

                case tt::RECONCILE_WARN:
                    // WARN: log + continue. Legacy dry_run=1 behavior preserved.
                    if (rr.cancel_actions > 0) {
                        fprintf(stderr,
                            "[reconcile] WARN mode: %d cancel_action(s) "
                            "detected; continuing (set reconcile_mode=auto_sync "
                            "to apply replay + cancel)\n",
                            rr.cancel_actions);
                    }
                    break;

                case tt::RECONCILE_AUTO_SYNC: {
                    // AUTO_SYNC = composition of N independent helper-actions.
                    // .B.1: Reconcile_ApplyMissedFills (replay; pure OMS state
                    //       mutation; no exchange writes)
                    // .B.2: Reconcile_AutoCancelStale (real exchange cancels
                    //       via lambda-injected BinanceOrderAPI_CancelOrder
                    //       per Option E template-deferred dep injection)
                    // v5.15.5.F.4c.3 WIP2d-1.B.1 — pass cfg.cores so reconciled fills bind
                    // originating-core fee_rate via Order_BindPreResolved (Class 27 cross-core
                    // accuracy closure for in-flight Orders; cores[0] fallback for released).
                    int replayed = tt::Reconcile_ApplyMissedFills(
                        &oms, trades, n_trades, cfg.cores);
                    fprintf(stderr,
                        "[reconcile] AUTO_SYNC replay: %d missed fill(s) "
                        "applied (last_seen_trade_id=%llu)\n",
                        replayed, (unsigned long long)oms.last_seen_trade_id);

                    // v5.14.4.B.2 — zombie order cleanup. Lambda injects
                    // the network primitive (BinanceOrderAPI_CancelOrder)
                    // into the template helper without Reconcile.hpp needing
                    // to take a network include. Symmetric with .B.1's
                    // template-deferred OMS dependency injection.
                    int cancelled = tt::Reconcile_AutoCancelStale(
                        [&](const char* order_id) -> int {
                            return BinanceOrderAPI_CancelOrder(api, order_id);
                        },
                        orders, n_orders);
                    fprintf(stderr,
                        "[reconcile] AUTO_SYNC cancel: %d zombie order(s) "
                        "cancelled (out of %d cancel_action(s) detected)\n",
                        cancelled, rr.cancel_actions);
                    break;
                }
            }
        }
    }

    std::atomic<bool> producer_done{false};
    std::atomic<uint64_t> ticks_produced{0};
    std::atomic<uint64_t> ticks_consumed_total{0};
    // Latest market data for the live TUI panel. Producer writes, render
    // thread reads. Doubles are 8-byte aligned so the read/write is atomic
    // on x86 even without an explicit atomic; we relax that for simplicity
    // since the TUI is informational, not load-bearing.
    std::atomic<double> last_price{0.0};
    std::atomic<double> last_volume{0.0};

    //----------------------------------------------------------------------
    // v5.15.4 — Mode-specific cfg normalize
    //----------------------------------------------------------------------
    // When trading_mode=LIVE, auto-tighten safety defaults the operator
    // hasn't explicitly set: model_verify_strict 0→1 (STRICT);
    // reconcile_mode WARN→STRICT. Explicit operator overrides honored
    // via cfg_keys_explicit bitmap (parser sets bits at parse time).
    // Runs BEFORE LiveReadiness_Verify so the boot gate sees normalized
    // values. Paper/shadow modes are passthrough.
    ControllerConfig_NormalizeForMode<F>(cfg);

    //----------------------------------------------------------------------
    // v5.15.2 — Live-readiness boot gate
    //----------------------------------------------------------------------
    // Pre-flight checklist via FOREACH_LIVE_READINESS_CHECK. When
    // trading_mode=LIVE, REFUSES boot if any LR_SEV_REFUSE-severity check
    // fails. When PAPER or SHADOW, logs failures as WARN-only + proceeds
    // (visibility-by-default so operators see the full checklist at every
    // boot before flipping to live).
    //
    // Drift state read directly from handle->drift_flags_at_load via
    // aggregate_zoo_drift helper — PerCoreSnap.failure_flags isn't
    // populated until snapshot publish (slow-path tail; AFTER pthread
    // spawns). Boot gate runs before pthread spawns; reads from
    // engine-side source of truth.
    {
        int lr_result = tt::LiveReadiness_Verify<F>(cfg, state);
        if (lr_result < 0) {
            fprintf(stderr,
                "[v5.15.2] engine exit: live-readiness REFUSED. "
                "trading_mode=live + pre-flight failure(s). "
                "Either fix the items above or set trading_mode=paper to ship-test.\n");
            return;
        }
    }

    //----------------------------------------------------------------------
    // GUI thread (ImGui build only)
    //----------------------------------------------------------------------
    // Same double-buffered TUISharedState pattern as the legacy engine in
    // main.cpp. The producer thread's slow-path populates TUISnapshot via
    // TUI_CopySnapshotSharded. The GUI thread reads the front buffer.
#ifdef USE_IMGUI_GUI
    static TUISharedState g_shared;
    memset(&g_shared, 0, sizeof(g_shared));
    g_shared.config_path = "engine_sharded.cfg";
    TUISnapshot_InitSeq(&g_shared);  // v5.11.3.B — seq starts at 0 (idx=0, parity=stable)
    g_shared.quit_requested = 0;
    g_shared.pause_requested = 0;
    g_shared.reload_requested = 0;
    g_shared.drag_slot = -1;
    for (int i = 0; i < 16; ++i) g_shared.swap_strategy_requested[i] = STRATEGY_NONE;
    for (int i = 0; i < 16; ++i) g_shared.manual_close_requested[i]  = 0;
    // v5.10.0c — hot model swap state. memset above already zeroed
    // these, but explicit init clarifies intent: 0 = no pending swap.
    for (int i = 0; i < 16; ++i) {
        g_shared.swap_model_path_requested[i] = 0;
        g_shared.pending_model_path[i][0]     = '\0';
    }
    // Wire the signal handler's GUI-quit pointer to this g_shared. After
    // this assignment, SIGINT will set BOTH g_engine_sharded_shutdown AND
    // g_shared.quit_requested in one atomic-ish step, ensuring the GUI
    // thread can drop out of its SDL event loop in the same tick as the
    // engine threads see the shutdown flag.
    g_engine_sharded_gui_quit_ptr = &g_shared.quit_requested;
    static CandleAccumulator g_candle_acc;
    CandleAccumulator_Init(&g_candle_acc, 60);
    g_shared.candle_acc = &g_candle_acc;
    pthread_t gui_tid;
    pthread_create(&gui_tid, NULL, gui_thread_fn, &g_shared);
#endif

    // Reset Paper coordination flag for per-core slow-path threads.
    // Producer's reset handler sets this before touching shared state,
    // slow-paths park on it, producer clears when reset completes.
    // Declared at EngineSharded_Run scope so BOTH the producer thread
    // lambda (which writes it during reset) AND the per-core slow-path
    // lambdas (which read it at top of poll loop) can capture it by
    // reference.
    std::atomic<bool> paper_reset_in_progress{false};

    //----------------------------------------------------------------------
    // v5.0.2 (Engine Topology): function-scope arrays so the GUI publish
    // block (inside the producer thread lambda) can hand them to
    // TUI_PopulateTopology each tick. Hot-path CPUs are derived from the
    // pin convention (CPU i+1 for hot-path i); slow-path CPUs are filled
    // during the slow-path spawn loop further below; -1 = unpinned.
    //----------------------------------------------------------------------
    int  topo_hot_cpu[16];
    int  topo_slow_cpu[16];
    uint32_t topo_poll_interval[16];
    for (int i = 0; i < 16; ++i) {
        topo_hot_cpu[i]  = (i < num_cores) ? (i + 1) : -1;
        topo_slow_cpu[i] = -1;
        topo_poll_interval[i] = (i < num_cores)
            ? ControllerConfig_ResolveForCore(cfg, i).poll_interval
            : 0u;
    }

    //----------------------------------------------------------------------
    // v5.1.3 — fee-floor warning. A profitable round-trip needs gross
    // P&L > 2× fee_rate (entry + exit fees, both at taker rate worst-case).
    // We use 3× as the warning threshold so a winning trade clears fees
    // with at least 1× fee_rate of profit margin (otherwise rounding +
    // slippage erode the edge).
    //
    // Per-core resolved take_profit_pct must be >= 3 × fee_rate_taker.
    // Strategies that emit dynamic TP (e.g. EMA's stddev-based) can still
    // trip the runtime guard inside Strategy_BuildParameters even if cfg
    // looks fine here — boot warning catches static-cfg cases only.
    //----------------------------------------------------------------------
    // v5.15.5.F.4c.3 WIP2d-1.B.1 — boot validation now reads per-core fee_rate_taker
    // (Pattern 2 — per-core in-scope; cfg.cores[i] in loop scope). Each core's tp floor
    // is computed against its own fee_rate_taker since per-core fee rates can differ.
    for (int i = 0; i < num_cores; ++i) {
        double fee_taker_i = FPN_ToDouble(cfg.cores[i].fee_rate_taker);
        if (fee_taker_i <= 0.0) fee_taker_i = FPN_ToDouble(cfg.cores[i].fee_rate);  // fallback
        double tp_floor_i = 3.0 * fee_taker_i;
        double tp_pct = FPN_ToDouble(cfg.cores[i].take_profit_pct);
        if (tp_pct > 0.0 && tp_pct < tp_floor_i) {
            fprintf(stderr,
                "[sharded] WARN: core %d take_profit_pct=%.4f%% is below "
                "the fee floor (3 × taker=%.4f%% = %.4f%%). Winning trades "
                "will be net-negative after fees. Recommend tp_pct >= %.4f%%.\n",
                i, tp_pct * 100.0, fee_taker_i * 100.0,
                tp_floor_i * 100.0, tp_floor_i * 100.0);
        }
    }
    int  topo_producer_cpu = 0;
    int  topo_drainer_cpu  = num_cores + 1;
    long topo_nproc        = sysconf(_SC_NPROCESSORS_ONLN);
    if (topo_nproc < 1) topo_nproc = 1;

    //----------------------------------------------------------------------
    // Producer thread — generates synthetic ticks and fans out to all cores
    //----------------------------------------------------------------------
    std::thread producer([&producer_done, &ticks_produced, &bcfg, &last_price, &last_volume,
                          &cfg, &state, &oms, num_cores, use_synthetic, tsc_ghz,
                          &ema_price, &ema_alpha, live_trading,
                          &paper_reset_in_progress,
                          &topo_hot_cpu, &topo_slow_cpu, &topo_poll_interval,
                          topo_producer_cpu, topo_drainer_cpu, topo_nproc] {
        EngineSharded_PinThread(0);  // best-effort pin to CPU 0
        uint64_t seq = 0;
        // v4.3.1 — slow_path_interval now reads cfg.poll_interval to match
        // backtest (which always used cfg.poll_interval=100 default).
        // Pre-v4.3.1 sharded hardcoded this to 8, which meant rolling
        // stats covered 12.5× less time history at SERVING (sharded) than
        // at TRAINING (backtest) — silent train-serve drift on every
        // RollingStats-derived feature (slope, R², variance, EMA spread,
        // etc.). Aligning eliminates the drift.
        // Cost: sharded slow path fires 12.5× less often. Slow path is
        // not perf-critical (microseconds per fire, runs on dedicated
        // producer CPU). User can override via engine.cfg poll_interval=N
        // for tighter cadence at the cost of train-serve parity.
        int slow_path_interval = (int)cfg.poll_interval;
        if (slow_path_interval < 1) slow_path_interval = 100;
        fprintf(stderr, "[sharded] slow_path_interval = %d ticks "
                        "(from cfg.poll_interval — matches backtest cadence)\n",
                slow_path_interval);
        int slow_path_counter = 0;

        // fan_out body extracted to tt::EngineSharded_Async_FanOut<F>() in
        // EngineSharded/Async.hpp at v5.15.5.F.4d.1.B.6 Phase B Step B.2.
        // Thin wrapper lambda preserves the producer thread's `fan_out(...)`
        // call-site shape (2 callers below: synthetic loop + real Binance loop).
        // Lambda captures everything the hoisted function needs as explicit args
        // and forwards through; under non-GUI build, shared_ptr + candle_acc_ptr
        // are nullptr (the hoisted body's #ifdef gates elide their use).
#ifdef USE_IMGUI_GUI
        TUISharedState*    shared_ptr_for_fanout    = &g_shared;
        CandleAccumulator* candle_acc_ptr_for_fanout = &g_candle_acc;
#else
        TUISharedState*    shared_ptr_for_fanout    = nullptr;
        CandleAccumulator* candle_acc_ptr_for_fanout = nullptr;
#endif
        auto fan_out = [num_cores, &seq, &ticks_produced, &last_price, &last_volume,
                        &cfg, &state, &oms, &slow_path_counter, slow_path_interval, tsc_ghz,
                        &ema_price, ema_alpha, live_trading,
                        &paper_reset_in_progress,
                        &topo_hot_cpu, &topo_slow_cpu, &topo_poll_interval,
                        topo_producer_cpu, topo_drainer_cpu, topo_nproc,
                        shared_ptr_for_fanout, candle_acc_ptr_for_fanout]
                       (double price_d, double volume_d, uint64_t ts_us,
                        int is_buyer_maker = 0) -> bool {  // v4.3 — defaulted for synthetic paths
            return tt::EngineSharded_Async_FanOut<F>(
                price_d, volume_d, ts_us, is_buyer_maker,
                // by-value captures
                num_cores, slow_path_interval, tsc_ghz, ema_alpha, live_trading,
                topo_producer_cpu, topo_drainer_cpu, topo_nproc,
                // by-ref captures
                seq, ticks_produced, last_price, last_volume,
                cfg, state, oms, slow_path_counter, ema_price,
                paper_reset_in_progress,
                topo_hot_cpu, topo_slow_cpu, topo_poll_interval,
                // file-local-static args (block-scope statics in EngineSharded_Run;
                // captured into the wrapper lambda via local access, then forwarded).
                tick_rings, cores, g_tick_rec, g_depth_shared,
                shared_ptr_for_fanout, candle_acc_ptr_for_fanout);
        };

        if (use_synthetic) {
            // SYNTHETIC FALLBACK — sawtooth around $60100 (straddles a sample
            // BG threshold and TP/SL bands). Used when BinanceStream_Init
            // failed (e.g. running offline / no network).
            while (!g_engine_sharded_shutdown) {
                double phase = (double)(seq % 200);
                double price = 60050.0 + (phase < 100.0 ? phase : (200.0 - phase));
                // small random volume variation for realistic candle rendering
                double vol = 1.5 + (double)((seq * 7 + 13) % 100) / 100.0;
                if (!fan_out(price, vol, (uint64_t)(seq * 1000))) break;
                // throttle to ~3 ticks/sec to match real BTC feed cadence.
                // without this the chart gets one massive candle and the
                // GUI oscillates wildly from millions of ticks/sec.
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
            }
        } else {
            // REAL BINANCE FEED — same poll/drain pattern as the legacy
            // engine main loop, but instead of calling
            // PortfolioController_Tick we fan each tick out to N execution
            // core tick rings.
            while (!g_engine_sharded_shutdown) {
                int ready = BinanceStream_Poll(&bs, bcfg.poll_timeout_ms);
                if (g_engine_sharded_shutdown) break;
                if (!(ready & POLL_SOCKET)) continue;
                while (!g_engine_sharded_shutdown) {
                    DataStream<F> ds;
                    int ok = BinanceStream_ReadTick(&bs, &ds);
                    if (!ok) {
                        fprintf(stderr, "[sharded] Binance disconnect — reconnecting\n");
                        BinanceStream_Reconnect(&bs, &bcfg);
                        break;
                    }
                    // Convert to Tick<F>. ds.timestamp_ms is already milliseconds
                    // since epoch from Binance; expand to micros for the per-core
                    // Tick.timestamp field.
                    if (!fan_out(ds.price_d, ds.volume_d,
                                  (uint64_t)time(NULL) * 1000000ULL,
                                  ds.is_buyer_maker)) {
                        break;
                    }
                    if (!BinanceStream_HasPending(&bs)) break;
                }
            }
        }
        producer_done.store(true, std::memory_order_release);
    });

    //----------------------------------------------------------------------
    // Executor threads — one per execution core
    //----------------------------------------------------------------------
    // Lambdas can't capture static arrays directly, so we pass the index
    // in by value and let the lambda body reference the file-scope statics.
    std::vector<std::thread> executors;
    executors.reserve(num_cores);
    for (int i = 0; i < num_cores; ++i) {
        executors.emplace_back([i, &producer_done, &ticks_consumed_total] {
            EngineSharded_PinThread(i + 1);  // CPU 1..N (CPU 0 = producer)
            Tick<F> t;
            uint64_t local_consumed = 0;
            while (!g_engine_sharded_shutdown) {
                if (SPSCRing_TryPop(&tick_rings[i], &t)) {
                    ExecutionCore_Tick(&cores[i], t);
                    local_consumed++;
                }
                if (producer_done.load(std::memory_order_acquire) &&
                    SPSCRing_Depth(&tick_rings[i]) == 0) {
                    break;
                }
            }
            ticks_consumed_total.fetch_add(local_consumed, std::memory_order_relaxed);
        });
    }

    //----------------------------------------------------------------------
    // Drainer thread — controller side
    //----------------------------------------------------------------------
    // open-coded drain so we can hook order submission AFTER each event.
    // identical structure to EventLoop_DrainEvents but with an OrderManager_Submit
    // call inserted between OnEvent and the loop continue, then a single
    // OrderManager_Tick at the end of each pass. paper mode passes through
    // the OMS as a no-op (OrderManager_Tick marks orders FILLED immediately
    // without touching the api); live mode does the synchronous REST call
    // inside OrderManager_Tick and brackets it with steady_clock for the
    // order latency stats. phase 02 will move the REST call to a worker
    // thread so the drainer no longer blocks.
    //
    // why open-code instead of adding a hook to ControllerEventLoop.hpp:
    // keeps the generic event loop free of REST dependencies (it stays
    // testable in isolation), and the live trading concern is contained
    // in this file where it belongs. the OMS itself is generic — the
    // file-specific concern is wiring it into the per-tick drain pattern.
    //
    // qty source rule:
    //   entry: read from cores[slot].intended_qty (matches what OnEvent will
    //          write into portfolio.positions[slot].quantity via OpenSlot)
    //   exit:  read from portfolio.positions[slot].quantity BEFORE OnEvent,
    //          since CloseSlot inside OnEvent clears the slot

    // drain_post_fill + drain_manual_closes lambdas hoisted to
    // EngineSharded/SlowPath.hpp at v5.15.5.F.4d.1.B.6 (Phase B Step B.3).
    //   - EngineSharded_SlowPath_DrainPostFill: thin wrapper around EventLoop_DrainPostFill
    //   - EngineSharded_SlowPath_DrainManualCloses: MERGED LIVE+NO-OP variants per
    //     Decision H. #ifdef USE_IMGUI_GUI gate moved INSIDE function body. Single source
    //     of truth + sister to .B.4 EngineCommon_BootPerCore dual-cfg shape.
    // shared_ptr declared unconditionally (nullptr under non-GUI build) so the
    // call site is unconditional: SlowPath_DrainManualCloses(state, oms, cfg,
    // last_price, shared_ptr). Body's #ifdef inside gates the dereference.
#ifdef USE_IMGUI_GUI
    TUISharedState* shared_ptr = &g_shared;
#else
    TUISharedState* shared_ptr = nullptr;
#endif

    // v5.15.5.F.4d.1.B.6 Phase B Step B.2: drain_with_submit hoisted to
    // EngineSharded/Async.hpp as tt::EngineSharded_Async_DrainWithSubmit<F>().
    // Call sites below pass (state, oms, ticks_produced, cfg) explicitly.
    std::thread drainer([&state, &oms, &ticks_produced, &producer_done,
                         &cfg, &last_price, shared_ptr] {
        // v5.15.5.C.4 Phase T1: &cfg added to capture list for DrainerConstants_Init
        // v5.15.5.C.4 Phase F — drainer-local bucket arrays for phase-separated
        // dispatch. ~7 KB stack allocation; reused per cycle (Reset at top of
        // DrainIntoBuckets). NOT added to OmsState (transient per-cycle scratch).
        tt::OmsDrainBuckets drain_buckets;
        EngineSharded_PinThread(state.registered_count + 1);
        while (!g_engine_sharded_shutdown) {
            // v5.15.5.C.3 Phase 7.B — bench gate per-cycle rdtsc bracket.
            // Wraps the 4-step drainer cycle below. Compile-time elided when
            // BENCH=false (production); zero instructions emitted into the
            // drainer body. When BENCH=true, ~10-15 cycle overhead per
            // cycle (rdtsc + histogram bucket bump + min/max compare).
            uint64_t _bench_t0 = 0;
            if constexpr (BENCH) {
                _bench_t0 = (uint64_t)__rdtsc();
            }
            // Sequence per cycle:
            //   1. drain_with_submit / drain_manual_closes — these push
            //      SubmitCommands into oms.submit_queues (v4.7.37; was direct
            //      OrderManager_Submit calls before Phase B).
            //   2. OMS_DrainSubmit — drainer (sole Submit caller) pops the
            //      queues and calls Submit serially. Preserves OMS contract.
            //   3. OrderManager_Tick — drains result_queue / ws_result_queue
            //      / reconcile_queue, calls HandleFill on completed orders.
            //   4. drain_post_fill — applies per-core CoreContext updates
            //      from FillRecords.
            //
            // v5.15.5.C.4 Phase T1 — DrainerConstants cache (drainer-thread-
            // stable cfg + state predicates). One init per cycle; consumers
            // read fields directly. Replaces prior 6× scattered
            // BITMAP_IS_SET(oms_state_flags, PARTIAL_EXIT_ENABLED) reads.
            const tt::DrainerConstants dc =
                tt::DrainerConstants_Init(state.registered_count, cfg, oms);

            int total_drained = tt::EngineSharded_Async_DrainWithSubmit<F>(state, oms, ticks_produced, cfg);
            EngineSharded_SlowPath_DrainManualCloses(state, oms, cfg, last_price, shared_ptr);
            OMS_DrainSubmit(&oms, dc.drain_count);  // v4.7.37; v5.15.5.C.4 T1 uses cached dc.drain_count

            // v5.15.5.C.4 Phase F — phase-separated drain (replaces unified
            // OrderManager_Tick). Drain commands into per-direction buckets;
            // process Phase A (closes) → Phase A.5 (DrainPostFill consumer
            // pass; reads CLOSE-form Position state — unlocks Phase G+H
            // derives) → Phase B (opens; Portfolio_OpenSlot fires here) →
            // Phase C (reconciles; phase-invariant safe). See
            // DESIGN_SPECS/phase-separated-drainer-for-safe-cross-temporal-derives.md.
            tt::OrderManager_DrainIntoBuckets(&oms, &drain_buckets);
            tt::OrderManager_ProcessBucket_Closes(&oms, &drain_buckets);  // Phase A
            EngineSharded_SlowPath_DrainPostFill(state, oms, cfg);                                              // Phase A.5
            tt::OrderManager_ProcessBucket_Opens(&oms, &drain_buckets);   // Phase B
            tt::OrderManager_ProcessBucket_Reconciles(&oms, &drain_buckets);  // Phase C

            if constexpr (BENCH) {
                uint64_t _bench_dt = (uint64_t)__rdtsc() - _bench_t0;
                LatencyHistogram_Accumulate(&g_engine_drainer_cycle_hist, _bench_dt);
            }

            if (total_drained == 0) std::this_thread::yield();
            if (producer_done.load(std::memory_order_acquire)) {
                for (int k = 0; k < 16; ++k) {
                    tt::EngineSharded_Async_DrainWithSubmit<F>(state, oms, ticks_produced, cfg);
                    EngineSharded_SlowPath_DrainManualCloses(state, oms, cfg, last_price, shared_ptr);
                    // v5.4.1 Bug B2: same partials-aware drain count as the
                    // main loop above. v5.15.5.C.4 Phase T1: per-iter dc
                    // recompute (state may have shifted across k iterations).
                    const tt::DrainerConstants dc_shutdown =
                        tt::DrainerConstants_Init(state.registered_count, cfg, oms);
                    OMS_DrainSubmit(&oms, dc_shutdown.drain_count);  // v4.7.37
                    // v5.15.5.C.4 Phase F — phase-separated drain (shutdown
                    // path mirrors main loop). drain_buckets is the drainer-
                    // thread-local bucket array declared at lambda entry.
                    tt::OrderManager_DrainIntoBuckets(&oms, &drain_buckets);
                    tt::OrderManager_ProcessBucket_Closes(&oms, &drain_buckets);
                    EngineSharded_SlowPath_DrainPostFill(state, oms, cfg);
                    tt::OrderManager_ProcessBucket_Opens(&oms, &drain_buckets);
                    tt::OrderManager_ProcessBucket_Reconciles(&oms, &drain_buckets);
                }
                break;
            }
        }
    });

    // Spawn N per-core slow-path threads. Each runs OneCore helpers
    // (RebuildOneCore, TimeExitOneCore, TrailingSLRatchetOneCore) on
    // a fixed cadence. Producer continues doing GLOBAL work
    // (RollingStats pushes, depth state, snapshot save, GUI publish,
    // account-level KillSwitchEvaluate).
    //
    // v5.0.2: slow-path pinning. cfg.slow_path_pin_offset:
    //   < 0  → no pin (OS-scheduled, original v5.0 behavior)
    //   == 0 → auto: base = drainer_cpu + 1 = num_cores + 2
    //   > 0  → explicit base
    // Slow-path c pins to (base + c) mod nproc. HT-sharing acceptable
    // (slow-path is jitter-tolerant). Best-effort: pin failure is logged
    // and execution continues unpinned.
    //
    // Reset Paper coordination: paper_reset_in_progress atomic declared
    // earlier (~line 786) so fan_out lambda's reset handler can reference
    // it. Slow-paths park (yield) when set; producer's reset handler sets,
    // runs reset, clears.
    // Slow-path threads spawn hoisted to tt::EngineSharded_SpawnSlowPathThreads<F>
    // in EngineSharded/Run/SlowPathThreads.hpp at v5.15.5.F.4d.1.B.6 Phase B Step
    // B.4.1 (~400 LOC moved out). Per-core spawn loop + smart-pin selection +
    // per-thread body (paper-reset park + cadence yield + hot-swap pickup +
    // EngineCommon_SlowPathCycleOneCore dispatch) all live in the helper. Block-
    // scope statics (g_shared + g_depth_shared) passed by reference per B.2 hoist
    // discipline. Slow-path SEMANTICS preserved bytewise — same body, same atomic
    // ordering, same #ifdef gates.
    std::vector<std::thread> slow_paths;
#ifdef USE_IMGUI_GUI
    TUISharedState* g_shared_ptr_for_sp = &g_shared;
#else
    TUISharedState* g_shared_ptr_for_sp = nullptr;
#endif
    EngineSharded_SpawnSlowPathThreads<F>(
        num_cores, cfg, state, oms, cores,
        ticks_produced, last_price, last_volume,
        paper_reset_in_progress,
        topo_producer_cpu, topo_drainer_cpu, topo_nproc,
        topo_slow_cpu,
        g_shared_ptr_for_sp,
        g_depth_shared,
        slow_paths);

#ifdef USE_IMGUI_GUI
    // GUI mode: the GUI thread handles rendering. Just wait for shutdown.
    while (!g_engine_sharded_shutdown) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (g_shared.quit_requested) g_engine_sharded_shutdown = 1;
    }
#else
    // ANSI TUI render loop hoisted to tt::EngineSharded_AnsiTui_RenderLoop<F>
    // in EngineSharded/Run/AnsiTui.hpp at v5.15.5.F.4d.1.B.6 Phase B Step B.4.1
    // (~230 LOC moved out). Block-scope statics (g_user_data + g_reconciler)
    // passed by reference per B.2 hoist discipline. SH_* color macros + #undefs
    // are scoped INSIDE the hoisted function body — no leak into surrounding code.
    EngineSharded_AnsiTui_RenderLoop<F>(
        live_trading, num_cores, tsc_ghz, use_synthetic,
        state, oms,
        last_price, last_volume,
        ticks_produced, ticks_consumed_total,
        cores,
        g_user_data, g_reconciler, g_sharded_order_lat);
#endif  // USE_IMGUI_GUI else (ANSI TUI)

    fprintf(stderr, "[sharded] shutdown requested, joining threads...\n");

    // Pre-join shutdown helpers hoisted to tt::EngineSharded_Shutdown_PreJoin<F>
    // in EngineSharded/Run/Shutdown.hpp at v5.15.5.F.4d.1.B.6 Phase B Step B.4.1
    // (~76 LOC moved out). Final snapshot save (paper mode) + per-core bandit
    // state save + position-still-open advisory.
    EngineSharded_Shutdown_PreJoin<F>(live_trading, num_cores, cfg, state, oms);
    // Per-stage shutdown logging — when the process refuses to exit on close,
    // these tell us WHICH thread is hung. Without them every shutdown bug
    // looks the same from outside ("the engine doesn't die"). Each stage
    // also signals its dedicated quit flag BEFORE joining so the thread has
    // a chance to see it on its next loop iteration. v4.0.1 added per-stage
    // signals after observing producer+depth could be in mid-reconnect with
    // their dedicated flags not yet set.
    fprintf(stderr, "[sharded]   joining depth thread...\n");
    if (g_depth_tid != 0) {
        __atomic_store_n(&g_depth_shared.quit_requested, 1, __ATOMIC_RELEASE);
        pthread_join(g_depth_tid, NULL);
    }
    fprintf(stderr, "[sharded]   joining producer...\n");
    producer.join();
    fprintf(stderr, "[sharded]   joining executors (%d)...\n", num_cores);
    for (auto& e : executors) e.join();
    fprintf(stderr, "[sharded]   joining drainer...\n");
    drainer.join();
    if (!slow_paths.empty()) {
        fprintf(stderr, "[sharded]   joining slow-paths (%zu)...\n", slow_paths.size());
        for (auto& sp : slow_paths) sp.join();
    }
#ifdef USE_IMGUI_GUI
    fprintf(stderr, "[sharded]   joining GUI...\n");
    g_shared.quit_requested = 1;
    pthread_join(gui_tid, NULL);
#endif
    fprintf(stderr, "[sharded]   closing recorders...\n");
    DepthRecorder_Close(&g_depth_rec);
    TickRecorder_Close(&g_tick_rec);  // Phase 8a (post-coding c7)
    fprintf(stderr, "[sharded]   shutting down notify worker...\n");
    if (g_notify) {
        NotifyState_Shutdown(g_notify);
        g_notify = nullptr;
    }
    fprintf(stderr, "[sharded]   shutting down OMS...\n");
    OrderManager_Shutdown(&oms);

    // v5.11.6.A — InitArena destroy MOVED to after per-struct frees
    // (Strategy_FreePerCore loop below at line ~3274). Pre-fix ordering
    // had InitArena_Destroy here BEFORE the strategy free loop, which
    // would zero out InitArena_Global() while strategy frees were still
    // running — InitArena_Owns check in Strategy_FreePerCore would
    // return 0 and `delete` would run on already-unmapped memory.
    // Closes parity-check 2026-05-07 v5.11.6 sprint-exit Finding 7b.

    // Post-join shutdown helpers hoisted to tt::EngineSharded_Shutdown_PostJoin<F, BENCH>
    // in EngineSharded/Run/Shutdown.hpp at v5.15.5.F.4d.1.B.6 Phase B Step B.4.1
    // (~117 LOC moved out). Final counter dump + DumpLatency + ORDER LATENCY dump
    // + OMS COUNTERS dump + reconciler/userdata/adapter shutdown +
    // Strategy_FreePerCore loop + InitArena destroy + BENCH-gated drainer cycle
    // histogram dump + signal handler restore. Block-scope statics passed by
    // reference per B.2 hoist discipline.
    EngineSharded_Shutdown_PostJoin<F, BENCH>(
        live_trading, num_cores, tsc_ghz,
        ticks_produced, ticks_consumed_total,
        state, oms, cores,
        g_reconciler, g_sharded_binance_adapter, g_user_data,
        g_init_arena, g_sharded_order_lat,
        prev_int, prev_term);
}

}  // namespace tt
