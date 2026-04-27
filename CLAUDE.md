# CLAUDE.md

## Overview

Tick-level crypto trading engine in C++. Branchless fixed-point arithmetic, bitmap-based portfolio management, regression-driven gate adjustment with regime detection. Per-core sharded hot path (40-400ns p99), single-symbol producer thread fanning real Binance ticks across SPSC rings to per-core consumers.

**Sharded is the production engine and the active development focus.** Legacy single-threaded LIVE mode (`engine_mode=single_core`) remains as a runtime-warned deprecated benchmark. Legacy BACKTEST mode is gone (Track E.7, 2026-04-26) — `Backtest_Run` is a thin wrapper around `BacktestSharded_Run`. ANSI TUI (`build/engine`) is deprecated for production but kept for headless contexts.

## Build

**Wrapper script (preferred):** `./build.sh` provides single entry point for all build variants.

```bash
./build.sh test    # build engine + run controller_test
./build.sh gui     # build engine_gui + foxml_suite
./build.sh suite   # build foxml_suite with XGBoost (requires libxgboost)
./build.sh all     # build engine + gui (skip suite by default)
./build.sh clean   # wipe all build dirs
```

Build options: `-DLATENCY_PROFILING=ON`, `-DLATENCY_LITE=ON`, `-DLATENCY_BENCH=ON`, `-DBUSY_POLL=ON`, `-DUSE_NATIVE_128=ON`. See README.md for details.

**Build directory layout** (intentional — different compile flags require different outputs):
- `build/` — ANSI engine + tests (no GUI deps, no XGBoost)
- `build_gui/` — engine_gui + foxml_suite (ImGui + SDL2 + OpenGL3)
- `build_suite/` — same as build_gui + XGBoost-linked variant of foxml_suite
- `build_lat/` — latency-profiling variant (with `-DLATENCY_PROFILING=ON`)

### Dependencies

| Target | Dependencies |
|--------|-------------|
| ANSI TUI (`build/engine`) | None (zero-dependency, deprecated for production) |
| ImGui GUI (`build_gui/engine_gui`) | SDL2, OpenGL3 |
| FoxML Suite (`build_suite/foxml_suite`) | SDL2, OpenGL3, XGBoost C library (for ML training) |

**XGBoost C library** (required for `-DUSE_XGBOOST=ON`):
```bash
git clone --recurse-submodules https://github.com/dmlc/xgboost.git /tmp/xgboost
cd /tmp/xgboost && mkdir build && cd build
cmake .. -DBUILD_STATIC_LIB=OFF && make -j$(nproc)
sudo make install && sudo ldconfig
```

`build.sh` auto-symlinks `engine.cfg` into each build dir so `./engine_*` connects to live Binance out of the box. `bin/engine_gui` symlinks to `build_gui/engine_gui` for canonical "latest" access.

## Architecture (sharded)

N execution cores (default 4, cap 16), each with its own ExecutionCore + parameters + tick ring + slot in the central OMS portfolio. Branchless ~60ns hot path per core. Producer thread pinned to one CPU, fans real Binance ticks (or synthetic for offline benchmark) across SPSC rings to per-core consumer threads.

**Sharded mode key properties:**
- Per-core strategy (`core_N_strategy=simple_dip|momentum|ema_cross|ml`)
- Per-core ML model (`core_N_model_path=...` or `core_N_model_dir=...` for a CoreModelZoo with auto-discovered roles barrier/buy_signal/regime/exit)
- Per-core risk allocation (`core_N_risk_pct=...`, default = `risk_pct / N`)
- Per-core ConfidenceScorer (when STRATEGY_ML in use)
- Per-core hot-swap of strategy live (GUI dropdown + Apply button)
- AUTO regime mode per core
- Risk distributed: a single bad core can lose its allocation, not the account
- Hot path p99 target: ≤500ns per core (measured 460-567ns post-rdtsc-floor 2026-04-27)
- No portfolio walk on the hot path — each core only owns its slot
- Partial exits (when `partial_exit_enabled=1`): each core owns 2 portfolio slots (legs A+B); max cores = 8

```
HOT PATH (every tick, per core, branchless):
  BG_Evaluate         → can_enter mask
  SG_Evaluate (×2)    → can_exit_a, can_exit_b masks
  TradeEvent push     → SPSC ring (rare branch — predicts not-taken)

SLOW PATH (every poll_interval ticks):
  RollingStats_Push   → least-squares regression (slope, R², variance) + VWAP
  Regime_ComputeSignals → 16-field RegimeSignals (used by ML feature pack)
  Strategy_BuildParameters → strategy-specific gate params (with partial-exits cap)
  EventLoop_RebuildAllParameters → seqlock push to per-core cached_params
  drain_with_submit   → consume TradeEvents, route to OMS, log to CSV

ASYNC THREADS:
  Binance trade WS (live ticks)
  Binance depth WS (book imbalance, spread metrics)
  TickRecorder + DepthRecorder (daily-rotating CSVs in data/)
  Notify worker (alerts via stderr/command backend)
  GUI thread (ImGui render, reads TUISharedState via seqlock)
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
  ema_sma_spread, ema_above_sma          (trend bias)
  book_imbalance, spread_bps, spread_z   (microstructure — Wave 1+2)
  flow_buy_ewma, flow_sell_ewma          (D.2 flow EWMAs)
  large_trade_z                          (D.4 large-trade z-score)

        ↓

Regime_Classify (score-based):
  trending_score (5 signals: slope×2, R², acceleration, volume)
  volatile_score (2 signals: variance spike, no direction)
  → RANGING / TRENDING / VOLATILE with hysteresis

        ↓

Strategy dispatch → position adjustment on regime switch
```

## Directory Structure

