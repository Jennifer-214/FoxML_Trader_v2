#pragma once

//======================================================================
// [FILE]_[GUI/ChartPanel.hpp]
//----------------------------------------------------------------------
// [TAG]_[[GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[TradingView-style charts as separate dockable ImGui/ImPlot windows — Price, Volume, Live P&L, Equity Curve; all share per-frame ChartState prepared from the candle snapshot, the price chart supports drag-to-edit TP/SL and an ML overlay]
//======================================================================
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
#include "../MemHeaders/PerNodeStateFlagsRegistry.hpp"  // v5.14.9.B.2 — STATE_FLAG_IS_SET

//======================================================================
// [STRUCT]_[ChartSettings]
//----------------------------------------------------------------------
// [TAG]_[[GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[chart display settings — visible candle count + interval, overlay toggles, Y auto-fit control, show-all-levels, per-core marker filter]
//======================================================================
// chart display settings (mutable — controlled by GUI dropdowns)
//======================================================================
// [CODE]
//======================================================================
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
    bool show_ml_overlay = false;  // prediction probability + confidence band
    // v4.7.7: chart auto-fit control. When true (initial state OR after the
    // user clicks "Reset View"), the next frame applies ImPlotCond_Always
    // on the Y axis to re-center on candles. Auto-cleared after one frame
    // so subsequent frames use ImPlotCond_Once and the user can scroll-
    // wheel zoom without the auto-fit snapping back.
    bool y_reset_requested = true;
    // v4.7.21: opt-in "fit Y to all TP/SL levels" mode. Default off — Reset
    // View fits to candles + nearby TP/SL only (current behavior). When ON,
    // Reset View pulls Y range out to include every position's full TP/SL
    // span, even if those sit far from current price. Useful for seeing
    // the full risk geometry at a glance; bad as a default because far TP/SL
    // squashes price action into a thin band.
    bool show_all_levels = false;
    // v4.7.10: core filter for entry/TP/SL markers. -1 = all cores (default,
    // unchanged behavior). 0..N-1 = show only that core's positions on the
    // chart. Affects entry markers, TP/SL dashed lines + tags, and
    // Y-axis auto-expansion. Candles + indicators (VWAP/SMA/sessions/H/L)
    // are global to the symbol and always rendered regardless.
    int node_filter = -1;
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]   (tool-refreshed — layout emitter cannot probe this block yet; quartet lands when the emitter covers it, D-327)
//======================================================================
// [END_STRUCT]_[ChartSettings]
//======================================================================

//======================================================================
// [STRUCT]_[ChartState]
//----------------------------------------------------------------------
// [TAG]_[[GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[per-frame prepared chart data shared by all chart windows — visible OHLCV arrays + SMA + VWAP + X range + candle half-width]
//======================================================================
//======================================================================
// [CODE]
//======================================================================
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
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]   (tool-refreshed — layout emitter cannot probe this block yet; quartet lands when the emitter covers it, D-327)
//======================================================================
// [END_STRUCT]_[ChartState]
//======================================================================

