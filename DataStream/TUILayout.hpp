// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [TUI LAYOUTS]
//======================================================================================================
// preset layout compositions using the widgets from TUIWidgets.hpp
// cycle with 'l' key: Standard → Positions Focus → Compact → Standard
//======================================================================================================
#ifndef TUI_LAYOUT_HPP
#define TUI_LAYOUT_HPP

#include <ftxui/dom/elements.hpp>
#include "TUIWidgets.hpp"

using namespace ftxui;

#define LAYOUT_STANDARD  0
#define LAYOUT_POSITIONS 1
#define LAYOUT_COMPACT   2
#define LAYOUT_COUNT     3

//======================================================================================================
// [LAYOUT 1: STANDARD]
//======================================================================================================
// classic two-column: left = stats/config, right = positions + graphs
//======================================================================================================
static inline Element Layout_Standard(const TUISnapshot *s, int term_w, int term_h) {
    Elements left_col;
    left_col.push_back(Widget_Price(s));
    left_col.push_back(separator() | color(foxml::surf));
    left_col.push_back(Widget_Market(s));
    left_col.push_back(separator() | color(foxml::surf));
    left_col.push_back(Widget_BuyGate(s));
    left_col.push_back(separator() | color(foxml::surf));
    left_col.push_back(Widget_Portfolio(s));
    left_col.push_back(separator() | color(foxml::surf));
    left_col.push_back(Widget_PnL(s));
    left_col.push_back(separator() | color(foxml::surf));
    left_col.push_back(Widget_Risk(s));
    left_col.push_back(separator() | color(foxml::surf));
    left_col.push_back(Widget_Stats(s));
    left_col.push_back(separator() | color(foxml::surf));
    left_col.push_back(Widget_Config(s));
#ifdef LATENCY_PROFILING
    left_col.push_back(separator() | color(foxml::surf));
    left_col.push_back(Widget_Latency(s));
#endif

    Elements right_col;
    right_col.push_back(Widget_Positions(s));
    right_col.push_back(separator() | color(foxml::surf));
    right_col.push_back(Widget_PriceGraph(s, term_w / 2, term_h));
    right_col.push_back(separator() | color(foxml::surf));
    right_col.push_back(Widget_PnLGraph(s, term_w / 2, term_h));

    return vbox({
        Widget_Header(s),
        hbox({
            vbox(left_col) | flex,
            separator() | color(foxml::surf),
            vbox(right_col) | flex,
        }) | flex,
        separator() | color(foxml::surf),
        Widget_Controls(),
    });
}

//======================================================================================================
// [LAYOUT 2: POSITIONS FOCUS]
//======================================================================================================
// positions take the left (larger), stats on right
//======================================================================================================
static inline Element Layout_PositionsFocus(const TUISnapshot *s, int term_w, int term_h) {
    Elements right_col;
    right_col.push_back(Widget_Price(s));
    right_col.push_back(separator() | color(foxml::surf));
    right_col.push_back(Widget_Market(s));
    right_col.push_back(separator() | color(foxml::surf));
    right_col.push_back(Widget_BuyGate(s));
    right_col.push_back(separator() | color(foxml::surf));
    right_col.push_back(Widget_Portfolio(s));
    right_col.push_back(separator() | color(foxml::surf));
    right_col.push_back(Widget_PnL(s));
    right_col.push_back(separator() | color(foxml::surf));
    right_col.push_back(Widget_Risk(s));
    right_col.push_back(separator() | color(foxml::surf));
    right_col.push_back(Widget_Stats(s));

    Elements left_col;
    left_col.push_back(Widget_Positions(s));
    left_col.push_back(separator() | color(foxml::surf));
    left_col.push_back(Widget_PriceGraph(s, term_w * 3 / 5, term_h));
    left_col.push_back(separator() | color(foxml::surf));
    left_col.push_back(Widget_PnLGraph(s, term_w * 3 / 5, term_h));
#ifdef LATENCY_PROFILING
    left_col.push_back(separator() | color(foxml::surf));
    left_col.push_back(Widget_Latency(s));
#endif

    return vbox({
        Widget_Header(s),
        hbox({
            vbox(left_col) | flex_grow,
            separator() | color(foxml::surf),
            vbox(right_col) | flex,
        }) | flex,
        separator() | color(foxml::surf),
        Widget_Controls(),
    });
}

