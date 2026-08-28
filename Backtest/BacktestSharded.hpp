// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[Backtest/BacktestSharded.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [BACKTEST] [DETERMINISM]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[single-threaded sharded backtest — runs the per-core architecture against historical ticks through the SAME shared OMS fill+drain pipeline as live, so backtest-vs-live accounting is parity by construction]
// [CONTAINS]
//   - [FUNCTION]_[SharedBacktest_FromHistorical]
//   - [FUNCTION]_[BacktestSharded_Run]
//======================================================================================================
// Single-threaded sharded backtest path, built on the per-core architecture
// from CoreFrameworks/ShardedBacktestDriver.hpp. Backtest_Run (BacktestEngine.hpp)
// is now a thin UNCONDITIONAL wrapper around BacktestSharded_Run — the legacy
// PortfolioController-coupled replay body was deleted and `engine_mode` no longer
// dispatches (a leftover `engine_mode=single_core` is ignored / a no-op; E.1.1).
// The file stays separate from BacktestEngine.hpp so the sharded run path is
// testable in isolation.
//
// What this populates in BacktestResults:
//   - stats.total_pnl, stats.total_trades, stats.ticks_processed
//   - stats.win_rate, stats.avg_win, stats.avg_loss, stats.profit_factor
//   - stats.max_drawdown, stats.max_drawdown_pct
//   - equity_curve (per-trade snapshots)
//   - feature_matrix (when run_cfg->collect_features=1 — the Track E.1 hook packs
//     a row per slow-path rebuild via Regime_ComputeSignals + Features_PackAll, the
//     same feature SSoT the live ML serve path uses)
//
// What this leaves regime-agnostic: sample_regimes[] is written 0 (each core may
// run a different strategy — no single central regime), so Past Runs / regime
// histograms treat sharded results as regime-agnostic.
//======================================================================================================

#pragma once

#include "../CoreFrameworks/ControllerConfig.hpp"
#include "../CoreFrameworks/ModelValidation.hpp"  // v5.14.2.E.1 — NodeModelZoo_ValidateAgainstCfg (closes PARITY-012)
#include "../ML_Headers/FeatureRegistryOverlay.hpp"  // v5.14.3.B — FeatureOverlay_PostLoadVerify
#include "../CoreFrameworks/EventLoopAggregates.hpp"
#include "../CoreFrameworks/ExecutionCore.hpp"
#include "../CoreFrameworks/ShardedBacktestDriver.hpp"
#include "../CoreFrameworks/EngineCommon.hpp"  // v5.15.5.F.4d.1.B.4 — shared train-serve helpers (ApplyBnbDiscount + BootGlobal + BootPerCore + SlowPathCycle*)
#include "../CoreFrameworks/IndexSpaces.hpp"   // E.1.3 P0/TD-299 — typed per-NODE subscripts
#include "../CoreFrameworks/ShardedSnapshot.hpp"  // Track E.7 — TUI_CopySnapshotSharded
#include "PhaseTimers.hpp"  // v5.10.0 Item A — per-phase backtest timers
#include "../CoreFrameworks/Tick.hpp"
#include "../DataStream/DepthReplayState.hpp"  // Track E.3 — depth replay
#include "../DataStream/EngineTUI.hpp"  // for TUISnapshot
#include "../GUI/CandleAccumulator.hpp"  // Track E.7 — chart panel feed
#include "../FixedPoint/FixedPointN.hpp"
#include "../ML_Headers/ConfidenceScore.hpp"  // ConfidenceScorer_Init for ML nodes (E.2)
#include "../ML_Headers/NodeModelZoo.hpp"     // E.2 — load per-node ML zoos
#include "../ML_Headers/ModelInference.hpp"   // for stamp helpers
#include "../ML_Headers/FeatureRegistry.hpp"  // v5.8.1b: Features_PackAll replaces ModelFeatures_Pack
#include "../ML_Headers/ROR_regressor.hpp"
#include "../ML_Headers/RollingStats.hpp"
#include "../Strategies/RegimeDetector.hpp"  // CumDeltaState, TickRateState, Regime_ComputeSignals
#include "../Strategies/StrategyInterface.hpp"  // STRATEGY_ML, STRATEGY_NONE constants
#include "BacktestEngine.hpp"  // for BacktestResults, BacktestRunConfig, BacktestData_Load
#include "LabelFunctions.hpp"  // for HistoricalTick

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/time.h>

