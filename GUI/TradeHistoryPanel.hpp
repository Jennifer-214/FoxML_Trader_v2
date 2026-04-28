#pragma once
// TradeHistoryPanel — sortable table of all trades from CSV
// shows entry, exit, P&L, reason, hold time per trade

#include "imgui.h"
#include "FoxmlTheme.hpp"
#include "TradeReader.hpp"
#include "../Strategies/StrategyInterface.hpp"  // STRATEGY_SHORT_NAMES, NUM_STRATEGIES

// extended trade info parsed from CSV for the history table
struct TradeHistoryEntry {
    double entry_price, exit_price;
    double qty, pnl, fee;
    char reason[8];  // "TP", "SL", "TIME", etc.
    char strategy[8]; // "MR", "MOM", "DIP", "EMA"
    int tick;
    int core_id;     // P.3 partials: which core fired this trade
    int leg;         // P.3 partials: 0 = leg A or single, 1 = leg B
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

    // Sharded trade log format (CoreFrameworks/ShardedTradeLog.hpp v3):
    //   E rows: timestamp,core_id,strategy_id,E,event_price,entry_price,
    //           0,0,entry_fee,balance,trade_size                  (11 cols)
    //   X rows: timestamp,core_id,strategy_id,X,event_price,entry_price,
    //           exit_price,net_pnl,total_fees,balance,trade_size  (11 cols)
    //
    // The panel only displays X rows (completed exits). Each leg exit
    // produces a separate X row when partial exits are enabled — leg A
    // and leg B exits are shown as independent rows with the same
    // entry_price but potentially different exit_price/pnl/fees.
    //
    // No header row in the sharded log — no skip-first.
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '\0' || line[0] == '\n' || line[0] == '#') continue;

        // Column 3: 'E' or 'X'. Quick reject on non-X rows.
        char kind_s[8];
        csv_field(line, 3, kind_s, sizeof(kind_s));
        if (kind_s[0] != 'X') continue;
        if (th->count >= MAX_HISTORY) break;

        char tick_s[24], core_s[8], strat_s[8];
        char entry_s[32], exit_s[32], qty_s[32], pnl_s[32], fees_s[32];
        csv_field(line, 0,  tick_s,  sizeof(tick_s));
        csv_field(line, 1,  core_s,  sizeof(core_s));
        csv_field(line, 2,  strat_s, sizeof(strat_s));
        csv_field(line, 5,  entry_s, sizeof(entry_s));
        csv_field(line, 6,  exit_s,  sizeof(exit_s));
        csv_field(line, 7,  pnl_s,   sizeof(pnl_s));
        csv_field(line, 8,  fees_s,  sizeof(fees_s));
        csv_field(line, 10, qty_s,   sizeof(qty_s));

        TradeHistoryEntry *e = &th->entries[th->count++];
        e->entry_price = atof(entry_s);
        e->exit_price  = atof(exit_s);
        e->qty         = atof(qty_s);
        e->pnl         = atof(pnl_s);
        e->fee         = atof(fees_s);
        e->tick        = (int)atoll(tick_s);
        e->core_id     = atoi(core_s);

        // P.3 partials: core_id in the CSV is actually the PORTFOLIO SLOT
        // (the drainer passes Sharded_LegSlot result as Submit's core_id).
        // Slot c → core (c/2), leg (c%2) when partials enabled. With
        // partials disabled, slot == core_id and leg == 0.
        // Heuristic: even slot = leg A, odd slot = leg B. Works for both
        // single-position (slot==core_id, leg always 0) and paired
        // (slots 2c+0 + 2c+1) modes.
        e->leg = e->core_id & 1;

        // Reason: derive from price direction (exit vs entry), not P&L
        // sign. The sharded log doesn't carry an explicit reason field
        // yet, but the gate that fired is determined by price crossing:
        //   exit_price > entry_price → TP gate fired (price hit upper)
        //   exit_price < entry_price → SL gate fired (price hit lower)
        //   exit_price == entry_price → noise / no-move (rare)
        // Pre-v4.7.5 used P&L sign which lied when fees ate a real TP
        // (small TP gain - 2× taker fees = negative P&L → labeled "SL"
        // even though the TP gate is what fired).
        // v4.7.19: tolerance-based FLAT. With sub-cent price diffs that
        // round to the same display value (e.g. exit $76740.02 vs entry
        // $76740.05), the strict < / > comparison labels micro-moves as
        // SL/TP. Manual closes + duplicate-fill ghost rows commonly land
        // here. Threshold: 0.5 basis points = 0.005% of entry, same
        // FLICKER_BPS used by the chart's neutral-candle threshold.
        double price_diff = e->exit_price - e->entry_price;
        double flat_thr   = e->entry_price * 0.00005;
        if      (price_diff >  flat_thr) strncpy(e->reason, "TP", 7);
        else if (price_diff < -flat_thr) strncpy(e->reason, "SL", 7);
        else                             strncpy(e->reason, "FLAT", 7);
        e->reason[7] = '\0';

        // Strategy: column 2 is numeric strategy_id (uint8_t). Use the
        // canonical lookup table from StrategyInterface.hpp — bounds-
        // checked, automatically picks up new strategies as they're
        // added without touching this panel.
        int sid = atoi(strat_s);
        const char *sname = (sid >= 0 && sid < (int)NUM_STRATEGIES)
            ? STRATEGY_SHORT_NAMES[sid] : "?";
        strncpy(e->strategy, sname, 7);
        e->strategy[7] = '\0';
    }
    fclose(f);
}

