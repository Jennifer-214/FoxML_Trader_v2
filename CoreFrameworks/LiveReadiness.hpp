// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[CoreFrameworks/LiveReadiness.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING] [BOOT_TIME] [CAPITAL_BEARING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the live-readiness boot gate (v5.15.2) — registry-driven pre-flight checks; live REFUSES on failure, paper WARNs]
// [CONTAINS]
//   - [ENUM]_[LiveReadinessSeverity]
//   - [FUNCTION]_[aggregate_zoo_drift]
//   - [FUNCTION]_[check_live_capital_gated_until_e]
//   - [REGISTRY]_[FOREACH_LIVE_READINESS_CHECK]
//   - [FUNCTION]_[LiveReadiness_Verify]
//======================================================================================================
#ifndef LIVE_READINESS_HPP
#define LIVE_READINESS_HPP

#include <stdio.h>
#include <stdint.h>
#include "../MemHeaders/BitmapMacros.hpp"
#include "../MemHeaders/FailureModeRegistry.hpp"
#include "ControllerConfig.hpp"
#include "ControllerEventLoop.hpp"      // EventLoopState
#include "../ML_Headers/NodeModelZoo.hpp"  // NodeModelZoo + ModelHandle.drift_flags_at_load
#include "../Strategies/StrategyInterface.hpp"  // STRATEGY_ML

namespace tt {

//======================================================================
// [ENUM]_[LiveReadinessSeverity]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[per-check severity — REFUSE blocks a live boot; WARN logs everywhere]
//======================================================================
// [CODE]
//======================================================================
enum LiveReadinessSeverity : uint8_t {
    LR_SEV_REFUSE = 0,   // live mode: blocks boot. paper/shadow: WARN.
    LR_SEV_WARN   = 1,   // live mode: WARN-only. paper/shadow: WARN.
};
//======================================================================
// [END_CODE]
//======================================================================
// [END_ENUM]_[LiveReadinessSeverity]
//======================================================================

//======================================================================
// [FUNCTION]_[aggregate_zoo_drift]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML] [HELPER]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the ONE drift chokepoint — OR of drift_flags_at_load across all 4 zoo roles, read from the handle SSoT]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline uint16_t aggregate_zoo_drift(const NodeModelZoo<F>* zoo) {
    if (!zoo) return 0;
    return (uint16_t)(zoo->buy_signal.drift_flags_at_load |
                      zoo->barrier.drift_flags_at_load    |
                      zoo->regime.drift_flags_at_load     |
                      zoo->exit.drift_flags_at_load);
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// OR-aggregates drift_flags_at_load across all 4 zoo roles. Boot-gate
// helpers use this to query drift state from the source-of-truth
// (handle) rather than from PerNodeSnap.failure_flags (which isn't
// populated until snapshot publish — i.e., AFTER pthread spawns; boot
// gate runs BEFORE).
//======================================================================
// [END_FUNCTION]_[aggregate_zoo_drift]
//======================================================================

//------------------------------------------------------------------------------------------------------
// [SECTION]_[check helpers — one template fn per registry entry]
//------------------------------------------------------------------------------------------------------

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
    // Branchless mask compare: all bits 0..num_execution_nodes-1 must be set.
    // Reuses cfg.node_strategies_explicit_set bitmap (v5.9.0c precedent).
    if (cfg.num_execution_nodes <= 0 || cfg.num_execution_nodes > 16) return false;
    uint16_t expected = (uint16_t)((1u << cfg.num_execution_nodes) - 1u);
    return (cfg.node_strategies_explicit_set & expected) == expected;
}

