// SPDX-License-Identifier: AGPL-3.0

//======================================================================================================
// [FILE]_[CoreFrameworks/EngineCommon.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [BOOT_TIME] [ML_INFERENCE]]
// [REFERENCE]_[DESIGN_SPEC]_[shared-helper-extract-for-train-serve-mirror-close]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[shared train-serve helpers — LIVE + BACKTEST both delegate per-node boot and slow-path-cycle here; closes the execution-layer Class-18 mirror (PARITY-026..032) by construction]
// [CONTAINS]
//   - [FUNCTION]_[EngineCommon_ApplyBnbDiscount]
//   - [FUNCTION]_[EngineCommon_BootGlobal]
//   - [FUNCTION]_[EngineCommon_BootPerCore]
//   - [FUNCTION]_[EngineCommon_SlowPathCycleOneCore]
//   - [FUNCTION]_[EngineCommon_SlowPathCycleAllCores]
//======================================================================================================
// Shared train-serve helpers for the per-core lifecycle.
// First canonical of the shared-helper-extract-for-train-serve-mirror-close pattern
// (DESIGN_SPECS/refactor-patterns/shared-helper-extract-for-train-serve-mirror-close.md).
//
// PURPOSE
// -------
// Both `EngineSharded_Run` (live; per_node_slow arch) and `BacktestSharded_Run`
// (backtest; iterates per-core via SlowPathCycleOneCore) delegate per-core boot +
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
//   - cfg flag branches at boot time (e.g., cfg.lifecycle_cfg_flags BITMAP_IS_SET dispatch)
//   - Conditional compile (#ifdef LATENCY_PROFILING)
//   - External wrapper before/after helper (e.g., bandit_state_prior_path operator override)
// NOT via nullable args — every helper takes reference (`&`), no pointer (`*`) args.
// NOT via cfg flags that duplicate semantics — that's a Class 24 anti-pattern.
//
// Legitimate live-only exemptions (per M5 false-positive surface):
//   - Persistence sinks: ShardedTradeLog_Init, OrderManager_OpenCalibrationLog
//   - Threading observability: NodeLatencyStats_Enable
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
// (state.nodes[c].regime_state.current_regime in SlowPathCycleOneCore), but the
// backtest feature collector context downstream needs ONE regime value per tick
// (not [MAX_EXECUTION_NODES]).
//
// Live engine doesn't have this constraint: live inference accesses
// state.nodes[c].regime_state per-core directly (canonical site: the exit-submit
// block in EngineCommon_SlowPathCycleOneCore below).
//
// Rationale for core 0 specifically: preserves sample_regimes=0 semantic that
// fc_ctx.regime_state held pre-.B.4. Future-readable: grep BACKTEST_REGIME_SAMPLE_CORE
// to find this constant. Future contributors can change the sampling strategy
// (e.g., majority-vote across cores) by updating this constant + comment.

#pragma once

#include <cstdint>      // uint64_t (used in slow-path-cycle helper signatures for ts_us)
#include <cstdio>       // fprintf (used in ApplyBnbDiscount stderr message)
#include <x86intrin.h>  // __rdtsc (slow-path latency sampling — sister to the run loop's rdtsc bracketing in EngineSharded/Run.hpp)

// Phase B includes (added as helper bodies land; sister-convention relative paths):
//   B.0 ApplyBnbDiscount → ControllerConfig.hpp (cfg.nodes[c].fee_rate_*) + FixedPointN.hpp (FPN_Binary<F> arithmetic)
//   B.1 BootGlobal → ControllerEventLoop.hpp (EventLoopState_Init + ConfigureKillSwitch) + OrderManager.hpp (OrderManagerState<F>) + RegimeDetector.hpp (Regime_Init)
#include "ControllerConfig.hpp"                  // ControllerConfig<F>, MAX_EXECUTION_NODES, MASK_RISK_CFG_KILL_SWITCH_ENABLED (transitive via RiskCfgFlagRegistry)
#include "ControllerEventLoop.hpp"               // EventLoopState<F>, EventLoopState_Init, EventLoopState_ConfigureKillSwitch, EventLoopState_RegisterCore, EventLoopState_SetCoreStrategy
#include "OrderManager.hpp"                      // OrderManagerState<F>
#include "ExecutionCore.hpp"                     // ExecutionCore<F>, ExecutionCore_Init, ExecutionCore_SetPermission, SPSCRing<Tick<F>, EXECUTION_NODE_TICK_RING_SIZE>
#include "ModelValidation.hpp"                   // NodeModelZoo_ValidateAgainstCfg (extracted at v5.14.2.E.1; closes PARITY-012)
#include "../FixedPoint/FixedPointN.hpp"         // FPN_Binary<F>, FPN_Mul, FPN_FromDouble, FPN_Zero, FPN_ToDouble
#include "../MemHeaders/NodeStateFlagRegistry.hpp"  // NODE_STATE_FLAG_SET, MASK_NODE_STATE_MODEL_LOAD_FAILED
#include "../Strategies/RegimeDetector.hpp"      // Regime_Init
#include "../Strategies/StrategyInterface.hpp"   // STRATEGY_ML, STRATEGY_NONE (auto-generated via FOREACH_STRATEGY X-macro)
#include "../Strategies/StrategyLifecycle.hpp"   // tt::Strategy_InitPerCore (closes PARITY-029)
#include "../ML_Headers/ModelInference.hpp"      // MODEL_BACKEND_XGBOOST
#include "../ML_Headers/NodeModelZoo.hpp"        // NodeModelZoo<F>, EnsembleModelZoo<F>, NodeModelZoo_Init, EnsembleModelZoo_Init, NodeModelZoo_LoadFromDir, NodeModelZoo_LoadLegacy, NodeModelZoo_PostLoadSetup, EnsembleModelZoo_AutoDetectFromDir, EnsembleModelZoo_PostLoadSetup, NodeModelZoo_Free, MASK_EZOO_ACTIVE
#include "../ML_Headers/ConfidenceScore.hpp"     // ConfidenceScorer_Init, ConfidenceScorer_BindCompositeCfg, CONFIDENCE_FRESHNESS_TAU_DEFAULT
#include "../ML_Headers/RollingTurnover.hpp"     // RollingTurnover_Init
#include "../ML_Headers/FeatureRegistryOverlay.hpp"  // FeatureOverlay_PostLoadVerify
// Phase B Step B.3 includes (v1.7.3 N-6 + N-2 + N-3 + N-4; landed at v1.7.3 amendment cycle):
#include "../DataStream/BinanceDepth.hpp"             // BookSnapshot<F> sister-canonical reuse per v1.7.3 N-6 (DepthSnapshot NOT invented; reuse existing canonical per feedback_audit_canonical_sister_before_new_infra)
#include "SlowPathGateRegistry.hpp"                   // SLOW_PATH_GATE_AUTOPOPULATE_ENGINE_WIDE macro + MASK_BREAKEVEN_ON_PROFIT cached gate bit (D1-B; v1.7.3 N-2 correct arg signature is (state.global_gate_state, cfg))

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
//   #include "ML_Headers/NodeModelZoo.hpp"
//   #include "MemHeaders/BitmapMacros.hpp"
//   #include "FixedPoint/FixedPointN.hpp"
//   #include <cstdint>
// Verify at Phase B Step A audit time — actual includes resolved during body extract.

