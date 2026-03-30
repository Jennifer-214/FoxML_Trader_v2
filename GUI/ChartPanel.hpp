#pragma once
// ChartPanel — TradingView-style charts as separate dockable windows
// Price Chart, Volume, Equity Curve — each independently arrangeable
//
// all 3 windows share prepared chart data via ChartState

#include "imgui.h"
#include "implot.h"
#include "FoxmlTheme.hpp"
#include "CandleAccumulator.hpp"
#include "TradeReader.hpp"

// chart display settings (mutable — controlled by GUI dropdowns)
struct ChartSettings {
    int visible_candles = 60;
    int candle_interval = 60;  // seconds
};

//==========================================================================
// SHARED CHART STATE — prepared once per frame, read by all 3 windows
//==========================================================================
struct ChartState {
    // visible candle data
    double xs[CANDLE_MAX + 1], opens[CANDLE_MAX + 1], highs[CANDLE_MAX + 1];
    double lows[CANDLE_MAX + 1], closes[CANDLE_MAX + 1];
    double volumes[CANDLE_MAX + 1], buy_ratios[CANDLE_MAX + 1];
    double times_sec[CANDLE_MAX + 1];
    double sma[CANDLE_MAX + 1];
    int vis_count;
    int sma_first;
    double last_price;
    double vwap;
    double x_lo, x_hi;
    float candle_hw;  // half-width in pixels
    bool ready;
};

static inline void ChartState_Prepare(ChartState *cs, const CandleSnapshot *csnap,
                                       const ChartSettings *settings) {
    int n = csnap->count;
    int vis = settings->visible_candles;
    cs->ready = (n >= 2);
    if (!cs->ready) return;

    int vis_start = 0;
    cs->vis_count = n;
    if (n > vis) {
        vis_start = n - vis;
        cs->vis_count = vis;
    }

    for (int i = 0; i < cs->vis_count; i++) {
        const Candle &c = csnap->candles[vis_start + i];
        cs->xs[i]     = (double)i;
        cs->opens[i]  = c.open;
        cs->highs[i]  = c.high;
        cs->lows[i]   = c.low;
        cs->closes[i] = c.close;
        cs->volumes[i] = c.volume;
        cs->buy_ratios[i] = (c.volume > 0) ? c.buy_vol / c.volume : 0.5;
        cs->times_sec[i] = c.time_sec;
    }

    cs->last_price = csnap->candles[n - 1].close;
    cs->vwap = csnap->vwap;
    cs->x_lo = -0.5;
    cs->x_hi = vis - 0.5;

    // SMA
    cs->sma_first = -1;
    memset(cs->sma, 0, sizeof(cs->sma));
    if (n >= 20) {
        for (int i = 0; i < cs->vis_count; i++) {
            int gi = vis_start + i;
            if (gi < 19) continue;
            double sum = 0;
            for (int j = gi - 19; j <= gi; j++) sum += csnap->candles[j].close;
            cs->sma[i] = sum / 20.0;
            if (cs->sma_first < 0) cs->sma_first = i;
        }
    }
}

