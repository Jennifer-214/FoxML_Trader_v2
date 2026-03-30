# v3.4.0 — P&L Audit Fix + EMA/SMA Crossover Regime Detection (2026-03-30)

## Fixed

### P&L Tracking Audit (5 bugs)
- **TradeReader equity curve was fee-blind** — computed P&L as `(price - entry) * qty` with zero fee deduction. Now reads `fee_cost` from CSV column 14, pairs entry/exit fees FIFO, subtracts both. This was the main discrepancy (-$14 engine vs -$8 GUI).
- **Two conflicting total_pnl formulas** — direct TUI used `realized + unrealized` (double-counted entry fees on open positions), snapshot TUI used `equity - starting` (correct). Unified both to `equity - starting_balance`.
- **starting_balance not snapshot-persisted** — came from engine.cfg on each restart. If config changed between sessions, return % broke. Now saved in snapshot v8.
- **gross_losses corrupted by time exits** — unconditionally negated `pos_pnl` into `gross_losses`. If a time exit was a small winner, this added a negative value. Now uses same branchless masking as DrainExits.
- **Entry fee reconstructed at exit time** — used current `fee_rate` to compute `entry_fee_recon`, which broke if fee_rate was hot-reloaded between entry and exit. Now stores actual `entry_fee` on Position struct at fill time.

### Snapshot v8
- New field: `starting_balance` persisted (immune to config edits between sessions)
- Position struct gained `entry_fee` field (backward compat: zero-init for old snapshots)
- Old v7 snapshots load fine (missing fields get defaults)

## Changed

### EMA/SMA Crossover Regime Detection
- **Replaced regression slope signals with EMA/SMA crossover** in `Regime_Classify`
  - Old: `abs(short_slope) > threshold` and `abs(long_slope) > threshold` (least-squares regression, lags by 128+ samples)
  - New: `abs(ema_sma_spread) > crossover_threshold` (EMA reacts every tick vs SMA lagging by ~12,800 effective ticks)
  - EMA/SMA spread = `(ema - sma) / sma`, normalized and asset-independent
- **Added 3 fields to RegimeSignals**: `ema_sma_spread`, `ema_sma_spread_long`, `ema_above_sma`
- **New config**: `regime_crossover_threshold=0.0005` (0.05% EMA-SMA gap = trending, ~$35 at BTC $70k)
- **Kept unchanged**: R² consistency, ROR acceleration, volume confirmation, volatile scoring (vol_ratio), hysteresis, regime constants, Regime_ToStrategy mapping, Regime_AdjustPositions
- **TUI display**: new `ema/sma: +0.0123% ↗` line in regime section
- `regime_slope_threshold` still parses but no longer affects classification (backward compat)

## Added
- **Settings tooltips** — hover any non-obvious field in the ImGui Settings panel for a description
  - Strategy selector: shows all options (-1 through 2) with names
  - Entry filters: offset, volume mult, spacing, stddev mult explained
  - Trailing TP/SL: hold score, trail distances
  - Momentum: breakout, TP/SL multipliers
  - Risk: max drawdown circuit breaker explanation
  - EMA: alpha value guide (fast/default/slow)

## Files Modified
- `GUI/SettingsPanel.hpp` — hover tooltips for all non-obvious config fields
- `GUI/TradeReader.hpp` — fee-aware equity curve (entry/exit fee pairing)
- `CoreFrameworks/Portfolio.hpp` — `entry_fee` field on Position struct
- `CoreFrameworks/PortfolioController.hpp` — store entry_fee at fill, use at exit, snapshot v8, gross_losses fix
- `DataStream/EngineTUI.hpp` — unified total_pnl formula, ema_sma_spread snapshot field
- `DataStream/TUIAnsi.hpp` — EMA/SMA spread display line
- `Strategies/RegimeDetector.hpp` — crossover signals, compute, classify
- `CoreFrameworks/ControllerConfig.hpp` — regime_crossover_threshold
- `engine.cfg` — new crossover threshold
- `Version.hpp` — 3.3.0 → 3.4.0
