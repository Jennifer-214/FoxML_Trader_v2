// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[CoreFrameworks/ModelValidation.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML] [SLOW_PATH]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the post-load cross-zoo drift validator — registry-driven stamp<->cfg/build/runtime checks over every handle]
// [CONTAINS]
//   - [FUNCTION]_[NodeModelZoo_ValidateAgainstCfg]
//======================================================================================================
// Cross-zoo validator. Walks every loaded handle (single zoo + ensemble) and
// detects stamp ↔ cfg / build / runtime drift via registry-driven dispatch.
//
// v5.15.5.A.7 — STRUCTURAL REFACTOR (closes inferred Class 18 mirror class):
//   - 14 manual drift if-blocks (subgroup 1 xgb cross-binary WARN + subgroup
//     2 inference_cfg Tier 1/2) REPLACED with single FOREACH_CFG_DRIFT_CHECK
//     X-macro walker. Adding next drift check is now 1 registry row vs touching
//     this function body.
//   - Template-deferred LogFn injection (3rd application of
//     template-deferred-dependency-injection.md pattern after v5.14.4.B
//     Reconcile_ApplyMissedFills/AutoCancelStale). Default `tt::StderrLog`
//     preserves backward-compat for all 4 production callers; tests inject
//     capturing functors without stderr-redirect hackery.
//   - Per-category bits set on `h->drift_flags_at_load` (FAILURE_MASK_cfg_*)
//     — closes ArchField ↔ CfgDrift bitmap asymmetry (ArchField sets per-entry
//     bits via FOREACH_ARCH_FIELD_DRIFT; cfg-drift now sets per-category bits
//     via FOREACH_CFG_DRIFT_CHECK Y3 category dispatch).
//   - Ack flags migrated to ops_cfg_flags bitmap (TECH_DEBT-009 boolean orphan
//     tail closed). Function parameters preserve original int signature
//     (boundary-stable refactor); callers pass `BITMAP_IS_SET(...)` at the
//     call sites (mechanical migration; existing callers updated v5.15.5.A.7).
//
// CALLERS (4 production sites — all signature-compatible post-refactor):
//   1. CoreFrameworks/EngineSharded.hpp boot loop
//   2. CoreFrameworks/HotSwap.hpp single-zoo hot-swap
//   3. CoreFrameworks/EnsembleHotSwap.hpp ensemble hot-swap
//   4. Backtest/BacktestSharded.hpp validate path (PARITY-012)
//
// PATTERNS (DESIGN_SPECS cross-refs):
//   - x-macro-registry-with-presence-dispatch.md (Y3 token-paste dispatch)
//   - dual-axis-y3-dispatch-pattern.md (severity × category × compare_kind)
//   - stamp-vs-runtime-drift-detection-registry.md (canonical drift pattern)
//   - template-deferred-dependency-injection.md (LogFn template parameter)
//   - bitmap-flag-api.md (per-category fail_mask BITMAP_SET)
//   - structural-fix-preferred-decision-framework.md (Class 18 extinction)
//
// CLAUDE.md cross-refs: items 13 (X-macro), 15 (parity-tested), 17 (slow-path
// only; +0 ns hot-path delta), 19 (structural fix), 20 (BITMAP_* API), 23
// (type-trait dispatch via templated helpers).
//======================================================================================================

#pragma once

#include "ControllerConfig.hpp"
#include "ControllerEventLoop.hpp"   // NodeContext<F> for cfg_drift_* writeback
#include "../ML_Headers/NodeModelZoo.hpp"
#include "../ML_Headers/ModelInference.hpp"
#include "../ML_Headers/BuildFlags.hpp"  // BUILD_FLAGS_HASH
#include "../ML_Headers/CfgDriftCheckRegistry.hpp"  // v5.15.5.A.7: FOREACH_CFG_DRIFT_CHECK + Y3 dispatchers

#include <stdarg.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <type_traits>  // is_array_v / is_floating_point_v / is_unsigned_v for log_drift_pair

