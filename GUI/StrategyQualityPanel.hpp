#pragma once
//==============================================================================
// [STRATEGY QUALITY PANEL] — v5.7.6
//==============================================================================
// Reads health.jsonl on-demand (Refresh button) and renders per-strategy
// quality aggregates from cat="entry" / cat="exit" lines emitted by v5.7.1.
//
// Design choices:
//   - Refresh-button (not 5s background thread) — simpler, cheaper, no
//     thread/file handle lifecycle to manage. Operator hits Refresh when
//     they want updated numbers.
//   - Tail-read last N=2000 lines (caps panel cost regardless of file size)
//   - In-place line parsing — no JSON library; the v5.7.1 log lines have a
//     known format with key=value pairs in the msg field
//   - LC_NUMERIC=C pinned around strtod calls to match the writer side
//   - No persistence — aggregates computed per Refresh, panel state is
//     ephemeral within the GUI session
//==============================================================================

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <ctime>
#include <locale.h>
#include "imgui.h"
#include "FoxmlTheme.hpp"
#include "../Strategies/StrategyInterface.hpp"

namespace tt {

struct StrategyQualityAggregate {
    uint32_t entries = 0;
    uint32_t exits   = 0;
    uint32_t wins    = 0;
    uint32_t losses  = 0;
    double   net_pnl_sum   = 0.0;
    double   fees_sum      = 0.0;
    double   notional_sum  = 0.0;  // for bps computation
    // Per-regime entry counts (indexed by regime id 0..NUM_REGIMES-1)
    uint32_t entries_by_regime[5] = {0,0,0,0,0};
    double   net_by_regime[5]     = {0,0,0,0,0};
};

struct StrategyQualityState {
    bool   loaded = false;
    char   last_error[128] = {0};
    int    lines_parsed = 0;
    int    entries_parsed = 0;
    int    exits_parsed = 0;
    StrategyQualityAggregate agg[NUM_STRATEGIES];
};

// Parse "key=val" out of a log line msg. Returns 1 if key found and val
// written into out (caller-supplied buffer). val is whitespace-terminated.
inline int sq_parse_kv(const char* line, const char* key,
                        char* out, size_t cap) {
    size_t klen = strlen(key);
    const char* p = line;
    while ((p = strstr(p, key)) != nullptr) {
        // Need to be at start of line or preceded by whitespace
        if (p != line && *(p - 1) != ' ' && *(p - 1) != '"') {
            p++;
            continue;
        }
        if (p[klen] != '=') {
            p++;
            continue;
        }
        const char* val = p + klen + 1;
        size_t i = 0;
        while (i + 1 < cap && val[i] && val[i] != ' ' && val[i] != '"' && val[i] != '\n') {
            out[i] = val[i];
            i++;
        }
        out[i] = '\0';
        return 1;
    }
    return 0;
}

// Tail-read last `max_lines` from path. Returns number of lines read into
// `lines` (caller-supplied buffer of size max_lines × line_cap). Each
// line is null-terminated.
inline int sq_tail_read(const char* path, int max_lines, int line_cap,
                         char* lines_buf, char* error_out, size_t error_cap) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        snprintf(error_out, error_cap, "fopen failed: %s", path);
        return 0;
    }
    // Find file size, seek to ~max_lines × 256 bytes from end (heuristic).
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    long start = size - (long)max_lines * 256;
    if (start < 0) start = 0;
    fseek(f, start, SEEK_SET);
    // Discard partial line at start of read window (unless start==0).
    char throwaway[2048];
    if (start > 0) {
        if (!fgets(throwaway, sizeof(throwaway), f)) {
            fclose(f);
            return 0;
        }
    }
    int count = 0;
    int head = 0;  // ring index — when full, overwrite oldest
    char tmp[2048];
    while (fgets(tmp, sizeof(tmp), f)) {
        char* dest = lines_buf + (size_t)head * (size_t)line_cap;
        size_t n = strlen(tmp);
        if (n >= (size_t)line_cap) n = line_cap - 1;
        memcpy(dest, tmp, n);
        dest[n] = '\0';
        // Strip trailing newline
        if (n > 0 && dest[n - 1] == '\n') dest[n - 1] = '\0';
        head = (head + 1) % max_lines;
        if (count < max_lines) count++;
    }
    fclose(f);
    // Compact: if ring wrapped, rotate so oldest first
    if (count == max_lines && head != 0) {
        // Rotate by `head` slots. Simple O(N) approach: copy to temp, write back.
        char* rotated = (char*)malloc((size_t)max_lines * (size_t)line_cap);
        if (rotated) {
            for (int i = 0; i < max_lines; ++i) {
                int src = (head + i) % max_lines;
                memcpy(rotated + (size_t)i * line_cap,
                       lines_buf + (size_t)src * line_cap, line_cap);
            }
            memcpy(lines_buf, rotated, (size_t)max_lines * (size_t)line_cap);
            free(rotated);
        }
    }
    return count;
}

