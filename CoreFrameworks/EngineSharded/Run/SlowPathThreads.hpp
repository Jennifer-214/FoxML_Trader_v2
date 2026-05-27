// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [EngineSharded/Run/SlowPathThreads.hpp — per-core slow-path thread spawn block]
//======================================================================================================
// Sub-sub-file of CoreFrameworks/EngineSharded/Run.hpp (split per file-size-split-discipline.md
// at v5.15.5.F.4d.1.B.6 Phase B Step B.4.1; second-tier subfolder pattern to bring Run.hpp
// under the 1,500-line source-header threshold).
//
// Contains:
//   - EngineSharded_SpawnSlowPathThreads<F> — smart-pin selection + per-core slow-path
//     pthread spawn loop. Hoisted from EngineSharded_Run inline block (originally
//     ~400 LOC at Run.hpp:1378-1777). Each spawned thread runs a long-lived loop that
//     parks on pause/reset coordination, picks up hot-swap requests (GUI build only),
//     and dispatches per-cycle slow-path work via EngineCommon_SlowPathCycleOneCore.
//
// **CRITICAL — slow-path semantics PRESERVED:** This hoist preserves slow-path
// SEMANTICS exactly:
//   - Same lambda body composition (paper-reset park + cadence yield + hot-swap pickup +
//     EngineCommon_SlowPathCycleOneCore dispatch). Identical atomic-ordering semantics.
//   - Same captures-as-args translation discipline (B.2 Async.hpp lesson — block-scope
//     statics CANNOT be referenced from hoisted header function; pass by reference).
//   - Same SMT-aware pin selection (EngineSharded_SmartSlowPathPins) — no change to pin
//     base / fallback / unpinned logic.
//   - Hot-swap atomics use the same __atomic_load_n / __atomic_store_n primitives with
//     identical memory ordering (__ATOMIC_ACQUIRE / __ATOMIC_RELEASE).
//   - GUI #ifdef gates moved INSIDE the body (paused_engines_mask poll + g_shared
//     hot-swap pickup block) — preprocessor-elided under non-GUI build, function still
//     exists. Sister to SlowPath.hpp DrainManualCloses Decision H merge.
//
// **Block-scope-statics-as-args (B.2 lesson):**
//   - g_shared (TUISharedState) — passed by reference (nullable; nullptr under non-GUI
//     build via caller-side gating, but signature stays uniform via pointer arg). GUI
//     hot-swap block #ifdef gates dereference.
//   - g_depth_shared (DepthSharedState<F>) — passed by reference; consumed inside
//     slow-path cycle dispatch (active_idx + snapshots[] read for BookSnapshot<F>).
//
// **Hot-path discipline:** N/A — this is SLOW PATH per the engine threading model
// (per CLAUDE.md). Hot path = BG_Evaluate + SG_Evaluate + ExecutionCore_Tick living
// in ExecutionCore.hpp + Strategies/. The slow-path thread body dispatches into
// EngineCommon_SlowPathCycleOneCore (already extracted at `.B.4`). Spawn-time setup
// + per-cycle dispatch glue lives here; the actual slow-path WORK lives in
// EngineCommon. Header location has no hot-path latency impact.
//======================================================================================================

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "../../../FixedPoint/FixedPointN.hpp"

#include "../../ControllerConfig.hpp"          // ControllerConfig + ControllerConfig_ResolveForCore
#include "../../ControllerEventLoop.hpp"       // EventLoopState + SP_SECTION_COUNT
#include "../../ExecutionCore.hpp"             // ExecutionCore<F>
#include "../../OrderManager.hpp"              // OrderManagerState
#include "../../CoreLatencyStats.hpp"          // CoreLatencyStats_Enable
#include "../../EngineCommon.hpp"              // EngineCommon_SlowPathCycleOneCore
#include "../../HotSwap.hpp"                   // HotSwap_ShadowLoad_Ensemble / _SingleZoo
#include "../../EnsembleHotSwap.hpp"           // EnsembleModelZoo<F>
#include "../../ModelValidation.hpp"           // CoreModelZoo_ValidateAgainstCfg
#include "../../../ML_Headers/FeatureRegistryOverlay.hpp"  // FeatureOverlay_PostLoadVerify
#include "../../../MemHeaders/CoreStateFlagRegistry.hpp"   // CORE_STATE_FLAG_{SET,CLR}
#include "../../../MemHeaders/BitmapMacros.hpp"            // BITMAP_IS_SET

