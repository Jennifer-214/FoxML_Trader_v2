# Integration Contracts

Step-by-step recipes for adding common things to the codebase. **Read this file when wiring a new cfg field, GUI panel, strategy, ML feature, etc.**

## New RegimeDetector signal
1. Add field to `RegimeSignals<F>`
2. Populate in `Regime_ComputeSignals()`
3. Use in `Regime_Classify()` scoring

## New config field
1. `ControllerConfig.hpp`: struct + default + parser macro (`CFG_PARSE_FPN`, `CFG_PARSE_PCT` for 15.0→0.15, `CFG_PARSE_FPN_POS`, `CFG_PARSE_U32`, `CFG_PARSE_INT`)
2. `engine.cfg` and `backtest.cfg`: add with comment
3. Hot-reload is automatic. Protect a field by save/restore in HotReload.

## New GUI-editable config field
4. `GUI/SettingsPanel.hpp`: one line in `field_defs[]` (loading/UI/saving automatic)
5. Add tooltip in `SetItemTooltip` section (mandatory)
6. New strategy → update `default_strategy` tooltip

## ImGui Label IDs (load-bearing — v4.7.33)
ImGui derives widget IDs from visible labels. Two widgets with the same label at the same scope = ID collision = wrong checkmark state, hover errors, duplicate-icon warnings. Three rules:
1. **Widgets inside a loop** (per-core rows, per-position buttons) → `ImGui::PushID(i)` per iteration. Per-core P&L "Reset" buttons + Positions "Close" buttons follow this pattern.
2. **Widget label matching enclosing CollapsingHeader / parent scope** → append `##suffix` to the widget label. ImGui hides everything after `##` in display but uses the full string in the ID hash. E.g. `"Vol Sizing##bool"` displays "Vol Sizing" but doesn't collide with the "Vol Sizing" section header.
3. **Single-instance widgets with unique labels** → label as-is is fine.

Adding a new GUI-editable cfg field whose label matches its section name? Use `##bool` / `##toggle` / `##unique` suffix.

## New per-core override field (v4.7.24+)
1. Add ONE line to `PER_CORE_OVERRIDE_FIELDS(PCT, RAW)` X-macro in `CoreFrameworks/ControllerConfig.hpp` — choose `PCT(name)` for percent-stored fields (cfg writes 4.0, stored 0.04) or `RAW(name)` for direct FPN
2. Add corresponding `per_core_fields[]` entry in `GUI/SettingsPanel.hpp` for the per-core tab
3. **Verify the consumer** reads via `ControllerConfig_ResolveForCore` OR uses the `if (!FPN_IsZero(ov.X)) ov.X else cfg.X` pattern — direct `cfg.X` reads bypass the override (silent no-op). Both live (`EngineSharded.hpp`) and backtest (`BacktestSharded.hpp`) sites must be fixed in lockstep for train-serve parity.

The X-macro auto-generates: struct member, init zeroing, resolver overwrite, cfg parser case. Single-source-of-truth list for per-core overrides.

## New TUI/GUI display field
1. Add to `TUISnapshot` struct (`DataStream/EngineTUI.hpp`)
2. Populate in `TUI_CopySnapshot()`
3. Display in `GUI_Panel_*` (DashboardPanels.hpp)

Backtest auto-syncs via `BacktestSnapshot_Copy`.

## New chart overlay
1. `GUI/ChartPanel.hpp`: add to `ChartState` if needed, render in `GUI_PriceChart()`
2. Engine-sourced data → also update `TUISnapshot` + `TUI_CopySnapshot()`

## New strategy
1. `StrategyInterface.hpp`: `#define STRATEGY_YOUR_NAME N` (append-only)
2. `Strategies/YourName.hpp`: state struct + `_Init` + `_BuildParameters`
3. `StrategyParameters.hpp`: dispatch case
4. `RegimeDetector.hpp`: `Regime_ToStrategy` mapping
5. `tests/controller_test.cpp`: regression tests

**Sharded gate direction:** `BG_Evaluate` defaults to buy-below. Buy-above (momentum, breakout) MUST set `out->flags |= GATE_FLAG_BUY_ABOVE` in `_BuildParameters`. Otherwise core silently buys dips while GUI claims momentum.

**Partial exits:** strategies write only leg-A (`tp_pct`, `sl_pct`). Dispatcher post-cap sets `GATE_FLAG_PAIR_ACTIVE` + `tp_pct_b = tp_pct * cfg.tp2_mult` when enabled, clears both when disabled. Don't write `tp_pct_b` or `GATE_FLAG_PAIR_ACTIVE` from a strategy.

## Bumping version
1. `Version.hpp`: `ENGINE_VERSION_STRING`
2. `DOCS/CHANGELOG.md`: summary table
3. `DOCS/changelogs/YYYY-MM-DD-X.md`: dated changelog
4. `git tag vX.Y.Z && git push origin vX.Y.Z`

Patch (Z) = bug/cfg/TUI; Minor (Y) = features/strategies; Major (X) = architectural rewrites.

## Public release (FoxML_Trader)
Public repo `Jennyfirrr/FoxML_Trader` uses its own v1.0.x. **Uses legacy architecture — not 1:1 with sharded.** Copy files, push, tag `v1.0.N`, `gh release create` with `<3` in title. NEVER push `engine.cfg`, `controller.cfg`, `.env`, API keys, or `plans/`.

## New ML feature
1. `ModelInference.hpp`: `FEAT_NEW_NAME` at current `MODEL_NUM_FEATURES`
2. Increment `MODEL_NUM_FEATURES` + `MODEL_FORMAT_VERSION`
3. Pack line in `ModelFeatures_Pack()`
4. Add to `RegimeSignals<F>` + populate in `Regime_ComputeSignals` (single site)
5. New state? Add to BOTH `EngineSharded_Run` AND `BacktestSharded_Run` with parity in update cadence
6. Retrain all models (old fail version check)

`FEAT_*` constants are **append-only** — never reorder, never remove.

## foxml_suite parity
Same repo, same headers. Both targets must compile clean: `cmake --build build && cmake --build build_suite`.

## Centralized constants
- `Version.hpp`: `ENGINE_VERSION_STRING`
- `Limits.hpp`: `MAX_PORTFOLIO_POSITIONS`, `CANDLE_HISTORY_MAX`
