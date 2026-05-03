# Safety Invariants

**Read this file before changing OMS, kill switch, snapshot, hot path, slow-path threading, or anything labeled load-bearing.** Each invariant has a "why" — knowing it prevents reintroducing the bug it caught.

## Position Exit Invariants
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

## FPN Division Guards
Every `FPN_DivNoAssert(num, den)` MUST guard `if (FPN_IsZero(den)) return;`. `FPN_DivNoAssert` saturates to MAX on zero — silent extreme values.

## Fill-Counter Atomicity (load-bearing — v4.7.19)

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

## Config Field Conventions
- `_pct` suffix: stored as decimal (0.04 = 4%), parsed `/100.0`. Stddev mult use: `mult = field × 100`
- `_mult` suffix: direct value (3.0 = 3.0σ), parsed raw. Used directly: `offset = stddev × field`

Momentum positions use `momentum_tp_mult` / `momentum_sl_mult`. MR positions use `take_profit_pct × 100` / `stop_loss_pct × 100`. Never cross.

## Cross-Mode Init Placement (load-bearing)

`main.cpp` dispatches to `tt::EngineSharded_Run` (~line 154) and **returns** from `main()`. Code AFTER the dispatch only runs in legacy mode. Code that should run in BOTH modes MUST be:
(a) initialized BEFORE the dispatch in `main.cpp`, OR
(b) called from inside `EngineSharded_Run`

Verification: `grep -n "engine_mode == ENGINE_MODE_SHARDED" main.cpp` — your init must be ≤ that line OR also in `EngineSharded_Run`.

Affects (must work in sharded): Depth WS + DepthRecorder, TickRecorder, NotifyState + g_notify, book_imbalance feed, any new background thread / shared global / recorder.

Cross-architecture features port from legacy `PortfolioController` to sharded `OrderManager_HandleFill` / `EventLoop_OnEvent`. Same logic, different host struct.

## FPN-Only Accounting
Balance, P&L, fees, equity, position pricing → `FPN<F>` only. `double` only at boundaries:
- OK: `FPN_ToDouble` for display/logging/CSV/printf
- OK: `FPN_FromDouble` at exchange API boundary
- NOT OK: decision-logic intermediate doubles

Known accepted violations: `peak_equity`, `session_start_equity`, `max_drawdown` (kill switch), `ConfidenceScorer` IC/RMSE/freshness (out of scope).

## FPN Comparison Completeness
Use `FPN_LessThan`, `FPN_GreaterThanOrEqual` etc. — never partial word comparisons. Inline opt in `Portfolio.hpp:226-229` only compares MSW+LSW (known bug, can miss exits near price boundaries).

## Halt Flag Invariant
Suppressing buying MUST set `ctrl->buying_halted = 1` AND zero `ctrl->gate_offset`. Ad-hoc zeroing of `buy_conds` alone fails — hot-path tracking restores from `gate_offset`.

## Confidence Loop Invariant

When `confidence_enabled=1` AND `strategy_id == STRATEGY_ML`:

1. Every fill pushes `(prediction, realized_return)` into `RollingIC` + `RollingRMSE` via `ConfidenceScorer_Update`. ONE update site — IC contamination = wrong confidence.
2. Confidence computed inside slow-path gate block, before buy-gate decision. Hot path may NOT call `ConfidenceScorer_Compute` (Spearman O(W²)).
3. Effective threshold: `effective_thr = base * (scale - conf)`, clamped ≤ 1.0. `scale = cfg.confidence_threshold_scale` (default 2.0). `base = cfg.ml_buy_threshold`. Modify formula → update `controller_test.cpp` "Phase 6prep: Gate effective threshold" same commit.
4. Safe-by-default on noise floor: `abs_ic` clamps to `CONFIDENCE_MIN_IC_DEFAULT = 0.01`, conf ≈ 0.01, effective ≈ `2.0 * base` — gate effectively never fires.
5. Confidence read on slow path, displayed via `last_confidence`. NEVER read `last_confidence` on hot path.
6. Tunables: `cfg.confidence_window` (default 32, max 64), `cfg.confidence_freshness_tau` (default 300s), `cfg.confidence_threshold_scale` (default 2.0).

## Train-Serve Feature Parity (load-bearing)

ML trains on backtest features (`BacktestSharded_Run`), serves on live features (`EngineSharded_Run`). BOTH paths MUST call `Regime_ComputeSignals` with equivalent state.

`ModelFeatures_Pack` reads `RegimeSignals + RollingStats`. Several need state beyond rolling: `ror_slope` (RORRegressor), `ema_sma_spread`/`ema_above_sma` (ema_price), `book_imbalance`/`spread_bps`/`spread_z` (depth), `flow_*_ewma`/`large_trade_z` (flow EWMAs).

