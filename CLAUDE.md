# CLAUDE.md

## Overview

Tick-level crypto trading engine in C++. Branchless fixed-point arithmetic, bitmap-based portfolio management, regression-driven gate adjustment with regime detection. Per-core sharded hot path (40-400ns p99), single-symbol producer thread fanning real Binance ticks across SPSC rings to per-core consumers.

**Sharded is production. Legacy single_core LIVE is deprecated (warned at boot). Legacy backtest is gone — `Backtest_Run` wraps `BacktestSharded_Run`.**

## Build

`./build.sh test` (engine + controller_test), `gui` (engine_gui + foxml_suite), `suite` (suite with XGBoost), `all`, `clean`. Build flags: `-DLATENCY_PROFILING=ON`, `-DLATENCY_LITE=ON`, `-DLATENCY_BENCH=ON`, `-DBUSY_POLL=ON`, `-DUSE_NATIVE_128=ON`.

Build dirs (different compile flags → different outputs): `build/` (ANSI + tests, zero deps), `build_gui/` (engine_gui + foxml_suite — SDL2 + OpenGL3 + ImGui), `build_suite/` (same + XGBoost), `build_lat/` (LATENCY_PROFILING).

XGBoost C library (for `-DUSE_XGBOOST=ON`): clone `dmlc/xgboost` recursive, cmake with `-DBUILD_STATIC_LIB=OFF`, install + ldconfig.

`build.sh` symlinks `engine.cfg` into each build dir; `bin/engine_gui` → `build_gui/engine_gui`.

## Architecture (sharded)

N cores (default 4, cap 16), each with ExecutionCore + parameters + tick ring + central OMS slot. Branchless ~60ns hot path per core. Producer pinned to one CPU, fans ticks across SPSC rings to per-core consumers.

- Per-core strategy (`core_N_strategy=simple_dip|momentum|ema_cross|ml`)
- Per-core ML model (`core_N_model_path=...` or `core_N_model_dir=...` for CoreModelZoo with auto-discovered roles)
- Per-core risk (`core_N_risk_pct=...`, default `risk_pct / N`)
- Per-core ConfidenceScorer (when STRATEGY_ML)
- Hot-swap strategy live (GUI dropdown + Apply)
- AUTO regime per core
- Hot path p99 ≤500ns (measured 460-567ns post-rdtsc-floor)
- No portfolio walk on hot path — each core owns its slot
- Partial exits (`partial_exit_enabled=1`): each core owns 2 slots (legs A+B); max cores = 8

```
HOT PATH (per tick, per core, branchless):
  BG_Evaluate → SG_Evaluate ×2 → TradeEvent push (rare branch)

SLOW PATH (every poll_interval ticks):
  RollingStats_Push → Regime_ComputeSignals → Strategy_BuildParameters
  → EventLoop_RebuildAllParameters (seqlock to per-core cached_params)
  → drain_with_submit (TradeEvents → OMS, log to CSV)

ASYNC THREADS:
  Binance trade WS, depth WS, Tick/DepthRecorder, Notify worker, GUI thread
```

## Data Flow: Regime Detection

```
RollingStats (128 + 512 tick windows) →
  RegimeSignals (short/long slope, R², variance, vol_ratio, ror_slope,
                 volume_slope, ema_sma_spread, ema_above_sma,
                 book_imbalance, spread_bps, spread_z,
                 flow_buy_ewma, flow_sell_ewma, large_trade_z) →
  Regime_Classify (trending_score + volatile_score with hysteresis) →
    RANGING / TRENDING / VOLATILE → strategy dispatch
```

`RegimeSignals` is the extensibility point — new signal = new field + populate in `Regime_ComputeSignals` + use in `Regime_Classify`.

## Directory Structure

