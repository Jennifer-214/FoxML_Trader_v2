# CLAUDE.md

## Overview

Tick-level crypto trading engine in C++. Branchless fixed-point arithmetic, bitmap-based portfolio management, regression-driven gate adjustment with regime detection. Single-symbol, single-threaded hot path with multicore TUI.

## Build

CMake with zero-dependency ANSI TUI:
```bash
cmake -B build && cmake --build build         # production (ANSI TUI, no deps)
./build/controller_test                        # run tests (236 assertions)
cd build && ./engine                           # run engine (needs engine.cfg symlink)
```

Build options: `-DLATENCY_PROFILING=ON`, `-DLATENCY_LITE=ON`, `-DLATENCY_BENCH=ON`, `-DBUSY_POLL=ON`, `-DUSE_NATIVE_128=ON`. See README.md for details.

### Dependencies

| Target | Dependencies |
|--------|-------------|
| ANSI TUI (`build/engine`) | None (zero-dependency) |
| ImGui GUI (`build_gui/engine_gui`) | SDL2, OpenGL3 |
| FoxML Suite (`build_suite/foxml_suite`) | SDL2, OpenGL3, XGBoost C library (for ML training) |

**XGBoost C library** (required for `-DUSE_XGBOOST=ON`):
```bash
# build from source (not in most package managers)
git clone --recurse-submodules https://github.com/dmlc/xgboost.git /tmp/xgboost
cd /tmp/xgboost && mkdir build && cd build
cmake .. -DBUILD_STATIC_LIB=OFF && make -j$(nproc)
sudo make install && sudo ldconfig
```

## Architecture

```
HOT PATH (every tick):
  BuyGate (branchless) -> OrderPool
  PositionExitGate (branchless bitmap walk) -> ExitBuffer

EVERY TICK (PortfolioController_Tick):
  - consume new fills from pool into portfolio (branchless consolidation)
  - clear consumed pool slots

EVERY N TICKS (slow path):
  - RollingStats_Push (least-squares regression: slope, R², variance)
  - regime detection: RegimeSignals → score-based Regime_Classify
  - strategy dispatch: MR or Momentum adapt + buy signal
  - drain exit buffer, log sells to CSV
  - push rolling.price_slope to ROR (trend acceleration)
```

## Data Flow: Regime Detection

```
RollingStats (128-tick window):
  price_slope (least-squares), price_r_squared, price_variance
  volume_avg, volume_slope, price_avg, price_stddev (range/4), min, max

RollingStats (512-tick window):
  same fields, broader timeframe

ROR Regressor:
  fed rolling.price_slope each slow-path cycle → slope-of-slopes (acceleration)

        ↓ all feed into ↓

RegimeSignals struct (Strategies/RegimeDetector.hpp):
  short_slope, short_r2, short_variance  (128-tick)
  long_slope, long_r2, long_variance     (512-tick)
  vol_ratio (short/long variance)        (self-adapting volatility)
  ror_slope                              (trend acceleration)
  volume_slope                           (volume confirmation)
  [future: model_score, order_flow]      (extensibility hooks)

        ↓

Regime_Classify (score-based):
  trending_score (5 signals: slope×2, R², acceleration, volume)
  volatile_score (2 signals: variance spike, no direction)
  → RANGING / TRENDING / VOLATILE with hysteresis

        ↓

Strategy dispatch → position adjustment on regime switch
```

## Directory Structure

