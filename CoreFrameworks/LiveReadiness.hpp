// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [LIVE-READINESS BOOT GATE — v5.15.2]
//======================================================================================================
// FOREACH_LIVE_READINESS_CHECK X-macro registry + LiveReadiness_Verify
// boot-time pre-flight gate. When `cfg.trading_mode == TRADING_MODE_LIVE`,
// the gate REFUSES boot if any S_REFUSE-severity check fails; WARN-only
// for S_WARN-severity. When trading_mode is PAPER or SHADOW, all failures
// log as WARN — engine proceeds. Visibility-by-default for operators
// staging toward live deployment.
//
// Pattern: curve-registry-pattern.md (X-macro + fn-pointer dispatch).
// Same shape as FOREACH_DEGRADATION_CURVE (v5.14.9.A) +
// FOREACH_BANDIT_ALGORITHM (v5.14.10.A). Boot-only path; ~10us total for
// 9 checks; well below operator-perceptible threshold.
//
// Adding a new pre-flight check (1 row):
//   1. Add X(name, fn_ptr, severity, fix_hint) to FOREACH_LIVE_READINESS_CHECK
//   2. Define `inline bool fn_ptr<F>(const ControllerConfig<F>&, const EventLoopState<F>&)`
//   3. Tests for the check's pass / fail behavior
//
// FUTURE LEVERAGE: v6.0 headless service can introspect this registry to
// publish "what does the boot gate check" via REST/JSON endpoint without
// dispatch overhead (compile-time table; constexpr-iterable).
//
// CROSS-REFERENCES:
//   - CLAUDE.md item 9 (single chokepoint per concern — aggregate_zoo_drift)
//   - CLAUDE.md item 13 (X-macro registry for multi-site additions)
//   - CLAUDE.md item 19 (structural fix when bug class can recur)
//   - CLAUDE.md item 20 (BITMAP_* primitive for drift bit checks)
//   - DESIGN_SPECS/curve-registry-pattern.md (fn-pointer dispatch table)
//   - v5.14.4 reconcile_mode (mode-discriminator cfg field precedent)
//   - v5.15.1 FOREACH_FAILURE_MODE drift bits + drift_flags_at_load on ModelHandle
//======================================================================================================
#ifndef LIVE_READINESS_HPP
#define LIVE_READINESS_HPP

#include <stdio.h>
#include <stdint.h>
#include "../MemHeaders/BitmapMacros.hpp"
#include "../MemHeaders/FailureModeRegistry.hpp"
#include "ControllerConfig.hpp"
#include "ControllerEventLoop.hpp"      // EventLoopState
#include "../ML_Headers/CoreModelZoo.hpp"  // CoreModelZoo + ModelHandle.drift_flags_at_load
#include "../Strategies/StrategyInterface.hpp"  // STRATEGY_ML

