// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[CoreFrameworks/EnsembleHotSwap.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML] [SLOW_PATH]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the atomic ensemble hot-swap helper — same-thread Free->Init->Load->PostLoadSetup; test-isolatable split]
// [CONTAINS]
//   - [FUNCTION]_[EngineSharded_HotSwapEnsemble]
//======================================================================================================

#pragma once

#include "ControllerConfig.hpp"
#include "../ML_Headers/NodeModelZoo.hpp"

#include <stdio.h>

namespace tt {

//======================================================================
// [FUNCTION]_[EngineSharded_HotSwapEnsemble]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML] [SLOW_PATH] [CRITICAL]]
// [REFERENCE]_[PARITY]_[PARITY-009]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[atomic ensemble swap on the owning slow-path thread — cache horizons, Free, Init, LoadFromCfg, full PostLoadSetup]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline int EngineSharded_HotSwapEnsemble(EnsembleModelZoo<F>* swap_ezoo,
                                          const ControllerConfig<F>& cfg,
                                          int node_id,
                                          const char* new_base_dir,
                                          int swap_backend) {
    if (!swap_ezoo || !new_base_dir || new_base_dir[0] == '\0') {
        fprintf(stderr,
            "[hot_swap] ensemble node %d FAILED: empty path or null zoo\n",
            node_id);
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
            "[hot_swap] ensemble node %d FAILED: no cached horizons "
            "to load (boot must have failed)\n", node_id);
        return 0;
    }

    // 2. Tear down old ensemble state (model handles + bandit counts).
    EnsembleModelZoo_Free(swap_ezoo);

    // 3. Re-init zero state. Clears v5.14.1.E fields too
    //    (exit_ridge_state, exit_reward_ring, head, predict_call_count).
    EnsembleModelZoo_Init(swap_ezoo);

    // 4. Load new ensemble across all horizons (buy_signal/barrier/
    //    regime/exit_predictor for each). v5.14.2.E.1 — closes PARITY-009.A/.B:
    //    pass cfg.held_out_stamp_secret + FPN_ToDouble(cfg.gap_acceptable_threshold)
    //    instead of hardcoded nullptr/0.05 (matches boot at
    //    EngineSharded.hpp:1161-1162).
    int total = EnsembleModelZoo_LoadFromCfg(
        swap_ezoo,
        new_base_dir,
        horizons,
        h_count,
        swap_backend,
        /*held_out_stamp_secret=*/cfg.held_out_stamp_secret,
        /*gap_threshold=*/FPN_ToDouble(cfg.gap_acceptable_threshold),
        /*held_out_gate_strict=*/cfg.held_out_gate_strict,
        /*acknowledge_cross_binary_drift=*/(int)BITMAP_IS_SET(cfg.ops_cfg_flags, MASK_OPS_CFG_ACKNOWLEDGE_CROSS_BINARY_DRIFT));
    if (total == 0) {
        fprintf(stderr,
            "[hot_swap] ensemble node %d FAILED: 0 roles loaded "
            "from %s\n", node_id, new_base_dir);
        return 0;
    }

    // 5. Canonical post-load setup (X-macro registry FOREACH_ENSEMBLE_POST_LOAD).
    //    7 steps: InitBandits, InitExitBandits, blend_mode (per-core override),
    //    SetDisabledHorizons, LoadBanditState, SetBanditSaveInterval,
    //    LoadExitBanditState. v5.14.2.E.1 — closes PARITY-009.C/.D/.E:
    //    pre-fix only ran 4 of these.
    EnsembleModelZoo_PostLoadSetup<F>(swap_ezoo, cfg, node_id, new_base_dir);

    fprintf(stderr,
        "[hot_swap] ensemble node %d swapped to %s "
        "(%d roles loaded; primary=%s; exit=%d)\n",
        node_id, new_base_dir, total,
        swap_ezoo->primary_role_name[0]
            ? swap_ezoo->primary_role_name : "(none)",
        swap_ezoo->exit_predictor_count);

    // NOTE: Post-load inference_cfg drift validation
    // (NodeModelZoo_ValidateAgainstCfg) is intentionally called by the
    // CALLER in EngineSharded.hpp, NOT here — that function lives in
    // EngineSharded.hpp and including it here would create circular
    // dependency (EngineSharded.hpp includes this file). The caller has
    // visibility to both this helper + ValidateAgainstCfg + can pass
    // its own EventLoopCoreState. v5.14.2.E.1 closes PARITY-009.F by
    // adding the validate call at the caller's site — see ensemble
    // hot-swap branch in EngineSharded_Run.

    return 1;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
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
//
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
//======================================================================
// [END_FUNCTION]_[EngineSharded_HotSwapEnsemble]
//======================================================================

}  // namespace tt
