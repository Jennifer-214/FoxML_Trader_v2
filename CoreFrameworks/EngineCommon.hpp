// SPDX-License-Identifier: AGPL-3.0
//
// CoreFrameworks/EngineCommon.hpp
//
// Shared train-serve helpers for the per-core lifecycle.
// First canonical of the shared-helper-extract-for-train-serve-mirror-close pattern
// (DESIGN_SPECS/refactor-patterns/shared-helper-extract-for-train-serve-mirror-close.md).
//
// PURPOSE
// -------
// Both `EngineSharded_Run` (live; per_core_slow arch) and `BacktestSharded_Run`
// (backtest; centralized arch via AllCores wrapper) delegate per-core boot +
// per-core slow-path-cycle work to the helpers declared here. This collapses the
// Class 18 mirror between EngineSharded.hpp + BacktestSharded.hpp at the
// execution layer; closes PARITY-026/027/028/029/030/031/032 by-construction.
//
// LIFECYCLE EVENTS COVERED
// ------------------------
//   1. Boot: ApplyBnbDiscount (once)     → BootGlobal (once)   → BootPerCore (N times)
//   2. Slow-path-cycle: SlowPathCycleOneCore (per-thread per-core; live)
//                       SlowPathCycleAllCores (per-tick once; backtest fans via wrapper)
//
// HELPER COUNT: 5
//   - EngineCommon_ApplyBnbDiscount   — NON-CONST cfg ONE-SHOT mutator (sister to Sharded_ValidatePartialExitCfg)
//   - EngineCommon_BootGlobal         — const cfg one-shot global subsystem init
//   - EngineCommon_BootPerCore        — const cfg per-core boot
//   - EngineCommon_SlowPathCycleOneCore   — const cfg per-core slow-path-cycle body (atomic unit)
//   - EngineCommon_SlowPathCycleAllCores  — const cfg fan wrapper (~10 LOC; loops OneCore N times)
//
// CONST-CORRECTNESS DISCIPLINE
// ----------------------------
// Only ONE helper takes non-const cfg: EngineCommon_ApplyBnbDiscount. This is the
// SINGLE pre-loop cfg mutator. Any future cfg mutation must create its own
// sister `EngineCommon_ApplyXxx` helper — DO NOT mutate cfg inside BootGlobal /
// BootPerCore / SlowPathCycle* helpers (their signatures enforce this via
// `const ControllerConfig<F>&` reference type). Type system prevents drift.
//
// Sister precedent for non-const pre-loop helpers: `Sharded_ValidatePartialExitCfg`
// (existing one-shot cfg validator; same shape).
//
// PER-CALL-SITE EXEMPTION DISCIPLINE
// ----------------------------------
// Legitimate live-only / backtest-only differences are handled via:
//   - cfg flag branches at boot time (e.g., cfg.engine_arch dispatch)
//   - Conditional compile (#ifdef LATENCY_PROFILING)
//   - External wrapper before/after helper (e.g., bandit_state_prior_path operator override)
// NOT via nullable args — every helper takes reference (`&`), no pointer (`*`) args.
// NOT via cfg flags that duplicate semantics — that's a Class 24 anti-pattern.
//
// Legitimate live-only exemptions (per M5 false-positive surface):
//   - Persistence sinks: ShardedTradeLog_Init, OrderManager_OpenCalibrationLog
//   - Threading observability: CoreLatencyStats_Enable
// These STAY in EngineSharded_Run caller scope (NOT in helpers).
//
// STATIC-SCOPE DISCIPLINE (Decision G)
// ------------------------------------
// EngineSharded_Run holds ~30 function-scope `static` objects (g_notify_state /
// g_tick_rec / g_depth_shared / g_init_arena / g_calibration_log_file / etc.).
// These STAY in caller scope (process-lifetime + thread-shared semantics).
// Helpers MUST NOT define new statics.
//
// PARITY-031 CLOSURE: BACKTEST_REGIME_SAMPLE_CORE
// -----------------------------------------------
// Backtest samples regime from a SINGLE canonical core to preserve the pre-.B.4
// fc_ctx.regime_state semantic (single regime value per feature collector tick).
// Per-core regime variance IS collected at per-core inference time
// (state.cores[c].regime_state.current_regime in SlowPathCycleOneCore), but the
// backtest feature collector context downstream needs ONE regime value per tick
// (not [MAX_EXECUTION_CORES]).
//
// Live engine doesn't have this constraint: live inference accesses
// state.cores[c].regime_state per-core directly (canonical site EngineSharded:3194).
//
// Rationale for core 0 specifically: preserves sample_regimes=0 semantic that
// fc_ctx.regime_state held pre-.B.4. Future-readable: grep BACKTEST_REGIME_SAMPLE_CORE
// to find this constant. Future contributors can change the sampling strategy
// (e.g., majority-vote across cores) by updating this constant + comment.

