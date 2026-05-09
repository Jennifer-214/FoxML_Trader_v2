// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [CORE MODEL ZOO]
//======================================================================================================
// per-core bundle of role-specific model handles. lets each ML core load
// multiple specialized models (barrier prediction, regime classifier, exit
// timing, buy signal) from a single config-specified directory.
//
// usage:
//   CoreModelZoo<F> zoo;
//   CoreModelZoo_Init(&zoo);
//   CoreModelZoo_LoadFromDir(&zoo, "models/aggressive/", MODEL_BACKEND_XGBOOST);
//   // dispatcher receives &zoo as model_ctx
//   if (zoo.loaded_mask & CORE_MODEL_BARRIER) {
//       float multi[3];
//       Model_PredictMulti(&zoo.barrier, features, n, multi, 3);
//       // multi[0]=stable, multi[1]=peak, multi[2]=valley
//   }
//   CoreModelZoo_Free(&zoo);
//
// directory layout:
//   models/aggressive/
//     barrier.json       # 3-class softmax: stable/peak/valley
//     buy_signal.json    # legacy single-binary (backward compat)
//     regime.json        # multi-class regime classifier (future)
//     exit.json          # exit timing (future)
//
// missing files = role disabled (silently no-op). bundle deployment is atomic.
//======================================================================================================
#ifndef CORE_MODEL_ZOO_HPP
#define CORE_MODEL_ZOO_HPP

#include "ModelInference.hpp"
#include "FeatureRegistry.hpp"  // v5.8.6: FEATURE_REGISTRY_HASH() drift catch
#include "../Backtest/LabelFunctions.hpp"  // v5.10.1.A: LABEL_REGISTRY_HASH() drift catch
#include "../CoreFrameworks/ControllerConfig.hpp"  // v5.14.1.B.3: cfg* for X-macro drift check
#include "BanditLearning.hpp"   // v5.10.0a.G.7 — per-regime BanditState in EnsembleModelZoo
#include "RidgeBlender.hpp"     // v5.14.0 — Ridge risk-parity blending state on EnsembleModelZoo
#include "../Strategies/StrategyInterface.hpp"  // v5.10.0a.G.7 — NUM_REGIMES
#include "../Version.hpp"        // v5.8.6: ENGINE_VERSION_STRING for boot log
#include <stdio.h>
#include <string.h>
#include <stdlib.h>      // v5.10.0a.G.5 — strtol for AutoDetect horizon parse
#include <sys/stat.h>
#include <sys/types.h>   // v5.10.0a.G.5 — dirent for AutoDetect filesystem scan
#include <unistd.h>      // v5.10.0a.G.9 — access() for bandit_state.json probe
#include <dirent.h>      // v5.10.0a.G.5 — opendir/readdir for AutoDetect

// role bitmap — set in zoo->loaded_mask when a model is successfully loaded
#define CORE_MODEL_BARRIER     (1u << 0)  // 3-class softmax: stable/peak/valley
#define CORE_MODEL_REGIME      (1u << 1)  // multi-class regime classifier
#define CORE_MODEL_EXIT        (1u << 2)  // exit timing model
#define CORE_MODEL_BUY_SIGNAL  (1u << 3)  // legacy single-binary buy signal

template <unsigned F>
struct CoreModelZoo {
    ModelHandle<F> barrier;     // 3-class P(stable)/P(peak)/P(valley) when num_outputs=3
    ModelHandle<F> regime;      // multi-class regime
    ModelHandle<F> exit;        // exit timing
    ModelHandle<F> buy_signal;  // legacy single-binary
    unsigned int loaded_mask;   // bitmap of loaded roles (CORE_MODEL_*)
    // v5.11.62 — primary-role indirection. Strategy code reads
    // zoo->primary_handle (set by LoadFromDir to whichever role file was
    // actually present in priority order: buy_signal > barrier > regime).
    // Decouples strategy logic from "which role file did the trainer save
    // under" — operator can train barrier 3-class OR buy_signal binary
    // and the engine handles both transparently. nullptr = no model loaded.
    ModelHandle<F> *primary_handle;
    int             primary_target_class;   // mirrors primary_handle->buy_class_idx for snapshot
    char            primary_role_name[16];  // "buy_signal" | "barrier" | "regime" | ""
};

//======================================================================================================
template <unsigned F>
inline void CoreModelZoo_Init(CoreModelZoo<F> *zoo) {
    Model_Init(&zoo->barrier);
    Model_Init(&zoo->regime);
    Model_Init(&zoo->exit);
    Model_Init(&zoo->buy_signal);
    zoo->loaded_mask = 0;
    zoo->primary_handle = nullptr;
    zoo->primary_target_class = 0;
    zoo->primary_role_name[0] = '\0';
}

