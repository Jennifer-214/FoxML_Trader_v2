// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[ML_Headers/FeatureRegistryOverlay.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [DETERMINISM]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[3-layer overlay fingerprint verification (v5.14.3.B) — sidecar layer-2 hash vs stamp; the canonical post-load entry point for all 6 mirror sites]
// [CONTAINS]
//   - [FUNCTION]_[FeatureOverlay_PostLoadVerify]   (+ the sidecar parser shares the file)
//======================================================================================================
// 3-layer registry fingerprinting (overlay-aware lineage):
//   layer-1 = base FEATURE_REGISTRY_HASH (existing C++ FNV-1a; v5.8.6)
//   layer-2 = SHA256(canonical overlay JSON)        ← Python tool writes
//   layer-3 = SHA256(layer1 || layer2)              ← Python tool writes
//
// Engine load-time verification:
//   - For each ModelHandle with has_overlay_hash=1 in stamp body:
//     - Read `<model>.overlay.json` sidecar
//     - Extract `computed_layer2_hash` field from sidecar
//     - Compare to stamp body's overlay_hash field
//     - WARN/REFUSE per cfg.held_out_gate_strict
//   - Legacy stamps (has_overlay_hash=0) skip silently
//
// Tampering protection: stamp body's overlay_hash is HMAC-protected by
// stamp_write_for_model. Operator tampering with sidecar contents OR
// sidecar's self-reported hash is detected because the cross-check
// against HMAC-protected stamp body fails.
//
// HELPER LOCATION (per v5.14.2.E.1 precedent): standalone header so
// caller sites (boot ×2 / backtest ×2 / hot-swap ×2 = 6 sites) all
// invoke the same code. Adding a new overlay verification step = 1-line
// edit to the helper. Class 18 (mirror data-flow incomplete) extinct
// for the overlay surface, structurally.
//
// CALLED FROM (6 sites — symmetry test verifies):
//   - CoreFrameworks/EngineSharded.hpp boot single-zoo (~1108-1131)
//   - CoreFrameworks/EngineSharded.hpp boot ensemble (~1153-1207)
//   - Backtest/BacktestSharded.hpp boot single-zoo (~291-307)
//   - Backtest/BacktestSharded.hpp boot ensemble (~312-368)
//   - CoreFrameworks/EngineSharded.hpp single-zoo hot-swap (~2772-2820)
//   - CoreFrameworks/EngineSharded.hpp ensemble hot-swap caller (~2747+)
//======================================================================================================

#pragma once

#include "../CoreFrameworks/ControllerConfig.hpp"
#include "NodeModelZoo.hpp"
#include "ModelInference.hpp"

#include <stdio.h>
#include <string.h>

namespace tt {

//======================================================================================================
// [FUNCTION]_[FeatureOverlay_PostLoadVerify]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [DETERMINISM]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[read <model>.overlay.json, extract layer-2 hash, compare against the stamp; WARN/refuse per strict mode; the sidecar parser rides in this section]
//======================================================================
// [CODE]
//======================================================================
// SIDECAR PARSER — extract computed_layer2_hash field
//======================================================================================================
// Parses `<model>.overlay.json` and extracts the value of
// `computed_layer2_hash` (64-char hex SHA256). Returns 1 on success,
// 0 on failure (file missing, parse error, field missing).
//
// Minimal JSON parsing: substring search for the canonical key. Avoids
// pulling in a JSON library for a single field extraction. Tolerates
// whitespace/formatting variations because Python's json.dumps writes
// `"computed_layer2_hash": "..."` consistently (any whitespace between
// key and ":" handled).
//======================================================================================================

inline int FeatureOverlay_ParseLayer2HashFromSidecar(
    const char* model_path,
    char* out_hex,                  // out: 65-byte buffer (64 hex + null)
    size_t out_cap,
    char* err,                       // out: error message buffer
    size_t err_cap) {
    if (!model_path || !out_hex || out_cap < 65 || !err || err_cap < 32) {
        return 0;
    }
    out_hex[0] = '\0';

    char sidecar_path[512];
    snprintf(sidecar_path, sizeof(sidecar_path), "%s.overlay.json", model_path);

    FILE* f = fopen(sidecar_path, "r");
    if (!f) {
        snprintf(err, err_cap, "sidecar missing: %s", sidecar_path);
        return 0;
    }

    char buf[8192] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) {
        snprintf(err, err_cap, "empty sidecar file");
        return 0;
    }
    buf[n] = '\0';