template <unsigned F>
inline bool check_all_ml_cores_have_model(const ControllerConfig<F>& cfg,
                                          const EventLoopState<F>& state) {
    for (uint16_t i = 0; i < cfg.num_execution_nodes && i < 16; ++i) {
        if (cfg.node_strategies[i] == STRATEGY_ML &&
            state.nodes[i].model_handle == nullptr) {
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
    for (uint16_t i = 0; i < cfg.num_execution_nodes && i < 16; ++i) {
        const NodeModelZoo<F>* zoo = (const NodeModelZoo<F>*)state.nodes[i].model_handle;
        if (BITMAP_IS_SET(aggregate_zoo_drift(zoo), FAILURE_MASK_model_age_warn)) {
            return false;
        }
    }
    return true;
}

template <unsigned F>
inline bool check_no_feature_hash_drift(const ControllerConfig<F>& cfg,
                                        const EventLoopState<F>& state) {
    for (uint16_t i = 0; i < cfg.num_execution_nodes && i < 16; ++i) {
        const NodeModelZoo<F>* zoo = (const NodeModelZoo<F>*)state.nodes[i].model_handle;
        if (BITMAP_IS_SET(aggregate_zoo_drift(zoo), FAILURE_MASK_feature_hash_drift)) {
            return false;
        }
    }
    return true;
}

template <unsigned F>
inline bool check_no_label_hash_drift(const ControllerConfig<F>& cfg,
                                      const EventLoopState<F>& state) {
    for (uint16_t i = 0; i < cfg.num_execution_nodes && i < 16; ++i) {
        const NodeModelZoo<F>* zoo = (const NodeModelZoo<F>*)state.nodes[i].model_handle;
        if (BITMAP_IS_SET(aggregate_zoo_drift(zoo), FAILURE_MASK_label_hash_drift)) {
            return false;
        }
    }
    return true;
}

template <unsigned F>
inline bool check_no_build_flags_drift(const ControllerConfig<F>& cfg,
                                       const EventLoopState<F>& state) {
    for (uint16_t i = 0; i < cfg.num_execution_nodes && i < 16; ++i) {
        const NodeModelZoo<F>* zoo = (const NodeModelZoo<F>*)state.nodes[i].model_handle;
        if (BITMAP_IS_SET(aggregate_zoo_drift(zoo), FAILURE_MASK_build_flags_drift)) {
            return false;
        }
    }
    return true;
}

template <unsigned F>
inline bool check_all_stamps_hmac_verified(const ControllerConfig<F>& cfg,
                                           const EventLoopState<F>& state) {
    for (uint16_t i = 0; i < cfg.num_execution_nodes && i < 16; ++i) {
        const NodeModelZoo<F>* zoo = (const NodeModelZoo<F>*)state.nodes[i].model_handle;
        if (BITMAP_IS_SET(aggregate_zoo_drift(zoo), FAILURE_MASK_stamp_hmac_not_verified)) {
            return false;
        }
    }
    return true;
}

//======================================================================
// [FUNCTION]_[check_live_capital_gated_until_e]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING] [CAPITAL_BEARING] [CRITICAL]]
// [FUTURE_WORK]_[TECH_DEBT]_[TECH_DEBT-203]
// [REFERENCE]_[INVARIANT]_[[H21] [H22]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the .E.0.10 BLANKET live-capital refusal — fail-safe until the .E rework lands; H21-tombstoned for removal at v5.16]
// [REFERENCE]_[CLASS]_[47]
// [REFERENCE]_[TECH_DEBT]_[TECH_DEBT-203]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline bool check_live_capital_gated_until_e(const ControllerConfig<F>& cfg,
                                             const EventLoopState<F>&) {
    return !ControllerConfig_IsLiveCapital(cfg);  // PASS unless live capital is requested -> REFUSE in live
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Live trading is gated behind the WHOLE .E-series live-readiness rework (per-node aggregator +
// reconciliation + the cross-thread torn-read closure; the sprint end-goal). Until .E lands, live
// capital is REFUSED at boot — fail-safe: no accidental live trading on the pre-.E engine. Routes
// through the single capital-authority predicate (NEW-1 / RBP Class 47), so it inherits the .E.1
// per-cluster relocation (H22) with zero edits here.
//
// >>> H21 TOMBSTONE: REMOVE this fn + its FOREACH_LIVE_READINESS_CHECK row at .E / v5.16, when the
//     live-readiness rework actually lands. Do NOT silently leave it — it would block the intended
//     go-live. Tracked: TECH_DEBT-203 (removal) + the .E.1-foundation live-readiness completion. <<<
//======================================================================
// [END_FUNCTION]_[check_live_capital_gated_until_e]
//======================================================================

//======================================================================
// [REGISTRY]_[FOREACH_LIVE_READINESS_CHECK]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING] [BOOT_TIME] [FRAMEWORK_DISCIPLINE]]
// [REFERENCE]_[DESIGN_SPEC]_[curve-registry-pattern]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the pre-flight check table — one row = check fn + severity + operator fix hint; COUNT auto-derives]
// [COLUMN]_[name]_[diagnostic label (used in stderr WARN/REFUSE messages)]
// [COLUMN]_[fn_ptr]_[template fn returning bool (true = check PASSED)]
// [COLUMN]_[severity]_[LR_SEV_REFUSE (blocks live boot) or LR_SEV_WARN (logs only)]
// [COLUMN]_[fix_hint]_[operator-actionable guidance string (~80 chars)]
// [REFERENCE]_[INVARIANT]_[H21]
// [REFERENCE]_[TECH_DEBT]_[TECH_DEBT-203]
//======================================================================
// [CODE]
//======================================================================
#define FOREACH_LIVE_READINESS_CHECK(X) \
    X(live_capital_gated_until_e,  check_live_capital_gated_until_e,  LR_SEV_REFUSE, \
      "live capital is gated behind the .E-series live-readiness rework (per-node aggregator / reconciliation / torn-read closure); run trading_mode=paper or shadow. REMOVED at v5.16 when .E lands (H21 tombstone; TECH_DEBT-203).") \
    X(secret_nonempty,             check_secret_nonempty,             LR_SEV_REFUSE, \
      "set held_out_stamp_secret in cfg (HMAC verification)") \
    X(mlockall_required,           check_mlockall_required,           LR_SEV_REFUSE, \
      "set require_mlockall=1 in cfg (deterministic latency)") \
    X(all_cores_strategy_explicit, check_all_cores_strategy_explicit, LR_SEV_REFUSE, \
      "set node_<N>_strategy explicitly for all N in [0, num_execution_nodes)") \
    X(all_ml_cores_have_model,     check_all_ml_cores_have_model,     LR_SEV_REFUSE, \
      "set node_<N>_model_path or node_<N>_model_dir for all ML nodes") \
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
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
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
//======================================================================
// [END_REGISTRY]_[FOREACH_LIVE_READINESS_CHECK]
//======================================================================

//======================================================================
// [FUNCTION]_[LiveReadiness_Verify]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING] [BOOT_TIME] [CRITICAL]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the boot-time pre-flight walk — 0 PASS / -1 live REFUSE / 1 WARN-only; compile-time table, no dispatch overhead]
// [REFERENCE]_[CLASS]_[47]
// [REFERENCE]_[INVARIANT]_[H22]
//======================================================================
// [CODE]
//======================================================================
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
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Returns: 0 on PASS (no failures); -1 on REFUSE (live + any REFUSE-sev
// failed); 1 on WARN-only (paper/shadow, OR live + only WARN-sev failed).
//
// X-macro walks the registry inline; each entry's fn_ptr called once;
// log + tally per severity; final summary log. Compile-time table; no
// runtime dispatch overhead.
//======================================================================
// [END_FUNCTION]_[LiveReadiness_Verify]
//======================================================================

}  // namespace tt

#endif  // LIVE_READINESS_HPP
