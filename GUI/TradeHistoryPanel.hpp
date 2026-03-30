#pragma once
// TradeHistoryPanel — sortable table of all trades from CSV
// shows entry, exit, P&L, reason, hold time per trade

#include "imgui.h"
#include "FoxmlTheme.hpp"
#include "TradeReader.hpp"

// extended trade info parsed from CSV for the history table
struct TradeHistoryEntry {
    double entry_price, exit_price;
    double qty, pnl;
    char reason[8];  // "TP", "SL", "TIME", etc.
    int tick;
};

static constexpr int MAX_HISTORY = 256;

struct TradeHistory {
    TradeHistoryEntry entries[MAX_HISTORY];
    int count;
    char csv_path[256];
    long last_size;
};

static inline void TradeHistory_Init(TradeHistory *th, const char *path) {
    memset(th, 0, sizeof(*th));
    strncpy(th->csv_path, path, 255);
}

static inline void TradeHistory_Refresh(TradeHistory *th) {
    struct stat st;
    if (stat(th->csv_path, &st) != 0) return;
    if (st.st_size == th->last_size) return;
    th->last_size = st.st_size;
    th->count = 0;

    FILE *f = fopen(th->csv_path, "r");
    if (!f) return;

    char line[1024];
    int first = 1;
    // CSV: tick,side,price,quantity,entry_price,delta_pct,exit_reason,...
    while (fgets(line, sizeof(line), f)) {
        if (first) { first = 0; continue; }

        char side[16], price_s[32], qty_s[32], entry_s[32], reason_s[16];
        csv_field(line, 1, side, sizeof(side));
        if (strcmp(side, "SELL") != 0) continue;
        if (th->count >= MAX_HISTORY) break;

        csv_field(line, 2, price_s, sizeof(price_s));
        csv_field(line, 3, qty_s, sizeof(qty_s));
        csv_field(line, 4, entry_s, sizeof(entry_s));
        csv_field(line, 6, reason_s, sizeof(reason_s));

        char tick_s[16];
        csv_field(line, 0, tick_s, sizeof(tick_s));

        TradeHistoryEntry *e = &th->entries[th->count++];
        e->exit_price = atof(price_s);
        e->entry_price = atof(entry_s);
        if (e->entry_price < 1.0) e->entry_price = e->exit_price;
        e->qty = atof(qty_s);
        e->pnl = (e->exit_price - e->entry_price) * e->qty;
        e->tick = atoi(tick_s);
        strncpy(e->reason, reason_s, 7);
        e->reason[7] = '\0';
    }
    fclose(f);
}

static inline void GUI_Panel_TradeHistory(TradeHistory *th) {
    ImGui::Begin("Trade History");

    TradeHistory_Refresh(th);

    if (th->count == 0) {
        ImGui::TextColored(FoxmlColors::comment, "no completed trades yet");
        ImGui::End();
        return;
    }

    ImGui::TextColored(FoxmlColors::primary, "TRADE HISTORY");
    ImGui::SameLine();
    ImGui::TextColored(FoxmlColors::comment, "(%d trades)", th->count);
    ImGui::Separator();

    ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable;

    if (ImGui::BeginTable("##trades", 6, flags, ImVec2(0, -1))) {
        ImGui::TableSetupColumn("#",      ImGuiTableColumnFlags_WidthFixed, 30);
        ImGui::TableSetupColumn("Entry",  ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Exit",   ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("P&L",    ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Reason", ImGuiTableColumnFlags_WidthFixed, 45);
        ImGui::TableSetupColumn("Qty",    ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        // render newest first
        for (int i = th->count - 1; i >= 0; i--) {
            const TradeHistoryEntry *e = &th->entries[i];
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            ImGui::Text("%d", th->count - 1 - i + 1);

            ImGui::TableNextColumn();
            ImGui::Text("$%.0f", e->entry_price);

            ImGui::TableNextColumn();
            ImGui::Text("$%.0f", e->exit_price);

            ImGui::TableNextColumn();
            ImVec4 pnl_col = (e->pnl >= 0) ? FoxmlColors::green_b : FoxmlColors::red;
            ImGui::TextColored(pnl_col, "$%+.2f", e->pnl);

            ImGui::TableNextColumn();
            ImVec4 reason_col = (strcmp(e->reason, "TP") == 0) ? FoxmlColors::green_b : FoxmlColors::red;
            ImGui::TextColored(reason_col, "%s", e->reason);

            ImGui::TableNextColumn();
            ImGui::Text("%.6f", e->qty);
        }

        ImGui::EndTable();
    }

    ImGui::End();
}
