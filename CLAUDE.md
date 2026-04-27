# CLAUDE.md

## Overview

Tick-level crypto trading engine in C++. Branchless fixed-point arithmetic, bitmap-based portfolio management, regression-driven gate adjustment with regime detection. Single-symbol, single-threaded hot path with multicore TUI.

## Build

**Wrapper script (preferred):** `./build.sh` provides single entry point for all build variants.

```bash
./build.sh test    # build engine + run controller_test (279 assertions)
./build.sh gui     # build engine_gui + foxml_suite
./build.sh suite   # build foxml_suite with XGBoost (requires libxgboost)
./build.sh all     # build engine + gui (skip suite by default)
./build.sh clean   # wipe all build dirs
```

**Direct cmake (if you need it):**
```bash
cmake -B build && cmake --build build         # production (ANSI TUI, no deps)
./build/controller_test                        # run tests
cd build && ./engine                           # run engine (needs engine.cfg symlink)
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

**Default engine mode: SHARDED (per-core).** N execution cores (default 4,
cap 16), each with its own ExecutionCore + parameters + tick ring + slot
in the central OMS portfolio. Branchless ~60ns hot path per core. Producer
thread pinned to one CPU, fans real Binance ticks (or synthetic for
offline benchmark) across SPSC rings to per-core consumer threads.

**Sharded mode key properties:**
- Per-core strategy (`core_N_strategy=simple_dip|momentum|ema_cross|ml`)
- Per-core ML model (`core_N_model_path=...` or `core_N_model_dir=...` for
  a CoreModelZoo with auto-discovered roles barrier/buy_signal/regime/exit)
- Per-core risk allocation (`core_N_risk_pct=...`, default = `risk_pct / N`)
- Per-core ConfidenceScorer (when STRATEGY_ML in use, Phase 6prep wiring per controller)
- Risk distributed: a single bad core can lose its allocation, not the account
- Hot path p99 target: ≤500ns per core
- No portfolio walk on the hot path — each core only owns its slot

**Legacy single-threaded LIVE mode** (`engine_mode=single_core` in
`main.cpp`) remains available as a benchmark + regression baseline
but is **DEPRECATED**. A runtime warning fires at startup. Phase 8+
features may be incomplete in legacy live mode (see "Cross-Mode Init
Placement" invariant under Safety Invariants — adding init in
main.cpp post-dispatch silently skips the sharded path).

**Legacy BACKTEST mode is GONE** as of Track E.7 (2026-04-26).
`Backtest_Run` is now a thin wrapper around `BacktestSharded_Run`.
Setting `engine_mode=single_core` in `backtest.cfg` is a no-op going
forward — the cfg field is parsed but ignored for one release cycle.

```
LEGACY MODE (deprecated benchmark path):
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
2. `DataStream/EngineTUI.hpp`: populate in `TUI_CopySnapshot()` (live engine path)
3. **Backtest auto-syncs** — `Backtest/BacktestSnapshot.hpp::BacktestSnapshot_Copy()` is a thin wrapper that calls `TUI_CopySnapshot` and overrides only `live_trading=0`. New fields are inherited automatically.
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

**Snapshot sync rule (simplified 2026-04):**
`BacktestSnapshot_Copy` is now a thin wrapper around `TUI_CopySnapshot`. Adding a field to `TUISnapshot` requires updating ONE function:
- `DataStream/EngineTUI.hpp` → `TUI_CopySnapshot()` (live engine path)

The backtest path inherits automatically via the wrapper. The previous "update both" rule is obsolete — historical changelogs may still reference it.

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

### Active rollback tags (as of 2026-04-25)
- `pre-zoo` (`46b5a25`) — before all Phase 5 ML zoo work
- `pre-label-type-fix` (`2b27707`) — Saturday evening, before Sunday's label-type-aware metric overhaul
- `pre-hardening` (`8d175b1`) — Sunday morning, before afternoon hardening pass

After Phase 6 merge: `main-backup-2026-04-25`, `phase5d-merged`. Subsequent phase tags per `plans/live-readiness-master.md`.

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

## Plan Review Checklist (load-bearing — apply to every multi-day plan)

Before starting any plan that spans more than a single commit, walk it
through this checklist. Findings → either revise scope, add a mitigation,
or document an accepted exception in the plan itself. **Audit BEFORE
coding**, not after — every audit-after-the-fact in this codebase has
caught real bugs that would have shipped.

### 1. Hot path purity
- Does this add code to `ExecutionCore_Tick`, `BG_Evaluate`, `SG_Evaluate`,
  or any per-tick callsite?
- If yes, is every operation **branchless** (no `if`, only mask-select),
  **FPN-only** (no `double` arithmetic), and **alloc-free** (no malloc /
  syscall)?
- Hot path latency budget is **≤500ns p99**. Currently 40-400ns.
  Adding ≥10ns requires explicit justification in the plan.
- Default answer: hot path stays untouched. New code goes on slow path.

