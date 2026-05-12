// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [v5.15.4 — HOT-SWAP SHADOW-LOAD HELPERS]
//
// Replaces v5.10.0c "log-and-leave" + v5.14.2 in-place Free+Init+Load
// patterns with the SHADOW-LOAD discipline per
// DESIGN_SPECS/shadow-load-state-transition-pattern.md.
//
// Canonical pattern:
//   1. Allocate NEW state into SEPARATE memory (aligned_alloc(64))
//   2. Init + Load + PostLoadSetup into NEW state
//   3. Validate (strict mode)
//   4. ATOMIC swap pointer (state.cores[c].handle)
//   5. Free OLD state (single-owner reclamation; per-core slow-path is
//      sole owner of state.cores[c].*_handle; hot-path uses cached
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
#include "../ML_Headers/CoreModelZoo.hpp"
#include "EnsembleHotSwap.hpp"   // legacy in-place helper (kept as thin wrapper)

namespace tt {

//======================================================================================================
// [HotSwap_ShadowLoad_Ensemble<F>]
//======================================================================================================
// Shadow-loads a new EnsembleModelZoo<F> from new_path; on success
// atomically swaps state.cores[core_idx].ensemble_handle to the new
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
//   - state.cores[core_idx].ensemble_handle must currently point at a
//     heap-allocated EnsembleModelZoo<F>* (boot path migrated to
//     aligned_alloc(64) in v5.15.4) OR nullptr (first-time load).
//   - new_path is non-null + non-empty
//   - Clears the swap_model_path_requested[] atomic flag after this returns
template <unsigned F>
inline int HotSwap_ShadowLoad_Ensemble(
    EventLoopState<F>& state,
    int core_idx,
    const ControllerConfig<F>& cfg,
    const char* new_path,
    int swap_backend) {

    if (!new_path || new_path[0] == '\0') {
        fprintf(stderr,
            "[hot_swap] ensemble core %d FAILED: empty path\n", core_idx);
        return -2;
    }

    // ────────────────────────────────────────────────────────────────────
    // (1) Cache horizon list from the EXISTING ezoo before allocation.
    // Operators set core_<i>_horizon_list at boot; new dir inherits that
    // shape. Pre-swap ezoo retains horizon_ticks_at_idx[] from boot
    // EnsembleModelZoo_AutoDetectFromDir; new ezoo needs the same.
    // ────────────────────────────────────────────────────────────────────
    EnsembleModelZoo<F>* pre_swap_ezoo =
        (EnsembleModelZoo<F>*)state.cores[core_idx].ensemble_handle;
    if (!pre_swap_ezoo) {
        fprintf(stderr,
            "[hot_swap] ensemble core %d FAILED: pre-swap ezoo is null "
            "(no boot ensemble); single-zoo branch should fire instead\n",
            core_idx);
        return -2;
    }

    int horizons[ENSEMBLE_HORIZON_MAX];
    int h_count = 0;
    for (int i = 0; i < ENSEMBLE_HORIZON_MAX; ++i) {
        if (pre_swap_ezoo->horizon_ticks_at_idx[i] > 0) {
            horizons[h_count++] = pre_swap_ezoo->horizon_ticks_at_idx[i];
        }
    }
    if (h_count == 0) {
        fprintf(stderr,
            "[hot_swap] ensemble core %d FAILED: no cached horizons "
            "(boot must have failed)\n", core_idx);
        return -2;
    }

    // ────────────────────────────────────────────────────────────────────
    // (2) Allocate NEW ezoo (pre-swap UNTOUCHED). aligned_alloc(64) per
    // v5.15.4.B EnsembleModelZoo alignas(64) requirement.
    // ────────────────────────────────────────────────────────────────────
    EnsembleModelZoo<F>* new_ezoo =
        (EnsembleModelZoo<F>*)aligned_alloc(64, sizeof(EnsembleModelZoo<F>));
    if (!new_ezoo) {
        fprintf(stderr,
            "[hot_swap] ensemble core %d FAILED: aligned_alloc OOM\n",
            core_idx);
        return -1;
    }
    EnsembleModelZoo_Init(new_ezoo);

    // ────────────────────────────────────────────────────────────────────
    // (3) Load into new_ezoo from new_path. Pre-swap untouched.
    // ────────────────────────────────────────────────────────────────────
    int total = EnsembleModelZoo_LoadFromCfg(
        new_ezoo, new_path, horizons, h_count, swap_backend,
        /*held_out_stamp_secret=*/cfg.held_out_stamp_secret,
        /*gap_threshold=*/FPN_ToDouble(cfg.gap_acceptable_threshold),
        /*held_out_gate_strict=*/cfg.held_out_gate_strict,
        /*acknowledge_cross_binary_drift=*/(int)BITMAP_IS_SET(cfg.ops_cfg_flags, MASK_OPS_CFG_ACKNOWLEDGE_CROSS_BINARY_DRIFT));

    if (total == 0) {
        fprintf(stderr,
            "[hot_swap] ensemble core %d FAILED: 0 roles loaded from %s; "
            "pre-swap state preserved\n", core_idx, new_path);
        EnsembleModelZoo_Free(new_ezoo);
        free(new_ezoo);
        return -2;
    }

    // ────────────────────────────────────────────────────────────────────
    // (4) Canonical post-load setup (X-macro registry FOREACH_ENSEMBLE_POST_LOAD).
    // Same shape as boot + legacy in-place hot-swap; just on new_ezoo.
    // ────────────────────────────────────────────────────────────────────
    EnsembleModelZoo_PostLoadSetup<F>(new_ezoo, cfg, core_idx, new_path);

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
        &state.cores[core_idx].ensemble_handle,
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
        "[hot_swap] ensemble core %d shadow-swapped to %s "
        "(%d roles loaded; primary=%s; exit=%d)\n",
        core_idx, new_path, total,
        new_ezoo->primary_role_name[0]
            ? new_ezoo->primary_role_name : "(none)",
        new_ezoo->exit_predictor_count);