#pragma once

#include <cstdint>  // uint64_t (used in slow-path-cycle helper signatures for ts_us)
#include <cstdio>   // fprintf (used in ApplyBnbDiscount stderr message)

// Phase B includes (added as helper bodies land; sister-convention relative paths):
//   B.0 ApplyBnbDiscount → ControllerConfig.hpp (cfg.cores[c].fee_rate_*) + FixedPointN.hpp (FPN<F> arithmetic)
//   B.1 BootGlobal → ControllerEventLoop.hpp (EventLoopState_Init + ConfigureKillSwitch) + OrderManager.hpp (OrderManagerState<F>) + RegimeDetector.hpp (Regime_Init)
#include "ControllerConfig.hpp"                  // ControllerConfig<F>, MAX_EXECUTION_CORES, MASK_RISK_CFG_KILL_SWITCH_ENABLED (transitive via RiskCfgFlagRegistry)
#include "ControllerEventLoop.hpp"               // EventLoopState<F>, EventLoopState_Init, EventLoopState_ConfigureKillSwitch, EventLoopState_RegisterCore, EventLoopState_SetCoreStrategy
#include "OrderManager.hpp"                      // OrderManagerState<F>
#include "ExecutionCore.hpp"                     // ExecutionCore<F>, ExecutionCore_Init, ExecutionCore_SetPermission, SPSCRing<Tick<F>, EXECUTION_CORE_TICK_RING_SIZE>
#include "ModelValidation.hpp"                   // CoreModelZoo_ValidateAgainstCfg (extracted at v5.14.2.E.1; closes PARITY-012)
#include "../FixedPoint/FixedPointN.hpp"         // FPN<F>, FPN_Mul, FPN_FromDouble, FPN_Zero, FPN_ToDouble
#include "../MemHeaders/CoreStateFlagRegistry.hpp"  // CORE_STATE_FLAG_SET, MASK_CORE_STATE_MODEL_LOAD_FAILED
#include "../Strategies/RegimeDetector.hpp"      // Regime_Init
#include "../Strategies/StrategyInterface.hpp"   // STRATEGY_ML, STRATEGY_NONE (auto-generated via FOREACH_STRATEGY X-macro)
#include "../Strategies/StrategyLifecycle.hpp"   // tt::Strategy_InitPerCore (closes PARITY-029)
#include "../ML_Headers/ModelInference.hpp"      // MODEL_BACKEND_XGBOOST
#include "../ML_Headers/CoreModelZoo.hpp"        // CoreModelZoo<F>, EnsembleModelZoo<F>, CoreModelZoo_Init, EnsembleModelZoo_Init, CoreModelZoo_LoadFromDir, CoreModelZoo_LoadLegacy, CoreModelZoo_PostLoadSetup, EnsembleModelZoo_AutoDetectFromDir, EnsembleModelZoo_PostLoadSetup, CoreModelZoo_Free, MASK_EZOO_ACTIVE
#include "../ML_Headers/ConfidenceScore.hpp"     // ConfidenceScorer_Init, ConfidenceScorer_BindCompositeCfg, CONFIDENCE_FRESHNESS_TAU_DEFAULT
#include "../ML_Headers/RollingTurnover.hpp"     // RollingTurnover_Init
#include "../ML_Headers/FeatureRegistryOverlay.hpp"  // FeatureOverlay_PostLoadVerify