//======================================================================================================
// try to load a single model from <dir>/<role>.json, falling back to <role>.xgb
// returns 1 if loaded, 0 if file not found or load failed
//======================================================================================================
// v5.2.0: held-out gate. When `secret` is non-null + `strict != 0`, refuse
// to load a model file that doesn't have a valid `.stamp` sibling. See
// `verify_model_stamp` in ModelInference.hpp.
//
// Default args preserve pre-v5.2.0 callers — no gate when secret==nullptr.
template <unsigned F>
inline int CoreModelZoo_TryLoadRole(ModelHandle<F> *handle, const char *dir,
                                    const char *role_name, int backend,
                                    const char* held_out_stamp_secret = nullptr,
                                    double gap_threshold = 0.05,
                                    int held_out_gate_strict = 0,
                                    // v5.9.4 — operator opt-in: suppress
                                    // minor-version drift WARN. Cross-major
                                    // is still always refused/warned per
                                    // ModelStampResult.cross_major_engine.
                                    int acknowledge_cross_binary_drift = 0,
                                    // v5.11.18 main — runtime cfg's per-core
                                    // feature_mask. Default 0 = skip check
                                    // (v5.11.18a infrastructure default; legacy
                                    // path stays bytewise-identical). When
                                    // non-zero, verify_model_stamp refuses
                                    // load if stamp's feature_mask_train
                                    // doesn't match this value.
                                    uint64_t expected_feature_mask = 0,
                                    // v5.11.42 D.2 — expected horizon ticks
                                    // (parsed from dir name `_horizon_<N>` by
                                    // EnsembleModelZoo_LoadFromCfg). Default 0
                                    // = skip check (single-horizon load path).
                                    // When non-zero AND stamp has label_params,
                                    // post-load REFUSE if stamp's
                                    // label_lookahead_ticks differs from this
                                    // value (catches dir rename / copy mistake).
                                    int expected_horizon_ticks = 0,
                                    // v5.14.1.B.3 — cfg pointer for X-macro
                                    // drift check (Ridge + composite cfg
                                    // fields stamped via FOREACH_STAMP_BOUND_CFG).
                                    // Default nullptr = skip drift check (legacy
                                    // callers + tests). When non-null, post-
                                    // verify_model_stamp expansion compares
                                    // sr.<name> vs cfg->get_cfg_expr per
                                    // X-macro entry; mismatch → increments
                                    // sr.inference_cfg_drift_count + caller's
                                    // existing held_out_gate_strict gate decides
                                    // refuse-vs-warn.
                                    const ControllerConfig<F>* cfg_ptr = nullptr) {
    char path[512];
    struct stat st;
    const char* found_path = nullptr;

    // try .json first (modern XGBoost format, matches Training panel default)
    snprintf(path, sizeof(path), "%s/%s.json", dir, role_name);
    if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
        found_path = path;
    }

    // fall back to .xgb (older binary format)
    if (!found_path) {
        snprintf(path, sizeof(path), "%s/%s.xgb", dir, role_name);
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            found_path = path;
        }
    }

    // .txt for LightGBM
    if (!found_path && backend == MODEL_BACKEND_LIGHTGBM) {
        snprintf(path, sizeof(path), "%s/%s.txt", dir, role_name);
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            found_path = path;
        }
    }

    if (!found_path) return 0;

    // v5.2.0 held-out gate: verify stamp before loading. Skip in non-strict
    // modes (-1 = explicit skip, 0 = warn-only) to preserve back-compat
    // with un-stamped models. strict=1 = refuse load on any failure.
    //
    // v5.8.6: passes FEATURE_REGISTRY_HASH() through so the verifier can
    // catch train-serve feature-set drift (model trained against a
    // different FOREACH_FEATURE registry than the current build). Old
    // stamps without the hash field load with a stderr WARN — the
    // back-compat path in verify_model_stamp.
    ModelStampResult sr = {};
    int have_sr = 0;
    if (held_out_gate_strict != -1) {
        sr = verify_model_stamp(found_path,
            held_out_stamp_secret ? held_out_stamp_secret : "",
            gap_threshold,
            MODEL_FORMAT_VERSION,
            FEATURE_REGISTRY_HASH(),
            LABEL_REGISTRY_HASH(),  // v5.10.1.A — close Finding #1 consume side
            expected_feature_mask);  // v5.11.18 main — feature mask binding
        have_sr = 1;

        // v5.14.1.B.3 (PARITY-004 + PARITY-005) — X-macro drift check.
        // Compares stamp body's Ridge + composite cfg values vs current
        // cfg; increments sr.inference_cfg_drift_count per drift +
        // populates sr.reason on first drift. Resurrects v5.9.2b's
        // abandoned drift_count mechanism (partial — covers the 10
        // X-macro-registered fields; legacy v5.9.2b inference_cfg_*
        // fields gain coverage when v5.15+ migrates them into the
        // registry per CLEANUP-001).
        //
        // Surface G forward-compat: legacy stamps (pre-v5.14.1.B.3)
        // load with sr.has_<name>=0 → check skips silently. New stamps
        // with mismatched cfg → drift detected at load.
        //
        // cfg_ptr=nullptr (legacy callers + tests) → skip silently.
        // Local `cfg` reference inside the block lets the X-macro
        // entries use `cfg.field` syntax uniformly (matches the macro
        // contract documented in StampBoundCfgRegistry.hpp).
        if (cfg_ptr && sr.valid > 0) {
            const ControllerConfig<F>& cfg = *cfg_ptr;
            #define X(name, type, fmt, default_val, get_cfg_expr, emit_when)       \
                if (sr.has_##name) {                                                \
                    type cfg_val = (type)(get_cfg_expr);                            \
                    if (sr.name != cfg_val) {                                       \
                        sr.inference_cfg_drift_count++;                             \
                        if (sr.reason[0] == '\0') {                                 \
                            snprintf(sr.reason, sizeof(sr.reason),                  \
                                "%s drift: stamp=" fmt " cfg=" fmt,                 \
                                #name, sr.name, cfg_val);                           \
                        }                                                            \
                    }                                                                \
                }
            FOREACH_STAMP_BOUND_CFG(X)
            #undef X
            if (sr.inference_cfg_drift_count > 0) {
                sr.valid = 0;  // treat drift as verification failure
            }
        }

        if (sr.valid <= 0) {
            if (held_out_gate_strict == 1) {
                fprintf(stderr,
                    "[held-out gate] REFUSING to load %s — %s (strict mode)\n",
                    found_path, sr.reason);
                return 0;  // refuse
            }
            // warn-only: log and continue
            fprintf(stderr,
                "[held-out gate] WARN: %s — %s (strict=0, loading anyway)\n",
                found_path, sr.reason);
        } else {
            // v5.8.6: emit a single match-status line so operators can
            // see at-a-glance whether the loaded model agrees with the
            // current engine. Stamp's engine_version may be empty for
            // pre-v5.8.6 stamps; print "unknown" in that case.
            const char* stamp_eng = sr.engine_version[0] ? sr.engine_version : "unknown";
            fprintf(stderr,
                "[model] %s: trained_engine=%s registry=%016lx (current=%s/%016lx) — %s\n",
                found_path, stamp_eng,
                (unsigned long)sr.feature_registry_hash,
                ENGINE_VERSION_STRING,
                (unsigned long)FEATURE_REGISTRY_HASH(),
                sr.reason);
            // v5.9.4 — minor-version drift WARN. Same-major (cross_major=0)
            // but different minor (e.g. stamp=5.8.x, build=5.9.4) is usually
            // OK but worth surfacing so operator notices unintended deploys.
            // Patch-level drift (5.9.3a vs 5.9.3b) NOT warned — same minor.
            // Operator suppresses with cfg.acknowledge_cross_binary_version_drift=1.
            if (!sr.cross_major_engine && sr.engine_version[0] &&
                !acknowledge_cross_binary_drift) {
                int sm = 0, sn = 0;
                sscanf(sr.engine_version, "%d.%d", &sm, &sn);
                int cm = 0, cn = 0;
                sscanf(ENGINE_VERSION_STRING, "%d.%d", &cm, &cn);
                if (sm == cm && sn != cn) {
                    fprintf(stderr,
                        "[engine_version] WARN: %s stamp's engine_version=%s "
                        "differs from build %s by minor. Set "
                        "acknowledge_cross_binary_version_drift=1 to suppress.\n",
                        found_path, sr.engine_version, ENGINE_VERSION_STRING);
                }
            }
        }
    }

    int rc = Model_Load(handle, found_path, backend);
    if (rc <= 0) return rc;

    // v5.9.4a — copy stamp-derived fields onto the handle for engine
    // boot to surface (Phase 6 poll_interval WARN; future drift checks).
    // Only copies when stamp had the field; preserves Model_Init zero
    // defaults for legacy stamps.
    if (have_sr) {
        if (sr.has_training_poll_interval) {
            handle->training_poll_interval = sr.training_poll_interval;
            handle->has_training_poll_interval = 1;
        }
        // v5.9.5h — copy XGBoost hyperparams from stamp onto handle.
        // EngineSharded boot-WARN compares stamp_xgb_* vs cfg.xgb_*
        // (mirrors v5.9.4a poll_interval pattern). No refusal — hyperparams
        // don't affect inference, only forensics + reproducibility.
        if (sr.has_xgb_hyperparams) {
            handle->has_xgb_hyperparams        = 1;
            handle->stamp_xgb_max_depth        = sr.xgb_max_depth;
            handle->stamp_xgb_learning_rate    = sr.xgb_learning_rate;
            handle->stamp_xgb_n_estimators     = sr.xgb_n_estimators;
            handle->stamp_xgb_subsample        = sr.xgb_subsample;
            handle->stamp_xgb_colsample_bytree = sr.xgb_colsample_bytree;
            handle->stamp_xgb_min_child_weight = sr.xgb_min_child_weight;
            handle->stamp_xgb_seed             = sr.xgb_seed;
            size_t tmln = strnlen(sr.xgb_tree_method,
                                   sizeof(handle->stamp_xgb_tree_method) - 1);
            memcpy(handle->stamp_xgb_tree_method, sr.xgb_tree_method, tmln);
            handle->stamp_xgb_tree_method[tmln] = '\0';
        }
        // v5.9.5h Phase 10 — build flags fingerprint
        if (sr.has_build_flags_hash) {
            handle->has_build_flags_hash   = 1;
            handle->stamp_build_flags_hash = sr.build_flags_hash;
        }
        // v5.11.42 D.1 — copy stamp's xgb_train_nthread for engine boot WARN.
        if (sr.has_xgb_train_nthread) {
            handle->has_stamp_xgb_train_nthread = 1;
            handle->stamp_xgb_train_nthread     = sr.xgb_train_nthread;
        }
        // v5.11.42 D.2 — copy stamp's label params for ensemble dir-name
        // horizon-mismatch refusal at AutoDetect time.
        if (sr.has_label_params) {
            handle->has_stamp_label_params  = 1;
            handle->stamp_label_lookahead_ticks = sr.label_lookahead_ticks;
            handle->stamp_label_tp_pct          = sr.label_tp_pct;
            handle->stamp_label_sl_pct          = sr.label_sl_pct;
        }
        // v5.11.42 D.3 — copy stamp's scaler_sha256 for ensemble-sibling
        // consistency WARN.
        if (sr.has_scaler_fields && sr.scaler_sha256[0] != '\0') {
            handle->has_stamp_scaler_sha256 = 1;
            size_t n = strnlen(sr.scaler_sha256,
                               sizeof(handle->stamp_scaler_sha256) - 1);
            memcpy(handle->stamp_scaler_sha256, sr.scaler_sha256, n);
            handle->stamp_scaler_sha256[n] = '\0';
        }
        // v5.9.5i — copy stamp's inference cfg values. EngineSharded
        // boot-WARN/REFUSE compares vs cfg.*. Forward-compat: legacy
        // stamps (has_inference_cfg=0) leave handle's stamp_inf_* at
        // Model_Init zero defaults; comparison skipped.
        if (sr.has_inference_cfg) {
            handle->has_stamp_inference_cfg = 1;
            handle->stamp_inf_confidence_threshold_scale =
                sr.inference_cfg_confidence_threshold_scale;
            handle->stamp_inf_barrier_gate_enabled =
                sr.inference_cfg_barrier_gate_enabled;
            handle->stamp_inf_confidence_hard_block_threshold =
                sr.inference_cfg_confidence_hard_block_threshold;
            handle->stamp_inf_freshness_tau =
                sr.inference_cfg_freshness_tau;
        }
        if (sr.has_inference_cfg_bandit) {
            handle->has_stamp_bandit = 1;
            handle->stamp_inf_bandit_blend_ratio =
                sr.inference_cfg_bandit_blend_ratio;
        }
        if (sr.has_inference_cfg_fees) {
            handle->has_stamp_fees = 1;
            handle->stamp_inf_fee_rate_maker =
                sr.inference_cfg_fee_rate_maker;
            handle->stamp_inf_fee_rate_taker =
                sr.inference_cfg_fee_rate_taker;
        }
        if (sr.has_model_num_outputs) {
            handle->stamp_model_num_outputs = sr.model_num_outputs;
            handle->has_stamp_num_outputs = 1;
            // Phase 5 — verify stamp's claim matches Model_Load's seen
            // num_outputs. Mismatch = stamp tampered with OR XGBoost
            // loaded a different model than the trainer wrote. Refuse
            // in strict mode; warn otherwise.
            if (sr.model_num_outputs != handle->num_outputs) {
                if (held_out_gate_strict == 1) {
                    fprintf(stderr,
                        "[model] REFUSING %s — stamp claims model_num_outputs=%d "
                        "but Model_Load saw num_outputs=%d (strict mode)\n",
                        found_path, sr.model_num_outputs, handle->num_outputs);
                    Model_Free(handle);
                    Model_Init(handle);
                    return 0;
                }
                fprintf(stderr,
                    "[model] WARN: %s stamp claims model_num_outputs=%d but "
                    "loaded model has num_outputs=%d (strict=0, loading anyway)\n",
                    found_path, sr.model_num_outputs, handle->num_outputs);
            }
        }
        // v5.11.42 D.2 — horizon-mismatch refusal at ensemble load.
        // EnsembleModelZoo_LoadFromCfg parses horizon_ticks from dir
        // name `_horizon_<N>` and passes it as expected_horizon_ticks.
        // Stamp's label_lookahead_ticks must match. Catches: dir
        // rename, copy-paste mistake, two horizons accidentally swapped
        // between dirs. ALWAYS refuses on mismatch (no strict-mode
        // gating) since the model definitely shouldn't be loaded under
        // a horizon it wasn't trained for. Legacy stamps without
        // label_params (has_*=0) skip the check.
        if (expected_horizon_ticks > 0 && sr.has_label_params &&
            sr.label_lookahead_ticks != expected_horizon_ticks) {
            fprintf(stderr,
                "[model] REFUSING %s — stamp claims label_lookahead_ticks=%d "
                "but loaded from dir expecting horizon=%d (dir rename or "
                "copy-paste mistake?)\n",
                found_path, sr.label_lookahead_ticks, expected_horizon_ticks);
            Model_Free(handle);
            Model_Init(handle);
            return 0;
        }
    }

    // v5.9.3a — scaler sidecar load. Stamp claimed scaler present? Try
    // to load and verify <model>.scaler. 3-tier behavior on failure:
    //   strict=1: refuse model load (consistent with stamp drift refusal)
    //   strict=0: warn + set handle->scaler_load_failed=1, continue
    //             with identity scaler applied
    //   strict=-1: skip (no verification at all; same as today's policy)
    if (have_sr && sr.feature_scaler_present && held_out_gate_strict != -1) {
        char scaler_path[600];
        snprintf(scaler_path, sizeof(scaler_path), "%s.scaler", found_path);

        // Step 1: SHA-256 of the on-disk file matches stamp's claim.
        char actual_sha[80] = {0};
        int sha_ok = tt::sha256_file_hex_inproc(scaler_path, actual_sha, sizeof(actual_sha));
        int sha_match = (sha_ok && sr.scaler_sha256[0] != '\0' &&
                         strcmp(actual_sha, sr.scaler_sha256) == 0);

        // Step 2: load + parse the binary.
        int load_rc = sha_match ? tt::FeatureStandardizer_Load(&handle->scaler, scaler_path) : -1;

        // Step 3: registry hash + num_features match build.
        int verify_ok = (load_rc == 1) &&
                        tt::FeatureStandardizer_VerifyAgainstBuild(&handle->scaler);

        if (!verify_ok) {
            const char* why = !sha_ok           ? "sidecar missing or unreadable"
                            : !sha_match        ? "sidecar SHA-256 mismatch with stamp"
                            : load_rc == 0      ? "sidecar parse failed"
                            : load_rc == -1     ? "sidecar magic/format invalid"
                            :                     "registry_hash or num_features mismatch";
            if (held_out_gate_strict == 1) {
                fprintf(stderr,
                    "[scaler] REFUSING to load %s — %s (strict mode)\n",
                    scaler_path, why);
                tt::FeatureStandardizer_Free(&handle->scaler);
                handle->scaler_load_failed = 1;
                return 0;
            }
            // warn-mode: identity applied, surface to operator via PerCoreSnap
            fprintf(stderr,
                "[CRITICAL] scaler load failed (reason=%s) but engine continuing "
                "with identity (held_out_gate_strict=0). Predictions WILL drift "
                "from training distribution. Set strict=1 in cfg to refuse.\n", why);
            tt::FeatureStandardizer_Free(&handle->scaler);
            handle->scaler_load_failed = 1;
        } else {
            fprintf(stderr,
                "[scaler] %s: loaded (registry_hash=%016lx, num_features=%u)\n",
                scaler_path,
                (unsigned long)handle->scaler.registry_hash,
                (unsigned)handle->scaler.num_features);
        }
    }

    return rc;
}

//======================================================================================================
// auto-discover and load all roles present in `dir`. missing roles silently
// disabled. returns the number of roles loaded.
//======================================================================================================
template <unsigned F>
inline int CoreModelZoo_LoadFromDir(CoreModelZoo<F> *zoo, const char *dir, int backend,
                                     const char* held_out_stamp_secret = nullptr,
                                     double gap_threshold = 0.05,
                                     int held_out_gate_strict = 0,
                                     // v5.9.4 — operator opt-in, threaded
                                     // through to per-role load.
                                     int acknowledge_cross_binary_drift = 0,
                                     // v5.11.18 main — runtime cfg's
                                     // per-core feature_mask, threaded to
                                     // each role's verify_model_stamp call.
                                     // Default 0 = skip mask check.
                                     uint64_t expected_feature_mask = 0,
                                     // v5.14.1.B.3 — cfg pointer for X-macro
                                     // drift check (Ridge + composite cfg
                                     // stamp-bound fields). Default nullptr
                                     // = skip drift check (legacy callers,
                                     // tests). When non-null, threaded
                                     // through to each TryLoadRole call.
                                     const ControllerConfig<F>* cfg_ptr = nullptr) {
    if (!dir || dir[0] == '\0') return 0;

    int loaded = 0;
    if (CoreModelZoo_TryLoadRole(&zoo->barrier, dir, "barrier", backend,
            held_out_stamp_secret, gap_threshold, held_out_gate_strict,
            acknowledge_cross_binary_drift, expected_feature_mask,
            /*expected_horizon_ticks=*/0, cfg_ptr)) {
        zoo->loaded_mask |= CORE_MODEL_BARRIER;
        loaded++;
    }
    if (CoreModelZoo_TryLoadRole(&zoo->regime, dir, "regime", backend,
            held_out_stamp_secret, gap_threshold, held_out_gate_strict,
            acknowledge_cross_binary_drift, expected_feature_mask,
            /*expected_horizon_ticks=*/0, cfg_ptr)) {
        zoo->loaded_mask |= CORE_MODEL_REGIME;
        loaded++;
    }
    if (CoreModelZoo_TryLoadRole(&zoo->exit, dir, "exit", backend,
            held_out_stamp_secret, gap_threshold, held_out_gate_strict,
            acknowledge_cross_binary_drift, expected_feature_mask,
            /*expected_horizon_ticks=*/0, cfg_ptr)) {
        zoo->loaded_mask |= CORE_MODEL_EXIT;
        loaded++;
    }
    if (CoreModelZoo_TryLoadRole(&zoo->buy_signal, dir, "buy_signal", backend,
            held_out_stamp_secret, gap_threshold, held_out_gate_strict,
            acknowledge_cross_binary_drift, expected_feature_mask,
            /*expected_horizon_ticks=*/0, cfg_ptr)) {
        zoo->loaded_mask |= CORE_MODEL_BUY_SIGNAL;
        loaded++;
    }

    // v5.11.62 — primary-role indirection. Strategy code reads
    // zoo->primary_handle (single ModelHandle*) instead of one specific
    // role slot. Loader picks the first available role in priority
    // order: buy_signal > barrier > regime. For multiclass barrier
    // (PEAK_VALLEY_STABLE 3-class), buy_class_idx is set on the handle
    // so Model_Predict returns P(peak) as buy probability.
    zoo->primary_handle = nullptr;
    zoo->primary_target_class = 0;
    zoo->primary_role_name[0] = '\0';
    if (zoo->loaded_mask & CORE_MODEL_BUY_SIGNAL) {
        zoo->primary_handle = &zoo->buy_signal;
        zoo->buy_signal.buy_class_idx = 0;
        strncpy(zoo->primary_role_name, "buy_signal",
                sizeof(zoo->primary_role_name) - 1);
    } else if (zoo->loaded_mask & CORE_MODEL_BARRIER) {
        zoo->primary_handle = &zoo->barrier;
        zoo->barrier.buy_class_idx = (zoo->barrier.num_outputs >= 2) ? 1 : 0;
        zoo->primary_target_class = zoo->barrier.buy_class_idx;
        strncpy(zoo->primary_role_name, "barrier",
                sizeof(zoo->primary_role_name) - 1);
    } else if (zoo->loaded_mask & CORE_MODEL_REGIME) {
        zoo->primary_handle = &zoo->regime;
        zoo->regime.buy_class_idx = 0;  // operator opts in via cfg if 3-class regime
        strncpy(zoo->primary_role_name, "regime",
                sizeof(zoo->primary_role_name) - 1);
    }
    zoo->primary_role_name[sizeof(zoo->primary_role_name) - 1] = '\0';

    fprintf(stderr, "[ML] zoo loaded %d role(s) from %s (mask=0x%x); primary=%s "
                    "(class=%d)\n",
            loaded, dir, zoo->loaded_mask,
            zoo->primary_role_name[0] ? zoo->primary_role_name : "(none)",
            zoo->primary_target_class);
    return loaded;
}