- **CoreFrameworks/** — OrderGates, Portfolio, ExecutionCore, ControllerEventLoop, EngineSharded, ShardedSnapshot/Persist, GateParameters, TradeEvent
- **Strategies/** — RegimeDetector, MeanReversion, Momentum, SimpleDip, EmaCross, MLStrategy, StrategyParameters (dispatcher), StrategyInterface
- **DataStream/** — BinanceCrypto/Depth, DepthReplayState, DepthRecorder, TickRecorder, BinanceOrderAPI, EngineTUI
- **FixedPoint/** — FPN<F=64> (4096-bit)
- **MemHeaders/** — PoolAllocator (bitmap order pool), BuddyAllocator
- **ML_Headers/** — RollingStats, ROR_regressor, ConfidenceScore, ModelInference (XGBoost)
- **GUI/** — Dear ImGui native: FoxmlTheme, DashboardPanels, ChartPanel, CandleAccumulator, TradeReader, SettingsPanel, TradeHistoryPanel, LogViewerPanel, GuiThread
- **Backtest/** — `Backtest_Run` wrapper + `BacktestSharded_Run`, BacktestPanels, LabelFunctions
- **tests/** — controller_test.cpp (640+ assertions)
- **Version.hpp**, **Limits.hpp** — single source of truth

## Integration Contracts

### New RegimeDetector signal
1. Add field to `RegimeSignals<F>`
2. Populate in `Regime_ComputeSignals()`
3. Use in `Regime_Classify()` scoring

### New config field
1. `ControllerConfig.hpp`: struct + default + parser macro (`CFG_PARSE_FPN`, `CFG_PARSE_PCT` for 15.0→0.15, `CFG_PARSE_FPN_POS`, `CFG_PARSE_U32`, `CFG_PARSE_INT`)
2. `engine.cfg` and `backtest.cfg`: add with comment
3. Hot-reload is automatic. Protect a field by save/restore in HotReload.

### New GUI-editable config field
4. `GUI/SettingsPanel.hpp`: one line in `field_defs[]` (loading/UI/saving automatic)
5. Add tooltip in `SetItemTooltip` section (mandatory)
6. New strategy → update `default_strategy` tooltip

### New TUI/GUI display field
1. Add to `TUISnapshot` struct (`DataStream/EngineTUI.hpp`)
2. Populate in `TUI_CopySnapshot()`
3. Display in `GUI_Panel_*` (DashboardPanels.hpp)

Backtest auto-syncs via `BacktestSnapshot_Copy`.

### New chart overlay
1. `GUI/ChartPanel.hpp`: add to `ChartState` if needed, render in `GUI_PriceChart()`
2. Engine-sourced data → also update `TUISnapshot` + `TUI_CopySnapshot()`

### New strategy
1. `StrategyInterface.hpp`: `#define STRATEGY_YOUR_NAME N` (append-only)
2. `Strategies/YourName.hpp`: state struct + `_Init` + `_BuildParameters`
3. `StrategyParameters.hpp`: dispatch case
4. `RegimeDetector.hpp`: `Regime_ToStrategy` mapping
5. `tests/controller_test.cpp`: regression tests

**Sharded gate direction:** `BG_Evaluate` defaults to buy-below. Buy-above (momentum, breakout) MUST set `out->flags |= GATE_FLAG_BUY_ABOVE` in `_BuildParameters`. Otherwise core silently buys dips while GUI claims momentum.

**Partial exits:** strategies write only leg-A (`tp_pct`, `sl_pct`). Dispatcher post-cap sets `GATE_FLAG_PAIR_ACTIVE` + `tp_pct_b = tp_pct * cfg.tp2_mult` when enabled, clears both when disabled. Don't write `tp_pct_b` or `GATE_FLAG_PAIR_ACTIVE` from a strategy.

### Bumping version
1. `Version.hpp`: `ENGINE_VERSION_STRING`
2. `DOCS/CHANGELOG.md`: summary table
3. `DOCS/changelogs/YYYY-MM-DD-X.md`: dated changelog
4. `git tag vX.Y.Z && git push origin vX.Y.Z`

Patch (Z) = bug/cfg/TUI; Minor (Y) = features/strategies; Major (X) = architectural rewrites.

### Public release (FoxML_Trader)
Public repo `Jennyfirrr/FoxML_Trader` uses its own v1.0.x. **Uses legacy architecture — not 1:1 with sharded.** Copy files, push, tag `v1.0.N`, `gh release create` with `<3` in title. NEVER push `engine.cfg`, `controller.cfg`, `.env`, API keys, or `plans/`.

### New ML feature
1. `ModelInference.hpp`: `FEAT_NEW_NAME` at current `MODEL_NUM_FEATURES`
2. Increment `MODEL_NUM_FEATURES` + `MODEL_FORMAT_VERSION`
3. Pack line in `ModelFeatures_Pack()`
4. Add to `RegimeSignals<F>` + populate in `Regime_ComputeSignals` (single site)
5. New state? Add to BOTH `EngineSharded_Run` AND `BacktestSharded_Run` with parity in update cadence
6. Retrain all models (old fail version check)

`FEAT_*` constants are **append-only** — never reorder, never remove.

### foxml_suite parity
Same repo, same headers. Both targets must compile clean: `cmake --build build && cmake --build build_suite`.

### FoxML Suite Code Key

Data flow Backtest → GUI:
```
BacktestEngine.hpp (Backtest_Run → BacktestSharded_Run)
  → ShardedBacktestDriver (slow-path callbacks, feature collection)
  → BacktestResults (stats + ML features)
  → BacktestSnapshot_Copy() (state → TUISnapshot)
  → TradeLog CSV (logging/BACKTEST_order_history.csv)
```

GUI panels:
- Data, Run Control, Results, Training, Comparison: `BacktestPanels.hpp`
- Trade History: `TradeHistoryPanel.hpp` (reads CSV)
- Market/Account/Stats/Positions/Buy Gate: `DashboardPanels.hpp` (reads `TUISnapshot`)
- Chart: `ChartPanel.hpp` (CandleAccumulator)
- Settings: `SettingsPanel.hpp` (`backtest.cfg`)

Live engine = `engine.cfg`. Backtest suite = `backtest.cfg`. `default_strategy`: -2 = full 4-strat auto, -1 = legacy 2-strat, 0-4 = fixed.

**Trade log:** `TradeLog_Init(&log, "SYMBOL")` → `logging/SYMBOL_order_history.csv`. Format v3: `timestamp_us,core_id,strategy_id,event_type(E|X),event_price,entry_price,exit_price,pnl,fees,balance_after,trade_size`. With partials, `core_id` in CSV is portfolio SLOT (slot c → core c/2, leg c%2).

### Centralized constants
- `Version.hpp`: `ENGINE_VERSION_STRING`
- `Limits.hpp`: `MAX_PORTFOLIO_POSITIONS`, `CANDLE_HISTORY_MAX`

## Code Conventions

- `using namespace std;` throughout
- C-style with templates, no classes
- `Pattern_FunctionName` (e.g. `Portfolio_Init`, `BG_Evaluate`)
- Hot-path math is `FPN<F>` only, no floats (F=64 = 4096-bit)
- Branchless: mask tricks `-(uint64_t)pass`, word-level mask-select
- Inline comments explain reasoning
- **Preserve user's voice in existing comments when editing**

### Dynamic Sizing (Backtest Suite ONLY)
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

## Plan Review Checklist

Before any multi-day plan, walk through. **Audit BEFORE coding.**

1. **Hot path purity** — touches `ExecutionCore_Tick`/`BG_Evaluate`/`SG_Evaluate`? Branchless, FPN-only, alloc-free? p99 ≤500ns. New code defaults to slow path.
2. **Train-serve parity** — touches `RegimeSignals`/`ModelFeatures_Pack`? BOTH `BacktestSharded_Run` AND `EngineSharded_Run` populate with equivalent cadence + inputs.
3. **Surface area / coupling** — minimize files touched. `if (live_trading)` branches = wrong abstraction. New optional state owns lifecycle in ONE place. **Preserve public surface during refactors** — keep field names + signatures stable, change contents not names. Append-only enums, fields-at-end struct layouts.
4. **Pointer init + heap lifecycle** — every `*_Init` caller NULL-inits; `_Init` does `if (ptr) free(ptr)` before realloc; cleanup frees + NULLs; snapshots persist or document session-only.
5. **Backward compat** — `SHARDED_SNAPSHOT_VERSION` bump = old refused. `MODEL_FORMAT_VERSION` bump = old models fail. Saved Runs forward-compat (additions OK). Cfg additions OK, removals break user cfgs.
6. **Multi-threading correctness** — atomic vs not, SPSC ring producer/consumer, slow-path/hot-path on `GateParameters` uses seqlock. Backtest output reproducible.
7. **Test coverage** — round-trip hammer test, edge cases (cold start, full window, wraparound, zero, uninit), runs in `controller_test`.
8. **Docs + invariants** — load-bearing rule? Add Safety Invariant. Update Current State. Dated changelog.
9. **Forward maintenance** — 30+ sites to extend? redesign. Next similar feature copies code? factor helper. Document brittle assumptions.
10. **Rollback story** — tag `pre-{name}`, push to remote. Multi-week → branch. Each phase individually revertable.

Verdicts: **PASS** ✅ / **FIXED** ✅ (patched same pass) / **GAP** ⚠️ (must address) / **DRIFT** ⚠️ (pre-existing) / **DEFERRED** / **ACCEPTED**.

## Safety Invariants

### Position Exit Invariants
Every code path that sets/modifies `take_profit_price` or `stop_loss_price` MUST:
1. Preserve TP > entry > SL
2. SL distance ≥ 0.5 × TP distance (2:1 minimum reward/risk)
3. TP ≥ entry + (entry × fee_rate × 3)

```cpp
FPN<F> tp_dist = FPN_Sub(pos->take_profit_price, pos->entry_price);
FPN<F> min_sl_dist = FPN_Mul(tp_dist, FPN_FromDouble<F>(0.5));
FPN<F> sl_floor = FPN_SubSat(pos->entry_price, min_sl_dist);
pos->stop_loss_price = FPN_Min(pos->stop_loss_price, sl_floor);
```

### FPN Division Guards
Every `FPN_DivNoAssert(num, den)` MUST guard `if (FPN_IsZero(den)) return;`. `FPN_DivNoAssert` saturates to MAX on zero — silent extreme values.

### Fill-Counter Atomicity (load-bearing — v4.7.19)

**Rule:** heartbeat counters (`state.total_entries`, `state.total_exits`, `state.cores[].entries_processed`, `state.cores[].exits_processed`) MUST be bumped **only inside `EventLoop_DrainPostFill`**, walking `oms->last_opened_mask` / `oms->last_closed_mask`. These masks are populated by `OrderManager_HandleFill` exactly when a real fill writes a CSV row + mutates portfolio/balance. Bumping anywhere else (OnEvent, manual-close lambdas, time-exit, future bypass paths) decouples the counter from the actual fill — over-counts when `Submit` fails, when the result_queue is full, or when `HandleFill` rejects via the active-bitmap guard.

**`OrderManager_HandleFill` SELL branch MUST guard against double-close:**

```cpp
if ((oms->portfolio.active_bitmap & (uint16_t)(1u << pslot)) == 0) {
    fprintf(stderr, "[OMS] HandleFill: SELL on closed slot — no-op\n");
    return;
}
```

Without this guard, a duplicate SELL fill (e.g., manual-close racing with hot-path SG) reads stale `entry_price`/`quantity` from `Portfolio_CloseSlot`'s leftover position record, computes phantom gross/fees, drains balance, and writes a ghost CSV row.

**Why this is load-bearing.** v4.7.19 — Stats panel showed `exits: 2 (7 fills)` while trade log CSV had 5 rows. Counter bumps in `OnEvent` (mode 1) + manual-close + time-exit fired BEFORE Submit/HandleFill could fail; phantom fees drained from balance through ghost CSV rows. In live mode the same race would push duplicate `OrderManager_Submit` calls to Binance — second SELL could fill as an unintended SHORT.

Adding a new fill-producing path: just submit through OMS. `HandleFill` populates the masks, `DrainPostFill` bumps the counters and applies CoreContext updates. Single source of truth.

### Config Field Conventions
- `_pct` suffix: stored as decimal (0.04 = 4%), parsed `/100.0`. Stddev mult use: `mult = field × 100`
- `_mult` suffix: direct value (3.0 = 3.0σ), parsed raw. Used directly: `offset = stddev × field`

Momentum positions use `momentum_tp_mult` / `momentum_sl_mult`. MR positions use `take_profit_pct × 100` / `stop_loss_pct × 100`. Never cross.

### Cross-Mode Init Placement (load-bearing)

`main.cpp` dispatches to `tt::EngineSharded_Run` (~line 154) and **returns** from `main()`. Code AFTER the dispatch only runs in legacy mode. Code that should run in BOTH modes MUST be:
(a) initialized BEFORE the dispatch in `main.cpp`, OR
(b) called from inside `EngineSharded_Run`

Verification: `grep -n "engine_mode == ENGINE_MODE_SHARDED" main.cpp` — your init must be ≤ that line OR also in `EngineSharded_Run`.

Affects (must work in sharded): Depth WS + DepthRecorder, TickRecorder, NotifyState + g_notify, book_imbalance feed, any new background thread / shared global / recorder.

Cross-architecture features port from legacy `PortfolioController` to sharded `OrderManager_HandleFill` / `EventLoop_OnEvent`. Same logic, different host struct.

### FPN-Only Accounting
Balance, P&L, fees, equity, position pricing → `FPN<F>` only. `double` only at boundaries:
- OK: `FPN_ToDouble` for display/logging/CSV/printf
- OK: `FPN_FromDouble` at exchange API boundary
- NOT OK: decision-logic intermediate doubles

Known accepted violations: `peak_equity`, `session_start_equity`, `max_drawdown` (kill switch), `ConfidenceScorer` IC/RMSE/freshness (out of scope).

### FPN Comparison Completeness
Use `FPN_LessThan`, `FPN_GreaterThanOrEqual` etc. — never partial word comparisons. Inline opt in `Portfolio.hpp:226-229` only compares MSW+LSW (known bug, can miss exits near price boundaries).

### Halt Flag Invariant
Suppressing buying MUST set `ctrl->buying_halted = 1` AND zero `ctrl->gate_offset`. Ad-hoc zeroing of `buy_conds` alone fails — hot-path tracking restores from `gate_offset`.

### Confidence Loop Invariant

When `confidence_enabled=1` AND `strategy_id == STRATEGY_ML`:

1. Every fill pushes `(prediction, realized_return)` into `RollingIC` + `RollingRMSE` via `ConfidenceScorer_Update`. ONE update site — IC contamination = wrong confidence.
2. Confidence computed inside slow-path gate block, before buy-gate decision. Hot path may NOT call `ConfidenceScorer_Compute` (Spearman O(W²)).
3. Effective threshold: `effective_thr = base * (scale - conf)`, clamped ≤ 1.0. `scale = cfg.confidence_threshold_scale` (default 2.0). `base = cfg.ml_buy_threshold`. Modify formula → update `controller_test.cpp` "Phase 6prep: Gate effective threshold" same commit.
4. Safe-by-default on noise floor: `abs_ic` clamps to `CONFIDENCE_MIN_IC_DEFAULT = 0.01`, conf ≈ 0.01, effective ≈ `2.0 * base` — gate effectively never fires.
5. Confidence read on slow path, displayed via `last_confidence`. NEVER read `last_confidence` on hot path.
6. Tunables: `cfg.confidence_window` (default 32, max 64), `cfg.confidence_freshness_tau` (default 300s), `cfg.confidence_threshold_scale` (default 2.0).

### Train-Serve Feature Parity (load-bearing)

ML trains on backtest features (`BacktestSharded_Run`), serves on live features (`EngineSharded_Run`). BOTH paths MUST call `Regime_ComputeSignals` with equivalent state.

`ModelFeatures_Pack` reads `RegimeSignals + RollingStats`. Several need state beyond rolling: `ror_slope` (RORRegressor), `ema_sma_spread`/`ema_above_sma` (ema_price), `book_imbalance`/`spread_bps`/`spread_z` (depth), `flow_*_ewma`/`large_trade_z` (flow EWMAs).

Sharded engine maintains all in `EngineSharded_Run` static-locals; `BacktestSharded_Run` mirrors via `ShardedBacktestDriver` callbacks. Both threaded through `EventLoop_RebuildAllParameters`.

Adding feature to `ModelFeatures_Pack`:
1. `FEAT_NEW_NAME` + bump `MODEL_NUM_FEATURES` + `MODEL_FORMAT_VERSION`
2. Add to `RegimeSignals<F>`
3. Populate in `Regime_ComputeSignals` (single site)
4. New state? BOTH `EngineSharded_Run` AND `BacktestSharded_Run`, parity in update cadence
5. Retrain all models

Accepted divergences: maker/taker fees (backtest all-taker), full vs partial fills, tick timing.

Adding new depth-derived input:
1. Field on `BookSnapshot<F>` (`BinanceDepth.hpp`)
2. Compute in `depth_parse_json` (live)
3. Compute in `DepthReplayState_LoadDay` row-build (backtest)
4. Read on slow path via `EventLoop_RebuildAllParameters` or `Regime_ComputeSignals`
5. NOT on hot path

### Maker/Taker Fee Accuracy

1. Fee charge sites: `Fee_Compute(cfg, notional, is_maker)` or `oms->fee_rate_maker`/`oms->fee_rate_taker`. Never `FPN_Mul(notional, cfg.fee_rate)` (legacy single-rate).
2. `is_maker` source: live execReport WS parses Binance "m"; sync market BUY/SELL hardcodes `is_maker=0`; backtest hardcodes `is_maker=0`.
3. Pre-trade quantity sites (no-trade band, fee-floor TP, kill-switch estimate, spread_bps display) use legacy `fee_rate` (not fee charge on real fill). Each has comment.
4. Sanity: `total_fees == total_maker_fees + total_taker_fees` after every fill.
5. Cfg compat: only `fee_rate` set → mirror to maker+taker. `fee_rate` + only one of maker/taker → `[CFG] WARNING`.
6. `ORDER_PARTIAL` is NOT dead. Code with `if (state == ORDER_FILLED)` should consider PARTIAL. `Order_IsTerminal` returns false for PARTIAL.

### Held-Out Validation Discipline

1. Test set locked by default. `HeldOutSplit_Make` returns `locked=1`. `HeldOutSplit_TestAccessAllowed` returns 0 until `HeldOutSplit_Unlock(s, token)` with correct token.
2. Walk-forward CV runs ONLY on `[0, trainval_end_idx)`. `Backtest_RunFullValidation` enforces via sliced view.
3. Held-out evaluation runs ONCE per locked split. Don't iterate hyperparameters on held-out feedback. Need second eval → `HeldOutSplit_Relock` → new token.
4. Generalization gap: `|WF_mean_val - held_out|`. Default `cfg.gap_acceptable_threshold = 0.05`. Gap > threshold → walk-forward overfit. **Don't ship.**
5. `expected.cfg` saves `held_out_fraction`, `gap_acceptable_threshold` with bundle.
6. Token = friction not security. "Make accidental peeking impossible, intentional peeking auditable."

### Operational Alerting

1. New `NK_*` kind in `Notify.hpp` — append-only (cooldown index stable per-kind).
2. `Notify_Send(g_notify, level, kind, subject, body)` alongside `fprintf` (keep fprintf — forensic log).
3. Levels: `NOTIFY_INFO`, `NOTIFY_WARN`, `NOTIFY_ALERT` (user attention), `NOTIFY_CRITICAL` (engine cannot continue).
4. Same `NK_*` for same logical event everywhere — cooldown is per-kind.
5. NEVER call `Notify_Send` from hot path. Slow path / dedicated threads only.
6. Subject ≤128, body ≤512. Shell-escaped for Command backend; plain ASCII (no JSON escaping of `"`/`\`).
7. Guard `if (g_notify)` — backtest/tests leave null → no-op.

### Regime Adjustment Checklist
New transition case in `Regime_AdjustPositions`:
1. Guard stddev != 0 at function entry
2. Correct config family (momentum_*_mult for momentum, *_pct×100 for MR)
3. After all TP/SL mutations, re-check SL floor + TP floor
4. FPN_Max vs FPN_Min direction:
   - **Tighten TP** (closer to entry) = `FPN_Min`
   - **Widen TP** (further) = `FPN_Max`
   - **Tighten SL** (closer, SL<entry, pick higher) = `FPN_Max`
   - **Widen SL** (further) = `FPN_Min`
5. Regression test for new transition

### Label-type-aware metric invariant (Backtest Suite)

Every metric/display/training/validation site touching label values MUST consult `label_table[t].num_classes` (via `LabelType_IsBinary`/`LabelType_IsRegression`/`LabelType_IsMulticlass`) and branch.

| `num_classes` | Kind | Values | XGBoost objective | Metric |
|---|---|---|---|---|
| 0 | binary | {0.0,1.0}, 0.5=neutral | `binary:logistic` + `scale_pos_weight` | accuracy |
| 1 | regression | continuous | `reg:squarederror` | Pearson r |
| ≥2 | multiclass | int 0..K-1 | `multi:softprob` + `num_class=K` | argmax accuracy |

Branch sites: Sample panel, Train Model in-sample metric, Walk-Forward (`Backtest_RunWalkForward` — neutral filter, objective, `num_class`, `scale_pos_weight`, per-fold + aggregate), WF result display, Overfit detection, Save Run / `expected.cfg`.

Future hardening (deferred): `enum class LabelKind` for compiler exhaustive-check.

### Snapshot Re-Activation Invariant

`ShardedSnapshot_Load` restores `portfolio.active_bitmap` + `positions[slot]` — MUST also re-activate `ExecutionCore<F>` hot-path mirrors (`active`, `entry_price`, `live_tp`, `live_sl`). Otherwise restored positions are zombie — open in portfolio but `can_exit = active & sg_fires = 0`.

Fix in `CoreFrameworks/ShardedSnapshotPersist.hpp` after portfolio + core-context restore — walk the bitmap, copy `pos.entry_price`/`tp`/`sl` into `core_ptr`, set `active=1`.

New ExecutionCore hot-path state (e.g. partials' `live_tp_b`/`active_b`): snapshot loader handles too. Either persist+restore OR reset to safe defaults (current v8 doesn't include leg-B; snapshot-while-paired needs v9 bump).

### Snapshot Tick-Counter Drift

Slow-path code subtracting snapshot-persisted tick counter from current `ticks_produced` MUST guard `entry_t > now_tick` (uint64 underflow). Persisted survives restart; live counter resets to 0. Without guard: `now_tick - entry_t` underflows ~2^64 → time-exit thresholds trivially pass → spurious force-close every cycle.

Pattern (`EngineSharded.hpp` time-exit, ~line 1110):
```cpp
uint64_t entry_t = state.cores[slot].last_entry_tick;
if (entry_t == 0) continue;
if (entry_t > now_tick) {
    state.cores[slot].last_entry_tick = now_tick;
    continue;
}
uint64_t elapsed = now_tick - entry_t;
```

### Partial Exits — Two-Position-per-Core

`cfg.partial_exit_enabled=1` → each core owns 2 slots:
- core `c` → leg A in slot `2c`, leg B in slot `2c+1`
- max cores = `MAX_PORTFOLIO_POSITIONS / 2 = 8` (validated via `Sharded_ValidatePartialExitCfg`)

`partial_exit_enabled=0` (default): core `c` → slot `c` (1:1), `2c+1` unused.

`Sharded_LegSlot(core_id, leg, partial_enabled)` returns correct slot. `PARTIAL_LEG_A`/`PARTIAL_LEG_B` in `TradeEvent.hpp` (so `ExecutionCore_Tick` doesn't need EventLoop header).

**Hot path** — `ExecutionCore_Tick` branch-gates leg B SG via `if (__builtin_expect(active_b, 0))`. Steady state when partials disabled OR no leg-B open → leg-B FPN comparisons skip. Cost when active: ~1-2ns per tick.

**Slow path** — `Strategy_BuildParameters` post-cap sets `GATE_FLAG_PAIR_ACTIVE` + `tp_pct_b = tp_pct * cfg.tp2_mult` when enabled, clears both when disabled. Strategies stay leg-A-only.

**OMS drainer** — `EngineSharded.hpp:drain_with_submit` maps event → slot via `Sharded_LegSlot(core_id, leg, partial_exit_enabled)`. Entry: split `intended_qty` by `cfg.partial_exit_pct` (A=`partial_pct`, B=`1-partial_pct`). Exit: read qty from leg's `portfolio.positions[slot].quantity`. `core_id` param to `OrderManager_Submit` is actual portfolio slot; `event.leg` propagates to `Order::leg`.

Per-core counters (`last_entry_tick`, `last_entry_price`, `active_prediction`): updated only on **leg A** entry events (one trade = one stamp).

`tp2_mult` defensive default: when 0 or `tp_pct=0`, `tp_pct_b = tp_pct` (effective no-op). Default cfg = 2.0.

Deferred:
- `breakeven_on_partial=1` semantics (slow path ratchet leg B SL to entry after leg A TP1)
- Snapshot persistence of `live_tp_b`/`active_b`/`entry_price_b` (current v8 doesn't include)

Toggle: `cfg.partial_exit_enabled = 0` (default) preserves pre-partials behavior. Validation refuses boot if too many cores. Rollback: `pre-partial-exits` at `abd08d3`.

## Key Design Decisions

1. Portfolio uses `uint16_t` bitmap (not sequential count) — same as OrderPool
2. Per-position TP/SL exits on hot path, portfolio mgmt on slow path
3. Fill consumption every tick (zero unprotected exposure)
4. Single-slot mode (max_positions=1): exchange BTC balance IS the position, sell-all eliminates dust
5. Multi-slot fallback: per-position sells when max_positions > 1 (dust may accumulate)
6. Warmup observes market before trading (gates on slow-path sample count, not raw ticks)
7. 24-hour session lifecycle: warmup → trade → wind down → close all → reconnect
8. TUI independent of engine (engine runs headless, TUI reads state)
9. No API key for market data WS (public endpoint)
10. RollingStats computes regression inline (no separate feeder)
11. `RegimeSignals` is the extensibility point
12. Partial exits: dispatcher post-cap so strategies stay leg-A-only; hot path branch-gates leg B