//==========================================================================
// PRICE CHART — candlesticks + overlays
//==========================================================================
static inline void GUI_PriceChart(const ChartState *cs, const TUISnapshot *snap,
                                   TradeData *trades, ChartSettings *settings,
                                   CandleAccumulator *candle_acc) {
    ImGui::Begin("Price Chart");
    if (!cs->ready) {
        ImGui::TextColored(FoxmlColors::comment, "waiting for candle data...");
        ImGui::End();
        return;
    }

    int vc = cs->vis_count;
    int vis = settings->visible_candles;

    // title + controls
    ImGui::TextColored(FoxmlColors::primary, "foxml trader");
    ImGui::SameLine();
    ImGui::TextColored(FoxmlColors::wheat, "BTCUSDT  $%.2f", cs->last_price);
    if (cs->vwap > 0) {
        ImGui::SameLine(0, 20);
        ImGui::TextColored(FoxmlColors::comment, "VWAP $%.2f", cs->vwap);
    }

    // candle interval selector
    ImGui::SameLine(0, 20);
    ImGui::SetNextItemWidth(55);
    const char *intervals[] = {"15s", "30s", "1m", "5m"};
    int interval_secs[] = {15, 30, 60, 300};
    int cur_interval = 2;  // default 1m
    for (int i = 0; i < 4; i++)
        if (interval_secs[i] == settings->candle_interval) { cur_interval = i; break; }
    if (ImGui::Combo("##interval", &cur_interval, intervals, 4)) {
        if (interval_secs[cur_interval] != settings->candle_interval) {
            settings->candle_interval = interval_secs[cur_interval];
            if (candle_acc)
                CandleAccumulator_SetInterval(candle_acc, settings->candle_interval);
        }
    }

    // visible candles selector
    ImGui::SameLine(0, 10);
    ImGui::SetNextItemWidth(55);
    const char *windows[] = {"30", "60", "120", "240"};
    int window_vals[] = {30, 60, 120, 240};
    int cur_window = 1;
    for (int i = 0; i < 4; i++)
        if (window_vals[i] == settings->visible_candles) { cur_window = i; break; }
    if (ImGui::Combo("##window", &cur_window, windows, 4)) {
        settings->visible_candles = window_vals[cur_window];
    }

    ImPlot::PushStyleColor(ImPlotCol_PlotBg, FoxmlColors::bg_dark);
    if (ImPlot::BeginPlot("##price", ImVec2(-1, -1),
                           ImPlotFlags_NoTitle | ImPlotFlags_NoMouseText)) {

        ImPlot::SetupAxes(NULL, NULL, ImPlotAxisFlags_NoLabel, ImPlotAxisFlags_Opposite);
        ImPlot::SetupAxisLimits(ImAxis_X1, cs->x_lo, cs->x_hi, ImPlotCond_Always);

        // time tick labels — spaced to prevent overlap
        if (vc > 0) {
            float cw = ImPlot::GetPlotSize().x;
            float label_px = ImGui::CalcTextSize("00:00").x + 20.0f;
            int max_fit = (cw > 0 && label_px > 0) ? (int)(cw / label_px) : 6;
            if (max_fit < 2) max_fit = 2;
            if (max_fit > 12) max_fit = 12;
            int step = vc > max_fit ? vc / max_fit : 1;
            double tick_pos[16];
            const char *tick_labels[16];
            char tick_bufs[16][8];
            int tick_n = 0;
            for (int i = 0; i < vc && tick_n < 16; i += step) {
                if (cs->times_sec[i] < 1.0) continue;
                tick_pos[tick_n] = cs->xs[i];
                time_t t = (time_t)cs->times_sec[i];
                struct tm *tm = localtime(&t);
                snprintf(tick_bufs[tick_n], 8, "%02d:%02d", tm->tm_hour, tm->tm_min);
                tick_labels[tick_n] = tick_bufs[tick_n];
                tick_n++;
            }
            ImPlot::SetupAxisTicks(ImAxis_X1, tick_pos, tick_n, tick_labels);
        }

        // Y limits with padding + TP/SL expansion
        double y_min = 1e18, y_max = -1e18;
        for (int i = 0; i < vc; i++) {
            if (cs->lows[i] < y_min) y_min = cs->lows[i];
            if (cs->highs[i] > y_max) y_max = cs->highs[i];
        }
        double min_range = cs->last_price * 0.001;
        if (min_range < 20.0) min_range = 20.0;
        if ((y_max - y_min) < min_range) {
            double mid = (y_min + y_max) * 0.5;
            y_min = mid - min_range * 0.5;
            y_max = mid + min_range * 0.5;
        }
        for (int pi = 0; pi < 16; pi++) {
            const TUIPositionSnap *ps = &snap->positions[pi];
            if (ps->idx < 0) continue;
            if (ps->tp > 0 && ps->tp > y_max) y_max = ps->tp;
            if (ps->sl > 0 && ps->sl < y_min) y_min = ps->sl;
        }
        double pad = (y_max - y_min) * 0.1;
        ImPlot::SetupAxisLimits(ImAxis_Y1, y_min - pad, y_max + pad, ImPlotCond_Always);
        ImPlot::SetupAxisFormat(ImAxis_Y1, "$%.0f");

        // regime background shading
        ImDrawList *dl = ImPlot::GetPlotDrawList();
        ImPlot::PushPlotClipRect();
        {
            ImVec2 plot_tl = ImPlot::GetPlotPos();
            ImVec2 plot_sz = ImPlot::GetPlotSize();
            ImVec4 regime_col = {0, 0, 0, 0};
            if (snap->current_regime == 1)       // TRENDING — faint green
                regime_col = {FoxmlColors::green.x, FoxmlColors::green.y, FoxmlColors::green.z, 0.04f};
            else if (snap->current_regime == 2)  // VOLATILE — faint red
                regime_col = {FoxmlColors::red.x, FoxmlColors::red.y, FoxmlColors::red.z, 0.06f};
            else if (snap->current_regime == 3)  // TRENDING_DOWN — faint red
                regime_col = {FoxmlColors::red.x, FoxmlColors::red.y, FoxmlColors::red.z, 0.04f};
            if (regime_col.w > 0)
                dl->AddRectFilled(plot_tl, ImVec2(plot_tl.x + plot_sz.x, plot_tl.y + plot_sz.y),
                                  ImGui::GetColorU32(regime_col));
        }

        // current price line (thin dotted across full width)
        {
            ImVec2 price_l = ImPlot::PlotToPixels(cs->x_lo, cs->last_price);
            ImVec2 price_r = ImPlot::PlotToPixels(cs->x_hi, cs->last_price);
            dl->AddLine(price_l, price_r,
                        ImGui::GetColorU32(ImVec4(FoxmlColors::wheat.x, FoxmlColors::wheat.y,
                                                   FoxmlColors::wheat.z, 0.3f)), 1.0f);
        }

        // candlesticks
        float chart_w = ImPlot::GetPlotSize().x;
        float candle_px = (chart_w / vis) * 0.7f;
        if (candle_px < 3.0f) candle_px = 3.0f;
        float hw = candle_px * 0.5f;

        for (int i = 0; i < vc; i++) {
            ImVec2 po = ImPlot::PlotToPixels(cs->xs[i], cs->opens[i]);
            ImVec2 pc = ImPlot::PlotToPixels(cs->xs[i], cs->closes[i]);
            ImVec2 pl = ImPlot::PlotToPixels(cs->xs[i], cs->lows[i]);
            ImVec2 ph = ImPlot::PlotToPixels(cs->xs[i], cs->highs[i]);
            bool bull = cs->closes[i] >= cs->opens[i];
            ImU32 bc = ImGui::GetColorU32(bull ? FoxmlColors::green_b : FoxmlColors::red);
            ImU32 wc = ImGui::GetColorU32(bull ? FoxmlColors::sand : FoxmlColors::comment);
            float cx = po.x;
            dl->AddLine(ImVec2(cx, ph.y), ImVec2(cx, pl.y), wc, 1.0f);
            float top = (po.y < pc.y) ? po.y : pc.y;
            float bot = (po.y < pc.y) ? pc.y : po.y;
            if (bot - top < 1.0f) { top -= 0.5f; bot += 0.5f; }
            dl->AddRectFilled(ImVec2(cx - hw, top), ImVec2(cx + hw, bot), bc);
        }

        // trade markers — only show if price falls within a candle's high-low range
        // prevents old trades from snapping to unrelated candles as ghosts
        for (int mi = 0; mi < trades->marker_count; mi++) {
            const TradeMarker *m = &trades->markers[mi];
            int best_i = -1;
            for (int i = 0; i < vc; i++) {
                // trade price must be within this candle's wick range
                if (m->price >= cs->lows[i] && m->price <= cs->highs[i]) {
                    best_i = i;
                    break;  // take first match (oldest visible candle)
                }
            }
            if (best_i < 0) continue;
            ImVec2 pos = ImPlot::PlotToPixels(cs->xs[best_i], m->price);
            float sz = 6.0f;
            if (m->is_sell) {
                ImU32 col = ImGui::GetColorU32(m->is_tp ? FoxmlColors::green_b : FoxmlColors::red_b);
                dl->AddTriangleFilled(ImVec2(pos.x-sz, pos.y-sz), ImVec2(pos.x+sz, pos.y-sz),
                                      ImVec2(pos.x, pos.y+sz), col);
            } else {
                ImU32 col = ImGui::GetColorU32(FoxmlColors::green_b);
                dl->AddTriangleFilled(ImVec2(pos.x, pos.y-sz), ImVec2(pos.x-sz, pos.y+sz),
                                      ImVec2(pos.x+sz, pos.y+sz), col);
            }
        }
        ImPlot::PopPlotClipRect();

        // VWAP
        if (cs->vwap > 0) {
            double vy[2] = {cs->vwap, cs->vwap}, vx[2] = {cs->x_lo, cs->x_hi};
            ImPlotSpec s; s.LineColor = {FoxmlColors::wheat.x, FoxmlColors::wheat.y,
                                          FoxmlColors::wheat.z, 0.5f}; s.LineWeight = 1.0f;
            ImPlot::PlotLine("VWAP", vx, vy, 2, s);
        }

        // SMA (rolling average — what the old gate used)
        if (cs->sma_first >= 0) {
            ImPlotSpec s; s.LineColor = FoxmlColors::secondary; s.LineWeight = 1.0f;
            ImPlot::PlotLine("SMA", cs->xs + cs->sma_first, cs->sma + cs->sma_first,
                             vc - cs->sma_first, s);
        }

        // EMA (fast gate baseline — what the gate uses now)
        if (snap->ema_price > 0) {
            double ey[2] = {snap->ema_price, snap->ema_price};
            double ex[2] = {cs->x_lo, cs->x_hi};
            ImPlotSpec s;
            s.LineColor = FoxmlColors::cyan;
            s.LineWeight = 1.0f;
            ImPlot::PlotLine("EMA", ex, ey, 2, s);
        }

        // position overlays — grouped partial exit pairs, display-numbered
        // build display index map: bitmap slot → display number (matching positions table)
        int display_idx[16];
        int display_count = 0;
        for (int i = 0; i < 16; i++) {
            if (snap->positions[i].idx >= 0)
                display_idx[i] = display_count++;
            else
                display_idx[i] = -1;
        }

        // collect unique entry prices and group display indices
        double drawn_entries[16] = {};
        int drawn_entry_count = 0;

        // entry lines (grouped by price)
        for (int pi = 0; pi < 16; pi++) {
            const TUIPositionSnap *ps = &snap->positions[pi];
            if (ps->idx < 0 || ps->entry <= 0) continue;

            bool already_drawn = false;
            for (int j = 0; j < drawn_entry_count; j++) {
                if (fabs(drawn_entries[j] - ps->entry) < 0.01) {
                    already_drawn = true;
                    break;
                }
            }

            if (!already_drawn) {
                char group_ids[32] = {};
                int group_len = 0;
                for (int j = 0; j < 16; j++) {
                    if (snap->positions[j].idx < 0) continue;
                    if (fabs(snap->positions[j].entry - ps->entry) < 0.01) {
                        if (group_len > 0) { group_ids[group_len++] = ','; }
                        int dj = display_idx[j];
                        if (dj < 10) group_ids[group_len++] = '0' + dj;
                        else { group_ids[group_len++] = '1'; group_ids[group_len++] = '0' + (dj - 10); }
                    }
                }
                group_ids[group_len] = '\0';

                double y[2] = {ps->entry, ps->entry};
                double lx[2] = {cs->x_lo, cs->x_hi};
                ImPlotSpec s; s.LineColor = FoxmlColors::wheat; s.LineWeight = 2.0f;
                char lbl[16];
                snprintf(lbl, 16, "##e%d", pi);
                ImPlot::PlotLine(lbl, lx, y, 2, s);
                ImPlot::Annotation(cs->x_hi - 1, ps->entry, FoxmlColors::wheat,
                                   ImVec2(5, 0), true, "#%s $%.0f", group_ids, ps->entry);
                drawn_entries[drawn_entry_count++] = ps->entry;
            }
        }

        // TP/SL lines — collision-aware label stagger + per-position dash patterns
        {
            struct PosLabel { double price; float y_px; int di, pi; bool is_tp; };
            PosLabel plabels[32];
            int plabel_n = 0;

            for (int pi = 0; pi < 16; pi++) {
                const TUIPositionSnap *ps = &snap->positions[pi];
                if (ps->idx < 0) continue;
                int di = display_idx[pi];
                if (ps->tp > 0) {
                    float yp = ImPlot::PlotToPixels(0.0, ps->tp).y;
                    plabels[plabel_n++] = {ps->tp, yp, di, pi, true};
                }
                if (ps->sl > 0) {
                    float yp = ImPlot::PlotToPixels(0.0, ps->sl).y;
                    plabels[plabel_n++] = {ps->sl, yp, di, pi, false};
                }
            }

            // sort by screen Y (insertion sort, max 32 elements)
            for (int i = 1; i < plabel_n; i++) {
                PosLabel tmp = plabels[i];
                int j = i - 1;
                while (j >= 0 && plabels[j].y_px > tmp.y_px) {
                    plabels[j + 1] = plabels[j];
                    j--;
                }
                plabels[j + 1] = tmp;
            }

            // detect collision groups (within 20px) and assign horizontal stagger
            float x_offsets[32] = {};
            for (int i = 0; i < plabel_n; ) {
                int ge = i + 1;
                while (ge < plabel_n && (plabels[ge].y_px - plabels[i].y_px) < 20.0f)
                    ge++;
                for (int g = 0; g < ge - i; g++)
                    x_offsets[i + g] = (float)g * 60.0f;
                i = ge;
            }

            // dash patterns indexed by display_idx % 4:
            //   0: normal (8on 6off)  1: short (4on 4off)
            //   2: dot-dash (3on 3off 10on 3off)  3: long (14on 5off)
            static const float dp[][4] = {
                {8,6,8,6}, {4,4,4,4}, {3,3,10,3}, {14,5,14,5}
            };

            for (int li = 0; li < plabel_n; li++) {
                const PosLabel &lb = plabels[li];
                ImVec2 left  = ImPlot::PlotToPixels(cs->x_lo, lb.price);
                ImVec2 right = ImPlot::PlotToPixels(cs->x_hi, lb.price);

                ImVec4 lcol_v, acol_v;
                if (lb.is_tp) {
                    lcol_v = {FoxmlColors::green_b.x, FoxmlColors::green_b.y,
                              FoxmlColors::green_b.z, 0.6f};
                    acol_v = FoxmlColors::green_b;
                } else {
                    lcol_v = {FoxmlColors::red_b.x, FoxmlColors::red_b.y,
                              FoxmlColors::red_b.z, 0.6f};
                    acol_v = FoxmlColors::red;
                }
                ImU32 lcol = ImGui::GetColorU32(lcol_v);

                // dashed line with pattern keyed to display index
                const float *pat = dp[lb.di % 4];
                float total = right.x - left.x;
                int si = 0;
                for (float x = 0; x < total; ) {
                    float seg = pat[si % 4];
                    if ((si & 1) == 0) {
                        float x1 = left.x + x;
                        float x2 = left.x + x + seg;
                        if (x2 > right.x) x2 = right.x;
                        dl->AddLine(ImVec2(x1, left.y), ImVec2(x2, left.y), lcol, 1.0f);
                    }
                    x += seg;
                    si++;
                }

                // annotation with horizontal stagger when labels collide
                ImPlot::Annotation(cs->x_hi - 1, lb.price, acol_v,
                                   ImVec2(5 + x_offsets[li], 0), true,
                                   "#%d %s", lb.di, lb.is_tp ? "TP" : "SL");
            }
        }

        // buy gate threshold — cyan, thick dotted, distinct from entry/TP/SL
        if (snap->buy_p > 0.01) {
            ImVec2 left  = ImPlot::PlotToPixels(cs->x_lo, snap->buy_p);
            ImVec2 right = ImPlot::PlotToPixels(cs->x_hi, snap->buy_p);
            ImU32 gate_col = ImGui::GetColorU32(ImVec4(
                FoxmlColors::cyan.x, FoxmlColors::cyan.y, FoxmlColors::cyan.z, 0.7f));
            // dot-dash pattern (3px dot, 4px gap, 10px dash, 4px gap)
            float pattern[] = {3, 4, 10, 4};
            int pi_pat = 0;
            float total = right.x - left.x;
            bool drawing = true;
            for (float x = 0; x < total; ) {
                float seg = pattern[pi_pat % 4];
                if (drawing) {
                    float x1 = left.x + x;
                    float x2 = left.x + x + seg;
                    if (x2 > right.x) x2 = right.x;
                    dl->AddLine(ImVec2(x1, left.y), ImVec2(x2, left.y), gate_col, 1.5f);
                }
                x += seg;
                drawing = !drawing;
                pi_pat++;
            }
            ImPlot::Annotation(cs->x_lo + 1, snap->buy_p,
                               {FoxmlColors::cyan.x, FoxmlColors::cyan.y, FoxmlColors::cyan.z, 1.0f},
                               ImVec2(5, 0), true, "GATE $%.0f", snap->buy_p);
        }

        // hover tooltip
        if (ImPlot::IsPlotHovered()) {
            ImPlotPoint mouse = ImPlot::GetPlotMousePos();
            int idx = (int)(mouse.x + 0.5);
            if (idx >= 0 && idx < vc) {
                ImGui::BeginTooltip();
                ImGui::TextColored(FoxmlColors::wheat,
                    "O: $%.2f  H: $%.2f  L: $%.2f  C: $%.2f",
                    cs->opens[idx], cs->highs[idx], cs->lows[idx], cs->closes[idx]);
                ImGui::TextColored(FoxmlColors::comment, "Vol: %.4f", cs->volumes[idx]);
                ImGui::EndTooltip();
            }
        }

        ImPlot::EndPlot();
    }
    ImPlot::PopStyleColor();
    ImGui::End();
}