//======================================================================================================
// legacy single-model fallback: load just the buy_signal role from a single
// path (backward compat with the old core_N_model_path config field).
//======================================================================================================
template <unsigned F>
inline int CoreModelZoo_LoadLegacy(CoreModelZoo<F> *zoo, const char *path, int backend) {
    if (!path || path[0] == '\0') return 0;
    if (Model_Load(&zoo->buy_signal, path, backend)) {
        zoo->loaded_mask |= CORE_MODEL_BUY_SIGNAL;
        return 1;
    }
    return 0;
}

//======================================================================================================
template <unsigned F>
inline void CoreModelZoo_Free(CoreModelZoo<F> *zoo) {
    Model_Free(&zoo->barrier);
    Model_Free(&zoo->regime);
    Model_Free(&zoo->exit);
    Model_Free(&zoo->buy_signal);
    zoo->loaded_mask = 0;
}

//======================================================================================================
template <unsigned F>
inline int CoreModelZoo_HasAny(const CoreModelZoo<F> *zoo) {
    return zoo->loaded_mask != 0;
}

//======================================================================================================
// [STUPID-PROOF VERIFY]
//======================================================================================================
// reads <dir>/expected.cfg (written by foxml_suite Save Run) and verifies
// the live ML config matches what the model was trained against. mismatches
// are logged as warnings; if strict_mode is set, returns 0 to fail load.
//
// returns:
//   1 = no expected.cfg present (silent pass — backward compat with old runs)
//   1 = expected.cfg present and all fields match
//   1 = expected.cfg present, mismatches exist, strict_mode=0 (warn but ok)
//   0 = expected.cfg present, mismatches exist, strict_mode=1 (fail load)
//
// also runs a structural check — if any model in the zoo has 3+ outputs
// (multiclass softmax) but barrier_gate_enabled=0, warn that the engine
// will only use one class and the model is being underutilized.
//======================================================================================================
template <unsigned F>
// v4.3.1 — extended signature to also verify slow-path cadence + feature
// pack version. live_poll_interval and live_feature_format_version are
// the engine's runtime values; the loader compares them against what
// expected.cfg recorded at training time. Mismatch on cadence = silent
// train-serve drift; mismatch on feature format = wrong number of
// features in the pack, model crashes or produces garbage.
inline int CoreModelZoo_VerifyExpected(const CoreModelZoo<F> *zoo, const char *dir,
                                       int live_barrier_gate_enabled,
                                       double live_ml_buy_threshold,
                                       int strict_mode, int core_id,
                                       unsigned live_poll_interval = 0,
                                       unsigned live_feature_format_version = 0) {
    // structural check: multiclass model + barrier_gate_enabled=0 → warn
    int has_multiclass = (zoo->loaded_mask & CORE_MODEL_BARRIER) && zoo->barrier.num_outputs >= 2;
    if (has_multiclass && !live_barrier_gate_enabled) {
        fprintf(stderr, "[ML] core %d: WARNING — model has %d output classes (multiclass softmax)\n"
                        "                  but barrier_gate_enabled=0. only P(valley) used,\n"
                        "                  P(peak)/P(stable) ignored. set barrier_gate_enabled=1\n"
                        "                  to use the full model.\n",
                core_id, zoo->barrier.num_outputs);
    }

    // read expected.cfg if present
    if (!dir || dir[0] == '\0') return 1;
    char path[512];
    snprintf(path, sizeof(path), "%s/expected.cfg", dir);
    FILE *f = fopen(path, "r");
    if (!f) {
        // no expected.cfg = old run bundle, silent pass for backward compat
        return 1;
    }

    int expected_barrier_gate = -1;       // -1 = not specified in file
    double expected_threshold = -1.0;
    int expected_num_classes = -1;
    char expected_role[64] = "";
    // Phase 7 prep — informational (logged, not mismatch-checked against live
    // cfg). Discipline values the model was trained under. -1 = not in file.
    double expected_held_out_fraction = -1.0;
    double expected_gap_threshold     = -1.0;
    // v4.3.1 — train-serve cadence + feature pack version (-1 = old format)
    int expected_poll_interval        = -1;
    int expected_feature_format_ver   = -1;
    int expected_num_features         = -1;
    int mismatches = 0;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        // strip leading whitespace + skip comments + blank
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;
        // split on '='
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = p;
        char *val = eq + 1;
        // trim trailing whitespace on key
        char *kend = key + strlen(key) - 1;
        while (kend > key && (*kend == ' ' || *kend == '\t')) { *kend-- = '\0'; }
        // trim leading whitespace on val
        while (*val == ' ' || *val == '\t') val++;
        // strip trailing newline + comment from val
        char *vend = val + strcspn(val, "\r\n#");
        *vend = '\0';
        // trim trailing whitespace on val
        while (vend > val && (*(vend-1) == ' ' || *(vend-1) == '\t')) { *(--vend) = '\0'; }

        if (strcmp(key, "barrier_gate_enabled") == 0)        expected_barrier_gate = atoi(val);
        else if (strcmp(key, "ml_buy_threshold") == 0)       expected_threshold = tt::parse_double_fast(val);
        else if (strcmp(key, "expected_num_classes") == 0)   expected_num_classes = atoi(val);
        else if (strcmp(key, "expected_role") == 0) {
            strncpy(expected_role, val, sizeof(expected_role) - 1);
            expected_role[sizeof(expected_role) - 1] = '\0';
        }
        else if (strcmp(key, "held_out_fraction") == 0)        expected_held_out_fraction = tt::parse_double_fast(val);
        else if (strcmp(key, "gap_acceptable_threshold") == 0) expected_gap_threshold     = tt::parse_double_fast(val);
        else if (strcmp(key, "expected_poll_interval") == 0)         expected_poll_interval = atoi(val);
        else if (strcmp(key, "expected_feature_format_version") == 0) expected_feature_format_ver = atoi(val);
        else if (strcmp(key, "expected_num_features") == 0)          expected_num_features = atoi(val);
    }
    fclose(f);

    // v4.3.1 — slow-path cadence mismatch is silent train-serve drift.
    // Model was trained at training-cadence; serving at sharded-cadence.
    // If they differ, all RollingStats-derived features (slope, R², etc.)
    // describe different time windows than the model expects. Always warn.
    if (expected_poll_interval > 0 && live_poll_interval > 0 &&
        (unsigned)expected_poll_interval != live_poll_interval) {
        fprintf(stderr,
            "[ML] core %d: MISMATCH — model trained at poll_interval=%d, "
            "engine running at poll_interval=%u\n"
            "                  RollingStats time-windows differ %.1f×; "
            "predictions will diverge from training distribution.\n"
            "                  Set engine.cfg poll_interval=%d to match.\n",
            core_id, expected_poll_interval, live_poll_interval,
            (double)live_poll_interval / (double)expected_poll_interval,
            expected_poll_interval);
        mismatches++;
    }
    // v4.3 — feature format version mismatch = pack contents differ.
    // FEAT_* indices change → the model interprets feature N as something
    // it wasn't trained on. Hard fail.
    if (expected_feature_format_ver > 0 && live_feature_format_version > 0 &&
        (unsigned)expected_feature_format_ver != live_feature_format_version) {
        fprintf(stderr,
            "[ML] core %d: FATAL — model trained with feature_format=v%d "
            "but engine runtime is v%u. Feature indices differ; model "
            "would interpret inputs as wrong features.\n"
            "                  Retrain the model on the current engine.\n",
            core_id, expected_feature_format_ver, live_feature_format_version);
        mismatches++;
    }

    // compare each field, log mismatches
    if (expected_barrier_gate >= 0 && expected_barrier_gate != live_barrier_gate_enabled) {
        fprintf(stderr, "[ML] core %d: MISMATCH — expected.cfg says barrier_gate_enabled=%d, "
                        "engine.cfg has %d\n",
                core_id, expected_barrier_gate, live_barrier_gate_enabled);
        mismatches++;
    }
    if (expected_threshold >= 0.0 &&
        (live_ml_buy_threshold < expected_threshold - 0.001 ||
         live_ml_buy_threshold > expected_threshold + 0.001)) {
        fprintf(stderr, "[ML] core %d: MISMATCH — expected.cfg says ml_buy_threshold=%.3f, "
                        "engine.cfg has %.3f\n",
                core_id, expected_threshold, live_ml_buy_threshold);
        mismatches++;
    }
    if (expected_num_classes >= 2 && (zoo->loaded_mask & CORE_MODEL_BARRIER) &&
        zoo->barrier.num_outputs != expected_num_classes) {
        fprintf(stderr, "[ML] core %d: MISMATCH — expected.cfg says %d classes, "
                        "loaded model has %d outputs\n",
                core_id, expected_num_classes, zoo->barrier.num_outputs);
        mismatches++;
    }

    // Phase 7 prep — log discipline values informationally so the user knows
    // what validation regime the model was trained under. Not compared to
    // live cfg (yet); add comparison if drift becomes a real concern.
    if (expected_held_out_fraction >= 0.0 || expected_gap_threshold >= 0.0) {
        fprintf(stderr, "[ML] core %d: validation discipline — held_out=%.2f gap_threshold=%.3f\n",
                core_id,
                expected_held_out_fraction >= 0.0 ? expected_held_out_fraction : 0.0,
                expected_gap_threshold     >= 0.0 ? expected_gap_threshold     : 0.0);
    }

    if (mismatches == 0) {
        fprintf(stderr, "[ML] core %d: expected.cfg verified (role=%s, %d classes) ✓\n",
                core_id, expected_role[0] ? expected_role : "?",
                expected_num_classes >= 0 ? expected_num_classes : 0);
        return 1;
    }

    if (strict_mode > 0) {
        fprintf(stderr, "[ML] core %d: %d MISMATCH(ES) — STRICT MODE refusing to load.\n"
                        "                update engine.cfg to match expected.cfg, or set\n"
                        "                model_verify_strict=0 to override.\n",
                core_id, mismatches);
        return 0;
    } else {
        fprintf(stderr, "[ML] core %d: %d mismatch(es) — model may not behave as trained.\n"
                        "                fix engine.cfg to silence these warnings.\n",
                core_id, mismatches);
        return 1;
    }
}

