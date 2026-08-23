// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[CoreFrameworks/HotSwap.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML] [SLOW_PATH]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the shadow-load hot-swap helpers (v5.15.4) — build NEW state aside, atomic pointer swap, free OLD; pre-swap untouched on any failure]
// [REFERENCE]_[DESIGN_SPEC]_[shadow-load-state-transition-pattern]
// [REFERENCE]_[PARITY]_[PARITY-023]
// [CONTAINS]
//   - [FUNCTION]_[HotSwap_ShadowLoad_Ensemble]
//   - [FUNCTION]_[HotSwap_ShadowLoad_SingleZoo]
//======================================================================================================
// Replaces v5.10.0c "log-and-leave" + v5.14.2 in-place Free+Init+Load
// patterns with the SHADOW-LOAD discipline per
// DESIGN_SPECS/shadow-load-state-transition-pattern.md.
//
// Canonical pattern:
//   1. Allocate NEW state into SEPARATE memory (aligned_alloc(64))
//   2. Init + Load + PostLoadSetup into NEW state
//   3. Validate (strict mode)
//   4. ATOMIC swap pointer (state.nodes[c].handle)
//   5. Free OLD state (single-owner reclamation; per-core slow-path is
//      sole owner of state.nodes[c].*_handle; hot-path uses cached
//      cycle parameters via seqlock, never reads handle pointer)
//
// On failure (alloc / load / validate): Free NEW state; pre-swap
// pointer untouched. Caller sees nonzero rc; can continue serving
// from pre-swap state with no torn reads.
//
// CLOSES TECH_DEBT-005 (single-zoo + ensemble hot-swap strict-mode
// failure handling). PARITY-023's broken capture-pointer-and-revert
// design replaced by this discipline.
//
// CROSS-REFERENCES:
//   - DESIGN_SPECS/shadow-load-state-transition-pattern.md (canonical pattern)
//   - CLAUDE.md item 5 (lock-free reader-side; atomic seqlock patterns)
//   - CLAUDE.md item 13 (X-macro registry; FOREACH_ENSEMBLE_POST_LOAD)
//   - CLAUDE.md item 19 (structural fix preferred — replaces in-place Free)
//   - CLAUDE.md item 27 (struct padding determinism; alignas(64) on zoos)
//   - PARITY-023 (capture-pointer Revert design replaced)
//======================================================================================================

#pragma once

#include <stdlib.h>     // aligned_alloc, free
#include <stdio.h>
#include "ControllerConfig.hpp"
#include "ControllerEventLoop.hpp"   // EventLoopState<F> struct
#include "../ML_Headers/NodeModelZoo.hpp"

