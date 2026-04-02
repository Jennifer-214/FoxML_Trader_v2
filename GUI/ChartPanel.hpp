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
#include "../Strategies/StrategyInterface.hpp"

// chart display settings (mutable — controlled by GUI dropdowns)
struct ChartSettings {
    int visible_candles = 60;
    int candle_interval = 60;  // seconds
    // overlay toggles
    bool show_ribbon = true;
    bool show_price_tag = true;
    bool show_session_hl = true;
    bool show_session_div = true;
    bool show_spread = true;
    bool show_crosshair = true;
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
struct DragState {
    int active;       // currently dragging
    int slot;         // position bitmap slot
    int is_tp;        // 1=TP, 0=SL
    double price;     // current drag price
};

static inline void GUI_PriceChart(const ChartState *cs, const TUISnapshot *snap,
                                   TradeData *trades, ChartSettings *settings,
                                   CandleAccumulator *candle_acc,
                                   void *shared_state_ptr = NULL) {
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
    // EMA-SMA spread readout
    if (settings->show_spread && snap->ema_price > 0 && snap->roll_stddev > 0.01) {
        double avg = snap->roll_price_avg;
        double spread_sigma = (avg > 0) ? (snap->ema_price - avg) / snap->roll_stddev : 0;
        ImGui::SameLine(0, 20);
        ImVec4 spread_col = (spread_sigma > 0) ? FoxmlColors::green_b : FoxmlColors::red;
        ImGui::TextColored(spread_col, "spread: %+.2f\xcf\x83", spread_sigma);
    }

    // candle interval selector
    ImGui::SameLine(0, 20);
    ImGui::TextColored(FoxmlColors::comment, "interval");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(65);
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
    ImGui::SameLine(0, 15);
    ImGui::SameLine(0, 15);
    ImGui::SetNextItemWidth(90);
    const char *windows[] = {"30 bars", "60 bars", "120 bars", "240 bars"};
    int window_vals[] = {30, 60, 120, 240};
    int cur_window = 1;
    for (int i = 0; i < 4; i++)
        if (window_vals[i] == settings->visible_candles) { cur_window = i; break; }
    if (ImGui::Combo("##window", &cur_window, windows, 4)) {
        settings->visible_candles = window_vals[cur_window];
    }

    // overlay toggles
    ImGui::SameLine(0, 20);
    ImGui::Checkbox("Ribbon", &settings->show_ribbon);
    ImGui::SameLine(); ImGui::Checkbox("Sessions", &settings->show_session_div);
    ImGui::SameLine(); ImGui::Checkbox("H/L", &settings->show_session_hl);
    ImGui::SameLine(); ImGui::Checkbox("Tag", &settings->show_price_tag);

    ImPlot::PushStyleColor(ImPlotCol_PlotBg, FoxmlColors::bg_dark);
    // subtle Y grid lines for price readability
    ImPlot::PushStyleColor(ImPlotCol_AxisGrid, ImVec4(1, 1, 1, 0.06f));
    if (ImPlot::BeginPlot("##price", ImVec2(-1, -1),
                           ImPlotFlags_NoTitle | ImPlotFlags_NoMouseText)) {

        ImPlot::SetupAxes(NULL, NULL,
                          ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoGridLines,
                          ImPlotAxisFlags_Opposite);
        ImPlot::SetupAxisLimits(ImAxis_X1, cs->x_lo, cs->x_hi, ImPlotCond_Always);

        // time tick labels — static buffers so pointers survive until EndPlot
        static double tick_pos[16];
        static char tick_bufs[16][8];
        static const char *tick_labels_p[16];
        int tick_n = 0;
        if (vc > 0) {
            int step = vc > 5 ? vc / 5 : 1;
            for (int i = 0; i < vc && tick_n < 16; i += step) {
                if (cs->times_sec[i] < 1.0) continue;
                tick_pos[tick_n] = cs->xs[i];
                time_t t = (time_t)cs->times_sec[i];
                struct tm *tm = localtime(&t);
                snprintf(tick_bufs[tick_n], 8, "%02d:%02d", tm->tm_hour, tm->tm_min);
                tick_labels_p[tick_n] = tick_bufs[tick_n];
                tick_n++;
            }
            ImPlot::SetupAxisTicks(ImAxis_X1, tick_pos, tick_n, tick_labels_p);
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
            if (snap->current_regime == REGIME_TRENDING)
                regime_col = {FoxmlColors::green.x, FoxmlColors::green.y, FoxmlColors::green.z, 0.04f};
            else if (snap->current_regime == REGIME_MILD_TREND)
                regime_col = {FoxmlColors::sand.x, FoxmlColors::sand.y, FoxmlColors::sand.z, 0.04f};
            else if (snap->current_regime == REGIME_VOLATILE)
                regime_col = {FoxmlColors::red.x, FoxmlColors::red.y, FoxmlColors::red.z, 0.06f};
            else if (snap->current_regime == REGIME_TRENDING_DOWN)
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

        // entry markers — drawn from live position data (not CSV), keyed by entry_time
        // only active positions get markers, they disappear on close — no persistence/drift
        for (int pi = 0; pi < 16; pi++) {
            const TUIPositionSnap *ps = &snap->positions[pi];
            if (ps->idx < 0 || ps->entry_time == 0) continue;
            double et = (double)ps->entry_time;
            // find candle containing this entry time
            int best_i = -1;
            for (int i = vc - 1; i >= 0; i--) {
                if (cs->times_sec[i] <= et && (i == vc - 1 || cs->times_sec[i + 1] > et)) {
                    best_i = i;
                    break;
                }
            }
            if (best_i < 0) continue;
            ImVec2 pos = ImPlot::PlotToPixels(cs->xs[best_i], ps->entry);
            ImU32 col = ImGui::GetColorU32(ImVec4(FoxmlColors::green_b.x, FoxmlColors::green_b.y,
                                                    FoxmlColors::green_b.z, 0.8f));
            dl->AddCircleFilled(pos, 3.5f, col);
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

        // === EMA/SMA shaded ribbon ===
        if (settings->show_ribbon && snap->ema_price > 0 && cs->sma_first >= 0) {
            double ema_y[CANDLE_MAX + 1];
            for (int i = 0; i < vc; i++) ema_y[i] = snap->ema_price;
            int sf = cs->sma_first;
            int n = vc - sf;
            if (n > 1) {
                // determine dominant color from current state
                bool ema_above = (snap->ema_price > cs->sma[vc - 1]);
                ImVec4 fill_col = ema_above
                    ? ImVec4(FoxmlColors::green.x, FoxmlColors::green.y, FoxmlColors::green.z, 0.20f)
                    : ImVec4(FoxmlColors::red.x, FoxmlColors::red.y, FoxmlColors::red.z, 0.20f);
                ImPlotSpec rs;
                rs.FillColor = fill_col;
                rs.FillAlpha = fill_col.w;
                rs.LineColor = {0, 0, 0, 0};
                ImPlot::PlotShaded("##ema_sma_fill", cs->xs + sf, ema_y + sf,
                                    cs->sma + sf, n, rs);
            }
        }

        // === live price tag on Y-axis ===
        if (settings->show_price_tag) {
            ImVec2 price_px = ImPlot::PlotToPixels(cs->x_hi, cs->last_price);
            ImVec2 plot_r = ImPlot::PlotToPixels(cs->x_hi, 0);
            char ptag[16];
            snprintf(ptag, 16, "$%.0f", cs->last_price);
            ImVec2 tsz = ImGui::CalcTextSize(ptag);
            float pr = plot_r.x - 2;
            float pl = pr - tsz.x - 8;
            bool bull = (vc >= 2 && cs->closes[vc-1] >= cs->closes[vc-2]);
            ImVec4 tag_col = bull ? FoxmlColors::green_b : FoxmlColors::red;
            dl->AddRectFilled(ImVec2(pl, price_px.y - tsz.y * 0.5f - 2),
                              ImVec2(pr, price_px.y + tsz.y * 0.5f + 2),
                              ImGui::GetColorU32(tag_col), 2.0f);
            dl->AddText(ImVec2(pl + 4, price_px.y - tsz.y * 0.5f),
                        IM_COL32(255, 255, 255, 240), ptag);
        }

        // === session high/low markers ===
        if (settings->show_session_hl && snap->session_high > 0 && snap->session_low > 0) {
            ImU32 sess_col = ImGui::GetColorU32(ImVec4(
                FoxmlColors::comment.x, FoxmlColors::comment.y,
                FoxmlColors::comment.z, 0.25f));
            // session high
            ImVec2 sh_l = ImPlot::PlotToPixels(cs->x_lo, snap->session_high);
            ImVec2 sh_r = ImPlot::PlotToPixels(cs->x_hi, snap->session_high);
            for (float x = sh_l.x; x < sh_r.x; x += 8.0f) {
                float x2 = x + 3.0f; if (x2 > sh_r.x) x2 = sh_r.x;
                dl->AddLine(ImVec2(x, sh_l.y), ImVec2(x2, sh_l.y), sess_col, 1.0f);
            }
            // session low
            ImVec2 sl_l = ImPlot::PlotToPixels(cs->x_lo, snap->session_low);
            ImVec2 sl_r = ImPlot::PlotToPixels(cs->x_hi, snap->session_low);
            for (float x = sl_l.x; x < sl_r.x; x += 8.0f) {
                float x2 = x + 3.0f; if (x2 > sl_r.x) x2 = sl_r.x;
                dl->AddLine(ImVec2(x, sl_l.y), ImVec2(x2, sl_l.y), sess_col, 1.0f);
            }
        }

        // === session dividers (vertical lines at UTC hour boundaries) ===
        if (settings->show_session_div && vc > 1) {
            // session hours (UTC): Asian 0-8, EU 8-13, US 13-21, Overnight 21-24
            static const int boundaries[] = {0, 8, 13, 21};
            static const char *sess_labels[] = {"ASIA", "EU", "US", "OVER"};
            ImU32 div_col = ImGui::GetColorU32(ImVec4(1, 1, 1, 0.06f));
            ImVec2 plot_tl = ImPlot::GetPlotPos();
            for (int i = 0; i < vc - 1; i++) {
                if (cs->times_sec[i] < 1.0 || cs->times_sec[i+1] < 1.0) continue;
                time_t t0 = (time_t)cs->times_sec[i];
                time_t t1 = (time_t)cs->times_sec[i+1];
                struct tm *tm0 = gmtime(&t0);
                int h0 = tm0->tm_hour;
                struct tm *tm1 = gmtime(&t1);
                int h1 = tm1->tm_hour;
                // check if any session boundary was crossed
                for (int b = 0; b < 4; b++) {
                    int bh = boundaries[b];
                    if ((h0 < bh && h1 >= bh) || (h0 > h1 && bh <= h1)) {
                        ImVec2 top = ImPlot::PlotToPixels(cs->xs[i+1], 0);
                        ImVec2 bot_px = ImPlot::PlotToPixels(cs->xs[i+1], 0);
                        float px_x = top.x;
                        dl->AddLine(ImVec2(px_x, plot_tl.y),
                                    ImVec2(px_x, plot_tl.y + ImPlot::GetPlotSize().y),
                                    div_col, 1.0f);
                        // label at top
                        dl->AddText(ImVec2(px_x + 3, plot_tl.y + 2),
                                    ImGui::GetColorU32(ImVec4(1, 1, 1, 0.15f)),
                                    sess_labels[b]);
                    }
                }
            }
        }

        // position overlays — unified collision-aware label system
        // all labels (entry, TP, SL) collected first, then sorted and staggered
        int display_idx[16];
        int display_count = 0;
        for (int i = 0; i < 16; i++) {
            if (snap->positions[i].idx >= 0)
                display_idx[i] = display_count++;
            else
                display_idx[i] = -1;
        }

        struct ChartLabel { double price; float y_px; ImVec4 color; int di; char text[32]; };
        ChartLabel clabels[48];
        int clabel_n = 0;

        // dash patterns: 0=normal 1=short 2=dot-dash 3=long
        static const float dp[][4] = {
            {8,6,8,6}, {4,4,4,4}, {3,3,10,3}, {14,5,14,5}
        };

        // per-position accent colors (icon + line tint)
        static const ImVec4 pos_accent[] = {
            {0.40f, 0.85f, 0.85f, 1.0f},  // cyan
            {0.95f, 0.75f, 0.35f, 1.0f},  // gold
            {0.70f, 0.50f, 0.90f, 1.0f},  // purple
            {0.90f, 0.55f, 0.65f, 1.0f},  // rose
            {0.50f, 0.85f, 0.50f, 1.0f},  // lime
            {0.85f, 0.60f, 0.40f, 1.0f},  // coral
            {0.55f, 0.75f, 0.95f, 1.0f},  // sky
            {0.90f, 0.80f, 0.50f, 1.0f},  // butter
        };
        ImVec2 plot_right = ImPlot::PlotToPixels(cs->x_hi, 0);
        float right_edge = plot_right.x - 4;

        // newest position index for fade effect
        int newest_di = display_count - 1;

        // draw entry lines + collect entry labels
        double drawn_entries[16] = {};
        int drawn_entry_count = 0;
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
            if (already_drawn) continue;

            // find highest di in this entry group for fade
            int max_di = 0;
            char group_ids[32] = {};
            int group_len = 0;
            for (int j = 0; j < 16; j++) {
                if (snap->positions[j].idx < 0) continue;
                if (fabs(snap->positions[j].entry - ps->entry) < 0.01) {
                    if (group_len > 0) { group_ids[group_len++] = ','; }
                    int dj = display_idx[j];
                    if (dj < 10) group_ids[group_len++] = '0' + dj;
                    else { group_ids[group_len++] = '1'; group_ids[group_len++] = '0' + (dj - 10); }
                    if (dj > max_di) max_di = dj;
                }
            }
            group_ids[group_len] = '\0';

            // fade older positions (newest=full, oldest=40%)
            float age_alpha = (display_count <= 1) ? 1.0f :
                0.4f + 0.6f * ((float)max_di / newest_di);

            // entry line — full width, solid
            double y[2] = {ps->entry, ps->entry};
            double lx[2] = {cs->x_lo, cs->x_hi};
            ImPlotSpec s;
            s.LineColor = {FoxmlColors::wheat.x, FoxmlColors::wheat.y,
                           FoxmlColors::wheat.z, age_alpha};
            s.LineWeight = 1.5f;
            char lbl[16]; snprintf(lbl, 16, "##e%d", pi);
            ImPlot::PlotLine(lbl, lx, y, 2, s);

            ChartLabel &cl = clabels[clabel_n++];
            cl.price = ps->entry;
            cl.y_px = ImPlot::PlotToPixels(0.0, ps->entry).y;
            cl.color = {FoxmlColors::wheat.x, FoxmlColors::wheat.y,
                        FoxmlColors::wheat.z, age_alpha};
            cl.di = max_di;  // use highest di in group for icon
            snprintf(cl.text, 32, "#%s $%.0f", group_ids, ps->entry);
            drawn_entries[drawn_entry_count++] = ps->entry;
        }

        // draw TP/SL dashed lines with per-position accent colors
        for (int pi = 0; pi < 16; pi++) {
            const TUIPositionSnap *ps = &snap->positions[pi];
            if (ps->idx < 0) continue;
            int di = display_idx[pi];
            float age_alpha = (display_count <= 1) ? 1.0f :
                0.4f + 0.6f * ((float)di / newest_di);
            const ImVec4 &accent = pos_accent[di % 8];

            for (int tp_pass = 0; tp_pass < 2; tp_pass++) {
                double price = tp_pass == 0 ? ps->tp : ps->sl;
                if (price <= 0) continue;
                bool is_tp = (tp_pass == 0);

                ImVec2 left  = ImPlot::PlotToPixels(cs->x_lo, price);
                ImVec2 right = ImPlot::PlotToPixels(cs->x_hi, price);
                // blend accent with green/red so lines stay distinguishable
                ImVec4 base = is_tp
                    ? ImVec4(accent.x * 0.5f + 0.2f, accent.y * 0.3f + 0.5f,
                             accent.z * 0.3f + 0.2f, 0.6f * age_alpha)
                    : ImVec4(accent.x * 0.3f + 0.55f, accent.y * 0.3f + 0.1f,
                             accent.z * 0.3f + 0.1f, 0.6f * age_alpha);
                ImU32 lcol = ImGui::GetColorU32(base);

                const float *pat = dp[di % 4];
                float total = right.x - left.x;
                int si = 0;
                for (float x = 0; x < total; ) {
                    float seg = pat[si % 4];
                    if ((si & 1) == 0) {
                        float x1 = left.x + x, x2 = left.x + x + seg;
                        if (x2 > right.x) x2 = right.x;
                        dl->AddLine(ImVec2(x1, left.y), ImVec2(x2, left.y), lcol, 1.0f);
                    }
                    x += seg; si++;
                }

                ChartLabel &cl = clabels[clabel_n++];
                cl.price = price;
                cl.y_px = left.y;
                cl.di = di;
                cl.color = is_tp
                    ? ImVec4(FoxmlColors::green_b.x, FoxmlColors::green_b.y,
                             FoxmlColors::green_b.z, age_alpha)
                    : ImVec4(FoxmlColors::red.x, FoxmlColors::red.y,
                             FoxmlColors::red.z, age_alpha);
                // show P&L at this exit level
                double pnl = price - ps->entry;
                snprintf(cl.text, 32, "#%d %s %+.0f", di, is_tp ? "TP" : "SL", pnl);
            }
        }

        // draggable TP/SL lines
        static DragState drag = {0, -1, 0, 0};
        if (ImPlot::IsPlotHovered() || drag.active) {
            ImPlotPoint mouse = ImPlot::GetPlotMousePos();
            ImVec2 mouse_px = ImGui::GetMousePos();
            bool mouse_down = ImGui::IsMouseDown(0);

            if (!drag.active) {
                // hover detection: find nearest TP/SL line within 5px
                float best_dist = 6.0f;
                int best_slot = -1, best_tp = 0;
                for (int pi = 0; pi < 16; pi++) {
                    const TUIPositionSnap *ps = &snap->positions[pi];
                    if (ps->idx < 0) continue;
                    for (int t = 0; t < 2; t++) {
                        double p = t == 0 ? ps->tp : ps->sl;
                        if (p <= 0) continue;
                        float py = ImPlot::PlotToPixels(0, p).y;
                        float d = fabs(mouse_px.y - py);
                        if (d < best_dist) {
                            best_dist = d; best_slot = pi; best_tp = (t == 0);
                        }
                    }
                }
                if (best_slot >= 0) {
                    // highlight the hovered line
                    double hp = best_tp ? snap->positions[best_slot].tp
                                        : snap->positions[best_slot].sl;
                    ImVec2 hl = ImPlot::PlotToPixels(cs->x_lo, hp);
                    ImVec2 hr = ImPlot::PlotToPixels(cs->x_hi, hp);
                    dl->AddLine(hl, hr, IM_COL32(255,255,255,80), 2.0f);
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);

                    if (mouse_down) {
                        drag.active = 1; drag.slot = best_slot;
                        drag.is_tp = best_tp; drag.price = hp;
                    }
                }
            } else {
                // dragging — update price from mouse Y
                drag.price = mouse.y;
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);

                // draw drag preview line
                ImVec2 dl_l = ImPlot::PlotToPixels(cs->x_lo, drag.price);
                ImVec2 dl_r = ImPlot::PlotToPixels(cs->x_hi, drag.price);
                ImU32 dcol = drag.is_tp ? IM_COL32(100, 220, 100, 180)
                                        : IM_COL32(220, 100, 100, 180);
                dl->AddLine(dl_l, dl_r, dcol, 2.0f);

                // draw live P&L preview tag
                double entry = snap->positions[drag.slot].entry;
                double pnl = drag.price - entry;
                char drag_buf[32];
                snprintf(drag_buf, 32, "%s %+.0f ($%.0f)",
                         drag.is_tp ? "TP" : "SL", pnl, drag.price);
                ImVec2 dsz = ImGui::CalcTextSize(drag_buf);
                float dx = dl_r.x - dsz.x - 14;
                float dy = dl_l.y - dsz.y - 8;
                dl->AddRectFilled(ImVec2(dx, dy), ImVec2(dx + dsz.x + 10, dy + dsz.y + 6),
                                  ImGui::GetColorU32(ImVec4(0.15f, 0.15f, 0.15f, 0.9f)), 3.0f);
                dl->AddText(ImVec2(dx + 5, dy + 3), IM_COL32(255, 255, 255, 240), drag_buf);

                // update the label in clabels so stagger reflects drag position
                for (int li = 0; li < clabel_n; li++) {
                    if (fabs(clabels[li].price - (drag.is_tp ? snap->positions[drag.slot].tp
                                                             : snap->positions[drag.slot].sl)) < 0.01) {
                        clabels[li].price = drag.price;
                        clabels[li].y_px = dl_l.y;
                        snprintf(clabels[li].text, 32, "#%d %s %+.0f",
                                 display_idx[drag.slot], drag.is_tp ? "TP" : "SL", pnl);
                    }
                }

                if (!mouse_down) {
                    // release — send to engine
                    if (shared_state_ptr) {
                        TUISharedState *ss = (TUISharedState *)shared_state_ptr;
                        ss->drag_price = drag.price;
                        ss->drag_is_tp = drag.is_tp;
                        __atomic_store_n(&ss->drag_slot, drag.slot, __ATOMIC_RELEASE);
                    }
                    drag.active = 0; drag.slot = -1;
                }
            }
        }

        // sort labels by screen Y, detect collisions, stagger right-to-left
        for (int i = 1; i < clabel_n; i++) {
            ChartLabel tmp = clabels[i];
            int j = i - 1;
            while (j >= 0 && clabels[j].y_px > tmp.y_px) {
                clabels[j + 1] = clabels[j]; j--;
            }
            clabels[j + 1] = tmp;
        }
        float lbl_offsets[48] = {};
        float lbl_widths[48] = {};
        float icon_total = 4.0f * 2 + 4;  // icon_sz*2 + gap (matches icon_w below)
        for (int i = 0; i < clabel_n; i++)
            lbl_widths[i] = ImGui::CalcTextSize(clabels[i].text).x + 10.0f + icon_total;
        for (int i = 0; i < clabel_n; ) {
            int ge = i + 1;
            while (ge < clabel_n && (clabels[ge].y_px - clabels[ge - 1].y_px) < 26.0f)
                ge++;
            float running = 0;
            for (int g = i; g < ge; g++) {
                lbl_offsets[g] = running;
                running += lbl_widths[g] + 3.0f;
            }
            i = ge;
        }
        // draw labels with per-position shape icons, extending leftward
        float lpad = 3.0f;
        float icon_sz = 4.0f;
        float icon_w = icon_sz * 2 + 4;  // icon width + gap
        for (int i = 0; i < clabel_n; i++) {
            ImVec2 anchor = ImPlot::PlotToPixels(0, clabels[i].price);
            float box_r = right_edge - lbl_offsets[i];
            float box_l = box_r - lbl_widths[i];
            ImVec2 tsz = ImGui::CalcTextSize(clabels[i].text);
            float cy = anchor.y;
            ImVec2 tl(box_l, cy - tsz.y * 0.5f - lpad);
            ImVec2 br(box_r, cy + tsz.y * 0.5f + lpad);
            ImVec4 &c = clabels[i].color;
            dl->AddRectFilled(tl, br, ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, 0.85f)), 3.0f);