namespace tt {

//------------------------------------------------------------------------------------------------------
// [SECTION]_[Constants]
//------------------------------------------------------------------------------------------------------

// Backtest regime sampling — see file-header doc block for rationale.
// Future contributors changing sampling strategy: update this constant + the
// comment block at file header (grep BACKTEST_REGIME_SAMPLE_CORE to find).
constexpr int BACKTEST_REGIME_SAMPLE_CORE = 0;

//------------------------------------------------------------------------------------------------------
// [SECTION]_[Helper declarations (5 total)]
//------------------------------------------------------------------------------------------------------

//======================================================================
// [FUNCTION]_[EngineCommon_ApplyBnbDiscount]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BOOT_TIME] [CFG_FLOW]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[THE one non-const-cfg helper — one-shot BNB 0.75x fee discount onto every per-node fee_rate_*; PARITY-030 closure]
// [REFERENCE]_[DECISION]_[D-173]
// [REFERENCE]_[PARITY]_[PARITY-30]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline void EngineCommon_ApplyBnbDiscount(ControllerConfig<F>& cfg) {
    if (cfg.pay_fees_in_bnb) {
        Money bnb_factor = Money{ 75000000 };  // exact 0.75 (D-173 BNB discount; runtime guard rides P3)
        for (int c = 0; c < MAX_EXECUTION_NODES; ++c) {
            cfg.nodes[c].fee_rate_maker = Money_Mul(cfg.nodes[c].fee_rate_maker, bnb_factor);
            cfg.nodes[c].fee_rate_taker = Money_Mul(cfg.nodes[c].fee_rate_taker, bnb_factor);
        }
        fprintf(stderr,
            "[sharded] BNB fee discount ENABLED — applied per-node to cfg.nodes[c].fee_rate_*"
            " (verify Binance UI 'pay fees in BNB' is also on)\n");
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
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
//    Loop uses MAX_EXECUTION_NODES (compile-time max), not cfg.num_execution_nodes —
//    preserves exact pre-extract semantic for any future-activated cores.
//======================================================================
// [END_FUNCTION]_[EngineCommon_ApplyBnbDiscount]
//======================================================================

//======================================================================
// [FUNCTION]_[EngineCommon_BootGlobal]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BOOT_TIME]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[one-shot global boot — EventLoopState_Init + kill-switch configure (PARITY-026) + per-node Regime_Init; statics stay in the caller (Decision G)]
// [REFERENCE]_[PARITY]_[PARITY-26]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline void EngineCommon_BootGlobal(const ControllerConfig<F>& cfg,
                                     EventLoopState<F>& state,
                                     OrderManagerState<F>& oms) {
    // 1. EventLoopState_Init (per Step A.4 :742)
    EventLoopState_Init(&state, &oms);

    // 2. KillSwitch configure (per Step A.4 :749-753; PARITY-026 closure)
    if (BITMAP_IS_SET(cfg.risk_cfg_flags, MASK_RISK_CFG_KILL_SWITCH_ENABLED)) {
        EventLoopState_ConfigureKillSwitch(&state,
            Money_Zero(),                        // no hard balance floor; drawdown-only kill
            cfg.kill_switch_drawdown_pct);
    }

    // 3. Regime_Init per-core (per Step A.4 :760-762; cfg-driven hysteresis;
    //    sister to BacktestSharded.hpp:210-212 — train-serve parity by-construction)
    for (int i = 0; i < MAX_EXECUTION_NODES; ++i) {
        Regime_Init(&state.nodes[i].regime_state, (int)cfg.nodes[i].regime_hysteresis);
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// 2. EngineCommon_BootGlobal
//    const cfg ONE-SHOT global boot work (post-BNB-mutation):
//      - EventLoopState_Init(&state, &oms)
//      - EventLoopState_ConfigureKillSwitch (per PARITY-026 hotfix; gated on MASK_RISK_CFG_KILL_SWITCH_ENABLED)
//      - Regime_Init per-core loop (cfg.nodes[i].regime_hysteresis → state.nodes[i].regime_state)
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
//======================================================================
// [END_FUNCTION]_[EngineCommon_BootGlobal]
//======================================================================

//======================================================================
// [FUNCTION]_[EngineCommon_BootPerCore]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BOOT_TIME] [ML_INFERENCE]]
// [REFERENCE]_[INVARIANT]_[H22]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[per-node boot — ring/core init, register, strategy wire, full ML branch (zoo load + validate + confidence + turnover), Strategy_InitPerCore, permission=0; PARITY-027/028/029 closure]
// [REFERENCE]_[DECISION]_[D-221]
// [REFERENCE]_[PARITY]_[[PARITY-3] [PARITY-12] [PARITY-27] [PARITY-28] [PARITY-29]]
// [REFERENCE]_[TECH_DEBT]_[TECH_DEBT-4]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline void EngineCommon_BootPerCore(const ControllerConfig<F>& cfg,
                                      int c,
                                      EventLoopState<F>& state,
                                      SPSCRing<Tick<F>, EXECUTION_NODE_TICK_RING_SIZE>& tick_ring,
                                      ExecutionCore<F>& core,
                                      NodeModelZoo<F>* zoo_ptr,        // nullable: non-ML OR alloc-failed
                                      EnsembleModelZoo<F>* ezoo_ptr,   // nullable: same
                                      Money node_balance) {           // caller-precomputed (O2 bytewise-identical)
    // -------- Step 1-4: unconditional per-core init (per Step A.4 CSV ordering) --------
    //   LIVE :909, BACKTEST :252 — SPSC ring init for producer→hot path
    SPSCRing_Init(&tick_ring);
    //   LIVE :910, BACKTEST :253 — ExecutionCore (per-core hot-path state) init
    ExecutionCore_Init(&core, (uint16_t)c, &tick_ring);
    //   LIVE :911-914, BACKTEST :255-256 — register core with EventLoop; intended_tp/sl/qty
    //   placeholders (slow-path rebuild fills in real values)
    EventLoopState_RegisterCore(&state, &core,
        Money_Zero(),  // intended_tp
        Money_Zero(),  // intended_sl
        Money_Zero()); // intended_qty
    //   LIVE :921-923, BACKTEST :264-266 — wire per-core strategy + risk budget
    //   (node_balance precomputed at caller per v1.6 O2 bytewise-identical math discipline)
    EventLoopState_SetCoreStrategy(&state, c, cfg.node_strategies[c], node_balance);

    // -------- Step 5: ML branch (when STRATEGY_ML && zoo storage available) --------
    //   Per v1.7 P-B + Decision B: items 5a-5m ALL fire ONLY for ML strategy cores with
    //   caller-provided zoo storage. Non-ML cores OR LIVE alloc-failed cores skip entirely
    //   (zoo_ptr nullptr); preserves current behavior. CSV row 76 NOTE: post-`.B.4`, BACKTEST
    //   also calls NODE_STATE_FLAG_SET(MODEL_LOAD_FAILED) on full-fail — acceptable; flag
    //   harmless in backtest (no display); train-serve identity preserved by-construction.
    if (cfg.node_strategies[c] == STRATEGY_ML && zoo_ptr && ezoo_ptr) {
        // 5a. Zoo init (LIVE :949 + :962, BACKTEST :278 + :281; caller already Free'd backtest static)
        NodeModelZoo_Init(zoo_ptr);
        EnsembleModelZoo_Init(ezoo_ptr);

        // 5b. Backend resolution (LIVE :963, BACKTEST :282) — cfg-driven with XGBoost default
        int backend = cfg.ml_backend ? cfg.ml_backend : MODEL_BACKEND_XGBOOST;

        // 5c. Single-zoo load — path 1 (cfg.node_model_dir[c] set; LoadFromDir auto-discovery)
        //   OR paths 2-3 (legacy single buy_signal via cfg.node_model_path[c] / cfg.ml_model_path)
        int loaded = 0;
        if (cfg.node_model_dir[c][0]) {
            // LIVE :978-985, BACKTEST :291-296 — full cfg-derived args (LIVE has extra
            // expected_feature_mask + cfg_ptr per v5.11.18 + v5.14.1.B.3; per Step A.4 CSV
            // row 72 MATCH — semantically identical 3-resolution-path; argument asymmetry
            // absorbed in body)
            uint64_t mask_for_load = (cfg.node_feature_mask[c] != 0xFFFFFFFFFFFFFFFFULL)
                ? cfg.node_feature_mask[c] : 0;
            loaded = NodeModelZoo_LoadFromDir(zoo_ptr, cfg.node_model_dir[c],
                backend, /*secret=*/nullptr, /*gap=*/0.05,
                /*strict=*/cfg.held_out_gate_strict,
                (int)BITMAP_IS_SET(cfg.ops_cfg_flags, MASK_OPS_CFG_ACKNOWLEDGE_CROSS_BINARY_DRIFT),
                /*expected_feature_mask=*/mask_for_load,
                /*cfg_ptr=*/&cfg);
            fprintf(stderr, "[sharded] node %d: zoo from %s, %d role(s) loaded\n",
                    c, cfg.node_model_dir[c], loaded);
        } else {
            // LIVE :990-1001, BACKTEST :299-311 — legacy fallback chain
            const char* model_path = cfg.node_model_path[c][0]
                ? cfg.node_model_path[c] : cfg.ml_model_path;
            if (model_path[0]) {
                loaded = NodeModelZoo_LoadLegacy(zoo_ptr, model_path, backend);
                if (loaded) {
                    fprintf(stderr, "[sharded] node %d: legacy buy_signal model loaded from %s\n",
                            c, model_path);
                } else {
                    fprintf(stderr, "[sharded] node %d: ML model load FAILED (%s), "
                                    "falling back to SimpleDip\n", c, model_path);
                }
            }
        }

        // 5d. PostLoadSetup + strict-mode unload (LIVE :1004-1023, BACKTEST :313-327)
        //   v5.14.2.E.1 canonical PostLoadSetup helper (FOREACH_SINGLE_ZOO_POST_LOAD walks
        //   today's VerifyExpected step). Returns 1 if all steps OK; 0 if any failed.
        //   Strict-mode action stays at boot caller: Free + null + MODEL_LOAD_FAILED.
        if (loaded) {
            state.nodes[c].model_handle = zoo_ptr;
            if (cfg.node_model_dir[c][0]) {
                int post_ok = NodeModelZoo_PostLoadSetup<F>(zoo_ptr, cfg, c,
                                                             cfg.node_model_dir[c]);
                if (!post_ok && cfg.model_verify_strict > 0) {
                    fprintf(stderr, "[sharded] node %d: ML model UNLOADED due to strict verify failure\n", c);
                    NodeModelZoo_Free(zoo_ptr);
                    state.nodes[c].model_handle = NULL;
                    NODE_STATE_FLAG_SET(state.nodes[c], MODEL_LOAD_FAILED);
                }
            }
        }

        // 5e. Ensemble auto-detect (LIVE :1044-1083, BACKTEST :332-371) — multi-horizon
        //   sibling-dir support. v5.11.60 fix: runs REGARDLESS of single-zoo load result
        //   (multi-horizon-only deployments have no base dir; pre-fix path failed silently).
        //   v5.14.2.E.1 canonical PostLoadSetup walks FOREACH_ENSEMBLE_POST_LOAD steps.
        //   M5/CSV row 75 NOTE: oms.ezoo_refs[c] + oms.node_cfg_refs[c] wires are LIVE-only
        //   persistence sinks; STAY_IN_CALLER (caller wraps post-helper when ensemble_handle
        //   set). Same for BACKTEST bandit_state_prior_path override (Decision B).
        int ensemble_loaded = 0;
        if (cfg.node_model_dir[c][0]) {
            int n_loaded = EnsembleModelZoo_AutoDetectFromDir(
                ezoo_ptr,
                cfg.node_model_dir[c],
                backend,
                cfg.held_out_stamp_secret,
                FPN_ToDouble(cfg.gap_acceptable_threshold),
                cfg.held_out_gate_strict,
                (int)BITMAP_IS_SET(cfg.ops_cfg_flags, MASK_OPS_CFG_ACKNOWLEDGE_CROSS_BINARY_DRIFT));
            if (n_loaded > 0 && BITMAP_IS_SET(ezoo_ptr->init_flags, MASK_EZOO_ACTIVE)) {
                fprintf(stderr, "[sharded] node %d: ensemble active "
                                "(primary=%s, %d horizons; %d total models)\n",
                        c,
                        ezoo_ptr->primary_role_name[0]
                            ? ezoo_ptr->primary_role_name : "(none)",
                        ezoo_ptr->primary_count, n_loaded);
                EnsembleModelZoo_PostLoadSetup<F>(ezoo_ptr, cfg, c,
                                                   cfg.node_model_dir[c]);
                // v5.15.5.E.0.10 A6 ingress (D-221) — post-load corrupt finalize: union corrupt
                // arms into disabled_horizon_mask + the per-node majority-corrupt verdict. Runs
                // AFTER PostLoadSetup (incl. SetDisabledHorizons) so the disabled-union can't be
                // wiped. Single-threaded at boot; sets MODEL_CORRUPT (distinct from MODEL_LOAD_FAILED).
                if (EnsembleZoo_FinalizeCorrupt<F>(ezoo_ptr, FPN_ToDouble(cfg.model_corrupt_shalt_ratio))) {
                    NODE_STATE_FLAG_SET(state.nodes[c], MODEL_CORRUPT);
                    fprintf(stderr, "[model] node %d: ML barrier CORRUPT for the majority of "
                                    "ensemble arms (%d of %d) — node REFUSES new trades until "
                                    "RETRAIN (D-221)\n",
                            c, __builtin_popcount((unsigned)ezoo_ptr->corrupt_arms_mask),
                            ezoo_ptr->buy_signal_count);
                }
                state.nodes[c].ensemble_handle = ezoo_ptr;
                ensemble_loaded = 1;
            } else {
                state.nodes[c].ensemble_handle = nullptr;
            }
        }

        // 5f. Display flag (LIVE :1085-1095, BACKTEST :N/A pre-`.B.4`) — only fires when
        //   BOTH single + ensemble load failed. Per CSV row 76: post-`.B.4` BACKTEST also
        //   sets this flag (acceptable; harmless in backtest no-display context; train-serve
        //   identity preserved by-construction).
        if (!loaded && !ensemble_loaded) {
            NODE_STATE_FLAG_SET(state.nodes[c], MODEL_LOAD_FAILED);
        }

        // 5g. Cfg drift validators (LIVE :1102-1126, BACKTEST :380-401) — v5.10.2.A extracted;
        //   PARITY-012 closure (BACKTEST gained equivalence at v5.14.2.E.1). Counters written;
        //   FATAL log fires on REFUSE in strict mode; engine continues (TODO v5.10: free + refuse).
        //   v5.14.3.B FeatureOverlay sidecar verification (3-layer fingerprinting).
        if (loaded && cfg.node_model_dir[c][0]) {
            EnsembleModelZoo<F>* ezoo_for_validate =
                state.nodes[c].ensemble_handle ? ezoo_ptr : nullptr;
            NodeModelZoo_ValidateAgainstCfg<F>(
                zoo_ptr, ezoo_for_validate, cfg, /*node_id=*/c,
                cfg.held_out_gate_strict,
                (int)BITMAP_IS_SET(cfg.ops_cfg_flags, MASK_OPS_CFG_ACKNOWLEDGE_INFERENCE_CFG_DRIFT),
                (int)BITMAP_IS_SET(cfg.ops_cfg_flags, MASK_OPS_CFG_ACKNOWLEDGE_CROSS_BINARY_DRIFT),
                &state.display_meta[c], &state.nodes[c]);
            FeatureOverlay_PostLoadVerify<F>(
                zoo_ptr, ezoo_for_validate, /*node_id=*/c, cfg.held_out_gate_strict);
        }

        // 5h. ConfidenceScorer Init (LIVE :1136-1138, BACKTEST :408-410) — Phase 6prep
        //   sharded c12 + v5.14.9.D TECH_DEBT-004 close (tau hardcoded; legacy
        //   confidence_freshness_tau deleted; composite confidence v5.14.1 owns its own
        //   freshness via cfg.nodes[c].confidence_freshness_tau_secs).
        ConfidenceScorer_Init(&state.nodes[c].confidence,
                              (int)cfg.nodes[c].confidence_window,
                              CONFIDENCE_FRESHNESS_TAU_DEFAULT);

        // 5i. ConfidenceScorer composite cfg bind (LIVE :1141-1146, BACKTEST :N/A)
        //   v5.14.1.B.1 PARITY-003 — push composite cfg into scorer. No-op when
        //   MASK_ML_CFG_CONFIDENCE_COMPOSITE_ENABLED unset (legacy path).
        //   NEW for BACKTEST per v1.7.2 PARITY-028 closure.
        ConfidenceScorer_BindCompositeCfg(&state.nodes[c].confidence,
            BITMAP_IS_SET(cfg.ml_cfg_flags, MASK_ML_CFG_CONFIDENCE_COMPOSITE_ENABLED),
            FPN_ToDouble(cfg.nodes[c].confidence_freshness_tau_secs),
            FPN_ToDouble(cfg.nodes[c].confidence_capacity_target_dollars),
            FPN_ToDouble(cfg.nodes[c].confidence_capacity_kappa),
            FPN_ToDouble(cfg.nodes[c].confidence_rmse_baseline));

        // 5j. RollingTurnover Init (LIVE :1149-1151, BACKTEST :N/A)
        //   v5.14.1.G — re-init turnover with cfg-tunable window/topk (overrides
        //   EventLoopState_Init defaults of 100/3).
        //   NEW for BACKTEST per v1.7.2 PARITY-028 sister closure.
        RollingTurnover_Init(&state.nodes[c].turnover,
                              cfg.nodes[c].confidence_turnover_window,
                              cfg.nodes[c].confidence_turnover_topk);
    }

    // -------- Step 6: Strategy_InitPerCore (OUTSIDE ML branch; gated by strategy_id) --------
    //   LIVE :1164-1168, BACKTEST :N/A pre-`.B.4` — v5.4.0 Phase 1.3 — wire Strategy_InitPerCore.
    //   Allocates the strategy state struct matching state.nodes[c].strategy_id. Pre-warmup
    //   garbage initial values OK since permission=0 until warmup completes.
    //   PARITY-029 closure (pre-v5.4 F7 bug — BACKTEST never called this; entire strategy
    //   state lifecycle was orphaned for stateful strategies in backtest path).
    if (state.nodes[c].strategy_id != STRATEGY_NONE) {
        tt::Strategy_InitPerCore(&state, c, state.nodes[c].strategy_id,
                                  &state.nodes[c].slow_state->rolling_short,
                                  &cfg);
    }

    // -------- Step 7: SetPermission (LIVE :1173, BACKTEST :417) --------
    //   Cores start permission=0 (no entries fire). Slow-path rebuild grants permission once
    //   rolling-stats samples warm up to compute meaningful gate thresholds. Pre-E.2,
    //   BACKTEST set permission=1 immediately — let strategies fire on garbage rolling stats
    //   during first ticks; now matches LIVE warmup discipline.
    ExecutionCore_SetPermission(&core, 0);
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// 3. EngineCommon_BootPerCore
//    const cfg per-core boot:
//      - SPSCRing_Init(&tick_ring) + ExecutionCore_Init(&core, c, &tick_ring)
//      - EventLoopState_RegisterCore (per v1.5 D1 correction; NOT
//        OrderManager_RegisterCore which doesn't exist in the codebase)
//      - EventLoopState_SetCoreStrategy(&state, c, cfg.node_strategies[c], node_balance)
//      - ML branch (when cfg.node_strategies[c] == STRATEGY_ML && zoo_ptr && ezoo_ptr):
//          NodeModelZoo_Init + EnsembleModelZoo_Init + Load + PostLoadSetup +
//          ValidateAgainstCfg + FeatureOverlay_PostLoadVerify +
//          ConfidenceScorer_Init + ConfidenceScorer_BindCompositeCfg +
//          RollingTurnover_Init
//      - Strategy_InitPerCore (closes PARITY-029 — pre-v5.4 F7 bug; outside ML branch)
//      - ExecutionCore_SetPermission(&core, 0)
//
//    Called N times per boot (per-core loop). External wrappers (e.g.,
//    bandit_state_prior_path; oms.ezoo_refs LIVE-only wire; NodeLatencyStats_Enable
//    LIVE-only) called AFTER this returns per Decision B + M5 false-positive surface.
//
//    Caller responsibilities per v1.7 O4:
//      - Precompute node_balance per O2 bytewise-identical math (preserved from
//        LIVE :898-906 + :915-920 + BACKTEST :234-238 + :258-263 verbatim)
//      - Allocate zoo_ptr + ezoo_ptr per arch (LIVE: aligned_alloc(64) with
//        null-check + NODE_STATE_FLAG_SET(MODEL_LOAD_FAILED) on alloc fail;
//        BACKTEST: Free+Init static array element)
//      - Pass nullptr for both zoo_ptr + ezoo_ptr when non-ML strategy
//
//    Closes PARITY-027 (exit-model bind) + PARITY-028 (BindCompositeCfg +
//    RollingTurnover) + PARITY-029 (Strategy_InitPerCore) by-construction.
//
//    Signature (8 args per v1.7 O1 — drops unused oms; adds caller-owned
//    statics + nullable ML zoos + caller-precomputed node_balance):
//======================================================================
// [END_FUNCTION]_[EngineCommon_BootPerCore]
//======================================================================

//======================================================================
// [FUNCTION]_[EngineCommon_SlowPathCycleOneCore]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [SLOW_PATH] [ML_INFERENCE]]
// [REFERENCE]_[INVARIANT]_[[H8] [H22]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[THE per-node slow-path body (LIVE per-thread; BACKTEST via the fan wrapper) — rolling update, rebuild, seqlock push, ML exit submit, time-exit/trail/breakeven, warmup permission; PARITY-031/032 closure]
// [REFERENCE]_[CLASS]_[25]
// [REFERENCE]_[PARITY]_[[PARITY-31] [PARITY-32]]
// [REFERENCE]_[MEMORY]_[[feedback_audit_canonical_sister_before_new_infra] [feedback_motivated_collaborator_for_caramel]]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline void EngineCommon_SlowPathCycleOneCore(const ControllerConfig<F>& cfg,
                                               int c,
                                               EventLoopState<F>& state,
                                               OrderManagerState<F>& oms,
                                               Money price,
                                               Money volume,
                                               uint64_t ts_us,
                                               uint64_t now_tick,
                                               const BookSnapshot<F>& depth) {
    // v1.7.3 HIGH-4 Telemetry Path A INTERNAL: helper computes own rdtsc bracket
    // start + 5 NodeLatencyStats_Sample calls inside body. Slight BACKTEST
    // overhead (~25-50ns per cycle for rdtsc + array writes) preserves LIVE
    // per-section breakdown semantic (M5 LIVE-only display surface kept
    // untouched). Per feedback_motivated_collaborator_for_caramel — preserve
    // LIVE semantic > save BACKTEST ns.
    uint64_t _sp_t0 = __rdtsc();
    uint64_t _sec_t_other_start = _sp_t0;

    // v1.7.1 M2.B + v1.7.3 N-2 + D1-B: refresh engine-wide gate cache at body
    // entry BEFORE BITMAP_IS_SET reads downstream. Sister consumer pattern:
    // ControllerEventLoop.hpp's two SLOW_PATH_GATE_AUTOPOPULATE_ENGINE_WIDE
    // call sites. Macro signature `(state.global_gate_state,
    // cfg)` per v1.7.3 N-2 correction — was incorrectly `(state, cfg)` which
    // would COMPILE FAIL since EventLoopState<F> has no `.flags` member.
    SLOW_PATH_GATE_AUTOPOPULATE_ENGINE_WIDE(state.global_gate_state, cfg);

    // Derive double from caller-precomputed FPN_Binary<F> price for guard checks.
    // `price` IS the mtm_price per caller-side O2 bytewise-identical math
    // (LIVE :3068-3071 / BACKTEST equivalent — caller does
    // price = price_d > 0.0 ? FPN_FromDouble(price_d) : FPN_Zero()). FPN_Binary<F=64>
    // has 64 fractional bits + ~4032 integer bits; FPN_ToDouble of
    // FPN_FromDouble(x) recovers x bytewise-identical for normal doubles.
    double price_d = Money_ToDouble(price);

    // === Read shared market state (eventually-consistent) ===
    // Producer is single writer; slow-paths read with relaxed
    // ordering. Stale-by-poll-interval is acceptable for
    // slow-path strategy dispatch (always was — pre-migration
    // producer's slow-path also operated on whatever rolling
    // values were current at slow-path entry).
    //
    // Caller pre-resolved depth (LIVE: g_depth_shared.snapshots[active_idx];
    // BACKTEST: BookSnapshot constructed from ShardedBacktestDriver<F> pointer
    // fields per v1.7.4 NEW-1/2/3/4 corrections). Helper checks
    // MASK_GATE_CFG_DEPTH_ENABLED internally per LIVE :3052-3058 pattern —
    // when disabled, substitutes FPN_Zero regardless of what's in passed depth.
    FPN_Binary<F> book_imb = FPN_Zero<F>();
    double book_spread_d = 0.0, book_mid_d = 0.0;
    if (BITMAP_IS_SET(cfg.gate_cfg_flags, MASK_GATE_CFG_DEPTH_ENABLED)) {
        book_imb      = depth.imbalance;
        book_spread_d = FPN_ToDouble(depth.spread);
        book_mid_d    = FPN_ToDouble(depth.mid_price);
    }

    // Pre-loop scalar (matches RebuildAllParameters wrapper).
    int book_imbalance_blocked = 0;
    if (BITMAP_IS_SET(cfg.gate_cfg_flags, MASK_GATE_CFG_DEPTH_ENABLED) && !FPN_IsZero(cfg.min_book_imbalance)) {
        book_imbalance_blocked = FPN_LessThan(book_imb,
            cfg.min_book_imbalance) ? 1 : 0;
    }

    // rebuild_ts_us = caller-precomputed ts_us. LIVE caller does
    // std::chrono::system_clock::now() → microseconds at slow-path entry;
    // BACKTEST caller passes synthesized ts. Local alias preserved for
    // body-symbol parity with LIVE :3073 (bytewise-identical math).
    uint64_t rebuild_ts_us = ts_us;

    // v5.1.2 (full symmetric): use shared OneCore helper.
    // Single-writer is this thread (per_node_slow's c).
    auto* sst = state.nodes[c].slow_state;
    FPN_Binary<F> bs = BITMAP_IS_SET(cfg.gate_cfg_flags, MASK_GATE_CFG_DEPTH_ENABLED) ?
        FPN_FromDouble<F>(book_spread_d) : FPN_Zero<F>();
    EventLoop_UpdateRollingStateOneCore(
        &state, c,
        price, volume, rebuild_ts_us,
        /*is_buyer_maker=*/0, // TODO(parity-check Finding #5): plumb through scalar bus (v5.10.X)
        BITMAP_IS_SET(cfg.gate_cfg_flags, MASK_GATE_CFG_DEPTH_ENABLED) ? book_imb : FPN_Zero<F>(),
        bs,
        BITMAP_IS_SET(cfg.gate_cfg_flags, MASK_GATE_CFG_DEPTH_ENABLED) ? 1 : 0);

    // v5.1.1: bracket OTHER section (depth read + swap pickup
    // + per-cadence pushes setup).
    uint64_t _sec_t_rebuild_start = __rdtsc();
    NodeLatencyStats_Sample(
        &state.display_meta[c].slow_path_breakdown[tt::SP_SECTION_ROLLING],
        _sec_t_rebuild_start - _sec_t_other_start, _sec_t_rebuild_start);

    // === Strategy dispatch + gate parameter rebuild ===
    // v5.1.0: pass per-core slow_state pointers instead of
    // producer-shared state. Each engine reads ONLY its own.
    // v5.12.1.B clock hoist: pass rebuild_ts_us as now_us so
    // the recovery refusal check inside RebuildOneCore reuses
    // it instead of doing its own clock_gettime. Saves ~50ns/
    // cycle in the post-flatten recovery window.
    EventLoop_RebuildOneCore(
        &state, c, &sst->rolling_short, &cfg, &sst->rolling_long,
        &sst->regime_ror, &sst->ema_price,
        Money_IsZero(price) ? nullptr : &price,
        &sst->rolling_medium, &sst->rolling_baseline,
        &sst->cumdelta_state, &sst->tick_rate_state, rebuild_ts_us,
        BITMAP_IS_SET(cfg.gate_cfg_flags, MASK_GATE_CFG_DEPTH_ENABLED) ? &book_imb : nullptr,
        &sst->book_imb_history, &sst->flow_state,
        &sst->large_trade_state, &sst->spread_state,
        book_spread_d, book_mid_d, book_imbalance_blocked,
        /*now_us (clock hoist)=*/rebuild_ts_us);

    // v5.1.1: bracket REBUILD section.
    uint64_t _sec_t_push_start = __rdtsc();
    NodeLatencyStats_Sample(
        &state.display_meta[c].slow_path_breakdown[tt::SP_SECTION_REBUILD],
        _sec_t_push_start - _sec_t_rebuild_start, _sec_t_push_start);

    // === Push pending_params via seqlock (was inside
    // PushParameters wrapper; inline for per-core path).
    // v5.12.1.B.2 — pass publish_tick = now_tick (caller-precomputed
    // from ticks_produced.load() in LIVE; tick_index in BACKTEST)
    // so hot-path staleness gate sees fresh tick stamp.
    if (NODE_STATE_FLAG_IS_SET(state.nodes[c], DIRTY)) {
        ExecutionCore<F>* core = state.nodes[c].core;
        if (core) {
            ExecutionCore_SetParameters(core,
                state.nodes[c].pending_params,
                now_tick);
        }
        NODE_STATE_FLAG_CLR(state.nodes[c], DIRTY);
    }

    // v5.1.1: bracket PUSH_PARAMS section.
    uint64_t _sec_t_te_start = __rdtsc();
    NodeLatencyStats_Sample(
        &state.display_meta[c].slow_path_breakdown[tt::SP_SECTION_PUSH],
        _sec_t_te_start - _sec_t_push_start, _sec_t_te_start);

    // === v5.13.0.B — sell-side ML exit-prediction submit ===
    // RebuildOneCore wrote state.nodes[c].last_exit_prediction
    // (when BITMAP_IS_SET(cfg.ml_cfg_flags, MASK_ML_CFG_USE_EXIT_MODEL) && exit_predictor models loaded).
    // If above threshold and any positions are open on this
    // core's slot(s), fire MARKET_SELL via OMS_PushSubmit and
    // mark per-slot last_exit_predicted_bitmap for v5.13.4 reward
    // attribution. Default cfg path (use_exit_model=0): the
    // last_exit_prediction stays 0.0 → ~5ns flag check + skip.
    if (BITMAP_IS_SET(cfg.ml_cfg_flags, MASK_ML_CFG_USE_EXIT_MODEL)
        && state.nodes[c].last_exit_prediction
           > FPN_ToDouble(cfg.nodes[c].exit_threshold)  // Class 25 scope-discipline: per-node read at per-node scope (value-equivalent via walker propagation; future-proofs against per-node override addition)
        && price_d > 0.01) {
        // Slot mask: under partials each core owns 2 slots
        // (legs A + B); single-leg under partial_exit_enabled=0.
        // v5.15.5.C.2 (S3a) — bit-packed in oms_state_flags.
        int partial_on = BITMAP_IS_SET(oms.oms_state_flags, tt::MASK_OMS_STATE_PARTIAL_EXIT_ENABLED);
        uint16_t my_mask = partial_on
            ? (uint16_t)((1u << (c * 2)) | (1u << (c * 2 + 1)))
            : (uint16_t)(1u << c);
        uint16_t bm = (uint16_t)
            (oms.portfolio.active_bitmap & my_mask);
        if (bm) {
            Money price_fpn = Money{ money_from_double_payload(price_d) };
            while (bm) {
                int pidx = __builtin_ctz(bm);
                bm &= (uint16_t)(bm - 1);
                Money qty =
                    oms.portfolio.positions[pidx].quantity;
                if (Money_IsZero(qty)) continue;
                // Mark per-slot for v5.13.4 attribution + v5.13.0.B
                // calibration log. Set BEFORE OMS_PushSubmit so the
                // SPSC ring release-acquire makes it visible to drainer
                // when the fill arrives.
                // v5.15.5.C.2 (S3b) — bit-packed in last_exit_predicted_bitmap.
                BITMAP_SET(oms.last_exit_predicted_bitmap, BITMAP_BIT_U16(pidx));
                oms.last_exit_predicted_p[pidx] =
                    state.nodes[c].last_exit_prediction;
                // v5.13.4 — capture chosen arm + regime per-slot
                // for HandleFill's exit_bandit Update.
                // v5.13.6.C — defensive bounds (parity-check
                // M.3 gap-close 2026-05-08). Catches trainer↔
                // engine model dimension mismatch + regime
                // out-of-range at SUBMIT time vs. silently
                // skipping bandit update at attribution time.
                // CRITICAL log + clamp; doesn't refuse submit
                // (exit fires for safety; bandit skips later).
                int captured_arm =
                    state.nodes[c].last_exit_dominant_horizon;
                int captured_regime =
                    state.nodes[c].regime_state.current_regime;
                EnsembleModelZoo<F>* ezoo_b = (EnsembleModelZoo<F>*)
                    state.nodes[c].ensemble_handle;
                int n_arms_b = (ezoo_b
                    ? ezoo_b->exit_predictor_count : 0);
                if (captured_arm < 0 ||
                    captured_arm >= n_arms_b) {
                    static uint64_t s_arm_log_us[16] = {0};
                    Health_LogCriticalRateLimited(
                        &s_arm_log_us[c & 15], 60000000ULL,
                        c, "ml",
                        "exit submit: captured arm %d out of "
                        "range [0, %d) — bandit Update will "
                        "skip; trainer↔engine horizon count "
                        "mismatch?",
                        captured_arm, n_arms_b);
                    captured_arm = -1;  // -1 sentinel; HandleFill skips
                }
                if (captured_regime < 0 ||
                    captured_regime >= NUM_REGIMES) {
                    captured_regime = -1;
                }
                // v5.15.5.C.2.1 (LOW-2) — bit-packed in
                // last_exit_predicted_meta. OMS_META_PACK
                // sets the validity bit (replaces pre-LOW-2
                // -1 sentinel). If EITHER arm or regime is
                // -1 (out-of-range above), clear the slot
                // so drainer's OMS_META_IS_VALID predicate
                // returns false (no bandit Update).
                if (captured_arm >= 0 && captured_regime >= 0) {
                    oms.last_exit_predicted_meta[pidx] =
                        OMS_META_PACK(captured_arm, captured_regime);
                } else {
                    OMS_META_CLEAR(oms.last_exit_predicted_meta[pidx]);
                }
                // v5.15.5.C.4 Phase D5 — Class-18 helper
                // v5.15.5.F.4c.3 WIP2d-1.B.1 — per-core cfg required for Order_BindPreResolved at submit
                tt::OMS_PushExitForSlot(&oms, (int16_t)pidx,
                    qty, state.nodes[c].strategy_id, price_fpn,
                    /*leg*/(uint8_t)0, &cfg.nodes[c]);
            }
            state.nodes[c].strategy_halt_reason =
                SHALT_EXIT_PREDICTED;
        }
    }

    // === Time exit + trailing SL ratchet (per-core) ===
    if (cfg.nodes[c].max_hold_ticks > 0 && price_d > 0.01) {
        EventLoop_TimeExitOneCore(&state, &oms, cfg,
            now_tick, price_d, c);
    }
    // v5.1.1: bracket TIME_EXIT section.
    uint64_t _sec_t_tsl_start = __rdtsc();
    NodeLatencyStats_Sample(
        &state.display_meta[c].slow_path_breakdown[tt::SP_SECTION_TIME_EXIT],
        _sec_t_tsl_start - _sec_t_te_start, _sec_t_tsl_start);

    if (!FPN_IsZero(cfg.nodes[c].sl_trail_mult) &&
        !FPN_IsZero(cfg.nodes[c].tp_hold_score) &&
        !FPN_IsZero(sst->rolling_short.price_stddev) &&
        price_d > 0.01) {
        EventLoop_TrailingSLRatchetOneCore(&state, cfg,
            sst->rolling_short, price_d, c);
    }

    // === v5.15.5.F.4d.1.B.4 D1-B PARITY-032 closure: breakeven-on-profit ===
    // Cached gate bit refreshed at body entry via AUTOPOPULATE_ENGINE_WIDE.
    // Default cfg has MASK_LIFECYCLE_CFG_BREAKEVEN_ON_PROFIT UNSET → cached
    // MASK_BREAKEVEN_ON_PROFIT bit = 0 → branchless BITMAP_IS_SET returns 0
    // → no call (bytewise-identical to pre-`.B.4` per_node_slow behavior).
    // Cohorts with cfg flag SET → cached bit = 1 → fires (5+ year correctness
    // gap closed for per_node_slow arch). Sister consumers in
    // ControllerEventLoop.hpp: the MASK_LAZY_REBUILD_ACTIVE and
    // MASK_WS_FLATTEN_ACTIVE cached-gate reads. D1-B is legitimately NEW
    // slow-path-gate application — the RebuildAllParameters wrapper reads
    // cfg directly, NOT via cache.
    if (BITMAP_IS_SET(state.global_gate_state.flags, MASK_BREAKEVEN_ON_PROFIT)
        && price_d > 0.01) {
        EventLoop_BreakevenOnProfitOneCore(&state, cfg, price_d, c);
    }

    // v5.1.1: bracket TRAIL_SL section (now covers TrailingSLRatchet +
    // BreakevenOnProfit; D1-B sister exit-mechanism shares bracket).
    // Tail-end "OTHER" (warmup permission + post-cycle book-keeping)
    // folds into the next iteration's _sec_t_other_start delta —
    // negligible (<100ns) so we don't add another bracket.
    uint64_t _sec_t_tail = __rdtsc();
    NodeLatencyStats_Sample(
        &state.display_meta[c].slow_path_breakdown[tt::SP_SECTION_TRAIL_SL],
        _sec_t_tail - _sec_t_tsl_start, _sec_t_tail);

    // === Warmup permission grant (per-core check) ===
    uint32_t min_samples = cfg.min_warmup_samples > 0
        ? cfg.min_warmup_samples : 64;
    if (sst->rolling_short.count >= (int)min_samples &&
        state.nodes[c].strategy_id != STRATEGY_NONE) {
        // Original LIVE used producer-thread static `nodes[c]` array
        // address; helper uses the pointer stored on EventLoopState via
        // EventLoopState_RegisterCore (registration sets state.nodes[c].core
        // = address of producer's static array element; identical pointer).
        ExecutionCore_SetPermission(state.nodes[c].core, 1);
    }

    // v4.7.42 (Phase E): close rdtsc bracket + sample.
    uint64_t _sp_t1 = __rdtsc();
    NodeLatencyStats_Sample(&state.display_meta[c].slow_path_latency,
                             _sp_t1 - _sp_t0, _sp_t1);

    // v5.12.1.B clock hoist: reuse rebuild_ts_us captured at
    // slow-path entry instead of taking another system_clock::now()
    // read here. Saves ~50ns/cycle/core. sp_last_tick_us semantic
    // shifts from "wall-clock at end of cycle" to "wall-clock at
    // start of cycle" — sub-100us drift across the slow-path body,
    // irrelevant for both operator liveness display + CheckWsStaleness
    // 60s+ threshold math.
    {
        state.nodes[c].sp_telemetry.last_tick_us.store(rebuild_ts_us,
                                              std::memory_order_relaxed);
        state.nodes[c].sp_telemetry.cycles_total.fetch_add(1,
                                                  std::memory_order_relaxed);
        EventLoop_CheckWsStaleness(&state, cfg, price_d,
                                    rebuild_ts_us);
    }

    // NOTE: DrainPostFill stays on the drainer thread (single
    // writer of last_*_mask is HandleFill on drainer; same
    // thread reads + clears via DrainPostFill wrapper). No
    // need for atomic mask conversion in C.2.
    //
    // NOTE: KillSwitchEvaluate is GLOBAL (account-level
    // drawdown), runs on producer thread in LIVE; backtest
    // calls it from ShardedBacktestDriver scope per
    // Step C.4 N-4 REVERT. Per-core kill switch state is
    // mutated INSIDE RebuildOneCore.
    //
    // NOTE: Drag TP/SL pickup + manual close stay on drainer
    // + producer threads respectively. They submit via
    // OMS_PushSubmit (Phase B) — thread-safe.
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// 4. EngineCommon_SlowPathCycleOneCore
//    const cfg per-core slow-path-cycle body (atomic per-core unit):
//      - EventLoop_UpdateRollingStateOneCore
//      - EventLoop_RebuildOneCore (regime classification + strategy rebuild)
//      - EventLoop_TimeExitOneCore
//      - EventLoop_TrailingSLRatchetOneCore
//      - EventLoop_BreakevenOnProfitOneCore (PARITY-032 fold-in; was MISSING
//        from per_node_slow lambda pre-.B.4)
//      - ML exit-prediction submit (when MASK_ML_CFG_USE_EXIT_MODEL set)
//      - per-core regime collection (state.nodes[c].regime_state populated)
//
//    Live: called per-core from per_node_slow thread (each thread invokes once
//    per slow-path cycle).
//    Backtest: called via SlowPathCycleAllCores wrapper (which loops N times
//    per tick).
//
//    Closes PARITY-031 (per-core regime) + PARITY-032 (breakeven) +
//    auxiliary by-construction.
// Signature (v1.7.3 N-6: 9 args per /trace-deps + /dod-audit + /readiness + /bug-check
// convergent finding — body needs volume + now_tick + depth in addition to v1.7.2 6 args;
// BookSnapshot<F> sister-canonical reuse from DataStream/BinanceDepth.hpp:32-35 per
// feedback_audit_canonical_sister_before_new_infra — REUSE existing canonical instead of
// inventing new DepthBundle struct):
//
// Caller responsibilities (v1.7.3 HIGH-1 mtm_price + N-6 caller-wiring; v1.7.3 self-catch
// removed fabricated `enabled` field — BookSnapshot per DataStream/BinanceDepth.hpp:29-41
// has NO `enabled` field; helper checks MASK_GATE_CFG_DEPTH_ENABLED internally at read
// time per current LIVE pattern :3052-3058):
//   - LIVE per_node_slow lambda: resolve volume from last_volume.load();
//     now_tick from ticks_produced.load(); depth = g_depth_shared.snapshots[
//     __atomic_load_n(&g_depth_shared.active_idx, __ATOMIC_ACQUIRE)] (existing
//     BookSnapshot<F> ref; no new struct construction needed).
//     mtm_price precompute at caller-side per O2 bytewise-identical math discipline.
//   - BACKTEST ShardedBacktest_RunTick: resolve volume from tick.volume; now_tick from
//     (uint64_t)tick_index; depth via BookSnapshot<F> constructed from BACKTEST
//     ShardedBacktestDriver<F> pointer fields (drv->book_imbalance per :102 — NO
//     `current_` prefix; deref *drv->current_spread + *drv->current_mid_price per
//     :119-120 POINTER types; null-check each pointer individually before deref per
//     canonical idiom at :287/:307/:349-350; v1.7.4 NEW-1/2/3/4 path + prefix +
//     pointer-deref corrections landed; spec points to
//     CoreFrameworks/ShardedBacktestDriver.hpp for the source struct).
//   Helper handles depth-disabled flag at read time (BITMAP_IS_SET on cfg.gate_cfg_flags
//   MASK_GATE_CFG_DEPTH_ENABLED) — when disabled, helper substitutes FPN_Zero values
//   regardless of what's in the passed BookSnapshot. Matches current LIVE :3052-3058
//   pattern verbatim per bytewise-identical math discipline.
//======================================================================
// [END_FUNCTION]_[EngineCommon_SlowPathCycleOneCore]
//======================================================================

//======================================================================
// [FUNCTION]_[EngineCommon_SlowPathCycleAllCores]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BACKTEST] [SLOW_PATH]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[BACKTEST fan wrapper — loops SlowPathCycleOneCore over registered nodes once per tick; LIVE never calls it (per-node threads call OneCore directly)]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline void EngineCommon_SlowPathCycleAllCores(const ControllerConfig<F>& cfg,
                                                EventLoopState<F>& state,
                                                OrderManagerState<F>& oms,
                                                Money price,
                                                Money volume,
                                                uint64_t ts_us,
                                                uint64_t now_tick,
                                                const BookSnapshot<F>& depth) {
    // Fan wrapper: BACKTEST calls this ONCE per tick; loops over registered
    // cores firing SlowPathCycleOneCore per-core.
    // LIVE per_node_slow does NOT call this (each per-core thread invokes
    // OneCore directly). Per Option D future-orientation for v6.0 viewer
    // decoupling boundary — viewer reuses this wrapper as natural per-tick
    // mmap-publish API.
    //
    // Loop bound = state.registered_count; preserves num_nodes-clamped
    // iteration semantic at LIVE :622-624 + BACKTEST :223-225 by-construction
    // since EventLoopState_RegisterCore is the single increment site.
    for (int c = 0; c < state.registered_count; ++c) {
        EngineCommon_SlowPathCycleOneCore(cfg, c, state, oms,
                                           price, volume, ts_us, now_tick, depth);
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// 5. EngineCommon_SlowPathCycleAllCores
//    const cfg fan wrapper (~10 LOC for-loop calling SlowPathCycleOneCore N times).
//    Per Option D future-orientation for v6.0 viewer decoupling boundary:
//    viewer reuses this wrapper as the natural per-tick mmap-publish API.
//
//    Backtest calls this ONCE per tick (ShardedBacktest_RunTick).
//    Live does NOT call this (each per_node_slow thread calls OneCore directly).
//
//    Signature (v1.7.3 N-6 consequential: 8 args; pass-through to OneCore expanded args):
//======================================================================
// [END_FUNCTION]_[EngineCommon_SlowPathCycleAllCores]
//======================================================================

//======================================================================
// [FUNCTION]_[EngineCommon_DrainPostFill]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BACKTEST] [OMS_DRAINER] [CAPITAL_BEARING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the ONE cfg→args binder for post-fill drain — LIVE drainer + BACKTEST driver both call this; fans EventLoop_DrainPostFill with per-node cfg threading]
// [REFERENCE]_[CLASS]_[27]
// [REFERENCE]_[DESIGN_SPEC]_[[decision-time-data-binding-pattern.md] [train-serve-execution-layer-parity.md]]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline void EngineCommon_DrainPostFill(EventLoopState<F>& state,
                                        OrderManagerState<F>& oms,
                                        const ControllerConfig<F>& cfg) {
    EventLoop_DrainPostFill(&state, &oms, cfg.sl_cooldown_cycles,
                             cfg.ensemble_trade_reward_mult,
                             cfg.confidence_ic_floor,
                             cfg.confidence_ic_floor_window,
                             cfg.auto_kill_on_drift,
                             cfg.confidence_ic_variant,
                             &cfg);
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// E.1.2.C leg 0 (2026-08-20) — replaces EngineSharded_SlowPath_DrainPostFill
// (deleted from EngineSharded/SlowPath.hpp) and the backtest driver's two
// hand-rolled 4-arg calls: ONE binder shared by construction (M5 execution-
// layer parity), so live and backtest can never diverge on which cfg facts
// reach the drain again.
//
// History this closes: v5.14.1.F inserted confidence_ic_variant mid-signature
// in OneCore and the fan was never updated — every production call shifted
// its tail bindings one slot (the exit-bandit enable flag selected the IC
// variant; the fee literal truncated into the enable = 0), so the exit-bandit
// reward update was dead everywhere and enabling it in live would have
// poisoned drift-IC. The fan + OneCore are now FULLY de-defaulted (arity =
// compile-time guard) and the per-node facts (exit-bandit enable, counter-
// factual taker fee) derive from the threaded per-node cfg slice at point of
// use — which also COMPLETES the WIP2d-1.B.1 decision that deleted the fee
// ARG from the live call but left the parameter chain in both signatures.
//
// Backtest note: drift-floor/window/auto-kill + ic_variant now forward in
// backtest too (previously silently defaulted). At default cfg (floor=0)
// this is byte-inert; an operator who sets a drift floor in backtest.cfg
// gets LIVE-parity drift behavior in replay — parity by construction.
//======================================================================
// [END_FUNCTION]_[EngineCommon_DrainPostFill]
//======================================================================

}  // namespace tt