//======================================================================================================
// [v5.10.0a.G.3 — ENSEMBLE MODEL ZOO (multi-horizon sidecar struct)]
//======================================================================================================
// EnsembleModelZoo lives ALONGSIDE CoreModelZoo (not replacing it).
// Single-horizon callers use CoreModelZoo unchanged; multi-horizon
// callers populate EnsembleModelZoo when cfg.horizon_list non-empty.
//
// G.4 inference path: at per-tick predict, if ensemble->active, iterate
// loaded horizons + select highest-confidence prediction; else fall
// through to single-zoo (existing path).
//
// Storage shape: 4 roles × N horizons (HORIZON_LIST_MAX=8). Memory
// upper bound: 4 × 8 × ~5-50MB per ModelHandle = up to ~1.6GB per core.
// Operator opt-in via cfg.horizon_list; default empty = no extra memory.

// Mirror ControllerConfig::HORIZON_LIST_MAX. Avoids template instantiation
// circular dep; the value is small enough to hardcode.
#define ENSEMBLE_HORIZON_MAX 8

template <unsigned F>
struct EnsembleModelZoo {
    ModelHandle<F> barrier[ENSEMBLE_HORIZON_MAX];
    ModelHandle<F> regime[ENSEMBLE_HORIZON_MAX];
    ModelHandle<F> exit_predictor[ENSEMBLE_HORIZON_MAX];
    ModelHandle<F> buy_signal[ENSEMBLE_HORIZON_MAX];
    int barrier_count;
    int regime_count;
    int exit_predictor_count;
    int buy_signal_count;
    // Per-member horizon ticks (e.g. {100, 500, 1000} → ezoo populated
    // at indices 0..2 with horizon_ticks_at_idx[0..2] = {100, 500, 1000}).
    int horizon_ticks_at_idx[ENSEMBLE_HORIZON_MAX];
    int active;  // 0 = use single-zoo (existing); 1 = ensemble path
    // v5.10.0a.G.7 — per-regime bandit state (NUM_REGIMES from
    // FOREACH_REGIME X-macro). Each bandit has N arms = ezoo->buy_signal_count.
    // Cold start: uniform weights; G.8 reward path updates them per outcome.
    BanditState bandits[NUM_REGIMES];
    int initialized_bandits;       // 0 = bandits not yet wired (Init phase only)
    // Per-prediction tracking (G.7 + G.8 reward attribution)
    int last_predicted_regime_id;  // regime AT predict-time (NOT current; for G.8 attribution)
    int last_predicted_horizon_idx;// dominant horizon idx (display + G.8 reward)
    // v5.13.4 — sell-side bandit (parallel to buy-side). Same per-regime
    // shape; arms count = exit_predictor_count. Cold start: uniform; G.8-
    // style reward update fires from HandleFill exit branch when
    // last_exit_was_predicted[slot] && cfg.exit_bandit_enabled && NOT
    // flatten event. Counterfactual reward formula: actual_pnl_bps -
    // hypothetical_held_to_TP_pnl_bps (optimistic; biases against exits;
    // operator scales via cfg.exit_bandit_lr; refined post paper-test).
    BanditState exit_bandits[NUM_REGIMES];
    int initialized_exit_bandits;       // 0 = no exit models loaded; gates dispatch
    int last_predicted_exit_horizon_idx;// dominant exit_predictor arm at predict time

    // v5.14.0 — Ridge risk-parity blending state. Computed per slow-path
    // cycle when cfg.ridge_within_horizon=1 (default 0; preserves bandit
    // path bytewise). Reads ezoo->reward_ring's recent prediction history
    // to build N×N correlation matrix; Cholesky-solves Markowitz-style
    // optimal weights; falls back to uniform on singular Σ.
    //
    // Cache impact: ~5KB struct per ezoo. Slow-path single-writer + reader
    // on its own per-core ezoo; no false sharing. NOT in hot-path read set.
    //
    // Default: zero-init via RidgeWeights_Init in _Init below. Identity
    // correlation matrix = orthogonal models = Cholesky succeeds with
    // diagonal-only Σ. fallback_to_uniform stays 0 until the first
    // _Compute call sees a singular Σ.
    RidgeWeights<F> ridge_state;
    // v5.14.1.E — exit-side Ridge state. Mirrors ridge_state for the
    // exit_predictor handle array. Populated when cfg.exit_blender_mode=1
    // by mirroring the v5.14.0 buy-side ridge_within_horizon path against
    // exit_predictor[0..exit_predictor_count) instead of buy_signal[].
    // Default: zero-init via RidgeWeights_Init in _Init below.
    RidgeWeights<F> exit_ridge_state;
    char blend_mode[16];           // "weighted" or "selection" (cached from cfg)
    // v5.10.0a.G.7 — kill-switch bitmask. Bit i set = horizon i disabled
    // (skip predict + freeze its bandit weight). Set by parsing cfg's
    // core_N_disabled_horizons CSV at boot via _SetDisabledHorizons.
    uint32_t disabled_horizon_mask;
    // v5.10.0a.G.7 — regime hysteresis dampening. When current_regime
    // changes, blend OLD regime's weights with NEW for hysteresis cycles.
    int regime_transition_cycles_remaining;  // 0 = stable
    int prev_regime_id;            // regime BEFORE the transition
    // v5.10.0a.G.8 — reward attribution ring buffer. Each predict writes
    // a record (tick_index, regime_id, per-arm predictions, sample_price);
    // slow-path lookback walks ring → for old-enough records, computes
    // per-arm reward (direction match) → calls Bandit_Update.
    static constexpr int REWARD_RING_SIZE = 256;
    struct PredictionRecord {
        uint64_t predict_call;        // monotonic counter (increments per predict)
        int      regime_id;           // regime AT predict time (for attribution)
        float    predictions[ENSEMBLE_HORIZON_MAX];  // per-arm raw outputs
        float    sample_price;        // price at predict time
        uint8_t  rewarded_lookback;   // 1 = already rewarded by slow-path lookback
        uint8_t  rewarded_trade;      // 1 = already rewarded by trade-close
    };
    PredictionRecord reward_ring[REWARD_RING_SIZE];
    int reward_ring_head;             // next write slot
    uint64_t predict_call_count;      // monotonic predict counter (sets record.predict_call)
    // v5.14.1.E — exit-side prediction history (parallel to reward_ring).
    // Populated per-cycle from exit_predictor[i] predictions at the exit
    // prediction site. Used by Ridge solver when cfg.exit_blender_mode=1
    // to compute correlation matrix across exit handles. Default 0-init.
    PredictionRecord exit_reward_ring[REWARD_RING_SIZE];
    int exit_reward_ring_head;
    uint64_t exit_predict_call_count;
    // v5.10.0a.G.8 — drift watchdog (perf #3). Per-arm rolling IC tracker;
    // when IC drops below cfg.confidence_ic_floor, demote weight to ~0
    // across all regimes (manual override of bandit's natural learning).
    static constexpr int DRIFT_IC_HISTORY = 100;
    struct PerArmDrift {
        float    ic_history[DRIFT_IC_HISTORY];  // recent reward outcomes (1=correct, -1=wrong, 0=skip)
        int      ic_count;                       // populated entries (capped at DRIFT_IC_HISTORY)
        float    ic_avg;                         // running average over ic_history
        uint8_t  demoted;                        // 1 = forced near-zero weight; sticky until recovery
    };
    PerArmDrift drift[ENSEMBLE_HORIZON_MAX];
    // v5.10.0a.G.9 — bandit state persistence. base_dir is captured at
    // AutoDetectFromDir / LoadFromCfg time so the periodic save trigger
    // doesn't need ControllerConfig visibility from the bandit-update
    // helpers. Empty path = persistence disabled (no save attempted).
    char     bandit_save_path[400];     // <core_model_dir>/bandit_state.json
    int      bandit_save_interval;      // 0 = no periodic save (shutdown only)
    uint64_t bandit_update_count;       // monotonic; modulo'd against interval
    // v5.11.62 — primary-role indirection (mirrors CoreModelZoo). Strategy
    // + bandit code reads ezoo->primary_handles[0..primary_count) instead
    // of ezoo->buy_signal directly. Loader picks role at end of LoadFromCfg
    // based on which slots are populated, priority: buy_signal > barrier >
    // regime. nullptr = no primary role available.
    ModelHandle<F> *primary_handles;        // points into one of {buy_signal, barrier, regime}
    int             primary_count;          // mirrors *_count of chosen role
    int             primary_target_class;   // class index for buy probability extraction
    char            primary_role_name[16];  // "buy_signal" | "barrier" | "regime" | ""
};

template <unsigned F>
inline void EnsembleModelZoo_Init(EnsembleModelZoo<F> *ezoo) {
    for (int i = 0; i < ENSEMBLE_HORIZON_MAX; ++i) {
        Model_Init(&ezoo->barrier[i]);
        Model_Init(&ezoo->regime[i]);
        Model_Init(&ezoo->exit_predictor[i]);
        Model_Init(&ezoo->buy_signal[i]);
        ezoo->horizon_ticks_at_idx[i] = 0;
    }
    ezoo->barrier_count = 0;
    ezoo->regime_count = 0;
    ezoo->exit_predictor_count = 0;
    ezoo->buy_signal_count = 0;
    ezoo->active = 0;
    // v5.10.0a.G.7 — bandit state zero-init (full bandit init happens in
    // _InitBandits AFTER LoadFromCfg / AutoDetect populates buy_signal_count
    // so we know how many arms).
    memset(ezoo->bandits, 0, sizeof(ezoo->bandits));
    ezoo->initialized_bandits = 0;
    // v5.13.4 — exit-side bandit zero-init (full init in _InitExitBandits
    // AFTER LoadFromCfg populates exit_predictor_count).
    memset(ezoo->exit_bandits, 0, sizeof(ezoo->exit_bandits));
    ezoo->initialized_exit_bandits        = 0;
    ezoo->last_predicted_exit_horizon_idx = -1;
    // v5.14.0 — Ridge state zero-init. Identity Σ + zero μ/L/y/w/output
    // weights. Cholesky succeeds out-of-box on identity Σ regularized
    // by ridge λ; no per-core wiring needed beyond cfg flag check at
    // dispatch site (StrategyParameters.hpp ML_BuildParameters).
    RidgeWeights_Init(&ezoo->ridge_state);
    // v5.14.1.E — symmetric init for exit-side Ridge state.
    RidgeWeights_Init(&ezoo->exit_ridge_state);
    ezoo->last_predicted_regime_id = 0;
    ezoo->last_predicted_horizon_idx = -1;
    strncpy(ezoo->blend_mode, "weighted", sizeof(ezoo->blend_mode) - 1);
    ezoo->blend_mode[sizeof(ezoo->blend_mode) - 1] = '\0';
    ezoo->disabled_horizon_mask = 0;
    ezoo->regime_transition_cycles_remaining = 0;
    ezoo->prev_regime_id = 0;
    // v5.10.0a.G.8 — reward state init
    memset(ezoo->reward_ring, 0, sizeof(ezoo->reward_ring));
    ezoo->reward_ring_head = 0;
    // v5.14.1.E — symmetric init for exit-side prediction ring
    memset(ezoo->exit_reward_ring, 0, sizeof(ezoo->exit_reward_ring));
    ezoo->exit_reward_ring_head = 0;
    ezoo->exit_predict_call_count = 0;
    ezoo->predict_call_count = 0;
    memset(ezoo->drift, 0, sizeof(ezoo->drift));
    // v5.10.0a.G.9 — persistence config init (caller fills via _SetSavePath)
    ezoo->bandit_save_path[0] = '\0';
    ezoo->bandit_save_interval = 0;
    ezoo->bandit_update_count = 0;
    // v5.11.62 — primary-role indirection (set at end of LoadFromCfg /
    // AutoDetectFromDir; nullptr until a load populates it).
    ezoo->primary_handles = nullptr;
    ezoo->primary_count = 0;
    ezoo->primary_target_class = 0;
    ezoo->primary_role_name[0] = '\0';
}