            // shape icon keyed to position index
            int di = clabels[i].di;
            const ImVec4 &ac = pos_accent[di % 8];
            ImU32 ic = ImGui::GetColorU32(ac);
            float ix = box_l + icon_sz + 2;
            switch (di % 4) {
                case 0: dl->AddCircleFilled(ImVec2(ix, cy), icon_sz, ic); break;
                case 1: dl->AddRectFilled(ImVec2(ix-icon_sz, cy-icon_sz),
                                           ImVec2(ix+icon_sz, cy+icon_sz), ic); break;
                case 2: dl->AddTriangleFilled(ImVec2(ix, cy-icon_sz),
                                               ImVec2(ix-icon_sz, cy+icon_sz),
                                               ImVec2(ix+icon_sz, cy+icon_sz), ic); break;
                case 3: dl->AddTriangleFilled(ImVec2(ix, cy-icon_sz),  // diamond
                                               ImVec2(ix-icon_sz, cy),
                                               ImVec2(ix, cy+icon_sz), ic);
                         dl->AddTriangleFilled(ImVec2(ix, cy-icon_sz),
                                               ImVec2(ix+icon_sz, cy),
                                               ImVec2(ix, cy+icon_sz), ic); break;
            }

            dl->AddText(ImVec2(box_l + icon_w, tl.y + lpad), IM_COL32(255,255,255,230), clabels[i].text);
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