Sharded engine maintains all in `EngineSharded_Run` static-locals (pre-v5.1.2) or `state.cores[c].slow_state` (v5.1.2+); `BacktestSharded_Run` mirrors via `ShardedBacktestDriver` callbacks. Both threaded through `EventLoop_RebuildAllParameters_PerCore` (v5.1.2+).

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
4. Read on slow path via `EventLoop_RebuildAllParameters_PerCore` or `Regime_ComputeSignals`
5. NOT on hot path

## Maker/Taker Fee Accuracy

1. Fee charge sites: `Fee_Compute(cfg, notional, is_maker)` or `oms->fee_rate_maker`/`oms->fee_rate_taker`. Never `FPN_Mul(notional, cfg.fee_rate)` (legacy single-rate).
2. `is_maker` source: live execReport WS parses Binance "m"; sync market BUY/SELL hardcodes `is_maker=0`; backtest hardcodes `is_maker=0`.
3. Pre-trade quantity sites (no-trade band, fee-floor TP, kill-switch estimate, spread_bps display) use legacy `fee_rate` (not fee charge on real fill). Each has comment.
4. Sanity: `total_fees == total_maker_fees + total_taker_fees` after every fill.
5. Cfg compat: only `fee_rate` set → mirror to maker+taker. `fee_rate` + only one of maker/taker → `[CFG] WARNING`.
6. `ORDER_PARTIAL` is NOT dead. Code with `if (state == ORDER_FILLED)` should consider PARTIAL. `Order_IsTerminal` returns false for PARTIAL.

## Held-Out Validation Discipline

1. Test set locked by default. `HeldOutSplit_Make` returns `locked=1`. `HeldOutSplit_TestAccessAllowed` returns 0 until `HeldOutSplit_Unlock(s, token)` with correct token.
2. Walk-forward CV runs ONLY on `[0, trainval_end_idx)`. `Backtest_RunFullValidation` enforces via sliced view.
3. Held-out evaluation runs ONCE per locked split. Don't iterate hyperparameters on held-out feedback. Need second eval → `HeldOutSplit_Relock` → new token.
4. Generalization gap: `|WF_mean_val - held_out|`. Default `cfg.gap_acceptable_threshold = 0.05`. Gap > threshold → walk-forward overfit. **Don't ship.**
5. `expected.cfg` saves `held_out_fraction`, `gap_acceptable_threshold` with bundle.
6. Token = friction not security. "Make accidental peeking impossible, intentional peeking auditable."

## Operational Alerting

