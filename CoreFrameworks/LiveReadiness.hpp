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
//   - [FUNCTION]_[check_no_state_dir_contention]
//   - [FUNCTION]_[check_state_dir_writable]
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
#include "../MemHeaders/HealthLog.hpp"          // D-483 C — the walker's durable line per failed row
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

// E.1.2.C leg 3 (2026-08-20, R1 cascade #2) — ensemble sister. Before this,
// the live-readiness drift checks aggregated ONLY the single-zoo handles, so
// an ensemble-only deployment (the trainer's native layout) passed them
// VACUOUSLY — the ensemble's drift bits never fed the live gate. Mirrors the
// ShardedSnapshot dual-walk fix (single-zoo + ensemble arrays, exit incl.).
template <unsigned F>
inline uint16_t aggregate_ezoo_drift(const void* ezoo_handle) {
    const EnsembleModelZoo<F>* ez = (const EnsembleModelZoo<F>*)ezoo_handle;
    if (!ez) return 0;
    uint16_t acc = 0;
    for (int h = 0; h < ez->barrier_count        && h < ENSEMBLE_HORIZON_MAX; ++h) acc |= (uint16_t)ez->barrier[h].drift_flags_at_load;
    for (int h = 0; h < ez->regime_count         && h < ENSEMBLE_HORIZON_MAX; ++h) acc |= (uint16_t)ez->regime[h].drift_flags_at_load;
    for (int h = 0; h < ez->buy_signal_count     && h < ENSEMBLE_HORIZON_MAX; ++h) acc |= (uint16_t)ez->buy_signal[h].drift_flags_at_load;
    for (int h = 0; h < ez->exit_predictor_count && h < ENSEMBLE_HORIZON_MAX; ++h) acc |= (uint16_t)ez->exit_predictor[h].drift_flags_at_load;
    return acc;
}