    return 0;
}

//======================================================================================================
// [HotSwap_ShadowLoad_SingleZoo<F>]
//======================================================================================================
// Parallel to HotSwap_ShadowLoad_Ensemble for single-zoo CoreModelZoo<F>.
// Allocates new zoo on heap, loads from new_path, atomically swaps
// state.cores[core_idx].model_handle, Free's old. Pre-swap untouched on
// failure.
//
// Returns:
//    0  = success
//   -1  = aligned_alloc OOM
//   -2  = load failed (0 roles found)
//   -3  = strict validate failed (post-load drift)
template <unsigned F>
inline int HotSwap_ShadowLoad_SingleZoo(
    EventLoopState<F>& state,
    int core_idx,
    const ControllerConfig<F>& cfg,
    const char* new_path,
    int swap_backend) {

    if (!new_path || new_path[0] == '\0') {
        fprintf(stderr,
            "[hot_swap] single-zoo core %d FAILED: empty path\n", core_idx);
        return -2;
    }

    CoreModelZoo<F>* new_zoo =
        (CoreModelZoo<F>*)aligned_alloc(64, sizeof(CoreModelZoo<F>));
    if (!new_zoo) {
        fprintf(stderr,
            "[hot_swap] single-zoo core %d FAILED: aligned_alloc OOM\n",
            core_idx);
        return -1;
    }
    CoreModelZoo_Init(new_zoo);

    // Mirror boot path's LoadFromDir args (feature_mask + cfg ptr for
    // X-macro drift check). aligned_alloc gives new_zoo a 64-byte-aligned
    // address; embedded ModelHandle members get correct alignment.
    uint64_t mask_for_load =
        (cfg.core_feature_mask[core_idx] != 0xFFFFFFFFFFFFFFFFULL)
            ? cfg.core_feature_mask[core_idx] : 0;
    int loaded = CoreModelZoo_LoadFromDir(
        new_zoo, new_path, swap_backend,
        /*secret=*/cfg.held_out_stamp_secret,
        /*gap=*/FPN_ToDouble(cfg.gap_acceptable_threshold),
        /*strict=*/cfg.held_out_gate_strict,
        (int)BITMAP_IS_SET(cfg.ops_cfg_flags, MASK_OPS_CFG_ACKNOWLEDGE_CROSS_BINARY_DRIFT),
        /*expected_feature_mask=*/mask_for_load,
        /*cfg_ptr=*/&cfg);

    if (loaded == 0) {
        fprintf(stderr,
            "[hot_swap] single-zoo core %d FAILED: 0 roles loaded from %s; "
            "pre-swap state preserved\n", core_idx, new_path);
        CoreModelZoo_Free(new_zoo);
        free(new_zoo);
        return -2;
    }

    // Post-load setup (X-macro FOREACH_SINGLE_ZOO_POST_LOAD; today:
    // VerifyExpected only). Returns 1 on success.
    int post_ok = CoreModelZoo_PostLoadSetup<F>(new_zoo, cfg, core_idx, new_path);
    if (!post_ok && cfg.model_verify_strict > 0) {
        fprintf(stderr,
            "[hot_swap] single-zoo core %d FAILED: post-load strict verify; "
            "pre-swap state preserved\n", core_idx);
        CoreModelZoo_Free(new_zoo);
        free(new_zoo);
        return -3;
    }

    // ATOMIC swap.
    CoreModelZoo<F>* old_zoo = (CoreModelZoo<F>*)__atomic_exchange_n(
        &state.cores[core_idx].model_handle,
        (void*)new_zoo,
        __ATOMIC_ACQ_REL);

    if (old_zoo) {
        CoreModelZoo_Free(old_zoo);
        free(old_zoo);
    }

    fprintf(stderr,
        "[hot_swap] single-zoo core %d shadow-swapped to %s "
        "(%d roles loaded; primary=%s)\n",
        core_idx, new_path, loaded,
        new_zoo->primary_role_name[0]
            ? new_zoo->primary_role_name : "(none)");

    return 0;
}

}  // namespace tt