// v5.11.62 — backstop helper: when callers (tests + ad-hoc paths)
// synthesize ezoo state by setting buy_signal_count directly without
// going through LoadFromCfg / AutoDetectFromDir, the primary_* fields
// stay zero. Bandit ops + ProcessPredictionRecord need primary_count
// to size their state. This helper auto-promotes buy_signal to primary
// when primary is unset and buy_signal is populated. Idempotent —
// post-loader callers that already set primary_handles bypass it.
template <unsigned F>
inline void EnsembleModelZoo_EnsurePrimary(EnsembleModelZoo<F>* ezoo) {
    if (!ezoo) return;
    if (ezoo->primary_handles || ezoo->primary_count > 0) return;
    if (ezoo->buy_signal_count > 0) {
        ezoo->primary_handles = ezoo->buy_signal;
        ezoo->primary_count = ezoo->buy_signal_count;
        ezoo->primary_target_class = 0;
        strncpy(ezoo->primary_role_name, "buy_signal",
                sizeof(ezoo->primary_role_name) - 1);
    } else if (ezoo->barrier_count > 0) {
        ezoo->primary_handles = ezoo->barrier;
        ezoo->primary_count = ezoo->barrier_count;
        ezoo->primary_target_class =
            (ezoo->barrier[0].num_outputs >= 2) ? 1 : 0;
        for (int i = 0; i < ezoo->barrier_count; ++i) {
            ezoo->barrier[i].buy_class_idx =
                (ezoo->barrier[i].num_outputs >= 2) ? 1 : 0;
        }
        strncpy(ezoo->primary_role_name, "barrier",
                sizeof(ezoo->primary_role_name) - 1);
    } else if (ezoo->regime_count > 0) {
        ezoo->primary_handles = ezoo->regime;
        ezoo->primary_count = ezoo->regime_count;
        strncpy(ezoo->primary_role_name, "regime",
                sizeof(ezoo->primary_role_name) - 1);
    }
    ezoo->primary_role_name[sizeof(ezoo->primary_role_name) - 1] = '\0';
}

//======================================================================================================
// [v5.10.0a.G.8 — REWARD RING WRITE]
//======================================================================================================
// Called from ML_BuildParameters after each predict. Writes per-arm
// predictions + regime + sample_price into the ring at head; head
// advances modulo RING_SIZE (oldest record overwritten).
//
// predict_call_count is the monotonic predict counter; record's
// predict_call field captures the value at write time. Slow-path
// lookback uses (current_predict_call - record.predict_call) ≥
// (forward_ticks / poll_interval) to decide if record is old enough
// to reward.
template <unsigned F>
inline void EnsembleModelZoo_RecordPrediction(EnsembleModelZoo<F>* ezoo,
                                                int regime_id,
                                                const float* per_arm_preds,
                                                int n_arms,
                                                float sample_price) {
    if (!ezoo || !ezoo->active) return;
    int slot = ezoo->reward_ring_head % EnsembleModelZoo<F>::REWARD_RING_SIZE;
    auto& rec = ezoo->reward_ring[slot];
    ezoo->predict_call_count++;
    rec.predict_call = ezoo->predict_call_count;
    rec.regime_id = regime_id;
    rec.sample_price = sample_price;
    rec.rewarded_lookback = 0;
    rec.rewarded_trade = 0;
    int n = (n_arms < ENSEMBLE_HORIZON_MAX) ? n_arms : ENSEMBLE_HORIZON_MAX;
    for (int a = 0; a < n; ++a) rec.predictions[a] = per_arm_preds[a];
    for (int a = n; a < ENSEMBLE_HORIZON_MAX; ++a) rec.predictions[a] = 0.5f;
    ezoo->reward_ring_head = (ezoo->reward_ring_head + 1)
                             % EnsembleModelZoo<F>::REWARD_RING_SIZE;
}

//======================================================================================================
// [v5.10.0a.G.8 — DRIFT WATCHDOG (perf optimization #3)]
//======================================================================================================
// Updates per-arm IC running history with reward outcome. If IC drops
// below ic_floor for sustained window → demote (force near-zero weight
// across all regimes). Recovery: IC rises above ic_floor + 0.02
// hysteresis → un-demote, allow re-learn.
template <unsigned F>
inline void EnsembleModelZoo_UpdateDrift(EnsembleModelZoo<F>* ezoo,
                                           int arm,
                                           int correct,   // 1 = correct, 0 = wrong
                                           double ic_floor) {
    if (!ezoo) return;
    EnsembleModelZoo_EnsurePrimary(ezoo);
    if (arm < 0 || arm >= ezoo->primary_count) return;
    auto& d = ezoo->drift[arm];
    int idx = d.ic_count % EnsembleModelZoo<F>::DRIFT_IC_HISTORY;
    d.ic_history[idx] = correct ? 1.0f : -1.0f;
    if (d.ic_count < EnsembleModelZoo<F>::DRIFT_IC_HISTORY) d.ic_count++;
    // Recompute running average (cheap; bounded N)
    double sum = 0.0;
    int n = d.ic_count;
    if (n > EnsembleModelZoo<F>::DRIFT_IC_HISTORY) n = EnsembleModelZoo<F>::DRIFT_IC_HISTORY;
    for (int i = 0; i < n; ++i) sum += d.ic_history[i];
    d.ic_avg = (n > 0) ? (float)(sum / n) : 0.0f;
    // Demote / recover
    if (n >= 20 && d.ic_avg < (float)ic_floor && !d.demoted) {
        // Force near-zero weight across all regimes (operator escape from
        // a horizon that's gone bad faster than bandit's natural decay)
        for (int r = 0; r < NUM_REGIMES; ++r) {
            if (arm < ezoo->bandits[r].n_arms) {
                ezoo->bandits[r].weights[arm] = 1e-9;
            }
        }
        d.demoted = 1;
        fprintf(stderr, "[ensemble] DRIFT-WATCHDOG: arm %d (h%d) demoted "
                        "(IC=%.4f below floor %.4f); weights forced near 0.\n",
                arm, ezoo->horizon_ticks_at_idx[arm], d.ic_avg, ic_floor);
    } else if (d.demoted && d.ic_avg > (float)ic_floor + 0.02f) {
        d.demoted = 0;
        fprintf(stderr, "[ensemble] arm %d (h%d) recovered (IC=%.4f); "
                        "weight allowed to re-learn.\n",
                arm, ezoo->horizon_ticks_at_idx[arm], d.ic_avg);
    }
}

//======================================================================================================
// [v5.10.0a.G.8 — SLOW-PATH LOOKBACK REWARDS]
//======================================================================================================
// Walks reward ring; for records old enough that forward_ticks have
// elapsed since predict time, computes per-arm reward based on whether
// prediction direction matched the price move (current_price vs
// record.sample_price). Calls Bandit_Update on the matching regime's
// bandit. Marks records as rewarded to avoid double-rewarding.
template <unsigned F>
inline void EnsembleModelZoo_TickRewardsFromLookback(EnsembleModelZoo<F>* ezoo,
                                                       float current_price,
                                                       int forward_ticks,
                                                       int poll_interval,
                                                       double ic_floor) {
    if (!ezoo || !ezoo->active || !ezoo->initialized_bandits) return;
    if (poll_interval <= 0) poll_interval = 100;
    if (forward_ticks <= 0) forward_ticks = 1000;
    uint64_t lookback_calls = (uint64_t)((forward_ticks + poll_interval - 1)
                                          / poll_interval);
    if (lookback_calls == 0) lookback_calls = 1;
    uint64_t now = ezoo->predict_call_count;
    // v5.11.62 — n_arms = primary_count (matches ezoo->primary_handles)
    EnsembleModelZoo_EnsurePrimary(ezoo);
    int n_arms = ezoo->primary_count;

    // Walk all populated records; reward ones that are old enough + not
    // yet rewarded.
    for (int i = 0; i < EnsembleModelZoo<F>::REWARD_RING_SIZE; ++i) {
        auto& rec = ezoo->reward_ring[i];
        if (rec.predict_call == 0) continue;             // unpopulated slot
        if (rec.rewarded_lookback) continue;             // already rewarded
        if (now < rec.predict_call + lookback_calls) continue;  // too recent

        // Compute price delta sign
        if (rec.sample_price <= 0.0f) { rec.rewarded_lookback = 1; continue; }
        double price_delta = ((double)current_price - (double)rec.sample_price)
                              / (double)rec.sample_price;
        int regime = rec.regime_id;
        if (regime < 0 || regime >= NUM_REGIMES) regime = 0;

        // Per-arm reward: 1 if predicted direction matched, 0 otherwise.
        // Skip disabled arms (bitmask check).
        int updates_this_record = 0;
        for (int a = 0; a < n_arms; ++a) {
            if (ezoo->disabled_horizon_mask & (1u << a)) continue;
            float p = rec.predictions[a];
            int correct = ((p > 0.5f) == (price_delta > 0.0)) ? 1 : 0;
            // Reward signal in bps. Treat correct as +50bps, wrong as -50bps;
            // Bandit_Update accumulates these.
            double reward_bps = correct ? 50.0 : -50.0;
            Bandit_Update(&ezoo->bandits[regime], a, reward_bps);
            updates_this_record++;
            // Drift watchdog updates per-arm IC tracker
            EnsembleModelZoo_UpdateDrift(ezoo, a, correct, ic_floor);
        }
        rec.rewarded_lookback = 1;
        // v5.10.0a.G.9 — periodic save trigger after each ring record's
        // updates land. Cheap when bandit_save_interval==0 (no-op early
        // return). When fires (once every N updates), atomic file write
        // takes ~1ms — acceptable on slow path.
        EnsembleModelZoo_MaybeSaveBanditPeriodic(ezoo, updates_this_record);
    }
}

//======================================================================================================
// [v5.10.0a.G.8 — TRADE-CLOSE REWARD HOOK]
//======================================================================================================
// Called from EventLoop_DrainPostFill when a position closes (TP/SL
// exit). Looks up the MOST RECENT prediction record (proxy for "the
// model recommendation that drove this trade") and rewards based on
// realized P&L direction. Higher weight than slow-path (real money
// signal includes fees + slippage).
//
// reward_mult: cfg.ensemble_trade_reward_mult (default 4.0). Scales
// |reward_bps| × mult; correct predictions → positive bps, wrong →
// negative.
template <unsigned F>
inline void EnsembleModelZoo_TradeCloseReward(EnsembleModelZoo<F>* ezoo,
                                                double realized_pnl_bps,
                                                double reward_mult) {
    if (!ezoo || !ezoo->active || !ezoo->initialized_bandits) return;
    if (ezoo->predict_call_count == 0) return;  // no predictions yet

    // Find the most recent record that hasn't been trade-rewarded.
    // Walk ring backward from head.
    EnsembleModelZoo_EnsurePrimary(ezoo);
    int n_arms = ezoo->primary_count;
    int found = -1;
    for (int back = 1; back <= EnsembleModelZoo<F>::REWARD_RING_SIZE; ++back) {
        int idx = (ezoo->reward_ring_head - back +
                   EnsembleModelZoo<F>::REWARD_RING_SIZE)
                   % EnsembleModelZoo<F>::REWARD_RING_SIZE;
        if (ezoo->reward_ring[idx].predict_call == 0) break;
        if (ezoo->reward_ring[idx].rewarded_trade) continue;
        found = idx;
        break;
    }
    if (found < 0) return;

    auto& rec = ezoo->reward_ring[found];
    int regime = rec.regime_id;
    if (regime < 0 || regime >= NUM_REGIMES) regime = 0;
    int pnl_positive = (realized_pnl_bps > 0.0) ? 1 : 0;

    int trade_updates = 0;
    for (int a = 0; a < n_arms; ++a) {
        if (ezoo->disabled_horizon_mask & (1u << a)) continue;
        float p = rec.predictions[a];
        int correct = ((p > 0.5f) == (pnl_positive == 1)) ? 1 : 0;
        // Trade-close reward weighted higher than slow-path lookback
        double reward_bps = (correct ? 50.0 : -50.0) * reward_mult;
        Bandit_Update(&ezoo->bandits[regime], a, reward_bps);
        trade_updates++;
    }
    rec.rewarded_trade = 1;
    // v5.10.0a.G.9 — periodic save check (no-op when interval==0)
    EnsembleModelZoo_MaybeSaveBanditPeriodic(ezoo, trade_updates);
}

