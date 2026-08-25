#pragma once

//======================================================================
// [FILE]_[GUI/TradeHistoryPanel.hpp]
//----------------------------------------------------------------------
// [TAG]_[[GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[sortable Trade History table read from the engine CSV — entry/exit/qty/P&L/fee/reason/strategy + hold time per trade, partial-exit aware; refreshes on file-size change]
//======================================================================
// TradeHistoryPanel — sortable table of all trades from CSV
// shows entry, exit, P&L, reason, hold time per trade

#include "imgui.h"
#include "FoxmlTheme.hpp"
#include "TradeReader.hpp"
#include "../Strategies/StrategyInterface.hpp"  // STRATEGY_SHORT_NAMES, NUM_STRATEGIES

//======================================================================
// [STRUCT]_[TradeHistoryEntry]
//----------------------------------------------------------------------
// [TAG]_[[GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[one parsed trade row for the history table — entry/exit prices, qty/P&L/fee, reason + strategy tags, node/leg (partials), and hold time]
//======================================================================
// extended trade info parsed from CSV for the history table
//======================================================================
// [CODE]
//======================================================================
struct TradeHistoryEntry {
    double entry_price, exit_price;
    double qty, pnl, fee;
    char reason[8];  // "TP", "SL", "TIME", etc.
    char strategy[8]; // "MR", "MOM", "DIP", "EMA"
    int tick;
    int node_id;     // P.3 partials: which core fired this trade
    int leg;         // P.3 partials: 0 = leg A or single, 1 = leg B
    // v5.5.2: hold time. Computed from matching E (entry) row's timestamp_us
    // → X (exit) row's timestamp_us via a per-slot state machine in the
    // CSV reader. -1 = no matching E row found (CSV truncated, log
    // rotated, etc.).
    double hold_secs;
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
// [SIZE]_[80B]
// [ALIGN]_[8]
// [CACHE_LINES]_[2]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[TradeHistoryEntry]
//======================================================================

static constexpr int MAX_HISTORY = 256;

//======================================================================
// [STRUCT]_[TradeHistory]
//----------------------------------------------------------------------
// [TAG]_[[GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the loaded trade-history rows + CSV path + the file-size cache for change detection]
//======================================================================
//======================================================================
// [CODE]
//======================================================================
struct TradeHistory {
    TradeHistoryEntry entries[MAX_HISTORY];
    int count;
    char csv_path[256];
    long last_size;
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-07-18]
// [SIZE]_[20752B]
// [ALIGN]_[8]
// [CACHE_LINES]_[325]
// [STRADDLE]_[none]
//======================================================================
// [END_STRUCT]_[TradeHistory]
//======================================================================

//======================================================================
// [FUNCTION]_[TradeHistory_Init]
//----------------------------------------------------------------------
// [TAG]_[[GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[init a TradeHistory for a CSV path]
//======================================================================
//======================================================================
// [CODE]
//======================================================================
static inline void TradeHistory_Init(TradeHistory *th, const char *path) {
    memset(th, 0, sizeof(*th));
    strncpy(th->csv_path, path, 255);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[TradeHistory_Init]
//======================================================================

//======================================================================
// [FUNCTION]_[TradeHistory_Refresh]
//----------------------------------------------------------------------
// [TAG]_[[GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[reload the trade CSV if it grew — pair entry(E)/exit(X) rows into entries with hold time via a per-slot state machine]
//======================================================================
//======================================================================
// [CODE]
//======================================================================
static inline void TradeHistory_Refresh(TradeHistory *th, int partial_exit_enabled) {
    struct stat st;
    if (stat(th->csv_path, &st) != 0) return;
    if (st.st_size == th->last_size) return;
    th->last_size = st.st_size;
    th->count = 0;

    FILE *f = fopen(th->csv_path, "r");
    if (!f) return;

    // Sharded trade log format (CoreFrameworks/ShardedTradeLog.hpp v3):
    //   E rows: timestamp,node_id,strategy_id,E,event_price,entry_price,
    //           0,0,entry_fee,balance,trade_size                  (11 cols)
    //   X rows: timestamp,node_id,strategy_id,X,event_price,entry_price,
    //           exit_price,net_pnl,total_fees,balance,trade_size  (11 cols)
    //
    // The panel only displays X rows (completed exits). Each leg exit
    // produces a separate X row when partial exits are enabled — leg A
    // and leg B exits are shown as independent rows with the same
    // entry_price but potentially different exit_price/pnl/fees.
    //
    // No header row in the sharded log — no skip-first.
    //
    // v5.5.2: track entry timestamps per slot (16 max under partials) so
    // we can compute hold_secs on each matching X row. State machine:
    //   E row arrives → store timestamp[slot] = ts
    //   X row arrives → hold = ts - entry_ts[slot] (in seconds), reset slot
    // If a slot has no recent E (file truncated, leg desync), hold_secs = -1.
    uint64_t entry_ts_us[16] = {0};
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '\0' || line[0] == '\n' || line[0] == '#') continue;

        // Column 3: 'E' or 'X'. Build the entry-timestamp map from E rows
        // before processing X rows so hold_secs is available.
        char kind_s[8];
        csv_field(line, 3, kind_s, sizeof(kind_s));
        if (kind_s[0] == 'E') {
            char ets_s[24], ecore_s[8], eslot_s[8];
            csv_field(line, 0, ets_s,   sizeof(ets_s));
            csv_field(line, 1, ecore_s, sizeof(ecore_s));
            // s5-1b v4 rows: col 13 = slot_id (the per-leg key); col 1 is now
            // the TRUE node. v3 rows: col 13 empty → col 1 WAS the slot. Key
            // the entry-ts map by SLOT either way so legs pair independently.
            csv_field(line, 13, eslot_s, sizeof(eslot_s));
            // strtol not atoi: keeps the determinism net's raw-parse count flat
            // (integer strto* is locale-immune; the atoi family is counted wholesale).
            int eslot = eslot_s[0] ? (int)strtol(eslot_s, nullptr, 10)
                                   : (int)strtol(ecore_s, nullptr, 10);
            if (eslot >= 0 && eslot < 16) {
                entry_ts_us[eslot] = (uint64_t)atoll(ets_s);
            }
            continue;
        }
        if (kind_s[0] != 'X') continue;
        if (th->count >= MAX_HISTORY) break;

        char tick_s[24], node_s[8], strat_s[8];
        char entry_s[32], exit_s[32], qty_s[32], pnl_s[32], fees_s[32];
        csv_field(line, 0,  tick_s,  sizeof(tick_s));
        csv_field(line, 1,  node_s,  sizeof(node_s));
        csv_field(line, 2,  strat_s, sizeof(strat_s));
        csv_field(line, 5,  entry_s, sizeof(entry_s));
        csv_field(line, 6,  exit_s,  sizeof(exit_s));
        csv_field(line, 7,  pnl_s,   sizeof(pnl_s));
        csv_field(line, 8,  fees_s,  sizeof(fees_s));
        csv_field(line, 10, qty_s,   sizeof(qty_s));

        // v5.11.4.C — locale-immune CSV parsing (transitive via TradeReader.hpp)
        TradeHistoryEntry *e = &th->entries[th->count++];
        e->entry_price = tt::parse_double_fast(entry_s);
        e->exit_price  = tt::parse_double_fast(exit_s);
        e->qty         = tt::parse_double_fast(qty_s);
        e->pnl         = tt::parse_double_fast(pnl_s);
        e->fee         = tt::parse_double_fast(fees_s);
        e->tick        = (int)atoll(tick_s);
        e->node_id     = atoi(node_s);

        // s5-1b: pair by SLOT. v4 rows carry it in col 13 (col 1 is now the
        // TRUE node — under partials both legs share it, so keying by col 1
        // would cross-pair legs); v3 rows fall back to col 1, which WAS the
        // slot in that era.
        char xslot_s[8];
        csv_field(line, 13, xslot_s, sizeof(xslot_s));
        int xslot = xslot_s[0] ? (int)strtol(xslot_s, nullptr, 10) : e->node_id;

        // v5.5.2: compute hold_secs from entry-row timestamp. tick_s is
        // the X row's timestamp_us; entry_ts_us[slot] was set on the
        // matching E row above. If no E row was seen for this slot
        // (CSV truncated / log rotated), hold_secs = -1.0 sentinel.
        if (xslot >= 0 && xslot < 16 && entry_ts_us[xslot] > 0) {
            uint64_t exit_ts_us = (uint64_t)atoll(tick_s);
            if (exit_ts_us > entry_ts_us[xslot]) {
                e->hold_secs = (double)(exit_ts_us - entry_ts_us[xslot])
                                / 1000000.0;
            } else {
                e->hold_secs = 0.0;  // same-tick — same-second resolution
            }
            entry_ts_us[xslot] = 0;  // consume — next E sets it again
        } else {
            e->hold_secs = -1.0;  // no matching E row
        }

        // s5-1b: leg from col 14 when present (v4). v3 fallback keeps the
        // parity heuristic on the slot value (even = leg A, odd = leg B;
        // single-slot mode → always 0 by construction).
        char leg_s[4];
        csv_field(line, 14, leg_s, sizeof(leg_s));
        e->leg = leg_s[0] ? (int)strtol(leg_s, nullptr, 10) : (xslot & 1);

        // NORMALIZE col 1 to the TRUE node, era-agnostically, so the render
        // never re-derives. v4 rows (ShardedTradeLog v4, 2026-08-23) already
        // carry the true node in col 1; v3 rows carried the SLOT there.
        // The render used to derive unconditionally, which DOUBLE-derived
        // every v4 row under partials — node 1 displayed as "C0", collapsing
        // two nodes onto one label. Absent col 13 is the v3 tell.
        if (!xslot_s[0] && partial_exit_enabled) e->node_id = (e->node_id >> 1);

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
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[TradeHistory_Refresh]
//======================================================================

//======================================================================
// [FUNCTION]_[GUI_Panel_TradeHistory]
//----------------------------------------------------------------------
// [TAG]_[[GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[render the sortable Trade History table — entry/exit/P&L/reason/hold per trade]
//======================================================================
//======================================================================
// [CODE]
//======================================================================
static inline void GUI_Panel_TradeHistory(TradeHistory *th, int partial_exit_enabled = 0) {
    ImGui::Begin("Trade History");

    TradeHistory_Refresh(th, partial_exit_enabled);

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
    // as a ".B" suffix on the strategy name; the actual node_id was hidden
    // entirely (CSV stores portfolio SLOT in the node_id field, which under
    // partials is 2*core+leg). Now: Core column shows real core (slot/2
    // when partials enabled, slot otherwise); Leg column shows A/B/–.
    if (ImGui::BeginTable("##trades", 12, flags, ImVec2(0, -1))) {  // v5.5.3: +Hold
        ImGui::TableSetupColumn("#",      ImGuiTableColumnFlags_WidthFixed, 30);
        ImGui::TableSetupColumn("Node",   ImGuiTableColumnFlags_WidthFixed, 35);
        ImGui::TableSetupColumn("Leg",    ImGuiTableColumnFlags_WidthFixed, 30);
        ImGui::TableSetupColumn("Entry",  ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Exit",   ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("P&L",    ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Fee",    ImGuiTableColumnFlags_WidthFixed, 55);
        ImGui::TableSetupColumn("Reason", ImGuiTableColumnFlags_WidthFixed, 45);
        ImGui::TableSetupColumn("Strat",  ImGuiTableColumnFlags_WidthFixed, 45);
        ImGui::TableSetupColumn("In",     ImGuiTableColumnFlags_WidthFixed, 65);
        ImGui::TableSetupColumn("Out",    ImGuiTableColumnFlags_WidthFixed, 65);
        ImGui::TableSetupColumn("Hold",   ImGuiTableColumnFlags_WidthFixed, 60);  // v5.5.2
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        // render newest first
        for (int i = th->count - 1; i >= 0; i--) {
            const TradeHistoryEntry *e = &th->entries[i];
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            ImGui::Text("%d", th->count - 1 - i + 1);

            // v4.7.18: actual node_id + leg breakdown. As of ShardedTradeLog
            // v4 the CSV's col-1 node_id IS the true node; TradeHistory_Refresh
            // normalizes v3 rows (which carried the SLOT there) at parse time,
            // so this render is era-agnostic and must NOT derive again.
            int actual_core = e->node_id;   // already the TRUE node (normalized at parse)
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

            // v5.5.2: hold time. Formatted as Xs (<60s), XmYs (1-59 min),
            // XhYm (1+ hour). Sentinel hold_secs<0 → "—" (no E row matched).
            ImGui::TableNextColumn();
            if (e->hold_secs < 0.0) {
                ImGui::TextColored(FoxmlColors::comment, "—");
            } else if (e->hold_secs < 60.0) {
                ImGui::Text("%.0fs", e->hold_secs);
            } else if (e->hold_secs < 3600.0) {
                int m = (int)(e->hold_secs / 60.0);
                int s = (int)e->hold_secs % 60;
                ImGui::Text("%dm%ds", m, s);
            } else {
                int h = (int)(e->hold_secs / 3600.0);
                int m = ((int)e->hold_secs % 3600) / 60;
                ImGui::Text("%dh%dm", h, m);
            }
        }

        ImGui::EndTable();
    }

    ImGui::End();
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[GUI_Panel_TradeHistory]
//======================================================================