//==========================================================================
// VOLUME CHART — separate dockable window
//==========================================================================
static inline void GUI_VolumeChart(const ChartState *cs, const TUISnapshot *snap,
                                    const ChartSettings *settings) {
    ImGui::Begin("Volume");
    if (!cs->ready) {
        ImGui::TextColored(FoxmlColors::comment, "waiting for data...");
        ImGui::End();
        return;
    }

    int vc = cs->vis_count;
    int vis = settings->visible_candles;
    ImPlot::PushStyleColor(ImPlotCol_PlotBg, FoxmlColors::bg_dark);
    if (ImPlot::BeginPlot("##vol", ImVec2(-1, -1),
                           ImPlotFlags_NoTitle | ImPlotFlags_NoMouseText)) {
        ImPlot::SetupAxes(NULL, "Vol",
                          ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickLabels,
                          ImPlotAxisFlags_Opposite);
        ImPlot::SetupAxisScale(ImAxis_Y1, ImPlotScale_Log10);
        ImPlot::SetupAxisLimits(ImAxis_X1, cs->x_lo, cs->x_hi, ImPlotCond_Always);
        double vol_max = 0, vol_min = 1e18;
        for (int i = 0; i < vc; i++) {
            if (cs->volumes[i] > vol_max) vol_max = cs->volumes[i];
            if (cs->volumes[i] > 0.0001 && cs->volumes[i] < vol_min) vol_min = cs->volumes[i];
        }
        if (vol_max < 0.001) vol_max = 1.0;
        if (vol_min > vol_max) vol_min = vol_max * 0.01;
        ImPlot::SetupAxisLimits(ImAxis_Y1, vol_min * 0.5, vol_max * 2.0, ImPlotCond_Always);

        ImDrawList *dl = ImPlot::GetPlotDrawList();
        ImPlot::PushPlotClipRect();
        float chart_w = ImPlot::GetPlotSize().x;
        float bw = (chart_w / vis) * 0.35f;
        if (bw < 1.5f) bw = 1.5f;
        for (int i = 0; i < vc; i++) {
            ImVec4 col = (cs->buy_ratios[i] > 0.5) ? FoxmlColors::green : FoxmlColors::red_b;
            col.w = 0.7f;
            ImVec2 top = ImPlot::PlotToPixels(cs->xs[i], cs->volumes[i]);
            ImVec2 bot = ImPlot::PlotToPixels(cs->xs[i], 0.0);
            dl->AddRectFilled(ImVec2(top.x - bw, top.y), ImVec2(top.x + bw, bot.y),
                              ImGui::GetColorU32(col));
        }
        // volume gate threshold — cyan dot-dash, matching price chart gate style
        if (snap->buy_v > 0.0001) {
            ImVec2 left  = ImPlot::PlotToPixels(cs->x_lo, snap->buy_v);
            ImVec2 right = ImPlot::PlotToPixels(cs->x_hi, snap->buy_v);
            ImU32 gate_col = ImGui::GetColorU32(ImVec4(
                FoxmlColors::cyan.x, FoxmlColors::cyan.y, FoxmlColors::cyan.z, 0.7f));
            float pattern[] = {3, 4, 10, 4};
            int pi_pat = 0;
            bool drawing = true;
            for (float x = 0; x < right.x - left.x; ) {
                float seg = pattern[pi_pat % 4];
                if (drawing) {
                    float x1 = left.x + x;
                    float x2 = left.x + x + seg;
                    if (x2 > right.x) x2 = right.x;
                    dl->AddLine(ImVec2(x1, left.y), ImVec2(x2, left.y), gate_col, 1.5f);
                }
                x += seg;
                drawing = !drawing;
                pi_pat++;
            }
            ImPlot::Annotation(cs->x_lo + 1, snap->buy_v,
                               {FoxmlColors::cyan.x, FoxmlColors::cyan.y, FoxmlColors::cyan.z, 1.0f},
                               ImVec2(5, 0), true, "VOL GATE");
        }

        ImPlot::PopPlotClipRect();
        ImPlot::EndPlot();
    }
    ImPlot::PopStyleColor();
    ImGui::End();
}

