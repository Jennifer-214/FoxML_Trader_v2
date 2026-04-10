# 2026-04-09 — Sharded Engine GUI Wiring + Per-Strategy Config

scope: experimental branch `experiment/per-core-sharding`. wires the
per-core sharded engine into the existing ImGui GUI so all existing panels
(dashboard, chart, settings) display sharded mode data. also adds
per-strategy TP/SL config overrides so different strategies on different
cores can be independently tuned.

## Per-Strategy Config Overrides

the shared ControllerConfig had one `take_profit_pct` / `stop_loss_pct`
that all strategies read. with different strategies on different cores
(core 0 = SimpleDip, core 1 = Momentum), they all got the same TP/SL.

fix: per-strategy override fields that fall back to the shared value
when set to 0:

- `simpledip_tp_pct` / `simpledip_sl_pct` (percentage)
- `mr_tp_pct` / `mr_sl_pct` (percentage)
- `emacross_tp_pct` / `emacross_sl_pct` (percentage)
- momentum already had `momentum_tp_mult` / `momentum_sl_mult` (stddev mults)

each strategy's `_BuildParameters` now reads its own field first, falls
back to shared if zero. no behavior change for existing configs.

## Sharded Snapshot Adapter

new `CoreFrameworks/ShardedSnapshot.hpp`:
- `TUI_CopySnapshotSharded()` maps `EventLoopState` + `EventLoopAggregates`
  + `RollingStats` into `TUISnapshot` — the same contract all GUI panels read.
- one function, one site to update when adding display fields.
- populates: market data, account (balance/equity/PnL/drawdown), per-position
  details (entry/qty/TP/SL), per-core strategy assignment + buy gate price,
  rolling stats, config display, counters.

## GUI Wiring

`CoreFrameworks/EngineSharded.hpp`:
- `#ifdef USE_IMGUI_GUI`: spawns `gui_thread_fn` with `TUISharedState`
  double-buffered snapshots (same pattern as legacy engine in main.cpp)
- producer thread feeds `CandleAccumulator_Push` from every tick
- slow-path cadence populates `TUISnapshot` via `TUI_CopySnapshotSharded`
  + `TUI_PopulatePerCoreLatency`, atomic flip
- ANSI TUI render loop wrapped in `#ifndef USE_IMGUI_GUI`
- GUI `quit_requested` checked for clean shutdown

## Chart Overlays

`GUI/ChartPanel.hpp`:
- per-core buy gate horizontal lines in sharded mode
- color-coded by strategy: MR=blue, MOM=orange, DIP=green, ML=purple, EMA=cyan
- annotated with "C0 DIP", "C1 MOM" etc at right edge
- buy gate price read from `ParameterSlot_Read` on each core

## Settings Panel

`GUI/SettingsPanel.hpp`:
- "Core Strategies" section: `core_0_strategy` through `core_3_strategy`
  (hot-swappable, tooltip shows strategy ID mapping)
- "SimpleDip Tuning": `simpledip_tp_pct`, `simpledip_sl_pct`
- "MeanReversion Tuning": `mr_tp_pct`, `mr_sl_pct`
- "Momentum Tuning": `momentum_tp_mult`, `momentum_sl_mult`, `momentum_breakout_mult`
- "EMA Cross Tuning": `emacross_tp_pct`, `emacross_sl_pct`, `emacross_dip_mult`, `emacross_crossover_min`

all auto-loaded/saved/rendered by the existing `field_defs[]` machinery.

## Build

both targets build clean:
```bash
cmake -B build && cmake --build build           # ANSI TUI (no GUI deps)
cmake -B build_gui -DUSE_IMGUI_GUI=ON && cmake --build build_gui  # ImGui GUI
```

## Verification

- `cmake --build build` — engine + controller_test clean
- `cmake --build build_gui` — engine_gui + foxml_suite clean
- controller_test: 279/279
- all OMS tests: 9/9 + 27/27 + 31/31

## Rollback tags

- `pre-gui-wiring` — before this session's GUI work
- `pre-oms-phase-04-06` — before OMS phases 04-06
- `pre-oms-audit` — before audit fixes
- `pre-oms` — before all OMS work