namespace tt {

//======================================================================
// [FUNCTION]_[SharedBacktest_FromHistorical]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[convert a double-based HistoricalTick (Binance aggTrades) into a Tick<F> — once per tick on the single backtest thread, cost negligible vs the gate eval]
// [REFERENCE]_[PLAN]_[parity-2026-05-06-full.md]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
static inline Tick<F> SharedBacktest_FromHistorical(const HistoricalTick* h, uint64_t seq) {
    Tick<F> t;
    memset(&t, 0, sizeof(t));
    t.price     = Money{ money_from_double_payload(h->price) };  // historical-data ingress (exact recorder boundary rides the recorder rework)
    t.volume    = Money{ money_from_double_payload(h->qty) };
    t.timestamp = (uint64_t)h->timestamp_us;
    t.sequence  = seq;
    // v5.1.2 carry-forward — TODO(parity-check Finding #5):
    // h->is_buyer_maker IS available; the conversion drops it to mirror the
    // live slow-path's hardcoded-0 (parity-preserving for now — the live twin
    // is EngineCommon.hpp's `/*is_buyer_maker=*/0` at the same Finding #5). When
    // the live scalar-bus plumb-through lands, change to:
    //   t.is_buyer_maker = (uint8_t)(h->is_buyer_maker ? 1 : 0);
    // AND update both live + backtest slow-paths simultaneously.
    // See plans/_audits/parity-2026-05-06-full.md Finding #5.
    return t;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[SharedBacktest_FromHistorical]
//======================================================================

//======================================================================
// [FUNCTION]_[BacktestSharded_Run]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BACKTEST] [DETERMINISM]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[sharded backtest entry point — loads tick files, drives the per-core architecture through the shared OMS, aggregates P&L/win-loss/drawdown + equity curve; signature matches the Backtest_Run wrapper]
// [REFERENCE]_[DECISION]_[[C-1] [D-122] [D-170] [D-254] [D-255]]
// [REFERENCE]_[PARITY]_[[PARITY-26] [PARITY-27] [PARITY-28] [PARITY-29] [PARITY-30] [PARITY-31]]
// [REFERENCE]_[TECH_DEBT]_[TECH_DEBT-119]
//======================================================================
// [CODE]
//======================================================================
static inline void BacktestSharded_Run(BacktestResults *results,
                                        const BacktestRunConfig *run_cfg,
                                        volatile int *progress_pct,
                                        volatile int *cancel_flag,
                                        CandleAccumulator *candle_acc,
                                        TUISnapshot *out_snapshot = NULL) {
    // Reset results — preserve dynamic allocations like the legacy path does
    BacktestResults_Reset(results);

    // Load config (or use override)
    ControllerConfig<BACKTEST_FP> cfg;
    if (run_cfg->use_config_override) {
        cfg = run_cfg->config_override;
    } else {
        cfg = ControllerConfig_Load<BACKTEST_FP>(run_cfg->config_path);
    }
    // ③ D-255 (C-1) — gate the backtest on capital validation. The suite + CLI run through here; pre-D-255
    // this path ZEROED the fault flag and ran a malformed cfg silently (a `stop_loss_pct=banana` → SL off).
    // Fail the run (do NOT process-abort — the GUI must stay alive); results stay reset/empty from
    // BacktestResults_Reset above. This is pre-fingerprint, so the golden is not perturbed.
    if (!cfg_capital_gate_ok(cfg, "backtest sharded")) {
        results->config_used = cfg;
        return;
    }
    cfg.slow_path_max_secs = 999999;
    results->config_used = cfg;
    results->config_used.cfg_load_fault_flags = 0;  // ③ D-254 — keep the capital-fault bitmap out of the RAW fingerprint (gate above guarantees flags==0 here; this stays as defense for any future capture path).

    fprintf(stderr, "[backtest sharded] mode=sharded nodes=%u default_strategy=%d\n",
            (unsigned)cfg.num_execution_nodes, cfg.default_strategy);

    // E.1.2.D leaf 12 (2026-08-22) — the csv_load_workers stub that lived here
    // is RETIRED with its cfg row (name burned, H21). Its own deferral
    // question ("is parallel ingest worth wiring?") was answered by scan-3's
    // measurements: NO — O5's binary tick sidecar supersedes the CSV parse
    // path, and leaf 5's batched label pass killed the re-read multiplier.

    // Partial exits P.1 — validate cfg before allocating cores. When
    // partial_exit_enabled=1, refuses to run if num_execution_nodes*2
    // exceeds MAX_PORTFOLIO_POSITIONS. Mirrors the EngineSharded_Run
    // boot-time check so backtest + live agree on cfg sanity.
    if (!Sharded_ValidatePartialExitCfg(&cfg)) {
        fprintf(stderr, "[backtest sharded] FATAL: partial-exit cfg "
                        "validation failed. Skipping run.\n");
        return;
    }

    // Track E.2 — multi-strategy support. The prior SimpleDip-only gate
    // is gone; per-core strategy comes from cfg.node_strategies[i] (set
    // by ControllerConfig_Load from `core_N_strategy=...` directives,
    // defaults to STRATEGY_SIMPLE_DIP when unset). Mirrors EngineSharded_Run.

    //----------------------------------------------------------------------
    // [SECTION]_[set up the per-core engine]
    //----------------------------------------------------------------------
    // Phase 03 chunk 1B: construct OMS first, then wire EventLoopState to it.
    ExchangeAdapter<BACKTEST_FP> empty_adapter{};
    OrderManagerState<BACKTEST_FP> oms;
    // v4.7.15: train-serve parity. Live engine defaults to event_log_mode=1
    // since v4.7.1 (HandleFill + FillRecord + DrainPostFill). Backtest was
    // stuck on mode 0 (legacy OnEvent path). Under partials this hit the
    // v4.7.0 slot-collision bug + per-core accounting diverged from live.
    // Pass event_log_mode=1 here so backtest uses the same fill+drain
    // pipeline as live. ShardedBacktestDriver::on_tick adds a matching
    // EventLoop_DrainPostFill call after OrderManager_Tick to consume
    // FillRecords on the same tick they're produced (no separate drainer
    // thread in backtest; everything runs synchronously).
    // v5.9.5e — pass empty event_log_path to disable disk persistence.
    // Mode=1 still drives the in-memory event log (fill+drain pipeline
    // parity with live, per v4.7.15). Backtest is hermetic; OMS starts
    // fresh from cfg.starting_balance every run. Pre-v5.9.5e backtest
    // silently inherited live OMS state across runs (stale balance,
    // polluted trade history). Feature/label pipeline unaffected (it
    // doesn't read OMS), but Past Runs P&L now reflects only the
    // current backtest run.
    // v5.15.5.C.3 (Finding A) — partial_exit_enabled now a required param.
    // Backtest mirrors live cfg's partials geometry so per-core sharding stays
    // consistent across train-serve.
    int bt_partial_exit_enabled =
        BITMAP_IS_SET(cfg.lifecycle_cfg_flags, MASK_LIFECYCLE_CFG_PARTIAL_EXIT_ENABLED) ? 1 : 0;
    // v5.15.5.F.4c.3 WIP2d-1.B.1 — `fee_rate` arg DELETED from OrderManager_Init; OMS fee_rate
    // scalar fields deleted. Per-core fee_rate now flows via cfg.nodes[c].fee_rate_maker/_taker
    // → Order_BindPreResolved at submit → o->pre_resolved.fee_rate → HandleFill.
    OrderManager_Init(&oms, empty_adapter, 0, bt_partial_exit_enabled,
                      cfg.starting_balance,
                      /*event_log_mode=*/1,
                      /*event_log_path=*/"");
    // v5.15.5.C.3 (Finding A) — external BITMAP_SET/CLR(PARTIAL_EXIT_ENABLED)
    // block dropped at backtest side too (same fix as EngineSharded.hpp).
    // Bit is now set inside OMS_INIT_AUTOPOPULATE via the BIT-kind registry
    // row for `partial_exit_enabled` (driven by the parameter passed to
    // OrderManager_Init above). Adding a new cfg-derived boot
    // bit flag = ONE row in FOREACH_OMS_FIELD; no more external SET sites.
    // Closes /dod-audit HIGH-1 (2026-05-13 Phase 3b audit).
    // v5.15.5.F.4d.1.B.4 Step C.2 — extracted to EngineCommon_ApplyBnbDiscount
    // (NEW for BACKTEST — closes PARITY-030 by-construction; LIVE sister at Step C.1
    // calls same helper. Pre-`.B.4` BACKTEST was missing BNB discount entirely; cohorts
    // using pay_fees_in_bnb=1 had train-serve fee divergence). Body in
    // CoreFrameworks/EngineCommon.hpp (EngineCommon_ApplyBnbDiscount).
    EngineCommon_ApplyBnbDiscount(cfg);

    EventLoopState<BACKTEST_FP> state;
    // v5.15.5.F.4d.1.B.4 Step C.2 — extracted to EngineCommon_BootGlobal (closes
    // PARITY-026 sister-discipline via TECH_DEBT-119 fold; LIVE sister at Step C.1
    // calls same helper). Body preserved verbatim from the prior BACKTEST inline
    // → CoreFrameworks/EngineCommon.hpp (EngineCommon_BootGlobal): Init +
    // ConfigureKillSwitch + Regime_Init loop with cfg-driven hysteresis per
    // cfg.nodes[i].regime_hysteresis). Note: helper INTERNAL order is
    // Init → KillSwitch → Regime; prior BACKTEST inline order was Init → Regime →
    // KillSwitch. Order-of-operations bytewise-identical because each step is
    // independent — Init sets defaults; KillSwitch + Regime_Init both write to
    // distinct state fields (kill_switch_state vs regime_state) with no
    // cross-dependency. Train-serve parity preserved.
    EngineCommon_BootGlobal(cfg, state, oms);

    int num_nodes = (int)cfg.num_execution_nodes;
    if (num_nodes < 1) num_nodes = 1;
    if (num_nodes > MAX_EXECUTION_NODES) num_nodes = MAX_EXECUTION_NODES;

    // Allocate per-core resources on the stack — small fixed array, no malloc
    static tt::NodeArray<SPSCRing<Tick<BACKTEST_FP>, EXECUTION_NODE_TICK_RING_SIZE>, MAX_EXECUTION_NODES> tick_rings;   // E.1.3 P0/TD-299: typed per-NODE
    static tt::NodeArray<ExecutionCore<BACKTEST_FP>, MAX_EXECUTION_NODES> nodes;                                        // E.1.3 P0/TD-299: typed per-NODE

    // Risk slice per core: even split of (total_balance × risk_pct) across
    // cores, with cfg.node_risk_pct[i] override allowed. Mirrors
    // EngineSharded_Run.
    double total_balance = Money_ToDouble(cfg.starting_balance);
    double default_risk = Money_ToDouble(cfg.risk_pct);
    if (default_risk <= 0.0) default_risk = 0.10;
    double default_per_node = (total_balance * default_risk) / (double)num_nodes;
    if (default_per_node < 1.0) default_per_node = 1.0;

    // Track E.2 — per-core ML model zoos. Static so they persist across the
    // call (NodeModelZoo holds open file/model handles); free + re-init each
    // run to avoid stale state when the user runs multiple backtests with
    // different ML configs in one suite session.
    static tt::NodeArray<NodeModelZoo<BACKTEST_FP>, MAX_EXECUTION_NODES> ml_zoos;  // typed (Class 61 — the ezoo cohort; a slot subscript cannot compile)
    // v5.10.0a.G.5 — per-core ensemble zoo (multi-horizon). Allocated alongside
    // single-zoo; populated by EnsembleModelZoo_AutoDetectFromDir if base_dir
    // has _horizon_<H> siblings on disk. Default empty = ezoo->active=0 =
    // single-zoo path runs unchanged.
    static tt::NodeArray<EnsembleModelZoo<BACKTEST_FP>, MAX_EXECUTION_NODES> ml_ensemble_zoos;  // typed (see ml_zoos)

    for (int i = 0; i < num_nodes; ++i) {
        // v5.15.5.F.4d.1.B.4 Step C.2 — per-core boot extracted to
        // EngineCommon_BootPerCore (TECH_DEBT-119 closure + closes PARITY-027 +
        // PARITY-028 (BindCompositeCfg + RollingTurnover_Init NEW for BACKTEST) +
        // PARITY-029 (Strategy_InitPerCore NEW for BACKTEST) by-construction; LIVE
        // sister at Step C.1 invokes same helper). Helper body preserved verbatim from
        // the prior BACKTEST inline → CoreFrameworks/EngineCommon.hpp (EngineCommon_BootPerCore).
        //
        // BACKTEST caller owns: node_balance precompute (O2 bytewise-identical math) +
        // ML zoo Free+Init prior-run state (static array vs LIVE aligned_alloc heap;
        // multi-run-per-process discipline frees accumulated state from prior backtest
        // in same suite session) + post-helper bandit_state_prior_path operator override
        // (Decision B external wrapper; BACKTEST-only because LIVE has no run_cfg
        // analog at this scope).

        // Per-core risk: same as LIVE per v1.6 O2 bytewise-identical math discipline.
        // E.1.1 ③/B — reads nodes[i].risk_pct (raw-copied from node_risk_pct[i]; 0=inherit) — byte-identical.
        double node_balance = default_per_node;
        if (!Money_IsZero(cfg.nodes[tt::NodeIdx{(int16_t)i}].risk_pct)) {
            node_balance = total_balance * Money_ToDouble(cfg.nodes[tt::NodeIdx{(int16_t)i}].risk_pct);
            if (node_balance < 1.0) node_balance = 1.0;
        }

        // BACKTEST ML zoo Free+Init prior-run state. NodeModelZoo holds heap
        // allocations; double-Init without Free leaks the prior allocation.
        NodeModelZoo<BACKTEST_FP>* zoo_ptr = nullptr;
        EnsembleModelZoo<BACKTEST_FP>* ezoo_ptr = nullptr;
        if (cfg.node_strategies[i] == STRATEGY_ML) {
            const tt::NodeIdx ni{(int16_t)i};   // i is the node-loop var; the zoos are typed per-NODE
            NodeModelZoo_Free(&ml_zoos[ni]);
            EnsembleModelZoo_Free(&ml_ensemble_zoos[ni]);
            zoo_ptr  = &ml_zoos[ni];
            ezoo_ptr = &ml_ensemble_zoos[ni];
        }

        // Helper call — closes PARITY-027/028/029 by-construction. Body covers:
        // SPSCRing_Init + ExecutionCore_Init + RegisterCore + SetCoreStrategy + full
        // ML branch (load/init/post-load/validate/overlay/ConfidenceScorer + NEW
        // BindCompositeCfg + NEW RollingTurnover_Init) + NEW Strategy_InitPerCore +
        // SetPermission.
        EngineCommon_BootPerCore(cfg, i, state, tick_rings[tt::NodeIdx{(int16_t)i}], nodes[tt::NodeIdx{(int16_t)i}],
                                  zoo_ptr, ezoo_ptr,
                                  Money{ money_from_double_payload(node_balance) });

        // Post-helper BACKTEST-only operator override (Decision B external wrapper).
        // v5.10.0a.next.1 — operator-explicit prior path overrides the default
        // LoadBanditState the helper just ran. Skips bundle-id check (operator may
        // be transferring weights from a sibling bundle deliberately for
        // transfer-learning experiments).
        if (state.nodes[tt::NodeIdx{(int16_t)i}].ensemble_handle != nullptr && run_cfg && run_cfg->bandit_state_prior_path[0]) {
            EnsembleModelZoo_LoadBanditStateFromPath(
                &ml_ensemble_zoos[tt::NodeIdx{(int16_t)i}],
                run_cfg->bandit_state_prior_path,
                /*skip_bundle_check=*/1);
        }
    }

    // RollingStats lives on the stack here so the slow-path parameter rebuild
    // has fresh data. Single-threaded backtest, no concurrency concerns.
    // Both windows (W=128 short, W=512 long) are maintained because production
    // SimpleDip uses MAX(short_max, long_max) for the dip reference high.
    static RollingStats<BACKTEST_FP, 128> rolling = RollingStats_Init<BACKTEST_FP, 128>();
    static RollingStats<BACKTEST_FP, 512> rolling_long = RollingStats_Init<BACKTEST_FP, 512>();
    rolling = RollingStats_Init<BACKTEST_FP, 128>();      // explicit reset each run
    rolling_long = RollingStats_Init<BACKTEST_FP, 512>(); // explicit reset each run

    //----------------------------------------------------------------------
    // [SECTION]_[Track E.1 — train-serve parity state]
    //----------------------------------------------------------------------
    // Mirrors EngineSharded_Run's static locals so RegimeSignals fields fed to
    // ML strategies + feature collection match what the live path produces.
    // Re-init each call so backtests are deterministic from a clean state.
    //----------------------------------------------------------------------
    static RORRegressor<BACKTEST_FP> regime_ror   = RORRegressor_Init<BACKTEST_FP>();
    static FPN_Binary<BACKTEST_FP>          ema_price    = FPN_Zero<BACKTEST_FP>();
    static RollingStats<BACKTEST_FP, 256>  rolling_medium   = RollingStats_Init<BACKTEST_FP, 256>();
    static RollingStats<BACKTEST_FP, 1024> rolling_baseline = RollingStats_Init<BACKTEST_FP, 1024>();
    static CumDeltaState<BACKTEST_FP>      cumdelta_state;
    static TickRateState                   tick_rate_state;
    regime_ror       = RORRegressor_Init<BACKTEST_FP>();
    ema_price        = FPN_Zero<BACKTEST_FP>();
    rolling_medium   = RollingStats_Init<BACKTEST_FP, 256>();
    rolling_baseline = RollingStats_Init<BACKTEST_FP, 1024>();
    CumDelta_Init(&cumdelta_state);
    TickRate_Init(&tick_rate_state);
    // v4.5 Wave 1 — D.1/D.2/D.4 state. Mirrors EngineSharded_Run; reset
    // each backtest run so multiple Collect Features clicks start clean.
    static BookImbalanceHistory<BACKTEST_FP, 1024> book_imb_history;
    static FlowState                               flow_state;
    static LargeTradeState<BACKTEST_FP, 1024>      large_trade_state;
    BookImbHistory_Init(&book_imb_history);
    FlowState_Init(&flow_state);
    LargeTradeState_Init(&large_trade_state);
    // v4.6 Wave 2 — D.3 spread state + holders updated per-tick from
    // depth_replay.current.spread / mid_price.
    static SpreadState<BACKTEST_FP, 1024> spread_state;
    static FPN_Binary<BACKTEST_FP>               spread_holder    = FPN_Zero<BACKTEST_FP>();
    static FPN_Binary<BACKTEST_FP>               mid_price_holder = FPN_Zero<BACKTEST_FP>();
    SpreadState_Init(&spread_state);
    spread_holder    = FPN_Zero<BACKTEST_FP>();
    mid_price_holder = FPN_Zero<BACKTEST_FP>();
    FPN_Binary<BACKTEST_FP> ema_alpha = !FPN_IsZero(cfg.gate_ema_alpha)
                                 ? cfg.gate_ema_alpha
                                 : FPN_FromDouble<BACKTEST_FP>(0.1);
    FPN_Binary<BACKTEST_FP> one_minus_alpha =
        FPN_Sub(FPN_FromDouble<BACKTEST_FP>(1.0), ema_alpha);

    //----------------------------------------------------------------------
    // [SECTION]_[Track E.3 — depth replay]
    //----------------------------------------------------------------------
    // Mirrors how EngineSharded_Run reads
    // book_imbalance from g_depth_shared.snapshots[active] each slow path.
    // DepthReplayState loads CSVs DepthRecorder wrote and advances the
    // current snapshot in lockstep with tick timestamps.
    //
    // Heap lifecycle (four-site rule):
    //   - static + Free-then-Init dance handles multi-run-per-process
    //     (suite reuses the same DepthReplayState across Collect Features
    //     clicks). Free first to avoid leaking the prior run's row buffer.
    //   - Free at function exit only if needed; static keeps last-run
    //     state alive for the next call (re-Init fully resets).
    //----------------------------------------------------------------------
    static DepthReplayState<BACKTEST_FP> depth_replay;
    static int depth_replay_initialized = 0;
    if (depth_replay_initialized) {
        DepthReplayState_Free(&depth_replay);
    }
    DepthReplayState_Init(&depth_replay, "BTCUSDT", "data");
    depth_replay_initialized = 1;
    // depth_enabled gate: when 0, replay state is initialized but never
    // advanced — book_imbalance_holder stays at zero, gate stays inert.
    // Mirrors live BITMAP_IS_SET(cfg.gate_cfg_flags, MASK_GATE_CFG_DEPTH_ENABLED)=0 (depth thread doesn't run).
    int depth_enabled = (int)BITMAP_IS_SET(cfg.gate_cfg_flags, MASK_GATE_CFG_DEPTH_ENABLED);
    // Holder for the current book_imbalance value passed to the driver.
    // Driver reads via pointer so updates between RunTick calls land
    // automatically.
    static FPN_Binary<BACKTEST_FP> book_imbalance_holder = FPN_Zero<BACKTEST_FP>();
    book_imbalance_holder = FPN_Zero<BACKTEST_FP>();

    ShardedBacktestDriver<BACKTEST_FP, 128, 512> drv;
    ShardedBacktestDriver_Init(&drv, &state, &rolling, &cfg, (int)cfg.poll_interval, &rolling_long, &oms);
    // Track E.1 — wire parity state into the driver. Driver pushes these on
    // slow-path firings and threads them into EventLoop_RebuildAllParameters.
    drv.rolling_medium    = &rolling_medium;
    drv.rolling_baseline  = &rolling_baseline;
    drv.cumdelta_state    = &cumdelta_state;
    drv.tick_rate_state   = &tick_rate_state;
    drv.regime_ror        = &regime_ror;
    drv.ema_price         = &ema_price;
    drv.book_imbalance    = depth_enabled ? &book_imbalance_holder : nullptr;
    // v4.5 Wave 1 — driver pushes to these on slow-path firings; consumed
    // by Regime_ComputeSignals via MLBuildContext threading.
    drv.book_imb_history  = &book_imb_history;
    drv.flow_state        = &flow_state;
    drv.large_trade_state = &large_trade_state;
    drv.spread_state      = &spread_state;
    drv.current_spread    = depth_enabled ? &spread_holder    : nullptr;
    drv.current_mid_price = depth_enabled ? &mid_price_holder : nullptr;

    //----------------------------------------------------------------------
    // [SECTION]_[Track E.1 — feature collection hook]
    //----------------------------------------------------------------------
    // When collect_features=1, register
    // a callback that fires after each slow-path rebuild and packs a row of
    // the feature_matrix using Regime_ComputeSignals (the same single
    // source-of-truth the live ML serve path uses). When collect_features=0,
    // the hook stays NULL and the driver runs identically to its prior shape.
    //----------------------------------------------------------------------
    struct FeatureCollectCtx {
        BacktestResults*  results;
        const ControllerConfig<BACKTEST_FP>* cfg;
        // warmup gate — skip collection until rolling stats have meaningful
        // data. Mirrors legacy `ctrl.state != CONTROLLER_WARMUP` in
        // PortfolioController.hpp.
        uint32_t          warmup_ticks;
        uint32_t          min_warmup_samples;
    };
    FeatureCollectCtx fc_ctx{};
    fc_ctx.results            = results;
    fc_ctx.cfg                = &cfg;
    fc_ctx.warmup_ticks       = cfg.warmup_ticks;
    fc_ctx.min_warmup_samples = cfg.min_warmup_samples;
    // Train-serve regime parity via per-core read (PARITY-031 closure):
    // EngineCommon_SlowPathCycleAllCores fires Regime_Classify per-core
    // before this callback runs (ShardedBacktestDriver tick ordering);
    // collector reads state.nodes[BACKTEST_REGIME_SAMPLE_CORE].regime_state
    // at feature-pack time — bytewise-identical to live serve path.

    if (run_cfg->collect_features) {
        drv.hook_ctx = &fc_ctx;
        drv.on_slow_path = [](void* ctx_v,
                              ShardedBacktestDriver<BACKTEST_FP, 128, 512>* d,
                              const Tick<BACKTEST_FP>& tk,
                              int tick_index) {
            auto* fc = (FeatureCollectCtx*)ctx_v;
            // Warmup gate — wait for tick threshold + rolling fill.
            if ((uint32_t)tick_index < fc->warmup_ticks) return;
            if (d->rolling->count < (int)fc->min_warmup_samples) return;
            // Capacity guard — silent truncation contaminates ML training.
            if (!BacktestResults_EnsureCapacity(fc->results,
                                                 fc->results->sample_count + 1)) return;
            // v5.10.0 Item A — feature_collect phase timer (post-gates so
            // the warmup branch noise doesn't pollute the metric).
            uint64_t fc_start_ns = tt::PhaseTimer_NowNs();

            // Regime_ComputeSignals with the EXACT inputs the live ML serve
            // path uses (mirrors StrategyParameters.hpp). When the driver
            // doesn't have ROR/EMA wired (legacy callers), this branch is
            // skipped and the row gets zeroed signals — same as a non-ML run.
            RegimeSignals<BACKTEST_FP> sig;
            memset(&sig, 0, sizeof(sig));
            if (d->rolling && d->rolling_long && d->regime_ror && d->ema_price) {
                Regime_ComputeSignals(&sig, d->rolling, d->rolling_long,
                                       d->regime_ror, *d->ema_price,
                                       d->rolling_medium, d->rolling_baseline,
                                       d->cumdelta_state, d->tick_rate_state,
                                       tk.timestamp,
                                       // v4.5 Wave 1 — pass new state so the
                                       // collected feature_matrix matches what
                                       // the ML strategy sees at serve time.
                                       d->book_imb_history,
                                       d->flow_state,
                                       d->large_trade_state,
                                       // v4.6 Wave 2 — D.3 spread state + values
                                       d->spread_state,
                                       d->current_spread
                                           ? FPN_ToDouble(*d->current_spread) : 0.0,
                                       d->current_mid_price
                                           ? FPN_ToDouble(*d->current_mid_price) : 0.0);
            }
            // v5.8.1b: registry-driven feature pack. Bytewise-equivalent to
            // legacy ModelFeatures_Pack — load-bearing for train-serve parity
            // since this matrix feeds model training and the live engine
            // packs the same features at serve time.
            //
            // v5.9.0 — NaN/Inf in feature pack → SKIP this sample. Don't
            // include garbage in the training matrix; would corrupt model
            // weights. Features_PackAll returns -1 sentinel on validation
            // failure.
            {
                // PARITY-031 closure (v5.15.5.F.4d.1.B.4): regime classified
                // per-core inside EngineCommon_SlowPathCycleAllCores (fired
                // before this callback by ShardedBacktestDriver tick ordering);
                // read from canonical sample core for ctx.current_regime —
                // bytewise-identical to pre-.B.4 fc_ctx.regime_state semantic
                // (single regime per feature collector tick).
                FeatureComputeCtx<BACKTEST_FP> ctx{};
                ctx.signals       = &sig;
                ctx.short_rolling = d->rolling;
                ctx.current_regime = d->state->nodes[tt::NodeIdx{BACKTEST_REGIME_SAMPLE_CORE}].regime_state.current_regime;
                int n = Features_PackAll(&ctx,
                    &fc->results->feature_matrix[fc->results->sample_count * MODEL_NUM_FEATURES]);
                if (n < 0) {
                    // Skip this sample entirely — don't bump sample_count.
                    // Per-feature breakdown logged once (rate-limit by global
                    // counter to avoid spam during pathological data).
                    static int nan_skip_warn_emitted = 0;
                    if (!nan_skip_warn_emitted) {
                        fprintf(stderr, "[backtest] WARN: NaN/Inf in feature pack at tick %d — skipping sample (further skips silent)\n", tick_index);
                        nan_skip_warn_emitted = 1;
                    }
                    // v5.10.0 Item A — count the wasted Features_PackAll
                    // compute; otherwise feature_collect_ns under-reports
                    // when NaN-skip rate is high.
                    tt::PhaseTimer_Global().feature_collect_ns +=
                        tt::PhaseTimer_NowNs() - fc_start_ns;
                    tt::PhaseTimer_Global().populated = 1;
                    return;  // exit the lambda; this sample dropped
                }
            }
            fc->results->sample_tick_indices[fc->results->sample_count] = (uint64_t)tick_index;
            fc->results->sample_prices[fc->results->sample_count] = Money_ToDouble(tk.price);
            // Sharded has no central regime field (each core may run a
            // different strategy). Default 0 — Past Runs / regime histograms
            // that read this should treat sharded results as regime-agnostic.
            fc->results->sample_regimes[fc->results->sample_count] = 0;
            fc->results->labels[fc->results->sample_count] = 0.0f;  // post-pass
            fc->results->sample_count++;
            // v5.10.0 Item A — accumulate feature_collect time.
            tt::PhaseTimer_Global().feature_collect_ns +=
                tt::PhaseTimer_NowNs() - fc_start_ns;
            tt::PhaseTimer_Global().populated = 1;
        };
    }

    //----------------------------------------------------------------------
    // [SECTION]_[replay loop]
    //----------------------------------------------------------------------
    struct timeval t_start, t_end;
    gettimeofday(&t_start, NULL);

    // v5.10.0 Item A — phase timer reset + wall-clock anchor for total_ns.
    // Reset clears prior runs (suite reuses the singleton across Backtest_Run
    // calls). populated flag flips once any phase records nonzero time.
    tt::PhaseTimer_Reset(&tt::PhaseTimer_Global());
    uint64_t pt_run_start_ns = tt::PhaseTimer_NowNs();

    int total_processed = 0;
    int total_ticks_all_files = 0;
    // Track E.2 — warmup gate. Cores start with permission=0; once rolling
    // stats accumulate min_warmup_samples, permission flips to 1 for all
    // non-NONE cores (idempotent). Mirrors EngineSharded_Run.
    int warmup_permission_granted = 0;

    // First pass: count ticks per file for the progress bar
    int *file_tick_counts = (int *)calloc(run_cfg->num_data_files, sizeof(int));
    int max_ticks_in_file = 0;
    for (int f = 0; f < run_cfg->num_data_files; f++) {
        FILE *fp = fopen(run_cfg->data_paths[f], "r");
        if (!fp) continue;
        int lines = 0;
        char buf[512];
        while (fgets(buf, sizeof(buf), fp)) lines++;
        fclose(fp);
        file_tick_counts[f] = lines - 1;
        total_ticks_all_files += file_tick_counts[f];
        if (file_tick_counts[f] > max_ticks_in_file) max_ticks_in_file = file_tick_counts[f];
    }
    if (total_ticks_all_files <= 0) total_ticks_all_files = 1;

    int max_ticks = max_ticks_in_file + 1024;
    if (max_ticks < 1024) max_ticks = 1024;
    HistoricalTick *ticks = (HistoricalTick *)malloc(max_ticks * sizeof(HistoricalTick));
    if (!ticks) {
        fprintf(stderr, "[backtest sharded] failed to allocate tick buffer\n");
        free(file_tick_counts);
        return;
    }

    // Track per-trade outcomes for win/loss/avg stats. We sample the
    // realized_pnl after each exit to compute the per-trade delta.
    double last_realized_pnl = 0.0;
    double cumulative_wins = 0.0;
    double cumulative_losses = 0.0;
    int win_count = 0;
    int loss_count = 0;
    double peak_equity = total_balance;
    double max_drawdown = 0.0;
    double max_dd_pct   = 0.0;  // v5.8.4c: tracked alongside max_drawdown via shared helper

    // DEBUG: track price range and dump threshold once after first slow path
    double price_lo = 1e18, price_hi = 0.0;
    int debug_dumped = 0;
    // Track E.7 — last seen price + volume for the post-run TUISnapshot
    // populate. Captured per tick; survives the inner-loop scope.
    double last_price_d  = 0.0;
    double last_volume_d = 0.0;

    int64_t prev_file_last_ts = 0;  // v5.9.2c: track for inter-file ordering check
    for (int f = 0; f < run_cfg->num_data_files; f++) {
        int count = 0;
        // v5.10.0 Item A — parse phase timer.
        uint64_t parse_start_ns = tt::PhaseTimer_NowNs();
        bool parse_ok = BacktestData_Load(ticks, &count, max_ticks, run_cfg->data_paths[f]);
        tt::PhaseTimer_Global().parse_ns +=
            tt::PhaseTimer_NowNs() - parse_start_ns;
        tt::PhaseTimer_Global().populated = 1;
        if (!parse_ok)
            continue;
        // v5.9.2c — validate intra-file ordering on each file
        // (sharded loads files one at a time, can't concat-validate
        // like BacktestEngine.hpp's label_ticks does).
        char file_label[280];
        snprintf(file_label, sizeof(file_label), "data file %d (%s)",
                 f, run_cfg->data_paths[f]);
        int sort_rc = BacktestData_ValidateSort(ticks, count,
                                                 cfg.csv_sort_check_mode, file_label);
        if (sort_rc < 0) continue;  // STRICT: skip this file, keep going
        // Inter-file ordering check
        if (f > 0 && count > 0 && ticks[0].timestamp_us < prev_file_last_ts) {
            fprintf(stderr,
                "[WARN] data file %d starts before file %d ends "
                "(first ts %lld < prev last %lld). Files may be out-of-order.\n",
                f, f - 1,
                (long long)ticks[0].timestamp_us,
                (long long)prev_file_last_ts);
        }
        if (count > 0) prev_file_last_ts = ticks[count - 1].timestamp_us;

        // v5.10.0 Item A — fan_out_hot phase timer wraps the inner tick
        // loop. Includes producer fan_out + per-core hot path execution.
        // feature_collect time is accounted separately inside the
        // on_slow_path lambda; subtract at end so fan_out_hot doesn't
        // double-count it.
        uint64_t hot_loop_start_ns = tt::PhaseTimer_NowNs();
        uint64_t fc_baseline_ns = tt::PhaseTimer_Global().feature_collect_ns;
        for (int i = 0; i < count; i++) {
            if (*cancel_flag) goto done;

            Tick<BACKTEST_FP> t = SharedBacktest_FromHistorical<BACKTEST_FP>(&ticks[i], (uint64_t)total_processed);

            // DEBUG: track price range
            if (ticks[i].price < price_lo) price_lo = ticks[i].price;
            if (ticks[i].price > price_hi) price_hi = ticks[i].price;
            last_price_d  = ticks[i].price;
            last_volume_d = ticks[i].qty;

            // Track E.1 — train-serve parity. Update EMA price every tick,
            // mirroring EngineSharded_Run + legacy
            // PortfolioController_Tick. Driver reads the resulting value via
            // drv.ema_price on slow-path firings; without per-tick updates
            // sig->ema_sma_spread + sig->ema_above_sma stay stale or zero.
            // D-122/D-170 producer-ingress cast: ema_price is a BINARY feature; the money
            // tick price crosses the domain boundary EXACTLY ONCE per tick here.
            FPN_Binary<BACKTEST_FP> price_bin = Money_ToBinary(t.price);
            FPN_Binary<BACKTEST_FP> ema_new = FPN_Add(
                FPN_Mul(ema_price, ema_alpha),
                FPN_Mul(price_bin,  one_minus_alpha));
            if (FPN_IsZero(ema_price)) ema_price = price_bin;
            else                       ema_price = ema_new;

            // Track E.3 — advance depth replay in lockstep with the tick
            // stream. _Advance walks rows whose timestamp_us <= t.timestamp,
            // updating depth_replay.current to the latest matching row. The
            // first call after a day boundary auto-loads the new day's CSV
            // (degrades silently to no-op if the file is missing). Read the
            // current imbalance into the holder the driver points at —
            // RebuildAllParameters reads via the holder pointer on the next
            // slow-path firing.
            if (depth_enabled) {
                DepthReplayState_Advance(&depth_replay, t.timestamp);
                book_imbalance_holder = depth_replay.current.imbalance;
                // v4.6 Wave 2 — refresh spread + mid holders from the same
                // BookSnapshot. Driver reads via pointer on slow path.
                spread_holder    = depth_replay.current.spread;
                mid_price_holder = depth_replay.current.mid_price;
            }

            // Step the per-core engine through this tick. Internally:
            //   1. RollingStats_Push so the slow path has fresh data
            //   2. Fan out to every core (1..num_nodes)
            //   3. DrainEvents (process any entries / exits)
            //   4. On cadence: rebuild parameters, push, evaluate kill switch
            //   5. Track E.1 — fire on_slow_path hook (when registered)
            uint64_t prev_slow_runs = drv.slow_path_runs;
            ShardedBacktest_RunTick(&drv, t, total_processed);

            // Track E.7 — feed candle accumulator for the chart panel.
            // Throttled to every 100th tick (legacy BacktestEngine.hpp mirrors
            // this) — 1-min candles don't need every tick, and unthrottled
            // CandleAccumulator's mutex contention freezes the GUI thread. Same
            // pattern as EngineSharded fan_out.
            if (candle_acc && (total_processed % 100) == 0) {
                CandleAccumulator_PushWithTime(candle_acc,
                    ticks[i].price, ticks[i].qty,
                    ticks[i].is_buyer_maker,
                    (double)(ticks[i].timestamp_us / 1000000));
            }

            // Track E.2 — warmup-aware permission grant. Mirrors
            // EngineSharded_Run. Pre-E.2, BacktestSharded set
            // permission=1 at startup, which let strategies fire on garbage
            // rolling stats during the first ticks. Now we grant permission
            // only after rolling.count crosses min_warmup_samples (default
            // 64 = half of W=128). Idempotent — once granted, stays granted.
            if (!warmup_permission_granted && drv.slow_path_runs > prev_slow_runs) {
                uint32_t min_samples = cfg.min_warmup_samples > 0
                    ? cfg.min_warmup_samples : 64;
                if (rolling.count >= (int)min_samples) {
                    for (int c = 0; c < num_nodes; ++c) {
                        if (state.nodes[tt::NodeIdx{(int16_t)c}].strategy_id != STRATEGY_NONE) {
                            ExecutionCore_SetPermission(&nodes[tt::NodeIdx{(int16_t)c}], 1);
                        }
                    }
                    warmup_permission_granted = 1;
                    fprintf(stderr, "[backtest sharded] warmup complete at tick %d "
                                    "(rolling.count=%d, threshold=%u) — permission granted\n",
                            total_processed, rolling.count, min_samples);
                }
            }

            // DEBUG: dump pending params after the first slow-path rebuild
            if (!debug_dumped && drv.slow_path_runs > 0) {
                debug_dumped = 1;
                fprintf(stderr, "[backtest sharded DEBUG] after first slow path (tick %d):\n",
                        total_processed);
                fprintf(stderr, "  price range so far : %.4f .. %.4f\n", price_lo, price_hi);
                fprintf(stderr, "  rolling.price_max  : %.4f\n", FPN_ToDouble(rolling.price_max));
                fprintf(stderr, "  rolling.volume_avg : %.4f\n", FPN_ToDouble(rolling.volume_avg));
                fprintf(stderr, "  rolling_long.max   : %.4f\n", FPN_ToDouble(rolling_long.price_max));
                fprintf(stderr, "  core[0] pending bg_threshold : %.4f\n",
                        Money_ToDouble(state.nodes[tt::NodeIdx{0}].pending_params.bg_price_threshold));
                fprintf(stderr, "  core[0] pending tp_price     : %.4f\n",
                        Money_ToDouble(state.nodes[tt::NodeIdx{0}].pending_params.sg_take_profit_price));
                fprintf(stderr, "  core[0] pending sl_price     : %.4f\n",
                        Money_ToDouble(state.nodes[tt::NodeIdx{0}].pending_params.sg_stop_loss_price));
                fprintf(stderr, "  core[0] pending trade_size   : %.8f\n",
                        Money_ToDouble(state.nodes[tt::NodeIdx{0}].pending_params.trade_size));
                fprintf(stderr, "  core[0] pending strategy_id  : %u\n",
                        (unsigned)state.nodes[tt::NodeIdx{0}].pending_params.strategy_id);
                fprintf(stderr, "  core[0] pending flags        : 0x%02x\n",
                        (unsigned)state.nodes[tt::NodeIdx{0}].pending_params.flags);
                fprintf(stderr, "  core[0] permission           : %u\n",
                        (unsigned)__atomic_load_n(&nodes[tt::NodeIdx{0}].permission, __ATOMIC_ACQUIRE));
            }

            // After the drain, check if any new exits happened by comparing
            // realized_pnl. If it changed, classify as win/loss and bump the
            // equity curve.
            double current_realized = Money_ToDouble(state.oms->realized_pnl);
            if (current_realized != last_realized_pnl) {
                double trade_pnl = current_realized - last_realized_pnl;
                if (trade_pnl > 0.0) {
                    cumulative_wins += trade_pnl;
                    win_count++;
                } else if (trade_pnl < 0.0) {
                    cumulative_losses += (-trade_pnl);
                    loss_count++;
                }
                last_realized_pnl = current_realized;
                // (v5.10.0a.G.8 trade-close reward fires inside
                // EventLoop_DrainPostFillOneCore — same hook as live,
                // so backtest + live route through one path with
                // bytewise-identical bandit feeding.)

                // Equity curve sample (one per completed trade).
                // dynamic growth — capping silently contaminates stats.
                if (BacktestResults_EnsureEquityCapacity(results, results->equity_count + 1)) {
                    double bal = Money_ToDouble(state.oms->balance);
                    results->equity_curve[results->equity_count] = bal;
                    results->equity_count++;
                }
            }

            // v5.8.4c: shared inner-update helper — same code path as
            // BacktestStats_ComputeFromEquity's post-hoc walk. Bytewise
            // FP identity guaranteed by construction.
            double cur_equity = Money_ToDouble(state.oms->balance);
            MaxDrawdown_UpdateIncremental(cur_equity, &peak_equity,
                                           &max_drawdown, &max_dd_pct);

            total_processed++;
            if ((total_processed & 0x3FFF) == 0) {
                *progress_pct = (int)(100.0 * total_processed / total_ticks_all_files);
            }
        }
        // v5.10.0 Item A — accumulate fan_out_hot for this file's tick loop.
        // Subtract feature_collect delta accumulated during this file so the
        // two phases don't double-count slow-path time.
        uint64_t fc_delta_ns =
            tt::PhaseTimer_Global().feature_collect_ns - fc_baseline_ns;
        uint64_t hot_loop_total_ns =
            tt::PhaseTimer_NowNs() - hot_loop_start_ns;
        if (hot_loop_total_ns > fc_delta_ns) {
            tt::PhaseTimer_Global().fan_out_hot_ns +=
                (hot_loop_total_ns - fc_delta_ns);
        }
        tt::PhaseTimer_Global().populated = 1;
    }

done:
    *progress_pct = 100;

    // Final drain to flush anything still in flight from the last tick
    EventLoop_DrainEvents(&state);
    // D-444 / I-1 MED-4 — FINAL FillEvent apply before final_balance/final_pnl are read
    // below: tail fills must book (this flush is thinner than the driver's — it reads
    // state.oms->balance directly into BacktestStats).
    if (state.oms) {
        (void)EngineCommon_FillRingsApply(state.agg, *state.oms);
    }
    // v5.10.0 Item A — capture wall-clock total for the sharded run.
    // Caller (Backtest_Run / Backtest_RunFullValidation) may extend with
    // label_compute / wf_eval / held_out_eval / stamp_emit and bump
    // total_ns again at end-of-pipeline. Multiple writes are OK — the
    // last one wins, which is what Summary cares about.
    tt::PhaseTimer_Global().total_ns =
        tt::PhaseTimer_NowNs() - pt_run_start_ns;

    gettimeofday(&t_end, NULL);
    double elapsed = (t_end.tv_sec - t_start.tv_sec) * 1000.0
                   + (t_end.tv_usec - t_start.tv_usec) / 1000.0;

    //----------------------------------------------------------------------
    // [SECTION]_[populate BacktestResults stats]
    //----------------------------------------------------------------------
    BacktestStats *stats = &results->stats;
    memset(stats, 0, sizeof(*stats));

    double final_balance = Money_ToDouble(state.oms->balance);
    double final_pnl = Money_ToDouble(state.oms->realized_pnl);

    stats->total_pnl = final_pnl;
    stats->total_trades = (uint32_t)(state.total_entries + state.total_exits) / 2;
    if (stats->total_trades == 0 && state.total_exits > 0) {
        stats->total_trades = (uint32_t)state.total_exits;
    }
    stats->ticks_processed = (uint64_t)total_processed;
    stats->wins = (uint32_t)win_count;
    stats->losses = (uint32_t)loss_count;
    int decided = win_count + loss_count;
    if (decided > 0) {
        stats->win_rate = (double)win_count / (double)decided;
    }
    if (win_count > 0) stats->avg_win = cumulative_wins / win_count;
    if (loss_count > 0) stats->avg_loss = cumulative_losses / loss_count;
    // v5.8.4c: route metrics through Compute_* helpers — kills the prior
    // 4-site profit_factor drift (epsilon 0.0001 vs 0.001 vs none vs
    // -1.0-sentinel). all_wins_run flag separates display semantics
    // from the numeric value for OPT_METRIC_PF / display rendering.
    stats->profit_factor    = Compute_ProfitFactor(cumulative_wins, cumulative_losses);
    stats->all_wins_run     = Compute_AllWinsRun(cumulative_wins, cumulative_losses);
    stats->expectancy       = Compute_Expectancy(stats->total_trades, stats->wins,
                                                   stats->avg_win, stats->avg_loss);
    stats->max_drawdown     = max_drawdown;
    stats->max_drawdown_pct = max_dd_pct * 100.0;
    double starting_bal = Money_ToDouble(cfg.starting_balance);
    stats->return_pct = Compute_ReturnPct(final_balance - starting_bal, starting_bal);
    stats->elapsed_ms = elapsed;

    // Track E.7 — populate TUISnapshot for the dashboard panels (Market,
    // Account, Stats, Positions, Buy Gate read from this). Same shape as
    // the live engine's TUI_CopySnapshotSharded path; foxml_suite's
    // dashboard renders identically. price_d_last is captured during
    // the replay loop above.
    if (out_snapshot) {
        TUI_CopySnapshotSharded<BACKTEST_FP, 128, 512>(
            out_snapshot, &state, &rolling, &rolling_long, &cfg,
            last_price_d, last_volume_d);
        out_snapshot->live_trading = 0;  // mirror BacktestSnapshot_Copy override
    }

    free(ticks);
    free(file_tick_counts);

    // v5.10.0a.G.9 — save bandit state on backtest completion. Each
    // core writes to its own <node_model_dir>/bandit_state.json. This
    // is the "save at shutdown" trigger from the G.9 plan; periodic
    // saves (cfg.ensemble_bandit_save_interval) cover the in-flight
    // case but final flush ensures end-state is persisted even if
    // total updates < interval.
    for (int i = 0; i < num_nodes; ++i) {
        const tt::NodeIdx ni{(int16_t)i};   // node-loop var; the ensemble zoos are typed per-NODE
        if (BITMAP_IS_SET(ml_ensemble_zoos[ni].init_flags, MASK_EZOO_ACTIVE) &&
            BITMAP_IS_SET(ml_ensemble_zoos[ni].init_flags, MASK_EZOO_BANDITS_READY) &&
            cfg.node_model_dir[i][0]) {
            // s5 BT-6 — ONE call for all four families. E.1.2.C leg 0
            // (2026-08-20) had to hand-add three of them here after backtest
            // completion was found dropping them; the shared helper is what
            // stops the next site from re-learning that lesson. NOTE unchanged:
            // state files in the model dir carry across runs BY DESIGN (delete
            // them between A/B arms for a fresh arm).
            char state_dir[sizeof(ml_ensemble_zoos[ni].bandit_save_path)];
            EnsembleModelZoo_DeriveStateDir(&ml_ensemble_zoos[ni], cfg.node_model_dir[i],
                                             state_dir, sizeof(state_dir));
            EnsembleModelZoo_SaveAllBanditState(&ml_ensemble_zoos[ni], state_dir,
                                                 "backtest sharded", i);
        }
    }

    fprintf(stderr, "[backtest sharded] completed: %d ticks in %.1fms, %u trades (%u/%u W/L), P&L $%.2f\n",
            total_processed, elapsed,
            stats->total_trades, stats->wins, stats->losses,
            stats->total_pnl);
    fprintf(stderr, "[backtest sharded DEBUG] state.total_entries=%lu state.total_exits=%lu\n",
            (unsigned long)state.total_entries, (unsigned long)state.total_exits);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[BacktestSharded_Run]
//======================================================================

}  // namespace tt