//======================================================================================================
// [v5.10.0a.G.7 — INIT BANDITS]
//======================================================================================================
// Call AFTER LoadFromCfg / AutoDetectFromDir populates ezoo->buy_signal_count.
// Initializes one BanditState per regime (NUM_REGIMES from FOREACH_REGIME),
// each with n_arms = buy_signal_count. Uniform initial weights.
//
// eta: cfg.ensemble_bandit_eta (Bandit-Exp3 learning rate; 0.1 default)
// min_warmup: cfg.ensemble_min_warmup_predictions (per regime; 100 default)
//
// Sets ezoo->initialized_bandits = 1 to gate G.7 dispatch (won't read bandits
// before they're initialized).
template <unsigned F>
inline void EnsembleModelZoo_InitBandits(EnsembleModelZoo<F>* ezoo,
                                           double eta, int min_warmup) {
    if (!ezoo) return;
    EnsembleModelZoo_EnsurePrimary(ezoo);
    // v5.11.62 — bandit operates on primary handles (set at load time
    // to whichever role was actually loaded). Fixes the case where
    // barrier role was loaded but buy_signal_count==0 → bandit
    // never initialized → predictions stayed uniform forever.
    int n_arms = ezoo->primary_count;
    if (n_arms < 2) {
        // Single-arm or empty ensemble — no point in bandits. Mark
        // initialized so dispatch doesn't loop forever, but bandits won't
        // be used (ezoo->active gates that anyway).
        ezoo->initialized_bandits = 1;
        return;
    }
    for (int r = 0; r < NUM_REGIMES; ++r) {
        // Bandit_Init signature: (state, n_arms, gamma, eta_max,
        //                          blend_ratio, min_samples, ramp_up)
        // Map cfg.ensemble_bandit_eta → eta_max; min_warmup → min_samples.
        // gamma + ramp_up + blend_ratio use sensible defaults.
        Bandit_Init(&ezoo->bandits[r], n_arms,
                    /*gamma=*/0.05,
                    /*eta_max=*/(eta > 0.0 ? eta : 0.1),
                    /*blend_ratio=*/1.0,         // full bandit influence in ensemble
                    /*min_samples=*/(min_warmup > 0 ? min_warmup : 100),
                    /*ramp_up=*/(min_warmup > 0 ? min_warmup * 2 : 200));
        // Set arm names for logging/debug
        for (int a = 0; a < n_arms; ++a) {
            char nm[32];
            snprintf(nm, sizeof(nm), "h%d", ezoo->horizon_ticks_at_idx[a]);
            Bandit_SetArmName(&ezoo->bandits[r], a, nm);
        }
    }
    ezoo->initialized_bandits = 1;
}

// v5.13.4 — sell-side bandit init. Mirrors _InitBandits above for the
// exit_predictor role. Arms count = exit_predictor_count (set at
// LoadFromCfg time). Defaults match buy-side (gamma=0.05, blend=1.0,
// min_samples=100, ramp=200) so exit-side learning shape matches buy-
// side discipline. Operator opts in via cfg.exit_bandit_enabled at the
// HandleFill attribution path; init is harmless if cfg is off (bandits
// just stay uniform until first reward arrives).
//
// Caller: EngineSharded boot, AFTER EnsembleModelZoo_LoadFromCfg
// populates exit_predictor_count.
template <unsigned F>
inline void EnsembleModelZoo_InitExitBandits(EnsembleModelZoo<F>* ezoo,
                                               double exit_eta,
                                               int min_warmup) {
    if (!ezoo) return;
    int n_arms = ezoo->exit_predictor_count;
    if (n_arms < 1) {
        // No exit models loaded — graceful skip. exit_bandits stay
        // zero-init; HandleFill attribution check
        // initialized_exit_bandits=0 → no Bandit_Update fires.
        ezoo->initialized_exit_bandits = 0;
        return;
    }
    if (n_arms < 2) {
        // Single-arm: no point in bandits, but mark initialized so
        // HandleFill can call Bandit_Update without crashing
        // (single-arm Update is a no-op accumulating reward stats).
        ezoo->initialized_exit_bandits = 1;
        return;
    }
    for (int r = 0; r < NUM_REGIMES; ++r) {
        Bandit_Init(&ezoo->exit_bandits[r], n_arms,
                    /*gamma=*/0.05,
                    /*eta_max=*/(exit_eta > 0.0 ? exit_eta : 0.1),
                    /*blend_ratio=*/1.0,
                    /*min_samples=*/(min_warmup > 0 ? min_warmup : 100),
                    /*ramp_up=*/(min_warmup > 0 ? min_warmup * 2 : 200));
        for (int a = 0; a < n_arms; ++a) {
            char nm[32];
            snprintf(nm, sizeof(nm), "exit_h%d",
                     ezoo->horizon_ticks_at_idx[a]);
            Bandit_SetArmName(&ezoo->exit_bandits[r], a, nm);
        }
    }
    ezoo->initialized_exit_bandits = 1;
}

//======================================================================================================
// [v5.10.0a.G.7 — KILL-SWITCH PARSER]
//======================================================================================================
// Parses CSV string ("100,500") → bitmask of horizon indices that match.
// Disabled horizons skip predict (saves N×predict cost per disabled);
// their bandit weights stay frozen at last value (skipped by Bandit_Update).
template <unsigned F>
inline void EnsembleModelZoo_SetDisabledHorizons(EnsembleModelZoo<F>* ezoo,
                                                   const char* csv) {
    if (!ezoo) return;
    EnsembleModelZoo_EnsurePrimary(ezoo);
    ezoo->disabled_horizon_mask = 0;
    if (!csv || csv[0] == '\0') return;
    const char* p = csv;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;
        char* end = nullptr;
        long h = strtol(p, &end, 10);
        if (end == p) break;
        // Find which arm this horizon ticks corresponds to
        // v5.11.62 — primary_count (matches primary_handles array length)
        for (int a = 0; a < ezoo->primary_count; ++a) {
            if (ezoo->horizon_ticks_at_idx[a] == (int)h) {
                ezoo->disabled_horizon_mask |= (1u << a);
                fprintf(stderr, "[ensemble] horizon %d (arm %d) DISABLED by cfg\n",
                        (int)h, a);
                break;
            }
        }
        p = end;
    }
}

template <unsigned F>
inline void EnsembleModelZoo_Free(EnsembleModelZoo<F> *ezoo) {
    for (int i = 0; i < ENSEMBLE_HORIZON_MAX; ++i) {
        Model_Free(&ezoo->barrier[i]);
        Model_Free(&ezoo->regime[i]);
        Model_Free(&ezoo->exit_predictor[i]);
        Model_Free(&ezoo->buy_signal[i]);
    }
    ezoo->barrier_count = 0;
    ezoo->regime_count = 0;
    ezoo->exit_predictor_count = 0;
    ezoo->buy_signal_count = 0;
    ezoo->active = 0;
}

// Load N models per role from per-horizon directories. Operator's
// Train Multi-Horizon worker (v5.10.0a.G.1) saves to:
//   models/<class_or_regr>/<run_name>_horizon_<H>/<role>.json
//
// This loader expects:
//   base_run_path = "models/<class>/<run_name>" (without _horizon_<H> suffix)
// Per horizon h in horizon_list[]:
//   try load <base_run_path>_horizon_<H>/<role>.json for each role
//
// Returns total models loaded across all roles + horizons. Sets
// ezoo->active=1 if any role got at least one horizon loaded.
template <unsigned F>
inline int EnsembleModelZoo_LoadFromCfg(EnsembleModelZoo<F> *ezoo,
                                         const char *base_run_path,
                                         const int *horizon_list,
                                         int horizon_count,
                                         int backend,
                                         const char* held_out_stamp_secret = nullptr,
                                         double gap_threshold = 0.05,
                                         int held_out_gate_strict = 0,
                                         int acknowledge_cross_binary_drift = 0) {
    if (!ezoo || !base_run_path || base_run_path[0] == '\0' ||
        !horizon_list || horizon_count <= 0) return 0;

    if (horizon_count > ENSEMBLE_HORIZON_MAX) horizon_count = ENSEMBLE_HORIZON_MAX;

    int total_loaded = 0;
    char per_horizon_dir[512];
    for (int h = 0; h < horizon_count; ++h) {
        int H = horizon_list[h];
        if (H <= 0) continue;
        snprintf(per_horizon_dir, sizeof(per_horizon_dir),
                 "%s_horizon_%d", base_run_path, H);

        // Try each role at this horizon's dir.
        // v5.11.42 D.2 — pass H as expected_horizon_ticks so TryLoadRole
        // refuses if stamp's label_lookahead_ticks doesn't match the dir
        // we loaded from.
        if (CoreModelZoo_TryLoadRole(&ezoo->barrier[ezoo->barrier_count],
                                       per_horizon_dir, "barrier", backend,
                                       held_out_stamp_secret, gap_threshold,
                                       held_out_gate_strict,
                                       acknowledge_cross_binary_drift,
                                       /*expected_feature_mask=*/0,
                                       /*expected_horizon_ticks=*/H)) {
            ezoo->horizon_ticks_at_idx[ezoo->barrier_count] = H;
            ezoo->barrier_count++;
            total_loaded++;
        }
        if (CoreModelZoo_TryLoadRole(&ezoo->regime[ezoo->regime_count],
                                       per_horizon_dir, "regime", backend,
                                       held_out_stamp_secret, gap_threshold,
                                       held_out_gate_strict,
                                       acknowledge_cross_binary_drift,
                                       /*expected_feature_mask=*/0,
                                       /*expected_horizon_ticks=*/H)) {
            ezoo->regime_count++;
            total_loaded++;
        }
        if (CoreModelZoo_TryLoadRole(&ezoo->exit_predictor[ezoo->exit_predictor_count],
                                       per_horizon_dir, "exit", backend,
                                       held_out_stamp_secret, gap_threshold,
                                       held_out_gate_strict,
                                       acknowledge_cross_binary_drift,
                                       /*expected_feature_mask=*/0,
                                       /*expected_horizon_ticks=*/H)) {
            ezoo->exit_predictor_count++;
            total_loaded++;
        }
        if (CoreModelZoo_TryLoadRole(&ezoo->buy_signal[ezoo->buy_signal_count],
                                       per_horizon_dir, "buy_signal", backend,
                                       held_out_stamp_secret, gap_threshold,
                                       held_out_gate_strict,
                                       acknowledge_cross_binary_drift,
                                       /*expected_feature_mask=*/0,
                                       /*expected_horizon_ticks=*/H)) {
            ezoo->buy_signal_count++;
            total_loaded++;
        }
    }

    // v5.11.62 — primary-role indirection (ensemble). Pick the first
    // available role and point ezoo->primary_handles at its array.
    // Priority: buy_signal > barrier > regime. Set per-handle
    // buy_class_idx so Model_Predict returns the right class probability
    // (class 1 = peak for PEAK_VALLEY_STABLE 3-class barrier; class 0
    // for binary). Strategy + bandit code reads primary_*, not buy_signal_*.
    ezoo->primary_handles = nullptr;
    ezoo->primary_count = 0;
    ezoo->primary_target_class = 0;
    ezoo->primary_role_name[0] = '\0';
    if (ezoo->buy_signal_count > 0) {
        ezoo->primary_handles = ezoo->buy_signal;
        ezoo->primary_count = ezoo->buy_signal_count;
        ezoo->primary_target_class = 0;
        for (int i = 0; i < ezoo->buy_signal_count; ++i) {
            ezoo->buy_signal[i].buy_class_idx = 0;
        }
        strncpy(ezoo->primary_role_name, "buy_signal",
                sizeof(ezoo->primary_role_name) - 1);
    } else if (ezoo->barrier_count > 0) {
        ezoo->primary_handles = ezoo->barrier;
        ezoo->primary_count = ezoo->barrier_count;
        int class_idx = (ezoo->barrier[0].num_outputs >= 2) ? 1 : 0;
        ezoo->primary_target_class = class_idx;
        for (int i = 0; i < ezoo->barrier_count; ++i) {
            ezoo->barrier[i].buy_class_idx =
                (ezoo->barrier[i].num_outputs >= 2) ? 1 : 0;
        }
        strncpy(ezoo->primary_role_name, "barrier",
                sizeof(ezoo->primary_role_name) - 1);
    } else if (ezoo->regime_count > 0) {
        ezoo->primary_handles = ezoo->regime;
        ezoo->primary_count = ezoo->regime_count;
        ezoo->primary_target_class = 0;
        for (int i = 0; i < ezoo->regime_count; ++i) {
            ezoo->regime[i].buy_class_idx = 0;
        }
        strncpy(ezoo->primary_role_name, "regime",
                sizeof(ezoo->primary_role_name) - 1);
    }
    ezoo->primary_role_name[sizeof(ezoo->primary_role_name) - 1] = '\0';

    // v5.13.0 (audit gap-close 2026-05-08) — exit_predictor buy_class_idx
    // aliasing. Independent of primary-role selection above (exit_predictor
    // is consumed by the v5.13 sell-side path, not by the buy-side
    // ensemble blend). Path 3 architecture: exit_predictor uses the SAME
    // PEAK_VALLEY_STABLE 3-class labels as buy-side barrier, so we extract
    // class 1 (peak) probability as "exit imminent" signal.
    //
    // CRITICAL: without this aliasing, exit_predictor handles default to
    // buy_class_idx=0 (VALLEY class) → Model_Predict_Normalized returns
    // valley probability instead of peak probability → exits would fire
    // at WRONG MOMENT (during dips instead of at peaks). Silent semantic
    // inversion that the /plan-check + /merge-scan audits caught before
    // coding started.
    //
    // Mirrors the barrier-role pattern (lines ~1280-1281) but applies to
    // exit_predictor independently.
    for (int i = 0; i < ezoo->exit_predictor_count; ++i) {
        ezoo->exit_predictor[i].buy_class_idx =
            (ezoo->exit_predictor[i].num_outputs >= 2) ? 1 : 0;
    }

    if (total_loaded > 0) {
        ezoo->active = 1;
        fprintf(stderr, "[ML] ensemble zoo: %d total models loaded "
                        "(barrier=%d, regime=%d, exit=%d, buy_signal=%d) "
                        "across %d horizons\n",
                total_loaded,
                ezoo->barrier_count, ezoo->regime_count,
                ezoo->exit_predictor_count, ezoo->buy_signal_count,
                horizon_count);

        // v5.11.42 D.3 — sibling consistency WARN. Per-horizon scalers
        // SHOULD be identical across siblings of the same role (scaler
        // is derived from the shared feature matrix, not from per-horizon
        // labels). If sibling scalers differ → WARN (operator may have
        // mixed training sessions or accidentally copied a sidecar from
        // a different run). Doesn't refuse — model already loaded; just
        // operator notification.
        auto check_sibling_scalers = [&](ModelHandle<F>* arr, int count, const char* role_name) {
            const char* baseline_sha = nullptr;
            int baseline_idx = -1;
            for (int i = 0; i < count; ++i) {
                if (!arr[i].has_stamp_scaler_sha256) continue;
                if (arr[i].stamp_scaler_sha256[0] == '\0') continue;
                if (baseline_sha == nullptr) {
                    baseline_sha = arr[i].stamp_scaler_sha256;
                    baseline_idx = i;
                    continue;
                }
                if (strcmp(arr[i].stamp_scaler_sha256, baseline_sha) != 0) {
                    fprintf(stderr,
                        "[sibling-consistency] WARN: ensemble role=%s "
                        "sibling[%d] scaler_sha256=%.16s... differs from "
                        "sibling[%d] scaler_sha256=%.16s... "
                        "(per-horizon scalers should be identical; mixed "
                        "training sessions or sidecar copy mistake?)\n",
                        role_name, i, arr[i].stamp_scaler_sha256,
                        baseline_idx, baseline_sha);
                }
            }
        };
        check_sibling_scalers(ezoo->barrier,        ezoo->barrier_count,        "barrier");
        check_sibling_scalers(ezoo->regime,         ezoo->regime_count,         "regime");
        check_sibling_scalers(ezoo->exit_predictor, ezoo->exit_predictor_count, "exit");
        check_sibling_scalers(ezoo->buy_signal,     ezoo->buy_signal_count,     "buy_signal");
    } else {
        fprintf(stderr, "[ML] ensemble zoo: no models loaded "
                        "(checked %d horizons under base '%s'; falling back "
                        "to single-zoo)\n",
                horizon_count, base_run_path);
    }
    return total_loaded;
}