//==========================================================================
// EQUITY CURVE — separate dockable window (only renders with trade data)
//==========================================================================
static inline void GUI_EquityChart(TradeData *trades) {
    ImGui::Begin("Equity Curve");
    if (trades->equity_count < 1) {
        ImGui::TextColored(FoxmlColors::comment, "waiting for trades...");
        ImGui::End();
        return;
    }

    int en = trades->equity_count;
    double eq_xs[MAX_TRADES], eq_ys[MAX_TRADES], zeros[MAX_TRADES] = {};
    for (int i = 0; i < en; i++) {
        eq_xs[i] = (double)i;
        eq_ys[i] = trades->equity[i].cumulative_pnl;
    }

    ImPlot::PushStyleColor(ImPlotCol_PlotBg, FoxmlColors::bg_dark);
    if (ImPlot::BeginPlot("##equity", ImVec2(-1, -1),
                           ImPlotFlags_NoTitle | ImPlotFlags_NoMouseText)) {
        ImPlot::SetupAxes(NULL, "P&L",
                          ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickLabels,
                          ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_Opposite);
        ImPlot::SetupAxisLimits(ImAxis_X1, -0.5, en - 0.5, ImPlotCond_Always);
        ImPlot::SetupAxisFormat(ImAxis_Y1, "$%+.2f");

        // zero line
        double zy[2] = {0, 0}, zx[2] = {-0.5, (double)(en - 0.5)};
        ImPlotSpec zs; zs.LineColor = FoxmlColors::surface; zs.LineWeight = 1.0f;
        ImPlot::PlotLine("##zero", zx, zy, 2, zs);

        // equity line
        ImPlotSpec es; es.LineColor = FoxmlColors::primary; es.LineWeight = 1.5f;
        ImPlot::PlotLine("P&L", eq_xs, eq_ys, en, es);

        // green fill
        ImPlotSpec gs; gs.FillColor = FoxmlColors::green; gs.FillAlpha = 0.15f;
        gs.LineColor = {0,0,0,0};
        ImPlot::PlotShaded("##fill", eq_xs, eq_ys, zeros, en, gs);

        // trade markers
        ImDrawList *dl = ImPlot::GetPlotDrawList();
        ImPlot::PushPlotClipRect();
        int si = 0;
        for (int mi = 0; mi < trades->marker_count && si < en; mi++) {
            if (!trades->markers[mi].is_sell) continue;
            ImVec2 pos = ImPlot::PlotToPixels(eq_xs[si], eq_ys[si]);
            ImU32 col = ImGui::GetColorU32(
                trades->markers[mi].is_tp ? FoxmlColors::green_b : FoxmlColors::red_b);
            dl->AddCircleFilled(pos, 4.0f, col);
            si++;
        }
        ImPlot::PopPlotClipRect();

        ImPlot::EndPlot();
    }
    ImPlot::PopStyleColor();
    ImGui::End();
}