inline void StrategyQuality_Refresh(StrategyQualityState* state,
                                     const char* health_log_path) {
    state->loaded = false;
    state->last_error[0] = '\0';
    state->lines_parsed = state->entries_parsed = state->exits_parsed = 0;
    for (int i = 0; i < NUM_STRATEGIES; ++i) {
        state->agg[i] = StrategyQualityAggregate{};
    }
    if (!health_log_path || !health_log_path[0]) {
        snprintf(state->last_error, sizeof(state->last_error),
                 "health_log_path not configured");
        return;
    }
    constexpr int MAX_LINES = 2000;
    constexpr int LINE_CAP  = 1024;
    char* lines = (char*)malloc((size_t)MAX_LINES * LINE_CAP);
    if (!lines) {
        snprintf(state->last_error, sizeof(state->last_error),
                 "alloc failed");
        return;
    }
    int n = sq_tail_read(health_log_path, MAX_LINES, LINE_CAP, lines,
                          state->last_error, sizeof(state->last_error));
    state->lines_parsed = n;
    // LC_NUMERIC=C around strtod calls so locale flips can't corrupt
    // numeric parsing (matches writer-side LC_NUMERIC pinning).
    char* prev_locale = setlocale(LC_NUMERIC, "C");
    for (int i = 0; i < n; ++i) {
        const char* line = lines + (size_t)i * LINE_CAP;
        char buf[64];
        // Filter cat=entry / cat=exit
        const char* cat_pos = strstr(line, "\"cat\":\"");
        if (!cat_pos) continue;
        bool is_entry = strncmp(cat_pos + 7, "entry\"", 6) == 0;
        bool is_exit  = strncmp(cat_pos + 7, "exit\"",  5) == 0;
        if (!is_entry && !is_exit) continue;
        if (!sq_parse_kv(line, "strat", buf, sizeof(buf))) continue;
        int sid = atoi(buf);
        if (sid < 0 || sid >= NUM_STRATEGIES) continue;
        auto& a = state->agg[sid];
        if (is_entry) {
            a.entries++;
            state->entries_parsed++;
            if (sq_parse_kv(line, "regime", buf, sizeof(buf))) {
                int r = atoi(buf);
                if (r >= 0 && r < 5) a.entries_by_regime[r]++;
            }
            if (sq_parse_kv(line, "entry_notional", buf, sizeof(buf))) {
                a.notional_sum += strtod(buf, nullptr);
            }
        } else {
            a.exits++;
            state->exits_parsed++;
            int was_win = 0;
            if (sq_parse_kv(line, "was_win", buf, sizeof(buf))) was_win = atoi(buf);
            if (was_win) a.wins++; else a.losses++;
            double net_pnl = 0.0;
            if (sq_parse_kv(line, "net_pnl", buf, sizeof(buf))) {
                net_pnl = strtod(buf, nullptr);
                a.net_pnl_sum += net_pnl;
            }
            if (sq_parse_kv(line, "total_fees", buf, sizeof(buf))) {
                a.fees_sum += strtod(buf, nullptr);
            }
            // Track per-regime exit pnl using regime captured at entry —
            // not perfect (regime may change between E and X), but a
            // reasonable proxy. Use the LAST entry's regime per strategy
            // as fallback.
            // (Not strictly accurate; v5.8+ could pair entry/exit by slot.)
            (void)0;
        }
    }
    setlocale(LC_NUMERIC, prev_locale);
    free(lines);
    state->loaded = (n > 0);
}