- **CoreFrameworks/** — OrderGates (buy gate), Portfolio (bitmap positions, exit gate), ExecutionCore (per-core hot path), ControllerEventLoop (per-core slow path), EngineSharded (top-level driver), ShardedSnapshot/Persist, GateParameters, TradeEvent
- **Strategies/** — RegimeDetector (RegimeSignals, score-based classify), MeanReversion, Momentum, SimpleDip, EmaCross, MLStrategy, StrategyParameters (dispatcher), StrategyInterface
- **DataStream/** — BinanceCrypto (trade WS), BinanceDepth (depth WS), DepthReplayState (backtest mirror), DepthRecorder, TickRecorder, BinanceOrderAPI (REST), EngineTUI (snapshot, thread), TUIAnsi (deprecated)
- **FixedPoint/** — FPN arbitrary-width fixed-point arithmetic (F=64 words = 4096-bit)
- **MemHeaders/** — PoolAllocator (bitmap order pool), BuddyAllocator
- **ML_Headers/** — RollingStats, ROR_regressor, ConfidenceScore, ModelInference (XGBoost)
- **GUI/** — Dear ImGui native GUI: FoxmlTheme, DashboardPanels, ChartPanel, CandleAccumulator, TradeReader, SettingsPanel, TradeHistoryPanel, LogViewerPanel, GuiThread
- **Backtest/** — BacktestEngine (`Backtest_Run` wrapper + `BacktestSharded_Run`), BacktestPanels (suite GUI), LabelFunctions
- **tests/** — controller_test.cpp (640+ assertions)
- **plans/** — implementation plans (gitignored)
- **vendor/** — vendored imgui + implot v0.18 (gitignored)
- **Version.hpp**, **Limits.hpp** — single source of truth

## Integration Contracts

When changing something, here's exactly what to update:

### Adding a new signal to RegimeDetector
1. `Strategies/RegimeDetector.hpp`: add field to `RegimeSignals<F>` struct
2. `Strategies/RegimeDetector.hpp`: populate it in `Regime_ComputeSignals()`
3. `Strategies/RegimeDetector.hpp`: use it in `Regime_Classify()` scoring
4. Done — RegimeSignals is the single extensibility point

### Adding a new config field
1. `CoreFrameworks/ControllerConfig.hpp`: add to struct + default + parser macro
2. `engine.cfg` and `backtest.cfg`: add with comment
3. Done — hot-reload is automatic via bulk struct copy

Macros: `CFG_PARSE_FPN(field)`, `CFG_PARSE_PCT(field)` (config 15.0 → stored 0.15), `CFG_PARSE_FPN_POS(field)`, `CFG_PARSE_U32(field)`, `CFG_PARSE_INT(field)`. To protect a field from hot-reload, save/restore in HotReload.

### Adding a GUI-editable config field (above + settings panel)
4. `GUI/SettingsPanel.hpp`: add ONE line to `field_defs[]` array — loading, UI, and saving are automatic
5. `GUI/SettingsPanel.hpp`: add tooltip in the `SetItemTooltip` section (ALWAYS — fields without tooltips are confusing)
6. If adding a new strategy: update the `default_strategy` tooltip to include the new ID and name

### Adding a new TUI/GUI display field
1. `DataStream/EngineTUI.hpp`: add to `TUISnapshot` struct
2. `DataStream/EngineTUI.hpp`: populate in `TUI_CopySnapshot()` (live engine path)
3. `GUI/DashboardPanels.hpp`: display in appropriate `GUI_Panel_*`

Backtest auto-syncs — `BacktestSnapshot_Copy` is a thin wrapper around `TUI_CopySnapshot`. ANSI TUI display (`DataStream/TUIAnsi.hpp`) is optional (deprecated).

### Adding a new chart overlay
1. `GUI/ChartPanel.hpp`: add to `ChartState` if new data needed, add render call in `GUI_PriceChart()`
2. If data comes from engine: also update `TUISnapshot` + `TUI_CopySnapshot()`

### Adding a new strategy
1. `Strategies/StrategyInterface.hpp`: add `#define STRATEGY_YOUR_NAME N` (append-only)
2. `Strategies/YourName.hpp`: create with state struct + `_Init` + `_BuildParameters` (sharded path) + optionally `_Adapt`/`_ExitAdjust` (legacy path)
3. `Strategies/StrategyParameters.hpp`: add case in `Strategy_BuildParameters` dispatch
4. `Strategies/RegimeDetector.hpp`: add `Regime_ToStrategy` mapping if AUTO regime should select it
5. `tests/controller_test.cpp`: regression tests

**Sharded gate direction:** `BG_Evaluate` defaults to buy-below. Buy-above strategies (momentum, breakout) MUST set `out->flags |= GATE_FLAG_BUY_ABOVE` in `_BuildParameters`. Otherwise the core silently buys dips while the GUI claims momentum (pre-v4.0.1 bug).

**Partial exits:** strategies write only leg-A fields (`tp_pct`, `sl_pct`). The dispatcher post-cap in `Strategy_BuildParameters` sets `GATE_FLAG_PAIR_ACTIVE` + `tp_pct_b = tp_pct * cfg.tp2_mult` when `cfg.partial_exit_enabled=1`, and explicitly clears both when disabled. Don't write `tp_pct_b` or `GATE_FLAG_PAIR_ACTIVE` from a strategy.

### Bumping version
1. `Version.hpp`: update `ENGINE_VERSION_STRING` (one file, one location — included by all renderers)
2. `DOCS/CHANGELOG.md`: update version summary table
3. `DOCS/changelogs/YYYY-MM-DD-X.md`: dated changelog
4. Tag: `git tag vX.Y.Z && git push origin vX.Y.Z`

Patch (Z) = bug/cfg/TUI; Minor (Y) = new features/strategies; Major (X) = architectural rewrites (FPN width, hot-path redesign).

### Public release (FoxML_Trader)
Public repo: `Jennyfirrr/FoxML_Trader` (`git@github.com:Jennyfirrr/FoxML_Trader.git`). Uses its own version numbering (v1.0.x), separate from internal engine version. **FoxML_Trader uses the legacy architecture — not 1:1 with the sharded engine.**

1. Copy changed files from `tick_trader_private` to `~/FoxML_Trader/`
2. Commit + push to `main`
3. Tag: `git tag v1.0.N && git push origin v1.0.N`
4. Create GitHub release: `gh release create v1.0.N --title "v1.0.N — Title <3" --notes "..."`

Rules:
- **Always include `<3` in the release title**
- **NEVER push private configs** — `engine.cfg`, `controller.cfg`, `.env`, API keys, plans/
- Update `CHANGELOG.md` in FoxML_Trader with each release; keep release notes short and human

### Adding a new ML feature
1. `ML_Headers/ModelInference.hpp`: add `FEAT_NEW_NAME` at current MODEL_NUM_FEATURES value
2. `ML_Headers/ModelInference.hpp`: increment `MODEL_NUM_FEATURES` + `MODEL_FORMAT_VERSION`
3. `ML_Headers/ModelInference.hpp`: add packing line in `ModelFeatures_Pack()`
4. Add the field to `RegimeSignals<F>` and populate in `Regime_ComputeSignals` (single site)
5. If the field needs new state (like ROR or EMA), add it to BOTH `EngineSharded_Run` (sharded live) AND `BacktestSharded_Run` (sharded backtest, mirrors via `ShardedBacktestDriver`), with parity in update cadence
6. Retrain all models (old models fail version check at load)

FEAT_* constants are **append-only** — never reorder, never remove.

### Keeping foxml_suite in sync
No manual sync needed — same repo, same headers. Both build targets must compile clean:
```bash
cmake --build build && cmake --build build_suite
```

### FoxML Suite Code Key

**Data flow: Backtest → GUI panels**
```
BacktestEngine.hpp (Backtest_Run → BacktestSharded_Run)
  → ShardedBacktestDriver         (runs slow-path callbacks, feature collection)
  → BacktestResults                (stats + ML features, static in RunControlState)
  → BacktestSnapshot_Copy()        (copies state → TUISnapshot for dashboard)
  → TradeLog CSV                   (logging/BACKTEST_order_history.csv)

GUI reads from:
  TUISnapshot (suite_snap)  → Market, Account, Stats, Positions, Buy Gate panels
  BacktestResults           → Results panel, Training panel (ML samples)
  Trade CSV                 → Trade History panel (TradeHistoryPanel.hpp)
```

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

**Config files:** Live engine = `engine.cfg`. Backtest suite = `backtest.cfg` (copy of engine.cfg, loaded by Run Control). `default_strategy`: -2 = full 4-strat auto, -1 = legacy 2-strat, 0-4 = fixed.

**Trade log naming:** `TradeLog_Init(&log, "SYMBOL")` creates `logging/SYMBOL_order_history.csv` (case-sensitive). Sharded log format (v3): `timestamp_us,core_id,strategy_id,event_type(E|X),event_price,entry_price,exit_price,pnl,fees,balance_after,trade_size`. With partials, `core_id` in the CSV is the portfolio SLOT (slot c → core c/2, leg c%2).

### Centralized constants
- `Version.hpp`: `ENGINE_VERSION_STRING` — included by all renderers
- `Limits.hpp`: `MAX_PORTFOLIO_POSITIONS`, `CANDLE_HISTORY_MAX` — included by Portfolio, TUISnapshot, ChartPanel

## Code Conventions

- `using namespace std;` used throughout
- C-style with templates, no classes
- Functions follow `Pattern_FunctionName` convention (e.g. `Portfolio_Init`, `BG_Evaluate`)
- All hot-path math uses `FPN<F>` fixed point, no floats (F=64 words = 4096-bit precision)
- Branchless patterns: mask tricks with `-(uint64_t)pass`, word-level mask-select
- Inline comments explain reasoning and learning insights
- User's voice/comments must be preserved exactly when editing existing files

### Dynamic Sizing (Backtest Suite ONLY)
Backtest buffers MUST NOT use compile-time caps that silently truncate data. Use dynamic allocation with growth:
- **Sample buffers** (`BacktestResults`): start at `BACKTEST_SAMPLES_INIT`, grow via `BacktestResults_EnsureCapacity()` (2× realloc)
- **Equity curve**: start at `BACKTEST_EQUITY_INIT`, grow via `BacktestResults_EnsureEquityCapacity()` — stats (Sharpe/DD/return) compute from this, silent truncation = wrong stats
- **Tick buffers**: sized from first-pass line count (no fixed max_ticks)
- **Init/Reset/Free**: call `BacktestResults_Init()` before use, `BacktestResults_Reset()` between runs, `BacktestResults_Free()` at shutdown

When adding new backtest buffers, prefer `malloc` + `realloc` over static arrays. Log allocation failures and degrade gracefully.

#### Dynamic-buffer lifecycle invariant (load-bearing)

When you add a new heap-allocated field to `BacktestResults` (or any struct with a `_Reset` helper), update **all four** sites:

1. `_Init` — `malloc` the buffer, set `field_capacity = INIT_CAP`
2. `_Reset` — save pointer + capacity, then re-restore them after `memset(0)`
3. `_Free` — `free` and NULL the pointer, zero the capacity
4. `_EnsureCapacity` — defensive `cap > 0 ? cap*2 : INIT_CAP` floor, never `0 *= 2`

**Why this is load-bearing.** ff9ac48 (Apr 2026) added `equity_curve` as dynamic but only updated _Init/_Free, not the hand-rolled save/restore inside `Backtest_Run`. The next run had `equity_capacity=0`. The first trade exit called `EnsureEquityCapacity(needed=1)` which executed `while (0 < 1) cap *= 2` — infinite spin at 100% CPU on the worker thread, GUI sees "still running" forever. Took a session to find. The `_Reset` helper exists so the knowledge of "which fields are dynamic" lives in exactly one place.

**Live engine is the opposite** — zero dynamic allocation on the hot path. All live buffers fixed-size, pre-allocated at startup. No malloc, no realloc, no syscalls in the tick loop. Hard rule for the execution engine.

## Plan Review Checklist (load-bearing — apply to every multi-day plan)

Before starting any plan that spans more than a single commit, walk it through this checklist. Findings → either revise scope, add a mitigation, or document an accepted exception. **Audit BEFORE coding** — every audit-after-the-fact in this codebase has caught real bugs.

### 1. Hot path purity
- Does this add code to `ExecutionCore_Tick`, `BG_Evaluate`, `SG_Evaluate`, or any per-tick callsite?
- If yes, is every operation **branchless** (no `if`, only mask-select), **FPN-only** (no `double`), and **alloc-free**?
- Hot path latency budget is **≤500ns p99**. Currently 40-400ns. Adding ≥10ns requires explicit justification.
- Default answer: hot path stays untouched. New code goes on slow path.

### 2. Train-serve parity
- Does this affect `RegimeSignals` fields or `ModelFeatures_Pack`?
- If yes, does **both** `BacktestSharded_Run` AND `EngineSharded_Run` populate the new state with **equivalent cadence and inputs**?
- New features must produce identical values for identical input streams. Document accepted divergence (maker/taker fees, tick timing) explicitly.
- Sites that may need updating: `EngineSharded_Run` static-locals, `BacktestSharded_Run` driver mirror, `ML_BuildParameters`, `Regime_ComputeSignals`, `ModelFeatures_Pack`.

### 3. Surface area / coupling
- How many files / call sites does this change?
- Adding `if (live_trading)` branches anywhere is a smell — abstraction boundary is wrong.
- For new optional state, follow the existing pattern (heap-allocated, NULL-init in caller, freed in cleanup).
- The right shape: new state owns its lifecycle in ONE place, all consumers receive it via params.
- **Forward-thinking test**: how many sites would the next similar feature need to modify? Aim to reduce that count.

### 4. Pointer init + heap lifecycle
- Any new heap-allocated pointer field on `OrderManagerState` / `EventLoopState` / `CoreContext`?
- If yes:
  - Every caller of `*_Init` must `NULL` the pointer first
  - `_Init` must `if (ptr) free(ptr)` before re-allocating (handles re-init / 24h reconnect)
  - Cleanup path must free + NULL on shutdown
  - Snapshot persistence must include the new state (or document why it's session-only)
- This is the doctrine behind the v4.3 segfault — three sites needed the same NULL-init line, two had it, one didn't.

### 5. Backward compatibility
- Snapshots: `SHARDED_SNAPSHOT_VERSION` bump → old files refused. OK if intentional, document in changelog.
- Models: `MODEL_FORMAT_VERSION` bump → old `.json` models fail load. Always document FEAT_* additions.
- Saved Runs: extending `summary.txt` / `expected.cfg` is forward-compat. Removing fields breaks Past Runs.
- Cfg files: adding new fields is fine (default in `_Default`, parser falls through). Removing parsed fields breaks user cfgs.

### 6. Multi-threading correctness
- New shared state? Identify producer + consumer threads.
- SPSC ring? Verify single producer, single consumer.
- Atomic vs non-atomic — anything multi-thread-shared without `std::atomic` or explicit `__atomic_*` is a race.
- Race conditions to specifically check: producer + drainer on OMS fields, slow path + hot path on `GateParameters` (use seqlock).
- Backtest mode determinism: even if multi-threaded, backtest output should be reproducible run-to-run.

### 7. Test coverage
- Is there a "hammer test" that exercises the round-trip / N-iteration case?
- Edge cases: cold start (count==0), full window (count==WINDOW), wraparound (count > WINDOW), zero inputs, uninitialized fields.
- Does it run in `controller_test`?

### 8. Docs + invariants
- Does this introduce a load-bearing rule? Add a section to "Safety Invariants" below.
- Update "Current State" with the new capability. Add a dated changelog entry.
- Significant plans → write to `plans/{name}.md` (gitignored), commit-message-link.

### 9. Forward maintenance
- Will this require touching 30+ sites to extend later? If yes, redesign for lower coupling.
- Will the next similar feature copy-paste this code? If yes, factor a helper.
- Identify brittle assumptions ("assumes max_positions=1") and document in comment.

### 10. Rollback story
- Tag `pre-{name}` before starting. Push to remote.
- For multi-week plans, branch `backup/pre-{name}-{date}`.
- Each phase commit individually revertable.

### Audit verdicts vocabulary

- **PASS ✅**: requirement met
- **FIXED ✅**: was an issue, patched in the same pass
- **GAP** ⚠️: real concern, must address before plan ships
- **DRIFT** ⚠️: pre-existing condition the plan inherits — note + mitigate
- **DEFERRED**: scoped out, has explicit follow-up
- **ACCEPTED**: known divergence, documented and lived with

## Safety Invariants

Rules that MUST be followed when writing or modifying trading logic.

### Position Exit Invariants
Every code path that sets or modifies `take_profit_price` or `stop_loss_price` MUST:
1. **Preserve TP > entry > SL**
2. **Re-check SL floor**: SL distance must be >= 0.5 × TP distance (2:1 minimum reward/risk)
3. **Re-check TP floor**: TP must be >= entry + (entry × fee_rate × 3)

The SL floor code pattern:
```cpp
FPN<F> tp_dist = FPN_Sub(pos->take_profit_price, pos->entry_price);
FPN<F> min_sl_dist = FPN_Mul(tp_dist, FPN_FromDouble<F>(0.5));
FPN<F> sl_floor = FPN_SubSat(pos->entry_price, min_sl_dist);
pos->stop_loss_price = FPN_Min(pos->stop_loss_price, sl_floor);
```

### FPN Division Guards
Every `FPN_DivNoAssert(numerator, denominator)` MUST have an explicit zero-check:
```cpp
if (FPN_IsZero(denominator)) return;  // or continue, or use fallback
```
Never rely on "it can't be zero in practice" — guard explicitly. `FPN_DivNoAssert` saturates to MAX on zero, which silently produces extreme values.

### Config Field Conventions
Two types of numeric config fields exist. Never mix them:
- **Percentage fields** (`_pct` suffix): stored as decimal (0.04 = 4%), parsed with `/100.0`. When used as stddev multiplier: `mult = field × 100`
- **Multiplier fields** (`_mult` suffix): stored as direct value (3.0 = 3.0σ), parsed raw. Used directly: `offset = stddev × field`

When adjusting momentum positions, use `momentum_tp_mult` / `momentum_sl_mult`. When adjusting MR positions, use `take_profit_pct × 100` / `stop_loss_pct × 100`. Never cross.

### Cross-Mode Init Placement (load-bearing)

**Sharded is the production engine mode; single_core is deprecated.** This shapes how new code lands.

`main.cpp` dispatches to `tt::EngineSharded_Run` near the top (~line 154) and **returns** from `main()`. Code AFTER the dispatch only runs in legacy mode. Code that should run in BOTH modes MUST be:

  (a) initialized BEFORE the dispatch in `main.cpp`, OR
  (b) called from inside `EngineSharded_Run` (in `EngineSharded.hpp`) too

**Verification gate**: when adding init code in `main.cpp`, ASK "does this need to run in sharded mode?" — if yes, place above the `engine_mode == ENGINE_MODE_SHARDED` dispatch. Quick check:

```bash
grep -n "engine_mode == ENGINE_MODE_SHARDED" main.cpp
# Your new init: line number must be ≤ that line, OR also called inside EngineSharded_Run.
```

**Things this affects** (must work in sharded): Depth WS thread + DepthRecorder, TickRecorder, NotifyState + g_notify, book_imbalance feed, any new background thread / shared global / recorder.

**Why this is load-bearing.** Phase 8a/8b shipped initialization in `main.cpp` AFTER the sharded dispatch — meaning sharded mode silently had none of those features. Caught at end of live-readiness coding (latency numbers + missing panels), fixed retroactively.

**Cross-architecture features**: some Phase features were wired on `PortfolioController` (legacy) and need equivalent wiring on `OrderManager_HandleFill` / `EventLoop_OnEvent` (sharded). Examples: maker/taker counters (legacy `DrainExits` → sharded `HandleFill`), confidence gate (legacy `Tick` → sharded ML strategy slow-path rebuild). When porting between architectures: same logic, different host struct.

### FPN-Only Accounting
Any code path that touches balance, P&L, fees, equity, or position pricing MUST use `FPN<F>` — never `double` or `float` for intermediate calculations. `double` is acceptable only at system boundaries:
- **OK**: `FPN_ToDouble` for display, logging, CSV output, printf
- **OK**: `FPN_FromDouble` at exchange API boundary (Binance returns doubles)
- **NOT OK**: `double equity = FPN_ToDouble(balance) + FPN_ToDouble(value)` for decision logic
- **NOT OK**: `double product = price_d * qty_d` before converting to FPN

**Known violations** (pre-existing, documented):
- `peak_equity`, `session_start_equity`, `max_drawdown` (kill switch fields)
- Kill switch equity/drawdown computation
- Confidence loop gate decision (`ConfidenceScorer` uses double throughout — IC, RMSE, freshness all double-valued; turning it FPN is out of scope)

### FPN Comparison Completeness
When comparing FPN values, use `FPN_LessThan`, `FPN_GreaterThanOrEqual`, etc. — never partial word comparisons. The inline optimization in `PositionExitGate` (Portfolio.hpp:226-229) only compares MSW and LSW, skipping middle words — known bug that can miss exits near price boundaries.

### Halt Flag Invariant
Every code path that suppresses buying MUST set `ctrl->buying_halted = 1` and zero `ctrl->gate_offset`. Ad-hoc zeroing of `buy_conds` alone is insufficient — hot-path gate tracking will restore it from `gate_offset` on the next tick.

### Confidence Loop Invariant

When `confidence_enabled=1` AND `strategy_id == STRATEGY_ML`:

1. **Every fill MUST push `(prediction, realized_return)` into `RollingIC` + `RollingRMSE`** via `ConfidenceScorer_Update`. Don't add second update sites — IC contamination = wrong confidence.
2. **Confidence MUST be computed inside the slow-path gate block**, before the buy-gate decision. Hot path may not call `ConfidenceScorer_Compute` (does Spearman ranking — O(W²)).
3. **Effective-threshold formula:** `effective_thr = base * (scale - conf)`, clamped to `≤ 1.0`. `scale = cfg.confidence_threshold_scale` (default 2.0). `base = cfg.ml_buy_threshold`. Modifying the formula → update the `controller_test.cpp` group "Phase 6prep: Gate effective threshold" in the same commit.
4. **Safe-by-default behavior on noise-floor models:** `abs_ic` clamps to `CONFIDENCE_MIN_IC_DEFAULT = 0.01`, freshness/stability stay near 1.0, so conf ≈ 0.01. Effective threshold ≈ `2.0 * base` — gate effectively never fires. ML strategy stays armed-but-inactive until real signal materializes.
5. **Confidence is read on slow path**, displayed via `last_confidence`. **NEVER read `last_confidence` from the hot path.**
6. **Tunables:** `cfg.confidence_window` (default 32, max 64), `cfg.confidence_freshness_tau` (default 300s), `cfg.confidence_threshold_scale` (default 2.0). Tuning meaningless on noise-floor models.

### Train-Serve Feature Parity (load-bearing)

The ML pipeline trains on features computed during sharded backtest replay (`BacktestSharded_Run`) and serves on features computed during sharded live execution (`EngineSharded_Run`). **Both paths MUST call `Regime_ComputeSignals` with equivalent state** or the model predicts on degraded inputs at serving time — silent train-serve drift, hard to detect.

`ModelFeatures_Pack` reads fields off `RegimeSignals + RollingStats`. Several require state beyond the rolling window: `ror_slope` (RORRegressor over slope history), `ema_sma_spread` / `ema_above_sma` (ema_price), `book_imbalance` / `spread_bps` / `spread_z` (depth feed), `flow_buy_ewma` / `flow_sell_ewma` / `large_trade_z` (flow EWMAs).

**The sharded engine maintains all of these** in `EngineSharded_Run` static-locals; `BacktestSharded_Run` mirrors them via the `ShardedBacktestDriver` callback hooks. Both threaded through `EventLoop_RebuildAllParameters` to ML strategies AND the feature-collection hook — single source of state.

**Adding a new feature to `ModelFeatures_Pack`:**
1. Define `FEAT_NEW_NAME` constant + bump `MODEL_NUM_FEATURES` + `MODEL_FORMAT_VERSION`
2. Add the field to `RegimeSignals<F>`
3. Populate it in `Regime_ComputeSignals` — single site
4. If new state required, add to BOTH `EngineSharded_Run` (live) AND `BacktestSharded_Run` (backtest), with parity in update cadence (per-tick vs slow-path). Both required because live + backtest run in different control flows even though they share `Regime_ComputeSignals` / `ModelFeatures_Pack` call sites.
5. Re-train all models — old models fail version check at load

**Other train-serve divergences (accepted):**
- Backtest is all-taker; live can be maker. Doesn't affect features.
- Backtest assumes full fills; live can have partials. Doesn't affect features.
- Tick timing — backtest replays CSV with stored timestamps, live has network jitter. Affects P&L from a given prediction, not the prediction itself.

**Adding a new depth-derived input.** Depth-state symmetry is at the public-API level: live reads `BookSnapshot<F>` via `DepthSharedState::snapshots[active].FIELD` (atomic), backtest reads same `BookSnapshot<F>` via `DepthReplayState::current.FIELD` (plain).
1. Add the field to `BookSnapshot<F>` (`DataStream/BinanceDepth.hpp`)
2. Compute in `depth_parse_json` (live)
3. Compute in `DepthReplayState_LoadDay` row-build block (backtest) — replay reads must agree with live for same input bid/ask/qty
4. Read on the slow path via `EventLoop_RebuildAllParameters` or `Regime_ComputeSignals`
5. Don't read on the hot path — depth state is slow-path only

**Future cleanup:** factor a shared "sharded slow-path init" helper that both `EngineSharded_Run` and `BacktestSharded_Run` call, collapsing two push sites to one.

### Maker/Taker Fee Accuracy

When applying fees:

1. **Fee charge sites** (booking the fee on a real fill) MUST use the per-fill rate. Either `Fee_Compute(cfg, notional, is_maker)` (cfg layer) or `oms->fee_rate_maker` / `oms->fee_rate_taker` (OMS layer). Never `FPN_Mul(notional, cfg.fee_rate)` — that's the legacy single-rate path.
2. **Source `is_maker` from the order**: live executionReport WS fills parse Binance "m" field; synchronous market BUY/SELL hardcodes `is_maker=0` (market = taker by definition); backtest hardcodes `is_maker=0` (all-taker simulation).
3. **Pre-trade quantity sites** (no-trade band, fee floor for TP, kill-switch estimate, spread_bps display) intentionally use the LEGACY `fee_rate` field — quantity in pre-trade computation, not fee charge on a real fill. Each such site has a `// Phase 8: pre-trade ... — leave as fee_rate` comment.
4. **Sanity invariant**: after every fill through synchronous path, `total_fees == total_maker_fees + total_taker_fees`.
5. **Cfg backward compat**: if user sets only `fee_rate` (legacy), it mirrors to both maker and taker at load time. If user sets `fee_rate` AND only ONE of maker/taker, a `[CFG] WARNING` fires.
6. **`ORDER_PARTIAL` is no longer a dead enum.** Adding code that does `if (state == ORDER_FILLED)` should consider whether `ORDER_PARTIAL` should also be handled. `Order_IsTerminal` correctly returns false for PARTIAL.

### Held-Out Validation Discipline

When training/evaluating an ML model in foxml_suite:

1. **Held-out test set is locked by default.** `HeldOutSplit_Make(total, fraction)` returns a struct with `locked=1`. `HeldOutSplit_TestAccessAllowed` returns 0 until `HeldOutSplit_Unlock(s, token)` is called with the correct token.
2. **Walk-forward CV runs ONLY on `[0, trainval_end_idx)`.** `Backtest_RunFullValidation` enforces this by passing a sliced view with `sample_count` capped at `trainval_end_idx`.
3. **Held-out evaluation runs ONCE per locked split.** Don't iterate on hyperparameters using held-out feedback. If you need a second evaluation, `HeldOutSplit_Relock` generates a new token.
4. **Generalization gap is the WAS-IT-REAL test:** `|WF_mean_val - held_out|`. Default threshold `cfg.gap_acceptable_threshold` (0.05) means gap > 5% = walk-forward was overfit. **Models with gap > threshold should not ship.**
5. **`expected.cfg` saves the discipline values** (`held_out_fraction`, `gap_acceptable_threshold`) alongside the model bundle.
6. **Token is friction not security.** Goal is "make accidental peeking impossible, intentional peeking auditable" — discipline mechanism, not a permission system. Resist any "ergonomic" change that weakens the lock.

### Operational Alerting

When adding a new alertable event:

1. Add a new `NK_*` kind to the `NotifyKind` enum in `Notify.hpp`. **Append-only** — never reorder; cooldown indexes are stable per-kind.
2. Call `Notify_Send(g_notify, level, kind, subject, body)` alongside the existing `fprintf`. Keep the `fprintf` (file logs are forensic record).
3. Levels: `NOTIFY_INFO`, `NOTIFY_WARN`, `NOTIFY_ALERT` (user attention), `NOTIFY_CRITICAL` (engine cannot continue).
4. Use the SAME `NK_*` kind for the same logical event everywhere — cooldown is per-kind.
5. **NEVER call `Notify_Send` from the hot path.** Slow path / dedicated threads only.
6. Subject ≤ 128 chars, body ≤ 512 chars. Shell-escaped (internal `'` → `'\''`) when Command backend is in use; `"` and `\` are NOT JSON-escaped — keep alert text plain ASCII.
7. Guard call sites with `if (g_notify)` — backtest and tests leave it null, all calls become no-ops.

### Regime Adjustment Checklist
When adding a new regime transition case in `Regime_AdjustPositions`:
1. Guard stddev != 0 at function entry
2. Use the correct config field family (momentum_*_mult for momentum positions, *_pct×100 for MR)
3. After all TP/SL mutations, re-check SL floor and TP floor
4. Verify FPN_Max vs FPN_Min direction:
   - **Tighten TP** (closer to entry) = `FPN_Min`
   - **Widen TP** (further from entry) = `FPN_Max`
   - **Tighten SL** (closer to entry) = `FPN_Max` (SL < entry, pick higher)
   - **Widen SL** (further from entry) = `FPN_Min`
5. Add a regression test for the new transition

### Label-type-aware metric invariant (load-bearing — Backtest Suite)

**Rule:** every metric, display, training, or validation site that touches label values MUST consult `label_table[t].num_classes` (via `LabelType_IsBinary` / `LabelType_IsRegression` / `LabelType_IsMulticlass` helpers in `LabelFunctions.hpp`) and branch on the kind. Never hardcode binary classification assumptions.

**The four label kinds and their metric semantics:**

| `num_classes` | Kind | Label values | XGBoost objective | Primary metric | Overfit detector |
|---|---|---|---|---|---|
| 0 | binary | {0.0, 1.0}, optionally 0.5=neutral (filtered) | `binary:logistic` + `scale_pos_weight` | accuracy [0,1] | `OverfitDetection_CheckDefaults` |
| 1 | regression | continuous | `reg:squarederror` | Pearson correlation r | `OverfitDetection_CheckRegressionDefaults` |
| ≥2 | multiclass | integer class ids 0..K-1 (as float) | `multi:softprob` + `num_class=K` | argmax accuracy | `OverfitDetection_CheckDefaults` |

**Sites that must branch on label kind:** Sample panel display (`GUI_Panel_Training` collection summary); Train Model in-sample metric; Walk-Forward `Backtest_RunWalkForward` (neutral filter, XGBoost objective + `num_class`, `scale_pos_weight`, per-fold metric, aggregate `WalkForwardResults`); Walk-Forward result display; Overfit detection; Save Run / `expected.cfg` writer.

**Why this is load-bearing.** 2026-04-25 — Forward P&L (regression label) reported `+: 0 / -: 2,254,869 / Ratio: 0.0%`, `Train Accuracy: 0.2%`, walk-forward `0.0%/0.0%/0.0%` per fold. None meaningful — binary-classification metrics on continuous regression labels. The fix wired all six sites to branch on kind.

**Future hardening (deferred):** turn `num_classes` into `enum class LabelKind` so the compiler exhaustive-checks switches.

### Snapshot Re-Activation Invariant (load-bearing — 2026-04-27)

**Rule:** when `ShardedSnapshot_Load` restores `portfolio.active_bitmap` + `portfolio.positions[slot]`, it MUST also re-activate the matching `ExecutionCore<F>` hot-path mirrors (`active`, `entry_price`, `live_tp`, `live_sl`). Otherwise restored positions are "zombie" — open in the portfolio but cannot exit because hot path's `can_exit = active & sg_fires` evaluates to 0.

**Why this is load-bearing.** 2026-04-27 — positions stayed open after price dropped below SL. Chart showed SL trigger markers but Positions panel kept showing them open. Root cause: snapshot persisted `Position` fields but NOT the ExecutionCore mirrors that the hot path reads.

**Fix at `CoreFrameworks/ShardedSnapshotPersist.hpp`** — after portfolio + core-context restoration, walk the bitmap:

```cpp
uint16_t bm = state->oms->portfolio.active_bitmap;
while (bm) {
    int slot = __builtin_ctz(bm);
    bm &= (uint16_t)(bm - 1);
    if (slot < 0 || slot >= (int)state->registered_count) continue;
    ExecutionCore<F>* core_ptr = state->cores[slot].core;
    if (!core_ptr) continue;
    const Position<F>& pos = state->oms->portfolio.positions[slot];
    core_ptr->entry_price = pos.entry_price;
    core_ptr->live_tp     = pos.take_profit_price;
    core_ptr->live_sl     = pos.stop_loss_price;
    core_ptr->active      = 1;
}
```

**When adding new ExecutionCore hot-path state (e.g. partial exits' `live_tp_b`/`active_b`):** the snapshot loader needs to handle them too. Either persist + restore, OR reset to safe defaults (snapshot v8 does not include leg-B fields; a snapshot-while-paired needs a v9 schema bump).

### Snapshot Tick-Counter Drift (load-bearing — 2026-04-27)

**Rule:** any slow-path code that subtracts a snapshot-persisted tick counter from the current `ticks_produced` MUST guard against `entry_t > now_tick` (uint64 underflow). The persisted counter survives engine restart; the live counter resets to 0. Without the guard, `now_tick - entry_t` underflows to ~2^64 → time-exit thresholds trivially pass → spurious force-close orders fire every cycle.

**Pattern (`EngineSharded.hpp` time-exit block, ~line 1110):**

```cpp
uint64_t entry_t = state.cores[slot].last_entry_tick;
if (entry_t == 0) continue;
if (entry_t > now_tick) {
    fprintf(stderr, "[sharded] core %d: stale entry_tick from snapshot ...\n", slot);
    state.cores[slot].last_entry_tick = now_tick;
    continue;
}
uint64_t elapsed = now_tick - entry_t;
```

**Why this is load-bearing.** 2026-04-27 — `engine.log` spammed with `time-exit (held 18446744073709536760 ticks, gain -X.X%)` — held count is approximately 2^64 minus a small offset. Spurious SELL submissions piled up against the genuine SL exit, blocking the slot from clearing.

### Partial Exits — Two-Position-per-Core (load-bearing — 2026-04-27)

**Architectural invariant:** when `cfg.partial_exit_enabled=1`, each sharded execution core owns TWO portfolio slots:
- core `c` → leg A in slot `2c`, leg B in slot `2c+1`
- max cores = `MAX_PORTFOLIO_POSITIONS / 2` = 8 (validated at boot via `Sharded_ValidatePartialExitCfg`)

When `cfg.partial_exit_enabled=0` (default): core `c` → slot `c` (1:1), slot `2c+1` is unused. `Sharded_LegSlot(core_id, leg, partial_enabled)` returns the correct slot for both modes.

**Slot mapping helper — `CoreFrameworks/ControllerEventLoop.hpp`:**
```cpp
Sharded_LegSlot(core_id, PARTIAL_LEG_A, partial_enabled)  // → core_id (off) or 2c (on)
Sharded_LegSlot(core_id, PARTIAL_LEG_B, partial_enabled)  // → -1 (off) or 2c+1 (on)
```
`PARTIAL_LEG_A` / `PARTIAL_LEG_B` constants live in `CoreFrameworks/TradeEvent.hpp` so `ExecutionCore_Tick` can use them without including the EventLoop header.

**Hot path — `ExecutionCore_Tick`:** branch-gated leg B SG check via `if (__builtin_expect(active_b, 0))`. When `active_b=0` (steady state when partials disabled OR partials enabled but no leg-B open), the leg-B FPN comparisons don't execute — preserves baseline latency. Latency cost when leg B active: ~1-2ns added per tick.

**Slow path — strategy dispatcher cap (`Strategies/StrategyParameters.hpp:Strategy_BuildParameters`):** every strategy's `_BuildParameters` writes the leg-A fields (`tp_pct`, `sl_pct`); the dispatcher applies a uniform post-cap that sets `GATE_FLAG_PAIR_ACTIVE` + `tp_pct_b = tp_pct * cfg.tp2_mult` when `cfg.partial_exit_enabled=1`. When disabled, dispatcher explicitly clears both. Adding a new strategy: do NOT write `tp_pct_b` or `GATE_FLAG_PAIR_ACTIVE` — the dispatcher handles partials uniformly.

**OMS drainer — `EngineSharded.hpp:drain_with_submit`:** maps each `TradeEvent` to a portfolio slot via `Sharded_LegSlot(event.core_id, event.leg, cfg.partial_exit_enabled)`. For entries, splits `intended_qty` by `cfg.partial_exit_pct` (leg A gets `partial_pct`, leg B gets `1 - partial_pct`). For exits, reads qty from the LEG's `portfolio.positions[portfolio_slot].quantity`. The `core_id` parameter to `OrderManager_Submit` is the actual portfolio slot; `event.leg` is propagated to `Order::leg` for trade-log observability.

**Per-core counters / state stamping** (`last_entry_tick`, `last_entry_price`, `active_prediction`): updated only on **leg A** entry events (one trade = one stamp). Leg B is the same trade's second slot; double-stamping would skew spacing checks + ConfidenceScorer feedback.

**`tp2_mult` defensive default:** when 0 or `tp_pct=0`, dispatcher falls back to `tp_pct_b = tp_pct` (leg B duplicates leg A, effectively a no-op). User cfgs should set `tp2_mult > 1.0` for meaningful partials (default cfg = 2.0).

**What's deferred:**
- `breakeven_on_partial=1` semantics — slow path should ratchet leg B's SL to entry_price after leg A's TP1 fires. Currently leg B's SL stays at original shared SL.
- Snapshot persistence of `live_tp_b` / `active_b` / `entry_price_b` — current snapshot v8 doesn't include these. Failure mode: leg B closes at next live tick that hits its TP/SL, matching single-leg semantics.

**Toggle:** `cfg.partial_exit_enabled = 0` (default) preserves all pre-partial-exit behavior. Validation refuses boot if enabled with too many cores. Rollback tag: `pre-partial-exits` at `abd08d3`.

## Current State (2026-04-27 — v4.7.0)

**Branch:** `experiment/per-core-sharding` (main). v4.4.0 shipped Track E (sharded backtest unification — legacy backtest body deleted, sharded is the only path); v4.5.0 shipped Wave 1 microstructure features (D.1 book imbalance over time, D.2 flow EWMAs, D.4 large-trade z-score); v4.6.0 shipped Wave 2 spread dynamics (D.3 spread bps + zscore); v4.7.0 shipped partial exits (P.1–P.5 — two-position-per-core).

Two snapshot bug fixes shipped 2026-04-27:
- **Time-exit underflow guard** (commit `b86b17f`) — entry_t > now_tick from snapshot drift was underflowing to ~2^64; spurious time-exit submissions.
- **Snapshot ExecutionCore re-activation** (commit `777f843`) — restored positions had `active=0` on the core; hot-path SG never fired → zombie positions.

**Engine capability summary:**
- Sharded engine: production. Per-core ExecutionCore (40-400ns p99 idle, 460-567ns p99 measured live), per-core strategy + per-core ML model + per-core risk allocation + per-core ConfidenceScorer.
- Hot-swap strategy per core (GUI dropdown + Apply).
- Live trading: architecturally complete (full OMS, paper/live sync, kill switch, orphan recovery, reconnect, maker/taker accounting, depth recording, alerting, partial exits). Live-data is the default — `cd build_gui && ./engine_gui` connects to Binance out of the box.
- ML pipeline: end-to-end working in sharded path. ConfidenceScorer per core; armed-but-inactive on noise-floor models via `effective_thr = base * (scale - conf)`.
- Microstructure features: 9 fields wired in both serve + backtest paths (book imbalance, flow EWMAs, large-trade z, spread dynamics).
- Legacy single-threaded LIVE: deprecated with runtime warning. Phase 8+ features may be incomplete (see Cross-Mode Init Placement).

**Tests:** controller_test 640+ assertions passing.

**Build state:** all targets clean — `build/engine`, `build_gui/engine_gui`, `build_gui/foxml_suite`, `build/controller_test`.

**Current focus:** validate with $10 paper soak, find ML signal. Legacy live engine deletion + cleanup tracked in `plans/legacy-deprecation-cleanup.md`.

## Key Design Decisions

1. Portfolio uses `uint16_t` bitmap (not sequential count) — same pattern as OrderPool
2. Per-position TP/SL exits on hot path, portfolio management on slow path
3. Fill consumption happens EVERY tick (zero unprotected exposure)
4. Single-slot mode (max_positions=1): exchange BTC balance IS the position, sell-all eliminates dust
5. Multi-slot fallback: per-position sells when max_positions > 1 (dust may accumulate)
6. Warmup phase observes market before trading (gates on slow-path sample count, not raw ticks)
7. 24-hour session lifecycle: warmup → trade → wind down → close all → reconnect
8. TUI is independent of engine (engine runs headless, TUI only reads state)
9. No API key needed for market data websocket (public endpoint)
10. RollingStats computes regression inline (no separate feeder for regime R²)
11. `RegimeSignals` struct is the extensibility point — new signal = new field + one comparison
12. Partial exits: dispatcher post-cap so strategies stay leg-A-only; hot path branch-gates leg B for steady-state latency
