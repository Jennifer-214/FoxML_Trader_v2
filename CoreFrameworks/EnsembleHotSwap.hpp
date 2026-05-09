// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [v5.14.2 — ENSEMBLE HOT-SWAP HELPER]
//
// Atomic ensemble model swap. Replaces the v5.10.0c REFUSE path that
// rejected hot-swap when ensemble inference was active. Same-thread
// (slow-path c is single-reader/writer for its own ezoo); brief
// window with empty zoo is safe — ML inference also runs on this
// same slow-path thread, can't preempt itself.
//
// Reuses the v5.13.0 / v5.13.4 ensemble + bandit primitives:
//   Free → Init → LoadFromCfg → InitBandits → InitExitBandits
//   → LoadBanditState → LoadExitBanditState
//
// Lives in its own header (split from EngineSharded.hpp) so tests can
// exercise the helper without dragging in the full sharded engine
// (Binance, Notify, Depth recorder globals, etc.). Boundary-stable
// refactor — caller side unchanged; only the storage location moved.
//======================================================================================================

#pragma once

#include "ControllerConfig.hpp"
#include "../ML_Headers/CoreModelZoo.hpp"

#include <stdio.h>

namespace tt {

// Caller responsibilities:
//   - swap_ezoo must be non-null and own its handle storage
//   - Open-position gate already passed (caller checks
//     acknowledge_hot_swap_with_open_positions)
//   - Caller clears the swap_model_path_requested[] atomic flag
//
// Returns 1 on success, 0 on failure (caller logs + sets
// model_load_failed). Failure modes:
//   - Empty new_base_dir
//   - No cached horizons in ezoo (boot must have been broken)
//   - LoadFromCfg returns 0 roles loaded
template <unsigned F>
inline int EngineSharded_HotSwapEnsemble(EnsembleModelZoo<F>* swap_ezoo,
                                          const ControllerConfig<F>& cfg,
                                          int core_id,
                                          const char* new_base_dir,
                                          int swap_backend) {
    if (!swap_ezoo || !new_base_dir || new_base_dir[0] == '\0') {
        fprintf(stderr,
            "[hot_swap] ensemble core %d FAILED: empty path or null zoo\n",
            core_id);
        return 0;
    }

    // 1. Cache horizons BEFORE Free. Free zeros barrier_count etc.,
    //    but horizon_ticks_at_idx[] retains its pre-Free values until
    //    overwritten by a fresh LoadFromCfg. Walk all slots and keep
    //    non-zero values to be conservative.
    int horizons[ENSEMBLE_HORIZON_MAX];
    int h_count = 0;
    for (int i = 0; i < ENSEMBLE_HORIZON_MAX; ++i) {
        if (swap_ezoo->horizon_ticks_at_idx[i] > 0) {
            horizons[h_count++] = swap_ezoo->horizon_ticks_at_idx[i];
        }
    }
    if (h_count == 0) {
        fprintf(stderr,
            "[hot_swap] ensemble core %d FAILED: no cached horizons "
            "to load (boot must have failed)\n", core_id);
        return 0;
    }

    // 2. Tear down old ensemble state (model handles + bandit counts).
    EnsembleModelZoo_Free(swap_ezoo);

    // 3. Re-init zero state. Clears v5.14.1.E fields too
    //    (exit_ridge_state, exit_reward_ring, head, predict_call_count).
    EnsembleModelZoo_Init(swap_ezoo);

    // 4. Load new ensemble across all horizons (buy_signal/barrier/
    //    regime/exit_predictor for each). v5.14.1.B.3 cross-binary
    //    drift gates passed through.
    int total = EnsembleModelZoo_LoadFromCfg(
        swap_ezoo,
        new_base_dir,
        horizons,
        h_count,
        swap_backend,
        /*held_out_stamp_secret=*/nullptr,
        /*gap_threshold=*/0.05,
        /*held_out_gate_strict=*/cfg.held_out_gate_strict,
        /*acknowledge_cross_binary_drift=*/cfg.acknowledge_cross_binary_version_drift);
    if (total == 0) {
        fprintf(stderr,
            "[hot_swap] ensemble core %d FAILED: 0 roles loaded "
            "from %s\n", core_id, new_base_dir);
        return 0;
    }

    // 5. Re-init buy + exit bandits with cfg defaults. Cold start:
    //    uniform priors. (G.8-style state overlay happens in step 6.)
    EnsembleModelZoo_InitBandits(swap_ezoo,
        cfg.ensemble_bandit_eta,
        cfg.ensemble_min_warmup_predictions);
    EnsembleModelZoo_InitExitBandits(swap_ezoo,
        cfg.exit_bandit_lr,
        cfg.ensemble_min_warmup_predictions);

    // 6. Overlay persisted bandit state if any. Bundle-id mismatch is
    //    a graceful skip — uniform priors stay (LoadBanditState logs).
    EnsembleModelZoo_LoadBanditState(swap_ezoo, new_base_dir);
    EnsembleModelZoo_LoadExitBanditState(swap_ezoo, new_base_dir);

    fprintf(stderr,
        "[hot_swap] ensemble core %d swapped to %s "
        "(%d roles loaded; primary=%s; exit=%d)\n",
        core_id, new_base_dir, total,
        swap_ezoo->primary_role_name[0]
            ? swap_ezoo->primary_role_name : "(none)",
        swap_ezoo->exit_predictor_count);
    return 1;
}

}  // namespace tt