### 2. Train-serve parity
- Does this affect `RegimeSignals` fields or `ModelFeatures_Pack`?
- If yes, does **both** the backtest path AND the sharded live path
  populate the new state with **equivalent cadence and inputs**?
- New features must produce identical values for identical input
  streams in both paths. Document any accepted divergence (e.g.,
  maker/taker fees, tick timing) explicitly.
- Sites that may need updating: `PortfolioController_Tick`,
  `EngineSharded_Run` fan_out, `ML_BuildParameters`, `Regime_ComputeSignals`,
  `ModelFeatures_Pack`. Any feature requiring depth data must work in
  both paths or be deferred until depth replay exists in backtest.

### 3. Surface area / coupling
- How many files / call sites does this change?
- Are we adding `if (live_trading)` branches anywhere? (Smell — usually
  means the abstraction boundary is wrong.)
- For new optional state, does it follow the existing pattern (heap-
  allocated, NULL-init in caller, freed in cleanup)?
- The right shape is: new state owns its lifecycle in ONE place, all
  consumers receive it via params. Adding a feature should NOT require
  touching 5+ unrelated files.
- **Forward-thinking test**: how many sites would the next similar
  feature need to modify? Aim to reduce that count, not match it.

### 4. Pointer init + heap lifecycle
- Any new heap-allocated pointer field on `PortfolioController` /
  `OrderManagerState` / `EventLoopState` / `CoreContext`?
