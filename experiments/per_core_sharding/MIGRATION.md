# Production migration recipe — per-core sharded engine

This is the handoff doc for plugging the per-core sharded engine into the production `tick_trader_private` codebase. The experiment validates the architecture; this doc spells out exactly what the production PR needs to do.

## What the experiment delivered

13 phases of work in `experiments/per_core_sharding/`. The deliverable is in two parts:

**New headers in `CoreFrameworks/`** (drop-in for production):
- `SPSCRing.hpp` — single-producer single-consumer lock-free ring with cache-line separated head/tail and cached counters
- `Tick.hpp` — 64-byte aligned market tick struct
- `TradeEvent.hpp` — entry/exit event passed from execution core to controller
- `GateParameters.hpp` — pure parameter pack consumed by BG_Evaluate / SG_Evaluate
- `ParameterSlot.hpp` — seqlock for atomic parameter handoff (wait-free producer, lock-free consumer)
- `ExecutionCore.hpp` — per-core state machine with branchless hot path
- `ControllerEventLoop.hpp` — controller-side event drain + parameter push + kill switch
- `ShardedTradeLog.hpp` — v3 CSV trade log writer
- `EventLoopAggregates.hpp` — flat money view for the existing TUI
- `ShardedBacktestDriver.hpp` — single-threaded replay driver for backtest
- `LegacyReferenceDriver.hpp` — reference single-threaded path used in the head-to-head test

**New strategy parameter file in `Strategies/`**:
- `StrategyParameters.hpp` — `Strategy_BuildParameters` dispatcher + `SimpleDip_BuildParameters` full port + stubs for MR/Momentum/EmaCross

**Test coverage**: 11 test files, 47 functional test cases passing in normal build, 3 concurrent stress tests passing under TSan, 1 head-to-head comparison passing byte-for-byte.

## Production migration steps

### Step 1 — Copy headers (no-op for code, just adds files)

Copy these files from the experiment worktree to the production tree:

```
cp ~/tick-trader-percore/CoreFrameworks/SPSCRing.hpp           ~/tick_trader_private/CoreFrameworks/
cp ~/tick-trader-percore/CoreFrameworks/Tick.hpp               ~/tick_trader_private/CoreFrameworks/
cp ~/tick-trader-percore/CoreFrameworks/TradeEvent.hpp         ~/tick_trader_private/CoreFrameworks/
cp ~/tick-trader-percore/CoreFrameworks/GateParameters.hpp     ~/tick_trader_private/CoreFrameworks/
cp ~/tick-trader-percore/CoreFrameworks/ParameterSlot.hpp      ~/tick_trader_private/CoreFrameworks/
cp ~/tick-trader-percore/CoreFrameworks/ExecutionCore.hpp      ~/tick_trader_private/CoreFrameworks/
cp ~/tick-trader-percore/CoreFrameworks/ControllerEventLoop.hpp ~/tick_trader_private/CoreFrameworks/
cp ~/tick-trader-percore/CoreFrameworks/ShardedTradeLog.hpp    ~/tick_trader_private/CoreFrameworks/
cp ~/tick-trader-percore/CoreFrameworks/EventLoopAggregates.hpp ~/tick_trader_private/CoreFrameworks/
cp ~/tick-trader-percore/CoreFrameworks/ShardedBacktestDriver.hpp ~/tick_trader_private/CoreFrameworks/
cp ~/tick-trader-percore/Strategies/StrategyParameters.hpp     ~/tick_trader_private/Strategies/
```

These are additive — they don't touch any existing production code.

### Step 2 — Add `engine_mode` config field

In `CoreFrameworks/ControllerConfig.hpp`:

```cpp
// Add to the enum/constant block
constexpr uint8_t ENGINE_MODE_SINGLE_CORE = 0;
constexpr uint8_t ENGINE_MODE_SHARDED     = 1;

// Add to ControllerConfig<F> struct
uint8_t engine_mode;
uint16_t num_execution_cores;
```

In `ControllerConfig_Default`:
```cpp
cfg.engine_mode = ENGINE_MODE_SINGLE_CORE;
cfg.num_execution_cores = 4;
```