//======================================================================================================
// [LAYOUT 3: COMPACT]
//======================================================================================================
// minimal — key numbers at top, positions below, graphs at bottom
//======================================================================================================
static inline Element Layout_Compact(const TUISnapshot *s, int term_w, int term_h) {
    // top bar: key metrics
    Color pnl_c = s->total_pnl >= 0 ? foxml::green : foxml::red;
    const char *regime = (s->current_regime == 1) ? "TRENDING" :
                         (s->current_regime == 2) ? "VOLATILE" : "RANGING";
    Color regime_color = (s->current_regime == 1) ? foxml::green :
                         (s->current_regime == 2) ? foxml::red : foxml::dim;

    auto top_bar = hbox({
        vbox({
            text("PRICE") | color(foxml::sand),
            text(fmt("$%.2f", s->price)) | bold | color(foxml::wheat),
        }),
        separator() | color(foxml::surf),
        vbox({
            text("P&L") | color(foxml::sand),
            text(fmt("$%+.2f", s->total_pnl)) | bold | color(pnl_c),
        }),
        separator() | color(foxml::surf),
        vbox({
            text("REGIME") | color(foxml::sand),
            text(regime) | bold | color(regime_color),
        }),
        separator() | color(foxml::surf),
        vbox({
            text(fmt_i("POS %d/16", s->active_count)) | color(foxml::sand),
            text(fmt("EQ $%.0f", s->equity)) | color(foxml::fg),
        }),
        filler(),
    }) | borderLight;

    // positions list (compact — one line per position)
    Elements pos_rows;
    int displayed = 0;
    for (int i = 0; i < 16; i++) {
        const TUIPositionSnap *ps = &s->positions[i];
        if (ps->idx < 0) continue;
        double diff = s->price - ps->entry;
        Color dc = diff >= 0 ? foxml::green : foxml::red;

        pos_rows.push_back(hbox({
            text(fmt_i("#%d ", displayed)) | color(foxml::wheat),
            text(fmt("$%.0f", ps->entry)) | color(foxml::fg),
            text("→") | color(foxml::dim),
            text(fmt("$%.0f", s->price)) | color(foxml::wheat),
            text(fmt(" %+.0f", diff)) | color(dc),
            text(fmt("  TP:$%.0f", ps->tp)) | color(foxml::green),
            text(fmt(" SL:$%.0f", ps->sl)) | color(foxml::red),
            text(fmt(" %.0fm", ps->hold_minutes)) | color(foxml::dim),
        }));
        displayed++;
    }
    if (displayed == 0)
        pos_rows.push_back(text("  (no positions)") | color(foxml::dim));

    return vbox({
        top_bar,
        vbox(pos_rows) | frame | flex,
        separator() | color(foxml::surf),
        hbox({
            Widget_PriceGraph(s, term_w / 2, term_h) | flex,
            separator() | color(foxml::surf),
            Widget_PnLGraph(s, term_w / 2, term_h) | flex,
        }),
        separator() | color(foxml::surf),
        Widget_Controls(),
    });
}

//======================================================================================================
// [LAYOUT DISPATCH]
//======================================================================================================
static inline Element Layout_Render(const TUISnapshot *s, int layout_id, int term_w, int term_h) {
    switch (layout_id) {
        case LAYOUT_POSITIONS: return Layout_PositionsFocus(s, term_w, term_h);
        case LAYOUT_COMPACT:   return Layout_Compact(s, term_w, term_h);
        default:               return Layout_Standard(s, term_w, term_h);
    }
}

#endif // TUI_LAYOUT_HPP
