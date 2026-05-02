// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [ML STATUS PANEL — v5.9.0b]
//======================================================================================================
// Per-core ML observability surface. Shows for each STRATEGY_ML core:
//   - Model load state: loaded / failed / no-model-configured (tri-state,
//     distinct semantics per V5_9_AUDIT-#2)
//   - Last prediction + threshold + effective threshold (post-confidence-
//     damping) + confidence (V5_9_AUDIT-#3)
//   - NaN/Inf event counters (feature pack + prediction)
//   - ConfidenceScorer IC + RMSE for diagnostic
//
// Stateless render — direct call from main loop, NOT in FOREACH_PANEL(X)
// registry. All data flows through TUISnapshot.per_core[i]. Mirrors the
// v5.8.6b engine header pattern.
//
// Closes silent-failure visibility gap from v5.8 paper testing: operator
// can no longer paper-soak for hours without knowing ML failed to load,
// fed garbage features, or fell through to SimpleDip.
//======================================================================================================
#pragma once

#include "imgui.h"
#include "FoxmlTheme.hpp"
#include "../DataStream/EngineTUI.hpp"

namespace tt {

inline void MLStatus_Render(const TUISnapshot* snap) {
    if (!snap) return;
    if (ImGui::Begin("ML Status")) {
        if (snap->per_core_count == 0) {
            ImGui::TextDisabled("No active cores.");
            ImGui::End();
            return;
        }

        // Header row.
        ImGui::TextColored(FoxmlColors::sand,
            "Per-core ML state. Failed/NaN counters surface silent failures.");
        ImGui::Separator();

        // Per-core rows.
        for (int i = 0; i < snap->per_core_count && i < 16; ++i) {
            const auto& pc = snap->per_core[i];

            // Only render rows for ML cores OR cores with ML symptoms
            // (load failed / NaN events) — keeps the panel quiet for
            // non-ML cores in mixed deployments.
            int has_ml_signal = pc.is_ml || pc.ml_model_load_failed ||
                                 pc.ml_nan_feature_events > 0 ||
                                 pc.ml_nan_prediction_events > 0;
            if (!has_ml_signal) continue;

            ImGui::TextColored(FoxmlColors::sand, "core %d:", i);
            ImGui::SameLine(0, 8);

            // Model state — tri-state.
            if (pc.ml_model_load_failed) {
                ImGui::TextColored(FoxmlColors::red, "model: LOAD FAILED");
                ImGui::SetItemTooltip(
                    "ML strategy was selected but the model file refused to load.\n"
                    "Stamp validation failed, file missing, or held_out_gate_strict=1\n"
                    "with mismatched registry hash. Check the boot log for details.\n"
                    "Operator action: verify cfg path + retrain if needed.");
            } else if (pc.ml_model_loaded) {
                ImGui::TextColored(FoxmlColors::green, "model: loaded");
            } else {
                ImGui::TextColored(FoxmlColors::sand, "model: (none configured)");
            }

            // Decision context — only meaningful when model loaded.
            if (pc.ml_model_loaded) {
                ImGui::SameLine(0, 14);
                ImGui::TextColored(FoxmlColors::sand, "pred:");
                ImGui::SameLine();
                ImGui::Text("%.4f", pc.ml_last_prediction);

                ImGui::SameLine(0, 8);
                ImGui::TextColored(FoxmlColors::sand, "thr:");
                ImGui::SameLine();
                // Effective threshold may differ from base when confidence
                // is enabled — show both if they differ.
                if (pc.ml_last_effective_threshold > 0.0 &&
                    pc.ml_last_effective_threshold != pc.ml_last_threshold) {
                    ImGui::Text("%.3f (eff: %.3f)",
                                pc.ml_last_threshold,
                                pc.ml_last_effective_threshold);
                    ImGui::SetItemTooltip(
                        "Base threshold (cfg ml_buy_threshold) vs effective\n"
                        "(post-confidence-damping). When confidence is low,\n"
                        "effective rises (entry harder); when high, drops\n"
                        "(entry easier). See StrategyParameters.hpp damping logic.");
                } else if (pc.ml_last_threshold > 0.0) {
                    ImGui::Text("%.3f", pc.ml_last_threshold);
                } else {
                    ImGui::TextDisabled("—");
                }

                if (pc.ml_last_confidence > 0.0) {
                    ImGui::SameLine(0, 8);
                    ImGui::TextColored(FoxmlColors::sand, "conf:");
                    ImGui::SameLine();
                    ImGui::Text("%.3f", pc.ml_last_confidence);
                }
            }

            // NaN/Inf counters — bright red if non-zero, dim if clean.
            if (pc.ml_nan_feature_events > 0 || pc.ml_nan_prediction_events > 0) {
                ImGui::Indent(20);
                ImGui::TextColored(FoxmlColors::red,
                    "NaN events: features=%u predictions=%u",
                    pc.ml_nan_feature_events, pc.ml_nan_prediction_events);
                ImGui::SetItemTooltip(
                    "Features_PackAll returned -1 sentinel (FPN out-of-range\n"
                    "or float NaN/Inf at packer boundary), or Model_Predict\n"
                    "produced NaN/Inf. Both cases skip the prediction this\n"
                    "cycle (no entry, fall-through to SimpleDip). Persistent\n"
                    "non-zero values mean upstream state corruption — check\n"
                    "depth/orderbook WS, rolling stats, or recent retrain.");
                ImGui::Unindent(20);
            }
        }
    }
    ImGui::End();
}

}  // namespace tt
