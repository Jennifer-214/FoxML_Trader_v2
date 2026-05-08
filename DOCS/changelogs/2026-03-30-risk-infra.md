# v3.5.0 — Risk Infrastructure + ML Harness Design (2026-03-30)

## Added

### Sticky Kill Switch
- **Daily loss limit**: halt all buying if session equity drops below configured % of session start (default 3%)
- **Drawdown limit**: halt if drawdown from intra-session peak exceeds configured % (default 5%)
- **Sticky behavior**: once triggered, stays active until 24h session reset or manual TUI `k` key
- **Post-kill warmup**: configurable observation period after kill reset before trading resumes (default 50 cycles)
- **Crash-proof**: kill switch state, session_start_equity, and peak_equity persist in snapshot v9
- Config: `kill_switch_enabled`, `kill_switch_daily_loss_pct`, `kill_switch_drawdown_pct`, `kill_recovery_warmup`
- TUI: `[KILL: DAILY LOSS]` or `[KILL: DRAWDOWN]` in red, `[RECOVERING (N)]` during warmup

### Vol-Scaled Position Sizing
- Scale position quantity inversely with volatility: `qty *= baseline_stddev / current_stddev`
- Uses 512-tick long-window stddev as self-calibrating baseline (no hardcoded "normal vol")
- Clamped to configurable bounds (default 0.25x-2.0x) to prevent extreme sizing
- Config: `vol_sizing_enabled`, `vol_scale_min`, `vol_scale_max`

### No-Trade Band (Cost-Aware Signal Gate)
- Suppress entries when signal strength is below fee breakeven: `|signal| < fee_rate * mult`
- Prevents churn in flat markets where round-trip fees exceed expected gain
- Applied after strategy buy signal, before all other gates
- Config: `no_trade_band_enabled`, `no_trade_band_mult`

### Per-Strategy Reward Attribution
- `StrategyStats[4]` accumulates realized P&L, wins, losses, trade count per strategy
- Fed in DrainExits from existing `entry_strategy[]` array (no new tracking overhead)
- TUI displays per-strategy breakdown: `MR: +$12.40 (3W/1L 75%) | Dip: +$8.20 (5W/2L 71%)`
- Persisted in snapshot v9 for crash recovery

### Welford Online Stats Tracker
- New `ML_Headers/WelfordStats.hpp`: O(1) incremental mean/variance computation
- Functions: `Welford_Init`, `Push`, `Variance`, `Stddev`, `ZScore`, `Reset`
- Numerically stable for unbounded streams (Welford's algorithm)
- Integrated: `pnl_tracker` (fed on every exit), `signal_tracker` (fed on every buy signal)
- Ready for future model prediction normalization

### ML Inference Harness Design Doc
- `plans/ml-inference-harness.md`: complete design for XGBoost/LightGBM integration
- Mode A: regime signal enrichment (model_score in RegimeSignals)
- Mode B: full STRATEGY_ML (id=3) with 4-function pattern
- Transferable patterns from FoxML LIVE_TRADING: confidence scoring, barrier gates, Exp3-IX bandit, ridge ensemble, vol-scaled sizing, cost-aware arbitration

### Complete GUI Settings Panel
- Added ~45 missing config fields across 10 new sections:
  - **Regime Detection**: crossover threshold, R², vol spike ratio, hysteresis
  - **Adaptation**: filter scale, offset/vol bounds, R² threshold, max shift
  - **Time-Based Exit**: max hold ticks, min gain %
  - **Gate Recovery**: idle reset cycles, SL cooldown
  - **Partial Exits**: split %, TP2 mult, breakeven toggle
  - **Session Filters**: Asian/EU/US/overnight multipliers
  - **Entry Filters**: min stddev, long slope, buy delta, VWAP offset
  - **Kill Switch / Vol Sizing / No-Trade Band**: all new risk fields
- Hover tooltips for all non-obvious fields

### Regime Auto Display
- When `default_strategy=-1`, TUI/GUI shows `AUTO > MEAN REVERSION` (or current sub-strategy)
- Clear visual distinction between fixed strategy and regime-driven selection

## Fixed

### Regime Auto Hot-Reload
- `default_strategy=-1` was silently ignored by hot-reload (condition `>= 0` filtered it out)
- Now triggers immediate re-evaluation from current regime on hot-reload or GUI change

### Session Equity Persistence
- `session_start_equity` and `peak_equity` now persist in snapshot v9
- Previously reset to current equity on restart, making disconnect losses invisible to kill switch
- Kill switch now correctly detects losses accumulated during disconnects

### Snapshot v9
- New fields: `kill_switch_active`, `kill_reason`, `daily_realized_pnl`, `session_start_equity`, `peak_equity`, `strategy_stats[4]`
- Backward compatible: v4-v8 snapshots load fine (new fields get defaults)

## Files Modified
- `CoreFrameworks/ControllerConfig.hpp` — 10 new config fields (kill switch, vol sizing, no-trade band)
- `CoreFrameworks/PortfolioController.hpp` — kill switch logic, vol sizing at fill, no-trade band gate, strategy stats in DrainExits, Welford integration, snapshot v9, regime auto hot-reload fix
- `ML_Headers/WelfordStats.hpp` — **NEW** (Welford online tracker)
- `DataStream/EngineTUI.hpp` — snapshot fields (kill, vol_scale, signal_strength, strat_stats, regime_auto), multicore kill reset signal
- `DataStream/TUIAnsi.hpp` — kill switch display, per-strategy stats, AUTO > strategy label, [k]ill reset in controls
- `GUI/DashboardPanels.hpp` — AUTO > strategy display
- `GUI/SettingsPanel.hpp` — 45+ new field_defs entries with tooltips
- `engine.cfg` — risk infrastructure config section
- `main.cpp` — multicore kill reset signal handler
- `Version.hpp` — 3.4.0 -> 3.5.0
- `plans/ml-inference-harness.md` — **NEW** (ML design doc)