// E.1.2.C leg 3 — "this ML node has a serving model": single zoo OR a ready
// ensemble. The three single-zoo-blind gates below share this predicate
// (Run.hpp's swap-to-ML refusal carries the same logic inline).
template <unsigned F>
inline bool node_has_serving_model(const NodeContext<F>& node) {
    if (node.model_handle != nullptr) return true;
    const EnsembleModelZoo<F>* ez = (const EnsembleModelZoo<F>*)node.ensemble_handle;
    return ez != nullptr
        && BITMAP_IS_SET(ez->init_flags, MASK_EZOO_ACTIVE)
        && ez->primary_count > 0
        && ez->primary_handles != nullptr;
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

// E.1.3 3b(ii) commit 1 (D-479) — the durable ring-full fatal record is a Health_Log line;
// an empty health_log_path makes it a stderr line only. Live capital needs the channel.
// Commit 2 (AR-8 N-3; Class 51 / AR-24): "set" is not "usable" — a mis-pointed or unwritable
// path passed the old non-empty test and failed at the FATAL moment, when Health_Log's fopen
// returned 0 and the stderr fallback blamed an "empty" path. Probe the channel the way its
// consumer uses it (fopen "a" + fclose — creates the file if absent, as
// Health_LogConfigureWithRotation's best-effort mkdir intends). Boot-time only.
template <unsigned F>
inline bool check_health_log_path_set(const ControllerConfig<F>& cfg,
                                      const EventLoopState<F>&) {
    if (cfg.health_log_path[0] == '\0') return false;
    FILE* f = fopen(cfg.health_log_path, "a");
    if (!f) return false;
    fclose(f);
    return true;
}

//======================================================================
// [FUNCTION]_[check_no_state_dir_contention]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING] [BOOT_TIME] [PERSISTENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[D-483 C — no ACTIVE ML node's bandit state dir is HELD by another node or process (the STATE_DIR_CONTENDED bind outcome); reads the bind's verdict, never re-derives a pairwise dir compare]
// [REFERENCE]_[DECISION]_[D-483]
// [REFERENCE]_[TECH_DEBT]_[TECH_DEBT-331]
// [REFERENCE]_[INVARIANT]_[H22]
//======================================================================
// [CODE]
//======================================================================
// The state-dir BIND (EnsembleModelZoo_BindStateDir — the bind_state_dir post-load row) has
// ALREADY run for every ML node when this gate walks (per-node boot precedes it), and the
// kernel performed the identity test at flock time. So the rows below read the bind OUTCOME
// from the ezoo's init flags rather than re-deriving an O(N²) string compare of cfg dirs —
// which could not see a second PROCESS on the dir, nor "x" vs "x/". TWO rows, not one:
// CONTENDED (give each node its own dir) and UNWRITABLE (fix permissions / disk) are
// different operator actions; a shared bit would mislabel one as the other (Class 51 mode C).
// Both are REFUSE in live: a node trading with persistence OFF silently loses every session's
// learning — and until the E.1.5 node-owned home lands, "shared dir" also means the OTHER
// node's learned weights are what this node loaded.
template <unsigned F>
inline bool check_no_state_dir_contention(const ControllerConfig<F>& cfg,
                                          const EventLoopState<F>& state) {
    for (uint16_t i = 0; i < cfg.num_execution_nodes && i < 16; ++i) {
        if (cfg.node_strategies[i] != STRATEGY_ML) continue;
        const EnsembleModelZoo<F>* ez =
            (const EnsembleModelZoo<F>*)state.nodes[tt::NodeIdx{(int16_t)i}].ensemble_handle;
        if (ez && BITMAP_IS_SET(ez->init_flags, MASK_EZOO_ACTIVE) &&
            BITMAP_IS_SET(ez->init_flags, MASK_EZOO_STATE_DIR_CONTENDED)) return false;
    }
    return true;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[check_no_state_dir_contention]
//======================================================================

//======================================================================
// [FUNCTION]_[check_state_dir_writable]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING] [BOOT_TIME] [PERSISTENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[D-483 C — every ACTIVE ML node's bandit state dir accepted its lock file (no STATE_DIR_UNWRITABLE bind outcome); the sibling of the contention row with the other operator action]
// [REFERENCE]_[DECISION]_[D-483]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline bool check_state_dir_writable(const ControllerConfig<F>& cfg,
                                     const EventLoopState<F>& state) {
    for (uint16_t i = 0; i < cfg.num_execution_nodes && i < 16; ++i) {
        if (cfg.node_strategies[i] != STRATEGY_ML) continue;
        const EnsembleModelZoo<F>* ez =
            (const EnsembleModelZoo<F>*)state.nodes[tt::NodeIdx{(int16_t)i}].ensemble_handle;
        if (ez && BITMAP_IS_SET(ez->init_flags, MASK_EZOO_ACTIVE) &&
            BITMAP_IS_SET(ez->init_flags, MASK_EZOO_STATE_DIR_UNWRITABLE)) return false;
    }
    return true;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[check_state_dir_writable]
//======================================================================

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
        // E.1.2.C leg 3 — ensemble-aware (was single-zoo-blind: an ML node
        // serving a VERIFIED ensemble was refused at live boot with a
        // misleading "no model" hint — R1's narrowing).
        if (cfg.node_strategies[i] == STRATEGY_ML &&
            !node_has_serving_model(state.nodes[tt::NodeIdx{(int16_t)i}])) {
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
        const NodeModelZoo<F>* zoo = (const NodeModelZoo<F>*)state.nodes[tt::NodeIdx{(int16_t)i}].model_handle;
        // E.1.2.C leg 3 — ensemble drift bits feed the gate too (was vacuous
        // for ensemble-only nodes).
        uint16_t drift = (uint16_t)(aggregate_zoo_drift(zoo) |
                                    aggregate_ezoo_drift<F>(state.nodes[tt::NodeIdx{(int16_t)i}].ensemble_handle));
        if (BITMAP_IS_SET(drift, FAILURE_MASK_model_age_warn)) {
            return false;
        }
    }
    return true;
}

template <unsigned F>
inline bool check_no_feature_hash_drift(const ControllerConfig<F>& cfg,
                                        const EventLoopState<F>& state) {
    for (uint16_t i = 0; i < cfg.num_execution_nodes && i < 16; ++i) {
        const NodeModelZoo<F>* zoo = (const NodeModelZoo<F>*)state.nodes[tt::NodeIdx{(int16_t)i}].model_handle;
        uint16_t drift = (uint16_t)(aggregate_zoo_drift(zoo) |
                                    aggregate_ezoo_drift<F>(state.nodes[tt::NodeIdx{(int16_t)i}].ensemble_handle));
        if (BITMAP_IS_SET(drift, FAILURE_MASK_feature_hash_drift)) {
            return false;
        }
    }
    return true;
}

template <unsigned F>
inline bool check_no_label_hash_drift(const ControllerConfig<F>& cfg,
                                      const EventLoopState<F>& state) {
    for (uint16_t i = 0; i < cfg.num_execution_nodes && i < 16; ++i) {
        const NodeModelZoo<F>* zoo = (const NodeModelZoo<F>*)state.nodes[tt::NodeIdx{(int16_t)i}].model_handle;
        uint16_t drift_label_hash_drift = (uint16_t)(aggregate_zoo_drift(zoo) |
                                    aggregate_ezoo_drift<F>(state.nodes[tt::NodeIdx{(int16_t)i}].ensemble_handle));
        if (BITMAP_IS_SET(drift_label_hash_drift, FAILURE_MASK_label_hash_drift)) {
            return false;
        }
    }
    return true;
}

template <unsigned F>
inline bool check_no_build_flags_drift(const ControllerConfig<F>& cfg,
                                       const EventLoopState<F>& state) {
    for (uint16_t i = 0; i < cfg.num_execution_nodes && i < 16; ++i) {
        const NodeModelZoo<F>* zoo = (const NodeModelZoo<F>*)state.nodes[tt::NodeIdx{(int16_t)i}].model_handle;
        uint16_t drift_build_flags_drift = (uint16_t)(aggregate_zoo_drift(zoo) |
                                    aggregate_ezoo_drift<F>(state.nodes[tt::NodeIdx{(int16_t)i}].ensemble_handle));
        if (BITMAP_IS_SET(drift_build_flags_drift, FAILURE_MASK_build_flags_drift)) {
            return false;
        }
    }
    return true;
}

template <unsigned F>
inline bool check_all_stamps_hmac_verified(const ControllerConfig<F>& cfg,
                                           const EventLoopState<F>& state) {
    for (uint16_t i = 0; i < cfg.num_execution_nodes && i < 16; ++i) {
        const NodeModelZoo<F>* zoo = (const NodeModelZoo<F>*)state.nodes[tt::NodeIdx{(int16_t)i}].model_handle;
        uint16_t drift_stamp_hmac_not_verified = (uint16_t)(aggregate_zoo_drift(zoo) |
                                    aggregate_ezoo_drift<F>(state.nodes[tt::NodeIdx{(int16_t)i}].ensemble_handle));
        if (BITMAP_IS_SET(drift_stamp_hmac_not_verified, FAILURE_MASK_stamp_hmac_not_verified)) {
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
      "set held_out_stamp_secret + model_verify_strict=1") \
    /* E.1.3 3b(ii) commit 1 (D-479 G3-2; V-1 finding M-3): the ring-full fatal record is a   */ \
    /* Health_Log CRITICAL line — the DURABLE half of "a fill is never dropped silently". With  */ \
    /* health_log_path empty (the cfg default) Health_Log returns 0 and the decoded fill exists  */ \
    /* on stderr only. Live capital REFUSES to boot without the durable channel.                 */ \
    X(health_log_path_set,         check_health_log_path_set,         LR_SEV_REFUSE, \
      "set a WRITABLE health_log_path (e.g. logging/health.jsonl; probed with fopen at boot) — the ring-full fatal record needs a durable channel") \
    /* D-483 C (2026-09-04) — the bind-outcome rows (TECH_DEBT-331). The state-dir bind ran at    */ \
    /* per-node boot; these read its verdict. A node whose learned state is not persisting must    */ \
    /* not go LIVE silently: CONTENDED = the other node's (or a backtest process's) weights are     */ \
    /* what it loaded + nothing carries over; UNWRITABLE = nothing carries over. Paper WARNs.       */ \
    X(no_state_dir_contention,     check_no_state_dir_contention,     LR_SEV_REFUSE, \
      "another node or process holds an ML node's bandit state dir — give each ML node its own node_<N>_model_dir, and never run a backtest on the paper engine's dir (TECH_DEBT-331 / D-483); persistence is OFF for the contended node until then") \
    X(state_dir_writable,          check_state_dir_writable,          LR_SEV_REFUSE, \
      "an ML node's bandit state dir is UNWRITABLE (its lock file could not be created) — fix the dir's permissions / free disk; persistence is OFF for that node until then")

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
// FOREACH_BANDIT_ALGORITHM (v5.14.10.A). Boot-only path; ~10us total (the
// row COUNT auto-derives — FOREACH_LIVE_READINESS_CHECK_COUNT; the test pins
// it); well below operator-perceptible threshold.
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

    // D-483 C (2026-09-04): every failed row ALSO lands one durable line in the health log
    // (Health_LogConfigure precedes this gate at boot; the health_log_path_set row makes the
    // channel a live REFUSE-precondition, so in live it is there when it matters). A pre-flight
    // failure the operator only ever saw scroll past on stderr was the Class-12 shape: a
    // guard that fires into the void. `warned` now counts PAPER failures too — the summary
    // below used to print "all pre-flight checks PASSED" under a column of paper WARN lines.
    #define X(name_id, fn_ptr, severity, fix_hint)                                              \
        if (!fn_ptr<F>(cfg, state)) {                                                            \
            if (live && (severity) == LR_SEV_REFUSE) {                                           \
                fprintf(stderr,                                                                  \
                    "[live_readiness] LIVE REFUSE: %s failed. Fix: %s\n",                        \
                    #name_id, fix_hint);                                                         \
                Health_Log(HEALTH_CRITICAL, "live_readiness", -1,                               \
                           "LIVE REFUSE: %s failed: %s", #name_id, fix_hint);                    \
                refused++;                                                                       \
            } else {                                                                             \
                fprintf(stderr,                                                                  \
                    "[live_readiness] %s mode WARN: %s failed. Fix: %s\n",                       \
                    mode_str, #name_id, fix_hint);                                               \
                Health_Log(HEALTH_WARN, "live_readiness", -1,                                   \
                           "%s mode WARN: %s failed: %s", mode_str, #name_id, fix_hint);         \
                warned++;                                                                        \
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
    } else {
        // paper / shadow: failures are WARN-only by contract (return 0), but SAY so —
        // never "PASSED" over a list of failed rows.
        fprintf(stderr, "[live_readiness] %s mode: %d pre-flight WARN(s); engine proceeding\n",
                mode_str, warned);
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