In `ControllerConfig_Load` parser table (follow the CLAUDE.md "Adding a new config field" pattern):
```cpp
if (strcmp(key, "engine_mode") == 0) {
    if (strcmp(val, "sharded") == 0) cfg.engine_mode = ENGINE_MODE_SHARDED;
    else cfg.engine_mode = ENGINE_MODE_SINGLE_CORE;
}
CFG_PARSE_U32(num_execution_cores)
```

### Step 3 — Hot-reload protection (P13.1)

In `PortfolioController_HotReload`:
```cpp
// engine_mode and num_execution_cores are startup-only — preserve them
// across hot reloads
uint8_t  saved_mode = ctrl->config.engine_mode;
uint16_t saved_cores = ctrl->config.num_execution_cores;
ctrl->config = new_config;
ctrl->config.engine_mode = saved_mode;
ctrl->config.num_execution_cores = saved_cores;
if (new_config.engine_mode != saved_mode) {
    fprintf(stderr, "WARNING: engine_mode change in hot reload ignored, restart required\n");
}
```

### Step 4 — Engine startup dispatch (in `main.cpp`)

```cpp
if (config.engine_mode == ENGINE_MODE_SHARDED) {
    fprintf(stderr, "starting in sharded mode with %u execution cores\n",
            config.num_execution_cores);
    Engine_RunSharded(&config);  // new — see below
} else {
    Engine_RunLegacy(&config);   // existing single-threaded path, possibly renamed
}
```

`Engine_RunSharded` does the production thread spawning + pinning. The minimal version:
1. Construct `EventLoopState<64>` with starting balance + fee rate from config
2. Construct N `ExecutionCore<64>` + N tick rings, register each with the event loop
3. Assign strategies via `EventLoopState_SetCoreStrategy` based on Regime_AllocateCores
4. Configure kill switch via `EventLoopState_ConfigureKillSwitch`
5. Optionally attach a `ShardedTradeLog` via `EventLoopState_AttachTradeLog`
6. Spawn N execution core threads (each pinned, each running ExecutionCore_Tick in a loop on its tick ring)
7. Spawn 1 controller thread running `EventLoop_RunController`
8. Spawn 1 market reader thread that fans ticks out to all N tick rings
9. On SIGTERM: set shutdown flag, join all threads

**Pinning concern**: pin all N+2 threads to the same NUMA node / CCD on AMD parts. On Intel monolithic this is automatic. See the CCD section below.

### Step 5 — Backtest dispatch

In `Backtest/BacktestEngine.hpp` `Backtest_Run`:
```cpp
if (bt->config.engine_mode == ENGINE_MODE_SHARDED) {
    BacktestSharded_Run(bt, results);  // wraps ShardedBacktest_Run
} else {
    BacktestLegacy_Run(bt, results);   // existing inline code
}
```

`BacktestSharded_Run` is a thin wrapper:
1. Construct `EventLoopState` from `bt->ctrl` config
2. Register N execution cores
3. Wire `ShardedBacktestDriver`
4. Call `ShardedBacktest_Run(&drv, bt->ticks, bt->num_ticks)`
5. Populate `results->snapshot` from `EventLoop_GetAggregates(state, last_tick.price)`

### Step 6 — TUI snapshot adapter

In `DataStream/EngineTUI.hpp` `TUI_CopySnapshot`, add a branch for sharded mode that pulls from `EventLoopState` via the aggregates helper:

```cpp
if (ctrl->config.engine_mode == ENGINE_MODE_SHARDED) {
    EventLoopAggregates agg = EventLoop_GetAggregates(
        &ctrl->event_loop_state, ctrl->last_tick_price);
    snap->balance      = agg.balance;
    snap->equity       = agg.equity;
    snap->realized     = agg.realized_pnl;
    snap->unrealized   = agg.unrealized_pnl;
    snap->total_pnl    = agg.realized_pnl + agg.unrealized_pnl;
    snap->active_count = agg.active_position_count;
    snap->max_drawdown = agg.max_drawdown;
    snap->max_drawdown_pct  = agg.max_drawdown_pct;
    snap->kill_switch_active = agg.kill_switch_tripped;
    // ... all OTHER fields (rolling stats, regime, ML, etc.) populate the
    // same way they always did, from the existing ctrl side
}
```

