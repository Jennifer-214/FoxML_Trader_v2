// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [BACKTEST SNAPSHOT]
//======================================================================================================
// thin wrapper around TUI_CopySnapshot — single implementation, no manual sync.
// adding a field to TUISnapshot? update TUI_CopySnapshot ONLY. backtest gets it free.
//
// the only backtest-specific override: live_trading = 0 (always paper in backtest).
//======================================================================================================
#ifndef BACKTEST_SNAPSHOT_HPP
#define BACKTEST_SNAPSHOT_HPP

#include "../DataStream/EngineTUI.hpp"
#include "../CoreFrameworks/PortfolioController.hpp"

template <unsigned F>
static inline void BacktestSnapshot_Copy(TUISnapshot *snap,
                                          const PortfolioController<F> *ctrl,
                                          double price, double volume) {
    // single implementation — no manual field sync needed
    TUI_CopySnapshot(snap, ctrl, price, volume);

    // backtest-specific overrides
    snap->live_trading = 0; // always paper in backtest
}

#endif // BACKTEST_SNAPSHOT_HPP
