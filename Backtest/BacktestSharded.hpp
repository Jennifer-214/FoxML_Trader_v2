// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [SHARDED BACKTEST]
//======================================================================================================
// Phase 13 of the per-core sharding migration. Single-threaded sharded
// backtest path. Mirrors Backtest_Run's interface but uses the per-core
// architecture from CoreFrameworks/ShardedBacktestDriver.hpp internally.
//
// Why a separate file: the legacy Backtest_Run is ~350 LOC of replay logic
// tightly coupled to PortfolioController. Adding a second 200 LOC path inline
// would balloon the file. Keeping them separate makes it easy to:
//   1. Compare implementations side by side during the migration
//   2. Eventually delete the legacy path when sharded is the default
//   3. Test each path in isolation
//
// The dispatcher in BacktestEngine.hpp peeks at config.engine_mode and routes
// to either Backtest_Run (legacy) or BacktestSharded_Run (this file).
//
// What this DOES populate in BacktestResults:
//   - stats.total_pnl, stats.total_trades, stats.ticks_processed
//   - stats.win_rate, stats.avg_win, stats.avg_loss, stats.profit_factor
//   - stats.max_drawdown, stats.max_drawdown_pct
//   - equity_curve (per-trade snapshots)
//
// What this DOES NOT populate (out of scope for the first migration cut):
//   - feature_matrix (ML feature collection — sharded path doesn't have
//     RollingStats wired into per-core context yet)
//   - regime tracking, gate reason diagnostics
//   - per-strategy stats breakdown
//
// These can be added once the sharded path is the default and the legacy
// PortfolioController is wired into the controller core for the slow-path
// stuff that doesn't change with sharding.
//======================================================================================================

#pragma once

#include "../CoreFrameworks/ControllerConfig.hpp"
#include "../CoreFrameworks/EventLoopAggregates.hpp"
#include "../CoreFrameworks/ExecutionCore.hpp"
#include "../CoreFrameworks/ShardedBacktestDriver.hpp"
#include "../CoreFrameworks/ShardedSnapshot.hpp"  // Track E.7 — TUI_CopySnapshotSharded
#include "../CoreFrameworks/Tick.hpp"
#include "../DataStream/DepthReplayState.hpp"  // Track E.3 — depth replay
#include "../DataStream/EngineTUI.hpp"  // for TUISnapshot
#include "../GUI/CandleAccumulator.hpp"  // Track E.7 — chart panel feed
#include "../FixedPoint/FixedPointN.hpp"
#include "../ML_Headers/ConfidenceScore.hpp"  // ConfidenceScorer_Init for ML cores (E.2)
#include "../ML_Headers/CoreModelZoo.hpp"     // E.2 — load per-core ML zoos
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

//======================================================================================================
// [HELPERS]
//======================================================================================================
// Convert a HistoricalTick (double-based, from Binance aggTrades) into a
// Tick<F> (the per-core architecture's tick struct). Done once per tick on
// the backtest's single thread; cost is negligible compared to the gate eval.
//======================================================================================================
template <unsigned F>
static inline Tick<F> SharedBacktest_FromHistorical(const HistoricalTick* h, uint64_t seq) {
    Tick<F> t;
    memset(&t, 0, sizeof(t));
    t.price     = FPN_FromDouble<F>(h->price);
    t.volume    = FPN_FromDouble<F>(h->qty);
    t.timestamp = (uint64_t)h->timestamp_us;
    t.sequence  = seq;
    return t;
}

