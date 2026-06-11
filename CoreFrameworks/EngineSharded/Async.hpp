// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [EngineSharded/Async.hpp — producer fan-out + drainer drain-with-submit hoisted helpers]
//======================================================================================================
// Sub-file of CoreFrameworks/EngineSharded.hpp (split per file-size-split-discipline.md
// at v5.15.5.F.4d.1.B.6; subfolder pattern first canonical).
//
// Contains:
//   - g_engine_drainer_cycle_hist — drainer-cycle bench-gate histogram (inline global;
//     moved here from EngineSharded.hpp per Decision C placement). C++17 inline-variable
//     discipline (single shared storage across TUs). Sister to Boot.hpp + .B.5 test_common.hpp.
//   - EngineSharded_Async_FanOut — hoist of producer-thread fan_out lambda. Pushes a tick
//     into every core's tick ring + replicates ema_price + runs slow-path GUI publish +
//     cfg hot-reload + paper-reset coordination at cadence.
//   - EngineSharded_Async_DrainWithSubmit — hoist of drainer-thread drain_with_submit
//     lambda. Drains each core's TradeEvent ring + builds SubmitCommand + pushes to OMS.
//
// **Decision B clarification:** Hoisted helpers use `template<unsigned F>` (NOT
// `template<F, BENCH>`). All Live/Backtest dispatch is cfg-flag-driven; BENCH gate
// touches only g_engine_drainer_cycle_hist via if-constexpr in the CALLER (drainer
// thread body in EngineSharded.hpp). Lambda bodies never referenced BENCH.
//
// **Captures-as-args translation:** fan_out captured 21 by-ref/by-value bindings + 4
// explicit params. Several were file-scope statics inside EngineSharded_Run (cores[],
// tick_rings[], g_tick_rec, g_depth_shared, g_shared, g_candle_acc) which CANNOT be
// referenced from a separate .hpp — they're function-local statics. They're passed as
// explicit args. drain_with_submit captured 4 by-ref bindings; all become explicit args.
//
// **GUI-conditional path:** g_shared / g_candle_acc references live inside #ifdef
// USE_IMGUI_GUI blocks within the body. Passed via nullable pointer args (TUISharedState*
// shared_ptr / CandleAccumulator* candle_acc_ptr); body's #ifdef gates dereference.
// Sister to SlowPath.hpp DrainManualCloses pattern at .B.6 Phase B.3 (Decision H merge).
//======================================================================================================

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>

#include "../../FixedPoint/FixedPointN.hpp"
#include "../../MemHeaders/BitmapMacros.hpp"
#include "../../MemHeaders/LatencyHistogram.hpp"
#include "../../MemHeaders/HealthLog.hpp"
#include "../../MemHeaders/OmsFieldRegistry.hpp"          // OMS_RESET_AUTOPOPULATE
#include "../../MemHeaders/CoreCtxInitRegistry.hpp"        // CORE_CTX_RESET_AUTOPOPULATE
#include "../../MemHeaders/CoreStateFlagRegistry.hpp"      // CORE_STATE_FLAG_{SET,CLR}

#include "../ControllerConfig.hpp"          // ControllerConfig + ControllerConfig_Load
#include "../ControllerEventLoop.hpp"       // EventLoop_OnEvent / _KillSwitchEvaluate / _UpdateEmaPriceAllCores / Sharded_LegSlot
#include "../ExecutionCore.hpp"             // ExecutionCore + EventLoopState
#include "../OrderManager.hpp"              // OrderManagerState + SubmitCommand + OMS_PushSubmit
#include "../OrderEventLog.hpp"             // OrderEventLog_Reset
#include "../ShardedSnapshot.hpp"           // TUI_CopySnapshotSharded
#include "../ShardedSnapshotPersist.hpp"    // ShardedSnapshot_Save (declared as template; needed BEFORE call at line 428 to satisfy C++17 two-phase lookup; avoids -Wc++20-extensions warning)
#include "../ShardedTradeLog.hpp"           // ShardedTradeLog_Flush / _Rotate / _FormatPerCoreFilename
#include "../PaperResetArchive.hpp"         // PaperResetArchive_* + Summary_WriteJson
#include "../SPSCRing.hpp"                  // SPSCRing + SPSCRing_TryPush / _TryPop
#include "../Tick.hpp"                      // Tick<F>

#include "../../DataStream/TickRecorder.hpp"        // TickRecorder + TickRecorder_Push
#include "../../DataStream/BinanceDepth.hpp"        // DepthSharedState<F>
#include "../../DataStream/EngineTUI.hpp"           // TUISharedState + TUISnapshot + TUI_Populate*

#ifdef USE_IMGUI_GUI
#include "../../GUI/CandleAccumulator.hpp"          // CandleAccumulator + CandleAccumulator_Push
#else
// Forward-decl at global scope (NOT inside namespace tt) so the function-signature
// pointer-type `CandleAccumulator*` resolves to a complete-enough opaque type for
// nullable parameter passing under non-GUI build. Body's #ifdef USE_IMGUI_GUI gate
// elides any actual dereference. Matches SlowPath.hpp TUISharedState forward-decl
// discipline (.B.6 Phase B.3 lesson: global scope, not namespace scope, to avoid
// the namespace-shadowing pitfall).
struct CandleAccumulator;
#endif

#include "Boot.hpp"   // g_engine_sharded_shutdown — polled inside fan_out body

// parent_index: CoreFrameworks/EngineSharded.hpp