- If yes:
  - Every caller of `*_Init` must `NULL` the pointer first (4 sites
    today: `main.cpp:218`, `main.cpp:657`, `Backtest/BacktestEngine.hpp:507`,
    plus tests using `= {}` zero-init).
  - `_Init` must `if (ptr) free(ptr)` before re-allocating (handles
    re-init / 24h reconnect path).
  - Cleanup path must free + NULL on shutdown.
  - Snapshot persistence must include the new state (or document why
    it's session-only).
- This is the doctrine behind the v4.3 segfault — three sites needed
  the same NULL-init line, two had it, one didn't.

### 5. Backward compatibility
- Does this break existing saved data?
  - Snapshots: `SHARDED_SNAPSHOT_VERSION` bump → old files refused.
    OK if intentional, document in changelog.
  - Models: `MODEL_FORMAT_VERSION` bump → old `.json` models fail load.
    Always document the FEAT_* additions / changes in the changelog.
  - Saved Runs: extending `summary.txt` / `expected.cfg` is forward-
    compat (old fields still parse). Removing fields breaks Past Runs.
  - Cfg files: adding new fields is fine (default in `_Default`,
    parser falls through). Removing parsed fields breaks user cfgs.

### 6. Multi-threading correctness
- New shared state? Identify the producer + consumer threads.
- SPSC ring? Verify single producer, single consumer.
- Atomic vs non-atomic — anything multi-thread-shared without
  `std::atomic` or explicit `__atomic_*` is a race.
- Race conditions to specifically check: producer + drainer on OMS
  fields, slow path + hot path on GateParameters (use seqlock).
- Backtest mode determinism: even if the engine is multi-threaded,
  backtest output should be reproducible run-to-run for tuning to be
  meaningful. Synchronous tick replay (producer waits for executor)
  is one mitigation.

### 7. Test coverage
- Is there a "hammer test" that exercises the round-trip / N-iteration
  case? (Phase 2.1 cumdelta, v4.3 CumDelta_Push wraparound — these
  catch symmetry / wraparound bugs the next time someone touches the
  code.)
- Does the test cover **edge cases**: cold start (count==0), full
  window (count==WINDOW), wraparound (count > WINDOW), zero inputs,
  uninitialized fields?
- Does it run in `controller_test` (the 482-assertion default)?
- Migration plans (e.g., Track E) need parity tests: run BOTH old
  and new paths on the same input, diff feature outputs.

### 8. Docs + invariants
- Does this introduce a load-bearing rule that future devs need to
  know? If yes, add a section to "Safety Invariants" below or extend
  this checklist.
- Update "Current State" with the new capability.
- Add a dated changelog entry under `DOCS/changelogs/`.
- If a plan is significant, write it to `plans/{name}.md` (gitignored)
  and commit-message-link it.

### 9. Forward maintenance
- Will this require touching 30+ sites to extend later? If yes,
  redesign for lower coupling.
- Will the next similar feature copy-paste this code? If yes,
  factor a helper / template / generic pattern.
- Will future engine changes likely break this? Identify the
  brittle assumption (e.g., "assumes max_positions=1") and document
  it in the comment.

### 10. Rollback story
- Tag `pre-{name}` before starting. Push to remote.
- For multi-week plans, branch `backup/pre-{name}-{date}` so the
  full state survives even if tags get reorganized.
- Each phase commit should be individually revertable.
- Plan document records what was tried and why, so a rollback later
  has context.

### Audit verdicts vocabulary

When applying this checklist, label findings consistently:

- **PASS ✅**: requirement met
- **FIXED ✅**: was an issue, patched in the same pass
- **GAP** ⚠️: real concern, must address before plan ships
- **DRIFT** ⚠️: pre-existing condition the plan inherits — note + mitigate
- **DEFERRED**: scoped out, has explicit follow-up
- **ACCEPTED**: known divergence, documented and lived with

This vocabulary keeps audit reports calibrated. "Issue" / "concern" /
"problem" are too vague — these labels say what should happen next.

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

### Cross-Mode Init Placement (load-bearing — Phase 13+)

**Sharded is the production engine mode; single_core is deprecated.** This
shapes how new code lands.

`main.cpp` dispatches to `tt::EngineSharded_Run` near the top (~line 154)
and **returns** from `main()`. Code AFTER the dispatch only runs in legacy
mode. Code that should run in BOTH modes MUST be:

  (a) initialized BEFORE the dispatch in `main.cpp`, OR
  (b) called from inside `EngineSharded_Run` (in `EngineSharded.hpp`) too

**Verification gate**: when adding init code in `main.cpp`, ASK "does this
need to run in sharded mode?" — if yes, place above the
`engine_mode == ENGINE_MODE_SHARDED` dispatch. Quick check:

```bash
grep -n "engine_mode == ENGINE_MODE_SHARDED" main.cpp
# Your new init: line number must be ≤ that line, OR also called inside
# EngineSharded_Run.
```

**Things this affects** (must work in both modes):
- Depth WS thread + DepthRecorder (Phase 8a)
- TickRecorder
- NotifyState + g_notify (Phase 8b)
- book_imbalance feed into per-core controllers
- Any new background thread, shared global, or recorder

**Why this is load-bearing.** Phase 8a/8b shipped initialization in
`main.cpp` AFTER the sharded dispatch — meaning sharded mode silently had
none of those features. Caught by direct observation (latency numbers +
missing panels) at the end of live-readiness coding, fixed retroactively.
The rule is now in code: doctrine in this file + the runtime warning when
legacy mode boots.

**Cross-architecture features** (not just init placement): some Phase
features are wired on `PortfolioController` (legacy controller) but need
equivalent wiring on `OrderManager_HandleFill` / `EventLoop_OnEvent`
(sharded controllers). Examples:
- Phase 8 c5 maker/taker counters — port from `PortfolioController_DrainExits`
  to `OrderManager_HandleFill`
- Phase 6prep confidence gate — port from `PortfolioController_Tick` to
  the ML strategy slow-path rebuild in EngineSharded

When porting between architectures: same logic, different host struct.
Keep tests in both modes' code paths so regressions are visible.

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
- **Confidence loop gate decision** (PortfolioController.hpp:~1618, added 2026-04-25 Phase 6prep doc): `effective_thr = base * (scale - conf)` with `base`, `scale`, `conf` all in `double`. The `ConfidenceScorer` (`ML_Headers/ConfidenceScore.hpp`) uses double throughout (IC, RMSE, freshness all double-valued metrics). Pre-existing — Phase 6prep just made `scale` cfg-tunable, didn't change the type. Fix would require turning ConfidenceScorer math into FPN throughout, out of scope.

### FPN Comparison Completeness
When comparing FPN values, use `FPN_LessThan`, `FPN_GreaterThanOrEqual`, etc. — never partial word comparisons. The inline optimization in `PositionExitGate` (Portfolio.hpp:226-229) only compares MSW and LSW, skipping middle words — this is a known bug that can miss exits near price boundaries.

### Halt Flag Invariant
Every code path that suppresses buying MUST set `ctrl->buying_halted = 1` and zero `ctrl->gate_offset`. Ad-hoc zeroing of `buy_conds` alone is insufficient — hot-path gate tracking will restore it from `gate_offset` on the next tick.

### Confidence Loop Invariant (Phase 6prep)

When `confidence_enabled=1` AND `strategy_id == STRATEGY_ML`:

1. **Every fill MUST push `(prediction, realized_return)` into `RollingIC` + `RollingRMSE`** via `ConfidenceScorer_Update`. Already wired in `PortfolioController_HandleFill` at the post-exit accounting block. Don't add second update sites — IC contamination = wrong confidence.
2. **Confidence MUST be computed inside the slow-path gate block at `PortfolioController.hpp:~1614`**, before the buy-gate decision. Hot path may not call `ConfidenceScorer_Compute` (does Spearman ranking — O(W²) on rank computation; fine on slow path, not on every tick).
3. **Effective-threshold formula:** `effective_thr = base * (scale - conf)`, clamped to `≤ 1.0`. `scale` is `cfg.confidence_threshold_scale` (default 2.0). `base` is `cfg.ml_buy_threshold`. Modifying the formula = update the test in `controller_test.cpp` Group "Phase 6prep: Gate effective threshold" in the same commit.
4. **Safe-by-default behavior on noise-floor models:** when IC is near zero (no real signal), `abs_ic` clamps to `CONFIDENCE_MIN_IC_DEFAULT = 0.01`, freshness/stability stay near 1.0, so conf ≈ 0.01. Effective threshold ≈ `2.0 * base` — gate effectively never fires. **This is desirable** — ML strategy stays armed-but-inactive until real signal materializes.
5. **Confidence is read on slow path, displayed via `last_confidence`.** `last_confidence` is updated on every gate decision; TUI/GUI read the snapshot field. **NEVER read `last_confidence` from the hot path.**
6. **Tunables (Phase 6prep):** `cfg.confidence_window` (default 32, max 64), `cfg.confidence_freshness_tau` (default 300s = 5min), `cfg.confidence_threshold_scale` (default 2.0). All preserve pre-Phase-6prep hardcoded behavior at default values. Tuning these requires an actual signal to A/B against — meaningless on noise-floor models.

### Train-Serve Feature Parity (load-bearing — v4.0.1)

The ML pipeline trains on features computed during backtest replay
(legacy `PortfolioController` slow path) and serves on features
computed during live execution (sharded `EventLoopState` slow path).
**Both paths MUST call `Regime_ComputeSignals` with equivalent state**
or the model predicts on degraded inputs at serving time — silent
train-serve drift, hard to detect because it produces "real-looking"
predictions that just don't match what the model learned.

`ModelFeatures_Pack` reads 16 fields off `RegimeSignals + RollingStats`.
Three of them require state beyond the rolling window:
- `sig.ror_slope` — needs a `RORRegressor` of the rolling slope history
- `sig.ema_sma_spread` — needs an `ema_price` (exponentially smoothed)
- `sig.ema_above_sma` — derived from `ema_sma_spread`

**The sharded engine maintains both** as static locals in
`EngineSharded_Run`:
- `RORRegressor<F> regime_ror` — pushed each slow path in `fan_out`
  with `LinearRegression3XResult{slope, intercept=0, r_squared}`.
  Mirrors `PortfolioController.hpp:~1552`.
- `FPN<F> ema_price` — updated **every tick** in `fan_out` (not slow
  path). Branchless first-tick: if zero, take current price; else
  apply `ema_alpha * old + (1-alpha) * new`. Mirrors `PortfolioController_Tick`.

These are passed to `EventLoop_RebuildAllParameters` via the new
optional `ror_regressor` + `ema_price` parameters and threaded through
`MLBuildContext` to `ML_BuildParameters`. When both are non-null,
`ML_BuildParameters` calls `Regime_ComputeSignals` (single source of
truth for the feature pack). When null (legacy callers / tests), it
falls back to inline minimal population — preserves prior behavior
for those callers.

**Why this is load-bearing.** Pre-v4.0.1, the sharded path inline-
populated a smaller subset of `RegimeSignals`. The three fields above
stayed at zero. A model trained with non-zero values would see all
zeros at inference. Predictions diverge. Caught by direct audit
question ("are you sure the sharded engine produces the same features
the backtest does?") — could have shipped silently otherwise.

**Adding a new feature to `ModelFeatures_Pack`:**
1. Define `FEAT_NEW_NAME` constant + bump `MODEL_NUM_FEATURES` +
   `MODEL_FORMAT_VERSION` (existing rules)
2. Add the field to `RegimeSignals<F>`
3. Populate it in `Regime_ComputeSignals` — that's the single site
4. If the field needs new state (like ROR did), add it to BOTH
   `EngineSharded_Run` (sharded live path) AND `BacktestSharded_Run`
   (sharded backtest, mirrors via `ShardedBacktestDriver`), with
   parity in update cadence (per-tick vs slow-path). Post-Track-E.7
   the legacy `PortfolioController` push site is deleted; only the
   two sharded paths remain.
5. Re-train all models — old models will fail version check at load

**Other train-serve divergences (documented but accepted):**
- Backtest is all-taker; live can be maker. Doesn't affect features
  (model doesn't use fee-derived features).
- Backtest assumes full fills; live can have partials. Doesn't affect
  features.
- Tick timing — backtest replays CSV with stored timestamps, live has
  real network jitter. Affects P&L from a given prediction, not the
  prediction itself.

**Track E — sharded backtest unification (in progress, 2026-04-26+):**

The "add to BOTH paths" rule above is being retired. Track E completes
`BacktestSharded_Run` so that backtest *uses* the same
`Regime_ComputeSignals` call site as the live serve path. Once E.7
ships, "did we update both paths?" stops being a question — there is
one path. Plan: `plans/track-e-sharded-backtest.md`.

Wired now (E.1 + E.2 + E.3, 2026-04-26):
- **Feature collection** runs through `ShardedBacktestDriver::on_slow_path`
  (registered by `BacktestSharded_Run` when `collect_features=1`). The
  callback runs `Regime_ComputeSignals` with the same args as
  `StrategyParameters.hpp:469` (the live ML serve path) and packs via
  `ModelFeatures_Pack`. New `RegimeSignals` fields populated in
  `Regime_ComputeSignals` light up automatically in sharded backtest.
- **Strategy + risk + ML model wiring** in `BacktestSharded_Run` mirrors
  `EngineSharded_Run` (lines 559-648). `cfg.core_strategies[i]` and
  `cfg.core_risk_pct[i]` are honored. ML cores load `CoreModelZoo` +
  init `ConfidenceScorer` with the same cfg tunables. Adding a strategy
  to `EventLoop_RebuildAllParameters` dispatch picks up
  sharded-backtest support automatically — no separate plumbing.
- **Warmup gate** matches live: cores start at permission=0, granted
  when `rolling.count >= cfg.min_warmup_samples`. Pre-E.2 backtest set
  permission=1 immediately, which let strategies fire on garbage
  rolling stats during the first ticks.
- **Per-tick EMA** + **slow-path RORRegressor / CumDelta / TickRate**
  pushes mirror `EngineSharded_Run` static-local state. Driver threads
  these via `EventLoop_RebuildAllParameters` to ML strategies AND the
  feature-collection hook — single source of state.
- **Depth replay (E.3)** + **`book_imbalance` buy gate**. New
  `DepthReplayState<F>` (`DataStream/DepthReplayState.hpp`) reads
  the daily CSVs `DepthRecorder` writes (Phase 8a) and exposes a
  `BookSnapshot<F>` whose `imbalance` field consumers read identically
  to `DepthSharedState::snapshots[active].imbalance` in the live
  engine. `BacktestSharded_Run` advances the replay state in lockstep
  with the tick stream and pipes `imbalance` through to
  `EventLoop_RebuildAllParameters` via the new optional
  `book_imbalance` parameter. The live path (`EngineSharded_Run`,
  formerly the missing post-Phase-8a "coding c14") now reads from
  `g_depth_shared` symmetrically. Both paths converge on a new
  `GATE_FLAG_BUY_BLOCKED` flag that BG_Evaluate vetoes via a 1ns
  branchless mask (works for buy-above and buy-below strategies, unlike
  the legacy `zero_gate(reason)` pattern which only zeros
  `bg_price_threshold` — latent bug for momentum, separate fix). When
  `cfg.min_book_imbalance==0` the gate is inert (default cfg). Edge
  case: missing depth file for a tick file's date → `file_present=0`,
  `imbalance` stays at zero → with `min>0` the gate fails closed (no
  buys until the data lands), matching live's "no depth → don't trade"
  semantics.

Inherited by routing (E.4 + E.5, no migration code needed):
- **Walk-Forward / FullValidation** (`Backtest_RunWalkForward`,
  `Backtest_RunFullValidation`): read `BacktestResults.feature_matrix`
  directly. They never call the engine — the engine ran upstream
  during the data-collection pass that populated `BacktestResults`.
  When `engine_mode=sharded`, that upstream pass is now
  `BacktestSharded_Run` (E.1's dispatcher), so WF + FullValidation
  see sharded features automatically. Verified: only call site is
  `BacktestPanels.hpp:1421` consuming `state->results` populated by
  `BacktestPanels.hpp:217`'s `Backtest_Run` call.
- **Sweep / Optimizer** (`Backtest_RunSweep`): iterates over config
  combinations and calls `Backtest_Run` per iteration
  (`BacktestEngine.hpp:1626`). Each call routes through the
  dispatcher, so optimizer runs honor `engine_mode=sharded` per
  config. The `use_config_override + config_override` mechanism
  preserves user-set overrides including `engine_mode`.

Track E status: complete (E.1–E.7 all shipped 2026-04-26). Sharded
is the only backtest path. `Backtest_Run` is a thin wrapper around
`BacktestSharded_Run`. The legacy `PortfolioController`-driven body
is deleted (~350 LOC removed).

Follow-up cleanup (separate release):
- Drop `engine_mode` cfg parser. Currently parsed but ignored — one
  release cycle of grace so user cfgs setting `engine_mode=single_core`
  don't fail to load.
- Optional: factor a shared "sharded slow-path init" helper that both
  `EngineSharded_Run` and `BacktestSharded_Run` call, so future
  per-feature state additions land in one site instead of two.
- Parity harness (`tests/parity_harness.cpp`) remains as a one-shot
  diagnostic tool (committed under `track-e.6`) — useful if anyone
  resurrects legacy for benchmarking, otherwise inert.

**Adding new state during the transition.** Pre-E.7, Track E
temporarily *increased* the surface for adding `RegimeSignals` state
(PortfolioController + EngineSharded + BacktestSharded driver mirror).
Post-E.7 (2026-04-26), legacy backtest body is deleted — the
`PortfolioController` push site is gone. Adding new state now
requires TWO updates: `EngineSharded_Run` static-local push +
`BacktestSharded_Run` static-local mirror via `ShardedBacktestDriver`.
Both still required because the live + backtest paths run in
different *control flows* (live is multi-thread + producer fan_out;
backtest is single-thread + driver loop) even though they share the
same `Regime_ComputeSignals` / `ModelFeatures_Pack` call sites.
Future cleanup: factor a shared "sharded init" helper that both
`EngineSharded_Run` and `BacktestSharded_Run` call, collapsing to one
push site.

**Adding a new depth-derived input.** After E.3, depth-state symmetry
is achieved at the public-API level: live reads `BookSnapshot<F>` via
`DepthSharedState::snapshots[active].FIELD` (atomic), backtest reads
the same `BookSnapshot<F>` via `DepthReplayState::current.FIELD`
(plain). Adding a new derived feature (e.g. spread-bps for D.3,
book-imbalance-over-time for D.1):
1. Add the field to `BookSnapshot<F>` (`DataStream/BinanceDepth.hpp`).
2. Compute it in `depth_parse_json` (live).
3. Compute it in `DepthReplayState_LoadDay`'s row-build block
   (`DataStream/DepthReplayState.hpp`) so the replay reads agree with
   live for the same input bid/ask/qty values.
4. Read it on the slow path the same way `book_imbalance` is read in
   `EngineSharded_Run` and `BacktestSharded_Run`. Pass via
   `EventLoop_RebuildAllParameters` (or `Regime_ComputeSignals` if it's
   a feature pack input).
5. Don't read it on the hot path — depth state is slow-path only.

### Maker/Taker Fee Accuracy (Phase 8)

When applying fees in any code path:

1. **Fee charge sites** (booking the fee on a real fill) MUST use the per-fill rate. Either:
   - At the cfg layer: `Fee_Compute(cfg, notional, is_maker)` in `ControllerConfig.hpp` reads `cfg->fee_rate_maker` or `cfg->fee_rate_taker` based on the flag.
   - At the OMS layer: `oms->fee_rate_maker` / `oms->fee_rate_taker` directly (engine sets these from cfg after `OrderManager_Init`).
   Never `FPN_Mul(notional, cfg.fee_rate)` for an actual fee charge — that's the legacy single-rate path.
2. **Source `is_maker` from the order that produced the fill**, not from a heuristic. For:
   - Live executionReport WS fills: parsed from Binance "m" field by `ud_parse_execution_report`.
   - Synchronous market BUY/SELL: hardcoded `is_maker=0` (market orders are taker by exchange definition).
   - Backtest: hardcoded `is_maker=0` (all-taker simulation, documented divergence).
3. **Pre-trade quantity sites** (no-trade band, fee floor for TP, kill-switch estimate, spread_bps display) intentionally use the LEGACY `fee_rate` field, NOT the maker/taker fields. They use fee_rate as a quantity in pre-trade computation, not as a fee charge on a real fill. Each such site has a `// Phase 8: pre-trade ... — leave as fee_rate` comment.
4. **Sanity invariant**: after every fill that goes through the synchronous path, `total_fees == total_maker_fees + total_taker_fees` on the controller. The OMS event-log path books fees independently and doesn't update these counters.
5. **Cfg backward compat**: if user sets only `fee_rate` (legacy), it mirrors to both maker and taker at load time. If user sets fee_rate AND only ONE of maker/taker, a `[CFG] WARNING` fires (mixed-cfg = almost certainly an error). The mirroring uses parse-time explicit-set flags, NOT value comparison — explicit values matching defaults still count as explicit.
6. **`ORDER_PARTIAL` is no longer a dead enum.** Adding code that does `if (state == ORDER_FILLED)` should consider whether `ORDER_PARTIAL` should also be handled (e.g., partial-fill bookkeeping). `Order_IsTerminal` correctly returns false for PARTIAL.

### Held-Out Validation Discipline (Phase 7prep)

When training/evaluating an ML model in foxml_suite:

1. **Held-out test set is locked by default.** `HeldOutSplit_Make(total, fraction)` returns a struct with `locked=1`. `HeldOutSplit_TestAccessAllowed` returns 0 until `HeldOutSplit_Unlock(s, token)` is called with the correct token. Use this when you want a final unbiased generalization estimate that hyperparameter selection didn't peek at.
2. **Walk-forward CV runs ONLY on `[0, trainval_end_idx)`.** `Backtest_RunFullValidation` enforces this by passing a sliced view of `BacktestResults` with `sample_count` capped at `trainval_end_idx`. Don't access test indices `[test_start_idx, total_samples)` from training/tuning code.
3. **Held-out evaluation runs ONCE per locked split.** After unlock, run final eval, record gap. Don't iterate on hyperparameters using held-out feedback — that defeats the whole purpose. If you need a second evaluation, `HeldOutSplit_Relock` generates a new token (old token can't unlock).
4. **Generalization gap is the WAS-IT-REAL test:** `|WF_mean_val - held_out|`. Default threshold (`cfg.gap_acceptable_threshold`, default 0.05) means gap above 5% = walk-forward was overfit despite per-fold OK numbers. **Models with gap > threshold should not ship.**
5. **`expected.cfg` saves the discipline values** (`held_out_fraction`, `gap_acceptable_threshold`) alongside the model bundle. Live engine logs these at model load time so future devs see what regime the model was trained under. Mismatch with current engine cfg is currently informational; tighten to enforced-mismatch later if drift becomes a real concern.
6. **Token is friction not security.** Determined peeker can edit memory or read source. The goal is "make accidental peeking impossible, intentional peeking auditable" — discipline mechanism for ML training, not a permission system. Resist any "ergonomic" change that weakens the lock (auto-unlock-after-timeout, default-unlocked-in-development-mode) — they defeat the purpose.

### Operational Alerting (Phase 8b)

When adding a new alertable event:

1. Add a new `NK_*` kind to the `NotifyKind` enum in `Notify.hpp`. **Append-only** — never reorder existing values; cooldown indexes are stable per-kind.
2. Call `Notify_Send(g_notify, level, kind, subject, body)` alongside the existing `fprintf` at the event site. Keep the `fprintf` (file logs are the forensic record).
3. Choose the level:
   - `NOTIFY_INFO` — status updates, session start
   - `NOTIFY_WARN` — recoverable issues (reconnect, transient errors)
   - `NOTIFY_ALERT` — user attention required (kill switch trip, orphan)
   - `NOTIFY_CRITICAL` — engine cannot continue safely
4. Use the SAME `NK_*` kind for the same logical event everywhere — cooldown is per-kind, so a disconnect storm collapses to one alert per cooldown window.
5. **NEVER call `Notify_Send` from the hot path.** Slow path / dedicated threads only. The Notify worker thread runs the backend; callers enqueue and return immediately.
6. Subject ≤ 128 chars, body ≤ 512 chars. Both are shell-escaped (internal `'` → `'\''`) when the Command backend is in use, but `"` and `\` are NOT JSON-escaped — keep alert text plain ASCII to be safe across Discord/Slack/dunst/email backends.
7. Guard call sites with `if (g_notify)` — backtest and tests leave it null, all calls become no-ops.

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

## Current State (2026-04-25 overnight — v4.0.3)

**Branch:** `experiment/per-core-sharding` (main). v4.0.3 released —
twelve features ported from legacy `PortfolioController` to sharded
engine, bringing sharded to ~95% functional parity for live trading.
v4.0.2 fixed TickRecorder timestamp bug + warmup gating + shutdown
diagnostics. v4.0.1 fixed train-serve parity + shutdown promptness +
several v4.0 default-mode regressions.

Sharded now has features legacy doesn't: per-core ML model + confidence,
hot-swap strategies live, AUTO regime mode per core, per-core config,
40-400ns branchless hot path (vs legacy ~5µs).

Hot-path additions in v4.0.3 cumulative: ~6ns (GATE_FLAG_BUY_ABOVE
mask select + FPN_Max(sl, ratchet_sl) for trailing). Both branchless,
FPN-pure, well under 500ns p99 budget.

Deferred: partial exits (architectural — needs slot rework), trailing
TP (similar mechanism, lower priority), full EmaCross port.

**Tests:** controller_test 351/351 passing, depth_recorder_test 17/17 passing.

**Build state:** all four targets clean — `build/engine`, `build_gui/engine_gui`,
`build_gui/foxml_suite`, `build/controller_test`. `build.sh` auto-symlinks
`engine.cfg` into each build dir so `./engine_*` connects to live Binance
out of the box. See `build.sh` helper.

**Phase status:** live-readiness work is shipped. Phase 5 zoo + 8a depth
recorder + 8b notify + 8 maker/taker + 6prep confidence loop + 7prep
validation + sharded integration (c5-c11) + per-core ML loop (c12-c16) +
hot-swap + live-data default — all merged into main, all tagged.
Current focus shifts from "ship live infra" to "validate with $10 paper
soak and find ML signal."

**Engine capability summary:**
- Sharded engine: production. Per-core ExecutionCore (40-400ns p99 measured
  at idle), per-core strategy + per-core ML model + per-core risk allocation.
- Legacy single-threaded: DEPRECATED with runtime warning. Kept for
  benchmark/regression. Phase 8+ features may be incomplete (see CLAUDE.md
  "Cross-Mode Init Placement" invariant).
- ML pipeline: end-to-end working in BOTH legacy and sharded paths.
  ConfidenceScorer per core in sharded; armed-but-inactive on noise-floor
  models via `effective_thr = base * (scale - conf)`.
- Live trading: architecturally complete (full OMS, paper/live sync, kill
  switch, orphan recovery, reconnect, maker/taker accounting, depth
  recording, alerting). Live-data is the default — `cd build_gui && ./engine_gui`
  connects to Binance out of the box.

**Recent invariants (added during Phase 5/v4.0):**
- Dynamic-buffer lifecycle (Init/Reset/Free/EnsureCapacity must update together) — see "Dynamic Sizing" section
- Label-type-aware metrics (every metric site consults `label_table[t].num_classes`) — see "Label-type-aware metric invariant"
- Cross-mode init placement (Phase 13+ doctrine — sharded code path must be
  reached before main.cpp returns OR have an equivalent inside `EngineSharded_Run`)
- Confidence loop invariant (Phase 6prep, ported to sharded in v4.0 c12-c16)
- Maker/taker fee accuracy (Phase 8 — entry + exit branches in `OrderManager_HandleFill`)
- Held-out validation discipline (Phase 7prep — locked test split)

**Engine subsystem state:**
- **Default mode: SHARDED** (production since v4.0.0). Per-core ExecutionCore +
  per-core PortfolioController state slot in central OMS, branchless 40-400ns
  hot path measured. Legacy `engine_mode=single_core` deprecated, runtime
  warning at startup.
- Per-core strategy + ML model + risk allocation. 4 cores default (cap 16).
  Each core can run a different strategy + load its own CoreModelZoo
  (auto-discovered roles: barrier, buy_signal, regime, exit).
- Per-core ConfidenceScorer (v4.0 c12) — independent IC/RMSE/freshness per
  ML core. `last_confidence` displayed in TUISnapshot per_core[i] for the
  GUI "Per-Core ML" panel.
- Hot-swap strategy per core (v4.0). GUI dropdown + Apply button writes to
  `TUISharedState::swap_strategy_requested[c]`; controller slow path applies
  when no open position. New "Per-Core Strategy" panel.
- Post-SL cooldown: adaptive (scales by trend confidence at SL time) or fixed cycle count
- Regime detection: score-based with 7 signals, extensible RegimeSignals struct
- Volume spike detection: current/max ratio, spacing relaxation on 5x+ spikes
- RollingStats: real least-squares regression (slope, R², variance) + VWAP
- VWAP gate: buy signal gates on price being below volume-weighted average price
- Session awareness: per-session (Asian/EU/US/overnight) volume gate multiplier
- Snapshot persistence: v7 (entry_time + session stats survive restarts)
- Binance trade websocket: ACTIVE (live market data, runs on every engine startup
  unless `sharded_force_synthetic=1`)
- Binance depth websocket (Phase 8a, sharded c6): ACTIVE when `depth_enabled=1` in
  both legacy and sharded paths. `book_imbalance` fed from `DepthSharedState`
  on every tick. Default `min_book_imbalance=0` keeps the gate inert unless opted in.
- Binance user-data websocket: defined but not started in main.cpp / EngineSharded
  (parallel pattern to pre-Phase-8a depth — wiring exists but no `pthread_create`).
- DepthRecorder (Phase 8a, sharded c6): writes top-of-book to
  `data/{SYMBOL}/depth/YYYY-MM-DD.csv` when `record_depth=1 && depth_enabled=1`.
  Daily rotation, auto-prune via `record_max_days`. Gap markers on backward
  `last_update_id`, wallclock >2s silence, or explicit disconnect.
- TickRecorder (sharded c7): writes ticks to `data/{SYMBOL}/YYYY-MM-DD.csv` when
  `record_ticks=1`. Daily rotation, auto-prune via `record_max_days`.
- NotifyState (Phase 8b, sharded c8): operational alerting via stderr or external
  command backend. Per-kind cooldown, level filter, append-only `NK_*` enum.
- Confidence loop: WIRED in BOTH legacy and sharded (v4.0 c12-c16) — per-core
  scorer in sharded, single scorer in legacy. `confidence_enabled` cfg gate,
  defaults off.
- TUI: ANSI only (zero deps, diff-based rendering, foxml palette). FTXUI/notcurses removed.
- TUI snapshot: zero-pollution (full copy on slow path, live price/volume/active_count every tick).
  Sharded mode populates `per_core[i]` with both latency stats AND ML observability.
- Momentum TP/SL: adaptive (R²-scaled + ROR acceleration bonus at fill time)
- Trailing TP/SL: SL floor invariant enforced (only when SL below entry — free trades exempt)
- Slippage simulation: configurable entry/exit price adjustment (slippage_pct in engine.cfg)
- Single-slot mode: max_positions=1 (default), sells entire BTC balance on exit (no dust)
- Paper/live sync: unbacked paper positions are undone, startup recovers orphaned BTC
- foxml_suite: SamplesSnapshot pattern eliminates GUI/worker race on results->labels

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

### Sharded gate direction (v4.0.1)

The sharded hot path's `BG_Evaluate` defaults to buy-below
(`tick.price < bg_price_threshold`). For a buy-above strategy
(momentum, breakout, etc.), the strategy's `_BuildParameters` MUST set
`out->flags |= GATE_FLAG_BUY_ABOVE`. Otherwise the core silently buys
dips while the GUI claims it's a momentum strategy — pre-v4.0.1 bug.
The flag is read in both the standalone `BG_Evaluate` (tests) and the
inlined version in `ExecutionCore_Tick` (production). Selection is
branchless via mask, ~1ns hot-path overhead.
