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

#endif // CORE_MODEL_ZOO_HPP