//======================================================================================================
// [RUN]
//======================================================================================================
// Sharded backtest entry point. Same signature as Backtest_Run for the
// dispatcher. Loads tick files, runs the per-core architecture against them,
// aggregates results.
//======================================================================================================
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
    cfg.slow_path_max_secs = 999999;
    results->config_used = cfg;

    fprintf(stderr, "[backtest sharded] mode=sharded cores=%u default_strategy=%d\n",
            (unsigned)cfg.num_execution_cores, cfg.default_strategy);

    // Partial exits P.1 — validate cfg before allocating cores. When
    // partial_exit_enabled=1, refuses to run if num_execution_cores*2
    // exceeds MAX_PORTFOLIO_POSITIONS. Mirrors the EngineSharded_Run
    // boot-time check so backtest + live agree on cfg sanity.
    if (!Sharded_ValidatePartialExitCfg(&cfg)) {
        fprintf(stderr, "[backtest sharded] FATAL: partial-exit cfg "
                        "validation failed. Skipping run.\n");
        return;
    }

    // Track E.2 — multi-strategy support. The prior SimpleDip-only gate
    // is gone; per-core strategy comes from cfg.core_strategies[i] (set
    // by ControllerConfig_Load from `core_N_strategy=...` directives,
    // defaults to STRATEGY_SIMPLE_DIP when unset). Mirrors EngineSharded_Run
    // lines 559-648.

    //----------------------------------------------------------------------
    // Set up the per-core engine
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
    OrderManager_Init(&oms, empty_adapter, 0, cfg.starting_balance, cfg.fee_rate,
                      /*event_log_mode=*/1,
                      /*event_log_path=*/"");
    // Phase 8: backtest is all-taker. Set OMS rates explicitly so HandleFill's
    // is_maker branch picks the right rate. Both = fee_rate_taker → backtest
    // numerics unchanged from pre-Phase-8 (cfg legacy mirroring already set
    // fee_rate_taker = fee_rate). Documented divergence from live (which has
    // real per-fill maker/taker tagging from Binance executionReport).
    oms.fee_rate_maker = cfg.fee_rate_taker; // backtest = all-taker semantics
    oms.fee_rate_taker = cfg.fee_rate_taker;
    // v4.7.15: mirror partials geometry to OMS for the post-fill drainer's
    // slot→core_id mapping. Same as EngineSharded_Run sets it from cfg.
    oms.partial_exit_enabled = cfg.partial_exit_enabled ? 1 : 0;
    EventLoopState<BACKTEST_FP> state;
    EventLoopState_Init(&state, &oms);

    // Configure kill switch from the existing config fields. The drawdown
    // field in cfg is already a fraction (parsed via CFG_PARSE_PCT) so it
    // matches what _ConfigureKillSwitch expects.
    if (cfg.kill_switch_enabled) {
        EventLoopState_ConfigureKillSwitch(&state,
            FPN_Zero<BACKTEST_FP>(),  // no hard balance floor in legacy mode either
            cfg.kill_switch_drawdown_pct);
    }

    int num_cores = (int)cfg.num_execution_cores;
    if (num_cores < 1) num_cores = 1;
    if (num_cores > MAX_EXECUTION_CORES) num_cores = MAX_EXECUTION_CORES;

    // Allocate per-core resources on the stack — small fixed array, no malloc
    static SPSCRing<Tick<BACKTEST_FP>, EXECUTION_CORE_TICK_RING_SIZE> tick_rings[MAX_EXECUTION_CORES];
    static ExecutionCore<BACKTEST_FP> cores[MAX_EXECUTION_CORES];

    // Risk slice per core: even split of (total_balance × risk_pct) across
    // cores, with cfg.core_risk_pct[i] override allowed. Mirrors
    // EngineSharded_Run lines 549-557.
    double total_balance = FPN_ToDouble(cfg.starting_balance);
    double default_risk = FPN_ToDouble(cfg.risk_pct);
    if (default_risk <= 0.0) default_risk = 0.10;
    double default_per_core = (total_balance * default_risk) / (double)num_cores;
    if (default_per_core < 1.0) default_per_core = 1.0;

    // Track E.2 — per-core ML model zoos. Static so they persist across the
    // call (CoreModelZoo holds open file/model handles); free + re-init each
    // run to avoid stale state when the user runs multiple backtests with
    // different ML configs in one suite session.
    static CoreModelZoo<BACKTEST_FP> ml_zoos[MAX_EXECUTION_CORES];

    for (int i = 0; i < num_cores; ++i) {
        SPSCRing_Init(&tick_rings[i]);
        ExecutionCore_Init(&cores[i], (uint16_t)i, &tick_rings[i]);
        // intended TP/SL/qty get filled in by the slow-path strategy rebuild
        EventLoopState_RegisterCore(&state, &cores[i],
            FPN_Zero<BACKTEST_FP>(), FPN_Zero<BACKTEST_FP>(), FPN_Zero<BACKTEST_FP>());

        // per-core risk: cfg.core_risk_pct[i] override or even-split default
        double core_balance = default_per_core;
        if (!FPN_IsZero(cfg.core_risk_pct[i])) {
            core_balance = total_balance * FPN_ToDouble(cfg.core_risk_pct[i]);
            if (core_balance < 1.0) core_balance = 1.0;
        }
        EventLoopState_SetCoreStrategy(&state, i,
            cfg.core_strategies[i],
            FPN_FromDouble<BACKTEST_FP>(core_balance));

        // Track E.2 — ML model load for STRATEGY_ML cores. Three resolution
        // paths, mirrors EngineSharded_Run lines 576-631:
        //   1. core_N_model_dir set → CoreModelZoo from directory
        //   2. core_N_model_path set → legacy single buy_signal model
        //   3. ml_model_path globally → legacy fallback
        if (cfg.core_strategies[i] == STRATEGY_ML) {
            // Free any prior backtest run's zoo state on this slot before
            // reinit. CoreModelZoo holds heap allocations; double-Init
            // without Free leaks the prior allocation.
            CoreModelZoo_Free(&ml_zoos[i]);
            CoreModelZoo_Init(&ml_zoos[i]);
            int backend = cfg.ml_backend ? cfg.ml_backend : MODEL_BACKEND_XGBOOST;
            int loaded = 0;
            if (cfg.core_model_dir[i][0]) {
                loaded = CoreModelZoo_LoadFromDir(&ml_zoos[i],
                                                   cfg.core_model_dir[i], backend);
                fprintf(stderr, "[backtest sharded] core %d: zoo from %s, %d role(s) loaded\n",
                        i, cfg.core_model_dir[i], loaded);
            } else {
                const char* model_path = cfg.core_model_path[i][0]
                    ? cfg.core_model_path[i] : cfg.ml_model_path;
                if (model_path[0]) {
                    loaded = CoreModelZoo_LoadLegacy(&ml_zoos[i], model_path, backend);
                    if (loaded) {
                        fprintf(stderr, "[backtest sharded] core %d: legacy buy_signal model loaded from %s\n",
                                i, model_path);
                    } else {
                        fprintf(stderr, "[backtest sharded] core %d: ML model load FAILED (%s), "
                                         "falling back to SimpleDip\n", i, model_path);
                    }
                }
            }
            if (loaded) {
                state.cores[i].model_handle = &ml_zoos[i];
                if (cfg.core_model_dir[i][0]) {
                    int verify_ok = CoreModelZoo_VerifyExpected(&ml_zoos[i],
                        cfg.core_model_dir[i],
                        cfg.barrier_gate_enabled,
                        FPN_ToDouble(cfg.ml_buy_threshold),
                        cfg.model_verify_strict, i,
                        cfg.poll_interval,
                        (unsigned)MODEL_FORMAT_VERSION);
                    if (!verify_ok && cfg.model_verify_strict > 0) {
                        fprintf(stderr, "[backtest sharded] core %d: ML model UNLOADED due to "
                                         "strict verify failure\n", i);
                        CoreModelZoo_Free(&ml_zoos[i]);
                        state.cores[i].model_handle = NULL;
                    }
                }
            }
            // Phase 6prep — ConfidenceScorer with cfg tunables.
            // v4.7.32: per-core resolved tau (mirrors EngineSharded_Run
            // for train-serve parity). confidence_window stays global.
            const auto& ov_conf = cfg.core_overrides[i];
            FPN<BACKTEST_FP> tau_eff = !FPN_IsZero(ov_conf.confidence_freshness_tau)
                ? ov_conf.confidence_freshness_tau
                : cfg.confidence_freshness_tau;
            ConfidenceScorer_Init(&state.cores[i].confidence,
                                   (int)cfg.confidence_window,
                                   FPN_ToDouble(tau_eff));
        }

        // Track E.2 — permission starts 0; granted after warmup samples
        // accumulate. Mirrors EngineSharded_Run lines 999-1019. Pre-E.2,
        // BacktestSharded set permission=1 immediately, which let strategies
        // fire on garbage rolling stats during the first ticks.
        ExecutionCore_SetPermission(&cores[i], 0);
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
    // Track E.1 — train-serve parity state. Mirrors EngineSharded_Run's
    // static locals (lines 522-547) so RegimeSignals fields fed to ML
    // strategies + feature collection match what the live path produces.
    // Re-init each call so backtests are deterministic from a clean state.
    //----------------------------------------------------------------------
    static RORRegressor<BACKTEST_FP> regime_ror   = RORRegressor_Init<BACKTEST_FP>();
    static FPN<BACKTEST_FP>          ema_price    = FPN_Zero<BACKTEST_FP>();
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
    static FPN<BACKTEST_FP>               spread_holder    = FPN_Zero<BACKTEST_FP>();
    static FPN<BACKTEST_FP>               mid_price_holder = FPN_Zero<BACKTEST_FP>();
    SpreadState_Init(&spread_state);
    spread_holder    = FPN_Zero<BACKTEST_FP>();
    mid_price_holder = FPN_Zero<BACKTEST_FP>();
    FPN<BACKTEST_FP> ema_alpha = !FPN_IsZero(cfg.gate_ema_alpha)
                                 ? cfg.gate_ema_alpha
                                 : FPN_FromDouble<BACKTEST_FP>(0.1);
    FPN<BACKTEST_FP> one_minus_alpha =
        FPN_Sub(FPN_FromDouble<BACKTEST_FP>(1.0), ema_alpha);

    //----------------------------------------------------------------------
    // Track E.3 — depth replay. Mirrors how EngineSharded_Run reads
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
    // Mirrors live cfg.depth_enabled=0 (depth thread doesn't run).
    int depth_enabled = (int)cfg.depth_enabled;
    // Holder for the current book_imbalance value passed to the driver.
    // Driver reads via pointer so updates between RunTick calls land
    // automatically.
    static FPN<BACKTEST_FP> book_imbalance_holder = FPN_Zero<BACKTEST_FP>();
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
    // Track E.1 — feature collection hook. When collect_features=1, register
    // a callback that fires after each slow-path rebuild and packs a row of
    // the feature_matrix using Regime_ComputeSignals (the same single
    // source-of-truth the live ML serve path uses). When collect_features=0,
    // the hook stays NULL and the driver runs identically to its prior shape.
    //----------------------------------------------------------------------
    struct FeatureCollectCtx {
        BacktestResults*  results;
        const ControllerConfig<BACKTEST_FP>* cfg;
        // warmup gate — skip collection until rolling stats have meaningful
        // data. Mirrors legacy `ctrl.state != CONTROLLER_WARMUP` (lines
        // 1034-1035 of PortfolioController.hpp).
        uint32_t          warmup_ticks;
        uint32_t          min_warmup_samples;
    };
    FeatureCollectCtx fc_ctx{};
    fc_ctx.results            = results;
    fc_ctx.cfg                = &cfg;
    fc_ctx.warmup_ticks       = cfg.warmup_ticks;
    fc_ctx.min_warmup_samples = cfg.min_warmup_samples;

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

            // Regime_ComputeSignals with the EXACT inputs the live ML serve
            // path uses (mirrors StrategyParameters.hpp:469). When the driver
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
                FeatureComputeCtx<BACKTEST_FP> ctx{};
                ctx.signals       = &sig;
                ctx.short_rolling = d->rolling;
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
                    return;  // exit the lambda; this sample dropped
                }
            }
            fc->results->sample_tick_indices[fc->results->sample_count] = (uint64_t)tick_index;
            fc->results->sample_prices[fc->results->sample_count] = FPN_ToDouble(tk.price);
            // Sharded has no central regime field (each core may run a
            // different strategy). Default 0 — Past Runs / regime histograms
            // that read this should treat sharded results as regime-agnostic.
            fc->results->sample_regimes[fc->results->sample_count] = 0;
            fc->results->labels[fc->results->sample_count] = 0.0f;  // post-pass
            fc->results->sample_count++;
        };
    }

    //----------------------------------------------------------------------
    // Replay loop
    //----------------------------------------------------------------------
    struct timeval t_start, t_end;
    gettimeofday(&t_start, NULL);

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
        if (!BacktestData_Load(ticks, &count, max_ticks, run_cfg->data_paths[f]))
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

        for (int i = 0; i < count; i++) {
            if (*cancel_flag) goto done;

            Tick<BACKTEST_FP> t = SharedBacktest_FromHistorical<BACKTEST_FP>(&ticks[i], (uint64_t)total_processed);

            // DEBUG: track price range
            if (ticks[i].price < price_lo) price_lo = ticks[i].price;
            if (ticks[i].price > price_hi) price_hi = ticks[i].price;
            last_price_d  = ticks[i].price;
            last_volume_d = ticks[i].qty;

            // Track E.1 — train-serve parity. Update EMA price every tick,
            // mirroring EngineSharded_Run lines 769-774 + legacy
            // PortfolioController_Tick. Driver reads the resulting value via
            // drv.ema_price on slow-path firings; without per-tick updates
            // sig->ema_sma_spread + sig->ema_above_sma stay stale or zero.
            FPN<BACKTEST_FP> ema_new = FPN_Add(
                FPN_Mul(ema_price, ema_alpha),
                FPN_Mul(t.price,    one_minus_alpha));
            if (FPN_IsZero(ema_price)) ema_price = t.price;
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
            //   2. Fan out to every core (1..num_cores)
            //   3. DrainEvents (process any entries / exits)
            //   4. On cadence: rebuild parameters, push, evaluate kill switch
            //   5. Track E.1 — fire on_slow_path hook (when registered)
            uint64_t prev_slow_runs = drv.slow_path_runs;
            ShardedBacktest_RunTick(&drv, t, total_processed);

            // Track E.7 — feed candle accumulator for the chart panel.
            // Throttled to every 100th tick (legacy mirrors this exactly at
            // BacktestEngine.hpp:761) — 1-min candles don't need every tick,
            // and unthrottled CandleAccumulator's mutex contention freezes
            // the GUI thread. Same pattern as EngineSharded fan_out (line
            // ~785).
            if (candle_acc && (total_processed % 100) == 0) {
                CandleAccumulator_PushWithTime(candle_acc,
                    ticks[i].price, ticks[i].qty,
                    ticks[i].is_buyer_maker,
                    (double)(ticks[i].timestamp_us / 1000000));
            }

            // Track E.2 — warmup-aware permission grant. Mirrors
            // EngineSharded_Run lines 999-1019. Pre-E.2, BacktestSharded set
            // permission=1 at startup, which let strategies fire on garbage
            // rolling stats during the first ticks. Now we grant permission
            // only after rolling.count crosses min_warmup_samples (default
            // 64 = half of W=128). Idempotent — once granted, stays granted.
            if (!warmup_permission_granted && drv.slow_path_runs > prev_slow_runs) {
                uint32_t min_samples = cfg.min_warmup_samples > 0
                    ? cfg.min_warmup_samples : 64;
                if (rolling.count >= (int)min_samples) {
                    for (int c = 0; c < num_cores; ++c) {
                        if (state.cores[c].strategy_id != STRATEGY_NONE) {
                            ExecutionCore_SetPermission(&cores[c], 1);
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
                        FPN_ToDouble(state.cores[0].pending_params.bg_price_threshold));
                fprintf(stderr, "  core[0] pending tp_price     : %.4f\n",
                        FPN_ToDouble(state.cores[0].pending_params.sg_take_profit_price));
                fprintf(stderr, "  core[0] pending sl_price     : %.4f\n",
                        FPN_ToDouble(state.cores[0].pending_params.sg_stop_loss_price));
                fprintf(stderr, "  core[0] pending trade_size   : %.8f\n",
                        FPN_ToDouble(state.cores[0].pending_params.trade_size));
                fprintf(stderr, "  core[0] pending strategy_id  : %u\n",
                        (unsigned)state.cores[0].pending_params.strategy_id);
                fprintf(stderr, "  core[0] pending flags        : 0x%02x\n",
                        (unsigned)state.cores[0].pending_params.flags);
                fprintf(stderr, "  core[0] permission           : %u\n",
                        (unsigned)__atomic_load_n(&cores[0].permission, __ATOMIC_ACQUIRE));
            }

            // After the drain, check if any new exits happened by comparing
            // realized_pnl. If it changed, classify as win/loss and bump the
            // equity curve.
            double current_realized = FPN_ToDouble(state.oms->realized_pnl);
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

                // Equity curve sample (one per completed trade).
                // dynamic growth — capping silently contaminates stats.
                if (BacktestResults_EnsureEquityCapacity(results, results->equity_count + 1)) {
                    double bal = FPN_ToDouble(state.oms->balance);
                    results->equity_curve[results->equity_count] = bal;
                    results->equity_count++;
                }
            }

            // v5.8.4c: shared inner-update helper — same code path as
            // BacktestStats_ComputeFromEquity's post-hoc walk. Bytewise
            // FP identity guaranteed by construction.
            double cur_equity = FPN_ToDouble(state.oms->balance);
            MaxDrawdown_UpdateIncremental(cur_equity, &peak_equity,
                                           &max_drawdown, &max_dd_pct);

            total_processed++;
            if ((total_processed & 0x3FFF) == 0) {
                *progress_pct = (int)(100.0 * total_processed / total_ticks_all_files);
            }
        }
    }

done:
    *progress_pct = 100;

    // Final drain to flush anything still in flight from the last tick
    EventLoop_DrainEvents(&state);

    gettimeofday(&t_end, NULL);
    double elapsed = (t_end.tv_sec - t_start.tv_sec) * 1000.0
                   + (t_end.tv_usec - t_start.tv_usec) / 1000.0;

    //----------------------------------------------------------------------
    // Populate BacktestResults stats
    //----------------------------------------------------------------------
    BacktestStats *stats = &results->stats;
    memset(stats, 0, sizeof(*stats));

    double final_balance = FPN_ToDouble(state.oms->balance);
    double final_pnl = FPN_ToDouble(state.oms->realized_pnl);

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
    double starting_bal = FPN_ToDouble(cfg.starting_balance);
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

    fprintf(stderr, "[backtest sharded] completed: %d ticks in %.1fms, %u trades (%u/%u W/L), P&L $%.2f\n",
            total_processed, elapsed,
            stats->total_trades, stats->wins, stats->losses,
            stats->total_pnl);
    fprintf(stderr, "[backtest sharded DEBUG] state.total_entries=%lu state.total_exits=%lu\n",
            (unsigned long)state.total_entries, (unsigned long)state.total_exits);
}

}  // namespace tt
