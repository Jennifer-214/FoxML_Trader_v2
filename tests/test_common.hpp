// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [test_common.hpp — shared test infrastructure]
//======================================================================================================
// Extracted at v5.15.5.F.4d.1.B.5 (controller_test.cpp domain split).
//
// Shared infrastructure for the multi-binary test framework:
//   - Standard + engine includes (shared across all test sub-files)
//   - inline int test counters (C++17 inline-variable; single storage across TUs per program)
//   - inline check() assertion macro
//   - inline test_warmup_ctrl() helper (used by warmup-dependent tests)
//   - constexpr FP = 64 (fixed-point template arg)
//   - compile-time static_assert sanity checks (shared across all sub-files)
//   - wire_format_invariants.hpp inclusion (depends on check() defn)
//
// **C++17 inline-variable discipline (Decision C of .B.5 plan body):**
// `inline int tests_failed = 0;` gives single shared storage across all TUs that #include
// this header within the same program/binary. CRITICAL: do NOT refactor back to `static int` —
// `static int` in a header gives each TU its own private copy, silently breaking shared counter
// across sub-binary aggregation in the umbrella binary.
//
// Per-binary semantics:
//   - Each independent sub-binary (./build/controller_test_engine_boot, etc.) has its own
//     copy of `inline int tests_failed` (each is its own program).
//   - The umbrella binary (./build/controller_test) links all 9 sub-domain object files
//     together; linker dedups `inline int` into ONE storage instance shared across all
//     RunTests_X() calls.
//
// See sister memory `feedback_cpp17_inline_variable_for_shared_state_across_tus.md`
// (queued for .B.5 ship close) for full discipline.
//======================================================================================================

#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/resource.h>  // v5.11.0.B — getrlimit / RLIMIT_MEMLOCK
#include <unistd.h>
#include <limits>          // v5.9.0: std::numeric_limits<double>::quiet_NaN() in NaN guard tests
#include <thread>          // v5.11.3.B: std::thread for seqlock tear-free regression test
#include <atomic>          // v5.11.3.B: std::atomic for cross-thread coordination in tests
#include <locale.h>        // v5.11.4.A: setlocale for locale-immunity test
#include <type_traits>     // v5.14.8.A.0.b — std::is_array_v / std::extent_v for char-array dispatch
#include <sys/mman.h>      // v5.11.22 — MAP_HUGETLB constant

