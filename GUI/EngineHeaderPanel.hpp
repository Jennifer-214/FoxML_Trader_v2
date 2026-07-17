// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================
// [FILE]_[GUI/EngineHeaderPanel.hpp]
//----------------------------------------------------------------------
// [TAG]_[[GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[single-line engine header — release/engine/format/registry-hash from compile-time constants + the optional cfg-path and WS-heartbeat freshness from the snapshot; rendered identically in engine_gui + foxml_suite]
//======================================================================
#pragma once

#include "imgui.h"
#include "FoxmlTheme.hpp"
#include "../Version.hpp"
#include "../ML_Headers/FeatureRegistry.hpp"
#include "../ML_Headers/ModelInference.hpp"  // MODEL_FORMAT_VERSION
#include <chrono>  // v5.12.1.C — system_clock for WS heartbeat freshness

namespace tt {

//======================================================================
// [FUNCTION]_[EngineHeader_Render]
//----------------------------------------------------------------------
// [TAG]_[[GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[render the one-line Engine header — build-time version/format/registry fields, plus cfg source path + color-coded WS heartbeat freshness when a snapshot is passed]
//======================================================================
// v5.8.6b: original 3-arg version (engine + format + registry).
// v5.9.0c: optional snap arg displays the loaded cfg path. When snap is
// nullptr (legacy callers), only the 3 build-time fields render.
//======================================================================
// [CODE]
//======================================================================
inline void EngineHeader_Render(const struct TUISnapshot* snap = nullptr) {
    if (ImGui::Begin("Engine")) {
        ImGui::TextColored(FoxmlColors::sand, "release:");
        ImGui::SameLine();
        ImGui::Text("v%s", RELEASE_VERSION_STRING);
        ImGui::SameLine(0, 14);
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

        // v5.9.0c — cfg source path (V5_9_AUDIT-#4). Operators see at-a-
        // glance which cfg drove the configuration. Critical because
        // engine_gui reads engine.cfg while foxml_suite reads
        // backtest.cfg — the source-confusion was the today-bug root.
        if (snap && snap->source_cfg_path[0]) {
            ImGui::SameLine(0, 14);
            ImGui::TextColored(FoxmlColors::sand, "cfg:");
            ImGui::SameLine();
            ImGui::Text("%s", snap->source_cfg_path);
            ImGui::SetItemTooltip(
                "Cfg file path the binary parsed at boot.\n"
                "engine_gui reads engine.cfg; foxml_suite reads backtest.cfg.\n"
                "Edits to the OTHER file have no effect on this binary.\n"
                "If you expected different behavior, check which cfg you edited.");
        }

        // v5.12.1.C — WS heartbeat indicator. Color-coded by freshness:
        //   green  <100ms  — healthy
        //   yellow <1s     — degraded
        //   red    ≥5s     — stale (matches the v5.12.1.A flatten threshold class)
        // Pre-warmup (ws_last_tick_us == 0) renders dimmed gray.
        if (snap && snap->ws_last_tick_us > 0) {
            uint64_t now_us = (uint64_t)
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
            uint64_t gap_ms = (now_us > snap->ws_last_tick_us)
                ? (now_us - snap->ws_last_tick_us) / 1000ULL : 0;
            ImVec4 color = (gap_ms < 100)  ? ImVec4(0.0f, 1.0f, 0.0f, 1.0f)
                         : (gap_ms < 1000) ? ImVec4(1.0f, 1.0f, 0.0f, 1.0f)
                         : (gap_ms < 5000) ? ImVec4(1.0f, 0.6f, 0.0f, 1.0f)
                                           : ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
            ImGui::SameLine(0, 14);
            ImGui::TextColored(FoxmlColors::sand, "WS:");
            ImGui::SameLine();
            ImGui::TextColored(color, "%llu/s, %llums ago",
                (unsigned long long)(snap->ws_ticks_per_5s / 5ULL),
                (unsigned long long)gap_ms);
            ImGui::SetItemTooltip(
                "WebSocket heartbeat. Tick rate over last 5s + age of\n"
                "the most recent tick. Green <100ms, yellow <1s,\n"
                "orange <5s, red >=5s. Live producer publishes every\n"
                "incoming WS tick; backtest publishes synthetic ts so\n"
                "this row should always show a healthy rate.");
        } else if (snap) {
            ImGui::SameLine(0, 14);
            ImGui::TextColored(FoxmlColors::sand, "WS:");
            ImGui::SameLine();
            ImGui::TextDisabled("warmup");
        }
    }
    ImGui::End();
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[EngineHeader_Render]
//======================================================================

}  // namespace tt