    // Find `"computed_layer2_hash"`. Tolerate any whitespace before ':'
    // and any whitespace + opening quote after.
    const char* needle = "\"computed_layer2_hash\"";
    char* found = strstr(buf, needle);
    if (!found) {
        snprintf(err, err_cap, "computed_layer2_hash field missing");
        return 0;
    }
    found += strlen(needle);
    // Skip whitespace + ':'
    while (*found == ' ' || *found == '\t' || *found == '\n' || *found == ':') {
        found++;
    }
    // Expect opening quote
    if (*found != '"') {
        snprintf(err, err_cap, "malformed value (expected quote)");
        return 0;
    }
    found++;
    // Copy until closing quote (max 64 chars + null)
    size_t i = 0;
    while (*found && *found != '"' && i < 64) {
        out_hex[i++] = *found++;
    }
    out_hex[i] = '\0';
    if (i != 64) {
        snprintf(err, err_cap, "hash length %zu (expected 64)", i);
        return 0;
    }
    return 1;
}

//======================================================================================================
// POST-LOAD VERIFY HELPER — canonical entry point for all 6 mirror sites
//======================================================================================================
// Iterates every loaded ModelHandle in the zoo (single-zoo: 4 roles;
// ensemble: 4 roles × N horizons). For each handle with stamp body
// has_overlay_hash=1:
//   - Reads model's `.overlay.json` sidecar
//   - Compares sidecar's computed_layer2_hash to stamp's overlay_hash
//   - On mismatch: WARN (loose) or REFUSE (strict)
//   - On sidecar missing but stamp claims overlay: same WARN/REFUSE
//
// Returns:
//   0 = all loaded handles either skipped (legacy stamp) or matched
//  -1 = REFUSE in strict mode (one or more handles mismatched)
//
// Strict-mode failure handling: caller decides what to do with -1
// return (boot Free+null+flag; hot-swap log+leave; backtest log+continue).
// Helper itself just reports.
//======================================================================================================

template <unsigned F>
inline int FeatureOverlay_PostLoadVerify(
    const NodeModelZoo<F>* zoo,
    const EnsembleModelZoo<F>* ezoo,
    int node_id,
    int strict_mode) {
    int strict = (strict_mode == 1);
    int mismatch_count = 0;

    auto check_handle = [&](const ModelHandle<F>* h, const char* role_name, int h_idx) {
        if (!h) return;
        if (!STAMP_HAS(*h, overlay_hash)) return;  // legacy stamp; silent skip

        // Format location string (single-zoo: "core 0"; ensemble: "core 0 ensemble[2]")
        char loc[64];
        if (h_idx < 0) {
            snprintf(loc, sizeof(loc), "node %d", node_id);
        } else {
            snprintf(loc, sizeof(loc), "node %d ensemble[%d]", node_id, h_idx);
        }

        // Parse sidecar's computed_layer2_hash
        char sidecar_hash[65] = {0};
        char err[128] = {0};
        int parse_ok = FeatureOverlay_ParseLayer2HashFromSidecar(
            h->model_path, sidecar_hash, sizeof(sidecar_hash), err, sizeof(err));

        if (!parse_ok) {
            // Sidecar missing or unparseable but stamp claims overlay
            fprintf(stderr,
                "[overlay] %s: %s role=%s stamp claims overlay_hash=%s "
                "but sidecar parse FAILED: %s\n",
                strict ? "REFUSE (strict mode)" : "WARN",
                loc, role_name, h->overlay_hash, err);
            mismatch_count++;
            return;
        }

        // Compare sidecar hash to stamp body hash
        if (strncmp(sidecar_hash, h->overlay_hash, 64) != 0) {
            fprintf(stderr,
                "[overlay] %s: %s role=%s stamp claims overlay_hash=%s "
                "but sidecar computed_layer2_hash=%s (sidecar tampered "
                "or wrong sidecar copied next to model file)\n",
                strict ? "REFUSE (strict mode)" : "WARN",
                loc, role_name, h->overlay_hash, sidecar_hash);
            mismatch_count++;
        }
    };

    // 1. Single-zoo: 4 roles
    if (zoo) {
        check_handle(&zoo->buy_signal, "buy_signal", -1);
        check_handle(&zoo->barrier,    "barrier",    -1);
        check_handle(&zoo->regime,     "regime",     -1);
        check_handle(&zoo->exit,       "exit",       -1);
    }
    // 2. Ensemble: 4 roles × N horizons (parallel-array iteration)
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

    if (mismatch_count > 0 && strict) {
        fprintf(stderr,
            "[overlay] FATAL: node %d had %d overlay mismatch(es) in "
            "strict mode. Set held_out_gate_strict=0 (warn-only) OR "
            "regenerate sidecar via tools/feature_overlay.py write OR "
            "retrain the model with current overlay.\n",
            node_id, mismatch_count);
        return -1;
    }
    return 0;
}

}  // namespace tt//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[FeatureOverlay_PostLoadVerify]
//======================================================================