- **CoreFrameworks/** - OrderGates (buy gate), Portfolio (bitmap positions, exit gate), PortfolioController (feedback loop, regime wiring)
- **Strategies/** - RegimeDetector (RegimeSignals, score-based classify, position adjustment), MeanReversion, Momentum, StrategyInterface
- **DataStream/** - FauxFIX, MockGenerator, TradeLog, BinanceCrypto (websocket), BinanceOrderAPI (REST orders), EngineTUI (snapshot, thread), TUIAnsi (ANSI terminal renderer, zero deps)
- **FixedPoint/** - FPN arbitrary-width fixed-point arithmetic library
- **MemHeaders/** - PoolAllocator (bitmap order pool), BuddyAllocator
- **ML_Headers/** - RollingStats (regression + R²), LinearRegression3X, ROR_regressor (slope-of-slopes), GateControlNetwork
- **GUI/** - Dear ImGui native GUI: FoxmlTheme, DashboardPanels, ChartPanel (price/volume/equity), CandleAccumulator, TradeReader, SettingsPanel, TradeHistoryPanel, LogViewerPanel, GuiThread
- **Backtest/** - BacktestEngine (replay loop), BacktestPanels (suite GUI), LabelFunctions (ML targets)
- **tests/** - controller_test.cpp (279 assertions)
- **plans/** - implementation plans (gitignored)
- **vendor/** - vendored imgui (docking branch) + implot v0.18 (gitignored)
- **Version.hpp** - single source of truth for version string
- **Limits.hpp** - centralized compile-time constants (MAX_POSITIONS, CANDLE_MAX, etc.)

## Integration Contracts

When changing something, here's exactly what to update:

### Adding a new signal to RegimeDetector
1. `Strategies/RegimeDetector.hpp`: add field to `RegimeSignals<F>` struct
2. `Strategies/RegimeDetector.hpp`: populate it in `RegimeSignals_Compute()`
3. `Strategies/RegimeDetector.hpp`: use it in `Regime_Classify()` scoring
4. Done — RegimeSignals is the single extensibility point

### Adding a new config field
1. `CoreFrameworks/ControllerConfig.hpp`: add to struct + default + parser macro
2. `engine.cfg`: add with comment
3. Done — hot-reload is automatic via bulk struct copy

### Adding a GUI-editable config field (above + settings panel)
4. `GUI/SettingsPanel.hpp`: add ONE line to `field_defs[]` array — loading, UI, and saving are automatic
5. `GUI/SettingsPanel.hpp`: add tooltip in the `SetItemTooltip` section (ALWAYS — fields without tooltips are confusing)
6. If adding a new strategy: update the `default_strategy` tooltip to include the new ID and name

### Adding a new TUI/GUI display field
1. `DataStream/EngineTUI.hpp`: add to `TUISnapshot` struct
2. `DataStream/EngineTUI.hpp`: populate in `TUI_CopySnapshot()`
3. `Backtest/BacktestSnapshot.hpp`: populate in `BacktestSnapshot_Copy()` (when it exists)
4. `DataStream/TUIAnsi.hpp`: display in appropriate `ANSI_Section_*` (if ANSI TUI needed)
5. `GUI/DashboardPanels.hpp`: display in appropriate `GUI_Panel_*`

### Adding a new chart overlay
1. `GUI/ChartPanel.hpp`: add to `ChartState` if new data needed, add render call in `GUI_PriceChart()`
2. If data comes from engine: also update `TUISnapshot` + `TUI_CopySnapshot()`

### Adding a new strategy
1. `Strategies/StrategyInterface.hpp`: add `#define STRATEGY_YOUR_NAME N`
2. `Strategies/YourName.hpp`: create with _Init, _Adapt, _BuySignal, _ExitAdjust
3. `CoreFrameworks/PortfolioController.hpp`: add state field + ONE case in `PortfolioController_StrategyDispatch()` (single dispatch — no duplicate switches)
4. `Strategies/RegimeDetector.hpp`: add Regime_ToStrategy mapping + Regime_AdjustPositions cases
5. `tests/controller_test.cpp`: regression tests

### Bumping version
1. `Version.hpp`: update `ENGINE_VERSION_STRING` (one file, one location)
2. `DOCS/CHANGELOG.md`: update version summary table
3. `DOCS/changelogs/`: create dated changelog

### Public release (FoxML_Trader)
Public repo: `Jennyfirrr/FoxML_Trader` (remote: `git@github.com:Jennyfirrr/FoxML_Trader.git`)
Uses its own version numbering (v1.0.x), separate from the internal engine version.

1. Copy changed files from `tick_trader_private` to `~/FoxML_Trader/`
2. Commit + push to `main`
3. Tag: `git tag v1.0.N && git push origin v1.0.N`
4. Create GitHub release: `gh release create v1.0.N --title "v1.0.N — Title <3" --notes "..."`

Rules:
- **Always include `<3` in the release title**
- **NEVER push private configs** — `engine.cfg`, `controller.cfg`, `.env`, API keys, plans/
- Update `CHANGELOG.md` in FoxML_Trader with each release
- Keep release notes short and human — bullet the actual changes
- Patch (Z): bug fixes, visual tweaks, overlaps, spacing
- Minor (Y): new features, new panels, new chart overlays

### Adding a new ML feature
1. `ML_Headers/ModelInference.hpp`: add `FEAT_NEW_NAME` at current MODEL_NUM_FEATURES value
2. `ML_Headers/ModelInference.hpp`: increment MODEL_NUM_FEATURES
3. `ML_Headers/ModelInference.hpp`: increment MODEL_FORMAT_VERSION
4. `ML_Headers/ModelInference.hpp`: add packing line in `ModelFeatures_Pack()`
5. Retrain all models (old models will fail version check at load time)
6. If field comes from a new signal: also follow "Adding a new signal to RegimeDetector"

FEAT_* constants are **append-only** — never reorder, never remove.

### Keeping foxml_suite in sync
No manual sync needed — same repo, same headers. Both build targets must compile clean:
```bash
cmake --build build && cmake --build build_suite
```

### FoxML Suite Code Key

**Data flow: Backtest → GUI panels**
```
BacktestEngine.hpp (Backtest_Run)
  → PortfolioController<64> ctrl    (engine state, on worker thread stack)
  → BacktestResults results          (stats + ML features, static in RunControlState)
  → BacktestSnapshot_Copy()          (copies ctrl → TUISnapshot for dashboard)
  → TradeLog CSV                     (logging/BACKTEST_order_history.csv)

GUI reads from:
  TUISnapshot (suite_snap)  → Market, Account, Stats, Positions, Buy Gate panels
  BacktestResults           → Results panel, Training panel (ML samples)
  Trade CSV                 → Trade History panel (TradeHistoryPanel.hpp)
```

**Panel → source mapping:**
| Panel | Source | File |
|-------|--------|------|
| Data | filesystem scan | BacktestPanels.hpp:DataPanelState |
| Run Control | RunControlState | BacktestPanels.hpp:RunControlState |
| Results | BacktestResults.stats | BacktestPanels.hpp:GUI_Panel_Results |
| Trade History | logging/BACKTEST_order_history.csv | TradeHistoryPanel.hpp |
| Training | BacktestResults.feature_matrix | BacktestPanels.hpp:GUI_Panel_Training |
| Comparison | saved BacktestResults snapshots | BacktestPanels.hpp:ComparisonState |
| Market/Account/Stats/Positions/Buy Gate | TUISnapshot (suite_snap) | DashboardPanels.hpp |
| Chart | CandleAccumulator | ChartPanel.hpp |
| Settings | ControllerConfig via backtest.cfg | SettingsPanel.hpp |

**Snapshot sync rule:**
When adding a field to TUISnapshot, update BOTH:
1. `DataStream/EngineTUI.hpp` → `TUI_CopySnapshot()` (live engine)
2. `Backtest/BacktestSnapshot.hpp` → `BacktestSnapshot_Copy()` (backtest suite)

**Config:**
- Live engine: `engine.cfg`
- Backtest suite: `backtest.cfg` (copy of engine.cfg, loaded by Run Control)
- `default_strategy`: -2 = full 4-strat auto (MR+MOM+DIP+EMA), -1 = legacy 2-strat, 0-4 = fixed

**Trade log naming:**
`TradeLog_Init(&log, "SYMBOL")` creates `logging/SYMBOL_order_history.csv` (case-sensitive)

### Centralized constants
- `Version.hpp`: ENGINE_VERSION_STRING — included by all renderers
- `Limits.hpp`: MAX_PORTFOLIO_POSITIONS, CANDLE_HISTORY_MAX — included by Portfolio, TUISnapshot, ChartPanel

## Build

### ANSI TUI (zero deps)
```bash
cmake -B build && cmake --build build
```

### ImGui GUI (SDL2 + OpenGL3)
```bash
cmake -B build_gui -DUSE_IMGUI_GUI=ON [-DLATENCY_PROFILING=ON]
cmake --build build_gui
cd build_gui && ./engine_gui
```

### Backtest Suite (SDL2 + OpenGL3 + XGBoost)
```bash
cmake -B build_suite -DUSE_IMGUI_GUI=ON -DUSE_XGBOOST=ON
cmake --build build_suite --target foxml_suite
cd build_suite && ./foxml_suite
```
**Note**: `-DUSE_XGBOOST=ON` requires the XGBoost C library installed (see Dependencies above). Without it, the suite runs but Train Model and Walk-Forward buttons are disabled.

### Tests
```bash
./build/controller_test  # 279 assertions
```

## Versioning

Version string: `engine vX.Y.Z` — defined ONCE in `Version.hpp` as `ENGINE_VERSION_STRING`. Included by TUIAnsi.hpp, EngineTUI.hpp, and DashboardPanels.hpp. **Update Version.hpp only.**

- **X.Y.Z** follows changelog version in `DOCS/CHANGELOG.md` (version summary table, top row)
- **Patch (Z)**: bug fixes, guards, config changes, TUI tweaks
- **Minor (Y)**: new features (strategies, regime types, TUI modes), breaking config changes
- **Major (X)**: architectural rewrites (FPN width change, new hot-path design)

### Release process
1. Update `Version.hpp` (one file, one location)
2. Update `DOCS/CHANGELOG.md` version summary table
3. Create detailed changelog in `DOCS/changelogs/YYYY-MM-DD-X.md`
4. Commit, push to main
5. Tag: `git tag v3.0.21 && git push origin v3.0.21`

### Tag conventions
- `vX.Y.Z` — release tags (pushed to remote)
- `pre-*` — rollback points before risky changes (local)
- `rollback-*` — named rollback points (local)
- `backup/*` — branch backups (local)

## Code Conventions

- `using namespace std;` used throughout
- C-style with templates, no classes
- Functions follow Pattern_FunctionName convention (e.g. Portfolio_Init, BuyGate)
- All hot-path math uses FPN<F> fixed point, no floats (F=64 words = 4096-bit precision)
- Branchless patterns: mask tricks with -(uint64_t)pass, word-level mask-select
- Inline comments explain reasoning and learning insights
- User's voice/comments must be preserved exactly when editing existing files

### Dynamic Sizing (Backtest Suite ONLY)
Backtest buffers MUST NOT use compile-time caps that silently truncate data. Use dynamic allocation with growth:
- **Sample buffers** (`BacktestResults`): start at `BACKTEST_SAMPLES_INIT`, grow via `BacktestResults_EnsureCapacity()` (2x realloc)
- **Equity curve** (`BacktestResults`): start at `BACKTEST_EQUITY_INIT`, grow via `BacktestResults_EnsureEquityCapacity()` — stats (Sharpe/DD/return) compute from this, silent truncation = wrong stats
- **Tick buffers**: sized from first-pass line count (no fixed max_ticks)
- **Label reload**: sized to `total_processed` (no arbitrary cap)
- **Data files**: `MAX_DATA_FILES` in Limits.hpp (2048), not hardcoded 16
- **Init/Reset/Free**: call `BacktestResults_Init()` before use, `BacktestResults_Reset()` between runs, `BacktestResults_Free()` at shutdown (optimizer included)

When adding new backtest buffers, prefer `malloc` + `realloc` over static arrays. Log allocation failures and degrade gracefully (stop collecting, don't crash).

#### Dynamic-buffer lifecycle invariant (load-bearing)

When you add a new heap-allocated field to `BacktestResults` (or any struct with a `_Reset` helper), update **all four** sites:

1. `_Init` — `malloc` the buffer, set `field_capacity = INIT_CAP`
2. `_Reset` — save pointer + capacity, then re-restore them after `memset(0)` (so counts reset, allocations survive)
3. `_Free` — `free` and NULL the pointer, zero the capacity
4. `_EnsureCapacity` — defensive `cap > 0 ? cap*2 : INIT_CAP` floor, never `0 *= 2`

**Why this is load-bearing.** ff9ac48 (Apr 2026) added `equity_curve` as a dynamic field but only updated _Init/_Free, not the hand-rolled save/restore inside `Backtest_Run`. The next run had `equity_capacity=0`. The first trade exit called `EnsureEquityCapacity(needed=1)` which executed `while (0 < 1) cap *= 2` — infinite spin at 100% CPU on the worker thread, no stderr output, GUI sees "still running" forever. Took a session to find. The `_Reset` helper exists so the knowledge of "which fields are dynamic" lives in exactly one place; the EnsureCapacity floor is belt-and-suspenders against the next time someone adds a field and forgets to extend `_Reset`.

**Live engine is the opposite** — zero dynamic allocation on the hot path. All live buffers are fixed-size, pre-allocated at startup. No malloc, no realloc, no syscalls in the tick loop. This is a hard rule for the execution engine (`build/engine`, `build_gui/engine_gui`).

## Safety Invariants

Rules that MUST be followed when writing or modifying trading logic. These prevent the classes of bugs found in the March 2026 audit.

### Position Exit Invariants
Every code path that sets or modifies `take_profit_price` or `stop_loss_price` MUST:
1. **Preserve TP > entry > SL**: TP must be above entry_price, SL must be below entry_price
2. **Re-check SL floor**: SL distance must be >= 0.5 × TP distance (2:1 minimum reward/risk)
3. **Re-check TP floor**: TP must be >= entry + (entry × fee_rate × 3) (fee breakeven)

The SL floor code pattern (copy this exactly):
```cpp
FPN<F> tp_dist = FPN_Sub(pos->take_profit_price, pos->entry_price);
FPN<F> min_sl_dist = FPN_Mul(tp_dist, FPN_FromDouble<F>(0.5));
FPN<F> sl_floor = FPN_SubSat(pos->entry_price, min_sl_dist);
pos->stop_loss_price = FPN_Min(pos->stop_loss_price, sl_floor);
```

Locations that currently enforce these: fill-time (PortfolioController.hpp:492-512), regime adjustment (RegimeDetector.hpp:310-362).

### FPN Division Guards
Every `FPN_DivNoAssert(numerator, denominator)` MUST have an explicit zero-check on the denominator:
```cpp
if (FPN_IsZero(denominator)) return;  // or continue, or use fallback
```
Never rely on "it can't be zero in practice" — guard explicitly. FPN_DivNoAssert saturates to MAX on zero, which silently produces extreme values.

### Config Field Conventions
Two types of numeric config fields exist. Never mix them:
- **Percentage fields** (`_pct` suffix): stored as decimal (0.04 = 4%), parsed with `/100.0`. When used as stddev multiplier: `mult = field × 100` (e.g., 0.04 × 100 = 4.0σ)
- **Multiplier fields** (`_mult` suffix): stored as direct value (3.0 = 3.0σ), parsed raw. Used directly: `offset = stddev × field`

When adjusting momentum positions, use `momentum_tp_mult` / `momentum_sl_mult`.
When adjusting MR positions, use `take_profit_pct × 100` / `stop_loss_pct × 100`.
Never cross these — MR config on momentum positions (or vice versa) creates asymmetric exits.

### FPN-Only Accounting
Any code path that touches balance, P&L, fees, equity, or position pricing MUST use `FPN<F>` — never `double` or `float` for intermediate calculations. `double` is only acceptable at system boundaries:
- **OK**: `FPN_ToDouble` for display, logging, CSV output, printf
- **OK**: `FPN_FromDouble` at exchange API boundary (Binance returns doubles)
- **NOT OK**: `double equity = FPN_ToDouble(balance) + FPN_ToDouble(value)` for decision logic
- **NOT OK**: `double product = price_d * qty_d` before converting to FPN (precision loss in product)

**Known violations to fix** (as of 2026-04-01):
- `peak_equity`, `session_start_equity`, `max_drawdown` (PortfolioController struct) — double fields used by kill switch
- Kill switch equity/drawdown computation (PortfolioController.hpp ~line 1103-1135) — all double arithmetic
- Orphan recovery proceeds (main.cpp ~line 766) — `fp * fq` in double before FPN conversion

### FPN Comparison Completeness
When comparing FPN values, use `FPN_LessThan`, `FPN_GreaterThanOrEqual`, etc. — never partial word comparisons. The inline optimization in `PositionExitGate` (Portfolio.hpp:226-229) only compares MSW and LSW, skipping middle words — this is a known bug that can miss exits near price boundaries.

### Halt Flag Invariant
Every code path that suppresses buying MUST set `ctrl->buying_halted = 1` and zero `ctrl->gate_offset`. Ad-hoc zeroing of `buy_conds` alone is insufficient — hot-path gate tracking will restore it from `gate_offset` on the next tick.

### Regime Adjustment Checklist
When adding a new regime transition case in `Regime_AdjustPositions`:
1. Guard stddev != 0 at function entry
2. Use the correct config field family (momentum_*_mult for momentum positions, *_pct×100 for MR)
3. After all TP/SL mutations, re-check SL floor and TP floor
4. Verify FPN_Max vs FPN_Min direction:
   - **Tighten TP** (closer to entry) = FPN_Min (pick lower)
   - **Widen TP** (further from entry) = FPN_Max (pick higher)
   - **Tighten SL** (closer to entry) = FPN_Max (pick higher, since SL < entry)
   - **Widen SL** (further from entry) = FPN_Min (pick lower)
5. Add a regression test for the new transition

### Label-type-aware metric invariant (load-bearing — Backtest Suite)

**Rule:** every metric, display, training, or validation site that touches label values MUST consult `label_table[t].num_classes` (via `LabelType_IsBinary` / `LabelType_IsRegression` / `LabelType_IsMulticlass` helpers in `LabelFunctions.hpp`) and branch on the kind. Never hardcode binary classification assumptions.

**The four label kinds and their metric semantics:**

| `num_classes` | Kind | Label values | XGBoost objective | Primary metric | Overfit detector |
|---|---|---|---|---|---|
| 0 | binary | {0.0, 1.0}, optionally 0.5=neutral (filtered) | `binary:logistic` + `scale_pos_weight` | accuracy [0,1] | `OverfitDetection_CheckDefaults` (acc thresholds) |
| 1 | regression | continuous (any range) | `reg:squarederror` | Pearson correlation r | `OverfitDetection_CheckRegressionDefaults` (corr thresholds) |
| ≥2 | multiclass | integer class ids 0..K-1 (as float) | `multi:softprob` + `num_class=K` | argmax accuracy | `OverfitDetection_CheckDefaults` (acc thresholds — same as binary) |

**Sites that must branch on label kind** (verify when adding new metric/display code):

1. Sample panel display in `BacktestPanels.hpp` (`GUI_Panel_Training` collection summary) — kind determines whether to show +/-/neutral, per-class histogram, or min/max/mean/stddev.
2. Train Model in-sample metric in `BacktestPanels.hpp` (post-training prediction loop) — kind determines whether to compute accuracy, multiclass-accuracy, or MSE+correlation.
3. Walk-Forward `Backtest_RunWalkForward` in `BacktestEngine.hpp`:
   - Neutral filter at start: only run for binary (regression `0.5` is a real value, not a sentinel).
   - XGBoost objective + `num_class` param: select by kind.
   - `scale_pos_weight`: binary only (multiclass uses per-sample weights, regression doesn't have the concept).
   - Per-fold metric: pick `WalkForward_ComputeAccuracy` / `ComputeMulticlassAccuracy` / `ComputeMSE`+`ComputeCorrelation`.
   - Aggregate `WalkForwardResults`: write `mean_val_accuracy` for classification or `mean_val_correlation`+`mean_val_mse` for regression. Set `wf->label_kind` so display layer knows what to format.
4. Walk-Forward result display in `BacktestPanels.hpp` — read `wf->label_kind` and pick column headers + formatting (Acc% vs r vs MSE).
5. Overfit detection — pick `OverfitDetection_CheckDefaults` (classification) or `OverfitDetection_CheckRegressionDefaults` (regression). The `OverfitReport` struct fields are reused; field semantics depend on kind.
6. Save Run / `expected.cfg` writer — already records `expected_num_classes`. Verify on load.

**Why this is load-bearing.** 2026-04-25 morning session — ran Forward P&L (regression label) and got `+: 0 / -: 2,254,869 / Ratio: 0.0%` in the sample panel, `Train Accuracy: 0.2%`, and walk-forward `0.0%/0.0%/0.0%` for every fold. None of those numbers were meaningful — they were binary-classification metrics computed on continuous regression labels (every label below the binary 0.5 threshold → counted as "negative"; predictions binarized at 0.5 → useless on continuous output; walk-forward hardcoded `binary:logistic` so it actively trained nonsense models). Same shape as the equity_curve spinner from the previous day: a primitive existed (`label_table.num_classes`), a few sites consulted it (training-side objective), but the rest of the codebase still assumed binary. The fix wired all six sites above to branch on kind. The helpers + this rule make the next regression-vs-classification metric drift hard to write accidentally — **but enforcement is on the human**, the compiler doesn't catch a new metric site that simply doesn't call the helpers.

**Future hardening (deferred):** turn `num_classes` into `enum class LabelKind { Binary, Regression, Multiclass }` so the compiler exhaustive-checks switches. Larger surgery — touches every existing site again. Reasonable v2 once the current convention has settled.

## Current State

- Portfolio controller: COMPLETE (166/166 tests passing)
- Post-SL cooldown: adaptive (scales by trend confidence at SL time) or fixed cycle count
- Regime detection: score-based with 7 signals, extensible RegimeSignals struct
- Volume spike detection: current/max ratio, spacing relaxation on 5x+ spikes
- RollingStats: real least-squares regression (slope, R², variance) + VWAP
- VWAP gate: buy signal gates on price being below volume-weighted average price
- Session awareness: per-session (Asian/EU/US/overnight) volume gate multiplier
- Snapshot persistence: v7 (entry_time + session stats survive restarts)
- Binance websocket: WORKING (live market data)
- TUI: ANSI only (zero deps, diff-based rendering, foxml palette). FTXUI/notcurses removed.
- TUI snapshot: zero-pollution (full copy on slow path, live price/volume/active_count every tick)
- Momentum TP/SL: adaptive (R²-scaled + ROR acceleration bonus at fill time)
- Trailing TP/SL: SL floor invariant enforced (only when SL below entry — free trades exempt)
- Slippage simulation: configurable entry/exit price adjustment (slippage_pct in engine.cfg)
- Single-slot mode: max_positions=1 (default), sells entire BTC balance on exit (no dust)
- Paper/live sync: unbacked paper positions are undone, startup recovers orphaned BTC

## Key Design Decisions

1. Portfolio uses uint16_t bitmap (not sequential count) - same pattern as OrderPool
2. Per-position TP/SL exits on hot path, portfolio management on slow path
3. Fill consumption happens EVERY tick (zero unprotected exposure)
4. Single-slot mode (max_positions=1): exchange BTC balance IS the position, sell-all eliminates dust
5. Multi-slot fallback: per-position sells when max_positions > 1 (dust may accumulate)
6. Warmup phase observes market before trading (gates on slow-path sample count, not raw ticks)
7. 24-hour session lifecycle: warmup -> trade -> wind down -> close all -> reconnect
8. TUI is independent of engine (engine runs headless, TUI only reads state)
9. No API key needed for market data websocket (public endpoint)
10. RollingStats computes regression inline (no separate feeder for regime R²)
11. RegimeSignals struct is the extensibility point — new signal = new field + one comparison

## Extensibility Hooks

- **New regime signal**: add field to RegimeSignals + one comparison in Regime_Classify
- **FoxML model output**: add `model_score` to RegimeSignals, feed from bridge
- **New strategy**: follow the checklist below
- **New regime**: new constant + new mapping + optional position adjustment case
- **Lookup table**: RegimeSignals fields map to table indices naturally

## How To: Common Changes

### Adding a new config field
3 lines total, hot-reload is automatic:
1. **ControllerConfig.hpp struct**: add `FPN<F> my_field;` (or `uint32_t`, `int`)
2. **ControllerConfig_Default()**: add `cfg.my_field = FPN_FromDouble<F>(1.0);`
3. **ControllerConfig_Load() parser table**: add one macro line in the right category:
   - `CFG_PARSE_FPN(my_field)` — raw value (atof)
   - `CFG_PARSE_PCT(my_field)` — percentage (config says 15.0, stored as 0.15)
   - `CFG_PARSE_FPN_POS(my_field)` — clamped to >= 0
   - `CFG_PARSE_U32(my_field)` — unsigned int
   - `CFG_PARSE_INT(my_field)` — int

Hot-reload: **automatic** — `PortfolioController_HotReload` does bulk struct copy. To protect a field from reload (startup-only), save/restore it in HotReload.

### Adding a new buy gate
2 lines using Gate_Zero helper (OrderGates.hpp):
```cpp
int my_gate_ok = my_condition | !my_enabled;
Gate_Zero(&conds, my_gate_ok);  // zeros price+volume if gate fails
```

### Adding a TUI display field
1. **EngineTUI.hpp TUISnapshot struct**: add `double my_field;`
2. **EngineTUI.hpp TUI_CopySnapshot()**: add `snap->my_field = FPN_ToDouble(source);`
3. **TUIAnsi.hpp**: add display in the appropriate ANSI_Section_* function

Only 1 TUI renderer (TUIAnsi.hpp). FTXUI/notcurses were removed.

### Version bump
3 locations in 2 files (search for `engine v`):
- TUIAnsi.hpp (1×)
- EngineTUI.hpp (2×)

### Adding a RollingStats field
1. **RollingStats struct** (ML_Headers/RollingStats.hpp): add `FPN<F> my_field;`
2. **RollingStats_Init()**: add `rs.my_field = FPN_Zero<F>();`
3. **RollingStats_Push()**: compute it (runs on slow path, O(W) budget is fine)
4. Field is now readable from any strategy or regime detector via `rolling->my_field`

For running-sum fields (like VWAP), use the evict-old/add-new pattern with a `pv_buf[W]` ring buffer. For single-pass fields, compute inline in the existing loop.

### Adding a new exit reason
Rare — only needed for fundamentally new exit types (not TP/SL variants):
1. **Portfolio.hpp PositionExitGate**: add comparison logic + exit buffer record
2. **Portfolio.hpp ExitBufferRecord**: add reason constant
3. **PortfolioController.hpp DrainExits**: handle the new reason in P&L booking
4. **TUIAnsi.hpp**: display if needed
5. **tests/controller_test.cpp**: regression test

## Adding a New Strategy

Every strategy follows the same 4-function pattern. All logic runs on the slow path; the hot path only reads `buy_conds`.

### Step 1: Define constant
`Strategies/StrategyInterface.hpp`:
```cpp
#define STRATEGY_YOUR_NAME 2  // next available ID
```

### Step 2: Create strategy header
`Strategies/YourName.hpp` with:

**State struct:**
```cpp
template <unsigned F> struct YourNameState {
    RegressionFeederX<F> feeder;       // P&L regression (drives adaptation)
    RegressionFeederX<F> price_feeder; // price regression (drives trailing)
    FPN<F> live_param;                 // adaptive filter parameter
    BuySideGateConditions<F> buy_conds_initial; // anchor for max_shift clamp
    int has_regression;
};
```

**4 functions (same signatures as MeanReversion/Momentum):**
- `YourName_Init(state, rolling, buy_conds)` — set initial buy conditions from rolling stats
- `YourName_Adapt(state, price, portfolio_delta, active_bitmap, buy_conds, cfg)` — adjust filters via P&L regression
- `YourName_BuySignal(state, rolling, rolling_long, cfg)` — return `BuySideGateConditions` with `gate_direction`
- `YourName_ExitAdjust(portfolio, price, rolling, state, cfg)` — trailing TP/SL for running positions

### Step 3: Wire into PortfolioController
`CoreFrameworks/PortfolioController.hpp`:
- Add `YourNameState<F> your_name;` to `PortfolioController` struct
- Add `case STRATEGY_YOUR_NAME:` in the strategy dispatch switch (~line 677), calling _Adapt, _ExitAdjust, _BuySignal, and setting `gate_direction`
- Add same case in `PortfolioController_Unpause` for buy gate restore

### Step 4: Wire into RegimeDetector
`Strategies/RegimeDetector.hpp`:
- Add mapping in `Regime_ToStrategy`: `case REGIME_X: return STRATEGY_YOUR_NAME;`
- Add position adjustment case in `Regime_AdjustPositions` (follow the Regime Adjustment Checklist in Safety Invariants)

### Step 5: Add tests
`tests/controller_test.cpp`:
- Test buy signal generation
- Test TP/SL at fill time
- Test regime transition adjustments (SL floor must hold)

### Key patterns
- **Branchless**: `uint64_t mask = -(uint64_t)condition` + word-level AND/OR
- **FPN<F> only**: no floats in hot/warm paths
- **Regression adaptation**: feed P&L → compute slope → negate → shift filters → clamp to bounds → mask by R²
- **Trailing exits**: `hold_score = SNR × R²` → ratchet TP/SL upward with FPN_Max