namespace tt {

//======================================================================
// [FUNCTION]_[HotSwap_ShadowLoad_Ensemble]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML] [SLOW_PATH] [CRITICAL]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[ensemble shadow swap — 7 steps: cache horizons, alloc NEW, load, post-load, validate, atomic swap, free OLD]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline int HotSwap_ShadowLoad_Ensemble(
    EventLoopState<F>& state,
    int node_idx,
    const ControllerConfig<F>& cfg,
    const char* new_path,
    int swap_backend) {

    if (!new_path || new_path[0] == '\0') {
        fprintf(stderr,
            "[hot_swap] ensemble node %d FAILED: empty path\n", node_idx);
        return -2;
    }

    // ────────────────────────────────────────────────────────────────────
    // (1) Pre-swap ezoo must exist (gate invariant); its grid is kept ONLY
    // for the grid-change WARN below. The horizon grid itself is
    // RE-DETECTED from new_path — the E.1.2.C-era "hot-swap reuses the
    // boot grid" constraint is DEAD (2026-08-22): it made every cross-
    // family swap (twins {7500,15000,30000} ↔ HFT_0 {500,1000,5000})
    // probe the wrong horizon_* children, load 0 roles, and silently
    // preserve pre-swap state — the operator's "apply live doesn't change
    // the models" repro. D-431's nested layout made the grid a property
    // of the DIR, so swap now discovers it exactly like boot does (same
    // AutoDetect sister, same args).
    // ────────────────────────────────────────────────────────────────────
    EnsembleModelZoo<F>* pre_swap_ezoo =
        (EnsembleModelZoo<F>*)state.nodes[node_idx].ensemble_handle;
    if (!pre_swap_ezoo) {
        fprintf(stderr,
            "[hot_swap] ensemble node %d FAILED: pre-swap ezoo is null "
            "(no boot ensemble); single-zoo branch should fire instead\n",
            node_idx);
        return -2;
    }

    // ────────────────────────────────────────────────────────────────────
    // (1b) s5 BT-6 — FLUSH the outgoing ezoo's learned state BEFORE anything
    // else touches it. This site did not exist: the swap built the new ezoo,
    // swapped, and freed the old one with NO Save* call anywhere in this file,
    // so every "Apply (live)" discarded up to a full flush interval (5000
    // updates) of Exp3 + Thompson learning. It bites hardest during exactly the
    // train→swap→evaluate loop an operator runs when tuning.
    //
    // WHY AT THE TOP, not "pre-free" between the swap and the free (which is
    // where it looks like it belongs): by then step (4) PostLoadSetup has
    // already LOADED state from the target dir into new_ezoo. Saving the old
    // ezoo afterwards writes the fresh tail to disk where the live in-memory
    // ezoo will never see it — recovered only at the NEXT boot or swap, a
    // silent one-generation lag on a same-dir re-apply. Flushing first also
    // makes every later failure path safe: on any early return below, the old
    // state is still live in memory AND now on disk.
    //
    // Destination is DERIVED from the outgoing ezoo's own save path (BT-7), not
    // from cfg — the two diverge precisely after a swap.
    //
    // Failure is LOG-AND-PROCEED, matching the periodic saver: a full disk must
    // never pin the engine to an old model. Thread-safety: the per-node slow
    // path is the sole ezoo owner and is the thread running this swap, the same
    // one that performs periodic saves.
    // ────────────────────────────────────────────────────────────────────
    {
        char state_dir[sizeof(pre_swap_ezoo->bandit_save_path)];
        if (EnsembleModelZoo_DeriveStateDir(pre_swap_ezoo, cfg.node_model_dir[node_idx],
                                             state_dir, sizeof(state_dir))) {
            EnsembleModelZoo_SaveAllBanditState(pre_swap_ezoo, state_dir,
                                                 "hot_swap pre-swap", node_idx);
        }
    }

    // ────────────────────────────────────────────────────────────────────
    // (2) Allocate NEW ezoo (pre-swap UNTOUCHED). aligned_alloc(64) per
    // v5.15.4.B EnsembleModelZoo alignas(64) requirement.
    // ────────────────────────────────────────────────────────────────────
    EnsembleModelZoo<F>* new_ezoo =
        (EnsembleModelZoo<F>*)aligned_alloc(64, sizeof(EnsembleModelZoo<F>));
    if (!new_ezoo) {
        fprintf(stderr,
            "[hot_swap] ensemble node %d FAILED: aligned_alloc OOM\n",
            node_idx);
        return -1;
    }
    EnsembleModelZoo_Init(new_ezoo);

    // ────────────────────────────────────────────────────────────────────
    // (3) Discover + load from new_path — the boot sister
    // (EnsembleModelZoo_AutoDetectFromDir, same args as EngineCommon 5e).
    // Pre-swap untouched on any failure.
    // ────────────────────────────────────────────────────────────────────
    int total = EnsembleModelZoo_AutoDetectFromDir(
        new_ezoo, new_path, swap_backend,
        /*held_out_stamp_secret=*/cfg.held_out_stamp_secret,
        /*gap_threshold=*/FPN_ToDouble(cfg.gap_acceptable_threshold),
        /*held_out_gate_strict=*/cfg.held_out_gate_strict,
        /*acknowledge_cross_binary_drift=*/(int)BITMAP_IS_SET(cfg.ops_cfg_flags, MASK_OPS_CFG_ACKNOWLEDGE_CROSS_BINARY_DRIFT));

    if (total == 0 || !BITMAP_IS_SET(new_ezoo->init_flags, MASK_EZOO_ACTIVE)) {
        fprintf(stderr,
            "[hot_swap] ensemble node %d FAILED: no horizon_* bundle "
            "detected/loaded under %s; pre-swap state preserved\n",
            node_idx, new_path);
        EnsembleModelZoo_Free(new_ezoo);
        free(new_ezoo);
        return -2;
    }

    // Grid-change WARN: per-horizon cfg semantics (node_N_disabled_horizons
    // mask bits, per-arm expectations) are POSITIONAL against the grid —
    // a changed grid re-means them. Loud, not blocking.
    // (2026-08-23 correction: the old parenthetical here claimed "bandit/Ridge
    // state in new_ezoo is fresh-init either way". That is FALSE and has been
    // since the POST_LOAD load rows landed — step (4) below OVERLAYS persisted
    // state from the target dir onto new_ezoo. In-flight fill attribution is
    // still clamped by the v5.13.6.C defensive bounds at update time.)
    {
        int grid_changed = 0;
        for (int i = 0; i < ENSEMBLE_HORIZON_MAX; ++i) {
            if (pre_swap_ezoo->horizon_ticks_at_idx[i] !=
                new_ezoo->horizon_ticks_at_idx[i]) { grid_changed = 1; break; }
        }
        if (grid_changed) {
            fprintf(stderr,
                "[hot_swap] ensemble node %d: horizon GRID CHANGED across "
                "swap (%d -> %d arms) — positional per-horizon cfg "
                "(disabled_horizons mask, per-arm expectations) now indexes "
                "the NEW grid; review if set\n",
                node_idx, pre_swap_ezoo->primary_count, new_ezoo->primary_count);
        }
    }

    // ────────────────────────────────────────────────────────────────────
    // (4) Canonical post-load setup (X-macro registry FOREACH_ENSEMBLE_POST_LOAD).
    // Same shape as boot + legacy in-place hot-swap; just on new_ezoo.
    // ────────────────────────────────────────────────────────────────────
    EnsembleModelZoo_PostLoadSetup<F>(new_ezoo, cfg, node_idx, new_path);

    // ────────────────────────────────────────────────────────────────────
    // (5) Strict validate. In strict mode + failure → Free new; pre-swap
    // untouched. Strict-mode validation hook intentionally minimal here;
    // most validation already happened during LoadFromCfg (per-role
    // verify_model_stamp checks). Future: add post-load drift gate.
    // ────────────────────────────────────────────────────────────────────
    if (cfg.model_verify_strict > 0) {
        // Reserve hook for future v5.X+ stricter validation.
        // Today: LoadFromCfg with held_out_gate_strict=1 already refused
        // if any per-role HMAC / drift check failed, so we'd have
        // returned at (3). This block is a no-op placeholder.
    }

    // ────────────────────────────────────────────────────────────────────
    // (6) ATOMIC swap pointer. Single x86_64 instruction on aligned
    // 8-byte pointer; lock-free; readers see old OR new, never torn.
    // ────────────────────────────────────────────────────────────────────
    EnsembleModelZoo<F>* old_ezoo = (EnsembleModelZoo<F>*)__atomic_exchange_n(
        &state.nodes[node_idx].ensemble_handle,
        (void*)new_ezoo,
        __ATOMIC_ACQ_REL);

    // ────────────────────────────────────────────────────────────────────
    // (7) Free OLD ezoo. Single-owner reclamation (see top comment).
    // ────────────────────────────────────────────────────────────────────
    if (old_ezoo) {
        EnsembleModelZoo_Free(old_ezoo);
        free(old_ezoo);
    }

    fprintf(stderr,
        "[hot_swap] ensemble node %d shadow-swapped to %s "
        "(%d roles loaded; primary=%s; exit=%d)\n",
        node_idx, new_path, total,
        new_ezoo->primary_role_name[0]
            ? new_ezoo->primary_role_name : "(none)",
        new_ezoo->exit_predictor_count);

    return 0;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Shadow-loads a new EnsembleModelZoo<F> from new_path; on success
// atomically swaps state.nodes[node_idx].ensemble_handle to the new
// allocation + Free's the old. Pre-swap state untouched on any failure.
//
// Returns:
//    0  = success (new ezoo active in slot)
//   -1  = aligned_alloc OOM
//   -2  = load failed (no roles found OR no horizons cached)
//   -3  = strict validate failed
//
// Reclamation strategy A (single-owner): per-core slow-path thread is
// the SOLE caller (writer = reader). Hot-path uses seqlock-cached cycle
// parameters and never reads ensemble_handle directly. So Free + free()
// of old_ezoo is safe immediately after atomic swap; no RCU grace.
//
// Caller responsibilities:
//   - state.nodes[node_idx].ensemble_handle must currently point at a
//     heap-allocated EnsembleModelZoo<F>* (boot path migrated to
//     aligned_alloc(64) in v5.15.4) OR nullptr (first-time load).
//   - new_path is non-null + non-empty
//   - Clears the swap_model_path_requested[] atomic flag after this returns
//======================================================================
// [END_FUNCTION]_[HotSwap_ShadowLoad_Ensemble]
//======================================================================

//======================================================================
// [FUNCTION]_[HotSwap_ShadowLoad_SingleZoo]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML] [SLOW_PATH] [CRITICAL]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[single-zoo shadow swap — the ensemble helper's parallel for NodeModelZoo; same alloc/load/swap/free discipline]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F>
inline int HotSwap_ShadowLoad_SingleZoo(
    EventLoopState<F>& state,
    int node_idx,
    const ControllerConfig<F>& cfg,
    const char* new_path,
    int swap_backend) {

    if (!new_path || new_path[0] == '\0') {
        fprintf(stderr,
            "[hot_swap] single-zoo node %d FAILED: empty path\n", node_idx);
        return -2;
    }

    NodeModelZoo<F>* new_zoo =
        (NodeModelZoo<F>*)aligned_alloc(64, sizeof(NodeModelZoo<F>));
    if (!new_zoo) {
        fprintf(stderr,
            "[hot_swap] single-zoo node %d FAILED: aligned_alloc OOM\n",
            node_idx);
        return -1;
    }
    NodeModelZoo_Init(new_zoo);

    // Mirror boot path's LoadFromDir args (feature_mask + cfg ptr for
    // X-macro drift check). aligned_alloc gives new_zoo a 64-byte-aligned
    // address; embedded ModelHandle members get correct alignment.
    uint64_t mask_for_load =
        (cfg.node_feature_mask[node_idx] != 0xFFFFFFFFFFFFFFFFULL)
            ? cfg.node_feature_mask[node_idx] : 0;
    int loaded = NodeModelZoo_LoadFromDir(
        new_zoo, new_path, swap_backend,
        /*secret=*/cfg.held_out_stamp_secret,
        /*gap=*/FPN_ToDouble(cfg.gap_acceptable_threshold),
        /*strict=*/cfg.held_out_gate_strict,
        (int)BITMAP_IS_SET(cfg.ops_cfg_flags, MASK_OPS_CFG_ACKNOWLEDGE_CROSS_BINARY_DRIFT),
        /*expected_feature_mask=*/mask_for_load,
        /*cfg_ptr=*/&cfg);

    if (loaded == 0) {
        fprintf(stderr,
            "[hot_swap] single-zoo node %d FAILED: 0 roles loaded from %s; "
            "pre-swap state preserved\n", node_idx, new_path);
        NodeModelZoo_Free(new_zoo);
        free(new_zoo);
        return -2;
    }

    // Post-load setup (X-macro FOREACH_SINGLE_ZOO_POST_LOAD; today:
    // VerifyExpected only). Returns 1 on success.
    int post_ok = NodeModelZoo_PostLoadSetup<F>(new_zoo, cfg, node_idx, new_path);
    if (!post_ok && cfg.model_verify_strict > 0) {
        fprintf(stderr,
            "[hot_swap] single-zoo node %d FAILED: post-load strict verify; "
            "pre-swap state preserved\n", node_idx);
        NodeModelZoo_Free(new_zoo);
        free(new_zoo);
        return -3;
    }

    // ATOMIC swap.
    NodeModelZoo<F>* old_zoo = (NodeModelZoo<F>*)__atomic_exchange_n(
        &state.nodes[node_idx].model_handle,
        (void*)new_zoo,
        __ATOMIC_ACQ_REL);

    if (old_zoo) {
        NodeModelZoo_Free(old_zoo);
        free(old_zoo);
    }

    fprintf(stderr,
        "[hot_swap] single-zoo node %d shadow-swapped to %s "
        "(%d roles loaded; primary=%s)\n",
        node_idx, new_path, loaded,
        new_zoo->primary_role_name[0]
            ? new_zoo->primary_role_name : "(none)");

    return 0;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Parallel to HotSwap_ShadowLoad_Ensemble for single-zoo NodeModelZoo<F>.
// Allocates new zoo on heap, loads from new_path, atomically swaps
// state.nodes[node_idx].model_handle, Free's old. Pre-swap untouched on
// failure.
//
// Returns:
//    0  = success
//   -1  = aligned_alloc OOM
//   -2  = load failed (0 roles found)
//   -3  = strict validate failed (post-load drift)
//======================================================================
// [END_FUNCTION]_[HotSwap_ShadowLoad_SingleZoo]
//======================================================================

}  // namespace tt