//======================================================================
// [FUNCTION]_[ChartState_Prepare]
//----------------------------------------------------------------------
// [TAG]_[[GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[prepare the shared ChartState once per frame from the candle snapshot + settings — extract visible candles, compute SMA/VWAP, set the X range]
//======================================================================
//======================================================================
// [CODE]
//======================================================================
static inline void ChartState_Prepare(ChartState *cs, const CandleSnapshot *csnap,
                                       const ChartSettings *settings) {
    int n = csnap->count;
    int vis = settings->visible_candles;
    cs->ready = (n >= 2);
    if (!cs->ready) return;

    // v4.7.12: load ALL candles (up to CANDLE_MAX = 4096), not just the
    // last `vis`. xs[i] is now absolute unix time (seconds since epoch)
    // instead of array index. This makes the X axis stable across ring-
    // buffer eviction — a candle's X coordinate doesn't shift when
    // older candles drop off, so user pan position stays anchored to
    // the same wall-clock time.
    //
    // Public-surface rule: kept field name `xs` and shape (double[]).
    // Consumers that PlotToPixels(cs->xs[i], ...) keep working — they
    // treat xs as opaque doubles. The only callers that DERIVE meaning
    // from xs values are inside this file and updated in lockstep.
    cs->vis_count = n;
    for (int i = 0; i < n; i++) {
        const Candle &c = csnap->candles[i];
        cs->xs[i]        = c.time_sec;
        cs->opens[i]     = c.open;
        cs->highs[i]     = c.high;
        cs->lows[i]      = c.low;
        cs->closes[i]    = c.close;
        cs->volumes[i]   = c.volume;
        cs->buy_ratios[i] = (c.volume > 0) ? c.buy_vol / c.volume : 0.5;
        cs->times_sec[i] = c.time_sec;
    }

    cs->last_price = csnap->candles[n - 1].close;
    cs->vwap = csnap->vwap;
    // Initial X view spans `vis * interval` seconds. Bars pack adjacent at
    // consistent interval-wide spacing.
    //   n < vis  (cold start): LEFT-anchored. Oldest candle pinned to the
    //              left edge; new candles march in from the left and
    //              extend rightward until the window fills.
    //   n >= vis (steady state): RIGHT-anchored. Newest candle on the
    //              right edge; window slides with each new candle so the
    //              latest `vis` always show.
    // Horizontal lines + right-edge labels read from ImPlot::GetPlotLimits()
    // so they follow live pan/zoom — see xL/xR captured at top of the plot.
    double interval = (double)settings->candle_interval;
    if (n < vis) {
        cs->x_lo = csnap->candles[0].time_sec - interval * 0.5;
        cs->x_hi = cs->x_lo + (double)vis * interval;
    } else {
        cs->x_hi = csnap->candles[n - 1].time_sec + interval * 0.5;
        cs->x_lo = cs->x_hi - (double)vis * interval;
    }

    // SMA
    cs->sma_first = -1;
    memset(cs->sma, 0, sizeof(cs->sma));
    if (n >= 20) {
        // v4.7.12: i now indexes into the full snapshot (no vis_start
        // offset). SMA only meaningful from index 19 onward.
        for (int i = 19; i < n; i++) {
            double sum = 0;
            for (int j = i - 19; j <= i; j++) sum += csnap->candles[j].close;
            cs->sma[i] = sum / 20.0;
            if (cs->sma_first < 0) cs->sma_first = i;
        }
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[ChartState_Prepare]
//======================================================================

//======================================================================
// [STRUCT]_[DragState]
//----------------------------------------------------------------------
// [TAG]_[[GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the TP/SL line-drag state — which position slot/leg is being dragged and to what price]
//======================================================================
//======================================================================
// [CODE]
//======================================================================
struct DragState {
    int active;       // currently dragging
    int slot;         // position bitmap slot
    int is_tp;        // 1=TP, 0=SL
    double price;     // current drag price
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]   (tool-refreshed — layout emitter cannot probe this block yet; quartet lands when the emitter covers it, D-327)
//======================================================================
// [END_STRUCT]_[DragState]
//======================================================================

//======================================================================
// [FUNCTION]_[GUI_PriceChart]
//----------------------------------------------------------------------
// [TAG]_[[GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[render the Price Chart window — candlesticks + VWAP/SMA/session overlays + per-core entry/TP/SL markers with drag-to-edit, ML prediction overlay, crosshair]
//======================================================================
//======================================================================
// [CODE]
//======================================================================
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
    // v5.11.62 — show ML overlay checkbox if ANY core has a model active,
    // either single-zoo (centralized snap) or ensemble (per-core).
    bool any_ml_active = snap->ml.ml_model_loaded != 0;
    if (!any_ml_active && snap->sharded_mode_active) {
        for (int i = 0; i < snap->per_node_count && i < 16; ++i) {
            if (STATE_FLAG_IS_SET(snap->per_node[i], ML_MODEL_LOADED) ||
                (snap->per_node[i].ensemble_active &&
                 snap->per_node[i].ensemble_n_horizons > 0)) {
                any_ml_active = true;
                break;
            }
        }
    }
    if (any_ml_active) {
        ImGui::SameLine(); ImGui::Checkbox("ML", &settings->show_ml_overlay);
    }

    // v4.7.7: Reset View — re-fit Y axis to candles + nearby TP/SL on
    // demand. Useful when user has scrolled out and wants to snap back
    // to "show me the action."
    ImGui::SameLine(0, 20);
    if (ImGui::SmallButton("Reset View")) {
        settings->y_reset_requested = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Re-fit Y axis to current candle range + nearby TP/SL.\n"
                          "Use scroll wheel inside the chart to zoom freely.");
    }
    // v4.7.21: opt-in toggle to make Reset View include all TP/SL levels
    // even if they sit far from current price. Off by default (otherwise
    // far TP/SL squashes candles into a thin band).
    ImGui::SameLine(0, 8);
    ImGui::Checkbox("All Levels", &settings->show_all_levels);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("When ON, Reset View pulls Y range to include every\n"
                          "position's TP and SL, even if they sit far from price.\n"
                          "Useful for seeing full risk geometry; squashes candles.");
    }

    // v4.7.10: per-core filter dropdown. When a specific core is picked,
    // only that core's entry markers + TP/SL lines render — useful for
    // isolating one core's behavior when 4 strategies overlap.
    ImGui::SameLine(0, 15);
    ImGui::TextColored(FoxmlColors::comment, "Node");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70);
    {
        static const char *node_labels[] = {"All", "0", "1", "2", "3"};
        int cur = (settings->node_filter < 0 || settings->node_filter > 3)
                  ? 0 : settings->node_filter + 1;
        if (ImGui::Combo("##node_filter", &cur, node_labels, 5)) {
            settings->node_filter = (cur == 0) ? -1 : (cur - 1);
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Show entry markers + TP/SL lines for one node only.\n"
                          "Candles + global indicators always render.");
    }

    // v4.7.10 helper: returns true if this slot should be skipped given the
    // core filter. -1 (= all) returns false unconditionally. Otherwise,
    // map slot → node_id (slot/2 under partials, slot otherwise) and
    // skip if that node_id != filter. Used in every position-iteration
    // loop below.
    int filter_core = settings->node_filter;
    int filter_partial = snap->partial_exit_enabled ? 1 : 0;
    auto slot_filtered_out = [filter_core, filter_partial](int slot_idx) -> bool {
        if (filter_core < 0) return false;
        int core = filter_partial ? (slot_idx >> 1) : slot_idx;
        return core != filter_core;
    };

    ImPlot::PushStyleColor(ImPlotCol_PlotBg, FoxmlColors::bg_dark);
    // subtle Y grid lines for price readability
    ImPlot::PushStyleColor(ImPlotCol_AxisGrid, ImVec4(1, 1, 1, 0.06f));
    // v4.7.18: pan vs drag-TP/SL routing.
    //   plain LMB drag → ImPlot pan (default)
    //   shift+LMB drag → grab + drag a TP/SL line (our handler below)
    // To make this work, we dynamically suppress ImPlot's pan when shift
    // is held — by setting PanMod to a modifier the user isn't pressing,
    // pan stops firing this frame, leaving LMB free for our drag. When
    // shift is released, PanMod reverts to none and plain drag pans
    // again. Saved input map is restored at EndPlot for other plots
    // (volume) to keep their default bindings.
    ImPlotInputMap prev_input_map = ImPlot::GetInputMap();
    if (ImGui::GetIO().KeyShift) {
        ImPlot::GetInputMap().PanMod = ImGuiMod_Ctrl;  // disable plain-LMB pan this frame
    } else {
        ImPlot::GetInputMap().PanMod = 0;
    }
    if (ImPlot::BeginPlot("##price", ImVec2(-1, -1),
                           ImPlotFlags_NoTitle | ImPlotFlags_NoMouseText)) {

        // v4.7.14 fix for v4.7.12 regression: X axis stays linear
        // (NOT ImPlotScale_Time). The time-scale transform broke
        // PlotToPixels for our manually-drawn candle bodies — they
        // landed at wrong pixel coords or outside the visible plot
        // area. We keep cs->xs as time_sec (stable across ring
        // eviction so pan-back still works), but feed it as plain
        // doubles. Manual HH:MM tick labels (loop below) restored.
        ImPlot::SetupAxes(NULL, NULL,
                          ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoGridLines,
                          ImPlotAxisFlags_Opposite);
        ImPlotCond x_cond = settings->y_reset_requested ? ImPlotCond_Always : ImPlotCond_Once;
        ImPlot::SetupAxisLimits(ImAxis_X1, cs->x_lo, cs->x_hi, x_cond);
        // secondary Y-axis for ML prediction overlay (0-1 range)
        if (settings->show_ml_overlay && snap->ml.pred_count > 1) {
            ImPlot::SetupAxis(ImAxis_Y2, NULL, ImPlotAxisFlags_NoGridLines | ImPlotAxisFlags_NoLabel);
            ImPlot::SetupAxisLimits(ImAxis_Y2, 0.0, 1.0, ImPlotCond_Always);
        }

        // v4.7.14: manual time tick labels restored. Now that xs[i] is
        // unix_seconds (eviction-stable), label values are taken directly
        // from xs[i] — same as pre-v4.7.12 except no vis_start offset
        // since ChartState_Prepare loads the full snapshot now.
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

        // Y limits with padding + TP/SL expansion (bounded).
        // v4.7.6: previously we unconditionally expanded Y to include
        // every position's TP and SL — fine when configs set TP/SL
        // close to current price, but with wider 1%+ TP/SL the chart
        // gets dominated by far markers and candles squish into ~10%
        // of vertical space. Now we expand for TP/SL only when they
        // sit within ~2× the candle range; further markers render at
        // the chart edge as out-of-view tags (clipped by ImPlot).
        //
        // v4.7.21: candle Y fit is now restricted to the visible-X window
        // [cs->x_lo, cs->x_hi]. Previously it walked all candles in the
        // buffer (up to 4096), so a deep dip from earlier in the session
        // would inflate candle_range and pull the expansion budget out
        // far enough to swallow distant SL lines — collapsing recent
        // price action into a thin band. Limiting to the post-reset
        // visible window matches what the user actually sees.
        double y_min = 1e18, y_max = -1e18;
        for (int i = 0; i < vc; i++) {
            if (cs->xs[i] < cs->x_lo || cs->xs[i] > cs->x_hi) continue;
            if (cs->lows[i] < y_min) y_min = cs->lows[i];
            if (cs->highs[i] > y_max) y_max = cs->highs[i];
        }
        // safety net: visible window may have zero candles momentarily
        // (cold start / pan beyond data). fall back to all candles.
        if (y_min > y_max) {
            for (int i = 0; i < vc; i++) {
                if (cs->lows[i] < y_min) y_min = cs->lows[i];
                if (cs->highs[i] > y_max) y_max = cs->highs[i];
            }
        }
        double min_range = cs->last_price * 0.001;
        if (min_range < 20.0) min_range = 20.0;
        if ((y_max - y_min) < min_range) {
            double mid = (y_min + y_max) * 0.5;
            y_min = mid - min_range * 0.5;
            y_max = mid + min_range * 0.5;
        }
        // Expansion budget: candle range × 2. TP/SL within this window
        // gets included; further markers stay where they are and ImPlot
        // clips them to the visible area.
        //
        // v4.7.21: when settings->show_all_levels is set, expansion is
        // unbounded — every position's TP/SL is included regardless of
        // distance from candles. Off by default; useful for seeing the
        // full risk geometry across the session.
        double candle_range = y_max - y_min;
        double expand_lo    = y_min - candle_range;
        double expand_hi    = y_max + candle_range;
        for (int pi = 0; pi < 16; pi++) {
            const TUIPositionSnap *ps = &snap->positions[pi];
            if (ps->idx < 0) continue;
            if (slot_filtered_out(ps->idx)) continue;
            if (settings->show_all_levels) {
                if (ps->tp > 0 && ps->tp > y_max) y_max = ps->tp;
                if (ps->sl > 0 && ps->sl < y_min) y_min = ps->sl;
            } else {
                if (ps->tp > 0 && ps->tp > y_max && ps->tp <= expand_hi) y_max = ps->tp;
                if (ps->sl > 0 && ps->sl < y_min && ps->sl >= expand_lo) y_min = ps->sl;
            }
        }
        double pad = (y_max - y_min) * 0.1;
        // v4.7.7: Always on first render OR when user clicks "Reset View",
        // Once otherwise so scroll-wheel zoom isn't snapped back every frame.
        ImPlotCond y_cond = settings->y_reset_requested ? ImPlotCond_Always : ImPlotCond_Once;
        ImPlot::SetupAxisLimits(ImAxis_Y1, y_min - pad, y_max + pad, y_cond);
        if (settings->y_reset_requested) settings->y_reset_requested = false;
        ImPlot::SetupAxisFormat(ImAxis_Y1, "$%.0f");

        // regime background shading
        ImDrawList *dl = ImPlot::GetPlotDrawList();
        ImPlot::PushPlotClipRect();
        // v4.7.16: live plot bounds. cs->x_lo/cs->x_hi are the INITIAL view
        // (candle range padded). Once the user pans or zooms, those bounds
        // become stale and any horizontal line drawn at them stops short of
        // the visible chart edge. ImPlot::GetPlotLimits() returns the
        // CURRENT visible X range every frame — use it for every full-width
        // horizontal line (price/VWAP/EMA/gates/H-L/crosshair/entry/TP/SL).
        ImPlotRect _plim = ImPlot::GetPlotLimits();
        const double xL = _plim.X.Min;
        const double xR = _plim.X.Max;
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
            ImVec2 price_l = ImPlot::PlotToPixels(xL, cs->last_price);
            ImVec2 price_r = ImPlot::PlotToPixels(xR, cs->last_price);
            dl->AddLine(price_l, price_r,
                        ImGui::GetColorU32(ImVec4(FoxmlColors::wheat.x, FoxmlColors::wheat.y,
                                                   FoxmlColors::wheat.z, 0.3f)), 1.0f);
        }

        // candlesticks
        // v4.7.14: candle width measured from one interval span at
        // CURRENT zoom level so candles compress/expand smoothly.
        // Reference points must be inside the visible plot area —
        // PlotToPixels for off-screen coords can return clipped/garbage
        // values on some ImPlot versions. Use cs->x_lo (always visible)
        // and cs->x_lo + interval as the reference span.
        ImVec2 px_l = ImPlot::PlotToPixels(cs->x_lo, cs->lows[0]);
        ImVec2 px_r = ImPlot::PlotToPixels(cs->x_lo + (double)settings->candle_interval, cs->lows[0]);
        float candle_px = (px_r.x - px_l.x) * 0.7f;
        if (candle_px < 1.5f) candle_px = 1.5f;
        if (candle_px > 40.0f) candle_px = 40.0f;
        float hw = candle_px * 0.5f;

        // Color flicker fix: tiny ±1-cent oscillations around opens[i] (live
        // candle) flip bull/bear every tick at HFT scale. Use a small basis-
        // point threshold (0.005% = ~$3.88 at $77k BTC). Within threshold:
        // neutral (wheat) — within tick-noise, no directional bias.
        const double FLICKER_BPS = 0.00005; // 0.005% = 0.5 basis points
        for (int i = 0; i < vc; i++) {
            ImVec2 po = ImPlot::PlotToPixels(cs->xs[i], cs->opens[i]);
            ImVec2 pc = ImPlot::PlotToPixels(cs->xs[i], cs->closes[i]);
            ImVec2 pl = ImPlot::PlotToPixels(cs->xs[i], cs->lows[i]);
            ImVec2 ph = ImPlot::PlotToPixels(cs->xs[i], cs->highs[i]);
            double delta = cs->closes[i] - cs->opens[i];
            double thr = cs->opens[i] * FLICKER_BPS;
            int bull_state = (delta > thr) ? 1 : (delta < -thr) ? -1 : 0;
            ImU32 bc, wc;
            if (bull_state > 0) {
                bc = ImGui::GetColorU32(FoxmlColors::green_b);
                wc = ImGui::GetColorU32(FoxmlColors::sand);
            } else if (bull_state < 0) {
                bc = ImGui::GetColorU32(FoxmlColors::red);
                wc = ImGui::GetColorU32(FoxmlColors::comment);
            } else {
                // neutral — within tick-noise of open
                bc = ImGui::GetColorU32(ImVec4(FoxmlColors::wheat.x, FoxmlColors::wheat.y,
                                                FoxmlColors::wheat.z, 0.6f));
                wc = ImGui::GetColorU32(FoxmlColors::comment);
            }
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
            if (slot_filtered_out(ps->idx)) continue;
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
            double vy[2] = {cs->vwap, cs->vwap}, vx[2] = {xL, xR};
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
            double ex[2] = {xL, xR};
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
            ImVec2 price_px = ImPlot::PlotToPixels(xR, cs->last_price);
            ImVec2 plot_r = ImPlot::PlotToPixels(xR, 0);
            char ptag[16];
            snprintf(ptag, 16, "$%.0f", cs->last_price);
            ImVec2 tsz = ImGui::CalcTextSize(ptag);
            float pr = plot_r.x - 2;
            float pl = pr - tsz.x - 8;
            // Flicker fix (same threshold as candle body): tiny oscillations
            // around closes[vc-2] flip bull/bear every tick. Threshold to a
            // 0.5 basis-point neutral zone — wheat tag below threshold.
            ImVec4 tag_col;
            if (vc >= 2) {
                double delta = cs->closes[vc-1] - cs->closes[vc-2];
                double thr = cs->closes[vc-2] * FLICKER_BPS;
                if (delta > thr)       tag_col = FoxmlColors::green_b;
                else if (delta < -thr) tag_col = FoxmlColors::red;
                else                   tag_col = FoxmlColors::wheat;
            } else {
                tag_col = FoxmlColors::wheat;
            }
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
            ImVec2 sh_l = ImPlot::PlotToPixels(xL, snap->session_high);
            ImVec2 sh_r = ImPlot::PlotToPixels(xR, snap->session_high);
            for (float x = sh_l.x; x < sh_r.x; x += 8.0f) {
                float x2 = x + 3.0f; if (x2 > sh_r.x) x2 = sh_r.x;
                dl->AddLine(ImVec2(x, sh_l.y), ImVec2(x2, sh_l.y), sess_col, 1.0f);
            }
            // session low
            ImVec2 sl_l = ImPlot::PlotToPixels(xL, snap->session_low);
            ImVec2 sl_r = ImPlot::PlotToPixels(xR, snap->session_low);
            for (float x = sl_l.x; x < sl_r.x; x += 8.0f) {
                float x2 = x + 3.0f; if (x2 > sl_r.x) x2 = sl_r.x;
                dl->AddLine(ImVec2(x, sl_l.y), ImVec2(x2, sl_l.y), sess_col, 1.0f);
            }
        }

        // === session dividers (vertical lines at UTC hour boundaries) ===
        if (settings->show_session_div && vc > 1) {
            // session hours (UTC): Asian 0-8, EU 8-13, US 13-21, Overnight 21-24
            static const int boundaries[] = {0, 8, 13, 21};
            // use shared SESSION_NAMES — drops the OVER/OVERNIGHT inconsistency
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
                                    SESSION_NAMES[b]);
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
        ImVec2 plot_right = ImPlot::PlotToPixels(xR, 0);
        float right_edge = plot_right.x - 4;

        // newest position index for fade effect
        int newest_di = display_count - 1;

        // draw entry lines + collect entry labels
        double drawn_entries[16] = {};
        int drawn_entry_count = 0;
        for (int pi = 0; pi < 16; pi++) {
            const TUIPositionSnap *ps = &snap->positions[pi];
            if (ps->idx < 0 || ps->entry <= 0) continue;
            if (slot_filtered_out(ps->idx)) continue;

            bool already_drawn = false;
            for (int j = 0; j < drawn_entry_count; j++) {
                if (fabs(drawn_entries[j] - ps->entry) < 0.01) {
                    already_drawn = true;
                    break;
                }
            }
            if (already_drawn) continue;

            // v5.11.64 — emit #core.leg notation so chart labels match the
            // Positions table format. Pre-fix: "#0,1,2,3 $80141" — slot
            // indices, opaque. Post-fix: "#0.A,0.B,1.A,1.B $80141" — core
            // and leg explicit, mirroring the Positions panel.
            int max_di = 0;
            char group_ids[96] = {};
            int group_len = 0;
            int partials = snap->partial_exit_enabled ? 1 : 0;
            for (int j = 0; j < 16; j++) {
                if (snap->positions[j].idx < 0) continue;
                if (fabs(snap->positions[j].entry - ps->entry) < 0.01) {
                    if (group_len > 0 && group_len < (int)sizeof(group_ids) - 1) {
                        group_ids[group_len++] = ',';
                    }
                    int slot = snap->positions[j].idx;
                    int row_core = partials ? (slot >> 1) : slot;
                    int row_leg  = partials ? (slot & 1)  : 0;
                    int wrote = 0;
                    if (partials) {
                        wrote = snprintf(group_ids + group_len,
                                         sizeof(group_ids) - group_len,
                                         "%d.%c", row_core,
                                         row_leg == 0 ? 'A' : 'B');
                    } else {
                        wrote = snprintf(group_ids + group_len,
                                         sizeof(group_ids) - group_len,
                                         "%d", row_core);
                    }
                    if (wrote > 0) group_len += wrote;
                    int dj = display_idx[j];
                    if (dj > max_di) max_di = dj;
                }
            }
            group_ids[group_len] = '\0';

            // fade older positions (newest=full, oldest=40%)
            float age_alpha = (display_count <= 1) ? 1.0f :
                0.4f + 0.6f * ((float)max_di / newest_di);

            // entry line — full width, solid
            double y[2] = {ps->entry, ps->entry};
            double lx[2] = {xL, xR};
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
            if (slot_filtered_out(ps->idx)) continue;
            int di = display_idx[pi];
            float age_alpha = (display_count <= 1) ? 1.0f :
                0.4f + 0.6f * ((float)di / newest_di);
            const ImVec4 &accent = pos_accent[di % 8];

            for (int tp_pass = 0; tp_pass < 2; tp_pass++) {
                double price = tp_pass == 0 ? ps->tp : ps->sl;
                if (price <= 0) continue;
                bool is_tp = (tp_pass == 0);

                ImVec2 left  = ImPlot::PlotToPixels(xL, price);
                ImVec2 right = ImPlot::PlotToPixels(xR, price);
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
                // v5.11.64 — #core.leg notation matches Positions table.
                double pnl = price - ps->entry;
                int slot = ps->idx;
                int partials = snap->partial_exit_enabled ? 1 : 0;
                int lbl_core = partials ? (slot >> 1) : slot;
                int lbl_leg  = partials ? (slot & 1)  : 0;
                if (partials) {
                    snprintf(cl.text, 32, "#%d.%c %s %+.0f",
                             lbl_core, lbl_leg == 0 ? 'A' : 'B',
                             is_tp ? "TP" : "SL", pnl);
                } else {
                    snprintf(cl.text, 32, "#%d %s %+.0f", lbl_core,
                             is_tp ? "TP" : "SL", pnl);
                }
            }
        }

        // draggable TP/SL lines (shift+LMB drag — see input-map block above)
        static DragState drag = {0, -1, 0, 0};
        if (ImPlot::IsPlotHovered() || drag.active) {
            ImPlotPoint mouse = ImPlot::GetPlotMousePos();
            ImVec2 mouse_px = ImGui::GetMousePos();
            bool mouse_down = ImGui::IsMouseDown(0);
            // v4.7.18: require shift to be held to START the drag.
            // Continuation (drag.active already 1) is independent — user
            // can release shift mid-drag without losing the grab.
            bool shift_held = ImGui::GetIO().KeyShift;

            if (!drag.active) {
                // hover detection: find nearest TP/SL line within 5px
                float best_dist = 6.0f;
                int best_slot = -1, best_tp = 0;
                for (int pi = 0; pi < 16; pi++) {
                    const TUIPositionSnap *ps = &snap->positions[pi];
                    if (ps->idx < 0) continue;
                    if (slot_filtered_out(ps->idx)) continue;
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
                if (best_slot >= 0 && shift_held) {
                    // highlight the hovered line (only when shift held —
                    // otherwise hover near a line shouldn't change cursor
                    // since plain drag is for panning)
                    double hp = best_tp ? snap->positions[best_slot].tp
                                        : snap->positions[best_slot].sl;
                    ImVec2 hl = ImPlot::PlotToPixels(xL, hp);
                    ImVec2 hr = ImPlot::PlotToPixels(xR, hp);
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
                ImVec2 dl_l = ImPlot::PlotToPixels(xL, drag.price);
                ImVec2 dl_r = ImPlot::PlotToPixels(xR, drag.price);
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
                        // v5.11.64 — #core.leg matches Positions table.
                        int slot = drag.slot;
                        int partials = snap->partial_exit_enabled ? 1 : 0;
                        int lbl_core = partials ? (slot >> 1) : slot;
                        int lbl_leg  = partials ? (slot & 1)  : 0;
                        if (partials) {
                            snprintf(clabels[li].text, 32, "#%d.%c %s %+.0f",
                                     lbl_core, lbl_leg == 0 ? 'A' : 'B',
                                     drag.is_tp ? "TP" : "SL", pnl);
                        } else {
                            snprintf(clabels[li].text, 32, "#%d %s %+.0f",
                                     lbl_core, drag.is_tp ? "TP" : "SL", pnl);
                        }
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

        // buy gate threshold — cyan, thick dotted, distinct from entry/TP/SL.
        // In sharded mode this is core 0's gate, redundant with the per-core
        // lines drawn below. Hide it to avoid double-drawing + reduce clutter.
        if (snap->buy_p > 0.01 && !snap->sharded_mode_active) {
            ImVec2 left  = ImPlot::PlotToPixels(xL, snap->buy_p);
            ImVec2 right = ImPlot::PlotToPixels(xR, snap->buy_p);
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
            ImPlot::Annotation(xL, snap->buy_p,
                               {FoxmlColors::cyan.x, FoxmlColors::cyan.y, FoxmlColors::cyan.z, 1.0f},
                               ImVec2(35, 0), true, "GATE $%.0f", snap->buy_p);
        }

        // hover crosshair + tooltip
        if (settings->show_crosshair && ImPlot::IsPlotHovered()) {
            ImPlotPoint mouse = ImPlot::GetPlotMousePos();

            // horizontal crosshair line
            ImVec2 ch_l = ImPlot::PlotToPixels(xL, mouse.y);
            ImVec2 ch_r = ImPlot::PlotToPixels(xR, mouse.y);
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

        // ML prediction overlay (secondary Y-axis, 0-1 range)
        if (settings->show_ml_overlay && snap->ml.pred_count > 1) {
            ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
            int n = snap->ml.pred_count;
            int len = MLSnapshot::PRED_HISTORY_LEN;
            int plot_n = (n < vc) ? n : vc;
            double xs[240], pred_ys[240], conf_ys[240];
            for (int i = 0; i < plot_n; i++) {
                int ring_idx = ((snap->ml.pred_head - plot_n + i) % len + len) % len;
                xs[i] = vc - plot_n + i;
                pred_ys[i] = snap->ml.pred_history[ring_idx];
                conf_ys[i] = snap->ml.conf_history[ring_idx];
            }
            ImVec4 pred_col = (snap->ml.ml_last_prediction >= 0.5)
                ? ImVec4(0.42f, 0.60f, 0.48f, 0.8f)
                : ImVec4(0.69f, 0.33f, 0.33f, 0.8f);
            ImPlotSpec ps; ps.LineColor = pred_col; ps.LineWeight = 1.5f;
            ImPlot::PlotLine("Prediction", xs, pred_ys, plot_n, ps);
            ImPlotSpec cs2; cs2.FillColor = ImVec4(0.5f, 0.5f, 0.5f, 0.15f);
            ImPlot::PlotShaded("Confidence", xs, conf_ys, plot_n, 0.0, cs2);
            double thresh_x[2] = {0, (double)(vc - 1)};
            double thresh_y[2] = {0.5, 0.5};
            ImPlotSpec ts; ts.LineColor = ImVec4(1, 1, 1, 0.2f); ts.LineWeight = 1.0f;
            ImPlot::PlotLine("##thresh", thresh_x, thresh_y, 2, ts);
        }

        // Per-core buy gate overlays (sharded mode only). One dashed
        // horizontal line per core at the buy gate price threshold,
        // color-coded by strategy. Only drawn when the price is non-zero.
        if (snap->sharded_mode_active) {
            ImVec4 strat_colors[NUM_STRATEGIES] = {
                {0.40f, 0.60f, 0.85f, 0.6f},  // 0 MR — blue
                {0.85f, 0.55f, 0.25f, 0.6f},  // 1 MOM — orange
                {0.45f, 0.75f, 0.45f, 0.6f},  // 2 DIP — green
                {0.65f, 0.45f, 0.80f, 0.6f},  // 3 ML — purple
                {0.35f, 0.75f, 0.80f, 0.6f},  // 4 EMA — cyan
                {0.90f, 0.80f, 0.50f, 0.6f},  // 5 AUTO — gold (was missing —
                                              //   gate lines for AUTO cores
                                              //   rendered as transparent
                                              //   black, invisible on chart)
            };
            static_assert(sizeof(strat_colors)/sizeof(strat_colors[0]) == NUM_STRATEGIES,
                          "strat_colors[] out of sync with NUM_STRATEGIES");
            // v4.7.16: gate line endpoints follow the live plot bounds
            // (xL/xR captured above) so lines span the full visible chart
            // even after user zoom/pan.
            double gate_lx[2] = {xL, xR};
            // Track drawn label Y pixels so cores with similar gate prices
            // stagger their labels vertically instead of stacking on top
            // of each other. Threshold: 14px. Subsequent labels offset
            // upward by 16px increments to keep them readable.
            float drawn_label_py[16] = {0};
            int drawn_label_n = 0;
            for (int ci = 0; ci < snap->per_node_count && ci < 16; ++ci) {
                // v4.7.13: per-core filter — when filter is set, only
                // show the selected core's gate. ci here is the node_id
                // directly (not a slot), so it's a simple equality check.
                if (settings->node_filter >= 0 && ci != settings->node_filter) continue;
                // skip cores that have an active position — their gate
                // is irrelevant while holding, and the line overlaps
                // with the position's TP/SL labels
                if (ci < 16 && snap->positions[ci].idx >= 0) continue;
                double bg_price = snap->per_node[ci].buy_gate_price;
                if (bg_price <= 0.0) continue;
                uint8_t sid = snap->per_node[ci].strategy_id_display;
                ImVec4 col = (sid < NUM_STRATEGIES) ? strat_colors[sid]
                                        : ImVec4(0.5f, 0.5f, 0.5f, 0.4f);
                const char *sname = (sid < NUM_STRATEGIES) ? STRATEGY_SHORT_NAMES[sid] : "?";
                double ly[2] = {bg_price, bg_price};
                char lbl[32]; snprintf(lbl, 32, "##bg%d", ci);
                ImPlotSpec bgs; bgs.LineColor = col; bgs.LineWeight = 1.0f;
                ImPlot::PlotLine(lbl, gate_lx, ly, 2, bgs);
                // label on the left edge to avoid overlapping TP/SL labels.
                // Stagger Y offset when gate price is within 14px of an
                // already-drawn label so multiple cores' labels don't
                // collide. Each collision pushes 16px further up.
                float label_py = ImPlot::PlotToPixels(xL, bg_price).y;
                int collisions = 0;
                for (int j = 0; j < drawn_label_n; j++) {
                    if (fabsf(drawn_label_py[j] - label_py) < 14.0f) collisions++;
                }
                drawn_label_py[drawn_label_n++] = label_py;
                ImPlot::Annotation(xL, bg_price, col,
                                    ImVec2(40, -5 - collisions * 16), false,
                                    "C%d %s", ci, sname);
            }
        }

        ImPlot::EndPlot();
    }
    ImPlot::GetInputMap() = prev_input_map;  // v4.7.18: restore pan/select bindings
    ImPlot::PopStyleColor(2);  // PlotBg + AxisGrid
    ImGui::End();
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[GUI_PriceChart]
//======================================================================

//======================================================================
// [FUNCTION]_[GUI_VolumeChart]
//----------------------------------------------------------------------
// [TAG]_[[GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[render the Volume chart window — per-candle buy/sell volume bars]
//======================================================================
//======================================================================
// [CODE]
//======================================================================
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
        // v4.7.16: live plot bounds for the volume chart's full-width
        // horizontals (gate line, crosshair). See price-chart counterpart
        // for rationale — cs->x_lo/cs->x_hi go stale on user pan/zoom.
        ImPlotRect _vplim = ImPlot::GetPlotLimits();
        const double vxL = _vplim.X.Min;
        const double vxR = _vplim.X.Max;
        float chart_w = ImPlot::GetPlotSize().x;
        float bw = (chart_w / vis) * 0.35f;
        if (bw < 1.5f) bw = 1.5f;
        for (int i = 0; i < vc; i++) {
            // Color volume bars by candle direction: green when close > open
            // (bullish candle), red when close < open (bearish drop). Matches
            // standard market-volume convention on tradingview-style charts.
            // Tiny flat candles (|delta| < FLICKER_BPS × open) treated as
            // neutral/green to avoid flicker on dust moves.
            double delta = cs->closes[i] - cs->opens[i];
            double thr   = cs->opens[i] * 0.00005;  // 0.5 bps flicker floor
            ImVec4 col = (delta < -thr) ? FoxmlColors::red_b : FoxmlColors::green;
            col.w = 0.7f;
            ImVec2 top = ImPlot::PlotToPixels(cs->xs[i], cs->volumes[i]);
            ImVec2 bot = ImPlot::PlotToPixels(cs->xs[i], 0.0);
            dl->AddRectFilled(ImVec2(top.x - bw, top.y), ImVec2(top.x + bw, bot.y),
                              ImGui::GetColorU32(col));
        }
        // volume gate threshold — cyan dot-dash, matching price chart gate style
        if (snap->buy_v > 0.0001) {
            ImVec2 left  = ImPlot::PlotToPixels(vxL, snap->buy_v);
            ImVec2 right = ImPlot::PlotToPixels(vxR, snap->buy_v);
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
            ImPlot::Annotation(vxL, snap->buy_v,
                               {FoxmlColors::cyan.x, FoxmlColors::cyan.y, FoxmlColors::cyan.z, 1.0f},
                               ImVec2(35, 0), true, "VOL GATE");
        }

        ImPlot::PopPlotClipRect();

        // hover crosshair + volume readout
        if (ImPlot::IsPlotHovered()) {
            ImPlotPoint mouse = ImPlot::GetPlotMousePos();
            ImVec2 ch_l = ImPlot::PlotToPixels(vxL, mouse.y);
            ImVec2 ch_r = ImPlot::PlotToPixels(vxR, mouse.y);
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
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[GUI_VolumeChart]
//======================================================================

//======================================================================
// [FUNCTION]_[GUI_LivePnLChart]
//----------------------------------------------------------------------
// [TAG]_[[GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[render the Live P&L chart — streaming from the snapshot's pnl_history ring buffer]
//======================================================================
//======================================================================
// [CODE]
//======================================================================
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
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[GUI_LivePnLChart]
//======================================================================

//======================================================================
// [FUNCTION]_[GUI_EquityChart]
//----------------------------------------------------------------------
// [TAG]_[[GUI]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[render the Equity Curve chart from the trade-data cumulative P&L]
//======================================================================
//======================================================================
// [CODE]
//======================================================================
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
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[GUI_EquityChart]
//======================================================================
