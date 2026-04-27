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
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

// role bitmap — set in zoo->loaded_mask when a model is successfully loaded
#define CORE_MODEL_BARRIER     (1u << 0)  // 3-class softmax: stable/peak/valley
#define CORE_MODEL_REGIME      (1u << 1)  // multi-class regime classifier
#define CORE_MODEL_EXIT        (1u << 2)  // exit timing model
#define CORE_MODEL_BUY_SIGNAL  (1u << 3)  // legacy single-binary buy signal

template <unsigned F>
struct CoreModelZoo {
    ModelHandle<F> barrier;     // primary: 3-class P(stable)/P(peak)/P(valley)
    ModelHandle<F> regime;      // future: multi-class regime
    ModelHandle<F> exit;        // future: exit timing
    ModelHandle<F> buy_signal;  // legacy single-binary
    unsigned int loaded_mask;   // bitmap of loaded roles (CORE_MODEL_*)
};

//======================================================================================================
template <unsigned F>
inline void CoreModelZoo_Init(CoreModelZoo<F> *zoo) {
    Model_Init(&zoo->barrier);
    Model_Init(&zoo->regime);
    Model_Init(&zoo->exit);
    Model_Init(&zoo->buy_signal);
    zoo->loaded_mask = 0;
}

//======================================================================================================
// try to load a single model from <dir>/<role>.json, falling back to <role>.xgb
// returns 1 if loaded, 0 if file not found or load failed
//======================================================================================================
template <unsigned F>
inline int CoreModelZoo_TryLoadRole(ModelHandle<F> *handle, const char *dir,
                                    const char *role_name, int backend) {
    char path[512];
    struct stat st;

    // try .json first (modern XGBoost format, matches Training panel default)
    snprintf(path, sizeof(path), "%s/%s.json", dir, role_name);
    if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
        return Model_Load(handle, path, backend);
    }

    // fall back to .xgb (older binary format)
    snprintf(path, sizeof(path), "%s/%s.xgb", dir, role_name);
    if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
        return Model_Load(handle, path, backend);
    }

    // .txt for LightGBM
    if (backend == MODEL_BACKEND_LIGHTGBM) {
        snprintf(path, sizeof(path), "%s/%s.txt", dir, role_name);
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            return Model_Load(handle, path, backend);
        }
    }

    return 0;
}

//======================================================================================================
// auto-discover and load all roles present in `dir`. missing roles silently
// disabled. returns the number of roles loaded.
//======================================================================================================
template <unsigned F>
inline int CoreModelZoo_LoadFromDir(CoreModelZoo<F> *zoo, const char *dir, int backend) {
    if (!dir || dir[0] == '\0') return 0;

    int loaded = 0;
    if (CoreModelZoo_TryLoadRole(&zoo->barrier, dir, "barrier", backend)) {
        zoo->loaded_mask |= CORE_MODEL_BARRIER;
        loaded++;
    }
    if (CoreModelZoo_TryLoadRole(&zoo->regime, dir, "regime", backend)) {
        zoo->loaded_mask |= CORE_MODEL_REGIME;
        loaded++;
    }
    if (CoreModelZoo_TryLoadRole(&zoo->exit, dir, "exit", backend)) {
        zoo->loaded_mask |= CORE_MODEL_EXIT;
        loaded++;
    }
    if (CoreModelZoo_TryLoadRole(&zoo->buy_signal, dir, "buy_signal", backend)) {
        zoo->loaded_mask |= CORE_MODEL_BUY_SIGNAL;
        loaded++;
    }

    fprintf(stderr, "[ML] zoo loaded %d role(s) from %s (mask=0x%x)\n",
            loaded, dir, zoo->loaded_mask);
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
        else if (strcmp(key, "ml_buy_threshold") == 0)       expected_threshold = atof(val);
        else if (strcmp(key, "expected_num_classes") == 0)   expected_num_classes = atoi(val);
        else if (strcmp(key, "expected_role") == 0) {
            strncpy(expected_role, val, sizeof(expected_role) - 1);
            expected_role[sizeof(expected_role) - 1] = '\0';
        }
        else if (strcmp(key, "held_out_fraction") == 0)        expected_held_out_fraction = atof(val);
        else if (strcmp(key, "gap_acceptable_threshold") == 0) expected_gap_threshold     = atof(val);
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

#endif // CORE_MODEL_ZOO_HPP