        // hover crosshair + tooltip
        if (settings->show_crosshair && ImPlot::IsPlotHovered()) {
            ImPlotPoint mouse = ImPlot::GetPlotMousePos();

            // horizontal crosshair line
            ImVec2 ch_l = ImPlot::PlotToPixels(cs->x_lo, mouse.y);
            ImVec2 ch_r = ImPlot::PlotToPixels(cs->x_hi, mouse.y);
            ImU32 ch_col = ImGui::GetColorU32(ImVec4(
                FoxmlColors::comment.x, FoxmlColors::comment.y,
                FoxmlColors::comment.z, 0.35f));
            // dotted line (2px on, 4px off)
            for (float x = ch_l.x; x < ch_r.x; x += 6.0f) {
                float x2 = x + 2.0f;
                if (x2 > ch_r.x) x2 = ch_r.x;
                dl->AddLine(ImVec2(x, ch_l.y), ImVec2(x2, ch_l.y), ch_col, 1.0f);
            }

            // price tag at right edge
            char price_buf[16];
            snprintf(price_buf, 16, "$%.0f", mouse.y);
            ImVec2 psz = ImGui::CalcTextSize(price_buf);
            float pr = right_edge;
            float pl = pr - psz.x - 8.0f;
            ImVec2 ptl(pl, ch_l.y - psz.y * 0.5f - 2);
            ImVec2 pbr(pr, ch_l.y + psz.y * 0.5f + 2);
            dl->AddRectFilled(ptl, pbr, ImGui::GetColorU32(FoxmlColors::surface), 2.0f);
            dl->AddText(ImVec2(pl + 4, ptl.y + 2), ImGui::GetColorU32(FoxmlColors::wheat), price_buf);

            // OHLCV tooltip
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
    ImPlot::PopStyleColor(2);  // PlotBg + AxisGrid
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
    ImPlot::PushStyleColor(ImPlotCol_AxisGrid, ImVec4(1, 1, 1, 0.06f));
    if (ImPlot::BeginPlot("##vol", ImVec2(-1, -1),
                           ImPlotFlags_NoTitle | ImPlotFlags_NoMouseText)) {
        ImPlot::SetupAxes(NULL, "Vol",
                          ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickLabels | ImPlotAxisFlags_NoGridLines,
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

        // hover crosshair + volume readout
        if (ImPlot::IsPlotHovered()) {
            ImPlotPoint mouse = ImPlot::GetPlotMousePos();
            ImVec2 ch_l = ImPlot::PlotToPixels(cs->x_lo, mouse.y);
            ImVec2 ch_r = ImPlot::PlotToPixels(cs->x_hi, mouse.y);
            ImU32 ch_col = ImGui::GetColorU32(ImVec4(
                FoxmlColors::comment.x, FoxmlColors::comment.y,
                FoxmlColors::comment.z, 0.35f));
            for (float x = ch_l.x; x < ch_r.x; x += 6.0f) {
                float x2 = x + 2.0f;
                if (x2 > ch_r.x) x2 = ch_r.x;
                dl->AddLine(ImVec2(x, ch_l.y), ImVec2(x2, ch_l.y), ch_col, 1.0f);
            }
            // volume tag at right edge
            char vol_buf[16];
            snprintf(vol_buf, 16, "%.4f", mouse.y);
            ImVec2 vsz = ImGui::CalcTextSize(vol_buf);
            ImVec2 vr_px = ImPlot::PlotToPixels(cs->x_hi, 0);
            float vr = vr_px.x - 4;
            float vl = vr - vsz.x - 8.0f;
            ImVec2 vtl(vl, ch_l.y - vsz.y * 0.5f - 2);
            ImVec2 vbr(vr, ch_l.y + vsz.y * 0.5f + 2);
            dl->AddRectFilled(vtl, vbr, ImGui::GetColorU32(FoxmlColors::surface), 2.0f);
            dl->AddText(ImVec2(vl + 4, vtl.y + 2), ImGui::GetColorU32(FoxmlColors::wheat), vol_buf);
        }

        ImPlot::EndPlot();
    }
    ImPlot::PopStyleColor(2);  // PlotBg + AxisGrid
    ImGui::End();
}

//==========================================================================
// LIVE P&L — streaming chart from pnl_history ring buffer
//==========================================================================
static inline void GUI_LivePnLChart(const TUISnapshot *s) {
    ImGui::Begin("Live P&L");
    if (s->graph_count < 2) {
        ImGui::TextColored(FoxmlColors::comment, "collecting data...");
        ImGui::End();
        return;
    }

    // unroll ring buffer into linear array
    int n = s->graph_count;
    int len = TUISnapshot::GRAPH_LEN;
    double xs[TUISnapshot::GRAPH_LEN], ys[TUISnapshot::GRAPH_LEN], zeros[TUISnapshot::GRAPH_LEN] = {};
    for (int i = 0; i < n; i++) {
        int ri = (s->graph_head - n + i + len) % len;
        xs[i] = (double)i;
        ys[i] = s->pnl_history[ri];
    }

    ImPlot::PushStyleColor(ImPlotCol_PlotBg, FoxmlColors::bg_dark);
    if (ImPlot::BeginPlot("##live_pnl", ImVec2(-1, -1),
                           ImPlotFlags_NoTitle | ImPlotFlags_NoMouseText)) {
        ImPlot::SetupAxes(NULL, "P&L",
                          ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickLabels,
                          ImPlotAxisFlags_Opposite);
        ImPlot::SetupAxisLimits(ImAxis_X1, -0.5, n - 0.5, ImPlotCond_Always);

        double ymin = 0, ymax = 0;
        for (int i = 0; i < n; i++) {
            if (ys[i] < ymin) ymin = ys[i];
            if (ys[i] > ymax) ymax = ys[i];
        }
        double yrange = ymax - ymin;
        if (yrange < 0.01) yrange = 1.0;
        double ypad = yrange * 0.15;
        ImPlot::SetupAxisLimits(ImAxis_Y1, ymin - ypad, ymax + ypad, ImPlotCond_Always);
        ImPlot::SetupAxisFormat(ImAxis_Y1, "$%+.2f");

        // zero line
        double zy[2] = {0, 0}, zx[2] = {-0.5, (double)(n - 0.5)};
        ImPlotSpec zs; zs.LineColor = FoxmlColors::surface; zs.LineWeight = 1.0f;
        ImPlot::PlotLine("##zero", zx, zy, 2, zs);

        // P&L line
        ImPlotSpec ls; ls.LineColor = FoxmlColors::primary; ls.LineWeight = 1.5f;
        ImPlot::PlotLine("P&L", xs, ys, n, ls);

        // shaded fill — green above zero, red below
        ImPlotSpec gs; gs.FillColor = FoxmlColors::green; gs.FillAlpha = 0.12f;
        gs.LineColor = {0,0,0,0};
        ImPlot::PlotShaded("##fill", xs, ys, zeros, n, gs);

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
                          ImPlotAxisFlags_Opposite);
        ImPlot::SetupAxisLimits(ImAxis_X1, -0.5, en - 0.5, ImPlotCond_Always);
        // Y padding so the curve doesn't slam into top/bottom edges
        double eq_min = 0, eq_max = 0;
        for (int i = 0; i < en; i++) {
            if (eq_ys[i] < eq_min) eq_min = eq_ys[i];
            if (eq_ys[i] > eq_max) eq_max = eq_ys[i];
        }
        double eq_range = eq_max - eq_min;
        if (eq_range < 0.01) eq_range = 1.0;
        double eq_pad = eq_range * 0.15;
        ImPlot::SetupAxisLimits(ImAxis_Y1, eq_min - eq_pad, eq_max + eq_pad, ImPlotCond_Always);
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

        // hover crosshair + P&L readout
        if (ImPlot::IsPlotHovered()) {
            ImPlotPoint mouse = ImPlot::GetPlotMousePos();
            ImVec2 ch_l = ImPlot::PlotToPixels(-0.5, mouse.y);
            ImVec2 ch_r = ImPlot::PlotToPixels(en - 0.5, mouse.y);
            ImU32 ch_col = ImGui::GetColorU32(ImVec4(
                FoxmlColors::comment.x, FoxmlColors::comment.y,
                FoxmlColors::comment.z, 0.35f));
            for (float x = ch_l.x; x < ch_r.x; x += 6.0f) {
                float x2 = x + 2.0f;
                if (x2 > ch_r.x) x2 = ch_r.x;
                dl->AddLine(ImVec2(x, ch_l.y), ImVec2(x2, ch_l.y), ch_col, 1.0f);
            }
            char pnl_buf[16];
            snprintf(pnl_buf, 16, "$%+.2f", mouse.y);
            ImVec2 psz = ImGui::CalcTextSize(pnl_buf);
            float pr = ch_r.x - 4;
            float pl = pr - psz.x - 8.0f;
            ImVec2 ptl(pl, ch_l.y - psz.y * 0.5f - 2);
            ImVec2 pbr(pr, ch_l.y + psz.y * 0.5f + 2);
            dl->AddRectFilled(ptl, pbr, ImGui::GetColorU32(FoxmlColors::surface), 2.0f);
            dl->AddText(ImVec2(pl + 4, ptl.y + 2), ImGui::GetColorU32(FoxmlColors::wheat), pnl_buf);
        }

        ImPlot::EndPlot();
    }
    ImPlot::PopStyleColor();
    ImGui::End();
}