#include "../../../Strategies/StrategyParameters.hpp"  // STRATEGY_NONE / STRATEGY_ML

#include "../../../DataStream/BinanceDepth.hpp"   // DepthSharedState<F>

#ifdef USE_IMGUI_GUI
#include "../../../DataStream/EngineTUI.hpp"      // TUISharedState
#else
// Forward-decl at global scope (NOT inside namespace tt) — same discipline as
// SlowPath.hpp DrainManualCloses (.B.6 Phase B.3 lesson). Body's #ifdef gate
// elides any actual dereference under non-GUI build.
struct TUISharedState;
#endif

#include "../Boot.hpp"  // g_engine_sharded_shutdown — polled inside thread body
#include "Utilities.hpp"  // EngineSharded_PinThread / EngineSharded_SmartSlowPathPins

// parent_index: CoreFrameworks/EngineSharded/Run.hpp

namespace tt {

//======================================================================================================
// EngineSharded_SpawnSlowPathThreads — smart-pin selection + per-core slow-path spawn loop
//======================================================================================================
// Spawns N per-core slow-path threads. Each thread runs OneCore helpers
// (EngineCommon_SlowPathCycleOneCore dispatches into RebuildOneCore +
// TimeExitOneCore + TrailingSLRatchetOneCore) on a fixed per-core cadence.
//
// v5.0.2: slow-path pinning. cfg.slow_path_pin_offset:
//   < 0  → no pin (OS-scheduled, original v5.0 behavior)
//   == 0 → auto: SmartSlowPathPins picks SMT-aware idle CPUs
//   > 0  → explicit base
//
// Each thread body parks on: (1) GUI pause mask (USE_IMGUI_GUI only), (2) paper_reset_in_progress
// coordination atomic (producer thread sets during reset). Cadence: wakes when enough ticks
// have passed since last_seen_tick. Picks up hot-swap requests (strategy swap + model swap)
// from g_shared if any (USE_IMGUI_GUI only).
//
// Args (explicit captures-as-args per B.2 discipline; block-scope statics passed by reference;
// per-EngineSharded_Run scalars + arrays passed by value or reference):
//   - num_cores: per-core count for spawn loop iteration.
//   - cfg: ControllerConfig<F> ref for slow_path_pin_offset / poll_interval / acknowledge_hot_swap*
//   - state: EventLoopState<F> ref for per-core slow-path latency Enable + slow-path cycle
//     dispatch (state.cores[c].slow_state inside helper).
//   - oms: OrderManagerState<F> ref for OMS access from cycle dispatch.
//   - cores: ExecutionCore<F> array (not directly read here; passed for parity with original
//     lambda captures — slow_state ref carries the actual data path).
//   - ticks_produced: atomic counter producer writes; slow-paths read for cadence gating.
//   - last_price / last_volume: atomic doubles producer writes; slow-paths read for cycle inputs.
//   - paper_reset_in_progress: atomic flag producer's reset handler sets; slow-paths park while set.
//   - topo_producer_cpu / topo_drainer_cpu: hot CPU IDs for SmartSlowPathPins.
//   - topo_nproc: total processor count.
//   - topo_slow_cpu: output array — chosen slow-path CPU per core (or -1) for topology panel.
//   - g_shared: TUISharedState block-scope static — passed by reference (nullable under
//     non-GUI build via gating; USE_IMGUI_GUI body deref guards usage).
//   - g_depth_shared: DepthSharedState<F> block-scope static — passed by reference for
//     BookSnapshot<F> read inside slow-path cycle dispatch.
//   - slow_paths: output thread vector — emplaced threads owned by caller for later join.
//
// Returns when all N threads are spawned (does NOT wait for them — they run until
// g_engine_sharded_shutdown is set, then exit).
//======================================================================================================
template <unsigned F>
inline void EngineSharded_SpawnSlowPathThreads(
    int num_cores,
    ControllerConfig<F>& cfg,
    EventLoopState<F>& state,
    OrderManagerState<F>& oms,
    ExecutionCore<F>* cores,
    std::atomic<uint64_t>& ticks_produced,
    std::atomic<double>& last_price,
    std::atomic<double>& last_volume,
    std::atomic<bool>& paper_reset_in_progress,
    int topo_producer_cpu,
    int topo_drainer_cpu,
    long topo_nproc,
    int* topo_slow_cpu,
    TUISharedState* g_shared_ptr,
    DepthSharedState<F>& g_depth_shared,
    std::vector<std::thread>& slow_paths
) {
    // v5.1.5: smart pin selection. With cfg.slow_path_pin_offset == 0
    // (auto), use SmartSlowPathPins to AVOID landing on SMT siblings of
    // producer/hot-path/drainer CPUs. Pre-v5.1.5 the auto-derive was a
    // simple (num_cores + 2 + c) % nproc, which on a 16-thread Intel
    // (8 phys × 2 SMT) put slow-paths 2,3 on CPUs 8,9 = SMT siblings of
    // producer/hot-path-0 → 2× slowdown vs slow-paths 0,1 on idle
    // physical cores 6,7. Smart logic prefers truly-idle CPUs first,
    // then SMT siblings of idle cores, then falls back gracefully.
    int sp_pins[16];
    long nproc = topo_nproc;
    bool smart_ok = false;
    int sp_pin_base = -1;  // <0 = no pin
    if (cfg.slow_path_pin_offset == 0) {
        smart_ok = EngineSharded_SmartSlowPathPins(
            /*producer_cpu=*/topo_producer_cpu,
            /*drainer_cpu=*/topo_drainer_cpu,
            /*num_hot=*/num_cores,
            /*num_slow=*/num_cores,
            sp_pins);
        if (smart_ok) {
            sp_pin_base = sp_pins[0];  // for log only
            fprintf(stderr, "[sharded] spawning %d "
                            "slow-path threads (smart pin: ", num_cores);
            for (int c = 0; c < num_cores; ++c) {
                fprintf(stderr, "%s%d", c ? "," : "", sp_pins[c]);
            }
            fprintf(stderr, ", nproc %ld)\n", nproc);
        } else {
            // Fallback: simple (num_cores + 2 + c) % nproc
            sp_pin_base = num_cores + 2;
            fprintf(stderr, "[sharded] spawning %d "
                            "slow-path threads (smart pin failed; fallback base CPU %d, nproc %ld)\n",
                    num_cores, sp_pin_base, nproc);
        }
    } else if (cfg.slow_path_pin_offset > 0) {
        sp_pin_base = cfg.slow_path_pin_offset;
        fprintf(stderr, "[sharded] spawning %d "
                        "slow-path threads (explicit pin base CPU %d, nproc %ld)\n",
                num_cores, sp_pin_base, nproc);
    } else {
        // < 0: no pin
        fprintf(stderr, "[sharded] spawning %d "
                        "slow-path threads (UNPINNED, slow_path_pin_offset=%d)\n",
                num_cores, cfg.slow_path_pin_offset);
    }
    slow_paths.reserve(num_cores);
    for (int c = 0; c < num_cores; ++c) {
        int sp_cpu;
        if (smart_ok) {
            sp_cpu = sp_pins[c];
        } else if (sp_pin_base >= 0) {
            sp_cpu = (int)((sp_pin_base + c) % nproc);
        } else {
            sp_cpu = -1;
        }
        topo_slow_cpu[c] = sp_cpu;  // v5.0.2: capture for topology panel
        slow_paths.emplace_back([c, sp_cpu, &state, &oms, &cores, &cfg,
                                  &ticks_produced, &last_price, &last_volume,
                                  &paper_reset_in_progress,
                                  g_shared_ptr, &g_depth_shared]() {
            (void)cores; // capture parity with original lambda; per-core access via state.cores[c]
            // v5.0.2: best-effort pin to chosen CPU. Failure logged,
            // execution continues unpinned.
            if (sp_cpu >= 0) {
                EngineSharded_PinThread(sp_cpu);
            }
            // v4.7.40 (Phase D): per-core poll interval from resolved
            // cfg. If core has core_N_poll_interval set, use that;
            // otherwise inherit global cfg.poll_interval.
            ControllerConfig<F> resolved_init =
                ControllerConfig_ResolveForCore(cfg, c);
            int slow_path_interval = (int)resolved_init.poll_interval;
            if (slow_path_interval < 1) slow_path_interval = 100;
            fprintf(stderr,
                "[slow-path-%d] poll_interval=%d ticks (override=%u, global=%u) "
                "pinned_cpu=%d\n",
                c, slow_path_interval,
                (unsigned)cfg.core_overrides[c].poll_interval,
                (unsigned)cfg.poll_interval,
                sp_cpu);
            // v4.7.42 (Phase E): enable per-core slow-path latency stats.
            // Sampled around the per-cycle work below (RebuildOneCore +
            // PushParameters + TimeExitOneCore + TrailingSL + permission).
            CoreLatencyStats_Enable(&state.display_meta[c].slow_path_latency);
            // v5.1.1: enable per-section breakdown stats.
            for (int s = 0; s < tt::SP_SECTION_COUNT; ++s) {
                CoreLatencyStats_Enable(&state.display_meta[c].slow_path_breakdown[s]);
            }
            uint64_t last_seen_tick = 0;
            while (!g_engine_sharded_shutdown) {
                // v5.0.3: user pause via paused_engines_mask bit c.
#ifdef USE_IMGUI_GUI
                if (g_shared_ptr->paused_engines_mask & (uint16_t)(1u << c)) {
                    state.cores[c].sp_telemetry.state.store(3, std::memory_order_relaxed);
                    state.cores[c].sp_telemetry.yield_count.fetch_add(1, std::memory_order_relaxed);
                    std::this_thread::yield();
                    continue;
                }
#endif
                // Reset Paper coordination — park while reset runs.
                if (paper_reset_in_progress.load(std::memory_order_acquire)) {
                    state.cores[c].sp_telemetry.state.store(1, std::memory_order_relaxed);
                    state.cores[c].sp_telemetry.yield_count.fetch_add(1, std::memory_order_relaxed);
                    std::this_thread::yield();
                    continue;
                }
                // Cadence — wake when enough ticks have passed.
                uint64_t now_tick = ticks_produced.load(std::memory_order_acquire);
                if (now_tick - last_seen_tick < (uint64_t)slow_path_interval) {
                    state.cores[c].sp_telemetry.state.store(2, std::memory_order_relaxed);
                    state.cores[c].sp_telemetry.yield_count.fetch_add(1, std::memory_order_relaxed);
                    std::this_thread::yield();
                    continue;
                }
                last_seen_tick = now_tick;
                state.cores[c].sp_telemetry.state.store(0, std::memory_order_relaxed);  // running

                // v5.15.5.F.4d.1.B.4 Step C.3 — slow-path latency telemetry
                // (v4.7.42 outer bracket + v5.1.1 per-section breakdown) moved
                // INSIDE EngineCommon_SlowPathCycleOneCore body per v1.7.3 HIGH-4
                // (Telemetry Path A INTERNAL); helper computes its own _sp_t0 +
                // 5 CoreLatencyStats_Sample calls.

                // Skip cores with STRATEGY_NONE (caller responsibility per
                // OneCore contract; OneCore would no-op on STRATEGY_NONE
                // body because Strategy_BuildParameters dispatcher skips it,
                // but the explicit check here is cheap and clarifying).
                if (state.cores[c].strategy_id == STRATEGY_NONE) continue;

                // === Per-core swap-pending pickup ===
                // Mirrors the producer-thread swap walker — but checks only
                // THIS core's request slot. Race-safe: __atomic_load gives
                // acquire on g_shared.swap_strategy_requested[c].
#ifdef USE_IMGUI_GUI
                {
                    uint8_t pending = __atomic_load_n(
                        &g_shared_ptr->swap_strategy_requested[c],
                        __ATOMIC_ACQUIRE);
                    if (pending != STRATEGY_NONE) {
                        // v5.15.5.C.2 (S3a) — bit-packed in oms_state_flags.
                        int partial_on = BITMAP_IS_SET(state.oms->oms_state_flags, tt::MASK_OMS_STATE_PARTIAL_EXIT_ENABLED);
                        uint16_t open_mask = partial_on
                            ? (uint16_t)((1u << (c * 2)) | (1u << (c * 2 + 1)))
                            : (uint16_t)(1u << c);
                        if ((state.oms->portfolio.active_bitmap & open_mask) == 0) {
                            if (pending == STRATEGY_ML &&
                                state.cores[c].model_handle == NULL) {
                                fprintf(stderr,
                                    "[slow-path-%d] refusing swap to ML — "
                                    "no model loaded\n", c);
                                __atomic_store_n(
                                    &g_shared_ptr->swap_strategy_requested[c],
                                    STRATEGY_NONE, __ATOMIC_RELEASE);
                            } else {
                                uint8_t old_strat = state.cores[c].strategy_id;
                                state.cores[c].strategy_id = pending;
                                __atomic_store_n(
                                    &g_shared_ptr->swap_strategy_requested[c],
                                    STRATEGY_NONE, __ATOMIC_RELEASE);
                                fprintf(stderr,
                                    "[slow-path-%d] strategy swapped %u -> %u\n",
                                    c, (unsigned)old_strat, (unsigned)pending);
                            }
                        }
                        // else: position open; leave pending — try next cycle
                    }
                }
                // === v5.10.0c — Per-core model hot-swap pickup ===
                // Mirrors the strategy hot-swap pattern above, but the
                // payload is a directory path (string) instead of a
                // strategy id (uint8). On request: free + reinit + reload
                // ml_zoos[c] from the new dir; update model_handle on
                // success, NULL it on failure (engine falls back to
                // SimpleDip via ML_BuildParameters dispatcher).
                {
                    uint8_t mswap = __atomic_load_n(
                        &g_shared_ptr->swap_model_path_requested[c],
                        __ATOMIC_ACQUIRE);
                    if (mswap) {
                        // v5.15.5.C.2 (S3a) — bit-packed in oms_state_flags.
                        int partial_on = BITMAP_IS_SET(state.oms->oms_state_flags, tt::MASK_OMS_STATE_PARTIAL_EXIT_ENABLED);
                        uint16_t open_mask = partial_on
                            ? (uint16_t)((1u << (c * 2)) | (1u << (c * 2 + 1)))
                            : (uint16_t)(1u << c);
                        int has_open = (state.oms->portfolio.active_bitmap & open_mask) != 0;

                        if (has_open && !cfg.acknowledge_hot_swap_with_open_positions) {
                            // Defer: leave flag set, retry next slow-path cycle.
                            // Operator opts in via cfg field if they want
                            // entry+exit on different models.
                        } else {
                            char new_path[256];
                            strncpy(new_path, g_shared_ptr->pending_model_path[c], 255);
                            new_path[255] = '\0';

                            if (new_path[0] == '\0') {
                                fprintf(stderr,
                                    "[hot_swap] core %d REFUSED: empty path\n", c);
                                __atomic_store_n(
                                    &g_shared_ptr->swap_model_path_requested[c], 0,
                                    __ATOMIC_RELEASE);
                            } else {
                                // Cast model_handle back to the typed zoo
                                // pointer set at boot (line ~805). NULL
                                // means the core wasn't STRATEGY_ML at
                                // boot, so no zoo storage was allocated;
                                // hot-swap requires operator pre-config.
                                CoreModelZoo<F>* swap_zoo =
                                    (CoreModelZoo<F>*)state.cores[c].model_handle;
                                if (swap_zoo == nullptr) {
                                    fprintf(stderr,
                                        "[hot_swap] core %d REFUSED: "
                                        "core not ML at boot (set "
                                        "core_%d_strategy=ml + restart "
                                        "to enable hot-swap)\n", c, c);
                                    __atomic_store_n(
                                        &g_shared_ptr->swap_model_path_requested[c], 0,
                                        __ATOMIC_RELEASE);
                                } else if (state.cores[c].ensemble_handle != nullptr) {
                                    // v5.15.4 — ENSEMBLE SHADOW-LOAD HOT-SWAP.
                                    // Replaces v5.14.2's in-place Free+Init+Load
                                    // pattern (now legacy in EnsembleHotSwap.hpp;
                                    // kept compiled but not called from this
                                    // production path). PARITY-023's broken
                                    // capture-pointer Revert design replaced by
                                    // shadow-load discipline per
                                    // DESIGN_SPECS/shadow-load-state-transition-pattern.md.
                                    //
                                    // Helper:
                                    //   1. Allocates NEW ezoo via aligned_alloc(64)
                                    //   2. Loads + PostLoadSetup into new_ezoo
                                    //   3. Atomically swaps state.cores[c].ensemble_handle
                                    //   4. Free's old ezoo
                                    // Pre-swap untouched on any failure; caller
                                    // sees nonzero rc and continues serving from
                                    // pre-swap state.
                                    int swap_backend = cfg.ml_backend
                                        ? cfg.ml_backend
                                        : MODEL_BACKEND_XGBOOST;
                                    int rc = tt::HotSwap_ShadowLoad_Ensemble<F>(
                                        state, c, cfg, new_path, swap_backend);
                                    if (rc != 0) {
                                        // Pre-swap state preserved automatically;
                                        // helper already logged the specific
                                        // failure mode. Leave model_load_failed
                                        // at its pre-call value (likely 0 if
                                        // pre-swap was healthy).
                                        fprintf(stderr,
                                            "[hot_swap] ensemble core %d shadow-load "
                                            "FAILED (rc=%d); pre-swap state preserved\n",
                                            c, rc);
                                    } else {
                                        CORE_STATE_FLAG_CLR(state.cores[c], MODEL_LOAD_FAILED);
                                        // Re-fetch ezoo after swap to run post-load
                                        // validators on the NEW ezoo. v5.14.2.E.1
                                        // closes PARITY-009.F: ValidateAgainstCfg +
                                        // FeatureOverlay_PostLoadVerify still run
                                        // on hot-swap (was bypassed pre-v5.14.2.E.1).
                                        EnsembleModelZoo<F>* swap_ezoo =
                                            (EnsembleModelZoo<F>*)state.cores[c].ensemble_handle;
                                        int validate_rc = CoreModelZoo_ValidateAgainstCfg<F>(
                                            /*zoo=*/nullptr,
                                            swap_ezoo,
                                            cfg, /*core_id=*/c,
                                            cfg.held_out_gate_strict,
                                            (int)BITMAP_IS_SET(cfg.ops_cfg_flags, MASK_OPS_CFG_ACKNOWLEDGE_INFERENCE_CFG_DRIFT),
                                            (int)BITMAP_IS_SET(cfg.ops_cfg_flags, MASK_OPS_CFG_ACKNOWLEDGE_CROSS_BINARY_DRIFT),
                                            &state.display_meta[c], &state.cores[c]);
                                        if (validate_rc < 0) {
                                            CORE_STATE_FLAG_SET(state.cores[c], MODEL_LOAD_FAILED);
                                            fprintf(stderr,
                                                "[hot_swap] ensemble core %d "
                                                "REFUSED post-load validation "
                                                "in strict mode; new model "
                                                "loaded but flagged degraded. "
                                                "Operator must reconcile cfg "
                                                "vs stamp + restart.\n", c);
                                        }
                                        int overlay_rc = FeatureOverlay_PostLoadVerify<F>(
                                            /*zoo=*/nullptr, swap_ezoo,
                                            /*core_id=*/c, cfg.held_out_gate_strict);
                                        if (overlay_rc < 0) {
                                            CORE_STATE_FLAG_SET(state.cores[c], MODEL_LOAD_FAILED);
                                        }
                                    }
                                    __atomic_store_n(
                                        &g_shared_ptr->swap_model_path_requested[c], 0,
                                        __ATOMIC_RELEASE);
                                } else {
                                    // v5.15.4 — SINGLE-ZOO SHADOW-LOAD HOT-SWAP.
                                    // Replaces in-place Free+Init+LoadFromDir
                                    // (v5.10.0c "log-and-leave" pattern; brief
                                    // empty-zoo window). Shadow-load discipline:
                                    // allocate NEW zoo + Load + PostLoadSetup,
                                    // atomically swap, Free old. Pre-swap
                                    // untouched on any failure → no empty-zoo
                                    // window; failed swaps preserve state.
                                    int swap_backend = cfg.ml_backend
                                        ? cfg.ml_backend
                                        : MODEL_BACKEND_XGBOOST;
                                    int rc = tt::HotSwap_ShadowLoad_SingleZoo<F>(
                                        state, c, cfg, new_path, swap_backend);
                                    if (rc != 0) {
                                        // Pre-swap state preserved by helper;
                                        // log + continue. Don't null the handle;
                                        // pre-swap zoo remains active.
                                        fprintf(stderr,
                                            "[hot_swap] single-zoo core %d shadow-load "
                                            "FAILED (rc=%d); pre-swap state preserved\n",
                                            c, rc);
                                    } else {
                                        CORE_STATE_FLAG_CLR(state.cores[c], MODEL_LOAD_FAILED);
                                        // Re-fetch zoo after swap to run post-load
                                        // validators on the NEW zoo (parity-check
                                        // Finding #3 closure preserved).
                                        CoreModelZoo<F>* new_swap_zoo =
                                            (CoreModelZoo<F>*)state.cores[c].model_handle;
                                        int validate_rc = CoreModelZoo_ValidateAgainstCfg<F>(
                                            new_swap_zoo,
                                            /*ezoo=*/nullptr,
                                            cfg, /*core_id=*/c,
                                            cfg.held_out_gate_strict,
                                            (int)BITMAP_IS_SET(cfg.ops_cfg_flags, MASK_OPS_CFG_ACKNOWLEDGE_INFERENCE_CFG_DRIFT),
                                            (int)BITMAP_IS_SET(cfg.ops_cfg_flags, MASK_OPS_CFG_ACKNOWLEDGE_CROSS_BINARY_DRIFT),
                                            &state.display_meta[c], &state.cores[c]);
                                        if (validate_rc < 0) {
                                            CORE_STATE_FLAG_SET(state.cores[c], MODEL_LOAD_FAILED);
                                            fprintf(stderr,
                                                "[hot_swap] core %d REFUSED post-load "
                                                "validation in strict mode; new model "
                                                "loaded but flagged degraded. Operator "
                                                "must reconcile cfg vs stamp + restart.\n", c);
                                        }
                                        int overlay_rc = FeatureOverlay_PostLoadVerify<F>(
                                            new_swap_zoo, /*ezoo=*/nullptr,
                                            /*core_id=*/c, cfg.held_out_gate_strict);
                                        if (overlay_rc < 0) {
                                            CORE_STATE_FLAG_SET(state.cores[c], MODEL_LOAD_FAILED);
                                        }
                                    }
                                    __atomic_store_n(
                                        &g_shared_ptr->swap_model_path_requested[c], 0,
                                        __ATOMIC_RELEASE);
                                }
                            }
                        }
                    }
                }
#endif
                // v5.15.5.F.4d.1.B.4 Step C.3 — extracted to
                // EngineCommon_SlowPathCycleOneCore (closes PARITY-032 by-construction
                // via D1-B BREAKEVEN_ON_PROFIT cached-gate dispatch sister to
                // TrailingSLRatchet; BACKTEST sister at Step C.4 invokes AllCores
                // wrapper). Helper body preserved verbatim from prior inline at LIVE
                // :2834-3101 → CoreFrameworks/EngineCommon.hpp:470-770. Caller resolves
                // per-cycle scalars per v1.6 O2 bytewise-identical math (price =
                // mtm_price per v1.7.3 HIGH-1) + v1.7.3 N-6 9-arg with BookSnapshot<F>
                // sister-canonical reuse. Telemetry Path A INTERNAL (helper computes
                // own rdtsc bracket + 5 CoreLatencyStats_Sample calls inside body per
                // v1.7.3 HIGH-4).

                // Per-cycle scalar inputs (mtm_price discipline preserved; helper takes
                // FPN<F> price = mtm_price, derives double internally via FPN_ToDouble
                // for guard checks).
                double price_d = last_price.load(std::memory_order_relaxed);
                double volume_d = last_volume.load(std::memory_order_relaxed);
                FPN<F> price = price_d > 0.0
                             ? FPN_FromDouble<F>(price_d) : FPN_Zero<F>();
                FPN<F> volume = volume_d > 0.0
                              ? FPN_FromDouble<F>(volume_d) : FPN_Zero<F>();
                uint64_t ts_us =
                    (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();

                // Depth snapshot — existing BookSnapshot<F> ref from g_depth_shared
                // static (LIVE; BACKTEST sister uses ShardedBacktestDriver<F> pointer
                // fields at Step C.4). Helper checks MASK_GATE_CFG_DEPTH_ENABLED
                // internally per v1.7.3 N-6 + canonical LIVE :3052-3058 pattern (no
                // `enabled` field on BookSnapshot).
                int dactive = __atomic_load_n(&g_depth_shared.active_idx, __ATOMIC_ACQUIRE);
                const BookSnapshot<F>& depth = g_depth_shared.snapshots[dactive];

                // Helper call (v1.7.3 N-6 9-arg signature)
                EngineCommon_SlowPathCycleOneCore(cfg, c, state, oms,
                                                   price, volume, ts_us,
                                                   now_tick, depth);

                // NOTE: DrainPostFill stays on the drainer thread (single writer of
                // last_*_mask is HandleFill on drainer; same thread reads + clears
                // via DrainPostFill wrapper). NOTE: KillSwitchEvaluate is GLOBAL
                // (account-level drawdown), runs on producer thread. Per-core kill
                // switch state is mutated INSIDE RebuildOneCore (now inside helper).
                // NOTE: Drag TP/SL pickup + manual close stay on drainer + producer
                // threads respectively. They submit via OMS_PushSubmit (Phase B) —
                // thread-safe.
            }
            fprintf(stderr, "[slow-path-%d] thread exiting\n", c);
        });
    }
}

} // namespace tt
