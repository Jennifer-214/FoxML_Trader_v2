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
#include "ControllerEventLoop.hpp"               // EventLoopState<F>, EventLoopState_Init, EventLoopState_ConfigureKillSwitch
#include "OrderManager.hpp"                      // OrderManagerState<F>
#include "../FixedPoint/FixedPointN.hpp"         // FPN<F>, FPN_Mul, FPN_FromDouble, FPN_Zero
#include "../Strategies/RegimeDetector.hpp"      // Regime_Init

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
template <int F>
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
template <int F>
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
//      - per-core OrderManager_RegisterCore
//      - ConfidenceScorer_Init + BindCompositeCfg
//      - RollingTurnover_Init
//      - per-core model load (when cfg.cores[c].strategy is ML)
//      - Strategy_InitPerCore (closes PARITY-029 — pre-v5.4 F7 bug)
//
//    Called N times per boot (per-core loop). External wrappers (e.g.,
//    bandit_state_prior_path) called AFTER this returns.
//
//    Closes PARITY-027 (exit-model bind) + PARITY-028 (BindCompositeCfg) +
//    PARITY-029 (Strategy_InitPerCore) by-construction.
template <int F>
void EngineCommon_BootPerCore(const ControllerConfig<F>& cfg,
                               int c,
                               EventLoopState<F>& state,
                               OrderManagerState<F>& oms);

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
template <int F>
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
template <int F>
void EngineCommon_SlowPathCycleAllCores(const ControllerConfig<F>& cfg,
                                         EventLoopState<F>& state,
                                         OrderManagerState<F>& oms,
                                         FPN<F> price,
                                         uint64_t ts_us);

}  // namespace tt
