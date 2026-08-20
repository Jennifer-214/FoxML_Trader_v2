// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[ML_Headers/NodeModelZoo.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE]]
// [SEAM]_[the model-bundle boot seam — every trainer-emitted bundle (stamps + scalers + bandit/Thompson state) enters the engine through these loaders and their verify gates]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the per-node model zoo — 4-role single-zoo + the multi-horizon EnsembleModelZoo sidecar: stamp-gated loading, A6 corrupt-arm ingress, bandit/Thompson learning state + persistence, and the post-load setup registries]
// [CONTAINS]
//   - [STRUCT]_[NodeModelZoo]                (+ [FUNCTION]s: Init / TryLoadRole / LoadFromDir / Free family (LoadLegacy + HasAny) / VerifyExpected)
//   - [STRUCT]_[PerArmBarriers] / [STRUCT]_[PredictionRecord] / [STRUCT]_[PerArmDrift]
//   - [STRUCT]_[EnsembleModelZoo]            (HOT/WARM/COLD tiered; 4 roles x 8 arms)
//   - [FUNCTION]_[ezoo_set_per_arm_barrier]  (+ EnsembleZoo_FinalizeCorrupt — the A6 ingress pair)
//   - [FUNCTION]_[EnsembleModelZoo_Init]     (+ EnsurePrimary)
//   - [FUNCTION]_[EnsembleModelZoo_TickRewardsFromLookback]   (+ RecordPrediction / UpdateDrift / TradeCloseReward — the G.8 family)
//   - [FUNCTION]_[EnsembleModelZoo_InitBandits]               (Exp3 buy/exit + Thompson buy/exit — 4 fns)
//   - [FUNCTION]_[EnsembleModelZoo_SetDisabledHorizons]       (+ Free)
//   - [FUNCTION]_[EnsembleModelZoo_LoadFromCfg] / [EnsembleModelZoo_AutoDetectFromDir]   (+ VerifyGridMemberConsistency)
//   - [FUNCTION]_[EnsembleModelZoo_LoadBanditState]           (G.9 Exp3 persistence family) / [EnsembleModelZoo_LoadThompsonState] (Thompson persistence family) / [EnsembleModelZoo_MaybeSaveBanditPeriodic] (tail family)
//   - [REGISTRY]_[FOREACH_ENSEMBLE_POST_LOAD] / [REGISTRY]_[FOREACH_SINGLE_ZOO_POST_LOAD]   (the PARITY-009..012 structural close)
//   - [FUNCTION]_[NodeModelZoo_CheckStaleModel]
// [REFERENCE]_[INVARIANT]_[[H9] [H22]]
// [REFERENCE]_[DESIGN_SPEC]_[postloadsetup-registry-pattern]
//======================================================================================================
// per-core bundle of role-specific model handles. lets each ML core load
// multiple specialized models (barrier prediction, regime classifier, exit
// timing, buy signal) from a single config-specified directory.
//
// usage:
//   NodeModelZoo<F> zoo;
//   NodeModelZoo_Init(&zoo);
//   NodeModelZoo_LoadFromDir(&zoo, "models/aggressive/", MODEL_BACKEND_XGBOOST);
//   // dispatcher receives &zoo as model_ctx
//   if (zoo.loaded_mask & NODE_MODEL_BARRIER) {
//       float multi[3];
//       Model_PredictMulti(&zoo.barrier, features, n, multi, 3);
//       // multi[0]=stable, multi[1]=peak, multi[2]=valley
//   }
//   NodeModelZoo_Free(&zoo);
//
// directory layout:
//   models/aggressive/
//     barrier.json       # 3-class softmax: stable/peak/valley
//     buy_signal.json    # legacy single-binary (backward compat)
//     regime.json        # multi-class regime classifier (future)
//     exit.json          # exit timing (REAL since E.1.2.C 3-role — the trainer emits it at side=1)
//
// missing files = role disabled (silently no-op). bundle deployment is atomic.
//======================================================================================================
#ifndef NODE_MODEL_ZOO_HPP
#define NODE_MODEL_ZOO_HPP

#include <cstdint>  // v5.15.5.F.4d TECH_DEBT-083 close — explicit IWYU for uintN_t (was transitively pulled)
#include "ModelInference.hpp"
#include "FeatureRegistry.hpp"  // v5.8.6: FEATURE_REGISTRY_HASH() drift catch
#include "../Backtest/LabelFunctions.hpp"  // v5.10.1.A: LABEL_REGISTRY_HASH() drift catch
#include "../CoreFrameworks/ControllerConfig.hpp"  // v5.14.1.B.3: cfg* for X-macro drift check
#include "BanditLearning.hpp"   // v5.10.0a.G.7 — per-regime BanditState in EnsembleModelZoo
#include "ThompsonBandit.hpp"   // v5.14.10.B — per-regime ThompsonBanditState in EnsembleModelZoo (parallel to bandits[])
#include "RidgeBlender.hpp"     // v5.14.0 — Ridge risk-parity blending state on EnsembleModelZoo
#include "PerArmFlagRegistry.hpp"     // v5.15.5.A.2.b — FOREACH_PER_ARM_FLAG for per-arm uint8_t bitmaps
#include "BarrierValidation.hpp"      // v5.15.5.E.0.10 A6 (D-221) — tt::barrier_is_corrupt SSoT predicate + SANE bounds
#include "EzooInitFlagRegistry.hpp"   // v5.15.5.A.2.c — FOREACH_EZOO_INIT_FLAG bit-pack for init state
#include "../Strategies/StrategyInterface.hpp"  // v5.10.0a.G.7 — NUM_REGIMES
#include "../Version.hpp"        // v5.8.6: ENGINE_VERSION_STRING for boot log
#include "../MemHeaders/HealthLog.hpp"  // v5.14.8.E: Health_LogCriticalRateLimited for stale-model log
#include "BuildFlags.hpp"  // v5.15.1: tt::BUILD_FLAGS_HASH() for FOREACH_ARCH_FIELD_DRIFT
#include "../MemHeaders/ArchFieldDriftRegistry.hpp"  // v5.15.1: FOREACH_ARCH_FIELD_DRIFT for arch-field drift detection at TryLoadRole chokepoint
#include <stdio.h>
#include <string.h>
#include <stdlib.h>      // v5.10.0a.G.5 — strtol for AutoDetect horizon parse
#include <sys/stat.h>
#include <sys/types.h>   // v5.10.0a.G.5 — dirent for AutoDetect filesystem scan
#include <unistd.h>      // v5.10.0a.G.9 — access() for bandit_state.json probe
#include <dirent.h>      // v5.10.0a.G.5 — opendir/readdir for AutoDetect

// role bitmap — set in zoo->loaded_mask when a model is successfully loaded
#define NODE_MODEL_BARRIER     (1u << 0)  // 3-class softmax: stable/peak/valley
#define NODE_MODEL_REGIME      (1u << 1)  // multi-class regime classifier
#define NODE_MODEL_EXIT        (1u << 2)  // exit timing model
#define NODE_MODEL_BUY_SIGNAL  (1u << 3)  // legacy single-binary buy signal

//======================================================================
// [STRUCT]_[NodeModelZoo]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the per-node 4-role model bundle (barrier/regime/exit/buy_signal) + loaded_mask + the v5.11.62 primary-role indirection strategy code reads]
// [INSTANTIATION]_[[64]]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
struct alignas(64) NodeModelZoo {
    ModelHandle<F> barrier;     // 3-class P(stable)/P(peak)/P(valley) when num_outputs=3
    ModelHandle<F> regime;      // multi-class regime
    ModelHandle<F> exit;        // exit timing
    ModelHandle<F> buy_signal;  // legacy single-binary
    unsigned int loaded_mask;   // bitmap of loaded roles (NODE_MODEL_*)
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
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// v5.15.4 — alignas(64) required so heap-allocated NodeModelZoo via
// aligned_alloc(64) (HotSwap_ShadowLoad_SingleZoo) gives the embedded
// ModelHandle<F> members (which are themselves alignas(64) since v5.15.0)
// correctly-aligned addresses. Without container-level alignas(64), heap
// allocation via plain malloc gives only 16-byte alignment + AVX-512
// vector-load fields inside ModelHandle could fault or run slow.
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-08-16]
//----------------------------------------------------------------------
// [SIZE]_[26688B]
// [ALIGN]_[64]
// [CACHE_LINES]_[417]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[NodeModelZoo]
//======================================================================
// v5.15.4 — size%64==0 invariant for shadow-load aligned_alloc(64).
// Compiler enforces alignment + size %16 == 0 (alignas implies); we also
// want size %64 == 0 so adjacent heap-allocated NodeModelZoo don't share
// cache lines with neighboring allocations + cluster cleanly. Each
// ModelHandle<F=64> is itself 64-byte aligned, so the struct sizeof
// is already a multiple of 64 (verified by static_assert).
// [ASSERT]_[LAYOUT_LOCK]_[sizeof(NodeModelZoo<64>) % 64 == 0]
static_assert(sizeof(NodeModelZoo<64>) % 64 == 0,
              "v5.15.4: NodeModelZoo<64> size must be multiple of 64 for cache-line discipline");
// [ASSERT]_[LAYOUT_LOCK]_[alignof(NodeModelZoo<64>) == 64]
static_assert(alignof(NodeModelZoo<64>) == 64,
              "v5.15.4: NodeModelZoo<64> must be cache-line aligned");

//======================================================================
// [FUNCTION]_[NodeModelZoo_Init]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[zero-state init — all 4 role handles + mask + primary indirection cleared]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline void NodeModelZoo_Init(NodeModelZoo<F> *zoo) {
    Model_Init(&zoo->barrier);
    Model_Init(&zoo->regime);
    Model_Init(&zoo->exit);
    Model_Init(&zoo->buy_signal);
    zoo->loaded_mask = 0;
    zoo->primary_handle = nullptr;
    zoo->primary_target_class = 0;
    zoo->primary_role_name[0] = '\0';
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[NodeModelZoo_Init]
//======================================================================

//======================================================================
// [FUNCTION]_[NodeModelZoo_TryLoadRole]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the per-role load gate — stamp verify (held-out gate + drift walkers) -> Model_Load -> stamp-derived handle copies -> scaler sidecar 3-tier verify -> the v5.15.1 drift chokepoint]
// [REFERENCE]_[INVARIANT]_[[H9] [H20]]
// [REFERENCE]_[CLASS]_[18]
// [REFERENCE]_[PARITY]_[[PARITY-4] [PARITY-5]]
// [REFERENCE]_[TECH_DEBT]_[TECH_DEBT-4]
//======================================================================
// [CODE]
//======================================================================
// E.1.2.C 3-role (F2) — the role-check decision, extracted PURE so the full
// slot x key-state x strict table is table-tested (the D2 verdict's pinned
// table). slot semantics: exit slot <=> role_name "exit"; buy slots keep
// legacy (keyless) tolerance; exit slots have ZERO legacy population so a
// keyless stamp there REFUSES in strict. strict==-1 never parses a stamp,
// so callers never reach this with it — kept in the table for totality.
enum RoleCheckDecision { ROLE_CHECK_PASS = 0, ROLE_CHECK_WARN = 1, ROLE_CHECK_REFUSE = 2 };
static inline int Model_RoleCheckDecide(const char* slot_role, const char* stamp_role,
                                        int has_role, int strict) {
    if (strict == -1) return ROLE_CHECK_PASS;
    const int exit_slot = (strcmp(slot_role, "exit") == 0);
    if (!has_role) {
        if (!exit_slot) return ROLE_CHECK_PASS;
        return (strict == 1) ? ROLE_CHECK_REFUSE : ROLE_CHECK_WARN;
    }
    if (strcmp(stamp_role, slot_role) == 0) return ROLE_CHECK_PASS;
    return (strict == 1) ? ROLE_CHECK_REFUSE : ROLE_CHECK_WARN;
}