// Phase B include enumeration (for body coding; uncomment as needed):
//   #include "CoreFrameworks/ControllerConfig.hpp"
//   #include "CoreFrameworks/EventLoopState.hpp"  // (if separate from ControllerEventLoop.hpp)
//   #include "CoreFrameworks/ControllerEventLoop.hpp"
//   #include "CoreFrameworks/OrderManager.hpp"
//   #include "CoreFrameworks/PortfolioController.hpp"
//   #include "CoreFrameworks/ExecutionCore.hpp"
//   #include "Strategies/StrategyParameters.hpp"
//   #include "ML_Headers/ConfidenceScore.hpp"
//   #include "ML_Headers/RollingTurnover.hpp"
//   #include "ML_Headers/ModelInference.hpp"           // per-core model load
//   #include "ML_Headers/CoreModelZoo.hpp"
//   #include "MemHeaders/BitmapMacros.hpp"
//   #include "FixedPoint/FixedPointN.hpp"
//   #include <cstdint>
// Verify at Phase B Step A audit time — actual includes resolved during body extract.

namespace tt {

// ==========================================================================
// Constants
// ==========================================================================

// Backtest regime sampling — see file-header doc block for rationale.
// Future contributors changing sampling strategy: update this constant + the
// comment block at file header (grep BACKTEST_REGIME_SAMPLE_CORE to find).
constexpr int BACKTEST_REGIME_SAMPLE_CORE = 0;

// ==========================================================================
// Helper declarations (5 total)
// ==========================================================================

// 1. EngineCommon_ApplyBnbDiscount
//    NON-CONST cfg ONE-SHOT mutator: applies BNB fee discount (0.75x) to all
//    per-core fee_rate_maker + fee_rate_taker when cfg.pay_fees_in_bnb is set.
//    Called ONCE per boot, BEFORE EngineCommon_BootGlobal. Sister to existing
//    Sharded_ValidatePartialExitCfg pre-loop cfg helper.
//
//    THE ONLY non-const-cfg helper in EngineCommon. All others take const ref.
//
//    Closes PARITY-030 by-construction: both EngineSharded + BacktestSharded
//    call this once, so BNB discount applies symmetrically.
//
//    Body extracted from EngineSharded.hpp:690-699 (verified at HEAD 64e7101).
//    Loop uses MAX_EXECUTION_CORES (compile-time max), not cfg.num_execution_cores —
//    preserves exact pre-extract semantic for any future-activated cores.
template <unsigned F>
inline void EngineCommon_ApplyBnbDiscount(ControllerConfig<F>& cfg) {
    if (cfg.pay_fees_in_bnb) {
        FPN<F> bnb_factor = FPN_FromDouble<F>(0.75);
        for (int c = 0; c < MAX_EXECUTION_CORES; ++c) {
            cfg.cores[c].fee_rate_maker = FPN_Mul(cfg.cores[c].fee_rate_maker, bnb_factor);
            cfg.cores[c].fee_rate_taker = FPN_Mul(cfg.cores[c].fee_rate_taker, bnb_factor);
        }
        fprintf(stderr,
            "[sharded] BNB fee discount ENABLED — applied per-core to cfg.cores[c].fee_rate_*"
            " (verify Binance UI 'pay fees in BNB' is also on)\n");
    }
}

// 2. EngineCommon_BootGlobal
//    const cfg ONE-SHOT global boot work (post-BNB-mutation):
//      - EventLoopState_Init(&state, &oms)
//      - EventLoopState_ConfigureKillSwitch (per PARITY-026 hotfix; gated on MASK_RISK_CFG_KILL_SWITCH_ENABLED)
//      - Regime_Init per-core loop (cfg.cores[i].regime_hysteresis → state.cores[i].regime_state)
//    EXCLUDES: function-scope statics (stay in caller per Decision G — trade_log + BinanceUserData
//              + NotifyState + TickRecorder/DepthRecorder + Notify worker spawn are M5 LIVE-only
//              persistence sinks / threading observability; stay in EngineSharded_Run caller scope)
//    EXCLUDES: BNB cfg mutation (now in ApplyBnbDiscount)
//
//    Called ONCE per boot, AFTER EngineCommon_ApplyBnbDiscount.
//
//    Body extracted from EngineSharded.hpp:742 + :749-753 + :760-762 (verified at HEAD 64e7101 +
//    Phase A Step A.4 enumeration). Closes PARITY-026 (kill_switch) + per-core regime init parity
//    sister-discipline (backtest already does the same at BacktestSharded.hpp:198 + :210-212).
template <unsigned F>
inline void EngineCommon_BootGlobal(const ControllerConfig<F>& cfg,
                                     EventLoopState<F>& state,
                                     OrderManagerState<F>& oms) {
    // 1. EventLoopState_Init (per Step A.4 :742)
    EventLoopState_Init(&state, &oms);

    // 2. KillSwitch configure (per Step A.4 :749-753; PARITY-026 closure)
    if (BITMAP_IS_SET(cfg.risk_cfg_flags, MASK_RISK_CFG_KILL_SWITCH_ENABLED)) {
        EventLoopState_ConfigureKillSwitch(&state,
            FPN_Zero<F>(),                       // no hard balance floor; drawdown-only kill
            cfg.kill_switch_drawdown_pct);
    }

    // 3. Regime_Init per-core (per Step A.4 :760-762; cfg-driven hysteresis;
    //    sister to BacktestSharded.hpp:210-212 — train-serve parity by-construction)
    for (int i = 0; i < MAX_EXECUTION_CORES; ++i) {
        Regime_Init(&state.cores[i].regime_state, (int)cfg.cores[i].regime_hysteresis);
    }
}

// 3. EngineCommon_BootPerCore
//    const cfg per-core boot:
//      - SPSCRing_Init(&tick_ring) + ExecutionCore_Init(&core, c, &tick_ring)
//      - EventLoopState_RegisterCore (per v1.5 D1 correction; NOT
//        OrderManager_RegisterCore which doesn't exist in the codebase)
//      - EventLoopState_SetCoreStrategy(&state, c, cfg.core_strategies[c], core_balance)
//      - ML branch (when cfg.core_strategies[c] == STRATEGY_ML && zoo_ptr && ezoo_ptr):
//          CoreModelZoo_Init + EnsembleModelZoo_Init + Load + PostLoadSetup +
//          ValidateAgainstCfg + FeatureOverlay_PostLoadVerify +
//          ConfidenceScorer_Init + ConfidenceScorer_BindCompositeCfg +
//          RollingTurnover_Init
//      - Strategy_InitPerCore (closes PARITY-029 — pre-v5.4 F7 bug; outside ML branch)
//      - ExecutionCore_SetPermission(&core, 0)
//
//    Called N times per boot (per-core loop). External wrappers (e.g.,
//    bandit_state_prior_path; oms.ezoo_refs LIVE-only wire; CoreLatencyStats_Enable
//    LIVE-only) called AFTER this returns per Decision B + M5 false-positive surface.
//
//    Caller responsibilities per v1.7 O4:
//      - Precompute core_balance per O2 bytewise-identical math (preserved from
//        LIVE :898-906 + :915-920 + BACKTEST :234-238 + :258-263 verbatim)
//      - Allocate zoo_ptr + ezoo_ptr per arch (LIVE: aligned_alloc(64) with
//        null-check + CORE_STATE_FLAG_SET(MODEL_LOAD_FAILED) on alloc fail;
//        BACKTEST: Free+Init static array element)
//      - Pass nullptr for both zoo_ptr + ezoo_ptr when non-ML strategy
//
//    Closes PARITY-027 (exit-model bind) + PARITY-028 (BindCompositeCfg +
//    RollingTurnover) + PARITY-029 (Strategy_InitPerCore) by-construction.
//
//    Signature (8 args per v1.7 O1 — drops unused oms; adds caller-owned
//    statics + nullable ML zoos + caller-precomputed core_balance):
template <unsigned F>
inline void EngineCommon_BootPerCore(const ControllerConfig<F>& cfg,
                                      int c,
                                      EventLoopState<F>& state,
                                      SPSCRing<Tick<F>, EXECUTION_CORE_TICK_RING_SIZE>& tick_ring,
                                      ExecutionCore<F>& core,
                                      CoreModelZoo<F>* zoo_ptr,        // nullable: non-ML OR alloc-failed
                                      EnsembleModelZoo<F>* ezoo_ptr,   // nullable: same
                                      FPN<F> core_balance) {           // caller-precomputed (O2 bytewise-identical)
    // -------- Step 1-4: unconditional per-core init (per Step A.4 CSV ordering) --------
    //   LIVE :909, BACKTEST :252 — SPSC ring init for producer→hot path
    SPSCRing_Init(&tick_ring);
    //   LIVE :910, BACKTEST :253 — ExecutionCore (per-core hot-path state) init
    ExecutionCore_Init(&core, (uint16_t)c, &tick_ring);
    //   LIVE :911-914, BACKTEST :255-256 — register core with EventLoop; intended_tp/sl/qty
    //   placeholders (slow-path rebuild fills in real values)
    EventLoopState_RegisterCore(&state, &core,
        FPN_Zero<F>(),  // intended_tp
        FPN_Zero<F>(),  // intended_sl
        FPN_Zero<F>()); // intended_qty
    //   LIVE :921-923, BACKTEST :264-266 — wire per-core strategy + risk budget
    //   (core_balance precomputed at caller per v1.6 O2 bytewise-identical math discipline)
    EventLoopState_SetCoreStrategy(&state, c, cfg.core_strategies[c], core_balance);

    // -------- Step 5: ML branch (when STRATEGY_ML && zoo storage available) --------
    //   Per v1.7 P-B + Decision B: items 5a-5m ALL fire ONLY for ML strategy cores with
    //   caller-provided zoo storage. Non-ML cores OR LIVE alloc-failed cores skip entirely
    //   (zoo_ptr nullptr); preserves current behavior. CSV row 76 NOTE: post-`.B.4`, BACKTEST
    //   also calls CORE_STATE_FLAG_SET(MODEL_LOAD_FAILED) on full-fail — acceptable; flag
    //   harmless in backtest (no display); train-serve identity preserved by-construction.
    if (cfg.core_strategies[c] == STRATEGY_ML && zoo_ptr && ezoo_ptr) {
        // 5a. Zoo init (LIVE :949 + :962, BACKTEST :278 + :281; caller already Free'd backtest static)
        CoreModelZoo_Init(zoo_ptr);
        EnsembleModelZoo_Init(ezoo_ptr);

        // 5b. Backend resolution (LIVE :963, BACKTEST :282) — cfg-driven with XGBoost default
        int backend = cfg.ml_backend ? cfg.ml_backend : MODEL_BACKEND_XGBOOST;

        // 5c. Single-zoo load — path 1 (cfg.core_model_dir[c] set; LoadFromDir auto-discovery)
        //   OR paths 2-3 (legacy single buy_signal via cfg.core_model_path[c] / cfg.ml_model_path)
        int loaded = 0;
        if (cfg.core_model_dir[c][0]) {
            // LIVE :978-985, BACKTEST :291-296 — full cfg-derived args (LIVE has extra
            // expected_feature_mask + cfg_ptr per v5.11.18 + v5.14.1.B.3; per Step A.4 CSV
            // row 72 MATCH — semantically identical 3-resolution-path; argument asymmetry
            // absorbed in body)
            uint64_t mask_for_load = (cfg.core_feature_mask[c] != 0xFFFFFFFFFFFFFFFFULL)
                ? cfg.core_feature_mask[c] : 0;
            loaded = CoreModelZoo_LoadFromDir(zoo_ptr, cfg.core_model_dir[c],
                backend, /*secret=*/nullptr, /*gap=*/0.05,
                /*strict=*/cfg.held_out_gate_strict,
                (int)BITMAP_IS_SET(cfg.ops_cfg_flags, MASK_OPS_CFG_ACKNOWLEDGE_CROSS_BINARY_DRIFT),
                /*expected_feature_mask=*/mask_for_load,
                /*cfg_ptr=*/&cfg);
            fprintf(stderr, "[sharded] core %d: zoo from %s, %d role(s) loaded\n",
                    c, cfg.core_model_dir[c], loaded);
        } else {
            // LIVE :990-1001, BACKTEST :299-311 — legacy fallback chain
            const char* model_path = cfg.core_model_path[c][0]
                ? cfg.core_model_path[c] : cfg.ml_model_path;
            if (model_path[0]) {
                loaded = CoreModelZoo_LoadLegacy(zoo_ptr, model_path, backend);
                if (loaded) {
                    fprintf(stderr, "[sharded] core %d: legacy buy_signal model loaded from %s\n",
                            c, model_path);
                } else {
                    fprintf(stderr, "[sharded] core %d: ML model load FAILED (%s), "
                                    "falling back to SimpleDip\n", c, model_path);
                }
            }
        }

        // 5d. PostLoadSetup + strict-mode unload (LIVE :1004-1023, BACKTEST :313-327)
        //   v5.14.2.E.1 canonical PostLoadSetup helper (FOREACH_SINGLE_ZOO_POST_LOAD walks
        //   today's VerifyExpected step). Returns 1 if all steps OK; 0 if any failed.
        //   Strict-mode action stays at boot caller: Free + null + MODEL_LOAD_FAILED.
        if (loaded) {
            state.cores[c].model_handle = zoo_ptr;
            if (cfg.core_model_dir[c][0]) {
                int post_ok = CoreModelZoo_PostLoadSetup<F>(zoo_ptr, cfg, c,
                                                             cfg.core_model_dir[c]);
                if (!post_ok && cfg.model_verify_strict > 0) {
                    fprintf(stderr, "[sharded] core %d: ML model UNLOADED due to strict verify failure\n", c);
                    CoreModelZoo_Free(zoo_ptr);
                    state.cores[c].model_handle = NULL;
                    CORE_STATE_FLAG_SET(state.cores[c], MODEL_LOAD_FAILED);
                }
            }
        }

        // 5e. Ensemble auto-detect (LIVE :1044-1083, BACKTEST :332-371) — multi-horizon
        //   sibling-dir support. v5.11.60 fix: runs REGARDLESS of single-zoo load result
        //   (multi-horizon-only deployments have no base dir; pre-fix path failed silently).
        //   v5.14.2.E.1 canonical PostLoadSetup walks FOREACH_ENSEMBLE_POST_LOAD steps.
        //   M5/CSV row 75 NOTE: oms.ezoo_refs[c] + oms.core_cfg_refs[c] wires are LIVE-only
        //   persistence sinks; STAY_IN_CALLER (caller wraps post-helper when ensemble_handle
        //   set). Same for BACKTEST bandit_state_prior_path override (Decision B).
        int ensemble_loaded = 0;
        if (cfg.core_model_dir[c][0]) {
            int n_loaded = EnsembleModelZoo_AutoDetectFromDir(
                ezoo_ptr,
                cfg.core_model_dir[c],
                backend,
                cfg.held_out_stamp_secret,
                FPN_ToDouble(cfg.gap_acceptable_threshold),
                cfg.held_out_gate_strict,
                (int)BITMAP_IS_SET(cfg.ops_cfg_flags, MASK_OPS_CFG_ACKNOWLEDGE_CROSS_BINARY_DRIFT));
            if (n_loaded > 0 && BITMAP_IS_SET(ezoo_ptr->init_flags, MASK_EZOO_ACTIVE)) {
                fprintf(stderr, "[sharded] core %d: ensemble active "
                                "(primary=%s, %d horizons; %d total models)\n",
                        c,
                        ezoo_ptr->primary_role_name[0]
                            ? ezoo_ptr->primary_role_name : "(none)",
                        ezoo_ptr->primary_count, n_loaded);
                EnsembleModelZoo_PostLoadSetup<F>(ezoo_ptr, cfg, c,
                                                   cfg.core_model_dir[c]);
                state.cores[c].ensemble_handle = ezoo_ptr;
                ensemble_loaded = 1;
            } else {
                state.cores[c].ensemble_handle = nullptr;
            }
        }

        // 5f. Display flag (LIVE :1085-1095, BACKTEST :N/A pre-`.B.4`) — only fires when
        //   BOTH single + ensemble load failed. Per CSV row 76: post-`.B.4` BACKTEST also
        //   sets this flag (acceptable; harmless in backtest no-display context; train-serve
        //   identity preserved by-construction).
        if (!loaded && !ensemble_loaded) {
            CORE_STATE_FLAG_SET(state.cores[c], MODEL_LOAD_FAILED);
        }

        // 5g. Cfg drift validators (LIVE :1102-1126, BACKTEST :380-401) — v5.10.2.A extracted;
        //   PARITY-012 closure (BACKTEST gained equivalence at v5.14.2.E.1). Counters written;
        //   FATAL log fires on REFUSE in strict mode; engine continues (TODO v5.10: free + refuse).
        //   v5.14.3.B FeatureOverlay sidecar verification (3-layer fingerprinting).
        if (loaded && cfg.core_model_dir[c][0]) {
            EnsembleModelZoo<F>* ezoo_for_validate =
                state.cores[c].ensemble_handle ? ezoo_ptr : nullptr;
            CoreModelZoo_ValidateAgainstCfg<F>(
                zoo_ptr, ezoo_for_validate, cfg, /*core_id=*/c,
                cfg.held_out_gate_strict,
                (int)BITMAP_IS_SET(cfg.ops_cfg_flags, MASK_OPS_CFG_ACKNOWLEDGE_INFERENCE_CFG_DRIFT),
                (int)BITMAP_IS_SET(cfg.ops_cfg_flags, MASK_OPS_CFG_ACKNOWLEDGE_CROSS_BINARY_DRIFT),
                &state.display_meta[c], &state.cores[c]);
            FeatureOverlay_PostLoadVerify<F>(
                zoo_ptr, ezoo_for_validate, /*core_id=*/c, cfg.held_out_gate_strict);
        }

        // 5h. ConfidenceScorer Init (LIVE :1136-1138, BACKTEST :408-410) — Phase 6prep
        //   sharded c12 + v5.14.9.D TECH_DEBT-004 close (tau hardcoded; legacy
        //   confidence_freshness_tau deleted; composite confidence v5.14.1 owns its own
        //   freshness via cfg.cores[c].confidence_freshness_tau_secs).
        ConfidenceScorer_Init(&state.cores[c].confidence,
                              (int)cfg.cores[c].confidence_window,
                              CONFIDENCE_FRESHNESS_TAU_DEFAULT);

        // 5i. ConfidenceScorer composite cfg bind (LIVE :1141-1146, BACKTEST :N/A)
        //   v5.14.1.B.1 PARITY-003 — push composite cfg into scorer. No-op when
        //   MASK_ML_CFG_CONFIDENCE_COMPOSITE_ENABLED unset (legacy path).
        //   NEW for BACKTEST per v1.7.2 PARITY-028 closure.
        ConfidenceScorer_BindCompositeCfg(&state.cores[c].confidence,
            BITMAP_IS_SET(cfg.ml_cfg_flags, MASK_ML_CFG_CONFIDENCE_COMPOSITE_ENABLED),
            FPN_ToDouble(cfg.cores[c].confidence_freshness_tau_secs),
            FPN_ToDouble(cfg.cores[c].confidence_capacity_target_dollars),
            FPN_ToDouble(cfg.cores[c].confidence_capacity_kappa),
            FPN_ToDouble(cfg.cores[c].confidence_rmse_baseline));

        // 5j. RollingTurnover Init (LIVE :1149-1151, BACKTEST :N/A)
        //   v5.14.1.G — re-init turnover with cfg-tunable window/topk (overrides
        //   EventLoopState_Init defaults of 100/3).
        //   NEW for BACKTEST per v1.7.2 PARITY-028 sister closure.
        RollingTurnover_Init(&state.cores[c].turnover,
                              cfg.cores[c].confidence_turnover_window,
                              cfg.cores[c].confidence_turnover_topk);
    }

    // -------- Step 6: Strategy_InitPerCore (OUTSIDE ML branch; gated by strategy_id) --------
    //   LIVE :1164-1168, BACKTEST :N/A pre-`.B.4` — v5.4.0 Phase 1.3 — wire Strategy_InitPerCore.
    //   Allocates the strategy state struct matching state.cores[c].strategy_id. Pre-warmup
    //   garbage initial values OK since permission=0 until warmup completes.
    //   PARITY-029 closure (pre-v5.4 F7 bug — BACKTEST never called this; entire strategy
    //   state lifecycle was orphaned for stateful strategies in backtest path).
    if (state.cores[c].strategy_id != STRATEGY_NONE) {
        tt::Strategy_InitPerCore(&state, c, state.cores[c].strategy_id,
                                  &state.cores[c].slow_state->rolling_short,
                                  &cfg);
    }

    // -------- Step 7: SetPermission (LIVE :1173, BACKTEST :417) --------
    //   Cores start permission=0 (no entries fire). Slow-path rebuild grants permission once
    //   rolling-stats samples warm up to compute meaningful gate thresholds. Pre-E.2,
    //   BACKTEST set permission=1 immediately — let strategies fire on garbage rolling stats
    //   during first ticks; now matches LIVE warmup discipline.
    ExecutionCore_SetPermission(&core, 0);
}

// 4. EngineCommon_SlowPathCycleOneCore
//    const cfg per-core slow-path-cycle body (atomic per-core unit):
//      - EventLoop_UpdateRollingStateOneCore
//      - EventLoop_RebuildOneCore (regime classification + strategy rebuild)
//      - EventLoop_TimeExitOneCore
//      - EventLoop_TrailingSLRatchetOneCore
//      - EventLoop_BreakevenOnProfit (PARITY-032 fold-in; was MISSING from
//        per_core_slow lambda pre-.B.4)
//      - ML exit-prediction submit (when MASK_ML_CFG_USE_EXIT_MODEL set)
//      - per-core regime collection (state.cores[c].regime_state populated)
//
//    Live: called per-core from per_core_slow thread (each thread invokes once
//    per slow-path cycle).
//    Backtest: called via SlowPathCycleAllCores wrapper (which loops N times
//    per tick).
//
//    Closes PARITY-031 (per-core regime) + PARITY-032 (breakeven) +
//    auxiliary by-construction.
template <unsigned F>
void EngineCommon_SlowPathCycleOneCore(const ControllerConfig<F>& cfg,
                                        int c,
                                        EventLoopState<F>& state,
                                        OrderManagerState<F>& oms,
                                        FPN<F> price,
                                        uint64_t ts_us);

// 5. EngineCommon_SlowPathCycleAllCores
//    const cfg fan wrapper (~10 LOC for-loop calling SlowPathCycleOneCore N times).
//    Per Option D future-orientation for v6.0 viewer decoupling boundary:
//    viewer reuses this wrapper as the natural per-tick mmap-publish API.
//
//    Backtest calls this ONCE per tick (ShardedBacktest_RunTick).
//    Live does NOT call this (each per_core_slow thread calls OneCore directly).
template <unsigned F>
void EngineCommon_SlowPathCycleAllCores(const ControllerConfig<F>& cfg,
                                         EventLoopState<F>& state,
                                         OrderManagerState<F>& oms,
                                         FPN<F> price,
                                         uint64_t ts_us);

}  // namespace tt
