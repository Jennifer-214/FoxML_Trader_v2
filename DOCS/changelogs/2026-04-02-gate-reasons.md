# 2026-04-02 — v3.6.0: Dashboard Polish + Gate Reasons + Structured Logging

## GUI Dashboard Polish
- Panel reorganization: 10 panels → 7 (merged Portfolio+P&L+Risk → Account, Market Structure+Regime → Market)
- Removed Config panel (duplicated by Settings tab) and W/L from TopBar (duplicated by Stats)
- Position progress bar: colored SL→TP bar per open position (red near SL, yellow middle, green near TP)
- Danger meter: gradient bar in Buy Gate panel when crash protection activates (green → yellow → red)
- Live P&L chart: streaming implot chart from pnl_history ring buffer, docked alongside Volume
- Live entry markers: green dot on chart at exact entry candle from position timestamps (no CSV drift)
- Gross/net P&L in Account panel with color coding

## Gate Reason Codes
- 13 `GATE_REASON_*` constants covering every path that zeros the buy gate
- Displayed in 3 locations: status bar `PAUSED (no_signal)`, buy gate `GATE OFF (cooldown)`, detailed banner
- Codes: ok, warmup, no_signal, no_trade, book, danger, kill, recovery, volatile, cooldown, wind_down, paused, downtrend
- Kill and danger reasons render in red, everything else in yellow
- Both ANSI TUI and ImGui GUI updated

## Structured Logging
- `[SESSION]` warmup complete with sample count, strategy, price
- `[TRADE]` BUY/SELL with price, qty, value, strategy, P&L, balance
- `[REGIME]` transitions (RANGING -> TRENDING, etc.)
- `[GATE]` state changes (ok -> no_signal, cooldown -> ok, etc.)
- All slow-path only — zero hot-path latency impact

## Logging Directory
- All runtime files moved to `logging/`: snapshots, trade CSVs, metrics, engine.log
- `rm -rf logging/*` for clean start, `zip session.zip logging/*` for archival
- Directory auto-created on startup
- Added to .gitignore

## Bug Fixes
- Fix balance drift false positive: only check when flat (remaining==0), not with open positions
- Fix hot-path kill fprintf: was passing FPN<64> struct to %.2f (undefined behavior)
- Add kill switch debug dump: prints price, qty, bitmap, per-position values on trigger
- Wire up K key for kill reset in ImGui GUI (was ANSI TUI-only)
- Disable chart trade markers (timestamps don't align with candles, visual clutter)
- Show kill switch limit alongside observed maxDD: `0.07% / 10.0%`

## Tests
- Kill switch regression: small loss must not trip kill (6 assertions)
- Balance drift: round trip zero-drift, equity consistency, Portfolio_ComputeValue accuracy (12 assertions)
- 259 total assertions (was 245)