static inline void GUI_Panel_TradeHistory(TradeHistory *th, int partial_exit_enabled = 0) {
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

    // v4.7.18: explicit Core + Leg columns. Pre-v4.7.18 the leg was buried
    // as a ".B" suffix on the strategy name; the actual core_id was hidden
    // entirely (CSV stores portfolio SLOT in the core_id field, which under
    // partials is 2*core+leg). Now: Core column shows real core (slot/2
    // when partials enabled, slot otherwise); Leg column shows A/B/–.
    if (ImGui::BeginTable("##trades", 11, flags, ImVec2(0, -1))) {
        ImGui::TableSetupColumn("#",      ImGuiTableColumnFlags_WidthFixed, 30);
        ImGui::TableSetupColumn("Core",   ImGuiTableColumnFlags_WidthFixed, 35);
        ImGui::TableSetupColumn("Leg",    ImGuiTableColumnFlags_WidthFixed, 30);
        ImGui::TableSetupColumn("Entry",  ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Exit",   ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("P&L",    ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Fee",    ImGuiTableColumnFlags_WidthFixed, 55);
        ImGui::TableSetupColumn("Reason", ImGuiTableColumnFlags_WidthFixed, 45);
        ImGui::TableSetupColumn("Strat",  ImGuiTableColumnFlags_WidthFixed, 45);
        ImGui::TableSetupColumn("In",     ImGuiTableColumnFlags_WidthFixed, 65);
        ImGui::TableSetupColumn("Out",    ImGuiTableColumnFlags_WidthFixed, 65);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        // render newest first
        for (int i = th->count - 1; i >= 0; i--) {
            const TradeHistoryEntry *e = &th->entries[i];
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            ImGui::Text("%d", th->count - 1 - i + 1);

            // v4.7.18: actual core_id + leg breakdown. CSV's core_id field
            // is the portfolio SLOT (slot c → core c/2, leg c%2 under
            // partials). With partials disabled, slot == core, leg == 0.
            int actual_core = partial_exit_enabled ? (e->core_id >> 1) : e->core_id;
            ImGui::TableNextColumn();
            ImGui::TextColored(FoxmlColors::wheat, "C%d", actual_core);
            ImGui::TableNextColumn();
            if (partial_exit_enabled) {
                ImVec4 leg_col = (e->leg == 0) ? FoxmlColors::green_b : FoxmlColors::sand;
                ImGui::TextColored(leg_col, "%s", e->leg == 0 ? "A" : "B");
            } else {
                ImGui::TextColored(FoxmlColors::comment, "–");
            }

            ImGui::TableNextColumn();
            ImGui::Text("$%.0f", e->entry_price);

            ImGui::TableNextColumn();
            ImGui::Text("$%.0f", e->exit_price);

            ImGui::TableNextColumn();
            ImVec4 pnl_col = (e->pnl >= 0) ? FoxmlColors::green_b : FoxmlColors::red;
            ImGui::TextColored(pnl_col, "$%+.2f", e->pnl);

            ImGui::TableNextColumn();
            // red fee = fees killed an otherwise profitable trade
            double gross = (e->exit_price - e->entry_price) * e->qty;
            ImVec4 fee_col = (gross > 0 && e->pnl < 0) ? FoxmlColors::red : FoxmlColors::comment;
            ImGui::TextColored(fee_col, "$%.2f", e->fee);

            ImGui::TableNextColumn();
            // Color reason by gate type (TP=green, SL=red, FLAT=gray).
            // Note: a "TP" row can still have negative P&L when fees > gain
            // — that's a fee-bleed signal, distinct from a real SL hit.
            // The reason column shows what GATE fired; the P&L column shows
            // whether you made money. Both can disagree; that's diagnostic.
            ImVec4 reason_col;
            if (strcmp(e->reason, "TP") == 0)        reason_col = FoxmlColors::green_b;
            else if (strcmp(e->reason, "SL") == 0)   reason_col = FoxmlColors::red;
            else                                      reason_col = FoxmlColors::comment;
            ImGui::TextColored(reason_col, "%s", e->reason);

            ImGui::TableNextColumn();
            // v4.7.19: strip .B suffix — Leg column already conveys the
            // leg, so the suffix was redundant after v4.7.18 added the
            // dedicated Leg column.
            ImGui::TextColored(FoxmlColors::comment, "%s", e->strategy);

            ImGui::TableNextColumn();
            ImGui::Text("$%.0f", e->entry_price * e->qty);

            ImGui::TableNextColumn();
            ImGui::Text("$%.0f", e->exit_price * e->qty);
        }

        ImGui::EndTable();
    }

    ImGui::End();
}