#include "../DataStream/MockGenerator.hpp"
#include "../CoreFrameworks/PortfolioController.hpp"
#include "../CoreFrameworks/Order.hpp"
#include "../CoreFrameworks/OrderManager.hpp"
#include "../CoreFrameworks/ControllerEventLoop.hpp"        // Phase 2.1 tests
#include "../CoreFrameworks/SystemInit.hpp"                  // v5.11.0.A — engine_set_mxcsr_ftz_daz
#include "../CoreFrameworks/ExecutionCore.hpp"              // Phase 2.1 tests
#include "../CoreFrameworks/ShardedSnapshotPersist.hpp"     // Phase 4 tests
#include "../CoreFrameworks/ShardedBacktestDriver.hpp"      // Track E.1 tests
#include "../CoreFrameworks/EngineCommon.hpp"                // v5.15.5.F.4d.1.B.4 — shared train-serve helpers
#include "../DataStream/EngineTUI.hpp"                       // v5.0.4 — topology populator tests
#include "../CoreFrameworks/Reconcile.hpp"                   // v5.2.1 — live reconciliation tests
#include "../ML_Headers/CoreModelZoo.hpp"                    // Track E.2 tests
#include "../ML_Headers/ThompsonBandit.hpp"                  // v5.14.10.A — Bayesian Thompson sampling bandit
#include "../ML_Headers/BanditAlgorithmRegistry.hpp"         // v5.14.10.A — FOREACH_BANDIT_ALGORITHM dispatch registry
#include "../CoreFrameworks/EnsembleHotSwap.hpp"              // v5.14.2 — EngineSharded_HotSwapEnsemble template
#include "../ML_Headers/FeatureRegistry.hpp"                  // v5.8.1a tests
#include "../Backtest/PhaseTimers.hpp"                        // v5.10.0 Item A — phase timer tests
#include "../MemHeaders/BuddyAllocator.hpp"                   // v5.11.13 — typo fix + O(1) order lookup tests
#include "../MemHeaders/InitArena.hpp"                        // v5.11.22 — MAP_HUGETLB opt-in tests
#include "../MemHeaders/CoreCtxSummaryFieldRegistry.hpp"     // v5.15.5.C.3 Phase 4 — FOREACH_CORE_CTX_SUMMARY_FIELD + JSON emit
#include "../CoreFrameworks/PaperResetArchive.hpp"            // v5.15.5.C.3 Phase 6 — paper-reset archive helpers
#include "../MemHeaders/LatencyHistogram.hpp"                 // v5.15.5.C.3 Phase 7.A — LatencyHistogram primitive
#include "../DataStream/DepthReplayState.hpp"                // Track E.3 tests
#include "../ML_Headers/FlowFeatures.hpp"                    // v4.5 Wave 1 tests
#include "../DataStream/BinanceUserData.hpp"
#include "../Backtest/BacktestEngine.hpp"
#include "../Backtest/BacktestSharded.hpp"                    // v5.9.2 — parity test calls Backtest_Run end-to-end
#include "../Backtest/HeldOutSplit.hpp"
#include "../MemHeaders/HmacSha256.hpp"                       // v5.3.0 Phase B — in-process HMAC primitive
#include "../MemHeaders/RunHistory.hpp"                       // v5.3.2 Phase C — JSONL append-only run history
#include "../MemHeaders/HealthLog.hpp"                        // v5.4.0 Phase 0.1 — structured operational diagnostic log
#include "../MemHeaders/BitmapMacros.hpp"                     // v5.14.8.A.0.b — reusable BITMAP_* API
#include "../MemHeaders/FailureModeRegistry.hpp"              // v5.14.8.B — FOREACH_FAILURE_MODE pseudo-registry
#include "../MemHeaders/PerCoreStateFlagsRegistry.hpp"        // v5.14.9.B.2 — FOREACH_PER_CORE_STATE_FLAG
#include "../MemHeaders/ArchFieldDriftRegistry.hpp"           // v5.15.1 — FOREACH_ARCH_FIELD_DRIFT
#include "../CoreFrameworks/LiveReadiness.hpp"                // v5.15.2 — FOREACH_LIVE_READINESS_CHECK + helpers
#include "../ML_Headers/StampBoundModelConstRegistry.hpp"     // v5.14.8.A.0.b — registry tests + presence column dispatch
#include "../ML_Headers/StampHelper.hpp"                      // v5.15.3.A — StampArgs<F> + Stamp_AssembleAndEmit
#include "../CoreFrameworks/HotSwap.hpp"                      // v5.15.4 — HotSwap_ShadowLoad_{Ensemble,SingleZoo}
#include "../CoreFrameworks/CfgFieldRegistry.hpp"             // v5.15.5.F.4b — universal cfg field registry
#include "../CoreFrameworks/CfgFieldDispatch.hpp"             // v5.15.5.F.4b — tt:: type-trait dispatch
#include "../CoreFrameworks/StampBoundDerivedFilter.hpp"      // v5.15.5.F.4d.1.A — Path γ first canonical consumer
#include "../MemHeaders/CfgGateRegistry.hpp"                  // v5.15.5.F.4d.1.B.1 — derived-filter consumer macros
#include "../Strategies/StrategyLifecycle.hpp"                // v5.4.0 Phase 1.2 — Strategy_InitPerCore / FreePerCore

using namespace std;

//======================================================================================================
// [Shared test counters — C++17 inline-variable; single storage across TUs per binary]
//======================================================================================================
inline int tests_passed = 0;
inline int tests_failed = 0;

inline void check(const char *name, int condition) {
    if (condition) {
        printf("  [PASS] %s\n", name);
        tests_passed++;
    } else {
        printf("  [FAIL] %s\n", name);
        tests_failed++;
    }
}