Same pattern in `Backtest/BacktestSnapshot.hpp` `BacktestSnapshot_Copy`.

### Step 7 — SettingsPanel field

In `GUI/SettingsPanel.hpp` `field_defs[]`:
```cpp
{"engine_mode", &cfg.engine_mode, FIELD_TYPE_ENUM,
 {.enum_vals = {"single_core", "sharded"}},
 "single_core: legacy single-threaded engine. sharded: experimental per-core "
 "risk-sharded execution. Restart required to take effect."},
```

### Step 8 — `engine.cfg` documentation

```ini
# Engine architecture mode
#   single_core (default): legacy single-threaded engine
#   sharded:               experimental per-core risk-sharded execution
#                          requires num_execution_cores to be set
#                          REQUIRES RESTART to take effect (not hot-reloadable)
engine_mode = single_core

# Number of execution cores in sharded mode (ignored in single_core mode)
# Recommended: physical core count - 2 (one for controller, one for OS)
# Cap is MAX_EXECUTION_CORES = 16
num_execution_cores = 4
```

### Step 9 — CHANGELOG entry (v3.9.0)

```markdown
## v3.9.0 — Per-core risk-sharded execution (experimental)

### Shared
- New config field `engine_mode = single_core | sharded` (default `single_core`)
- New config field `num_execution_cores` (default 4)
- CSV trade log v3 format includes `core_id` and `strategy_id` columns
  (only when `engine_mode = sharded`, written to `_sharded_order_history.csv`)

### Execution Engine
- New `sharded` mode: per-core risk-sharded execution
- Hot path measured at ~47ns of actual work per tick on i5-1035G4 laptop
- Default remains `single_core` for safety; sharded is opt-in
- Multi-threaded: 1 controller core + 1 market reader + N execution cores
- New ExecutionCore<F> and EventLoopState<F> in CoreFrameworks/
- AMD users: pin all engine threads to one CCD to avoid cross-die latency
- Hot-reload safe: changing engine_mode requires restart, hot reloads ignore changes

### Backtest Suite
- Backtest supports both modes via the engine_mode config field
- Same dataset, both modes, head-to-head test produces byte-identical trade
  decisions (validated in experiments/per_core_sharding/test_migration_head_to_head)

### Known limitations
- Strategy parameter packs for MeanReversion / Momentum / EmaCross are stubs;
  the production migration must port them following the SimpleDip_BuildParameters
  pattern in Strategies/StrategyParameters.hpp
- Snapshot v11 (per-core state persistence) is not yet implemented; restarting
  in sharded mode loses per-core fill state. Single_core snapshots load fine.
- Sharded mode is experimental and not recommended for live trading until
  testnet soak is complete
```

## Cross-cutting concerns

### AMD CCD pinning (P13 / topology note)
On Ryzen / EPYC parts with multiple Core Complex Dies, all N+2 engine threads MUST be pinned to the same CCD or you'll pay 70-100ns of cross-die coherence latency on every cross-core operation (tick fan-out, event drain). On Intel monolithic dies (Ice Lake desktop, all consumer Intel) this isn't a concern.

Detect at startup:
```cpp
long ncores = sysconf(_SC_NPROCESSORS_ONLN);
if (config.num_execution_cores > ncores - 2) {
    fprintf(stderr, "WARNING: num_execution_cores > physical_cores - 2, may oversubscribe\n");
}
```

### Strategy parameter port (the biggest remaining task)
`Strategies/StrategyParameters.hpp` has SimpleDip fully ported but MR / Momentum / EmaCross are stubs. Each one needs:

```cpp
template <unsigned F, unsigned W>
inline void MeanReversion_BuildParameters(
    const RollingStats<F, W>* rolling,
    const ControllerConfig<F>* config,
    FPN<F> allocated_balance,
    GateParameters<F>* out
) {
    // 1. Compute the same gate thresholds the existing MeanReversion strategy
    //    produces in MeanReversion_BuySignal
    // 2. Apply the SL floor invariant (CLAUDE.md Safety Invariants)
    // 3. Set strategy_id, flags, trade_size from allocated_balance
}
```

The reference implementation lives in the existing `Strategies/MeanReversion.hpp` (and `Momentum.hpp`, `EmaCross.hpp`). The new function pulls the same gate values into a `GateParameters` pack instead of mutating `BuySideGateConditions` directly.

After porting each strategy, add a regression test that runs both implementations on the same RollingStats and verifies they produce identical packs. Pattern is in `test_strategy_parameters.cpp::test_simpledip_basic`.

### Soak testing before live use
- Step 1: backtest both modes on a 90-day dataset, compare P&L (target: within 0.1%)
- Step 2: 24-hour testnet run in sharded mode with paper trading
- Step 3: 1-week testnet run with small real positions
- Step 4: only after Step 3 is clean, recommend sharded for live trading

The risk is concurrency bugs that don't appear in single-threaded backtest. Run with `-DTSAN=ON` regularly during the soak period.

## What did NOT get built (intentional scope cuts)

- **Snapshot v11** (phase 7) — skipped during the experiment. Production needs to extend the existing snapshot format with per-core state before sharded mode can survive a restart.
- **TUI per-core debug panel** — out of scope. The existing TUI shows aggregate balance / equity / positions which is enough for the user. A per-core debug panel can be added later as a new optional view.
- **Per-core kill switch** — global only for now. Per-core (e.g. "core 3 is losing money, disable that one") is a one-line change later because each core has its own permission flag.
- **Real production legacy comparison** — the experiment compares against a `LegacyReferenceDriver` that uses the same gate functions. The production migration PR needs to also run a backtest comparison against the actual production single_core engine on the same dataset.

## File-by-file integration checklist

- [ ] `CoreFrameworks/SPSCRing.hpp` (new file)
- [ ] `CoreFrameworks/Tick.hpp` (new file)
- [ ] `CoreFrameworks/TradeEvent.hpp` (new file)
- [ ] `CoreFrameworks/GateParameters.hpp` (new file)
- [ ] `CoreFrameworks/ParameterSlot.hpp` (new file)
- [ ] `CoreFrameworks/ExecutionCore.hpp` (new file)
- [ ] `CoreFrameworks/ControllerEventLoop.hpp` (new file)
- [ ] `CoreFrameworks/ShardedTradeLog.hpp` (new file)
- [ ] `CoreFrameworks/EventLoopAggregates.hpp` (new file)
- [ ] `CoreFrameworks/ShardedBacktestDriver.hpp` (new file)
- [ ] `Strategies/StrategyParameters.hpp` (new file, full SimpleDip port + stubs)
- [ ] `CoreFrameworks/ControllerConfig.hpp` — add `engine_mode` + `num_execution_cores` fields
- [ ] `CoreFrameworks/PortfolioController.hpp` — `_HotReload` save/restore for engine_mode
- [ ] `main.cpp` — startup dispatch on engine_mode
- [ ] `main.cpp` — `Engine_RunSharded` thread spawn + pin
- [ ] `Backtest/BacktestEngine.hpp` — `Backtest_Run` dispatch on engine_mode
- [ ] `DataStream/EngineTUI.hpp` — `TUI_CopySnapshot` sharded branch using `EventLoop_GetAggregates`
- [ ] `Backtest/BacktestSnapshot.hpp` — same sharded branch
- [ ] `GUI/SettingsPanel.hpp` — `engine_mode` field + tooltip
- [ ] `engine.cfg.example` — document `engine_mode` + `num_execution_cores`
- [ ] `DOCS/CHANGELOG.md` — v3.9.0 entry
- [ ] `Strategies/StrategyParameters.hpp` — port MR / Momentum / EmaCross stubs to real implementations
- [ ] Run head-to-head: production single_core vs production sharded on a 90-day dataset, P&L within 0.1%