//======================================================================================================
// [v5.10.0a.G.5 — AUTO-DETECT ENSEMBLE FROM DISK]
//======================================================================================================
// Scans <base_dir>_horizon_* siblings on disk. For each sibling found:
//   - Verify load via CoreModelZoo_TryLoadRole
//   - Read stamp body's grid_member_count + grid_member_idx (v5.10.0a.G.2)
//   - Validate consistency: all loaded siblings must agree on grid_member_count
//   - Place each model at its grid_member_idx slot in the ensemble
//
// Operator workflow:
//   1. Train Multi-Horizon (G.1) → models/<run>/<run>_horizon_<H>/role.json
//   2. Cfg: core_N_model_dir=models/<run>  (NOTE: base path WITHOUT _horizon_<H>)
//   3. Engine boot calls AutoDetectFromDir(ezoo, "models/<run>", ...)
//   4. Function discovers all _horizon_* siblings + populates ezoo
//
// Returns total models loaded across all roles + horizons. Sets
// ezoo->active=1 if any role got at least one horizon loaded; logs
// the discovered horizon set.
//
// Backward-compat:
//   - empty base_dir → no-op, returns 0
//   - no siblings on disk → returns 0, ezoo->active stays 0; engine
//     falls back to single-zoo path
//   - inconsistent grid_member_count across siblings → log error +
//     skip inconsistent ones (load only those that agree on count)
//   - missing stamps → load anyway with warn (legacy multi-train
//     pre-v5.10.0a.G.2 might not have grid_member_count stamped)
//
// v5.10.0a.next reader: if all loaded stamps have per_regime_val_acc
// fields (added by future trainer-side ship), use as bandit init
// priors. Currently no-op since the stamp fields don't exist yet.

// v5.10.1.B — Cross-handle grid_member_count consistency check
// (parity-check Finding #2 consume-side closure; Option C).
//
// Walks every loaded handle across the 4 ensemble roles and verifies that
// all stamps agree on grid_member_count. Re-parses each stamp via
// verify_model_stamp() rather than caching the value on ModelHandle.
// Wasteful (re-opens file at boot) but boundary-stable per CLAUDE.local.md
// "boundary-stable refactor" rule (no struct schema cascade).
//
// Back-compat: legacy stamps without grid_member_count log a WARN-and-load
// note explaining that train_multi_horizon_worker_fn doesn't emit stamps yet.
// Closure of the emit side is deferred to v5.10.X.
//
// Returns:
//   1 — OK (uniform grid_member_count or all-legacy WARN-and-load)
//   0 — REFUSE (mismatched grid_member_count across siblings; caller unwinds)
//
// Extracted from EnsembleModelZoo_AutoDetectFromDir for unit-testable isolation.
template <unsigned F>
inline int EnsembleZoo_VerifyGridMemberConsistency(
    EnsembleModelZoo<F> *ezoo,
    const char *held_out_stamp_secret,
    double gap_threshold)
{
    int agreed_count   = -1;
    int legacy_count   = 0;
    int total_handles  = 0;

    for (int role = 0; role < 4; ++role) {
        ModelHandle<F> *role_arr;
        int count;
        switch (role) {
            case 0: role_arr = ezoo->buy_signal;     count = ezoo->buy_signal_count;     break;
            case 1: role_arr = ezoo->barrier;        count = ezoo->barrier_count;        break;
            case 2: role_arr = ezoo->regime;         count = ezoo->regime_count;         break;
            case 3: role_arr = ezoo->exit_predictor; count = ezoo->exit_predictor_count; break;
            default: continue;
        }
        for (int h = 0; h < count; ++h) {
            const ModelHandle<F> *m = &role_arr[h];
            if (!Model_IsLoaded(m) || m->model_path[0] == '\0') continue;
            ++total_handles;

            // Re-parse stamp file from disk. Pass 0 for the registry hashes —
            // we already verified them on the original load via
            // EnsembleModelZoo_LoadFromCfg → CoreModelZoo_TryLoadRole; here we
            // just need grid_member_count out of the body.
            ModelStampResult sr = verify_model_stamp(
                m->model_path,
                held_out_stamp_secret ? held_out_stamp_secret : "",
                gap_threshold,
                MODEL_FORMAT_VERSION,
                /*expected_feature_registry_hash=*/0,
                /*expected_label_registry_hash=*/0);

            if (!sr.has_grid_member_count) {
                ++legacy_count;
                continue; // back-compat: unstamped multi-horizon model
            }
            if (agreed_count < 0) {
                agreed_count = (int)sr.grid_member_count;
            } else if ((int)sr.grid_member_count != agreed_count) {
                fprintf(stderr,
                    "[ensemble_auto_detect] REFUSED: handle role=%d h=%d "
                    "stamps grid_member_count=%u; expected %d. "
                    "Mixed-training-run ensemble.\n",
                    role, h, (unsigned)sr.grid_member_count, agreed_count);
                return 0;  // refuse
            }
        }
    }

    if (agreed_count > 0) {
        fprintf(stderr,
            "[ensemble_auto_detect] OK: %d/%d handles agree on grid_member_count=%d\n",
            (total_handles - legacy_count), total_handles, agreed_count);
    } else if (legacy_count > 0) {
        fprintf(stderr,
            "[ensemble_auto_detect] WARN: %d/%d handles missing grid_member_count "
            "(unstamped multi-horizon model OR pre-v5.10.0a.G.2 ensemble); "
            "consistency check skipped. "
            "TODO(v5.10.X): wire stamp_write_for_model into "
            "train_multi_horizon_worker_fn to emit stamps.\n",
            legacy_count, total_handles);
    }
    return 1;  // ok (uniform or all-legacy)
}

template <unsigned F>
inline int EnsembleModelZoo_AutoDetectFromDir(
    EnsembleModelZoo<F> *ezoo,
    const char *base_dir,             // e.g. "models/test_case3" (no _horizon_<H> suffix)
    int backend,
    const char* held_out_stamp_secret = nullptr,
    double gap_threshold = 0.05,
    int held_out_gate_strict = 0,
    int acknowledge_cross_binary_drift = 0) {
    if (!ezoo || !base_dir || base_dir[0] == '\0') return 0;

    // Step 1: scan filesystem for <base_dir>_horizon_<N> siblings.
    // We need parent dir + base name to enumerate.
    char parent_path[400];
    char base_name[200];
    {
        size_t dlen = strnlen(base_dir, 400);
        if (dlen == 0 || dlen >= 400) return 0;
        // Strip trailing slash if any
        char b[400];
        memcpy(b, base_dir, dlen);
        b[dlen] = '\0';
        if (b[dlen - 1] == '/') { b[dlen - 1] = '\0'; dlen--; }
        // Find last slash
        const char *last_slash = strrchr(b, '/');
        if (last_slash) {
            size_t plen = (size_t)(last_slash - b);
            if (plen >= sizeof(parent_path)) plen = sizeof(parent_path) - 1;
            memcpy(parent_path, b, plen);
            parent_path[plen] = '\0';
            size_t blen = strnlen(last_slash + 1, sizeof(base_name) - 1);
            memcpy(base_name, last_slash + 1, blen);
            base_name[blen] = '\0';
        } else {
            // No slash: cwd-relative
            parent_path[0] = '.';
            parent_path[1] = '\0';
            size_t blen = strnlen(b, sizeof(base_name) - 1);
            memcpy(base_name, b, blen);
            base_name[blen] = '\0';
        }
    }

    DIR *dir = opendir(parent_path);
    if (!dir) {
        // Parent dir not readable (operator's base_dir doesn't exist) — silent
        // fail. Caller falls back to single-zoo path.
        return 0;
    }

    // Pattern to match: <base_name>_horizon_<digits>
    char prefix[256];
    int prefix_len = snprintf(prefix, sizeof(prefix), "%s_horizon_", base_name);

    // Collect candidate horizon ticks (sorted ascending so dispatch is
    // deterministic regardless of filesystem readdir order).
    int discovered_horizons[ENSEMBLE_HORIZON_MAX] = {0};
    int n_discovered = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (n_discovered >= ENSEMBLE_HORIZON_MAX) break;
        // Match prefix
        if (strncmp(entry->d_name, prefix, prefix_len) != 0) continue;
        // Parse trailing number
        char *suffix = entry->d_name + prefix_len;
        char *end = nullptr;
        long h = strtol(suffix, &end, 10);
        if (end == suffix || *end != '\0') continue;  // non-numeric suffix
        if (h <= 0 || h > 1000000) continue;          // sanity bounds
        discovered_horizons[n_discovered++] = (int)h;
    }
    closedir(dir);

    if (n_discovered == 0) {
        // No siblings found; ezoo stays inactive
        return 0;
    }

    // Sort ascending (insertion sort; n is tiny)
    for (int i = 1; i < n_discovered; ++i) {
        int v = discovered_horizons[i];
        int j = i - 1;
        while (j >= 0 && discovered_horizons[j] > v) {
            discovered_horizons[j + 1] = discovered_horizons[j];
            j--;
        }
        discovered_horizons[j + 1] = v;
    }

    // Step 2: load each horizon via existing LoadFromCfg machinery.
    // This is identical to the operator-cfg-driven path; just wires from
    // disk-discovery instead of cfg.horizon_list.
    int total = EnsembleModelZoo_LoadFromCfg(ezoo, base_dir,
                                               discovered_horizons, n_discovered,
                                               backend,
                                               held_out_stamp_secret,
                                               gap_threshold,
                                               held_out_gate_strict,
                                               acknowledge_cross_binary_drift);

    // v5.10.1.B — Cross-handle grid_member_count consistency check
    // (parity-check Finding #2 consume-side closure; Option C).
    // Validator extracted into EnsembleZoo_VerifyGridMemberConsistency for
    // unit-testable isolation; runs only when models actually loaded.
    if (total > 0 && ezoo->active) {
        int validator_rc = EnsembleZoo_VerifyGridMemberConsistency(
            ezoo, held_out_stamp_secret, gap_threshold);
        if (validator_rc == 0) {
            // Mismatched grid_member_count across siblings → unwind + refuse.
            EnsembleModelZoo_Free(ezoo);
            return 0;  // match function contract: returns "total models loaded"
        }
    }

    if (total > 0 && ezoo->active) {
        // Build a comma-separated list for the log
        char hlog[256];
        int off = 0;
        for (int h = 0; h < n_discovered && off < (int)sizeof(hlog) - 8; ++h) {
            off += snprintf(hlog + off, sizeof(hlog) - off,
                            "%s%d", h == 0 ? "" : ",", discovered_horizons[h]);
        }
        fprintf(stderr, "[ensemble] auto-detected %d horizons under '%s': {%s}\n",
                n_discovered, base_dir, hlog);
    }

    return total;
}

