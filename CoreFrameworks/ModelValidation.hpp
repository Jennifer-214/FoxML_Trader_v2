// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [v5.14.2.E.1 — POST-LOAD MODEL VALIDATION]
//======================================================================================================
// Extracts the cross-zoo validator from EngineSharded.hpp into its own header
// so BacktestSharded.hpp + EnsembleHotSwap.hpp callers can use it directly
// without circular includes. Boundary-stable refactor (callers' API unchanged;
// only storage location moved).
//
// Why moved: PARITY-012 required BacktestSharded to call ValidateAgainstCfg,
// but BacktestSharded.hpp doesn't include EngineSharded.hpp (different code
// path). Same boundary-stable pattern as v5.14.2.A's EnsembleHotSwap.hpp split.
//======================================================================================================

#pragma once

#include "ControllerConfig.hpp"
#include "ControllerEventLoop.hpp"   // CoreContext<F> for cfg_drift_* writeback
#include "../ML_Headers/CoreModelZoo.hpp"
#include "../ML_Headers/ModelInference.hpp"
#include "../ML_Headers/BuildFlags.hpp"  // BUILD_FLAGS_HASH

#include <stdio.h>
#include <math.h>
#include <string.h>

namespace tt {

//======================================================================================================
// [v5.10.2.A — POST-LOAD VALIDATOR]
//======================================================================================================
// Cross-zoo validator extracted from EngineSharded_Run boot loop body
// (parity-check Findings #3 + #7 + #10). Subsumes THREE existing WARN/REFUSE
// subgroups uniformly across single zoo + ensemble parallel-array handles:
//
//   1. v5.9.4 + v5.9.5h xgb-and-friends WARN
//      (training_poll_interval + xgb_hyperparams + build_flags_hash)
//      gated by !acknowledge_cross_binary_version_drift
//   2. v5.9.5i inference_cfg drift detection
//      Tier 1 (freshness_tau, threshold_scale, barrier_gate_enabled) REFUSE in strict
//      Tier 2 (hard_block, bandit, fees) WARN regardless
//      gated by !acknowledge_inference_cfg_drift
//
// Writes cfg_drift_tier1/tier2_count + strict_refused into ctx.
// Returns 0 on accept, -1 on REFUSE in strict mode (Tier 1 mismatch).
//
// Callable from:
//   - EngineSharded_Run boot loop (replaces inline blocks)
//   - EngineSharded_Run hot swap branch (post-CoreModelZoo_LoadFromDir)
//
// Ensemble support: pass &ml_ensemble_zoos[i] for ezoo when
// state.cores[i].ensemble_handle != nullptr, else nullptr.
// Closes parity-check Finding #7 (drift block iterated single-zoo only).
//
// Hot-swap rollback semantics: helper returns -1 on Tier 1 REFUSE, but
// caller in hot-swap context logs + leaves (model_load_failed=1) rather
// than crashing the engine — pre-swap state isn't snapshotted, so true
// rollback would require additional infrastructure (deferred to v5.10.X).
//======================================================================================================
template <unsigned F>
static inline int CoreModelZoo_ValidateAgainstCfg(
    CoreModelZoo<F>* zoo,
    EnsembleModelZoo<F>* ezoo,                       // nullptr when ensemble inactive
    const ControllerConfig<F>& cfg,
    int core_id,
    int strict_mode,                                  // cfg.held_out_gate_strict
    int acknowledge_inference_cfg_drift,              // suppresses drift block
    int acknowledge_cross_binary_version_drift,       // suppresses xgb/poll/build_flags WARN
    CoreContext<F>* ctx                               // for cfg_drift_* counter writeback
) {
    int strict = (strict_mode == 1);
    int tier1_count = 0;
    int tier2_count = 0;
    int tier1_refused_count = 0;

    // Inner check: applies xgb-and-friends WARN (subgroup 1) + drift (subgroup 2)
    // to a single ModelHandle. Lambda captures the per-core context (logs,
    // counters) so per-handle work stays tight. h_idx >= 0 means ensemble
    // member at slot [h_idx]; -1 means single-zoo.
    auto check_handle = [&](ModelHandle<F>* h, const char* role_name, int h_idx) {
        if (!h) return;
        // Distinguishable log prefix: "core 0" vs "core 0 ensemble[2]"
        char loc[64];
        if (h_idx < 0) {
            snprintf(loc, sizeof(loc), "core %d", core_id);
        } else {
            snprintf(loc, sizeof(loc), "core %d ensemble[%d]", core_id, h_idx);
        }

        // === Subgroup 1: xgb-and-friends WARN (training_poll_interval +
        //                  xgb_hyperparams + build_flags_hash) ===
        if (!acknowledge_cross_binary_version_drift) {
            if (h->has_training_poll_interval &&
                h->training_poll_interval != cfg.poll_interval) {
                fprintf(stderr,
                    "[poll_interval] WARN: %s role=%s stamp claims "
                    "training_poll_interval=%u but cfg.poll_interval=%u "
                    "(set acknowledge_cross_binary_version_drift=1 to suppress)\n",
                    loc, role_name,
                    (unsigned)h->training_poll_interval,
                    (unsigned)cfg.poll_interval);
            }
            if (h->has_xgb_hyperparams) {
                double cfg_subsample = FPN_ToDouble(cfg.xgb_subsample);
                double cfg_colsample = FPN_ToDouble(cfg.xgb_colsample_bytree);
                if (fabs(h->stamp_xgb_subsample - cfg_subsample) > 1e-6) {
                    fprintf(stderr,
                        "[xgb_hyperparams] WARN: %s role=%s stamp "
                        "claims xgb_subsample=%.4f but cfg=%.4f\n",
                        loc, role_name, h->stamp_xgb_subsample, cfg_subsample);
                }
                if (fabs(h->stamp_xgb_colsample_bytree - cfg_colsample) > 1e-6) {
                    fprintf(stderr,
                        "[xgb_hyperparams] WARN: %s role=%s stamp "
                        "claims xgb_colsample_bytree=%.4f but cfg=%.4f\n",
                        loc, role_name, h->stamp_xgb_colsample_bytree,
                        cfg_colsample);
                }
                if (h->stamp_xgb_min_child_weight != cfg.xgb_min_child_weight) {
                    fprintf(stderr,
                        "[xgb_hyperparams] WARN: %s role=%s stamp "
                        "claims xgb_min_child_weight=%d but cfg=%d\n",
                        loc, role_name, h->stamp_xgb_min_child_weight,
                        cfg.xgb_min_child_weight);
                }
                if (h->stamp_xgb_seed != cfg.xgb_seed) {
                    fprintf(stderr,
                        "[xgb_hyperparams] WARN: %s role=%s stamp "
                        "claims xgb_seed=%d but cfg=%d\n",
                        loc, role_name, h->stamp_xgb_seed, cfg.xgb_seed);
                }
                if (strcmp(h->stamp_xgb_tree_method, cfg.xgb_tree_method) != 0) {
                    fprintf(stderr,
                        "[xgb_hyperparams] WARN: %s role=%s stamp "
                        "claims xgb_tree_method=%s but cfg=%s\n",
                        loc, role_name, h->stamp_xgb_tree_method,
                        cfg.xgb_tree_method);
                }
            }
            if (h->has_build_flags_hash &&
                h->stamp_build_flags_hash != tt::BUILD_FLAGS_HASH()) {
                fprintf(stderr,
                    "[build_flags] WARN: %s role=%s stamp claims "
                    "build_flags_hash=%016lx but current build is %016lx "
                    "(cross-build drift; set acknowledge_cross_binary_version_drift=1 to suppress)\n",
                    loc, role_name,
                    (unsigned long)h->stamp_build_flags_hash,
                    (unsigned long)tt::BUILD_FLAGS_HASH());
            }
            // v5.11.42 D.1 — xgb_train_nthread mode-divergence WARN.
            // Stamp's nthread=1 + cfg nthread>1 → operator trained in
            // parallel multi-horizon mode (which pins to 1) but engine
            // would now retrain at higher nthread → bytewise model
            // divergence. Forensic only — model already trained, can't
            // be retrained at load. Operator notification.
            if (h->has_stamp_xgb_train_nthread &&
                h->stamp_xgb_train_nthread != cfg.xgb_train_nthread) {
                fprintf(stderr,
                    "[xgb_train_nthread] WARN: %s role=%s stamp claims "
                    "xgb_train_nthread=%d but cfg.xgb_train_nthread=%d "
                    "(mode divergence; stamp=1 indicates parallel multi-horizon "
                    "training, cfg>1 indicates serial mode would diverge "
                    "bytewise on retrain; set acknowledge_cross_binary_version_drift=1 "
                    "to suppress)\n",
                    loc, role_name,
                    h->stamp_xgb_train_nthread,
                    cfg.xgb_train_nthread);
            }
        }

        // === Subgroup 2: inference_cfg drift (Tier 1 REFUSE in strict;
        //                  Tier 2 WARN regardless) ===
        if (!acknowledge_inference_cfg_drift && h->has_stamp_inference_cfg) {
            double cfg_cts = FPN_ToDouble(cfg.confidence_threshold_scale);
            double cfg_chb = FPN_ToDouble(cfg.confidence_hard_block_threshold);
            // v5.14.9.D — DELETED legacy confidence_freshness_tau drift
            // check (TECH_DEBT-004 close). Cfg field + stamp body entry
            // deleted; manual drift check no longer applicable.

            // Tier 1: directly affects serving math
            bool tier1_drift = false;
            if (fabs(h->stamp_inf_confidence_threshold_scale - cfg_cts) > 1e-6) {
                fprintf(stderr,
                    "[inference_cfg] %s: %s role=%s stamp claims "
                    "confidence_threshold_scale=%.4f but cfg=%.4f\n",
                    strict ? "REFUSE (Tier 1, strict mode)" : "WARN (Tier 1)",
                    loc, role_name, h->stamp_inf_confidence_threshold_scale, cfg_cts);
                tier1_drift = true;
                ++tier1_count;
            }
            if (h->stamp_inf_barrier_gate_enabled != BITMAP_IS_SET(cfg.gate_cfg_flags, MASK_GATE_CFG_BARRIER_GATE_ENABLED)) {
                fprintf(stderr,
                    "[inference_cfg] %s: %s role=%s stamp claims "
                    "barrier_gate_enabled=%d but cfg=%d\n",
                    strict ? "REFUSE (Tier 1, strict mode)" : "WARN (Tier 1)",
                    loc, role_name, h->stamp_inf_barrier_gate_enabled,
                    BITMAP_IS_SET(cfg.gate_cfg_flags, MASK_GATE_CFG_BARRIER_GATE_ENABLED));
                tier1_drift = true;
                ++tier1_count;
            }
            if (tier1_drift && strict) ++tier1_refused_count;

            // Tier 2: WARN regardless of strict mode
            if (fabs(h->stamp_inf_confidence_hard_block_threshold - cfg_chb) > 1e-6) {
                fprintf(stderr,
                    "[inference_cfg] WARN (Tier 2): %s role=%s stamp "
                    "claims confidence_hard_block_threshold=%.4f but cfg=%.4f\n",
                    loc, role_name, h->stamp_inf_confidence_hard_block_threshold,
                    cfg_chb);
                ++tier2_count;
            }
            if (h->has_stamp_bandit && BITMAP_IS_SET(cfg.ml_cfg_flags, MASK_ML_CFG_BANDIT_ENABLED)) {
                double cfg_bbr = FPN_ToDouble(cfg.bandit_blend_ratio);
                if (fabs(h->stamp_inf_bandit_blend_ratio - cfg_bbr) > 1e-6) {
                    fprintf(stderr,
                        "[inference_cfg] WARN (Tier 2): %s role=%s stamp "
                        "claims bandit_blend_ratio=%.4f but cfg=%.4f\n",
                        loc, role_name, h->stamp_inf_bandit_blend_ratio, cfg_bbr);
                }
            }
            if (h->has_stamp_fees && BITMAP_IS_SET(cfg.gate_cfg_flags, MASK_GATE_CFG_COST_GATE_ENABLED)) {
                double cfg_frm = FPN_ToDouble(cfg.fee_rate_maker);
                double cfg_frt = FPN_ToDouble(cfg.fee_rate_taker);
                if (fabs(h->stamp_inf_fee_rate_maker - cfg_frm) > 1e-6) {
                    fprintf(stderr,
                        "[inference_cfg] WARN (Tier 2): %s role=%s stamp "
                        "claims fee_rate_maker=%.6f but cfg=%.6f\n",
                        loc, role_name, h->stamp_inf_fee_rate_maker, cfg_frm);
                }
                if (fabs(h->stamp_inf_fee_rate_taker - cfg_frt) > 1e-6) {
                    fprintf(stderr,
                        "[inference_cfg] WARN (Tier 2): %s role=%s stamp "
                        "claims fee_rate_taker=%.6f but cfg=%.6f\n",
                        loc, role_name, h->stamp_inf_fee_rate_taker, cfg_frt);
                }
            }
        }
    };

    // 1. Single zoo: 4 roles
    //    CoreModelZoo struct uses `exit` (singular) per CoreModelZoo.hpp:60
    if (zoo) {
        check_handle(&zoo->buy_signal, "buy_signal", -1);
        check_handle(&zoo->barrier,    "barrier",    -1);
        check_handle(&zoo->regime,     "regime",     -1);
        check_handle(&zoo->exit,       "exit",       -1);
    }
    // 2. Ensemble handles (Finding #7 closure): 4 roles × N horizons
    //    EnsembleModelZoo struct uses `exit_predictor` (NOT `exit`)
    //    per CoreModelZoo.hpp:616
    if (ezoo && ezoo->active) {
        for (int h = 0; h < ezoo->buy_signal_count; ++h)
            check_handle(&ezoo->buy_signal[h], "buy_signal", h);
        for (int h = 0; h < ezoo->barrier_count; ++h)
            check_handle(&ezoo->barrier[h], "barrier", h);
        for (int h = 0; h < ezoo->regime_count; ++h)
            check_handle(&ezoo->regime[h], "regime", h);
        for (int h = 0; h < ezoo->exit_predictor_count; ++h)
            check_handle(&ezoo->exit_predictor[h], "exit", h);
    }

    // Writeback drift counters (Finding #10 closure: now updated on hot-swap too)
    if (ctx) {
        ctx->cfg_drift_tier1_count = (uint8_t)(tier1_count > 255 ? 255 : tier1_count);
        ctx->cfg_drift_tier2_count = (uint8_t)(tier2_count > 255 ? 255 : tier2_count);
        ctx->cfg_drift_strict_refused = (tier1_refused_count > 0) ? 1 : 0;
    }

    if (tier1_refused_count > 0 && strict) {
        fprintf(stderr,
            "[inference_cfg] FATAL: core %d had %d Tier 1 mismatch(es) "
            "in strict mode. Set held_out_gate_strict=0 (warn-only) "
            "OR acknowledge_inference_cfg_drift=1 to bypass, "
            "OR retrain the model with current cfg.\n",
            core_id, tier1_refused_count);
        return -1;  // REFUSE
    }
    return 0;
}

}  // namespace tt