namespace tt {

//======================================================================================================
// [DRAINER-CYCLE BENCH HISTOGRAM — file-shared inline global]
//======================================================================================================
// Bench-gate histogram for per-drainer-cycle latency. Inline (C++17) so all TUs that
// include this header see the same instance — the drainer thread body in
// EngineSharded.hpp accumulates into it under `if constexpr (BENCH)`, and the engine
// shutdown summary line reads from it.
//
// `inline` (C++17) ensures one definition across all translation units. `alignas(64)`
// is on the type itself (LatencyHistogram); the variable declaration inherits it.
//
// Reset at engine boot via LatencyHistogram_Reset before any drainer thread spawns.
// When BENCH=false (default; production), the if-constexpr blocks that touch this
// global are compile-time elided — the variable is allocated but never written/read
// (linker may DCE it if all translation units instantiate BENCH=false, but the inline
// keyword keeps the symbol valid even if untouched).
//======================================================================================================
inline LatencyHistogram g_engine_drainer_cycle_hist{};

//======================================================================================================
// EngineSharded_Async_FanOut — hoist of producer-thread fan_out lambda
//======================================================================================================
// Pushes a single tick out to every core's tick ring, updates global state (ticks_produced
// counter, last_price/_volume atomics, ema_price replication), records the tick to CSV
// when enabled, feeds the GUI candle accumulator, and at slow-path cadence (every
// poll_interval ticks) runs the cfg hot-reload + per-core kill-switch reset + book
// depth read + paper-reset coordination + GUI TUISnapshot publish.
//
// Originally a lambda inside the producer thread of EngineSharded_Run; hoisted to a
// named function at v5.15.5.F.4d.1.B.6 per Decision B (Option a; captures → explicit
// args). Body unchanged from lambda body modulo capture → arg translation.
//
// **Args translated from lambda captures (24 captures + 4 explicit params):**
//   - by-ref captures (13): seq, ticks_produced, last_price, last_volume, cfg, state,
//     oms, slow_path_counter, ema_price, paper_reset_in_progress, topo_hot_cpu,
//     topo_slow_cpu, topo_poll_interval
//   - by-value captures (8): num_cores, slow_path_interval, tsc_ghz, ema_alpha,
//     live_trading, topo_producer_cpu, topo_drainer_cpu, topo_nproc
//   - file-local-static args (6 — NOT captures; passed because they're function-local
//     statics in EngineSharded_Run and CANNOT be referenced from header scope):
//     tick_rings, cores, tick_rec, depth_shared, shared_ptr (GUI), candle_acc_ptr (GUI)
//   - explicit params (4): price_d, volume_d, ts_us, is_buyer_maker
//
// Returns false on shutdown observation (caller breaks out of producer loop).
//======================================================================================================
template<unsigned F>
inline bool EngineSharded_Async_FanOut(
    // Explicit lambda params
    double price_d,
    double volume_d,
    uint64_t ts_us,
    int is_buyer_maker,
    // By-value captures
    int num_cores,
    int slow_path_interval,
    double tsc_ghz,
    FPN_Binary<F> ema_alpha,
    bool live_trading,
    int topo_producer_cpu,
    int topo_drainer_cpu,
    long topo_nproc,
    // By-ref captures
    uint64_t& seq,
    std::atomic<uint64_t>& ticks_produced,
    std::atomic<double>& last_price,
    std::atomic<double>& last_volume,
    ControllerConfig<F>& cfg,
    EventLoopState<F>& state,
    OrderManagerState<F>& oms,
    int& slow_path_counter,
    FPN_Binary<F>& ema_price,
    std::atomic<bool>& paper_reset_in_progress,
    int* topo_hot_cpu,        // array decays to pointer; 16-element
    int* topo_slow_cpu,       // 16-element
    uint32_t* topo_poll_interval,  // 16-element
    // File-local-static args (passed because not accessible from header scope)
    SPSCRing<Tick<F>, EXECUTION_CORE_TICK_RING_SIZE>* tick_rings,
    ExecutionCore<F>* cores,
    TickRecorder& tick_rec,
    DepthSharedState<F>& depth_shared,
    TUISharedState* shared_ptr,     // nullable; #ifdef USE_IMGUI_GUI gates dereference
    // candle_acc_ptr declared unconditionally (passed nullptr under non-GUI build)
    // so the signature is build-flag-invariant — mirrors shared_ptr discipline +
    // SlowPath.hpp DrainManualCloses pattern (.B.6 Phase B.3). Under non-GUI build
    // CandleAccumulator is forward-declared at global scope above; pointer-to-
    // incomplete-type is legal in a parameter-list because no dereference happens.
    CandleAccumulator* candle_acc_ptr
) {
    Tick<F> t;
    memset(&t, 0, sizeof(t));
    t.price = Money{ money_from_double_payload(price_d) };   // replay/TUI-side ingress bridge
    t.volume = Money{ money_from_double_payload(volume_d) };
    t.timestamp = ts_us;
    t.sequence = seq++;
    t.is_buyer_maker = (uint8_t)(is_buyer_maker ? 1 : 0);
    for (int c = 0; c < num_cores; ++c) {
        while (!SPSCRing_TryPush(&tick_rings[c], t)) {
            if (g_engine_sharded_shutdown) return false;
        }
        if (g_engine_sharded_shutdown) return false;
    }
    ticks_produced.fetch_add(1, std::memory_order_relaxed);
    last_price.store(price_d, std::memory_order_relaxed);
    last_volume.store(volume_d, std::memory_order_relaxed);
    // v5.12.1.A.1+.2 — publish LOCAL wall-clock us of this tick to
    // EventLoopState::last_ws_tick_us. Producer is the SOLE writer;
    // slow-path threads + GUI read with acquire ordering.
    //
    // SEMANTICS REFINED in .A.2: was ts_us (Binance exchange time)
    // in the .A.1 commit; switched to local system_clock here so
    // EventLoop_CheckWsStaleness can compare against another local
    // clock read self-consistently (no NTP/skew dependency).
    // Backtest still uses tick.timestamp (synthetic, deterministic);
    // operator MUST keep cfg.ws_dead_time_flatten_enabled=0 in
    // backtest to avoid phantom-flatten under that mismatch.
    uint64_t local_now_us = (uint64_t)
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    // v5.15.5.B.2 — wrapped in WsHeartbeatTelemetry alignas(64) cluster.
    state.ws_telemetry.last_tick_us.store(local_now_us, std::memory_order_release);
    (void)ts_us;  // ts_us still used by other fan_out consumers
    // v5.12.1.C — heartbeat throughput. 5 × 1-second-bucket ring;
    // current bucket = (now_sec % 5). Reset on second-rollover.
    // Sum of fresh buckets = ticks/5s shown in header bar. Single-
    // writer (producer); reader sees eventual consistency.
    // Reuses local_now_us from above — no extra clock read.
    {
        uint64_t now_sec = local_now_us / 1000000ULL;
        int b = (int)(now_sec % 5);
        if (state.ws_telemetry.bucket_last_sec[b] != now_sec) {
            state.ws_telemetry.bucket_last_sec[b] = now_sec;
            state.ws_telemetry.bucket_count[b] = 0;
        }
        state.ws_telemetry.bucket_count[b]++;
        uint64_t total = 0;
        for (int i = 0; i < 5; ++i) {
            if (state.ws_telemetry.bucket_last_sec[i] >= now_sec - 4) {
                total += state.ws_telemetry.bucket_count[i];
            }
        }
        state.ws_telemetry.ticks_per_5s.store(total, std::memory_order_relaxed);
    }

    // v5.1.4: GUI drag-TP/SL pickup runs per-tick, NOT at slow-path
    // cadence. Pre-v5.1.4 this lived in the cadence block and gave
    // 5-33s of perceived latency. Cost: one atomic_load + branch on
    // -1 per tick (~5ns on a modern x86). Branch is taken only on
    // the rare tick where a drag has just happened.
#ifdef USE_IMGUI_GUI
    if (shared_ptr) {
        int slot = __atomic_load_n(&shared_ptr->drag_slot, __ATOMIC_ACQUIRE);
        if (slot >= 0 && slot < 16) {
            int is_tp = shared_ptr->drag_is_tp;
            double dprice = shared_ptr->drag_price;
            __atomic_store_n(&shared_ptr->drag_slot, -1, __ATOMIC_RELEASE);
            auto *pos = &state.oms->portfolio.positions[slot];
            if (state.oms->portfolio.active_bitmap & (uint16_t)(1u << slot)) {
                if (is_tp) pos->take_profit_price = Money{ money_from_double_payload(dprice) };
                else       pos->stop_loss_price   = Money{ money_from_double_payload(dprice) };
                fprintf(stderr, "[sharded] GUI drag: slot %d %s -> $%.2f\n",
                        slot, is_tp ? "TP" : "SL", dprice);
            }
        }
    }
#endif

    // v4.0 train-serve parity: update EMA price on every tick (matches
    // legacy PortfolioController_Tick behavior). Used by ML feature
    // pack — without this, sig->ema_sma_spread + sig->ema_above_sma
    // stay zero in sharded while backtest produces them non-zero.
    // First-tick branchless: if ema is zero, take current price as-is;
    // otherwise standard exponential smoothing.
    FPN_Binary<F> one_minus_alpha = FPN_Sub(FPN_FromDouble<F>(1.0), ema_alpha);
    // D-122 producer feature ingress: ONE money->binary cast per tick for the ema chain.
    FPN_Binary<F> price_bin = Money_ToBinary(t.price);
    FPN_Binary<F> ema_new = FPN_Add(
        FPN_Mul(ema_price, ema_alpha),
        FPN_Mul(price_bin, one_minus_alpha));
    if (FPN_IsZero(ema_price)) ema_price = price_bin;
    else                       ema_price = ema_new;
    // v5.1.2 (full symmetric decoupling): replicate ema_price to
    // ALL engines' slow_state in BOTH arches. Single-writer is
    // the producer thread. Cost: N FPN_Binary copies per tick — trivial.
    EventLoop_UpdateEmaPriceAllCores(&state, ema_price);

    // Phase 8a (post-coding c7) — record raw tick to CSV when enabled.
    // No-op when record_ticks=0 (the gate is inside TickRecorder_Push).
    // v5.1.2 architectural carry-forward — is_buyer_maker isn't available
    // on the sharded fan_out's scalar bus; producer + recorder + slow-path
    // all pass 0. (Legacy path passes the real value from BinanceStream
    // tick read.) Train-serve parity preserved (both broken the same way)
    // but FEAT_VOLUME_DELTA in slow-path RollingStats is locked at +1.0
    // → effectively zero-information. Full closure (~4h) plumbs through
    // a g_last_buyer_maker scalar; deferred to v5.10.X or v5.11+.
    // See plans/plan_checks/parity-2026-05-06-full.md Finding #5.
    TickRecorder_Push(&tick_rec, price_d, volume_d, (int64_t)ts_us, 0);

#ifdef USE_IMGUI_GUI
    // Feed candles for the chart panel (same pattern as main.cpp:396)
    if (candle_acc_ptr) {
        CandleAccumulator_Push(candle_acc_ptr, price_d, volume_d, 0);
    }
#endif

    // Slow path: feed rolling stats and rebuild gate parameters
    // every poll_interval ticks. Matches the legacy controller's
    // sampling cadence so the per-core threshold computation gets
    // the same input distribution.
    slow_path_counter++;
    if (slow_path_counter >= slow_path_interval) {
        slow_path_counter = 0;
#ifdef USE_IMGUI_GUI
        // v5.1.4: drag-TP/SL pickup MOVED out of the slow-path cadence
        // block to fire every tick — see fan_out's tick-rate handler
        // at line ~870. Pre-v5.1.4 this lived here and added 5-33s of
        // latency on every TP/SL drag, plus dropped back-to-back drags
        // when the second one overwrote the slot before the engine
        // saw the first. Tick-rate pickup gives ~tens of ms latency
        // and reduces the back-to-back drop window proportionally.

        // v4.0 hot-reload: GUI Settings panel writes engine.cfg and
        // sets reload_requested. Sharded mode previously ignored this
        // (only legacy main.cpp consumed it), so per-core overrides
        // edited via the GUI never took effect until restart. Re-read
        // engine.cfg, copy reloadable fields into the in-memory cfg.
        //
        // Boot-only fields are preserved (engine_mode, num_execution_cores,
        // model paths, starting_balance, fee_rate_maker/taker, exchange
        // routing, recording flags). Tunables are bulk-copied including
        // the per-core overrides array.
        if (shared_ptr && __atomic_exchange_n(&shared_ptr->reload_requested, 0,
                                              __ATOMIC_ACQ_REL)) {
            ControllerConfig<F> new_cfg = ControllerConfig_Load<F>("engine.cfg");
            // boot-only: preserve fields that the running engine cannot
            // change live (would require thread restart, file I/O off
            // the controller thread, or different per-core wiring).
            new_cfg.engine_mode         = cfg.engine_mode;
            new_cfg.num_execution_cores = cfg.num_execution_cores;
            new_cfg.starting_balance    = cfg.starting_balance;
            for (int c = 0; c < 16; ++c) {
                memcpy(new_cfg.core_model_path[c],
                       cfg.core_model_path[c],
                       sizeof(cfg.core_model_path[c]));
                memcpy(new_cfg.core_model_dir[c],
                       cfg.core_model_dir[c],
                       sizeof(cfg.core_model_dir[c]));
            }
            cfg = new_cfg;

            // Phase 2.1: recompute allocated_balance after reload.
            // Pre-2.1 the allocated_balance field was set ONCE in
            // EngineSharded_Run init and never refreshed — changing
            // risk_pct or core_N_risk_pct in cfg was silently
            // stale. Mirror the startup formula here.
            // NOTE: don't touch core_open_notional during reload —
            // open positions still exist; the new allocation either
            // expands or shrinks the budget but the deployed
            // notional is unchanged.
            {
                double total_balance = Money_ToDouble(cfg.starting_balance);
                double default_risk  = Money_ToDouble(cfg.risk_pct);
                if (default_risk <= 0.0) default_risk = 0.10;
                double default_per_core = (total_balance * default_risk) / (double)num_cores;
                if (default_per_core < 1.0) default_per_core = 1.0;
                for (int c = 0; c < num_cores; ++c) {
                    double core_balance = default_per_core;
                    if (!Money_IsZero(cfg.core_risk_pct[c])) {
                        core_balance = total_balance * Money_ToDouble(cfg.core_risk_pct[c]);
                        if (core_balance < 1.0) core_balance = 1.0;
                    }
                    state.cores[c].allocated_balance = Money{ money_from_double_payload(core_balance) };
                }
            }
            fprintf(stderr, "[sharded] cfg hot-reloaded "
                    "(per-core overrides + tunables refreshed, allocations recomputed)\n");
        }
        // Phase 3: process per-core kill resets BEFORE the rebuild
        // so a freshly-reset core can be re-evaluated this cycle.
        // Each reset clears the trip flag and refreshes peak to the
        // current value. GUI-only — shared_ptr lives inside the
        // USE_IMGUI_GUI ifdef.
        if (shared_ptr) {
            for (int c = 0; c < num_cores; ++c) {
                if (shared_ptr->kill_reset_per_core[c]) {
                    shared_ptr->kill_reset_per_core[c] = 0;
                    CORE_STATE_FLAG_CLR(state.cores[c], KILL_TRIPPED);
                    state.cores[c].core_peak_balance = Money_Zero();
                    state.cores[c].core_dd_pct = Money_Zero();
                    fprintf(stderr, "[sharded] core %d kill switch RESET\n", c);
                }
            }
        }
#endif
        // Phase 3: pass current_price for MTM kill switch evaluation.
        // Read once from the producer atomic; tracker is realized-only
        // on the first slow path before any tick has been seen.
        Money mtm_price = Money{ money_from_double_payload(
            last_price.load(std::memory_order_relaxed)) };  // producer atomic carries double (S-8 vehicle rework rides P3)
        // v4.3 — pass expanded feature-pack state for the model's
        // medium-horizon features. Same pointers go to both the AUTO
        // regime resolution branch (above the dispatch) and the ML
        // strategy branch (via MLBuildContext) — single source of
        // truth, train-serve parity preserved.
        uint64_t rebuild_ts_us = (uint64_t)
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        // Track E.3 (post-coding c14, finally landed 2026-04-26)
        // — feed book_imbalance from the depth thread into the
        // per-core slow-path rebuild. The depth thread runs only
        // when BITMAP_IS_SET(cfg.gate_cfg_flags, MASK_GATE_CFG_DEPTH_ENABLED)=1 and writes to active_idx with
        // RELEASE; we read with ACQUIRE for matching ordering.
        // When depth_enabled=0 OR the depth thread hasn't received
        // a snapshot yet, the value stays at FPN_Zero (init) —
        // RebuildAllParameters' gate check fails closed if cfg.
        // min_book_imbalance>0, which is the desired semantics
        // (no data → no buys, since we can't evaluate the gate).
        FPN_Binary<F> book_imb;
        FPN_Binary<F> book_spread   = FPN_Zero<F>();
        FPN_Binary<F> book_mid      = FPN_Zero<F>();
        if (BITMAP_IS_SET(cfg.gate_cfg_flags, MASK_GATE_CFG_DEPTH_ENABLED)) {
            int dactive = __atomic_load_n(&depth_shared.active_idx,
                                           __ATOMIC_ACQUIRE);
            book_imb    = depth_shared.snapshots[dactive].imbalance;
            book_spread = depth_shared.snapshots[dactive].spread;
            book_mid    = depth_shared.snapshots[dactive].mid_price;
        } else {
            book_imb = FPN_Zero<F>();
        }
        // v4.5 Wave 1 — push book_imbalance into history at slow-path
        // cadence. Mirror in BacktestSharded driver. Push happens
        // BEFORE RebuildAllParameters so Regime_ComputeSignals reads
        // the freshly-updated history.
        // Phase 4 — periodic snapshot save. Once every ~1024 slow-path
        // cycles, paper mode only. With slow_path_interval=8 ticks and
        // ~10 ticks/sec that's roughly every 13 minutes — frequent
        // enough to bound state-loss-on-crash, infrequent enough to
        // not spam the disk. Atomic rename means a crash mid-save
        // leaves the previous good file intact.
        static int save_counter = 0;
        if (!live_trading && (++save_counter >= 1024)) {
            save_counter = 0;
            // v5.15.5.C.2 (S3a + S4): canonical mirror via bit-packed oms_state_flags.
            ShardedSnapshot_Save<F>(&state, "data/sharded_snapshot.dat",
                                      BITMAP_IS_SET(state.oms->oms_state_flags, tt::MASK_OMS_STATE_PARTIAL_EXIT_ENABLED));
        }
        // KNOWN RACE (audit 2026-04-09): KillSwitchEvaluate reads
        // oms->balance from this (producer) thread while the drainer
        // thread writes it via OnEvent / OMS_Tick fill handler.
        // FPN_Binary<64> is 64 words — torn reads are possible under
        // concurrent writes. Probability is low at current event
        // rates (~1 exit/sec vs 5 Hz slow path). Consequence:
        // false-positive or missed kill switch trip from a garbage
        // FPN_Binary comparison. Pre-existing race (sharded engine always
        // had producer + drainer on separate threads).
        // TODO: move kill switch eval to drainer thread, or use an
        // atomic balance snapshot for the comparison.
        EventLoop_KillSwitchEvaluate(&state);
        // Warmup gating: don't grant permission until rolling stats
        // have meaningful data. Pre-v4.0.1 this was `count >= 1`,
        // which let MR (buy-below-avg) AND MOM (buy-above-avg) BOTH
        // fire on the second tick — every restart instantly opened
        // 2 positions with garbage TP/SL because rolling.price_avg
        // and rolling.price_stddev hadn't stabilized yet.
        //
        // Use cfg.min_warmup_samples if set (>0), else half the
        // short window (= 64). 64 samples gives a stable enough
        // rolling stddev for momentum sizing to compute non-trivial
        // TP/SL distances and prevents the both-strategies-fire
        // race during the first few ticks.
        uint32_t min_samples = cfg.min_warmup_samples > 0
            ? cfg.min_warmup_samples : 64;
        // Permission grant: per-core slow-path threads grant their
        // own permission once their per-core rolling_short.count
        // crosses min_samples threshold (see SlowPathCycleOneCore).

#ifdef USE_IMGUI_GUI
        // Populate TUISnapshot for the GUI — same double-buffered
        // pattern as legacy engine in main.cpp:845-912.
        if (shared_ptr) {
            // v5.11.3.B — seqlock publish: parity bit flips odd → fill
            // back → flip even (idx toggled). Reader retries if mid-write.
            auto pub = TUISnapshot_Publish_Begin(shared_ptr);
            TUISnapshot *bs = pub.back;
            const TUISnapshot *fs = pub.front;
            // carry graph history ring buffers from front buffer
            memcpy(bs->price_history, fs->price_history, sizeof(bs->price_history));
            memcpy(bs->volume_history, fs->volume_history, sizeof(bs->volume_history));
            memcpy(bs->pnl_history, fs->pnl_history, sizeof(bs->pnl_history));
            bs->graph_head = fs->graph_head;
            bs->graph_count = fs->graph_count;
            // populate from sharded state
            // v5.1.2: TUI snapshot reads engine 0's slow_state since
            // all engines have identical pushes (same input/cadence).
            auto* sst0 = state.cores[0].slow_state;
            if (sst0) {
                TUI_CopySnapshotSharded(bs, &state, &sst0->rolling_short,
                                         &sst0->rolling_long, &cfg, price_d, volume_d);
            }
            TUI_PopulatePerCoreLatency(bs, cores, num_cores, tsc_ghz);
            // v5.0.1 (Phase H): slow-path latency from CoreContext.
            TUI_PopulatePerCoreSlowPathLatency(bs, &state, tsc_ghz);
            // v5.0.2 (Phase H): topology — system + thread layout.
            TUI_PopulateTopology(bs,
                                  topo_producer_cpu, topo_drainer_cpu,
                                  (int)topo_nproc,
                                  cfg.slow_path_pin_offset,
                                  topo_hot_cpu, topo_slow_cpu,
                                  topo_poll_interval);
            // v5.0.3 (Engine Topology advanced): live thread state.
            TUI_PopulateAdvancedTopology(bs, &state, &oms);
            // v4.7.18: paper-reset seq for history-clearing panels
            bs->paper_reset_seq = (uint32_t)shared_ptr->paper_reset_seq;
            // v5.11.4.B — health log WARN on first non-zero observation
            // of async log writer trouble (parity-check Section J).
            // One-shot per process: the moment ring_full_spins or
            // writer_realloc_failed_count cross zero, emit a single
            // WARN line. Operator can grep the health log for the
            // string to detect silent writer-thread distress.
            {
                static uint64_t prev_ring_full = 0;
                static uint64_t prev_realloc_failed = 0;
                if (bs->oms_log_ring_full_spins != prev_ring_full) {
                    if (prev_ring_full == 0) {
                        tt::Health_Log(tt::HEALTH_WARN, "oms_log", -1,
                            "async writer ring saturation: ring_full_spins=%llu "
                            "(drainer is spin-waiting for writer to drain; "
                            "investigate disk health or bump ring size)",
                            (unsigned long long)bs->oms_log_ring_full_spins);
                    }
                    prev_ring_full = bs->oms_log_ring_full_spins;
                }
                if (bs->oms_log_writer_realloc_failed != prev_realloc_failed) {
                    if (prev_realloc_failed == 0) {
                        tt::Health_Log(tt::HEALTH_CRITICAL, "oms_log", -1,
                            "async writer realloc OOM: writer_realloc_failed=%llu "
                            "(events may be dropped; entries[] cannot grow)",
                            (unsigned long long)bs->oms_log_writer_realloc_failed);
                    }
                    prev_realloc_failed = bs->oms_log_writer_realloc_failed;
                }
                // v5.11.5.D — log_full_drops first-non-zero WARN
                // (parity-check J.1). Distinct from ring_full_spins:
                // log_full_drops fires when the mmap'd entries[]
                // buffer is saturated; events have been DROPPED.
                // Operator may need to bump
                // ORDER_EVENT_LOG_MAX_CAPACITY or run an offline
                // log-rotation step.
                static uint64_t prev_log_drops = 0;
                if (bs->oms_log_full_drops != prev_log_drops) {
                    if (prev_log_drops == 0) {
                        tt::Health_Log(tt::HEALTH_CRITICAL, "oms_log", -1,
                            "event log capacity exhausted: log_full_drops=%llu "
                            "(events DROPPED; bump ORDER_EVENT_LOG_MAX_CAPACITY "
                            "or rotate the log)",
                            (unsigned long long)bs->oms_log_full_drops);
                    }
                    prev_log_drops = bs->oms_log_full_drops;
                }
            }
            // append current data point to graph ring buffers
            bs->price_history[bs->graph_head] = bs->price;
            bs->volume_history[bs->graph_head] = bs->volume;
            bs->pnl_history[bs->graph_head] = bs->total_pnl;
            bs->graph_head = (bs->graph_head + 1) % TUISnapshot::GRAPH_LEN;
            if (bs->graph_count < TUISnapshot::GRAPH_LEN) bs->graph_count++;
            TUISnapshot_Publish_End(shared_ptr);  // v5.11.3.B — flips parity even, idx toggled
        }
        // check GUI quit request
        if (shared_ptr && shared_ptr->quit_requested) {
            g_engine_sharded_shutdown = 1;
        }
        // paper reset: zero balance, clear positions, reset counters
        if (shared_ptr && shared_ptr->paper_reset_requested && !cfg.use_real_money) {
            shared_ptr->paper_reset_requested = 0;
            // Coordinate Reset Paper with per-core slow-path threads.
            // Set the in-progress flag → slow-paths park (yield) at
            // top of their loop. After reset completes, clear the
            // flag → slow-paths resume with fresh state.
            paper_reset_in_progress.store(true, std::memory_order_release);
            // Brief yield to let slow-paths observe the flag and park.
            // Worst case they don't yet — shared state writes below
            // proceed concurrently, slow-path's next read sees fresh
            // values (eventually consistent acceptable for slow-path).
            std::this_thread::yield();

            // v5.15.5.C.3 Phase 6 — paper-reset archive flow. Captures the
            // prior session's state into a timestamped directory BEFORE the
            // OMS reset wipes paper_session_start_us. Operator can review
            // archived sessions in data/paper_resets/{start_iso}_to_{end_iso}.paper/
            //   - snapshot.dat: full OMS + per-core state (ShardedSnapshot_Save)
            //   - trades.csv:    copy of logging/SYMBOL_order_history.csv
            //   - summary.json:  session + global + per_core + per_strategy +
            //                     per_regime (per_regime is empty placeholder;
            //                     Phase 5.B follow-up or focused aggregator
            //                     populates from trades.csv post-rotation)
            // Failures are NON-FATAL (log to stderr; continue with reset).
            // MUST RUN BEFORE OMS_RESET_AUTOPOPULATE: the registry's
            // paper_session_start_us RESET expression evaluates tt::_oms_now_us()
            // and overwrites the prior session's anchor. We need the prior
            // start_us for the archive dirname.
            {
                uint64_t prior_start_us = state.oms->paper_session_start_us;
                uint64_t end_us = (uint64_t)
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                char dirname[256];
                tt::PaperResetArchive_FormatDirname(prior_start_us, end_us,
                                                     dirname, sizeof(dirname));
                if (tt::PaperResetArchive_CreateDirectories(dirname)) {
                    // 1) snapshot.dat — full OMS + per-core via existing ShardedSnapshot_Save
                    char snapshot_path[512];
                    std::snprintf(snapshot_path, sizeof(snapshot_path),
                                  "%s/snapshot.dat", dirname);
                    int partial_on =
                        BITMAP_IS_SET(state.oms->oms_state_flags,
                                      tt::MASK_OMS_STATE_PARTIAL_EXIT_ENABLED) ? 1 : 0;
                    ShardedSnapshot_Save(&state, snapshot_path, partial_on);

                    // 2) trades.csv — copy of logging/<SYMBOL>_order_history.csv (aggregate)
                    //    Flush + copy via fread/fwrite loop (no live file pointer disturbance;
                    //    ShardedTradeLog_Rotate below handles the rotation of the live file).
                    //
                    //    v5.15.5.C.3 Phase 5.B — ALSO copies per-core mirror files to
                    //    `<dirname>/trades/core_<N>.csv` (1 + N files per archive). The
                    //    aggregate file preserves GUI/TradeReader backward compat; the
                    //    per-core files enable per-core archive analysis (regime
                    //    aggregation, per-core PnL post-mortem, etc.) without parsing
                    //    the aggregate. mkdir of `<dirname>/trades/` is best-effort.
                    if (state.oms->trade_log) {
                        if (state.oms->trade_log->file) {
                            std::fflush(state.oms->trade_log->file);
                        }
                        // Flush per-core files before copy (mirror writes are line-buffered
                        // but explicit flush ensures the on-disk snapshot is consistent).
                        tt::ShardedTradeLog_Flush(state.oms->trade_log);

                        // v5.15.5.C.3 Phase 5.B — local file-copy helper. Used by
                        // aggregate + per-core archive copies below; deduplicates the
                        // fread/fwrite loop + close-on-all-paths handling. Failure is
                        // best-effort silent (matches pre-helper behavior).
                        auto copy_file = [](const char* src, const char* dst) {
                            FILE* sf = std::fopen(src, "r");
                            FILE* df = std::fopen(dst, "w");
                            if (sf && df) {
                                char buf[4096];
                                size_t n;
                                while ((n = std::fread(buf, 1, sizeof(buf), sf)) > 0) {
                                    std::fwrite(buf, 1, n, df);
                                }
                            }
                            if (sf) std::fclose(sf);
                            if (df) std::fclose(df);
                        };

                        // Aggregate copy: <dirname>/trades.csv
                        char trade_src[256], trade_dst[512];
                        std::snprintf(trade_src, sizeof(trade_src),
                                      "logging/%s_order_history.csv",
                                      state.oms->trade_log->symbol);
                        std::snprintf(trade_dst, sizeof(trade_dst),
                                      "%s/trades.csv", dirname);
                        copy_file(trade_src, trade_dst);

                        // Per-core copies: <dirname>/trades/core_<N>.csv (N = 0..MAX_EXECUTION_CORES-1).
                        // Per-core source filename built via the single source of truth
                        // (ShardedTradeLog_FormatPerCoreFilename — also used by _Init + _Rotate).
                        char trades_subdir[384];
                        std::snprintf(trades_subdir, sizeof(trades_subdir),
                                      "%s/trades", dirname);
                        if (tt::PaperResetArchive_CreateDirectories(trades_subdir)) {
                            for (int c = 0; c < MAX_EXECUTION_CORES; ++c) {
                                if (!state.oms->trade_log->per_core_files[c]) continue;
                                char per_src[256], per_dst[512];
                                if (!tt::ShardedTradeLog_FormatPerCoreFilename(
                                        per_src, sizeof(per_src),
                                        state.oms->trade_log->symbol, c)) continue;
                                std::snprintf(per_dst, sizeof(per_dst),
                                              "%s/core_%d.csv", trades_subdir, c);
                                copy_file(per_src, per_dst);
                            }
                        }
                    }

                    // 3) summary.json — session + global + per_core + per_strategy + per_regime
                    char summary_path[512];
                    std::snprintf(summary_path, sizeof(summary_path),
                                  "%s/summary.json", dirname);
                    tt::Summary_WriteJson(summary_path, state, cfg,
                                           num_cores, end_us);

                    std::fprintf(stderr,
                        "[archive] paper-reset session archived: %s "
                        "(snapshot + trades + summary)\n", dirname);
                } else {
                    std::fprintf(stderr,
                        "[archive] WARNING — failed to create archive directory %s; "
                        "proceeding with reset without archive\n", dirname);
                }
            }

            // v5.15.5.C.3 Phase 3b — full OMS reset via canonical registry.
            // Replaces 10 explicit field assignments (balance, realized_pnl,
            // ks_peak_balance, kill_switch_tripped bit clear, total_fees,
            // total_maker_fees, total_taker_fees, maker_fills_count,
            // taker_fills_count, Portfolio_Init) with single AUTOPOPULATE call.
            // Adds DO_RESET coverage for atomics (total_submitted/filled/
            // rejected — v5.5.6 Class-5 recurring-bug close completion) +
            // paper_session_start_us refresh (Phase 2 archive anchor). See
            // FOREACH_OMS_FIELD in MemHeaders/OmsFieldRegistry.hpp; RESET_KIND
            // column selects which fields participate.
            OMS_RESET_AUTOPOPULATE(state.oms, cfg.starting_balance);
            state.total_entries = 0;
            state.total_exits   = 0;
            state.total_events_processed = 0;
            // v5.15.5.B.7 — Per-slot paper-reset via CORE_CTX_RESET_AUTOPOPULATE
            // companion macro. Closes the per-session-counter mirror class
            // structurally: ~16 explicit field resets pre-.B.7 (anchored by
            // Phase 2.1 P&L leak, Phase 3 kill switch, v4.7.26 partner pairing
            // + gross accumulator leak, v5.4.3 SL-cooldown / idle-cycle leak)
            // collapse to ONE registry-driven walk. Adding a new "per-session
            // counter" in the future = ONE row in FOREACH_CORE_CTX_RESET_FIELD;
            // every paper-reset site auto-flows. See MemHeaders/CoreCtxInitRegistry.hpp.
            for (int c = 0; c < num_cores; ++c) {
                CORE_CTX_RESET_AUTOPOPULATE(state, c);
                // persist-8 (.E.0.10): CORE_CTX_RESET resets ctx VALUE fields, but the ExecutionCore is a
                // pointer-target (CoreCtxInitRegistry.hpp:94 "pointer; registration persists") — its hot
                // mirror is outside the registry's reach. Clear the active flag so the (un-parked) hot path
                // doesn't evaluate TP/SL on the now-wiped portfolio until the next cadence-gated slow-path
                // re-arm. Single-byte flag (hardware-atomic on x86); once active=0 the hot path skips the
                // exit eval, so the stale live_tp/live_sl are never read. Paper-mode only + the phantom sell
                // is guarded downstream (OrderManager.hpp:1185 active_bitmap check) → hygiene, LOW severity.
                // The robust version (full hot-path quiesce during reset, like the slow path at Run.hpp:1670)
                // pairs with conc-5's concurrency pass.
                cores[c].active = 0;
            }
            // v4.7.18: rotate the trade history CSV to a timestamped
            // backup so the GUI's Trade History panel goes blank
            // instead of mixing pre-reset rows with new ones.
            if (state.oms->trade_log) {
                ShardedTradeLog_Rotate(state.oms->trade_log);
            }
            // v4.7.18: also truncate the OMS event log on disk so
            // the next engine restart doesn't replay 40+ zombie
            // events from the prior session into the fresh state.
            OrderEventLog_Reset(&state.oms->event_log);
            // v4.7.18: bump the reset sequence counter so retained-
            // history GUI panels (Per-Core P&L ring buffer, equity
            // curve, etc.) can clear themselves.
            shared_ptr->paper_reset_seq++;
            fprintf(stderr, "[sharded] paper reset: balance=$%.2f "
                            "(seq=%u, trade log + event log rotated)\n",
                    Money_ToDouble(cfg.starting_balance),
                    (unsigned)shared_ptr->paper_reset_seq);
            // v4.7.39 (Phase C.2): reset complete; release slow-path
            // threads. They were parked on this flag — next loop
            // iteration sees fresh state.
            paper_reset_in_progress.store(false, std::memory_order_release);
        }
#endif
    }
    return true;
}

//======================================================================================================
// EngineSharded_Async_DrainWithSubmit — hoist of drainer-thread drain_with_submit lambda
//======================================================================================================
// Drains each core's TradeEvent ring (up to MAX_EVENTS_PER_DRAIN_PER_CORE per ring per
// cycle), calls EventLoop_OnEvent for bookkeeping, computes order qty (with partial-
// exit-leg split), builds SubmitCommand, and pushes to OMS via OMS_PushSubmit (so the
// drainer's later OMS_DrainSubmit pass serializes Submit calls). Returns the total
// number of events drained this cycle.
//
// Originally a lambda inside EngineSharded_Run drainer thread; hoisted to a named
// function at v5.15.5.F.4d.1.B.6 per Decision B (Option a). Body unchanged from lambda
// body modulo capture → arg translation (4 by-ref captures → explicit args).
//======================================================================================================
template<unsigned F>
inline int EngineSharded_Async_DrainWithSubmit(
    EventLoopState<F>& state,
    OrderManagerState<F>& oms,
    std::atomic<uint64_t>& ticks_produced,
    const ControllerConfig<F>& cfg
) {
    int total_drained = 0;
    // v5.15.5.C.4 Phase T1 — hoist partial_on out of inner event loop.
    // Drainer-thread-stable predicate; one read per drain_with_submit call
    // vs N events × 1 read. Saves ~16-32 cycles/cycle at typical burst.
    const int partial_on = BITMAP_IS_SET(state.oms->oms_state_flags, tt::MASK_OMS_STATE_PARTIAL_EXIT_ENABLED);
    for (int slot = 0; slot < state.registered_count; ++slot) {
        ExecutionCore<F>* core = state.cores[slot].core;
        if (core == nullptr) continue;
        for (int i = 0; i < MAX_EVENTS_PER_DRAIN_PER_CORE; ++i) {
            TradeEvent<F> event;
            if (!SPSCRing_TryPop(&core->event_ring, &event)) break;

            bool is_entry = (event.type & TRADE_EVENT_ENTRY) != 0;
            bool is_exit  = (event.type & TRADE_EVENT_EXIT)  != 0;

            // P.3: map (core_id, leg) → portfolio slot. When
            // partial_exit_enabled=0, slot == core_id (1:1 mapping
            // preserves pre-P.3 behavior). When enabled, slot = 2*c+leg.
            // v5.15.5.C.2 (S3a + S4): canonical mirror via bit-packed oms_state_flags.
            // v5.15.5.C.4 Phase T1: partial_on hoisted to lambda-scope above.
            int portfolio_slot = Sharded_LegSlot(slot, (int)event.leg, partial_on);
            if (portfolio_slot < 0) {
                // Defensive: malformed event (e.g. leg=1 without partials
                // enabled). Drop + log; don't crash the drainer.
                fprintf(stderr,
                    "[sharded] drainer: invalid (core=%d, leg=%u) for "
                    "partial_enabled=%d → dropping event\n",
                    slot, (unsigned)event.leg, partial_on);
                continue;
            }

            // snapshot exit qty BEFORE OnEvent because CloseSlot clears it
            double order_qty_d = 0.0;
            if (is_exit) {
                // Exit qty: read from the LEG's portfolio slot (leg A's
                // qty for leg A exit, leg B's for leg B exit). With
                // partials disabled, portfolio_slot == slot == core_id
                // and behavior is identical to pre-P.3.
                order_qty_d = Money_ToDouble(
                    state.oms->portfolio.positions[portfolio_slot].quantity);
            } else if (is_entry) {
                // Entry qty: split intended_qty between legs by
                // partial_exit_pct. Leg A gets partial_pct, leg B gets
                // (1 - partial_pct). When partials disabled, leg is
                // always 0 and we use the full intended_qty (no split).
                // v4.7.32: read partial_exit_pct from per-core override
                // when set (0 = inherit). Pre-fix it always read global,
                // making the per-core override a silent no-op.
                // v5.15.5.C.4 Phase T1: hoisted core_overrides[slot] ref —
                // single deref shared with the tp2_mult read at line ~2386
                // below (was two separate `const auto&` declarations).
                double full_qty = Money_ToDouble(state.cores[slot].intended_qty);
                const auto& ov_slot = cfg.core_overrides[slot];
                Money partial_pct = !Money_IsZero(ov_slot.partial_exit_pct)
                    ? ov_slot.partial_exit_pct : cfg.cores[slot].partial_exit_pct;
                if (partial_on && event.leg == PARTIAL_LEG_A) {
                    order_qty_d = full_qty * Money_ToDouble(partial_pct);
                } else if (partial_on && event.leg == PARTIAL_LEG_B) {
                    order_qty_d = full_qty * (1.0 - Money_ToDouble(partial_pct));
                } else {
                    order_qty_d = full_qty;
                }
            }

            EventLoop_OnEvent(&state, event);
            ++total_drained;

            if ((is_entry || is_exit) && order_qty_d > 0.0) {
                // v4.7.2: leg B's intended_tp must be TP2, not TP1.
                // intended_tp on the core is leg A's absolute TP. For
                // leg B, scale the TP-distance by cfg.tp2_mult — same
                // computation the hot path does for live_tp_b, just
                // expressed in absolute price form so Position.tp +
                // snapshot persistence reflect the actual TP2. Keeps
                // the panel display honest AND prevents
                // snapshot-restore-while-paired from reviving leg B
                // with TP1 instead of TP2.
                Money leg_tp = state.cores[slot].intended_tp;
                if (is_entry && partial_on && event.leg == PARTIAL_LEG_B) {
                    // v5.15.5.C.4 Phase T1: use leg_tp local (already
                    // captured at line above) instead of re-reading
                    // state.cores[slot].intended_tp; saves 1 indexed read.
                    Money tp_dist_a = Money_Sub(leg_tp, event.price);
                    // v4.7.32: per-core tp2_mult override (0 = inherit).
                    // v5.15.5.C.4 Phase T1: NOTE — `ov_slot` from earlier
                    // entry branch is NOT in scope here; the entry-qty
                    // branch + this entry-tp branch are sibling blocks.
                    // Re-declare `ov_tp2` local for tp2_mult access.
                    // Future structural fix: hoist `ov_slot` to top of
                    // iteration (above is_exit/is_entry split) if more
                    // sites need it.
                    const auto& ov_tp2 = cfg.core_overrides[slot];
                    Money tp2_mult_eff = !Money_IsZero(ov_tp2.tp2_mult)
                        ? ov_tp2.tp2_mult : cfg.cores[slot].tp2_mult;
                    Money tp_dist_b = Money_Mul(tp_dist_a, tp2_mult_eff);
                    leg_tp = Money_Add(event.price, tp_dist_b);
                }
                // v4.7.37 (Phase B reordered): push through OMS_PushSubmit
                // instead of calling Submit directly. Drainer drains the
                // queue + calls Submit serially — preserves OMS single-
                // caller contract for when Phase C spawns N producers.
                // v5.15.5.F.4c.3 WIP2d-1.B.1 option (A refined) — required-field ctor + optional .field = X.
                // ctor: (core_id, type, qty, leg, core_cfg); optional intended_tp/intended_sl/strategy_id/event_price.
                SubmitCommand<F> cmd((int16_t)portfolio_slot,                                       // P.3: actual slot, not core_id
                                      is_entry ? ORDER_MARKET_BUY : ORDER_MARKET_SELL,
                                      Money{ money_from_double_payload(order_qty_d) },  // partial-qty exact split rides P3
                                      event.leg,                                                    // P.3: leg propagated to Order
                                      &cfg.cores[slot]);                                            // per-core cfg (sharded: core_id == slot)
                cmd.intended_tp = leg_tp;
                cmd.intended_sl = state.cores[slot].intended_sl;
                cmd.strategy_id = state.cores[slot].strategy_id;
                cmd.event_price = event.price;
                OMS_PushSubmit(&oms, cmd);
                // Phase 6prep sharded c13: snapshot the staged prediction
                // into active_prediction at entry submit. Persists across
                // the entry→exit window so the IC update at exit pairs the
                // realized return with the prediction that actually
                // triggered the trade — not the latest rebuild's value.
                // Only on leg A entry — leg B is part of the same trade,
                // shouldn't double-stamp the prediction.
                if (is_entry && event.leg == PARTIAL_LEG_A &&
                    state.cores[slot].strategy_id == STRATEGY_ML) {
                    state.cores[slot].active_prediction =
                        state.cores[slot].staged_prediction;
                }
                // v4.0.3 spacing + time-based exit: stamp this entry
                // for cross-cutting checks on the next rebuild. Only
                // on leg A entry (one trade = one entry stamp).
                if (is_entry && event.leg == PARTIAL_LEG_A) {
                    state.cores[slot].last_entry_price = event.price;
                    state.cores[slot].last_entry_tick  = ticks_produced.load(std::memory_order_relaxed);
                    // v4.7.6: wall-clock entry time so GUI's "Hold"
                    // column can show real elapsed minutes for open
                    // positions. Microseconds since epoch — divide
                    // by 60_000_000 in the snapshot copy for minutes.
                    state.cores[slot].last_entry_wall_us = (uint64_t)
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
                }
            }
        }
    }
    return total_drained;
}

} // namespace tt