//======================================================================================================
// [v5.10.0a.G.9 — BUNDLE SHA + BANDIT STATE LOAD/SAVE]
//======================================================================================================
// "Bundle SHA": deterministic 64-char hex derived from each loaded
// horizon's training_fingerprint. NOT a cryptographic SHA — just a
// stable identifier for "this exact set of models." Detects when
// operator swaps models without clearing the bandit_state.json;
// mismatch → load returns 0 → caller falls back to uniform via
// EnsembleModelZoo_InitBandits.
//
// Algorithm: concat first 8 chars of each loaded handle's
// training_fingerprint (in horizon-sorted order) into a 64-char
// hex string. Pads with '0' if fewer than 8 horizons. Same horizons
// + same fingerprints → same bundle id.
template <unsigned F>
inline void EnsembleModelZoo_ComputeBundleId(
    const EnsembleModelZoo<F>* ezoo, char* hex_out, size_t hex_cap) {
    if (!ezoo || !hex_out || hex_cap < 65) return;
    memset(hex_out, '0', 64);
    hex_out[64] = '\0';
    // v5.11.62 — bundle ID computed from primary handles (matches what
    // strategy actually uses). Same handle array bandit weights bind to.
    EnsembleModelZoo_EnsurePrimary(const_cast<EnsembleModelZoo<F>*>(ezoo));
    int n = ezoo->primary_count;
    if (n > 8) n = 8;
    for (int a = 0; a < n && ezoo->primary_handles; ++a) {
        const ModelHandle<F>& h = ezoo->primary_handles[a];
        // Copy first 8 hex chars of training_fingerprint into slot a.
        // If fingerprint is empty or too short, leave zeros.
        const char* fp = h.training_fingerprint;
        size_t flen = strnlen(fp, 65);
        if (flen >= 8) {
            memcpy(hex_out + a * 8, fp, 8);
        }
    }
}

// Save bandit state to <base_dir>/bandit_state.json. Returns 1 on
// success, 0 on failure (silent — caller logs if it cares).
template <unsigned F>
inline int EnsembleModelZoo_SaveBanditState(
    const EnsembleModelZoo<F>* ezoo, const char* base_dir,
    const char* const* regime_names) {
    if (!ezoo || !ezoo->active || !ezoo->initialized_bandits) return 0;
    if (!base_dir || base_dir[0] == '\0') return 0;
    char path[512];
    snprintf(path, sizeof(path), "%s/bandit_state.json", base_dir);
    char bundle_id[65];
    EnsembleModelZoo_ComputeBundleId(ezoo, bundle_id, sizeof(bundle_id));
    return Bandit_SaveJSON(ezoo->bandits, NUM_REGIMES, path,
                            bundle_id, regime_names);
}

// v5.13.4.C — sell-side bandit persistence. Mirrors _SaveBanditState
// for the exit_bandits[] array. Saved as a SEPARATE file
// (<base_dir>/exit_bandit_state.json) rather than extending the
// existing JSON format, which would cascade into Bandit_SaveJSON /
// Bandit_LoadJSON (broader callers, larger blast radius). Forward-
// compat by absence: legacy bundles without the file load with
// uniform priors (same shape as a fresh deploy). Returns 1 on
// success, 0 on failure (silent).
template <unsigned F>
inline int EnsembleModelZoo_SaveExitBanditState(
    const EnsembleModelZoo<F>* ezoo, const char* base_dir,
    const char* const* regime_names) {
    if (!ezoo || !ezoo->initialized_exit_bandits) return 0;
    if (ezoo->exit_predictor_count < 2) return 0;  // single-arm: nothing to save
    if (!base_dir || base_dir[0] == '\0') return 0;
    char path[512];
    snprintf(path, sizeof(path), "%s/exit_bandit_state.json", base_dir);
    char bundle_id[65];
    EnsembleModelZoo_ComputeBundleId(ezoo, bundle_id, sizeof(bundle_id));
    return Bandit_SaveJSON(ezoo->exit_bandits, NUM_REGIMES, path,
                            bundle_id, regime_names);
}

// Load bandit state from <base_dir>/bandit_state.json. Returns 1 on
// success (overlays weights/cum_reward/pulls onto pre-initialized
// bandits), 0 on missing/corrupt/mismatched file.
//
// Also captures base_dir into ezoo->bandit_save_path so periodic +
// shutdown save can find it without re-deriving from cfg later.
//
// Caller must call EnsembleModelZoo_InitBandits FIRST to set up the
// uniform priors + arm count + gamma. This function only overlays.
template <unsigned F>
inline int EnsembleModelZoo_LoadBanditState(
    EnsembleModelZoo<F>* ezoo, const char* base_dir) {
    if (!ezoo || !ezoo->active || !ezoo->initialized_bandits) return 0;
    if (!base_dir || base_dir[0] == '\0') return 0;
    char path[512];
    snprintf(path, sizeof(path), "%s/bandit_state.json", base_dir);
    // Capture path for periodic + shutdown save triggers.
    strncpy(ezoo->bandit_save_path, path, sizeof(ezoo->bandit_save_path) - 1);
    ezoo->bandit_save_path[sizeof(ezoo->bandit_save_path) - 1] = '\0';
    char expected_id[65];
    EnsembleModelZoo_ComputeBundleId(ezoo, expected_id, sizeof(expected_id));
    int loaded = Bandit_LoadJSON(ezoo->bandits, NUM_REGIMES, path,
                                   expected_id, ezoo->primary_count);
    if (loaded) {
        fprintf(stderr, "[ensemble] loaded bandit state from %s\n", path);
    } else {
        // Missing or mismatch → caller's prior _InitBandits uniform stays.
        // Don't warn loudly: missing on first run is normal.
        if (access(path, F_OK) == 0) {
            fprintf(stderr, "[ensemble] bandit_state.json present but rejected "
                            "(format/sha/n_arms mismatch); starting uniform\n");
        }
    }
    return loaded;
}

// v5.13.4.C — sell-side bandit load. Mirrors _LoadBanditState for the
// exit_bandits[] array. Reads <base_dir>/exit_bandit_state.json.
// Returns 1 on success, 0 on missing/mismatch (uniform priors stay).
// Caller must call _InitExitBandits FIRST to set up uniform priors.
template <unsigned F>
inline int EnsembleModelZoo_LoadExitBanditState(
    EnsembleModelZoo<F>* ezoo, const char* base_dir) {
    if (!ezoo || !ezoo->initialized_exit_bandits) return 0;
    if (ezoo->exit_predictor_count < 2) return 0;  // single-arm: skip load
    if (!base_dir || base_dir[0] == '\0') return 0;
    char path[512];
    snprintf(path, sizeof(path), "%s/exit_bandit_state.json", base_dir);
    char expected_id[65];
    EnsembleModelZoo_ComputeBundleId(ezoo, expected_id, sizeof(expected_id));
    int loaded = Bandit_LoadJSON(ezoo->exit_bandits, NUM_REGIMES, path,
                                   expected_id, ezoo->exit_predictor_count);
    if (loaded) {
        fprintf(stderr, "[ensemble] loaded exit_bandit state from %s\n", path);
    } else if (access(path, F_OK) == 0) {
        fprintf(stderr, "[ensemble] exit_bandit_state.json present but "
                        "rejected (format/sha/n_arms mismatch); "
                        "starting uniform\n");
    }
    return loaded;
}

// v5.10.0a.next.1 — load bandit state from an EXPLICIT path with optional
// bundle-id check skip. Used by BacktestRunConfig.bandit_state_prior_path
// when operator wants to bootstrap a new ensemble from a sibling bundle's
// learned weights (e.g. transfer learning across runs with the same N
// horizons but different model contents). Returns 1 if loaded.
//
// skip_bundle_check=1 → operator-explicit override; bundle-id mismatch
// is allowed (typical when transferring between sibling models).
// skip_bundle_check=0 → normal path; behaves like _LoadBanditState.
//
// Does NOT update ezoo->bandit_save_path — caller's _LoadBanditState
// (if it ran first) wins for periodic-save destination, OR caller can
// set bandit_save_path explicitly via _LoadBanditState before this.
template <unsigned F>
inline int EnsembleModelZoo_LoadBanditStateFromPath(
    EnsembleModelZoo<F>* ezoo, const char* path, int skip_bundle_check) {
    if (!ezoo || !ezoo->active || !ezoo->initialized_bandits) return 0;
    if (!path || path[0] == '\0') return 0;
    char expected_id[65];
    if (skip_bundle_check) {
        expected_id[0] = '\0';  // empty SHA → Bandit_LoadJSON skips check
    } else {
        EnsembleModelZoo_ComputeBundleId(ezoo, expected_id, sizeof(expected_id));
    }
    int loaded = Bandit_LoadJSON(ezoo->bandits, NUM_REGIMES, path,
                                   expected_id, ezoo->primary_count);
    if (loaded) {
        fprintf(stderr, "[ensemble] loaded bandit prior from %s%s\n", path,
                skip_bundle_check ? " (bundle-id check SKIPPED — operator override)"
                                   : "");
    } else if (access(path, F_OK) == 0) {
        fprintf(stderr, "[ensemble] bandit prior at %s present but rejected "
                        "(format/n_arms mismatch)\n", path);
    }
    return loaded;
}

// Configure periodic save cadence. Called once at boot after
// _LoadBanditState. interval=0 disables periodic; shutdown save still
// fires regardless.
template <unsigned F>
inline void EnsembleModelZoo_SetBanditSaveInterval(
    EnsembleModelZoo<F>* ezoo, int interval) {
    if (!ezoo) return;
    ezoo->bandit_save_interval = (interval < 0) ? 0 : interval;
    ezoo->bandit_update_count = 0;
}

// Periodic save trigger. Called after each Bandit_Update batch
// (TickRewardsFromLookback / TradeCloseReward fold this in). Increments
// bandit_update_count; when count crosses interval threshold, flush
// state to bandit_save_path. No-op if path empty or interval==0.
//
// Locale: fprintf in Bandit_SaveJSON uses "C" locale via global
// LC_NUMERIC; engine boot pins this. Render-thread safety: this fires
// on the slow-path / drainer threads, never in hot path.
template <unsigned F>
inline void EnsembleModelZoo_MaybeSaveBanditPeriodic(
    EnsembleModelZoo<F>* ezoo, int updates_this_call) {
    if (!ezoo || !ezoo->active || !ezoo->initialized_bandits) return;
    if (ezoo->bandit_save_interval <= 0) return;
    if (ezoo->bandit_save_path[0] == '\0') return;
    if (updates_this_call <= 0) return;
    uint64_t before = ezoo->bandit_update_count;
    ezoo->bandit_update_count += (uint64_t)updates_this_call;
    uint64_t threshold = (uint64_t)ezoo->bandit_save_interval;
    // Cross threshold: did before/threshold differ from after/threshold?
    if (before / threshold == ezoo->bandit_update_count / threshold) {
        return;  // didn't cross
    }
    char expected_id[65];
    EnsembleModelZoo_ComputeBundleId(ezoo, expected_id, sizeof(expected_id));
    int ok = Bandit_SaveJSON(ezoo->bandits, NUM_REGIMES,
                               ezoo->bandit_save_path, expected_id, nullptr);
    if (!ok) {
        fprintf(stderr, "[ensemble] periodic bandit save FAILED to %s "
                        "(disk full?); next attempt at +%llu updates\n",
                ezoo->bandit_save_path, (unsigned long long)threshold);
    }
}

#endif // CORE_MODEL_ZOO_HPP