// helper: run warmup to completion, auto-computes tick count from config.
// Handles both warmup_ticks and min_warmup_samples gates.
inline void test_warmup_ctrl(PortfolioController<64> *ctrl, OrderPool<64> *pool,
                              TradeLog *log, double base_price, double base_vol) {
    int ticks = (int)ctrl->config.warmup_ticks;
    int for_samples = (int)ctrl->config.min_warmup_samples * (int)ctrl->config.poll_interval;
    if (for_samples > ticks) ticks = for_samples;
    ticks += 5; // margin
    for (int i = 0; i < ticks; i++) {
        PortfolioController_Tick(ctrl, pool,
            FPN_FromDouble<64>(base_price + (i % 10) * 0.3),
            FPN_FromDouble<64>(base_vol), log);
    }
}

//======================================================================================================
// [Shared fixed-point template arg]
//======================================================================================================
constexpr unsigned FP = 64;

//======================================================================================================
// [v5.15.5.F.4b] Shared compile-time static_asserts
//======================================================================================================
// is_FPN_v + FPN<F>::F member exposure work correctly.
// CfgFieldRegistry sanity asserts (v5.15.5.F.4c.3 — two-registry architecture).
// See DESIGN_SPECS/framework-patterns/type-trait-dispatch-via-tt-namespace.md +
// DOCS/recurring-bug-patterns/class-23-*.md.
//======================================================================================================
static_assert(is_FPN_v<FPN<64>>,                "v5.15.5.F.4b: is_FPN_v should detect FPN<64>");
static_assert(is_FPN_v<FPN<128>>,               "v5.15.5.F.4b: is_FPN_v should detect FPN<128>");
static_assert(!is_FPN_v<double>,                "v5.15.5.F.4b: is_FPN_v should reject double");
static_assert(!is_FPN_v<int>,                   "v5.15.5.F.4b: is_FPN_v should reject int");
static_assert(!is_FPN_v<uint64_t>,              "v5.15.5.F.4b: is_FPN_v should reject uint64_t");
static_assert(FPN<64>::F == 64,                 "v5.15.5.F.4b: FPN<64>::F should expose template param value (64)");
static_assert(FPN<128>::F == 128,               "v5.15.5.F.4b: FPN<128>::F should expose template param value (128)");

static_assert(sizeof(CfgFieldDescriptor) <= 128,
              "v5.15.5.F.4b: CfgFieldDescriptor must fit two cache lines");
static_assert(FIELD_IDX_GLOBAL_END > 0,
              "v5.15.5.F.4c.3: FOREACH_GLOBAL_CFG_FIELD must have at least one entry");
static_assert(FIELD_IDX_PER_CORE_END > 0,
              "v5.15.5.F.4c.3: FOREACH_PER_CORE_CFG_FIELD must have at least one entry");
static_assert(FIELD_IDX_GLOBAL_END == sizeof(g_global_cfg_field_descriptors) / sizeof(g_global_cfg_field_descriptors[0]),
              "v5.15.5.F.4c.3: g_global_cfg_field_descriptors size must match FIELD_IDX_GLOBAL_END");
static_assert(FIELD_IDX_PER_CORE_END == sizeof(g_per_core_cfg_field_descriptors) / sizeof(g_per_core_cfg_field_descriptors[0]),
              "v5.15.5.F.4c.3: g_per_core_cfg_field_descriptors size must match FIELD_IDX_PER_CORE_END");
static_assert(STRAT_CAT_USES_FLOW_DATA < (1ull << 32),
              "v5.15.5.F.4b: StrategyCategory bitmap overflow guard");
static_assert(OP_MODE_CAT_OFFLINE < (1u << 16),
              "v5.15.5.F.4b: OpModeCategory bitmap overflow guard");
static_assert(CfgFieldDescriptor::LOG_VALUE_FORBIDDEN < (1u << 16),
              "v5.15.5.F.4b: MetadataFlag bitmap overflow guard");

//======================================================================================================
// [wire_format_invariants.hpp — reusable I1-I5 structural invariants helper]
//======================================================================================================
// Included AFTER check() definition since its inline body calls check().
// v5.15.5.F.4d.1.A first canonical consumer of FOREACH_METADATA_BIT.
//======================================================================================================
#include "wire_format_invariants.hpp"