1. New `NK_*` kind in `Notify.hpp` — append-only (cooldown index stable per-kind).
2. `Notify_Send(g_notify, level, kind, subject, body)` alongside `fprintf` (keep fprintf — forensic log).
3. Levels: `NOTIFY_INFO`, `NOTIFY_WARN`, `NOTIFY_ALERT` (user attention), `NOTIFY_CRITICAL` (engine cannot continue).
4. Same `NK_*` for same logical event everywhere — cooldown is per-kind.
5. NEVER call `Notify_Send` from hot path. Slow path / dedicated threads only.
6. Subject ≤128, body ≤512. Shell-escaped for Command backend; plain ASCII (no JSON escaping of `"`/`\`).
7. Guard `if (g_notify)` — backtest/tests leave null → no-op.

## Regime Adjustment Checklist
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

## Label-type-aware metric invariant (Backtest Suite)

Every metric/display/training/validation site touching label values MUST consult `label_table[t].num_classes` (via `LabelType_IsBinary`/`LabelType_IsRegression`/`LabelType_IsMulticlass`) and branch.

| `num_classes` | Kind | Values | XGBoost objective | Metric |
|---|---|---|---|---|
| 0 | binary | {0.0,1.0}, 0.5=neutral | `binary:logistic` + `scale_pos_weight` | accuracy |
| 1 | regression | continuous | `reg:squarederror` | Pearson r |
| ≥2 | multiclass | int 0..K-1 | `multi:softprob` + `num_class=K` | argmax accuracy |

Branch sites: Sample panel, Train Model in-sample metric, Walk-Forward (`Backtest_RunWalkForward` — neutral filter, objective, `num_class`, `scale_pos_weight`, per-fold + aggregate), WF result display, Overfit detection, Save Run / `expected.cfg`.

Future hardening (deferred): `enum class LabelKind` for compiler exhaustive-check.

## Snapshot Re-Activation Invariant

`ShardedSnapshot_Load` restores `portfolio.active_bitmap` + `positions[slot]` — MUST also re-activate `ExecutionCore<F>` hot-path mirrors (`active`, `entry_price`, `live_tp`, `live_sl`). Otherwise restored positions are zombie — open in portfolio but `can_exit = active & sg_fires = 0`.

Fix in `CoreFrameworks/ShardedSnapshotPersist.hpp` after portfolio + core-context restore — walk the bitmap, copy `pos.entry_price`/`tp`/`sl` into `core_ptr`, set `active=1`.

New ExecutionCore hot-path state (e.g. partials' `live_tp_b`/`active_b`): snapshot loader handles too. Either persist+restore OR reset to safe defaults (current v8 doesn't include leg-B; snapshot-while-paired needs v9 bump).

## Snapshot Tick-Counter Drift

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

## Per-Core Data-Plane Single-Writer (v5.1.0+)

Each engine OWNS its rolling/regime/flow state via heap-allocated
`CoreContext::slow_state` (`CoreSlowState<F>` in `ControllerEventLoop.hpp`).
Single-writer per `state.cores[c].slow_state`:

- **Per-tick `ema_price`**: producer writes ALL N engines' copies in
  `fan_out` via `EventLoop_UpdateEmaPriceAllCores`.
- **Per-cadence fields** (rolling stats, regime_ror, cumdelta, tick_rate,
  flow, large_trade, book_imb_history, spread_state):
  - `centralized`: producer iterates c=0..N via `EventLoop_UpdateRollingStateAllCores`.
  - `per_core_slow`: per-core slow-path c writes its OWN slow_state via
    `EventLoop_UpdateRollingStateOneCore(state, c, ...)`.
  - backtest (single-thread): same helper, linear iteration.

Cross-thread reads bounded to per-tick `ema_price` in per_core_slow
(producer→engine c) — relaxed loads, x86-acceptable. Adding new
rolling/regime input: field on `CoreSlowState<F>` + init line + push in
`UpdateRollingStateOneCore` + read in `Regime_ComputeSignals`. All 3
callers pick up automatically.

Direct reads from producer-thread shared state (legacy v5.0.x and
earlier) are gone — `static RollingStats rolling_short` etc. removed in
v5.1.2. Reading via `state.cores[c].slow_state` is the only correct path.

## Lifecycle Bitmap Single-Writer (v5.0.3)

`TUISharedState::paused_engines_mask` (uint16_t) controls per-engine
slow-path pausing. GUI is sole writer per bit; per-core slow-path c is
single-reader of bit c only. No atomic ops needed beyond the volatile
load. Slow-path checks at TOP of loop (before reset-paper park + cadence
yield), sets `sp_state=3`, increments `sp_yield_count`, yields. Hot-path
unaffected.

## Per-Section Latency Stats Single-Writer (v5.1.1)

Each `CoreContext::slow_path_breakdown[section]` is single-writer by the
thread running that section. Sections (append-only, indices stable for
GUI): ROLLING, REBUILD, PUSH, TIME_EXIT, TRAIL_SL. Bracket cost
~10ns × 5 sections = ~50ns/cycle, < 1% of microsecond-scale slow-path.

## Partial Exits — Two-Position-per-Core

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

## W/L Pair Classification under Partials (v4.7.21)

A "trade" under partials = leg A + leg B together (one logical position). W/L counted per trade-pair, never per leg. The first leg to close stashes its net into `partner_pending_pnl`; the second leg's close sums both nets and classifies the **pair** as W (sum > 0) or L (sum ≤ 0). Tie counts as L.

Implementation in `EventLoop_DrainPostFillOneCore` (lines ~888-905). `core_gross_wins` / `core_gross_losses` accumulate the **pair net** into the matching bucket — single classification site.

## Train-Serve Handoff Verification (v5.9.2a+)

Any artifact traveling from training to serving — model file, stamp body, scaler sidecar (v5.9.3), and any future analogous artifact (ensemble weights, feature config, calibration curves) — MUST satisfy ALL FIVE:

1. **SHA-pinned in stamp body.** Artifact's bytewise hash recorded in the parent stamp. Engine load-time verifier compares.
2. **Load-time verification.** `verify_<artifact>` function exists + is called from `CoreModelZoo_TryLoadRole` (or analogous load path).
3. **Refuse-or-warn 3-tier behavior.** `held_out_gate_strict=1` REFUSES on mismatch; `=0` WARNs + applies identity/default with distinct PerCoreSnap surface. Silent fallback is forbidden.
4. **Atomic write.** Trainer writes via `.tmp + rename` (or `O_TMPFILE + linkat`) so engine never reads partial artifact.
5. **Tests cover refusal path AND warn-mode observability path.** Two tests minimum: (a) corrupted artifact + strict=1 → refuses; (b) corrupted artifact + strict=0 → warns + flag visible in PerCoreSnap.

**Why:** train-serve drift via incomplete handoff verification is the #1 silent-bug source in production ML systems. v5.9 is the codebase's investment in structural prevention. Each new artifact added without all 5 satisfied is a future v5.X.Y bug waiting to ship.

**How to apply:** when adding a new train-serve artifact (Phase 4 adds the scaler; future phases may add similar artifacts), check off all 5 before merge. Cross-references:
- `DOCS/CLAUDE_ML_INVARIANTS.md` — ML-specific rules per artifact
- `DOCS/PARITY_LIFECYCLE.md` — operator-facing change matrix
- `tests/INVARIANTS_MAP.md` — verification-coverage table