template <unsigned F>
inline int NodeModelZoo_TryLoadRole(ModelHandle<F> *handle, const char *dir,
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
            // v5.15.5.F.4d.1.B.3 Step 1.6.6 (Decision B (a); codified at v1.12 plan body) —
            // legacy FOREACH_STAMP_BOUND_CFG drift walker replaced with framework
            // DRIFT_CHECK_FROM_DERIVED macro wrapper. Framework template fn at
            // CfgGateRegistry.hpp:332+ walks 4 master cfg registries filtered by
            // STAMP_BOUND_CFG_DERIVED bit; mask-select per H20 branchless; first-failure-wins
            // reason buffer attribution preserved per Step 0.5a primitive.
            // FAILURE_MASK symbol: FAILURE_MASK_cfg_binding_drift (per CRIT-CONV-4).
            uint64_t failure_flags = 0;
            DRIFT_CHECK_FROM_DERIVED(failure_flags, sr, cfg, sr.inference_cfg_drift_count, sr.reason, sizeof(sr.reason));
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
        // v5.15.0 — stamp-derived field copies now mirror ModelStampResult
        // canonical names; STAMP_SET writes bit-packed has_flags. Each
        // group/standalone bit follows the FOREACH_STAMP_BOUND_MODEL_CONST
        // bit allocation (registry's enum StampHasFlagBit).
        //
        // FUTURE OPPORTUNITY: extract this entire block into a
        // STAMP_HANDLE_COPY_FROM_RESULT(handle, sr) companion macro
        // mirroring STAMP_MODEL_CONST_AUTOPOPULATE. Walks the registry +
        // emits per-entry copy + STAMP_SET. Closes the Class 18 mirror
        // for handle/result copy at the same surface AUTOPOPULATE closed
        // for emit. Deferred from v5.15.0 to keep .A bounded; ~30 LOC
        // new infrastructure when triggered.
        if (STAMP_HAS(sr, training_poll_interval)) {
            handle->training_poll_interval = sr.training_poll_interval;
            STAMP_SET(*handle, training_poll_interval);
        }
        // v5.9.5h — copy XGBoost hyperparams from stamp onto handle.
        // EngineSharded boot-WARN compares xgb_* vs cfg.xgb_*. No refusal —
        // hyperparams don't affect inference, only forensics + reproducibility.
        if (STAMP_HAS(sr, xgb_hyperparams)) {
            STAMP_SET(*handle, xgb_hyperparams);
            handle->xgb_max_depth        = sr.xgb_max_depth;
            handle->xgb_learning_rate    = sr.xgb_learning_rate;
            handle->xgb_n_estimators     = sr.xgb_n_estimators;
            handle->xgb_subsample        = sr.xgb_subsample;
            handle->xgb_colsample_bytree = sr.xgb_colsample_bytree;
            handle->xgb_min_child_weight = sr.xgb_min_child_weight;
            handle->xgb_seed             = sr.xgb_seed;
            size_t tmln = strnlen(sr.xgb_tree_method,
                                   sizeof(handle->xgb_tree_method) - 1);
            memcpy(handle->xgb_tree_method, sr.xgb_tree_method, tmln);
            handle->xgb_tree_method[tmln] = '\0';
        }
        // v5.9.5h Phase 10 — build flags fingerprint
        if (STAMP_HAS(sr, build_flags_hash)) {
            STAMP_SET(*handle, build_flags_hash);
            handle->build_flags_hash = sr.build_flags_hash;
        }
        // v5.11.42 D.1 — copy stamp's xgb_train_nthread for engine boot WARN.
        if (STAMP_HAS(sr, xgb_train_nthread)) {
            STAMP_SET(*handle, xgb_train_nthread);
            handle->xgb_train_nthread = sr.xgb_train_nthread;
        }
        // v5.11.42 D.2 — copy stamp's label params for ensemble dir-name
        // horizon-mismatch refusal at AutoDetect time.
        if (STAMP_HAS(sr, label_params)) {
            STAMP_SET(*handle, label_params);
            handle->label_lookahead_ticks = sr.label_lookahead_ticks;
            handle->label_tp_pct          = sr.label_tp_pct;
            handle->label_sl_pct          = sr.label_sl_pct;
        }
        // v5.11.42 D.3 — copy stamp's scaler_sha256 for ensemble-sibling
        // consistency WARN.
        if (STAMP_HAS(sr, scaler) && sr.scaler_sha256[0] != '\0') {
            STAMP_SET(*handle, scaler);
            size_t n = strnlen(sr.scaler_sha256,
                               sizeof(handle->scaler_sha256) - 1);
            memcpy(handle->scaler_sha256, sr.scaler_sha256, n);
            handle->scaler_sha256[n] = '\0';
        }
        // v5.14.3.B — copy stamp's overlay-derived fields for
        // FeatureOverlay_PostLoadVerify. Forward-compat: legacy stamps
        // (overlay_hash bit unset) leave handle's overlay_hash empty;
        // verify skips silently.
        if (STAMP_HAS(sr, overlay_hash) && sr.overlay_hash[0] != '\0') {
            STAMP_SET(*handle, overlay_hash);
            size_t n = strnlen(sr.overlay_hash,
                               sizeof(handle->overlay_hash) - 1);
            memcpy(handle->overlay_hash, sr.overlay_hash, n);
            handle->overlay_hash[n] = '\0';
        }
        if (STAMP_HAS(sr, effective_hash) && sr.effective_hash[0] != '\0') {
            STAMP_SET(*handle, effective_hash);
            size_t n = strnlen(sr.effective_hash,
                               sizeof(handle->effective_hash) - 1);
            memcpy(handle->effective_hash, sr.effective_hash, n);
            handle->effective_hash[n] = '\0';
        }
        // v5.14.8.E — copy stale-model gate fields from stamp to handle.
        // Read by NodeModelZoo_CheckStaleModel at boot.
        if (STAMP_HAS(sr, training_timestamp_us)) {
            STAMP_SET(*handle, training_timestamp_us);
            handle->training_timestamp_us = sr.training_timestamp_us;
        }
        if (STAMP_HAS(sr, run_name) && sr.run_name[0] != '\0') {
            STAMP_SET(*handle, run_name);
            size_t n = strnlen(sr.run_name, sizeof(handle->run_name) - 1);
            memcpy(handle->run_name, sr.run_name, n);
            handle->run_name[n] = '\0';
        }
        // v5.9.5i — copy stamp's inference cfg values. EngineSharded
        // boot-WARN/REFUSE compares vs cfg.*. Forward-compat: legacy
        // stamps (inference_cfg bit unset) leave handle's
        // inference_cfg_* at Model_Init zero defaults; comparison skipped.
        if (STAMP_HAS(sr, inference_cfg)) {
            STAMP_SET(*handle, inference_cfg);
            handle->confidence_threshold_scale =
                sr.confidence_threshold_scale;
            handle->barrier_gate_enabled =
                sr.barrier_gate_enabled;
            handle->confidence_hard_block_threshold =
                sr.confidence_hard_block_threshold;
            // v5.14.9.D — DELETED inference_cfg_freshness_tau population
            // (TECH_DEBT-004 close); registry entry + ModelHandle field deleted.
        }
        // 2026-08-17 (D-426) — the `inference_cfg_bandit_blend_ratio` sr→handle copy was REMOVED
        // with its row, for the identical reason as the `fees` copy described immediately below:
        // it copied a permanently-zero field onto the handle. This is the FOURTH site of one
        // pattern (fees emit → bandit emit → bandit panel display → this copy), and each time the
        // `fees` twin was removed the `bandit` sibling three lines away survived, because the
        // sweeps were scoped by what the previous sweep had touched rather than by tracing the
        // key. The lesson is in the sweep's SHAPE, not in any of the four sites.
        // 2026-08-16 — the `fees` sr→handle copy was REMOVED with the group. It copied
        // two permanently-zero fields onto the handle, where they had NO readers except
        // the operator panel that displayed them as the model's training-time fees.
        if (STAMP_HAS(sr, model_num_outputs)) {
            STAMP_SET(*handle, model_num_outputs);
            handle->model_num_outputs = sr.model_num_outputs;
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
        // E.1.2.C leg 2 (2026-08-20) — PARITY-043 close: copy the parsed
        // cfg-derived cohort sr -> handle (the fifth walker; per-field
        // has_-gated). Before this, 33 of 36 cohort fields were parsed into
        // the local `sr` and DISCARDED, so NodeModelZoo_ValidateAgainstCfg's
        // drift rows compared handle-side zeros against live cfg — two
        // REFUSE_STRICT rows (thompson_precision_prior/_obs) false-fired on
        // every load at a default cfg. Placed after Model_Load success +
        // the MODEL_CONST copies, before the horizon refusal, so a refused
        // load never carries copied state. One chokepoint covers boot,
        // both hot-swap paths, and backtest (all route through TryLoadRole).
        COPY_RESULT_TO_HANDLE_FROM_DERIVED(*handle, sr);
        // E.1.2.C 3-role (F2, per the D2 verdict: O1-only) — enforce the
        // EXISTING expected_role key (emitted since v5.15.3, checked by
        // NOTHING until now). The stamp's recorded role must match the slot
        // being loaded — a renamed buy model in an exit slot REFUSES instead
        // of silently trading inverted. Decision table: Model_RoleCheckDecide
        // above (pure, table-tested). WARN arm flags ml_role_mismatch on
        // drift_flags_at_load — rides the fixed dual-walk aggregation.
        {
            const int rc_role = Model_RoleCheckDecide(
                role_name, sr.expected_role,
                STAMP_HAS(sr, expected_role) ? 1 : 0, held_out_gate_strict);
            if (rc_role == ROLE_CHECK_REFUSE) {
                fprintf(stderr,
                    "[model] REFUSING %s — stamp expected_role='%s' vs slot '%s'%s (strict mode)\n",
                    found_path,
                    STAMP_HAS(sr, expected_role) ? sr.expected_role : "(absent)",
                    role_name,
                    STAMP_HAS(sr, expected_role) ? "" : " — keyless stamp in an exit slot");
                Model_Free(handle);
                Model_Init(handle);
                return 0;
            }
            if (rc_role == ROLE_CHECK_WARN) {
                fprintf(stderr,
                    "[model] WARN: %s stamp expected_role='%s' vs slot '%s' (strict=0, loading anyway)\n",
                    found_path,
                    STAMP_HAS(sr, expected_role) ? sr.expected_role : "(absent)",
                    role_name);
                BITMAP_SET(handle->drift_flags_at_load, FAILURE_MASK_ml_role_mismatch);
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
        // label_params (bit unset) skip the check.
        if (expected_horizon_ticks > 0 && STAMP_HAS(sr, label_params) &&
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
            // warn-mode: identity applied, surface to operator via PerNodeSnap
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

    //------------------------------------------------------------------
    // [SECTION]_[v5.15.1 — drift detection chokepoint]
    //------------------------------------------------------------------
    // Single source of truth per handle for arch-field + CFG + HMAC +
    // model-age drift bits.
    // Sets bits on handle->drift_flags_at_load using FOREACH_FAILURE_MODE
    // BIT_FLAG positions. ShardedSnapshot_Publish OR-aggregates across
    // all 4 zoo roles into PerNodeSnap.failure_flags for GUI Model Health
    // panel + (future v5.15.2) live-readiness boot gate consumption.
    //
    // Boot-only path; runs once per handle load. No slow-path or hot-path
    // cost. Per the NaN-free chokepoint precedent: one
    // chokepoint per concern, not scattered checks.
    //------------------------------------------------------------------
    if (have_sr) {
        // (1) Arch-field drift checks (registry-driven; auto-flows for
        //     future entries via FOREACH_ARCH_FIELD_DRIFT).
        #define X(name, stamp_field, runtime_value, fail_mask) \
            if ((stamp_field) != (runtime_value)) { \
                BITMAP_SET(handle->drift_flags_at_load, fail_mask); \
            }
        FOREACH_ARCH_FIELD_DRIFT(X)
        #undef X

        // (2) CFG_BINDING_DRIFT aggregate bit (single-fact; consolidates
        //     with the X-macro drift check earlier in this fn, which already
        //     counted drifts into sr.inference_cfg_drift_count).
        if (sr.inference_cfg_drift_count > 0) {
            BITMAP_SET(handle->drift_flags_at_load, FAILURE_MASK_cfg_binding_drift);
        }

        // (3) STAMP_HMAC_NOT_VERIFIED — set when caller invoked
        //     verify_model_stamp with empty secret (dev mode; HMAC sig
        //     was logged but not verified). v5.15.2 boot gate REFUSEs
        //     when trading_mode=live + this bit set.
        if (held_out_stamp_secret == nullptr || held_out_stamp_secret[0] == '\0') {
            BITMAP_SET(handle->drift_flags_at_load, FAILURE_MASK_stamp_hmac_not_verified);
        }

        // (4) MODEL_AGE_WARN — set when stamp has training_timestamp_us
        //     + cfg.model_max_age_hours > 0 + age exceeds threshold.
        //     Mirrors NodeModelZoo_CheckStaleModel's age check but sets
        //     a snapshot-publishable bit (vs CheckStaleModel's CRITICAL
        //     log path). Both paths fire independently.
        if (cfg_ptr && cfg_ptr->model_max_age_hours > 0 &&
            STAMP_HAS(sr, training_timestamp_us) && sr.training_timestamp_us > 0) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            uint64_t now_us = (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
            if (now_us > sr.training_timestamp_us) {
                uint64_t age_hours = (now_us - sr.training_timestamp_us) /
                                     (3600ULL * 1000000ULL);
                if (age_hours > cfg_ptr->model_max_age_hours) {
                    BITMAP_SET(handle->drift_flags_at_load, FAILURE_MASK_model_age_warn);
                }
            }
        }
    }

    return rc;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// try to load a single model from <dir>/<role>.json, falling back to <role>.xgb
// returns 1 if loaded, 0 if file not found or load failed
//
// v5.2.0: held-out gate. When `secret` is non-null + `strict != 0`, refuse
// to load a model file that doesn't have a valid `.stamp` sibling. See
// `verify_model_stamp` in ModelInference.hpp.
//
// Default args preserve pre-v5.2.0 callers — no gate when secret==nullptr.
//======================================================================
// [END_FUNCTION]_[NodeModelZoo_TryLoadRole]
//======================================================================

//======================================================================
// [FUNCTION]_[NodeModelZoo_LoadFromDir]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[auto-discover + load all 4 roles from a bundle dir (missing roles silently disabled) then pick the primary-role indirection: buy_signal > barrier > regime]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline int NodeModelZoo_LoadFromDir(NodeModelZoo<F> *zoo, const char *dir, int backend,
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
    if (NodeModelZoo_TryLoadRole(&zoo->barrier, dir, "barrier", backend,
            held_out_stamp_secret, gap_threshold, held_out_gate_strict,
            acknowledge_cross_binary_drift, expected_feature_mask,
            /*expected_horizon_ticks=*/0, cfg_ptr)) {
        zoo->loaded_mask |= NODE_MODEL_BARRIER;
        loaded++;
    }
    if (NodeModelZoo_TryLoadRole(&zoo->regime, dir, "regime", backend,
            held_out_stamp_secret, gap_threshold, held_out_gate_strict,
            acknowledge_cross_binary_drift, expected_feature_mask,
            /*expected_horizon_ticks=*/0, cfg_ptr)) {
        zoo->loaded_mask |= NODE_MODEL_REGIME;
        loaded++;
    }
    if (NodeModelZoo_TryLoadRole(&zoo->exit, dir, "exit", backend,
            held_out_stamp_secret, gap_threshold, held_out_gate_strict,
            acknowledge_cross_binary_drift, expected_feature_mask,
            /*expected_horizon_ticks=*/0, cfg_ptr)) {
        zoo->loaded_mask |= NODE_MODEL_EXIT;
        loaded++;
    }
    if (NodeModelZoo_TryLoadRole(&zoo->buy_signal, dir, "buy_signal", backend,
            held_out_stamp_secret, gap_threshold, held_out_gate_strict,
            acknowledge_cross_binary_drift, expected_feature_mask,
            /*expected_horizon_ticks=*/0, cfg_ptr)) {
        zoo->loaded_mask |= NODE_MODEL_BUY_SIGNAL;
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
    if (zoo->loaded_mask & NODE_MODEL_BUY_SIGNAL) {
        zoo->primary_handle = &zoo->buy_signal;
        zoo->buy_signal.buy_class_idx = 0;
        strncpy(zoo->primary_role_name, "buy_signal",
                sizeof(zoo->primary_role_name) - 1);
    } else if (zoo->loaded_mask & NODE_MODEL_BARRIER) {
        zoo->primary_handle = &zoo->barrier;
        zoo->barrier.buy_class_idx = (zoo->barrier.num_outputs >= 2) ? 1 : 0;
        zoo->primary_target_class = zoo->barrier.buy_class_idx;
        strncpy(zoo->primary_role_name, "barrier",
                sizeof(zoo->primary_role_name) - 1);
    } else if (zoo->loaded_mask & NODE_MODEL_REGIME) {
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
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// auto-discover and load all roles present in `dir`. missing roles silently
// disabled. returns the number of roles loaded.
//======================================================================
// [END_FUNCTION]_[NodeModelZoo_LoadFromDir]
//======================================================================

//======================================================================
// [FUNCTION]_[NodeModelZoo_Free]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[lifecycle tail family (LoadLegacy single-path fallback + HasAny predicate ride) — free all 4 role handles + clear the mask]
//======================================================================
// [CODE]
//======================================================================
// legacy single-model fallback: load just the buy_signal role from a single
// path (backward compat with the old core_N_model_path config field).
template <unsigned F>
inline int NodeModelZoo_LoadLegacy(NodeModelZoo<F> *zoo, const char *path, int backend) {
    if (!path || path[0] == '\0') return 0;
    if (Model_Load(&zoo->buy_signal, path, backend)) {
        zoo->loaded_mask |= NODE_MODEL_BUY_SIGNAL;
        return 1;
    }
    return 0;
}

template <unsigned F>
inline void NodeModelZoo_Free(NodeModelZoo<F> *zoo) {
    Model_Free(&zoo->barrier);
    Model_Free(&zoo->regime);
    Model_Free(&zoo->exit);
    Model_Free(&zoo->buy_signal);
    zoo->loaded_mask = 0;
}

template <unsigned F>
inline int NodeModelZoo_HasAny(const NodeModelZoo<F> *zoo) {
    return zoo->loaded_mask != 0;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[NodeModelZoo_Free]
//======================================================================

//======================================================================
// [FUNCTION]_[NodeModelZoo_VerifyExpected]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the STUPID-PROOF verify — expected.cfg (written by foxml_suite Save Run) vs live ML cfg; cadence + feature-format + gate/threshold/class mismatches; strict_mode fails the load]
//======================================================================
// [CODE]
//======================================================================
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
template <unsigned F>
// v4.3.1 — extended signature to also verify slow-path cadence + feature
// pack version. live_poll_interval and live_feature_format_version are
// the engine's runtime values; the loader compares them against what
// expected.cfg recorded at training time. Mismatch on cadence = silent
// train-serve drift; mismatch on feature format = wrong number of
// features in the pack, model crashes or produces garbage.
inline int NodeModelZoo_VerifyExpected(const NodeModelZoo<F> *zoo, const char *dir,
                                       int live_barrier_gate_enabled,
                                       double live_ml_buy_threshold,
                                       int strict_mode, int node_id,
                                       unsigned live_poll_interval = 0,
                                       unsigned live_feature_format_version = 0) {
    // structural check: multiclass model + barrier_gate_enabled=0 → warn
    int has_multiclass = (zoo->loaded_mask & NODE_MODEL_BARRIER) && zoo->barrier.num_outputs >= 2;
    if (has_multiclass && !live_barrier_gate_enabled) {
        fprintf(stderr, "[ML] node %d: WARNING — model has %d output classes (multiclass softmax)\n"
                        "                  but barrier_gate_enabled=0. only P(valley) used,\n"
                        "                  P(peak)/P(stable) ignored. set barrier_gate_enabled=1\n"
                        "                  to use the full model.\n",
                node_id, zoo->barrier.num_outputs);
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
            "[ML] node %d: MISMATCH — model trained at poll_interval=%d, "
            "engine running at poll_interval=%u\n"
            "                  RollingStats time-windows differ %.1f×; "
            "predictions will diverge from training distribution.\n"
            "                  Set engine.cfg poll_interval=%d to match.\n",
            node_id, expected_poll_interval, live_poll_interval,
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
            "[ML] node %d: FATAL — model trained with feature_format=v%d "
            "but engine runtime is v%u. Feature indices differ; model "
            "would interpret inputs as wrong features.\n"
            "                  Retrain the model on the current engine.\n",
            node_id, expected_feature_format_ver, live_feature_format_version);
        mismatches++;
    }

    // compare each field, log mismatches
    if (expected_barrier_gate >= 0 && expected_barrier_gate != live_barrier_gate_enabled) {
        fprintf(stderr, "[ML] node %d: MISMATCH — expected.cfg says barrier_gate_enabled=%d, "
                        "engine.cfg has %d\n",
                node_id, expected_barrier_gate, live_barrier_gate_enabled);
        mismatches++;
    }
    if (expected_threshold >= 0.0 &&
        (live_ml_buy_threshold < expected_threshold - 0.001 ||
         live_ml_buy_threshold > expected_threshold + 0.001)) {
        fprintf(stderr, "[ML] node %d: MISMATCH — expected.cfg says ml_buy_threshold=%.3f, "
                        "engine.cfg has %.3f\n",
                node_id, expected_threshold, live_ml_buy_threshold);
        mismatches++;
    }
    if (expected_num_classes >= 2 && (zoo->loaded_mask & NODE_MODEL_BARRIER) &&
        zoo->barrier.num_outputs != expected_num_classes) {
        fprintf(stderr, "[ML] node %d: MISMATCH — expected.cfg says %d classes, "
                        "loaded model has %d outputs\n",
                node_id, expected_num_classes, zoo->barrier.num_outputs);
        mismatches++;
    }

    // Phase 7 prep — log discipline values informationally so the user knows
    // what validation regime the model was trained under. Not compared to
    // live cfg (yet); add comparison if drift becomes a real concern.
    if (expected_held_out_fraction >= 0.0 || expected_gap_threshold >= 0.0) {
        fprintf(stderr, "[ML] node %d: validation discipline — held_out=%.2f gap_threshold=%.3f\n",
                node_id,
                expected_held_out_fraction >= 0.0 ? expected_held_out_fraction : 0.0,
                expected_gap_threshold     >= 0.0 ? expected_gap_threshold     : 0.0);
    }

    if (mismatches == 0) {
        fprintf(stderr, "[ML] node %d: expected.cfg verified (role=%s, %d classes) ✓\n",
                node_id, expected_role[0] ? expected_role : "?",
                expected_num_classes >= 0 ? expected_num_classes : 0);
        return 1;
    }

    if (strict_mode > 0) {
        fprintf(stderr, "[ML] node %d: %d MISMATCH(ES) — STRICT MODE refusing to load.\n"
                        "                update engine.cfg to match expected.cfg, or set\n"
                        "                model_verify_strict=0 to override.\n",
                node_id, mismatches);
        return 0;
    } else {
        fprintf(stderr, "[ML] node %d: %d mismatch(es) — model may not behave as trained.\n"
                        "                fix engine.cfg to silence these warnings.\n",
                node_id, mismatches);
        return 1;
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[NodeModelZoo_VerifyExpected]
//======================================================================

//----------------------------------------------------------------------
// [SECTION]_[v5.10.0a.G.3 — ENSEMBLE MODEL ZOO (multi-horizon sidecar struct)]
//----------------------------------------------------------------------
// EnsembleModelZoo lives ALONGSIDE NodeModelZoo (not replacing it).
// Single-horizon callers use NodeModelZoo unchanged; multi-horizon
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

// v5.15.5.E.0.10 A6 (D-221) — the per-arm bitmaps (disabled_horizon_mask /
// arms_with_barriers_mask / corrupt_arms_mask) are uint8_t = 8 bits = 8 arms.
// Pin the width to the arm count (bitmap-overflow-protection-discipline): widen
// the masks (uint8 -> uint16) if ENSEMBLE_HORIZON_MAX ever grows past 8.
// [ASSERT]_[BITMAP_OVERFLOW]_[ENSEMBLE_HORIZON_MAX <= 8 — per-arm uint8_t bitmap width pin]
static_assert(ENSEMBLE_HORIZON_MAX <= 8,
              "per-arm uint8_t bitmaps hold 8 arms; widen the masks if ENSEMBLE_HORIZON_MAX grows");

//======================================================================
// [STRUCT]_[PerArmBarriers]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [DATA_ORIENTED_DESIGN]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the AoS (tp, sl) pair — one cache-line read at DOMINANT-mode dispatch fetches BOTH barriers for the dominant arm; 8 pairs = exactly 64B]
// [REFERENCE]_[DESIGN_SPEC]_[per-horizon-barrier-blending-with-shadow-mode]
//======================================================================
// [CODE]
//======================================================================
struct PerArmBarriers {
    float tp;  // label_tp_pct from stamp body (zero when stamp lacks the field)
    float sl;  // label_sl_pct from stamp body
};
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// v5.15.5.A.2.a — per-arm trained barriers (Layout C AoS).
// Each entry pairs tp + sl floats so a single cache-line read at
// DOMINANT-mode dispatch (read per_arm_barriers[h]) fetches BOTH
// barriers for the dominant arm. struct{tp,sl} × 8 = 64 bytes
// exactly = 1 cache line; alignas(64) prevents straddling.
// Populated at LoadFromCfg per-arm copy site from each handle's
// stamp-body label_tp_pct/label_sl_pct (already loaded by the
// NodeModelZoo_TryLoadRole label_params copy).
// Rationale: cache-miss cost (~100ns cold) is 75-100× cycle cost
// per the latency-vs-cache decision framework;
// 1-cache-line read profile across both DOMINANT and BLEND modes
// wins over SoA double[8] separate arrays (2 cache lines per
// DOMINANT lookup). Pattern documented in
// DESIGN_SPECS/per-horizon-barrier-blending-with-shadow-mode.md.
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
//----------------------------------------------------------------------
// [SIZE]_[8B]
// [ALIGN]_[4]
// [CACHE_LINES]_[1]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[PerArmBarriers]
//======================================================================

//======================================================================
// [STRUCT]_[PredictionRecord]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[reward-attribution ring record (G.8) — per-predict per-arm outputs + regime + price + the two rewarded flags]
//======================================================================
// [CODE]
//======================================================================
struct PredictionRecord {
    uint64_t predict_call;        // monotonic counter (increments per predict)
    int      regime_id;           // regime AT predict time (for attribution)
    float    predictions[ENSEMBLE_HORIZON_MAX];  // per-arm raw outputs
    float    sample_price;        // price at predict time
    uint8_t  rewarded_lookback;   // 1 = already rewarded by slow-path lookback
    uint8_t  rewarded_trade;      // 1 = already rewarded by trade-close
};
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// v5.10.0a.G.8 — reward attribution ring buffer record (used in WARM cluster
// below). Each predict writes a record; slow-path lookback walks ring for
// reward attribution.
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
//----------------------------------------------------------------------
// [SIZE]_[56B]
// [ALIGN]_[8]
// [CACHE_LINES]_[1]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[PredictionRecord]
//======================================================================

//======================================================================
// [STRUCT]_[PerArmDrift]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[drift-watchdog state per arm (G.8) — IC history ring + running average + the sticky demoted flag]
//======================================================================
// [CODE]
//======================================================================
// v5.10.0a.G.8 — drift watchdog state (used in WARM cluster below).
struct PerArmDrift {
    float    ic_history[100];  // recent reward outcomes; capped at DRIFT_IC_HISTORY
    int      ic_count;          // populated entries
    float    ic_avg;            // running average
    uint8_t  demoted;           // 1 = forced near-zero weight; sticky until recovery
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
//----------------------------------------------------------------------
// [SIZE]_[412B]
// [ALIGN]_[4]
// [CACHE_LINES]_[7]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[PerArmDrift]
//======================================================================

//======================================================================
// [STRUCT]_[EnsembleModelZoo]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [DATA_ORIENTED_DESIGN]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the multi-horizon sidecar — 4 roles x 8 arms of handles + per-regime bandit/Thompson/Ridge state + reward ring + drift watchdogs, tiered HOT/WARM/COLD]
// [INSTANTIATION]_[[64]]
// [REFERENCE]_[DESIGN_SPEC]_[[cache-layout-discipline-for-hot-side-structs] [sink-fn-pointer-for-optional-side-effect-pattern.md]]
// [REFERENCE]_[CLASS]_[[24] [28]]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
struct alignas(64) EnsembleModelZoo {
    //---- [SECTION]_[HOT CLUSTER — small fields touched every slow-path cycle when ensemble active] ----
    // Per DESIGN_SPECS/cache-layout-discipline-for-hot-side-
    // structs.md Rule 4 (Hot/Warm/Cold tier clustering): small high-frequency
    // fields cluster at struct start so a single L1 fetch covers all of them
    // per slow-path cycle. Large arrays follow (each gets its own cache lines
    // by virtue of size + alignment).

    // Per-arm barriers (Layout C AoS; alignas(64) = 1 cache line for all 8 arms).
    alignas(64) PerArmBarriers per_arm_barriers[ENSEMBLE_HORIZON_MAX];

    // Small hot scalars — counts, indices, masks (the per-cycle gate inputs).
    int barrier_count;
    int regime_count;
    int exit_predictor_count;
    int buy_signal_count;
    int horizon_ticks_at_idx[ENSEMBLE_HORIZON_MAX];
    // v5.15.5.A.2.c — init flags bit-pack via FOREACH_EZOO_INIT_FLAG registry.
    // 4 bits used (ACTIVE, BANDITS_READY, EXIT_BANDITS_READY, THOMPSON_READY);
    // 4 free for future flags. Access: BITMAP_IS_SET(ezoo->init_flags, MASK_EZOO_*).
    uint8_t init_flags;
    // v5.15.5.A.2.b — per-arm flag bitmaps auto-generated from FOREACH_PER_ARM_FLAG:
    // disabled_horizon_mask + arms_with_barriers_mask (uint8_t each, 8 arms = 8 bits).
    PER_ARM_FLAG_DECLARE_FIELDS()

    // Per-prediction tracking (per-cycle writes).
    int last_predicted_regime_id;      // regime AT predict-time (G.8 attribution)
    int last_predicted_horizon_idx;    // dominant horizon idx (display + G.8 reward)
    int last_predicted_exit_horizon_idx; // dominant exit_predictor arm at predict time
    int last_predicted_buy_thompson_arm;   // Thompson's argmax-of-posterior (buy-side; cfg=2 telemetry)
    // v5.15.5.F.4d — exit-side Thompson mirror per FOREACH_BANDIT_SIDE auto-mirror (§ G.2 of merged plan body)
    int last_predicted_exit_thompson_arm;  // Thompson's argmax-of-posterior (exit-side; cfg=2/3 telemetry)
    int32_t _padding_exit_thompson = 0;    // explicit padding for byte-determinism (sister to OrderPreResolved._padding pattern)

    // v5.15.5.F.4d — Pattern 5 sink-fn-pointers per sink-fn-pointer-for-optional-side-effect-pattern.md.
    // Default = &noop_thompson_update (compile-time no-op; resolves here at struct construction in _Init);
    // boot-wired to &real_thompson_update by EnsembleModelZoo_InitBuyThompsonBandits (buy) +
    // EnsembleModelZoo_InitExitThompsonBandits (exit) when the respective subsystem initializes.
    // Consumer dispatch sites call UNCONDITIONALLY → branchless; eliminates `if (thompson_active)` per-call
    // branches throughout reward attribution paths (Class 24 + Class 28 sister structural closures).
    // Per FOREACH_BANDIT_SIDE auto-mirror (§ G of .F.4d merged plan body — 2 sides; meta-X-macro at
    // ML_Headers/bandit_dispatch_table.hpp). Future per-symbol side = 1 row in FOREACH_BANDIT_SIDE.
    ThompsonUpdateFn buy_thompson_update_fn;       // buy-side; ezoo->buy_thompson_bandits[r] consumer
    ThompsonUpdateFn exit_thompson_update_fn;  // exit-side; ezoo->exit_thompson_bandits[r] consumer

    // v5.11.62 — primary-role indirection (per-cycle read; pointer + small scalars).
    ModelHandle<F> *primary_handles;   // points into one of {buy_signal, barrier, regime}
    int             primary_count;     // mirrors *_count of chosen role
    int             primary_target_class; // class index for buy probability extraction

    //---- [SECTION]_[LARGE HOT arrays — touched per-cycle for predict/dispatch; each occupies its own cache lines] ----

    alignas(64) ModelHandle<F> barrier[ENSEMBLE_HORIZON_MAX];
    alignas(64) ModelHandle<F> regime[ENSEMBLE_HORIZON_MAX];
    alignas(64) ModelHandle<F> exit_predictor[ENSEMBLE_HORIZON_MAX];
    alignas(64) ModelHandle<F> buy_signal[ENSEMBLE_HORIZON_MAX];

    // v5.10.0a.G.7 — per-regime bandit state. Each bandit has N arms.
    // Cold start: uniform weights; G.8 reward path updates per outcome.
    alignas(64) BanditState bandits[NUM_REGIMES];

    // v5.13.4 — sell-side bandit (parallel to buy-side); arms count = exit_predictor_count.
    alignas(64) BanditState exit_bandits[NUM_REGIMES];

    // v5.14.10.B — Bayesian Thompson sampling bandits (BUY-side). Activated when
    // cfg.bandit_algorithm in {1, 2, 3, 4} per FOREACH_BANDIT_ALGORITHM thompson_up metadata bit.
    // Cfg=0 (EXP3) → never read but init'd anyway so cfg-flip mid-run doesn't see uninitialized state.
    // ~1000B per ezoo at NUM_REGIMES=5 (5 × 200B). Persistence: thompson_state.json.
    alignas(64) ThompsonBanditState buy_thompson_bandits[NUM_REGIMES];

    // v5.15.5.F.4d — Bayesian Thompson sampling bandits (EXIT-side). Exit-side Thompson mirror per
    // FOREACH_BANDIT_SIDE auto-mirror (§ G.2 of merged plan body). Closes pre-.F.4d asymmetry where
    // buy-side had Thompson but exit-side was Exp3-only. Activated when cfg.bandit_algorithm in
    // {1, 2, 3, 4} via exit_thompson_update_fn sink-fn-pointer dispatch (same metadata bits drive
    // both sides; per-side init flag MASK_EZOO_EXIT_THOMPSON_READY gates init wiring).
    // Persistence: thompson_exit_state.json (parallel to thompson_state.json buy-side file).
    // Size: ~1000B per ezoo at NUM_REGIMES=5 (5 × 200B sister to buy_thompson_bandits[]).
    alignas(64) ThompsonBanditState exit_thompson_bandits[NUM_REGIMES];

    //---- [SECTION]_[WARM CLUSTER — touched per regime-transition or sparse attribution paths] ----
    // v5.14.0 — Ridge risk-parity blending state. Computed per slow-path cycle
    // when cfg.ridge_within_horizon=1 (default 0). Reads reward_ring history,
    // builds N×N correlation matrix, Cholesky-solves optimal weights, falls
    // back to uniform on singular Σ. Slow-path single-writer + reader on its
    // own per-core ezoo; no false sharing. NOT in hot-path read set.
    alignas(64) RidgeWeights<F> ridge_state;
    // v5.14.1.E — exit-side Ridge (mirrors ridge_state for exit_predictor[]).
    alignas(64) RidgeWeights<F> exit_ridge_state;

    // v5.10.0a.G.7 — regime hysteresis dampening.
    int regime_transition_cycles_remaining;  // 0 = stable
    int prev_regime_id;                       // regime BEFORE transition

    // v5.10.0a.G.8 — reward attribution ring buffer. Writes per cycle (1 record);
    // reads sparse during slow-path lookback. 256-slot ring; ~32 bytes/record.
    static constexpr int REWARD_RING_SIZE = 256;
    static constexpr int DRIFT_IC_HISTORY = 100;
    alignas(64) PredictionRecord reward_ring[REWARD_RING_SIZE];
    int reward_ring_head;                     // next write slot
    uint64_t predict_call_count;              // monotonic predict counter

    // v5.14.1.E — exit-side prediction history (parallel to reward_ring).
    alignas(64) PredictionRecord exit_reward_ring[REWARD_RING_SIZE];
    int exit_reward_ring_head;
    uint64_t exit_predict_call_count;

    // v5.10.0a.G.8 — drift watchdog (per-arm rolling IC tracker).
    // Written on reward attribution events; demoted bit sticky until recovery.
    alignas(64) PerArmDrift drift[ENSEMBLE_HORIZON_MAX];

    //---- [SECTION]_[COLD CLUSTER — boot / persistence / display only] ----
    // v5.10.0a.G.9 — bandit state persistence config. base_dir captured at
    // AutoDetectFromDir / LoadFromCfg time; empty path = persistence disabled.
    alignas(64) char bandit_save_path[400];   // <node_model_dir>/bandit_state.json
    int      bandit_save_interval;             // 0 = no periodic save (shutdown only)
    uint64_t bandit_update_count;              // monotonic; modulo'd against interval

    char blend_mode[16];           // "weighted" or "selection" (cached from cfg)

    // v5.11.62 — display name for the primary-role indirection.
    char primary_role_name[16];    // "buy_signal" | "barrier" | "regime" | ""

    // v5.15.5.A.3 — arm_names extracted out of BanditState (saves 256B × 5 regimes
    // × 2 sides = 2560B of display-only data per ezoo). New display meta arrays
    // live here in COLD cluster, paired with the corresponding bandits[] /
    // exit_bandits[] in the HOT region. Slow-path bandit math never touches
    // these; display sites (MLStatusPanel + EngineTUI snapshot) read them.
    BanditDisplayMeta bandit_display[NUM_REGIMES];
    BanditDisplayMeta exit_bandit_display[NUM_REGIMES];
};
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// v5.15.4 — alignas(64) required so heap-allocated EnsembleModelZoo via
// aligned_alloc(64) (HotSwap_ShadowLoad_Ensemble) gives the embedded
// alignment-sensitive members correctly-aligned addresses:
//   - ModelHandle<F> arrays (alignas(64) per v5.15.0)
//   - RidgeWeights<F> (AVX-512 vectorized)
//   - ThompsonBanditState (gained alignas/padding per v5.14.11.B.7)
// Container struct also clusters cleanly at cache-line boundaries.
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-08-16]
//----------------------------------------------------------------------
// [SIZE]_[256576B]
// [ALIGN]_[64]
// [CACHE_LINES]_[4009]
// [STRADDLE]_[unverified: barrier regime exit_predictor buy_signal]
//======================================================================
// [END_STRUCT]_[EnsembleModelZoo]
//======================================================================
// v5.15.4 — size%64==0 invariant for shadow-load aligned_alloc(64).
// EnsembleModelZoo is large (~40-60KB depending on F) but the constituent
// member alignments (ModelHandle alignas(64) × 32 slots + RidgeWeights
// alignas(64) × 2 + ThompsonBanditState ×5) all sit on 64-byte boundaries,
// so total size is a multiple of 64 (verified by static_assert).
// [ASSERT]_[LAYOUT_LOCK]_[sizeof(EnsembleModelZoo<64>) % 64 == 0]
static_assert(sizeof(EnsembleModelZoo<64>) % 64 == 0,
              "v5.15.4: EnsembleModelZoo<64> size must be multiple of 64 for cache-line discipline + AVX-512 alignment of RidgeWeights");
// [ASSERT]_[LAYOUT_LOCK]_[alignof(EnsembleModelZoo<64>) == 64]
static_assert(alignof(EnsembleModelZoo<64>) == 64,
              "v5.15.4: EnsembleModelZoo<64> must be cache-line aligned");

// v5.15.5.F.4d — bandit dispatch table consumer header (Step 1.B + Step 3 of merged plan body).
// Included AFTER EnsembleModelZoo<F> struct + size/alignment static_asserts so the dispatch table's
// templates can reference the struct. Include guard makes the re-include of NodeModelZoo.hpp from
// inside bandit_dispatch_table.hpp a no-op; all needed symbols (EnsembleModelZoo<F>, NUM_REGIMES,
// ENSEMBLE_HORIZON_MAX, MASK_ORDER_BANDIT_3BIT, ThompsonUpdateFn) are visible at this point.
//
// Reward attribution consumer sites below (EnsembleModelZoo_TickRewardsFromLookback +
// _TradeCloseReward) use g_buy_reward_dispatch<F>[algo](...) for branchless metadata-driven dispatch.
// Exit-side analog (g_exit_reward_dispatch) consumed at ControllerEventLoop reward attribution sites.
#include "bandit_dispatch_table.hpp"

//======================================================================
// [FUNCTION]_[ezoo_set_per_arm_barrier]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the A6 corrupt-barrier ingress pair (EnsembleZoo_FinalizeCorrupt rides) — data write coupled with the mask bit; corrupt arms withheld + counted toward the majority-SHALT verdict]
// [REFERENCE]_[DESIGN_SPEC]_[registry-bitmap-set-discipline]
// [REFERENCE]_[INVARIANT]_[H22]
// [REFERENCE]_[DECISION]_[D-221]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline void ezoo_set_per_arm_barrier(EnsembleModelZoo<F>* ezoo, int arm_idx,
                                       float tp, float sl) {
    // v5.15.5.E.0.10 A6 ingress (D-221) — refuse a CORRUPT barrier (neg/NaN/+Inf/out-of-range;
    // tt::barrier_is_corrupt is the SSoT predicate, also applied at the StampHelper producer seam).
    // A corrupt arm is FULLY DISABLED: mark corrupt_arms_mask (counted toward the per-node
    // majority-SHALT + the sticky retrain alert) and WITHHOLD both the barrier value and the
    // LOADED_BARRIERS bit -> excluded from the barrier blend (arms_with_barriers_mask gate). The
    // disabled_horizon_mask union (prediction-loop exclusion) is applied at the post-load
    // EvaluateCorruptShalt finalize, AFTER SetDisabledHorizons' reset so it can't be wiped.
    if (tt::barrier_is_corrupt((double)tp, (double)sl)) {
        BITMAP_SET(ezoo->corrupt_arms_mask, BITMAP_BIT_U8(arm_idx));
        return;  // per_arm_barriers[arm_idx] stays zero-init; no LOADED_BARRIERS bit
    }
    ezoo->per_arm_barriers[arm_idx].tp = tp;
    ezoo->per_arm_barriers[arm_idx].sl = sl;
    BITMAP_SET(ezoo->arms_with_barriers_mask, BITMAP_BIT_U8(arm_idx));
}

// v5.15.5.E.0.10 A6 ingress (D-221) — post-load corrupt finalize. Call ONCE per ezoo after
// LoadFromCfg + SetDisabledHorizons (boot AND hot-swap), BEFORE publishing. (1) Unions the
// corrupt arms into disabled_horizon_mask (prediction-loop exclusion; applied HERE so
// SetDisabledHorizons' reset can't wipe it). (2) Returns the per-node majority-corrupt verdict:
// true when MORE than `shalt_ratio` of the barrier-bearing arms are corrupt (default 0.5 =
// strict majority) OR all are. The caller sets MASK_NODE_STATE_MODEL_CORRUPT + the sticky
// retrain log on a true verdict. H22: pure function of THIS node's own ezoo + its cfg ratio.
template <unsigned F>
inline bool EnsembleZoo_FinalizeCorrupt(EnsembleModelZoo<F>* ezoo, double shalt_ratio) {
    ezoo->disabled_horizon_mask |= ezoo->corrupt_arms_mask;
    int n_corrupt = __builtin_popcount((unsigned)ezoo->corrupt_arms_mask);
    int n_arms    = ezoo->buy_signal_count;  // barrier-bearing (primary) arm count
    if (n_corrupt == 0 || n_arms <= 0) return false;
    if (n_corrupt >= n_arms) return true;                    // all corrupt -> SHALT
    return (double)n_corrupt > shalt_ratio * (double)n_arms; // strict majority -> SHALT
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// v5.15.5.F.3 — registry-bitmap SET discipline accessor for per_arm_barriers.
// Per DESIGN_SPECS/registry-bitmap-set-discipline.md Fix 3 (accessor wrapper).
// Couples the data write (per_arm_barriers[idx].tp/sl) with the mark bit
// (arms_with_barriers_mask bit-idx). Direct field-write sites become
// detectable via /dod-audit Signature 1 ("data write without bit set").
//
// Pre-.F.3 BUG: LoadFromCfg copied per_arm_barriers but forgot to SET the
// mask → downstream reader at StrategyParameters.hpp gated barrier blending
// on the all-zero mask → ALL arms appeared barrierless → ensemble barrier
// blending SILENTLY DISABLED. Operator saw no errors but features didn't
// work as configured.
//======================================================================
// [END_FUNCTION]_[ezoo_set_per_arm_barrier]
//======================================================================

//======================================================================
// [FUNCTION]_[EnsembleModelZoo_Init]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[zero-state init family (EnsurePrimary backstop rides) — all 32 handles + bandit/Thompson/Ridge/ring/drift state cleared; sink-fn-pointers default to noop]
// [REFERENCE]_[CLASS]_[28]
// [REFERENCE]_[DECISION]_[D-221]
// [REFERENCE]_[INVARIANT]_[H20]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline void EnsembleModelZoo_Init(EnsembleModelZoo<F> *ezoo) {
    for (int i = 0; i < ENSEMBLE_HORIZON_MAX; ++i) {
        Model_Init(&ezoo->barrier[i]);
        Model_Init(&ezoo->regime[i]);
        Model_Init(&ezoo->exit_predictor[i]);
        Model_Init(&ezoo->buy_signal[i]);
        ezoo->horizon_ticks_at_idx[i] = 0;
        // v5.15.5.A.2.a — per-arm barriers zero-init. LoadFromCfg fills
        // these from stamp body for each successfully-loaded handle.
        ezoo->per_arm_barriers[i].tp = 0.0f;
        ezoo->per_arm_barriers[i].sl = 0.0f;
    }
    ezoo->barrier_count = 0;
    ezoo->regime_count = 0;
    ezoo->exit_predictor_count = 0;
    ezoo->buy_signal_count = 0;
    // v5.15.5.A.2.c — init_flags bit-pack: zero clears ACTIVE, BANDITS_READY,
    // EXIT_BANDITS_READY, THOMPSON_READY bits all at once. Full bandit/Thompson
    // init happens in _InitBandits / _InitExitBandits / _InitThompsonBandits
    // AFTER LoadFromCfg populates buy_signal_count / exit_predictor_count;
    // those Init helpers set the appropriate MASK_EZOO_*_READY bits on
    // completion.
    ezoo->init_flags = 0;
    // v5.10.0a.G.7 — bandit state zero-init (full bandit init happens in
    // _InitBandits AFTER LoadFromCfg / AutoDetect populates buy_signal_count
    // so we know how many arms).
    memset(ezoo->bandits, 0, sizeof(ezoo->bandits));
    // v5.13.4 — exit-side bandit zero-init (full init in _InitExitBandits
    // AFTER LoadFromCfg populates exit_predictor_count).
    memset(ezoo->exit_bandits, 0, sizeof(ezoo->exit_bandits));
    ezoo->last_predicted_exit_horizon_idx = -1;
    // v5.14.10.B — Thompson bandits zero-init (full init in _InitThompsonBandits
    // AFTER LoadFromCfg populates buy_signal_count). cfg.bandit_algorithm=0
    // path never reads buy_thompson_bandits; safe to leave at zero in that mode.
    memset(ezoo->buy_thompson_bandits, 0, sizeof(ezoo->buy_thompson_bandits));
    ezoo->last_predicted_buy_thompson_arm  = -1;
    // v5.15.5.F.4d — exit-side Thompson zero-init (parallel to buy-side; full init in
    // _InitExitThompsonBandits AFTER LoadFromCfg populates exit_predictor_count).
    memset(ezoo->exit_thompson_bandits, 0, sizeof(ezoo->exit_thompson_bandits));
    ezoo->last_predicted_exit_thompson_arm = -1;
    // v5.15.5.F.4d — Pattern 5 sink-fn-pointer default wiring. Both default to noop at construction;
    // _InitThompsonBandits / _InitExitThompsonBandits boot-wire to real_thompson_update if Thompson
    // subsystem actually enabled (n_arms >= 2 on the respective side). Consumer dispatch is uniform
    // indirect-call (branchless; H20 / Class 28 closure for dispatch family).
    ezoo->buy_thompson_update_fn       = &noop_thompson_update;
    ezoo->exit_thompson_update_fn  = &noop_thompson_update;
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
    // v5.15.5.A.2.b — per-arm flag bitmaps zero-init.
    // Adding a FOREACH_PER_ARM_FLAG entry requires a matching line here.
    ezoo->disabled_horizon_mask = 0;
    ezoo->arms_with_barriers_mask = 0;
    ezoo->corrupt_arms_mask = 0;  // v5.15.5.E.0.10 A6 (D-221) — per-arm corrupt bitmap
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
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[EnsembleModelZoo_Init]
//======================================================================

//======================================================================
// [FUNCTION]_[EnsembleModelZoo_TickRewardsFromLookback]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [SLOW_PATH]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the G.8 reward-attribution family (RecordPrediction ring write + UpdateDrift watchdog ride) — lookback walk rewards aged records via the per-algorithm dispatch table]
// [REFERENCE]_[CLASS]_[[24] [25] [28]]
//======================================================================
// [CODE]
//======================================================================
//------------------------------------------------------------------
// [SECTION]_[v5.10.0a.G.8 — REWARD RING WRITE]
//------------------------------------------------------------------
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
    if (!ezoo || !BITMAP_IS_SET(ezoo->init_flags, MASK_EZOO_ACTIVE)) return;
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

//------------------------------------------------------------------
// [SECTION]_[v5.10.0a.G.8 — DRIFT WATCHDOG (perf optimization #3)]
//------------------------------------------------------------------
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

//------------------------------------------------------------------
// [SECTION]_[v5.10.0a.G.8 — SLOW-PATH LOOKBACK REWARDS]
//------------------------------------------------------------------
// Walks reward ring; for records old enough that forward_ticks have
// elapsed since predict time, computes per-arm reward based on whether
// prediction direction matched the price move (current_price vs
// record.sample_price). Calls Bandit_Update on the matching regime's
// bandit. Marks records as rewarded to avoid double-rewarding.
// v5.15.5.F.4d — sig gained `node_cfg` param for per-core bandit_algorithm dispatch (Step 3 +
// § H Class 25 sweep of merged plan body). Default nullptr preserves backward compat: nullptr →
// algo=0 (EXP3) → leaf reward fn = exp3_only_reward → Bandit_Update call → bytewise identical to
// pre-.F.4d behavior. Production callers pass node_cfg to enable Thompson + ghost-mode + BLENDED
// reward attribution per FOREACH_BANDIT_ALGORITHM metadata. Closes Class 24 sister + Class 25 + Class 28
// at the buy-side attribution surface.
template <unsigned F>
inline void EnsembleModelZoo_TickRewardsFromLookback(EnsembleModelZoo<F>* ezoo,
                                                       float current_price,
                                                       int forward_ticks,
                                                       int poll_interval,
                                                       double ic_floor,
                                                       const PerNodeCfg<F>* node_cfg = nullptr) {
    if (!ezoo || !BITMAP_IS_SET(ezoo->init_flags, MASK_EZOO_ACTIVE) || !BITMAP_IS_SET(ezoo->init_flags, MASK_EZOO_BANDITS_READY)) return;
    if (poll_interval <= 0) poll_interval = 100;
    if (forward_ticks <= 0) forward_ticks = 1000;
    uint64_t lookback_calls = (uint64_t)((forward_ticks + poll_interval - 1)
                                          / poll_interval);
    if (lookback_calls == 0) lookback_calls = 1;
    uint64_t now = ezoo->predict_call_count;
    // v5.11.62 — n_arms = primary_count (matches ezoo->primary_handles)
    EnsembleModelZoo_EnsurePrimary(ezoo);
    int n_arms = ezoo->primary_count;

    // v5.15.5.F.4d — resolve per-core bandit algorithm + bounds-clamp defensively
    // (cfg parser clamps to [0,4] per CfgFieldRegistry.hpp; this guard is belt-and-suspenders).
    // Sister to BanditAlgorithm_Apply's bounds-check at BanditAlgorithmRegistry.hpp.
    int algo = node_cfg ? node_cfg->bandit_algorithm : (int)BANDIT_ALGO_EXP3;
    if (algo < 0 || algo >= FOREACH_BANDIT_ALGORITHM_COUNT) algo = (int)BANDIT_ALGO_EXP3;

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
            // Reward signal in bps. Treat correct as +50bps, wrong as -50bps.
            double reward_bps = correct ? 50.0 : -50.0;
            // v5.15.5.F.4d — branchless metadata-driven dispatch via g_buy_reward_dispatch
            // (Step 1.B of merged plan body). For algo=EXP3 (cfg=0) → calls Bandit_Update only
            // (bytewise identical to pre-.F.4d). For algo=THOMPSON / ghost modes / BLENDED →
            // also calls Thompson_Update via Pattern 5 sink (ezoo->buy_thompson_update_fn; noop if
            // subsystem not initialized). Closes Class 24 sister at attribution surface.
            g_buy_reward_dispatch<F>[algo](ezoo, regime, a, reward_bps);
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

//------------------------------------------------------------------
// [SECTION]_[v5.10.0a.G.8 — TRADE-CLOSE REWARD HOOK]
//------------------------------------------------------------------
// Called from EventLoop_DrainPostFill when a position closes (TP/SL
// exit). Looks up the MOST RECENT prediction record (proxy for "the
// model recommendation that drove this trade") and rewards based on
// realized P&L direction. Higher weight than slow-path (real money
// signal includes fees + slippage).
//
// reward_mult: cfg.ensemble_trade_reward_mult (default 4.0). Scales
// |reward_bps| × mult; correct predictions → positive bps, wrong →
// negative.
// v5.15.5.F.4d — sig gained `node_cfg` param for per-core bandit_algorithm dispatch (Step 3 +
// § H Class 25 sweep). Same backward-compat scheme as _TickRewardsFromLookback above: nullptr →
// algo=0 (EXP3) → exp3_only_reward → Bandit_Update only → bytewise identical to pre-.F.4d behavior.
// Production callers pass node_cfg to enable Thompson + ghost + BLENDED reward attribution. Closes
// Class 24 sister + Class 25 + Class 28 at the buy-side trade-close attribution surface.
template <unsigned F>
inline void EnsembleModelZoo_TradeCloseReward(EnsembleModelZoo<F>* ezoo,
                                                double realized_pnl_bps,
                                                double reward_mult,
                                                const PerNodeCfg<F>* node_cfg = nullptr) {
    if (!ezoo || !BITMAP_IS_SET(ezoo->init_flags, MASK_EZOO_ACTIVE) || !BITMAP_IS_SET(ezoo->init_flags, MASK_EZOO_BANDITS_READY)) return;
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

    // v5.15.5.F.4d — resolve per-core bandit algorithm + bounds-clamp defensively (cfg parser
    // clamps to [0,4]; defensive belt-and-suspenders sister to BanditAlgorithm_Apply).
    int algo = node_cfg ? node_cfg->bandit_algorithm : (int)BANDIT_ALGO_EXP3;
    if (algo < 0 || algo >= FOREACH_BANDIT_ALGORITHM_COUNT) algo = (int)BANDIT_ALGO_EXP3;

    int trade_updates = 0;
    for (int a = 0; a < n_arms; ++a) {
        if (ezoo->disabled_horizon_mask & (1u << a)) continue;
        float p = rec.predictions[a];
        int correct = ((p > 0.5f) == (pnl_positive == 1)) ? 1 : 0;
        // Trade-close reward weighted higher than slow-path lookback
        double reward_bps = (correct ? 50.0 : -50.0) * reward_mult;
        // v5.15.5.F.4d — branchless metadata-driven dispatch via g_buy_reward_dispatch.
        // For algo=EXP3 (cfg=0) → bytewise identical to pre-.F.4d (only Bandit_Update). For
        // algo=THOMPSON / ghost / BLENDED → also Thompson_Update via Pattern 5 sink.
        g_buy_reward_dispatch<F>[algo](ezoo, regime, a, reward_bps);
        trade_updates++;
    }
    rec.rewarded_trade = 1;
    // v5.10.0a.G.9 — periodic save check (no-op when interval==0)
    EnsembleModelZoo_MaybeSaveBanditPeriodic(ezoo, trade_updates);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[EnsembleModelZoo_TickRewardsFromLookback]
//======================================================================

//======================================================================
// [FUNCTION]_[EnsembleModelZoo_InitBandits]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the per-regime bandit init family — Exp3 buy/exit + Thompson buy/exit (4 fns); each sets its MASK_EZOO_*_READY gate; Thompson inits wire the Pattern-5 sink-fn-pointers to real]
// [REFERENCE]_[CLASS]_[[24] [28]]
// [REFERENCE]_[INVARIANT]_[H20]
// [REFERENCE]_[TECH_DEBT]_[TECH_DEBT-84]
//======================================================================
// [CODE]
//======================================================================
//------------------------------------------------------------------
// [SECTION]_[v5.10.0a.G.7 — INIT BANDITS]
//------------------------------------------------------------------
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
        // Single-arm or empty ensemble — no point in bandits. Mark the init PASS
        // as run so dispatch doesn't loop forever.
        //
        // ⚠ 2026-08-16 — this comment used to claim "bandits won't be used
        // (MASK_EZOO_ACTIVE gates that anyway)". BOTH HALVES WERE FALSE, and the
        // false belief is why the bug existed: the G.7 select path gates on
        // MASK_EZOO_BANDITS_READY — the flag set two lines below — NOT on ACTIVE,
        // and ACTIVE would not have helped anyway since it needs only
        // total_loaded > 0, which a single-arm ensemble satisfies. So this early
        // return handed the select path a zeroed BanditState with n_arms == 0, and
        // Bandit_GetProbabilities left its output buffer untouched → uninitialized
        // stack reached tp_pct / sl_pct. Fixed by guarding the select path on
        // primary_count >= 2 (StrategyParameters.hpp), so the READ condition now
        // matches this WRITE condition exactly. Do not re-widen one without the other.
        BITMAP_SET(ezoo->init_flags, MASK_EZOO_BANDITS_READY);
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
        // v5.15.5.A.3 — arm_names extracted to BanditDisplayMeta paired in ezoo->bandit_display.
        for (int a = 0; a < n_arms; ++a) {
            char nm[32];
            snprintf(nm, sizeof(nm), "h%d", ezoo->horizon_ticks_at_idx[a]);
            BanditDisplayMeta_SetArmName(&ezoo->bandit_display[r], a, nm);
        }
    }
    BITMAP_SET(ezoo->init_flags, MASK_EZOO_BANDITS_READY);
}

// v5.13.4 — sell-side bandit init. Mirrors _InitBandits above for the
// exit_predictor role. Arms count = exit_predictor_count (set at
// LoadFromCfg time). Defaults match buy-side (gamma=0.05, blend=1.0,
// min_samples=100, ramp=200) so exit-side learning shape matches buy-
// side discipline. Operator opts in via BITMAP_IS_SET(cfg.ml_cfg_flags, MASK_ML_CFG_EXIT_BANDIT_ENABLED) at the
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
        BITMAP_CLR(ezoo->init_flags, MASK_EZOO_EXIT_BANDITS_READY);
        return;
    }
    if (n_arms < 2) {
        // Single-arm: no point in bandits, but mark initialized so
        // HandleFill can call Bandit_Update without crashing
        // (single-arm Update is a no-op accumulating reward stats).
        BITMAP_SET(ezoo->init_flags, MASK_EZOO_EXIT_BANDITS_READY);
        return;
    }
    for (int r = 0; r < NUM_REGIMES; ++r) {
        Bandit_Init(&ezoo->exit_bandits[r], n_arms,
                    /*gamma=*/0.05,
                    /*eta_max=*/(exit_eta > 0.0 ? exit_eta : 0.1),
                    /*blend_ratio=*/1.0,
                    /*min_samples=*/(min_warmup > 0 ? min_warmup : 100),
                    /*ramp_up=*/(min_warmup > 0 ? min_warmup * 2 : 200));
        // v5.15.5.A.3 — exit-side arm_names paired in ezoo->exit_bandit_display.
        for (int a = 0; a < n_arms; ++a) {
            char nm[32];
            snprintf(nm, sizeof(nm), "exit_h%d",
                     ezoo->horizon_ticks_at_idx[a]);
            BanditDisplayMeta_SetArmName(&ezoo->exit_bandit_display[r], a, nm);
        }
    }
    BITMAP_SET(ezoo->init_flags, MASK_EZOO_EXIT_BANDITS_READY);
}

//------------------------------------------------------------------
// [SECTION]_[v5.14.10.B — THOMPSON BANDIT INIT]
//------------------------------------------------------------------
// Initializes one ThompsonBanditState per regime (NUM_REGIMES from FOREACH_REGIME),
// with arms = ezoo->primary_count (same dimensions as bandits[]). Each posterior
// starts at the prior (mu_post=mu_prior, precision_post=precision_prior); RNG
// seeded with operator-tunable cfg.thompson_rng_seed (default 42 for replay
// determinism).
//
// Caller: EngineSharded boot, AFTER EnsembleModelZoo_LoadFromCfg populates
// buy_signal_count + EnsureCorePrimary populates primary_count. Same call-
// sequence position as _InitBandits (above) + _InitExitBandits.
//
// Init is idempotent + safe to call when cfg.bandit_algorithm=0 (EXP3 default):
// the Thompson state stays initialized but never read by the dispatch path.
// This preserves runtime cfg-flip semantics (operator can set cfg=1 mid-run via
// /reload-cfg without re-init).
template <unsigned F>
inline void EnsembleModelZoo_InitBuyThompsonBandits(EnsembleModelZoo<F>* ezoo,
                                                  double mu_prior,
                                                  double precision_prior,
                                                  double precision_obs,
                                                  uint64_t rng_seed) {
    if (!ezoo) return;
    int n_arms = ezoo->primary_count;
    if (n_arms < 1) {
        // No primary models loaded — graceful skip. buy_thompson_bandits stay
        // zero-init; dispatch's nullptr check fallbacks to uniform weights.
        BITMAP_CLR(ezoo->init_flags, MASK_EZOO_BUY_THOMPSON_READY);
        return;
    }
    if (n_arms < 2) {
        // Single-arm: Thompson degrades to "always pick arm 0"; mark
        // initialized so dispatch fires without crashing.
        BITMAP_SET(ezoo->init_flags, MASK_EZOO_BUY_THOMPSON_READY);
        return;
    }
    // Per-regime init. Each regime gets its own RNG state derived from the
    // base seed XOR'd with regime index — keeps regimes' RNG sequences
    // independent (same seed across regimes would correlate posterior draws).
    for (int r = 0; r < NUM_REGIMES; ++r) {
        uint64_t per_regime_seed = rng_seed ^ ((uint64_t)(r + 1) * 0x9E3779B97F4A7C15ULL);
        Thompson_Init(&ezoo->buy_thompson_bandits[r], n_arms,
                      mu_prior, precision_prior, precision_obs, per_regime_seed);
    }
    BITMAP_SET(ezoo->init_flags, MASK_EZOO_BUY_THOMPSON_READY);
    // v5.15.5.F.4d — Pattern 5 sink-fn-pointer wire to real on successful init.
    // Consumer dispatch at reward attribution sites: `ezoo->buy_thompson_update_fn(...)` now resolves
    // to real_thompson_update → Thompson_Update fires. Replaces per-call cfg branch (Class 24 close).
    ezoo->buy_thompson_update_fn = &real_thompson_update;
}

//------------------------------------------------------------------
// [SECTION]_[v5.15.5.F.4d — EXIT-SIDE THOMPSON BANDIT INIT — hand-mirror of _InitThompsonBandits per FOREACH_BANDIT_SIDE]
//------------------------------------------------------------------
// Sister to EnsembleModelZoo_InitBuyThompsonBandits above. Initializes one ThompsonBanditState per
// regime over `exit_thompson_bandits[]` with arms = ezoo->exit_predictor_count (parallel to the
// _InitExitBandits Exp3 init above, which uses the same arm count source). Closes pre-.F.4d
// asymmetry where buy-side had Thompson but exit-side was Exp3-only.
//
// Per § G.2 of v5.15.5.F.4d merged plan body. Pattern 5 sink-fn-pointer wiring at end sets
// exit_thompson_update_fn → &real_thompson_update so reward attribution at exit-side dispatch sites
// fires Thompson_Update unconditionally + branchlessly (H20 / Class 24 + Class 28 sister closure).
//
// HAND-MIRROR (vs full FOREACH_BANDIT_SIDE auto-gen): keeping buy-side fn body asymmetric in name
// (`buy_thompson_bandits` vs `exit_thompson_bandits` field; `_InitThompsonBandits` vs `_InitExitThompsonBandits`
// fn name) avoids a cascade rename of existing `buy_thompson_bandits` field across ~50 call sites.
// TECH_DEBT-084 tracks the future symmetric rename + true X-macro auto-gen across all 6 per-side
// symbol families (sequenced for `.F.4f` cleanup ship OR later).
template <unsigned F>
inline void EnsembleModelZoo_InitExitThompsonBandits(EnsembleModelZoo<F>* ezoo,
                                                      double mu_prior,
                                                      double precision_prior,
                                                      double precision_obs,
                                                      uint64_t rng_seed) {
    if (!ezoo) return;
    int n_arms = ezoo->exit_predictor_count;
    if (n_arms < 1) {
        // No exit_predictor models loaded — graceful skip. exit_thompson_bandits stay zero-init;
        // dispatch's nullptr check fallbacks to uniform weights. Sink-fn stays at noop.
        BITMAP_CLR(ezoo->init_flags, MASK_EZOO_EXIT_THOMPSON_READY);
        return;
    }
    if (n_arms < 2) {
        // Single-arm: Thompson degrades to "always pick arm 0"; mark initialized so dispatch fires
        // without crashing. Sink-fn stays at noop (no posterior to update for single-arm).
        BITMAP_SET(ezoo->init_flags, MASK_EZOO_EXIT_THOMPSON_READY);
        return;
    }
    // Per-regime init. Mirror buy-side scheme: each regime gets a unique seed via XOR with regime
    // index. Additionally XOR with a side discriminator (0x5A55E55E55E55E55ULL) so buy-side + exit-side
    // RNG streams are decorrelated even if operator sets the same rng_seed cfg (avoids correlated
    // posterior draws across sides on the same regime).
    constexpr uint64_t EXIT_SIDE_RNG_DISCRIMINATOR = 0x5A55E55E55E55E55ULL;
    for (int r = 0; r < NUM_REGIMES; ++r) {
        uint64_t per_regime_seed = (rng_seed ^ EXIT_SIDE_RNG_DISCRIMINATOR)
                                 ^ ((uint64_t)(r + 1) * 0x9E3779B97F4A7C15ULL);
        Thompson_Init(&ezoo->exit_thompson_bandits[r], n_arms,
                      mu_prior, precision_prior, precision_obs, per_regime_seed);
    }
    BITMAP_SET(ezoo->init_flags, MASK_EZOO_EXIT_THOMPSON_READY);
    // Pattern 5 sink-fn-pointer wire to real on successful init.
    ezoo->exit_thompson_update_fn = &real_thompson_update;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// InitBandits: call AFTER LoadFromCfg / AutoDetectFromDir populates
// ezoo->buy_signal_count. Initializes one BanditState per regime
// (NUM_REGIMES from FOREACH_REGIME), each with n_arms = buy_signal_count.
// Uniform initial weights.
//
// eta: cfg.ensemble_bandit_eta (Bandit-Exp3 learning rate; 0.1 default)
// min_warmup: cfg.ensemble_min_warmup_predictions (per regime; 100 default)
//
// Sets BITMAP_SET(ezoo->init_flags, MASK_EZOO_BANDITS_READY) to gate G.7 dispatch (won't read bandits
// before they're initialized).
//======================================================================
// [END_FUNCTION]_[EnsembleModelZoo_InitBandits]
//======================================================================

//======================================================================
// [FUNCTION]_[EnsembleModelZoo_SetDisabledHorizons]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the kill-switch CSV parser (Free rides) — horizon ticks -> disabled_horizon_mask; disabled arms skip predict + freeze their bandit weights]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline void EnsembleModelZoo_SetDisabledHorizons(EnsembleModelZoo<F>* ezoo,
                                                   const char* csv) {
    if (!ezoo) return;
    EnsembleModelZoo_EnsurePrimary(ezoo);
    ezoo->disabled_horizon_mask = 0;  // uint8_t per FOREACH_PER_ARM_FLAG
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
    BITMAP_CLR(ezoo->init_flags, MASK_EZOO_ACTIVE);
    // v5.14.2.D — clear v5.14.1.E exit-side state for semantic completeness.
    // Init compensates in the hot-swap Free→Init→Load path, but Free called
    // outside that path (process exit, future error-recovery code) shouldn't
    // leave stale ridge weights / reward ring entries behind.
    RidgeWeights_Init(&ezoo->exit_ridge_state);
    memset(ezoo->exit_reward_ring, 0, sizeof(ezoo->exit_reward_ring));
    ezoo->exit_reward_ring_head = 0;
    ezoo->exit_predict_call_count = 0;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// SetDisabledHorizons: parses CSV string ("100,500") → bitmask of horizon
// indices that match. Disabled horizons skip predict (saves N×predict cost
// per disabled); their bandit weights stay frozen at last value (skipped
// by Bandit_Update).
//======================================================================
// [END_FUNCTION]_[EnsembleModelZoo_SetDisabledHorizons]
//======================================================================

//======================================================================
// [FUNCTION]_[EnsembleModelZoo_LoadFromCfg]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the multi-horizon loader — per horizon dir `<base>_horizon_<H>` try all 4 roles (horizon-mismatch refusal threaded), copy stamp barriers via the mask-coupled accessor, then pick the primary role]
// [REFERENCE]_[DESIGN_SPEC]_[registry-bitmap-set-discipline.md]
//======================================================================
// [CODE]
//======================================================================
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
        if (NodeModelZoo_TryLoadRole(&ezoo->barrier[ezoo->barrier_count],
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
        if (NodeModelZoo_TryLoadRole(&ezoo->regime[ezoo->regime_count],
                                       per_horizon_dir, "regime", backend,
                                       held_out_stamp_secret, gap_threshold,
                                       held_out_gate_strict,
                                       acknowledge_cross_binary_drift,
                                       /*expected_feature_mask=*/0,
                                       /*expected_horizon_ticks=*/H)) {
            ezoo->regime_count++;
            total_loaded++;
        }
        if (NodeModelZoo_TryLoadRole(&ezoo->exit_predictor[ezoo->exit_predictor_count],
                                       per_horizon_dir, "exit", backend,
                                       held_out_stamp_secret, gap_threshold,
                                       held_out_gate_strict,
                                       acknowledge_cross_binary_drift,
                                       /*expected_feature_mask=*/0,
                                       /*expected_horizon_ticks=*/H)) {
            ezoo->exit_predictor_count++;
            total_loaded++;
        }
        if (NodeModelZoo_TryLoadRole(&ezoo->buy_signal[ezoo->buy_signal_count],
                                       per_horizon_dir, "buy_signal", backend,
                                       held_out_stamp_secret, gap_threshold,
                                       held_out_gate_strict,
                                       acknowledge_cross_binary_drift,
                                       /*expected_feature_mask=*/0,
                                       /*expected_horizon_ticks=*/H)) {
            // v5.15.5.A.2.a — copy stamp-body barriers from just-loaded
            // handle into ezoo's tight-pack per_arm_barriers array.
            // Legacy stamps without label_params stay at zero (already
            // zero-init'd in EnsembleModelZoo_Init); the .A.2.b mask
            // gates them out at dispatch time.
            //
            // v5.15.5.F.3 — apply registry-bitmap-SET-discipline.md Fix 3
            // (accessor wrapper). Pre-.F.3 BUG: code wrote per_arm_barriers
            // but NEVER called BITMAP_SET(arms_with_barriers_mask, ...) →
            // downstream reader at StrategyParameters.hpp gated barrier
            // blending on the mask → mask all-zero → ALL arms appeared
            // barrierless → ensemble barrier blending SILENTLY DISABLED
            // even with valid stamp-bound barriers. Shape A of
            // DESIGN_SPECS/registry-bitmap-set-discipline.md (data write
            // without companion bit set).
            //
            // Fix: ezoo_set_per_arm_barrier inline accessor below; the
            // copy + mark happen together → forgetting becomes impossible.
            int arm_idx = ezoo->buy_signal_count;
            if (STAMP_HAS(ezoo->buy_signal[arm_idx], label_params)) {
                ezoo_set_per_arm_barrier(ezoo, arm_idx,
                    (float)ezoo->buy_signal[arm_idx].label_tp_pct,
                    (float)ezoo->buy_signal[arm_idx].label_sl_pct);
            }
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
    // buy_class_idx=0 (STABLE class — E.1.2.C comment-truth fix; index 0
    // is stable per LabelFunctions 3-class order, the hazard direction the
    // original text described survives) → Model_Predict_Normalized returns
    // valley probability instead of peak probability → exits would fire
    // at WRONG MOMENT (during dips instead of at peaks). Silent semantic
    // inversion that the /plan-check + /merge-scan audits caught before
    // coding started.
    //
    // Mirrors the barrier-role buy_class_idx aliasing above but applies to
    // exit_predictor independently.
    for (int i = 0; i < ezoo->exit_predictor_count; ++i) {
        ezoo->exit_predictor[i].buy_class_idx =
            (ezoo->exit_predictor[i].num_outputs >= 2) ? 1 : 0;
    }

    if (total_loaded > 0) {
        BITMAP_SET(ezoo->init_flags, MASK_EZOO_ACTIVE);
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
                if (!STAMP_HAS(arr[i], scaler)) continue;
                if (arr[i].scaler_sha256[0] == '\0') continue;
                if (baseline_sha == nullptr) {
                    baseline_sha = arr[i].scaler_sha256;
                    baseline_idx = i;
                    continue;
                }
                if (strcmp(arr[i].scaler_sha256, baseline_sha) != 0) {
                    fprintf(stderr,
                        "[sibling-consistency] WARN: ensemble role=%s "
                        "sibling[%d] scaler_sha256=%.16s... differs from "
                        "sibling[%d] scaler_sha256=%.16s... "
                        "(per-horizon scalers should be identical; mixed "
                        "training sessions or sidecar copy mistake?)\n",
                        role_name, i, arr[i].scaler_sha256,
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
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
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
// BITMAP_IS_SET(ezoo->init_flags, MASK_EZOO_ACTIVE)=1 if any role got at least one horizon loaded.
//======================================================================
// [END_FUNCTION]_[EnsembleModelZoo_LoadFromCfg]
//======================================================================

//======================================================================
// [FUNCTION]_[EnsembleModelZoo_AutoDetectFromDir]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[disk discovery of `_horizon_*` siblings -> sorted load via LoadFromCfg -> grid_member_count consistency verdict (VerifyGridMemberConsistency rides; mismatch unwinds the whole ensemble)]
//======================================================================
// [CODE]
//======================================================================
//------------------------------------------------------------------
// [SECTION]_[v5.10.0a.G.5 — AUTO-DETECT ENSEMBLE FROM DISK]
//------------------------------------------------------------------
// Scans <base_dir>_horizon_* siblings on disk. For each sibling found:
//   - Verify load via NodeModelZoo_TryLoadRole
//   - Read stamp body's grid_member_count + grid_member_idx (v5.10.0a.G.2)
//   - Validate consistency: all loaded siblings must agree on grid_member_count
//   - Place each model at its grid_member_idx slot in the ensemble
//
// Operator workflow:
//   1. Train Multi-Horizon (G.1) → models/<run>/<run>_horizon_<H>/role.json
//   2. Cfg: node_N_model_dir=models/<run>  (NOTE: base path WITHOUT _horizon_<H>)
//   3. Engine boot calls AutoDetectFromDir(ezoo, "models/<run>", ...)
//   4. Function discovers all _horizon_* siblings + populates ezoo
//
// Returns total models loaded across all roles + horizons. Sets
// BITMAP_IS_SET(ezoo->init_flags, MASK_EZOO_ACTIVE)=1 if any role got at least one horizon loaded; logs
// the discovered horizon set.
//
// Backward-compat:
//   - empty base_dir → no-op, returns 0
//   - no siblings on disk → returns 0, BITMAP_IS_SET(ezoo->init_flags, MASK_EZOO_ACTIVE) stays 0; engine
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
            // EnsembleModelZoo_LoadFromCfg → NodeModelZoo_TryLoadRole; here we
            // just need grid_member_count out of the body.
            ModelStampResult sr = verify_model_stamp(
                m->model_path,
                held_out_stamp_secret ? held_out_stamp_secret : "",
                gap_threshold,
                MODEL_FORMAT_VERSION,
                /*expected_feature_registry_hash=*/0,
                /*expected_label_registry_hash=*/0);

            if (!STAMP_HAS(sr, grid_member)) {
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
    if (total > 0 && BITMAP_IS_SET(ezoo->init_flags, MASK_EZOO_ACTIVE)) {
        int validator_rc = EnsembleZoo_VerifyGridMemberConsistency(
            ezoo, held_out_stamp_secret, gap_threshold);
        if (validator_rc == 0) {
            // Mismatched grid_member_count across siblings → unwind + refuse.
            EnsembleModelZoo_Free(ezoo);
            return 0;  // match function contract: returns "total models loaded"
        }
    }

    if (total > 0 && BITMAP_IS_SET(ezoo->init_flags, MASK_EZOO_ACTIVE)) {
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
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[EnsembleModelZoo_AutoDetectFromDir]
//======================================================================

//======================================================================
// [FUNCTION]_[EnsembleModelZoo_LoadBanditState]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [PERSISTENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the G.9 Exp3 bandit persistence family (ComputeBundleId + buy/exit save/load ride) — bundle-id gate keyed on primary fingerprints; missing/mismatch leaves uniform priors]
//======================================================================
// [CODE]
//======================================================================
//------------------------------------------------------------------
// [SECTION]_[v5.10.0a.G.9 — BUNDLE SHA + BANDIT STATE LOAD/SAVE]
//------------------------------------------------------------------
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
    if (!ezoo || !BITMAP_IS_SET(ezoo->init_flags, MASK_EZOO_ACTIVE) || !BITMAP_IS_SET(ezoo->init_flags, MASK_EZOO_BANDITS_READY)) return 0;
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
    if (!ezoo || !BITMAP_IS_SET(ezoo->init_flags, MASK_EZOO_EXIT_BANDITS_READY)) return 0;
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
    if (!ezoo || !BITMAP_IS_SET(ezoo->init_flags, MASK_EZOO_ACTIVE) || !BITMAP_IS_SET(ezoo->init_flags, MASK_EZOO_BANDITS_READY)) return 0;
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
    if (!ezoo || !BITMAP_IS_SET(ezoo->init_flags, MASK_EZOO_EXIT_BANDITS_READY)) return 0;
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
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[EnsembleModelZoo_LoadBanditState]
//======================================================================

//======================================================================
// [FUNCTION]_[EnsembleModelZoo_LoadThompsonState]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [PERSISTENCE] [DETERMINISM]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the Thompson posterior persistence family (buy/exit save + load ride) — LC_NUMERIC=C-pinned %.17g JSON, format_version=1, bundle-id + n_arms gates, forward-compat-by-absence]
// [REFERENCE]_[DESIGN_SPEC]_[wire-format-byte-preservation-discipline]
// [REFERENCE]_[TECH_DEBT]_[TECH_DEBT-84]
//======================================================================
// [CODE]
//======================================================================
//------------------------------------------------------------------
// [SECTION]_[v5.14.10.C — THOMPSON STATE PERSISTENCE]
//------------------------------------------------------------------
// Mirrors v5.13.4.C exit_bandit_state.json pattern but writes ThompsonBanditState's
// posterior (mu_post + precision_post + total_pulls) + per-regime RNG state to
// <base_dir>/thompson_state.json. Same forward-compat-by-absence shape: legacy
// bundles without thompson_state.json load with InitThompsonBandits uniform priors.
//
// WIRE FORMAT (per wire-format-byte-preservation-discipline.md, all 6 layers):
//   - Layer 2 — Locale pinned LC_NUMERIC=C around emit body (per-thread uselocale)
//   - Layer 3 — Per-entry fmt: %.17g for double (lossless mu/precision); %016lx hex for rng_state
//   - Layer 6 — Forward-compat-by-absence: missing file → uniform priors stay; missing fields default
//   - format_version=1 header (PATH for FUTURE format upgrades; bumped via .X.Y if breaking)
//
// Skipped silently when the MASK_EZOO_BUY_THOMPSON_READY bit is unset (cfg=0 default = no Thompson activity to persist).

// Save buy-side thompson state to <base_dir>/buy_thompson_state.json (v5.15.5.F.4d TECH_DEBT-084
// rename from thompson_state.json for symmetric naming with exit_thompson_state.json). Load-side
// back-compat alias falls back to legacy thompson_state.json — see EnsembleModelZoo_LoadThompsonState.
// Returns 1 on success.
template <unsigned F>
inline int EnsembleModelZoo_SaveThompsonState(
    const EnsembleModelZoo<F>* ezoo, const char* base_dir,
    const char* const* regime_names) {
    if (!ezoo || !BITMAP_IS_SET(ezoo->init_flags, MASK_EZOO_BUY_THOMPSON_READY)) return 0;
    if (ezoo->primary_count < 2) return 0;  // single-arm: nothing to save
    if (!base_dir || base_dir[0] == '\0') return 0;
    char path[512];
    snprintf(path, sizeof(path), "%s/buy_thompson_state.json", base_dir);
    char tmp_path[520];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    FILE* f = fopen(tmp_path, "w");
    if (!f) return 0;

    // v5.14.10.C — Locale pin per wire-format-byte-preservation-discipline.md Layer 2.
    locale_t pinned_locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
    locale_t prev_locale = (locale_t)0;
    if (pinned_locale) prev_locale = uselocale(pinned_locale);

    char bundle_id[65];
    EnsembleModelZoo_ComputeBundleId(ezoo, bundle_id, sizeof(bundle_id));

    fprintf(f, "{\n");
    fprintf(f, "  \"format_version\": 1,\n");
    fprintf(f, "  \"n_arms\": %d,\n", ezoo->primary_count);
    fprintf(f, "  \"n_regimes\": %d,\n", NUM_REGIMES);
    fprintf(f, "  \"model_bundle_sha256\": \"%s\",\n", bundle_id);
    fprintf(f, "  \"regimes\": [\n");
    for (int r = 0; r < NUM_REGIMES; ++r) {
        const ThompsonBanditState* tb = &ezoo->buy_thompson_bandits[r];
        const char* rname = (regime_names && regime_names[r]) ? regime_names[r] : "?";
        fprintf(f, "    {\n");
        fprintf(f, "      \"regime_id\": %d,\n", r);
        fprintf(f, "      \"regime_name\": \"%s\",\n", rname);
        fprintf(f, "      \"n_arms\": %d,\n", tb->n_arms);
        fprintf(f, "      \"mu_prior\": %.17g,\n", tb->mu_prior);
        fprintf(f, "      \"precision_prior\": %.17g,\n", tb->precision_prior);
        fprintf(f, "      \"precision_obs\": %.17g,\n", tb->precision_obs);
        fprintf(f, "      \"rng_state\": \"0x%016lx\",\n", (unsigned long)tb->rng_state);
        fprintf(f, "      \"mu_post\": [");
        for (int a = 0; a < tb->n_arms; ++a) {
            fprintf(f, "%s%.17g", (a > 0 ? ", " : ""), tb->mu_post[a]);
        }
        fprintf(f, "],\n");
        fprintf(f, "      \"precision_post\": [");
        for (int a = 0; a < tb->n_arms; ++a) {
            fprintf(f, "%s%.17g", (a > 0 ? ", " : ""), tb->precision_post[a]);
        }
        fprintf(f, "],\n");
        fprintf(f, "      \"total_pulls\": [");
        for (int a = 0; a < tb->n_arms; ++a) {
            fprintf(f, "%s%u", (a > 0 ? ", " : ""), tb->total_pulls[a]);
        }
        fprintf(f, "]\n");
        fprintf(f, "    }%s\n", (r < NUM_REGIMES - 1) ? "," : "");
    }
    fprintf(f, "  ]\n");
    fprintf(f, "}\n");

    if (pinned_locale) {
        uselocale(prev_locale);
        freelocale(pinned_locale);
    }

    if (fclose(f) != 0) {
        unlink(tmp_path);
        return 0;
    }
    if (rename(tmp_path, path) != 0) {
        unlink(tmp_path);
        return 0;
    }
    return 1;
}

//------------------------------------------------------------------
// [SECTION]_[v5.15.5.F.4d — EXIT-SIDE THOMPSON SAVE/LOAD — hand-mirror of buy-side per FOREACH_BANDIT_SIDE]
//------------------------------------------------------------------
// Per § G of v5.15.5.F.4d merged plan body. Filename scheme mirrors buy-side: persistence to
// `thompson_exit_state.json` (parallel to `thompson_state.json` buy-side file). Same wire format
// (format_version=1; same JSON shape) — just different filename + different ThompsonBanditState array
// (exit_thompson_bandits[] vs buy_thompson_bandits[]) + different init-flag gate.
//
// HAND-MIRROR per TECH_DEBT-084 deferred-rename decision: full FOREACH_BANDIT_SIDE auto-gen would
// require renaming buy_thompson_bandits → buy_thompson_bandits across ~50 call sites; current hand-mirror
// scoped to `.F.4d` as cost/benefit trade-off. Future cleanup ship collapses this into a single
// X-macro consumer expansion when the rename lands.

template <unsigned F>
inline int EnsembleModelZoo_SaveExitThompsonState(
    const EnsembleModelZoo<F>* ezoo, const char* base_dir,
    const char* const* regime_names) {
    if (!ezoo || !BITMAP_IS_SET(ezoo->init_flags, MASK_EZOO_EXIT_THOMPSON_READY)) return 0;
    if (ezoo->exit_predictor_count < 2) return 0;  // single-arm: nothing to save
    if (!base_dir || base_dir[0] == '\0') return 0;
    char path[512];
    // v5.15.5.F.4d TECH_DEBT-084 — rename thompson_exit_state.json → exit_thompson_state.json
    // for symmetric naming with buy_thompson_state.json. Load-side back-compat alias falls back.
    snprintf(path, sizeof(path), "%s/exit_thompson_state.json", base_dir);
    char tmp_path[520];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    FILE* f = fopen(tmp_path, "w");
    if (!f) return 0;

    // Locale pin per wire-format-byte-preservation-discipline.md Layer 2.
    locale_t pinned_locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
    locale_t prev_locale = (locale_t)0;
    if (pinned_locale) prev_locale = uselocale(pinned_locale);

    char bundle_id[65];
    EnsembleModelZoo_ComputeBundleId(ezoo, bundle_id, sizeof(bundle_id));

    fprintf(f, "{\n");
    fprintf(f, "  \"format_version\": 1,\n");
    fprintf(f, "  \"n_arms\": %d,\n", ezoo->exit_predictor_count);
    fprintf(f, "  \"n_regimes\": %d,\n", NUM_REGIMES);
    fprintf(f, "  \"model_bundle_sha256\": \"%s\",\n", bundle_id);
    fprintf(f, "  \"regimes\": [\n");
    for (int r = 0; r < NUM_REGIMES; ++r) {
        const ThompsonBanditState* tb = &ezoo->exit_thompson_bandits[r];
        const char* rname = (regime_names && regime_names[r]) ? regime_names[r] : "?";
        fprintf(f, "    {\n");
        fprintf(f, "      \"regime_id\": %d,\n", r);
        fprintf(f, "      \"regime_name\": \"%s\",\n", rname);
        fprintf(f, "      \"n_arms\": %d,\n", tb->n_arms);
        fprintf(f, "      \"mu_prior\": %.17g,\n", tb->mu_prior);
        fprintf(f, "      \"precision_prior\": %.17g,\n", tb->precision_prior);
        fprintf(f, "      \"precision_obs\": %.17g,\n", tb->precision_obs);
        fprintf(f, "      \"rng_state\": \"0x%016lx\",\n", (unsigned long)tb->rng_state);
        fprintf(f, "      \"mu_post\": [");
        for (int a = 0; a < tb->n_arms; ++a) {
            fprintf(f, "%s%.17g", (a > 0 ? ", " : ""), tb->mu_post[a]);
        }
        fprintf(f, "],\n");
        fprintf(f, "      \"precision_post\": [");
        for (int a = 0; a < tb->n_arms; ++a) {
            fprintf(f, "%s%.17g", (a > 0 ? ", " : ""), tb->precision_post[a]);
        }
        fprintf(f, "],\n");
        fprintf(f, "      \"total_pulls\": [");
        for (int a = 0; a < tb->n_arms; ++a) {
            fprintf(f, "%s%u", (a > 0 ? ", " : ""), tb->total_pulls[a]);
        }
        fprintf(f, "]\n");
        fprintf(f, "    }%s\n", (r < NUM_REGIMES - 1) ? "," : "");
    }
    fprintf(f, "  ]\n");
    fprintf(f, "}\n");

    if (pinned_locale) {
        uselocale(prev_locale);
        freelocale(pinned_locale);
    }

    if (fclose(f) != 0) {
        unlink(tmp_path);
        return 0;
    }
    if (rename(tmp_path, path) != 0) {
        unlink(tmp_path);
        return 0;
    }
    return 1;
}

// Load thompson state from <base_dir>/thompson_state.json. Returns 1 on
// success (overlays mu_post/precision_post/total_pulls/rng_state onto pre-
// initialized states), 0 on missing/corrupt/mismatched file.
//
// Caller must call EnsembleModelZoo_InitBuyThompsonBandits FIRST to set up
// uniform priors + arm count + base RNG seed. This function only overlays.
template <unsigned F>
inline int EnsembleModelZoo_LoadThompsonState(
    EnsembleModelZoo<F>* ezoo, const char* base_dir) {
    if (!ezoo || !BITMAP_IS_SET(ezoo->init_flags, MASK_EZOO_BUY_THOMPSON_READY)) return 0;
    if (ezoo->primary_count < 2) return 0;
    if (!base_dir || base_dir[0] == '\0') return 0;
    char path[512];
    // v5.15.5.F.4d TECH_DEBT-084 — try new symmetric name first; fall back to legacy name for
    // existing on-disk model bundles. Save-side writes new name; Load-side accepts either.
    snprintf(path, sizeof(path), "%s/buy_thompson_state.json", base_dir);
    FILE* f = fopen(path, "r");
    if (!f) {
        snprintf(path, sizeof(path), "%s/thompson_state.json", base_dir);
        f = fopen(path, "r");
    }
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    constexpr size_t BUF_CAP = 256 * 1024;
    if (fsize <= 0 || (size_t)fsize >= BUF_CAP) {
        fclose(f);
        return 0;
    }
    static thread_local char buf_storage[BUF_CAP];
    char* buf = buf_storage;
    size_t read_n = fread(buf, 1, (size_t)fsize, f);
    fclose(f);
    buf[read_n] = '\0';

    // format_version check (only v=1 supported today)
    const char* p = tt::json_io::find_key(buf, "format_version");
    if (!p) return 0;
    int fmt = (int)strtol(p, nullptr, 10);
    if (fmt != 1) {
        fprintf(stderr, "[ensemble] thompson_state.json format_version=%d unsupported (expected 1); rejecting\n", fmt);
        return 0;
    }

    // n_arms check (must match current ezoo)
    p = tt::json_io::find_key(buf, "n_arms");
    if (!p) return 0;
    int saved_n_arms = (int)strtol(p, nullptr, 10);
    if (saved_n_arms != ezoo->primary_count) {
        fprintf(stderr, "[ensemble] thompson_state.json n_arms=%d mismatch (expected %d); rejecting\n",
                saved_n_arms, ezoo->primary_count);
        return 0;
    }

    // bundle_id check (best-effort; mismatch rejects; empty saved → skip check)
    char expected_id[65];
    EnsembleModelZoo_ComputeBundleId(ezoo, expected_id, sizeof(expected_id));
    const char* sha_p = tt::json_io::find_key(buf, "model_bundle_sha256");
    if (sha_p && expected_id[0] != '\0') {
        while (*sha_p == ' ' || *sha_p == '\t') ++sha_p;
        if (*sha_p == '"') ++sha_p;
        char saved_id[65] = {0};
        for (int i = 0; i < 64 && *sha_p && *sha_p != '"'; ++i) saved_id[i] = *sha_p++;
        if (saved_id[0] != '\0' && strcmp(saved_id, expected_id) != 0) {
            fprintf(stderr, "[ensemble] thompson_state.json bundle_id mismatch; rejecting\n");
            return 0;
        }
    }

    // Walk regimes array. forward-compat-by-absence: if a regime entry is
    // missing or a field is missing, leave the corresponding tb field at
    // its prior (uniform priors from InitThompsonBandits).
    p = tt::json_io::find_key(buf, "regimes");
    if (!p) return 0;
    for (int r = 0; r < NUM_REGIMES; ++r) {
        ThompsonBanditState* tb = &ezoo->buy_thompson_bandits[r];
        const char* rid_p = tt::json_io::find_key(p, "regime_id");
        if (!rid_p) break;   // forward-compat: shorter files OK

        // Overlay scalars (mu_prior / precision_prior / precision_obs)
        const char* mp_p = tt::json_io::find_key(rid_p, "mu_prior");
        if (mp_p) tb->mu_prior = tt::parse_double_fast(mp_p);
        const char* pp_p = tt::json_io::find_key(rid_p, "precision_prior");
        if (pp_p) tb->precision_prior = tt::parse_double_fast(pp_p);
        const char* po_p = tt::json_io::find_key(rid_p, "precision_obs");
        if (po_p) tb->precision_obs = tt::parse_double_fast(po_p);

        // Overlay rng_state (hex preferred; decimal fallback)
        const char* rng_p = tt::json_io::find_key(rid_p, "rng_state");
        if (rng_p) {
            while (*rng_p == ' ' || *rng_p == '\t') ++rng_p;
            if (*rng_p == '"') ++rng_p;
            if (rng_p[0] == '0' && (rng_p[1] == 'x' || rng_p[1] == 'X')) {
                tb->rng_state = strtoull(rng_p + 2, nullptr, 16);
            } else {
                tb->rng_state = strtoull(rng_p, nullptr, 10);
            }
        }

        // Overlay posterior arrays
        const char* mu_p = tt::json_io::find_key(rid_p, "mu_post");
        if (mu_p) {
            double tmp[BANDIT_MAX_ARMS];
            int got = tt::json_io::parse_double_array(mu_p, tmp, BANDIT_MAX_ARMS);
            for (int a = 0; a < got && a < tb->n_arms; ++a) tb->mu_post[a] = tmp[a];
        }
        const char* pr_p = tt::json_io::find_key(rid_p, "precision_post");
        if (pr_p) {
            double tmp[BANDIT_MAX_ARMS];
            int got = tt::json_io::parse_double_array(pr_p, tmp, BANDIT_MAX_ARMS);
            for (int a = 0; a < got && a < tb->n_arms; ++a) tb->precision_post[a] = tmp[a];
        }
        const char* tp_p = tt::json_io::find_key(rid_p, "total_pulls");
        if (tp_p) {
            uint32_t tmp[BANDIT_MAX_ARMS];
            int got = tt::json_io::parse_uint32_array(tp_p, tmp, BANDIT_MAX_ARMS);
            for (int a = 0; a < got && a < tb->n_arms; ++a) tb->total_pulls[a] = tmp[a];
        }

        // Advance p past this regime entry for next find_key scan.
        // Best-effort linear advance; find_key handles overshoot gracefully (returns nullptr → break).
        p = rid_p + 1;
    }

    fprintf(stderr, "[ensemble] loaded thompson state from %s\n", path);
    return 1;
}

//------------------------------------------------------------------
// [SECTION]_[v5.15.5.F.4d — EXIT-SIDE THOMPSON LOAD — hand-mirror of buy-side per FOREACH_BANDIT_SIDE]
//------------------------------------------------------------------
// Mirror of EnsembleModelZoo_LoadThompsonState above. Loads from `thompson_exit_state.json`
// (parallel filename to buy-side `thompson_state.json`). Same forward-compat-by-absence semantics:
// missing file → uniform priors stay (no-op return 0); per-regime overlay with field-by-field
// idempotency; format_version=1 enforcement.
//
// Caller must call EnsembleModelZoo_InitExitThompsonBandits FIRST to set up uniform priors + arm
// count + base RNG seed. This function only overlays.
template <unsigned F>
inline int EnsembleModelZoo_LoadExitThompsonState(
    EnsembleModelZoo<F>* ezoo, const char* base_dir) {
    if (!ezoo || !BITMAP_IS_SET(ezoo->init_flags, MASK_EZOO_EXIT_THOMPSON_READY)) return 0;
    if (ezoo->exit_predictor_count < 2) return 0;
    if (!base_dir || base_dir[0] == '\0') return 0;
    char path[512];
    // v5.15.5.F.4d TECH_DEBT-084 — try new symmetric name first; fall back to legacy name for
    // existing on-disk model bundles. Save-side writes new name; Load-side accepts either.
    snprintf(path, sizeof(path), "%s/exit_thompson_state.json", base_dir);
    FILE* f = fopen(path, "r");
    if (!f) {
        snprintf(path, sizeof(path), "%s/thompson_exit_state.json", base_dir);
        f = fopen(path, "r");
    }
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    constexpr size_t BUF_CAP = 256 * 1024;
    if (fsize <= 0 || (size_t)fsize >= BUF_CAP) {
        fclose(f);
        return 0;
    }
    static thread_local char buf_storage[BUF_CAP];
    char* buf = buf_storage;
    size_t read_n = fread(buf, 1, (size_t)fsize, f);
    fclose(f);
    buf[read_n] = '\0';

    // format_version check (only v=1 supported today)
    const char* p = tt::json_io::find_key(buf, "format_version");
    if (!p) return 0;
    int fmt = (int)strtol(p, nullptr, 10);
    if (fmt != 1) {
        fprintf(stderr, "[ensemble] thompson_exit_state.json format_version=%d unsupported (expected 1); rejecting\n", fmt);
        return 0;
    }

    // n_arms check (must match current ezoo)
    p = tt::json_io::find_key(buf, "n_arms");
    if (!p) return 0;
    int saved_n_arms = (int)strtol(p, nullptr, 10);
    if (saved_n_arms != ezoo->exit_predictor_count) {
        fprintf(stderr, "[ensemble] thompson_exit_state.json n_arms=%d mismatch (expected %d); rejecting\n",
                saved_n_arms, ezoo->exit_predictor_count);
        return 0;
    }

    // bundle_id check (best-effort; mismatch rejects; empty saved → skip check)
    char expected_id[65];
    EnsembleModelZoo_ComputeBundleId(ezoo, expected_id, sizeof(expected_id));
    const char* sha_p = tt::json_io::find_key(buf, "model_bundle_sha256");
    if (sha_p && expected_id[0] != '\0') {
        while (*sha_p == ' ' || *sha_p == '\t') ++sha_p;
        if (*sha_p == '"') ++sha_p;
        char saved_id[65] = {0};
        for (int i = 0; i < 64 && *sha_p && *sha_p != '"'; ++i) saved_id[i] = *sha_p++;
        if (saved_id[0] != '\0' && strcmp(saved_id, expected_id) != 0) {
            fprintf(stderr, "[ensemble] thompson_exit_state.json bundle_id mismatch; rejecting\n");
            return 0;
        }
    }

    // Walk regimes array. forward-compat-by-absence: shorter files leave later regimes at prior.
    p = tt::json_io::find_key(buf, "regimes");
    if (!p) return 0;
    for (int r = 0; r < NUM_REGIMES; ++r) {
        ThompsonBanditState* tb = &ezoo->exit_thompson_bandits[r];
        const char* rid_p = tt::json_io::find_key(p, "regime_id");
        if (!rid_p) break;

        const char* mp_p = tt::json_io::find_key(rid_p, "mu_prior");
        if (mp_p) tb->mu_prior = tt::parse_double_fast(mp_p);
        const char* pp_p = tt::json_io::find_key(rid_p, "precision_prior");
        if (pp_p) tb->precision_prior = tt::parse_double_fast(pp_p);
        const char* po_p = tt::json_io::find_key(rid_p, "precision_obs");
        if (po_p) tb->precision_obs = tt::parse_double_fast(po_p);

        const char* rng_p = tt::json_io::find_key(rid_p, "rng_state");
        if (rng_p) {
            while (*rng_p == ' ' || *rng_p == '\t') ++rng_p;
            if (*rng_p == '"') ++rng_p;
            if (rng_p[0] == '0' && (rng_p[1] == 'x' || rng_p[1] == 'X')) {
                tb->rng_state = strtoull(rng_p + 2, nullptr, 16);
            } else {
                tb->rng_state = strtoull(rng_p, nullptr, 10);
            }
        }

        const char* mu_p = tt::json_io::find_key(rid_p, "mu_post");
        if (mu_p) {
            double tmp[BANDIT_MAX_ARMS];
            int got = tt::json_io::parse_double_array(mu_p, tmp, BANDIT_MAX_ARMS);
            for (int a = 0; a < got && a < tb->n_arms; ++a) tb->mu_post[a] = tmp[a];
        }
        const char* pr_p = tt::json_io::find_key(rid_p, "precision_post");
        if (pr_p) {
            double tmp[BANDIT_MAX_ARMS];
            int got = tt::json_io::parse_double_array(pr_p, tmp, BANDIT_MAX_ARMS);
            for (int a = 0; a < got && a < tb->n_arms; ++a) tb->precision_post[a] = tmp[a];
        }
        const char* tp_p = tt::json_io::find_key(rid_p, "total_pulls");
        if (tp_p) {
            uint32_t tmp[BANDIT_MAX_ARMS];
            int got = tt::json_io::parse_uint32_array(tp_p, tmp, BANDIT_MAX_ARMS);
            for (int a = 0; a < got && a < tb->n_arms; ++a) tb->total_pulls[a] = tmp[a];
        }

        p = rid_p + 1;
    }

    fprintf(stderr, "[ensemble] loaded thompson exit state from %s\n", path);
    return 1;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[EnsembleModelZoo_LoadThompsonState]
//======================================================================

//======================================================================
// [FUNCTION]_[EnsembleModelZoo_MaybeSaveBanditPeriodic]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [PERSISTENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the periodic-save tail family (LoadBanditStateFromPath transfer-learning override + SetBanditSaveInterval ride) — threshold-crossing flush to bandit_save_path]
//======================================================================
// [CODE]
//======================================================================
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
    if (!ezoo || !BITMAP_IS_SET(ezoo->init_flags, MASK_EZOO_ACTIVE) || !BITMAP_IS_SET(ezoo->init_flags, MASK_EZOO_BANDITS_READY)) return 0;
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
// Locale: Bandit_SaveJSON emits under LC_NUMERIC=C. The process is pinned
// LC_NUMERIC=C at boot (.E.0.1: main.cpp / GuiThread post-SDL) — the single
// locale authority — and Bandit_SaveJSON additionally pins thread-local
// (uselocale) for defense-in-depth on the HMAC/stamp-adjacent path. Render-thread
// safety: this fires on the slow-path / drainer threads, never in hot path.
template <unsigned F>
inline void EnsembleModelZoo_MaybeSaveBanditPeriodic(
    EnsembleModelZoo<F>* ezoo, int updates_this_call) {
    if (!ezoo || !BITMAP_IS_SET(ezoo->init_flags, MASK_EZOO_ACTIVE) || !BITMAP_IS_SET(ezoo->init_flags, MASK_EZOO_BANDITS_READY)) return;
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

    // 2026-08-16 — the periodic path saved ONLY buy-side Exp3. The other three pools
    // (exit Exp3, buy Thompson, exit Thompson) were shutdown-save only, so a hard kill
    // -- SIGKILL, OOM, power loss -- discarded everything they had learned since boot,
    // while buy-Exp3 survived. An asymmetry no operator would predict from the cfg,
    // since `ensemble_bandit_save_interval` reads as "the bandit save cadence".
    //
    // base_dir is DERIVED, not stored: bandit_save_path is constructed exactly as
    // "<base_dir>/bandit_state.json" at :2694 in this same file, so truncating at the
    // final '/' is an invariant of its construction rather than a guess about paths.
    // That is why this needs no new ezoo field -- and adding one would have moved a
    // 400-byte array into a struct whose layout is cache-tuned.
    //
    // All three savers self-guard on their own READY flag and return 0 when their side
    // was never initialized, so an ensemble without exit models or without Thompson
    // simply skips them.
    {
        char base_dir[sizeof(ezoo->bandit_save_path)];
        strncpy(base_dir, ezoo->bandit_save_path, sizeof(base_dir) - 1);
        base_dir[sizeof(base_dir) - 1] = '\0';
        char* last_slash = strrchr(base_dir, '/');
        if (last_slash) {
            *last_slash = '\0';
            EnsembleModelZoo_SaveExitBanditState(ezoo, base_dir, nullptr);
            EnsembleModelZoo_SaveThompsonState(ezoo, base_dir, nullptr);
            EnsembleModelZoo_SaveExitThompsonState(ezoo, base_dir, nullptr);
        }
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[EnsembleModelZoo_MaybeSaveBanditPeriodic]
//======================================================================

//======================================================================
// [REGISTRY]_[FOREACH_ENSEMBLE_POST_LOAD]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [FRAMEWORK_DISCIPLINE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the canonical ensemble post-load setup steps — boot/backtest/hot-swap all expand THIS registry (the PARITY-009..012 Class-18 structural close); PostLoadSetup + IsReadyForInference contract ride]
// [COLUMN]_[step_name]_[registry-row identifier for the setup step]
// [COLUMN]_[call_expression]_[the invocation — (ezoo, cfg, node_id, base_run_path) in scope from the helper body]
// [REFERENCE]_[DESIGN_SPEC]_[postloadsetup-registry-pattern]
// [REFERENCE]_[CLASS]_[18]
// [REFERENCE]_[PARITY]_[[PARITY-9] [PARITY-10] [PARITY-11] [PARITY-12]]
//======================================================================
// [CODE]
//======================================================================
// Canonical post-load setup steps for single-zoo + ensemble. Same shape as
// the stamp-bound cfg registries (v5.14.1) — extract recurring pattern, eliminate
// the bug class structurally.
//
// Why: pre-v5.14.2.E, the boot ensemble setup (EngineSharded boot site)
// was inlined as sequential calls. Backtest (BacktestSharded.hpp) had a
// near-mirror that drifted (missed v5.13.4 InitExitBandits + LoadExitBanditState
// = PARITY-010). Hot-swap (EnsembleHotSwap.hpp from v5.14.2.A) had a different
// near-mirror that drifted MORE (missed 6 steps = PARITY-009). Single-zoo
// hot-swap missed VerifyExpected (PARITY-011); backtest single-zoo missed
// ValidateAgainstCfg (PARITY-012).
//
// Class 18 mirror data-flow incomplete recurred 4× as PARITY-009/010/011/012.
// The structural fix: X-macro registry — adding a new post-load step is ONE
// line in the registry; boot, backtest, hot-swap inherit automatically.
// Compile-time enforced inclusion at all sites; bypass impossible.
//
// ValidateAgainstCfg is NOT in the registries — it's a cross-cutting validator
// that takes BOTH zoo + ezoo as one combined check, called by caller AFTER
// PostLoadSetup helpers run. Strict-mode failure handling stays at caller
// level (boot Free+null vs hot-swap log-only — preserved per the v5.10.0c
// semantics at the EngineSharded boot site).

// Multi-line blend_mode application extracted to helper for X-macro tuple
// cleanliness. Selects per-core override OR global default; null-terminates
// after strncpy.
template <unsigned F>
inline void ensemble_post_load_apply_blend_mode(EnsembleModelZoo<F>* ezoo,
                                                  const ControllerConfig<F>& cfg,
                                                  int node_id) {
    const char* mode = cfg.node_ensemble_blend_mode[node_id][0]
        ? cfg.node_ensemble_blend_mode[node_id]
        : cfg.ensemble_blend_mode;
    strncpy(ezoo->blend_mode, mode, sizeof(ezoo->blend_mode) - 1);
    ezoo->blend_mode[sizeof(ezoo->blend_mode) - 1] = '\0';
}

// Canonical post-load setup steps for ensemble.
// Each entry: X(step_name, call_expression). Expression invoked with
// (ezoo, cfg, node_id, base_run_path) in scope from helper body.
// Adding a new step: 1 line here. Boot, backtest, hot-swap inherit.
#define FOREACH_ENSEMBLE_POST_LOAD(X)                                          \
    X(init_bandits,        EnsembleModelZoo_InitBandits(ezoo,                   \
                               cfg.ensemble_bandit_eta,                          \
                               cfg.ensemble_min_warmup_predictions))             \
    X(init_exit_bandits,   EnsembleModelZoo_InitExitBandits(ezoo,                \
                               cfg.exit_bandit_lr,                                \
                               cfg.ensemble_min_warmup_predictions))             \
    X(blend_mode,          ensemble_post_load_apply_blend_mode(ezoo, cfg,        \
                               node_id))                                          \
    X(disabled_horizons,   EnsembleModelZoo_SetDisabledHorizons(ezoo,            \
                               cfg.node_disabled_horizons[node_id]))             \
    X(load_bandit_state,   EnsembleModelZoo_LoadBanditState(ezoo,                \
                               base_run_path))                                    \
    X(save_interval,       EnsembleModelZoo_SetBanditSaveInterval(ezoo,          \
                               cfg.ensemble_bandit_save_interval))               \
    X(load_exit_bandit,    EnsembleModelZoo_LoadExitBanditState(ezoo,            \
                               base_run_path))                                    \
    /* v5.14.10.C — Thompson sampling bandit init + load (parallel to bandits[] init/load above). */ \
    /* Class 18 mirror prevention via PostLoadSetup registry (per /trace-deps BLOCKING amendment). */ \
    /* Init unconditional (so cfg-flip mid-run sees pre-initialized state); Load idempotent overlay. */ \
    X(init_thompson_bandits, EnsembleModelZoo_InitBuyThompsonBandits(ezoo,           \
                               FPN_ToDouble(cfg.thompson_mu_prior),                \
                               FPN_ToDouble(cfg.thompson_precision_prior),         \
                               FPN_ToDouble(cfg.thompson_precision_obs),           \
                               cfg.thompson_rng_seed))                             \
    X(load_thompson_state,   EnsembleModelZoo_LoadThompsonState(ezoo,             \
                               base_run_path))                                     \
    /* v5.15.5.F.4d — exit-side Thompson mirror per FOREACH_BANDIT_SIDE (§ G of merged plan body). */ \
    /* Init unconditional (parallel to buy-side; cfg-flip mid-run sees pre-initialized state); */     \
    /* Load idempotent overlay (missing file → uniform priors stay). Closes pre-.F.4d asymmetry */    \
    /* where buy-side had Thompson init/load + exit-side was Exp3-only. */                            \
    X(init_exit_thompson_bandits, EnsembleModelZoo_InitExitThompsonBandits(ezoo,                      \
                               FPN_ToDouble(cfg.thompson_mu_prior),                                   \
                               FPN_ToDouble(cfg.thompson_precision_prior),                            \
                               FPN_ToDouble(cfg.thompson_precision_obs),                              \
                               cfg.thompson_rng_seed))                                                \
    X(load_exit_thompson_state, EnsembleModelZoo_LoadExitThompsonState(ezoo,                          \
                               base_run_path))

// Compile-time count for tests. Update when adding entries.
// v5.15.5.F.4d: 9 → 11 (added init_exit_thompson_bandits + load_exit_thompson_state for exit-side mirror).
#define FOREACH_ENSEMBLE_POST_LOAD_COUNT 11

// Canonical post-load setup for ensemble. All registry steps in one place
// (count = FOREACH_ENSEMBLE_POST_LOAD_COUNT).
// Boot, backtest, hot-swap call this; never inline the steps directly.
//
// Returns: void. Does NOT call ValidateAgainstCfg — that's the caller's
// responsibility (it takes both zoo + ezoo as combined check).
//
// Caller must hold the ezoo's slow-path thread (single-reader/writer for
// per-core ezoo). Same-thread invariant; no internal locking.
template <unsigned F>
inline void EnsembleModelZoo_PostLoadSetup(EnsembleModelZoo<F>* ezoo,
                                             const ControllerConfig<F>& cfg,
                                             int node_id,
                                             const char* base_run_path) {
    if (!ezoo || !base_run_path) return;
#define X(name, expr) expr;
    FOREACH_ENSEMBLE_POST_LOAD(X)
#undef X
}

// Canonical post-load contract: returns 1 iff the ezoo has all the side-effects
// that PostLoadSetup applies. Adding a new step to FOREACH_ENSEMBLE_POST_LOAD
// requires also extending this predicate so the contract stays honest.
//
// Used by tests to assert: pre-PostLoadSetup → false; post-PostLoadSetup → true.
// If anyone adds a new step at boot/backtest/hot-swap that bypasses the helper,
// the symmetry test compares boot output vs helper output and the missing step
// shows up as a false return here OR as a state divergence.
template <unsigned F>
inline int EnsembleModelZoo_IsReadyForInference(const EnsembleModelZoo<F>* ezoo) {
    if (!ezoo) return 0;
    // Step contracts (mirror FOREACH_ENSEMBLE_POST_LOAD):
    // - InitBandits: MASK_EZOO_BANDITS_READY set (or no bandits possible — primary_count<2)
    // - InitExitBandits: MASK_EZOO_EXIT_BANDITS_READY set (or no exit models — exit_predictor_count<2)
    // - blend_mode: non-empty (defaults to global cfg.ensemble_blend_mode)
    // - SetDisabledHorizons: disabled_horizon_mask written (any value valid)
    // - LoadBanditState: no boolean to check; idempotent overlay
    // - SetBanditSaveInterval: bandit_save_interval set if cfg.ensemble_bandit_save_interval>0
    // - LoadExitBanditState: no boolean to check; idempotent overlay
    // - InitThompsonBandits (v5.14.10.C): MASK_EZOO_BUY_THOMPSON_READY set when primary_count>=2
    // - LoadThompsonState (v5.14.10.C): no boolean to check; idempotent overlay (skipped silently when the READY bit is unset)
    if (ezoo->primary_count >= 2 && !BITMAP_IS_SET(ezoo->init_flags, MASK_EZOO_BANDITS_READY)) return 0;
    if (ezoo->exit_predictor_count >= 2 && !BITMAP_IS_SET(ezoo->init_flags, MASK_EZOO_EXIT_BANDITS_READY)) return 0;
    if (ezoo->primary_count >= 2 && !BITMAP_IS_SET(ezoo->init_flags, MASK_EZOO_BUY_THOMPSON_READY)) return 0;
    if (ezoo->blend_mode[0] == '\0') return 0;
    return 1;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_REGISTRY]_[FOREACH_ENSEMBLE_POST_LOAD]
//======================================================================

//======================================================================
// [REGISTRY]_[FOREACH_SINGLE_ZOO_POST_LOAD]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [FRAMEWORK_DISCIPLINE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the canonical single-zoo post-load steps (today: VerifyExpected; designed for growth) — NodeModelZoo_PostLoadSetup expands it; sister to FOREACH_ENSEMBLE_POST_LOAD]
// [COLUMN]_[step_name]_[registry-row identifier for the setup step]
// [COLUMN]_[call_expression]_[the invocation — returns int (1=ok, 0=failure); caller decides strict-mode action]
// [REFERENCE]_[DESIGN_SPEC]_[postloadsetup-registry-pattern]
//======================================================================
// [CODE]
//======================================================================
#define FOREACH_SINGLE_ZOO_POST_LOAD(X)                                        \
    X(verify_expected,     NodeModelZoo_VerifyExpected(zoo, base_run_path,      \
                               BITMAP_IS_SET(cfg.gate_cfg_flags, MASK_GATE_CFG_BARRIER_GATE_ENABLED),                         \
                               FPN_ToDouble(cfg.ml_buy_threshold),               \
                               cfg.model_verify_strict, node_id,                 \
                               cfg.poll_interval,                                 \
                               (unsigned)MODEL_FORMAT_VERSION))

// Compile-time count for tests. Update when adding entries.
#define FOREACH_SINGLE_ZOO_POST_LOAD_COUNT 1

// Canonical post-load setup for single-zoo.
// Today: VerifyExpected (cfg-vs-expected.cfg pre-check).
//
// Returns: 1 if all steps OK; 0 if any step failed (caller decides strict-mode
// action — Free+null at boot; flag-only at hot-swap).
template <unsigned F>
inline int NodeModelZoo_PostLoadSetup(const NodeModelZoo<F>* zoo,
                                        const ControllerConfig<F>& cfg,
                                        int node_id,
                                        const char* base_run_path) {
    if (!zoo || !base_run_path) return 0;
    int all_ok = 1;
#define X(name, expr) do { if (!(expr)) all_ok = 0; } while (0);
    FOREACH_SINGLE_ZOO_POST_LOAD(X)
#undef X
    return all_ok;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Canonical post-load setup steps for single-zoo.
// Today: 1 entry (VerifyExpected). Designed for growth.
// Each entry: X(step_name, call_expression). Expression returns int
// (1=ok, 0=failure). Caller checks return code to decide strict-mode action
// (Free+null at boot; flag-only at hot-swap per v5.10.0c semantics).
//======================================================================
// [END_REGISTRY]_[FOREACH_SINGLE_ZOO_POST_LOAD]
//======================================================================

//======================================================================
// [FUNCTION]_[NodeModelZoo_CheckStaleModel]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[v5.14.8.E stale-model age gate — stamp training_timestamp_us vs cfg.model_max_age_hours; -1 REFUSE in strict mode, rate-limited CRITICAL log otherwise]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline int NodeModelZoo_CheckStaleModel(const ModelHandle<F>* m,
                                          uint64_t now_us,
                                          uint32_t max_age_hours,
                                          int strict_mode) {
    if (max_age_hours == 0) return 0;             // disabled
    if (!m) return 0;                              // no handle
    if (!STAMP_HAS(*m, training_timestamp_us)) return 0;   // legacy stamp; skip check
    if (m->training_timestamp_us == 0) return 0;   // sentinel; skip
    if (m->training_timestamp_us > now_us) return 0; // future timestamp; treat as fresh

    uint64_t age_us = now_us - m->training_timestamp_us;
    uint64_t age_hours = age_us / (3600ULL * 1000000ULL);
    if (age_hours <= max_age_hours) return 0;     // fresh

    // Stale. Surface via CRITICAL log (rate-limited per call site).
    const char* run_name = STAMP_HAS(*m, run_name) ? m->run_name : "(unnamed)";
    static uint64_t last_stale_model_log_us = 0;
    tt::Health_LogCriticalRateLimited(
        &last_stale_model_log_us,
        60000000ULL,    // 60s rate-limit gate
        -1,              // global (not per-node)
        "stale_model",   // category
        "[stale_model] %s is %lluh old > max %uh (strict=%d)",
        run_name,
        (unsigned long long)age_hours,
        (unsigned)max_age_hours,
        strict_mode);

    return strict_mode ? -1 : 0;  // REFUSE in strict; WARN otherwise
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Boot-time check: if the loaded model's stamp claims training_timestamp_us
// older than cfg.model_max_age_hours, surface stale-model condition.
//
// Operator policy:
//   cfg.model_max_age_hours == 0          → disabled (no check)
//   strict_mode (held_out_gate_strict=1)  → REFUSE (return -1)
//   strict_mode == 0                      → WARN (return 0; engine continues)
//
// Legacy stamps without training_timestamp_us (has_training_timestamp_us=0)
// load with check skipped (forward-compat).
//
// Caller uses CRITICAL log for WARN/REFUSE surfacing. Rate-limited via
// per-call-site static.
//
// Returns: -1 on REFUSE, 0 on OK or WARN. Caller checks strict_mode +
// the returned value.
//======================================================================
// [END_FUNCTION]_[NodeModelZoo_CheckStaleModel]
//======================================================================

#endif // NODE_MODEL_ZOO_HPP