namespace tt {

//======================================================================================================
// [SEVERITY TOKENS]
//======================================================================================================

enum LiveReadinessSeverity : uint8_t {
    LR_SEV_REFUSE = 0,   // live mode: blocks boot. paper/shadow: WARN.
    LR_SEV_WARN   = 1,   // live mode: WARN-only. paper/shadow: WARN.
};

//======================================================================================================
// [DRIFT AGGREGATION HELPER — single chokepoint per CLAUDE.md item 9]
//======================================================================================================
// OR-aggregates drift_flags_at_load across all 4 zoo roles. Boot-gate
// helpers use this to query drift state from the source-of-truth
// (handle) rather than from PerCoreSnap.failure_flags (which isn't
// populated until snapshot publish — i.e., AFTER pthread spawns; boot
// gate runs BEFORE).

template <unsigned F>
inline uint16_t aggregate_zoo_drift(const CoreModelZoo<F>* zoo) {
    if (!zoo) return 0;
    return (uint16_t)(zoo->buy_signal.drift_flags_at_load |
                      zoo->barrier.drift_flags_at_load    |
                      zoo->regime.drift_flags_at_load     |
                      zoo->exit.drift_flags_at_load);
}

//======================================================================================================
// [CHECK HELPERS — one template fn per registry entry]
//======================================================================================================

template <unsigned F>
inline bool check_secret_nonempty(const ControllerConfig<F>& cfg,
                                  const EventLoopState<F>&) {
    return cfg.held_out_stamp_secret[0] != '\0';
}

template <unsigned F>
inline bool check_mlockall_required(const ControllerConfig<F>& cfg,
                                    const EventLoopState<F>&) {
    // If require_mlockall=1 and mlockall failed, main.cpp:209 returns 1
    // before reaching EngineSharded_Run. So under LIVE mode we just
    // verify the operator set the flag (engine wouldn't be here if
    // require_mlockall=1 AND mlockall failed).
    return cfg.require_mlockall != 0;
}

template <unsigned F>
inline bool check_all_cores_strategy_explicit(const ControllerConfig<F>& cfg,
                                              const EventLoopState<F>&) {
    // Branchless mask compare: all bits 0..num_execution_cores-1 must be set.
    // Reuses cfg.core_strategies_explicit_set bitmap (v5.9.0c precedent).
    if (cfg.num_execution_cores <= 0 || cfg.num_execution_cores > 16) return false;
    uint16_t expected = (uint16_t)((1u << cfg.num_execution_cores) - 1u);
    return (cfg.core_strategies_explicit_set & expected) == expected;
}

template <unsigned F>
inline bool check_all_ml_cores_have_model(const ControllerConfig<F>& cfg,
                                          const EventLoopState<F>& state) {
    for (uint16_t i = 0; i < cfg.num_execution_cores && i < 16; ++i) {
        if (cfg.core_strategies[i] == STRATEGY_ML &&
            state.cores[i].model_handle == nullptr) {
            return false;
        }
    }
    return true;
}

template <unsigned F>
inline bool check_model_max_age_set(const ControllerConfig<F>& cfg,
                                    const EventLoopState<F>& state) {
    // (a) operator opted in to age gating
    if (cfg.model_max_age_hours == 0) return false;
    // (b) no core has MODEL_AGE_WARN drift bit set
    for (uint16_t i = 0; i < cfg.num_execution_cores && i < 16; ++i) {
        const CoreModelZoo<F>* zoo = (const CoreModelZoo<F>*)state.cores[i].model_handle;
        if (BITMAP_IS_SET(aggregate_zoo_drift(zoo), FAILURE_MASK_model_age_warn)) {
            return false;
        }
    }
    return true;
}

template <unsigned F>
inline bool check_no_feature_hash_drift(const ControllerConfig<F>& cfg,
                                        const EventLoopState<F>& state) {
    for (uint16_t i = 0; i < cfg.num_execution_cores && i < 16; ++i) {
        const CoreModelZoo<F>* zoo = (const CoreModelZoo<F>*)state.cores[i].model_handle;
        if (BITMAP_IS_SET(aggregate_zoo_drift(zoo), FAILURE_MASK_feature_hash_drift)) {
            return false;
        }
    }
    return true;
}

template <unsigned F>
inline bool check_no_label_hash_drift(const ControllerConfig<F>& cfg,
                                      const EventLoopState<F>& state) {
    for (uint16_t i = 0; i < cfg.num_execution_cores && i < 16; ++i) {
        const CoreModelZoo<F>* zoo = (const CoreModelZoo<F>*)state.cores[i].model_handle;
        if (BITMAP_IS_SET(aggregate_zoo_drift(zoo), FAILURE_MASK_label_hash_drift)) {
            return false;
        }
    }
    return true;
}

template <unsigned F>
inline bool check_no_build_flags_drift(const ControllerConfig<F>& cfg,
                                       const EventLoopState<F>& state) {
    for (uint16_t i = 0; i < cfg.num_execution_cores && i < 16; ++i) {
        const CoreModelZoo<F>* zoo = (const CoreModelZoo<F>*)state.cores[i].model_handle;
        if (BITMAP_IS_SET(aggregate_zoo_drift(zoo), FAILURE_MASK_build_flags_drift)) {
            return false;
        }
    }
    return true;
}

template <unsigned F>
inline bool check_all_stamps_hmac_verified(const ControllerConfig<F>& cfg,
                                           const EventLoopState<F>& state) {
    for (uint16_t i = 0; i < cfg.num_execution_cores && i < 16; ++i) {
        const CoreModelZoo<F>* zoo = (const CoreModelZoo<F>*)state.cores[i].model_handle;
        if (BITMAP_IS_SET(aggregate_zoo_drift(zoo), FAILURE_MASK_stamp_hmac_not_verified)) {
            return false;
        }
    }
    return true;
}

//======================================================================================================
// [.E.0.10 Phase-D — BLANKET live-capital boot gate (D-77/F-2 + D-168)]
//======================================================================================================
// Live trading is gated behind the WHOLE .E-series live-readiness rework (per-node aggregator +
// reconciliation + the cross-thread torn-read closure; the sprint end-goal). Until .E lands, live
// capital is REFUSED at boot — fail-safe: no accidental live trading on the pre-.E engine. Routes
// through the single capital-authority predicate (NEW-1 / RBP Class 47), so it inherits the .E.1
// per-cluster relocation (H22) with zero edits here.
//
// >>> H21 TOMBSTONE: REMOVE this fn + its FOREACH_LIVE_READINESS_CHECK row at .E / v5.16, when the
//     live-readiness rework actually lands. Do NOT silently leave it — it would block the intended
//     go-live. Tracked: TECH_DEBT-203 (removal) + the .E.1-foundation live-readiness completion. <<<
template <unsigned F>
inline bool check_live_capital_gated_until_e(const ControllerConfig<F>& cfg,
                                             const EventLoopState<F>&) {
    return !ControllerConfig_IsLiveCapital(cfg);  // PASS unless live capital is requested -> REFUSE in live
}

//======================================================================================================
// [REGISTRY DEFINITION]
//======================================================================================================
// Tuple: X(name, fn_ptr, severity, fix_hint)
//   name      — diagnostic label (used in stderr WARN/REFUSE messages).
//   fn_ptr    — template fn returning bool (true = check PASSED).
//   severity  — LR_SEV_REFUSE (blocks live boot) or LR_SEV_WARN (logs only).
//   fix_hint  — operator-actionable guidance string (~80 chars).

#define FOREACH_LIVE_READINESS_CHECK(X) \
    X(live_capital_gated_until_e,  check_live_capital_gated_until_e,  LR_SEV_REFUSE, \
      "live capital is gated behind the .E-series live-readiness rework (per-node aggregator / reconciliation / torn-read closure); run trading_mode=paper or shadow. REMOVED at v5.16 when .E lands (H21 tombstone; TECH_DEBT-203).") \
    X(secret_nonempty,             check_secret_nonempty,             LR_SEV_REFUSE, \
      "set held_out_stamp_secret in cfg (HMAC verification)") \
    X(mlockall_required,           check_mlockall_required,           LR_SEV_REFUSE, \
      "set require_mlockall=1 in cfg (deterministic latency)") \
    X(all_cores_strategy_explicit, check_all_cores_strategy_explicit, LR_SEV_REFUSE, \
      "set core_<N>_strategy explicitly for all N in [0, num_execution_cores)") \
    X(all_ml_cores_have_model,     check_all_ml_cores_have_model,     LR_SEV_REFUSE, \
      "set core_<N>_model_path or core_<N>_model_dir for all ML cores") \
    X(model_max_age_set,           check_model_max_age_set,           LR_SEV_REFUSE, \
      "set model_max_age_hours > 0; retrain stale models") \
    X(no_feature_hash_drift,       check_no_feature_hash_drift,       LR_SEV_REFUSE, \
      "retrain with current FEATURE_REGISTRY_HASH") \
    X(no_label_hash_drift,         check_no_label_hash_drift,         LR_SEV_REFUSE, \
      "retrain with current LABEL_REGISTRY_HASH") \
    X(no_build_flags_drift,        check_no_build_flags_drift,        LR_SEV_WARN, \
      "rebuild engine with same flags as training, OR accept divergence") \
    X(all_stamps_hmac_verified,    check_all_stamps_hmac_verified,    LR_SEV_REFUSE, \
      "set held_out_stamp_secret + model_verify_strict=1")

#define FOREACH_LIVE_READINESS_CHECK_COUNT_ONE(name, fn_ptr, sev, hint) +1
#define FOREACH_LIVE_READINESS_CHECK_COUNT \
    (0 FOREACH_LIVE_READINESS_CHECK(FOREACH_LIVE_READINESS_CHECK_COUNT_ONE))

//======================================================================================================
// [LIVE-READINESS VERIFY — boot-time pre-flight gate]
//======================================================================================================
// Returns: 0 on PASS (no failures); -1 on REFUSE (live + any REFUSE-sev
// failed); 1 on WARN-only (paper/shadow, OR live + only WARN-sev failed).
//
// X-macro walks the registry inline; each entry's fn_ptr called once;
// log + tally per severity; final summary log. Compile-time table; no
// runtime dispatch overhead.

template <unsigned F>
inline int LiveReadiness_Verify(const ControllerConfig<F>& cfg,
                                const EventLoopState<F>& state) {
    const bool live = ControllerConfig_IsLiveCapital(cfg);  // NEW-1 single-authority predicate (RBP Class 47; the .E.1 per-cluster relocation seam, H22). Identical to trading_mode==LIVE today; routes through the one authority.
    const char* mode_str = live ? "live" :
                           (cfg.trading_mode == TRADING_MODE_SHADOW ? "shadow" : "paper");
    int refused = 0, warned = 0;

    fprintf(stderr,
        "[live_readiness] mode=%s; checking %d pre-flight items\n",
        mode_str, FOREACH_LIVE_READINESS_CHECK_COUNT);

    #define X(name_id, fn_ptr, severity, fix_hint)                                              \
        if (!fn_ptr<F>(cfg, state)) {                                                            \
            if (live && (severity) == LR_SEV_REFUSE) {                                           \
                fprintf(stderr,                                                                  \
                    "[live_readiness] LIVE REFUSE: %s failed. Fix: %s\n",                        \
                    #name_id, fix_hint);                                                         \
                refused++;                                                                       \
            } else {                                                                             \
                fprintf(stderr,                                                                  \
                    "[live_readiness] %s mode WARN: %s failed. Fix: %s\n",                       \
                    mode_str, #name_id, fix_hint);                                               \
                if (live) warned++;                                                              \
            }                                                                                    \
        }
    FOREACH_LIVE_READINESS_CHECK(X)
    #undef X

    if (live && refused > 0) {
        fprintf(stderr,
            "[live_readiness] LIVE REFUSED: %d critical pre-flight item(s) failed; "
            "engine will NOT start. Fix the above + retry.\n", refused);
        return -1;
    }
    if (live && warned > 0) {
        fprintf(stderr,
            "[live_readiness] live mode: %d WARN(s); engine proceeding\n", warned);
        return 1;
    }
    if (refused == 0 && warned == 0) {
        fprintf(stderr, "[live_readiness] all pre-flight checks PASSED\n");
    }
    return 0;
}

}  // namespace tt

#endif  // LIVE_READINESS_HPP
