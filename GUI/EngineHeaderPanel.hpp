// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [ENGINE HEADER PANEL — v5.8.6b]
//======================================================================================================
// Single-line ImGui header showing the running engine's version + feature
// registry hash + model format version. Same content rendered in both
// engine_gui (live) and foxml_suite (training/backtest) — call
// EngineHeader_Render() from each binary's render loop.
//
// All values pulled from compile-time constants:
//   ENGINE_VERSION_STRING (Version.hpp)
//   FEATURE_REGISTRY_HASH() (FeatureRegistry.hpp — FNV-1a fold)
//   MODEL_FORMAT_VERSION (ModelInference.hpp)
//
// This panel is the operator-visible counterpart to the boot-log line
// emitted by CoreModelZoo_TryLoadRole at model load. Shows what the
// CURRENT BUILD speaks; per-loaded-model match state is in the boot log.
// (Future ship can extend this panel with per-model match status if
// the boot log proves insufficient — for now it's deliberately minimal.)
//======================================================================================================
#pragma once

#include "imgui.h"
#include "FoxmlTheme.hpp"
#include "../Version.hpp"
#include "../ML_Headers/FeatureRegistry.hpp"
#include "../ML_Headers/ModelInference.hpp"  // MODEL_FORMAT_VERSION

namespace tt {

inline void EngineHeader_Render() {
    if (ImGui::Begin("Engine")) {
        ImGui::TextColored(FoxmlColors::sand, "engine:");
        ImGui::SameLine();
        ImGui::Text("%s", ENGINE_VERSION_STRING);

        ImGui::SameLine(0, 14);
        ImGui::TextColored(FoxmlColors::sand, "format:");
        ImGui::SameLine();
        ImGui::Text("v%d", MODEL_FORMAT_VERSION);

        ImGui::SameLine(0, 14);
        ImGui::TextColored(FoxmlColors::sand, "registry:");
        ImGui::SameLine();
        ImGui::Text("%016lx", (unsigned long)FEATURE_REGISTRY_HASH());
    }
    ImGui::End();
}

}  // namespace tt
