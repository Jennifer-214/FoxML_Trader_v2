# FoxML Suite Code Key

**Read this file when working on the backtest suite (`foxml_suite` target) — Run Control, Training, Walk-Forward, Held-Out, Comparison panels, or anything in `Backtest/`.**

## Data flow Backtest → GUI

```
BacktestEngine.hpp (Backtest_Run → BacktestSharded_Run)
  → ShardedBacktestDriver (slow-path callbacks, feature collection)
  → BacktestResults (stats + ML features)
  → BacktestSnapshot_Copy() (state → TUISnapshot)
  → TradeLog CSV (logging/BACKTEST_order_history.csv)
```

## GUI panels

- Data, Run Control, Results, Training, Comparison: `BacktestPanels.hpp`
- Trade History: `TradeHistoryPanel.hpp` (reads CSV)
- Market/Account/Stats/Positions/Buy Gate: `DashboardPanels.hpp` (reads `TUISnapshot`)
- Chart: `ChartPanel.hpp` (CandleAccumulator)
- Settings: `SettingsPanel.hpp` (`backtest.cfg`)

## Live vs backtest

Live engine = `engine.cfg`. Backtest suite = `backtest.cfg`. `default_strategy`: -2 = full 4-strat auto, -1 = legacy 2-strat, 0-4 = fixed.

## Trade log format

`TradeLog_Init(&log, "SYMBOL")` → `logging/SYMBOL_order_history.csv`.

Format v3: `timestamp_us,core_id,strategy_id,event_type(E|X),event_price,entry_price,exit_price,pnl,fees,balance_after,trade_size`.

With partials, `core_id` in CSV is portfolio SLOT (slot c → core c/2, leg c%2).

## Dynamic Sizing (Backtest Suite ONLY)

Backtest buffers MUST NOT use compile-time caps that silently truncate. Use dynamic alloc + growth:
- Sample buffers: start `BACKTEST_SAMPLES_INIT`, grow via `BacktestResults_EnsureCapacity()` (2× realloc)
- Equity curve: start `BACKTEST_EQUITY_INIT`, grow via `BacktestResults_EnsureEquityCapacity()`
- Tick buffers: sized from first-pass line count
- `Init`/`Reset`/`Free`: call appropriate helpers

When adding new heap-allocated `BacktestResults` field, update **all four** sites:
1. `_Init` — malloc + set `field_capacity = INIT_CAP`
2. `_Reset` — save pointer + capacity, restore after `memset(0)`
3. `_Free` — free + NULL pointer, zero capacity
4. `_EnsureCapacity` — defensive `cap > 0 ? cap*2 : INIT_CAP` floor (never `0 *= 2`)

`_Reset` exists so "which fields are dynamic" lives in one place.

**Live engine is the opposite** — zero dynamic alloc on hot path. All live buffers fixed-size, pre-allocated. No malloc/realloc/syscalls in tick loop. Hard rule.