namespace tt {

//------------------------------------------------------------------------------------------------------
// [SECTION]_[log injection support (v5.15.5.A.7) — StderrLog functor + log_drift_pair helper]
//------------------------------------------------------------------------------------------------------
// Default LogFn for production callers — preserves pre-refactor `fprintf(stderr, ...)` semantics
// exactly. Tests inject a capturing functor (e.g., recording lambda) for log-content assertions.
//
// LogFn is invoked as a printf-style callable: `log_fn("fmt %d", arg)`. Variadic template
// forwards args to vfprintf or vsnprintf depending on the functor. Per template-deferred-
// dependency-injection.md: zero runtime overhead in production (compiler inlines
// StderrLog::operator() at the call site).

//======================================================================
// [STRUCT]_[StderrLog]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [HELPER]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the default production LogFn functor — a printf-style operator() forwarding to fprintf(stderr); tests inject a capturing functor instead (zero runtime overhead, inlined)]
//======================================================================
// [CODE]
//======================================================================
struct StderrLog {
    template <typename... Args>
    void operator()(const char* fmt, Args... args) const {
        fprintf(stderr, fmt, args...);
    }
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]   (tool-refreshed — do NOT hand-edit; check_cache_layout --fix owns these)
//======================================================================
// [END_STRUCT]_[StderrLog]
//======================================================================

// Type-dispatched value pair logger for drift-detection output. Replaces ~14
// manual `fprintf(stderr, "stamp=%g cfg=%g", ...)` lines (each with custom
// format) with one templated dispatch via if-constexpr on T per CLAUDE.md item
// 23. Format choices match the pre-refactor manual code:
//   - char[N] arrays    → %s (string compare; mirrors xgb_tree_method)
//   - floating-point    → %.6g (mirrors confidence_threshold_scale, fee_rate_*, ridge_*)
//   - uint64 unsigned   → %016lx (hex; mirrors build_flags_hash)
//   - other unsigned    → %u (mirrors training_poll_interval)
//   - signed int        → %d (mirrors xgb_min_child_weight, xgb_seed, barrier_blend_mode)

// Separate T1/T2 template parameters because stamp side (array) and cfg side
// (const char*) may decay differently when the field is a char[N] string. T1
// drives dispatch (the stamp-side type carries the array information).
template <typename LogFn, typename T1, typename T2>
inline void log_drift_pair(LogFn& log_fn, const char* name, const T1& stamp_v, const T2& cfg_v) {
    if constexpr (std::is_array_v<T1> || std::is_pointer_v<std::decay_t<T1>>) {
        log_fn("stamp.%s=%s cfg.%s=%s", name, (const char*)stamp_v, name, (const char*)cfg_v);
    } else if constexpr (std::is_floating_point_v<T1>) {
        log_fn("stamp.%s=%.6g cfg.%s=%.6g", name, (double)stamp_v, name, (double)cfg_v);
    } else if constexpr (std::is_same_v<T1, uint64_t>) {
        log_fn("stamp.%s=%016lx cfg.%s=%016lx", name, (unsigned long)stamp_v, name, (unsigned long)cfg_v);
    } else if constexpr (std::is_unsigned_v<T1>) {
        log_fn("stamp.%s=%u cfg.%s=%u", name, (unsigned)stamp_v, name, (unsigned)cfg_v);
    } else {
        log_fn("stamp.%s=%d cfg.%s=%d", name, (int)stamp_v, name, (int)cfg_v);
    }
}

//======================================================================
// [FUNCTION]_[NodeModelZoo_ValidateAgainstCfg]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML] [SLOW_PATH] [CRITICAL]]
// [REFERENCE]_[DESIGN_SPEC]_[template-deferred-dependency-injection]
// [REFERENCE]_[DESIGN_SPEC]_[stamp-vs-runtime-drift-detection-registry]
// [REFERENCE]_[PARITY]_[PARITY-012]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the cross-zoo drift walk — FOREACH_CFG_DRIFT_CHECK over every handle; tier counters + strict REFUSE]
// [REFERENCE]_[CLASS]_[18]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F, typename LogFn = tt::StderrLog>
static inline int NodeModelZoo_ValidateAgainstCfg(
    NodeModelZoo<F>* zoo,
    EnsembleModelZoo<F>* ezoo,                       // nullptr when ensemble inactive
    const ControllerConfig<F>& cfg,
    int node_id,
    int strict_mode,                                  // cfg.held_out_gate_strict
    int acknowledge_inference_cfg_drift,              // ops_cfg_flags bit; suppresses INFERENCE_CFG category
    int acknowledge_cross_binary_version_drift,       // ops_cfg_flags bit; suppresses CROSS_BINARY category
    NodeContextDisplayMeta<F>* meta,                  // for cfg_drift_tier*_count writeback (v5.15.5.B.2: extracted from NodeContext)
    NodeContext<F>* ctx,                              // for cfg_drift_strict_refused bitmap bit (v5.15.5.B.3: bit-packed in node_state_flags)
    LogFn log_fn = LogFn{}                            // v5.15.5.A.7: injected logger (default = stderr)
) {
    int strict = (strict_mode == 1);
    int tier1_count = 0;
    int tier2_count = 0;
    int tier1_refused_count = 0;

    // Inner check: applies FOREACH_CFG_DRIFT_CHECK walker to a single ModelHandle.
    // Lambda captures per-core context (counters, log_fn) so per-handle work is tight.
    // h_idx >= 0 means ensemble member at slot [h_idx]; -1 means single-zoo.
    auto check_handle = [&](ModelHandle<F>* h, const char* role_name, int h_idx) {
        if (!h) return;
        // Distinguishable log prefix: "core 0" vs "core 0 ensemble[2]"
        char loc[64];
        if (h_idx < 0) {
            snprintf(loc, sizeof(loc), "node %d", node_id);
        } else {
            snprintf(loc, sizeof(loc), "node %d ensemble[%d]", node_id, h_idx);
        }

        // ──────────────────────────────────────────────────────────────────────
        // FOREACH_CFG_DRIFT_CHECK walker — replaces 14 manual if-blocks
        // ──────────────────────────────────────────────────────────────────────
        // Per-entry composition (dual-axis Y3 + per-entry compare/gate/fail_mask):
        //   1. gate_when (per-entry, includes STAMP_HAS forward-compat + cfg-side feature gate)
        //   2. category ack check (HANDLE_DRIFT_CATEGORY_<X>_ACK reads ops_cfg_flags bit)
        //   3. compare_kind dispatch (HANDLE_DRIFT_CMP_<X> per-type compare)
        //   4. severity counter mutation (HANDLE_DRIFT_SEVERITY_<X> tier1/tier2)
        //   5. log_fn emit with type-dispatched value formatting (tt::log_drift_pair)
        //   6. per-category fail_mask SET on h->drift_flags_at_load
        //
        // Adding a new drift check = 1 row in FOREACH_CFG_DRIFT_CHECK; function
        // body unchanged. Compile-time enforcement prevents Class 18 mirror.

        // Resolve ack-gate values per-category (cached locally to avoid repeated
        // BITMAP_IS_SET calls inside the walker; slow-path micro-optimization).
        const bool ack_inf_cfg     = HANDLE_DRIFT_CATEGORY_INFERENCE_CFG_ACK(cfg);
        const bool ack_cross_binary = HANDLE_DRIFT_CATEGORY_CROSS_BINARY_ACK(cfg);
        (void)acknowledge_inference_cfg_drift;     // function-param signature preserved for boundary stability;
        (void)acknowledge_cross_binary_version_drift; // cohort-migrated ack flags now resolved via ops_cfg_flags.

        #define X(NAME, type, severity, category, compare_kind,                                  \
                  get_stamp_expr, get_cfg_expr, gate_when, fail_mask, doc)                       \
            do {                                                                                  \
                /* Compose category ack-gate at compile time per entry (resolves to a            \
                 * specific local bool — ack_inf_cfg or ack_cross_binary): */                     \
                const bool _drift_acked = (HANDLE_DRIFT_CATEGORY_##category##_ACK_LOCAL);         \
                if ((gate_when) && !_drift_acked) {                                               \
                    const auto _stamp_v = (get_stamp_expr);                                       \
                    const auto _cfg_v   = (get_cfg_expr);                                         \
                    if (HANDLE_DRIFT_CMP_##compare_kind(_stamp_v, _cfg_v)) {                      \
                        log_fn("[cfg-drift] " #category " " #severity ": %s role=%s ",            \
                               loc, role_name);                                                   \
                        tt::log_drift_pair(log_fn, #NAME, _stamp_v, _cfg_v);                      \
                        log_fn(" — %s\n", doc);                                                   \
                        BITMAP_SET(h->drift_flags_at_load, fail_mask);                            \
                        HANDLE_DRIFT_SEVERITY_##severity(strict,                                  \
                            tier1_count, tier2_count, tier1_refused_count);                       \
                    }                                                                              \
                }                                                                                  \
            } while (0);
        // Per-entry ack-gate local resolves via Y3 token-paste:
        #define HANDLE_DRIFT_CATEGORY_INFERENCE_CFG_ACK_LOCAL ack_inf_cfg
        #define HANDLE_DRIFT_CATEGORY_CROSS_BINARY_ACK_LOCAL  ack_cross_binary
        FOREACH_CFG_DRIFT_CHECK(X)
        #undef HANDLE_DRIFT_CATEGORY_CROSS_BINARY_ACK_LOCAL
        #undef HANDLE_DRIFT_CATEGORY_INFERENCE_CFG_ACK_LOCAL
        #undef X
    };

    // 1. Single zoo: 4 roles (buy_signal, barrier, regime, exit).
    //    NodeModelZoo struct uses `exit` (singular) per NodeModelZoo.hpp:60.
    if (zoo) {
        check_handle(&zoo->buy_signal, "buy_signal", -1);
        check_handle(&zoo->barrier,    "barrier",    -1);
        check_handle(&zoo->regime,     "regime",     -1);
        check_handle(&zoo->exit,       "exit",       -1);
    }
    // 2. Ensemble handles: 4 roles × N horizons (closes parity-check Finding #7).
    //    EnsembleModelZoo struct uses `exit_predictor` per NodeModelZoo.hpp:616.
    if (ezoo && BITMAP_IS_SET(ezoo->init_flags, MASK_EZOO_ACTIVE)) {
        for (int h = 0; h < ezoo->buy_signal_count; ++h)
            check_handle(&ezoo->buy_signal[h], "buy_signal", h);
        for (int h = 0; h < ezoo->barrier_count; ++h)
            check_handle(&ezoo->barrier[h], "barrier", h);
        for (int h = 0; h < ezoo->regime_count; ++h)
            check_handle(&ezoo->regime[h], "regime", h);
        for (int h = 0; h < ezoo->exit_predictor_count; ++h)
            check_handle(&ezoo->exit_predictor[h], "exit", h);
    }

    // Writeback drift counters + strict-refused flag (closes parity-check
    // Finding #10 — now updated on hot-swap too via shared helper).
    // v5.15.5.B.2 — counters moved from NodeContext to NodeContextDisplayMeta.
    // v5.15.5.B.3 — cfg_drift_strict_refused migrated from DisplayMeta back
    // to NodeContext as a node_state_flags bitmap bit. Final home — closes
    // the byte-per-flag pattern that recurred through .B.2.
    if (meta) {
        meta->cfg_drift_tier1_count = (uint8_t)(tier1_count > 255 ? 255 : tier1_count);
        meta->cfg_drift_tier2_count = (uint8_t)(tier2_count > 255 ? 255 : tier2_count);
    }
    if (ctx) {
        if (tier1_refused_count > 0) {
            NODE_STATE_FLAG_SET(*ctx, CFG_DRIFT_STRICT_REFUSED);
        } else {
            NODE_STATE_FLAG_CLR(*ctx, CFG_DRIFT_STRICT_REFUSED);
        }
    }

    if (tier1_refused_count > 0 && strict) {
        log_fn(
            "[cfg-drift] FATAL: node %d had %d Tier 1 mismatch(es) in strict mode. "
            "Set held_out_gate_strict=0 (warn-only) OR acknowledge_inference_cfg_drift=1 "
            "in cfg (ops_cfg_flags bitmap v5.15.5.A.7+) to bypass, OR retrain the model "
            "with current cfg.\n",
            node_id, tier1_refused_count);
        return -1;  // REFUSE
    }
    return 0;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Cross-zoo validator extracted from EngineSharded_Run boot loop body. Subsumes
// drift detection across single zoo + ensemble parallel-array handles via
// FOREACH_CFG_DRIFT_CHECK X-macro walker (18 entries: 8 cross-binary WARN +
// 6 inference_cfg Tier 1/2 + 4 v5.15.5.A.7 per-horizon barrier cohort = 18).
//
// Writes cfg_drift_tier1/tier2_count + strict_refused into ctx. Returns 0 on
// accept, -1 on REFUSE in strict mode (Tier 1 mismatch).
//
// Callable from:
//   - EngineSharded_Run boot loop
//   - EngineSharded_Run hot swap branch (single-zoo + ensemble paths)
//   - BacktestSharded validate (PARITY-012)
//
// Ensemble support: pass &ml_ensemble_zoos[i] for ezoo when ensemble active,
// else nullptr.
//
// LogFn template parameter (v5.15.5.A.7) — default tt::StderrLog preserves
// pre-refactor production behavior; tests pass capturing functor for log
// content assertions without stderr-redirect.
//======================================================================
// [END_FUNCTION]_[NodeModelZoo_ValidateAgainstCfg]
//======================================================================

}  // namespace tt