inline void GUI_Panel_StrategyQuality(StrategyQualityState* state,
                                       const char* health_log_path) {
    ImGui::Begin("Strategy Quality");
    ImGui::TextColored(FoxmlColors::sand, "STRATEGY QUALITY");
    ImGui::SameLine(0, 20);
    if (ImGui::Button("Refresh")) {
        StrategyQuality_Refresh(state, health_log_path);
    }
    if (state->last_error[0]) {
        ImGui::TextColored(FoxmlColors::yellow, "%s", state->last_error);
    }
    if (!state->loaded) {
        ImGui::TextColored(FoxmlColors::comment,
            "Click Refresh. Reads last 2000 lines from health.jsonl");
        ImGui::TextColored(FoxmlColors::comment,
            "(set health_log_path in engine.cfg to enable per-trade logging)");
        ImGui::End();
        return;
    }
    ImGui::TextColored(FoxmlColors::comment,
        "lines=%d  entries=%d  exits=%d",
        state->lines_parsed, state->entries_parsed, state->exits_parsed);
    ImGui::Separator();

    ImGuiTableFlags tf = ImGuiTableFlags_BordersInnerV |
                          ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("##sq", 7, tf)) {
        ImGui::TableSetupColumn("Strat",   ImGuiTableColumnFlags_WidthFixed, 50);
        ImGui::TableSetupColumn("Trades",  ImGuiTableColumnFlags_WidthFixed, 55);
        ImGui::TableSetupColumn("W",       ImGuiTableColumnFlags_WidthFixed, 35);
        ImGui::TableSetupColumn("L",       ImGuiTableColumnFlags_WidthFixed, 35);
        ImGui::TableSetupColumn("Net $",   ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Fees $",  ImGuiTableColumnFlags_WidthFixed, 65);
        ImGui::TableSetupColumn("Net bps", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (int i = 0; i < NUM_STRATEGIES; ++i) {
            const auto& a = state->agg[i];
            if (a.entries == 0 && a.exits == 0) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(FoxmlColors::wheat, "%s", STRATEGY_SHORT_NAMES[i]);
            ImGui::TableNextColumn();
            ImGui::Text("%u/%u", a.entries, a.exits);
            ImGui::TableNextColumn();
            ImGui::TextColored(FoxmlColors::green, "%u", a.wins);
            ImGui::TableNextColumn();
            ImGui::TextColored(FoxmlColors::red, "%u", a.losses);
            ImGui::TableNextColumn();
            auto pnl_col = a.net_pnl_sum >= 0 ? FoxmlColors::green : FoxmlColors::red;
            ImGui::TextColored(pnl_col, "%+.2f", a.net_pnl_sum);
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", a.fees_sum);
            ImGui::TableNextColumn();
            double bps = (a.notional_sum > 0.0)
                ? (a.net_pnl_sum / a.notional_sum) * 10000.0 : 0.0;
            ImGui::TextColored(pnl_col, "%+.1f", bps);
        }
        ImGui::EndTable();
    }
    // Per-regime entry breakdown — small table per strategy with non-zero.
    ImGui::Separator();
    ImGui::TextColored(FoxmlColors::comment, "Entries by regime");
    static const char* regime_names[] = {"RANGE","TREND","VOLAT","TR_DN","EMACR"};
    if (ImGui::BeginTable("##sqr", 6, tf)) {
        ImGui::TableSetupColumn("Strat", ImGuiTableColumnFlags_WidthFixed, 50);
        for (int r = 0; r < 5; ++r) {
            ImGui::TableSetupColumn(regime_names[r],
                                     ImGuiTableColumnFlags_WidthFixed, 60);
        }
        ImGui::TableHeadersRow();
        for (int i = 0; i < NUM_STRATEGIES; ++i) {
            const auto& a = state->agg[i];
            if (a.entries == 0) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(FoxmlColors::wheat, "%s", STRATEGY_SHORT_NAMES[i]);
            for (int r = 0; r < 5; ++r) {
                ImGui::TableNextColumn();
                if (a.entries_by_regime[r] > 0) {
                    ImGui::Text("%u", a.entries_by_regime[r]);
                } else {
                    ImGui::TextColored(FoxmlColors::comment, "—");
                }
            }
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

}  // namespace tt
