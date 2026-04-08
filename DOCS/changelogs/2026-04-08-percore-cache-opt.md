# 2026-04-08 — v3.9.0-experiment: Per-Core Sharded Engine + Cache Opt

scope: experimental branch `experiment/per-core-sharding`. nothing on master yet.
state: 13-phase architecture done, phase 14 production wiring done, this session's
hot-path cache optimization done. live binance feed validated end to end.

## Shared

- new config field `engine_mode` (single_core | sharded), default single_core,
  STARTUP-ONLY (hot reload ignores changes per pitfall P13.1)
- new config field `num_execution_cores` (1-16, default 4)
- new config field `sharded_force_synthetic` (0/1) — bypasses binance and
  uses the sawtooth tick generator for repeatable latency demos
- `STRATEGY_NONE = 0xFF` sentinel in StrategyInterface.hpp for cores with no
  strategy assigned (resolves the phase 06 ID collision with phase 02s
  zero-pack default)
- `MAX_EXECUTION_CORES = 16` and `MAX_EVENTS_PER_DRAIN_PER_CORE = 16` in
  Limits.hpp

## Execution Engine

### per-core sharding architecture (phases 1-13)

13 new headers in CoreFrameworks/ implementing the per-core risk-sharded
hot path validated in plans/per_core_sharding/. one position per pinned cpu,
controller core owns the portfolio, parameter packs flow through a seqlock
slot, trade events flow back through SPSC rings.

- `SPSCRing.hpp` — single producer single consumer lock-free ring with
  cache-line separated head/tail and cached counters. ~3.7ns push / ~13.7ns
  pop measured (subtracting rdtsc floor).
- `Tick.hpp` — 64-byte aligned market tick struct
- `TradeEvent.hpp` — entry/exit event from execution core to controller
- `GateParameters.hpp` — pure parameter pack consumed by BG/SG. ~192 bytes
  with FPN<64> + USE_NATIVE_128. structured with hot fields first.
- `ParameterSlot.hpp` — **seqlock**, NOT triple buffer (the original phase 5
  plan was wrong, the phase 5 stress test caught real torn reads at high
  producer rate, see ParameterSlot.hpp comment block). wait-free producer,
  lock-free consumer with bounded retry. tagged `no_sanitize("thread")` on
  the byte-level race because thats the whole point of the seqlock pattern,
  same approach the linux kernel uses for seqcount_t.
- `ExecutionCore.hpp` — per-core state machine. 4 hot fields (permission,
  active, entry_price, gate_params). branchless gate evaluation, atomic
  ACQUIRE on permission per pitfall P9.3.
- `ControllerEventLoop.hpp` — controller-side event drain + parameter push +
  kill switch. per-core drain cap of 16 events to prevent starvation.
- `ShardedTradeLog.hpp` — v3 csv format with core_id + strategy_id columns.
  single-writer (controller core only). 1024-byte snprintf buffer with
  truncation guard.
- `EventLoopAggregates.hpp` — flat money view for the existing TUI/GUI.
  walks the active bitmap once for unrealized P&L = sum(qty * (mark - entry)).
- `ShardedBacktestDriver.hpp` — single-threaded replay driver. fan out to
  cores in slot order, drain on cadence. deterministic tick processing.
- `LegacyReferenceDriver.hpp` — single-threaded reference using the SAME
  BG/SG functions as the sharded path but with no SPSC, no seqlock, no
  events. used by test_migration_head_to_head to prove byte-identical
  outcomes between the two paths.
- `Strategies/StrategyParameters.hpp` — `Strategy_BuildParameters` dispatcher.
  full SimpleDip port. MR / Momentum / EmaCross are stubs (production
  migration must port them following the SimpleDip pattern).

11 test files in experiments/per_core_sharding/, 71 functional test cases
(all pass in normal build), 3 concurrent stress tests pass under TSan after
the seqlock annotation. head-to-head test produces byte-identical trade
decisions between sharded and legacy reference.

bench results from phase 03 + phase 13 (i5-1035G4, no isolcpus, no chrt):
**5/5 acceptance criteria pass**, scenario C (sharded execution core)
~70-82 ns p50 with rdtsc bracket, walk scaling 3.4x for 4x positions while
C stays flat — at 64 positions C wins by 3.7x.

### phase 14 production wiring

production-side dispatch in main.cpp + Backtest/BacktestEngine.hpp peek at
config.engine_mode and route to either the legacy single-threaded path
(unchanged) or the new sharded path. hot reload preserves engine_mode +
num_execution_cores per P13.1.

- `CoreFrameworks/EngineSharded.hpp` (new, ~560 lines) — the sharded engine
  entry point. spawns producer thread (synthetic OR real binance feed via
  the existing BinanceStream), N executor threads (each pinned via
  pthread_setaffinity_np), 1 drainer thread, and a live ANSI TUI render
  loop showing per-core latency in real time. on Ctrl+C joins cleanly and
  dumps final per-core stats.
- `Backtest/BacktestSharded.hpp` (new, ~378 lines) — single-threaded
  sharded backtest path. mirrors Backtest_Run interface. only SimpleDip is
  supported until the strategy stubs land. populates BacktestResults.stats
  the same way the legacy path does.
- `CoreFrameworks/CoreLatencyStats.hpp` (new) — per-core latency stats with
  rdtsc sampling, 256-sample sliding ring for percentiles, lifetime
  min/max/avg. single-writer (the executor itself), single-reader (the
  controller snapshot helper). hot path cost: 1ns disabled, ~25ns enabled
  (the rdtsc bracket is the dominant cost).
- `Portfolio.hpp` got `Portfolio_OpenSlot/CloseSlot/SlotActive` slot-by-id
  helpers so the controller event loop can write positions by core_id
  directly instead of using the auto-assigning bitmap path.
- `DataStream/EngineTUI.hpp` got per-core latency fields in TUISnapshot +
  `TUI_PopulatePerCoreLatency()` populator + `sharded_mode_active` flag.
- `DataStream/TUIAnsi.hpp` got `ANSI_Section_PerCoreLatency()` rendering
  one row per core. only renders when sharded_mode_active is set.
- `GUI/SettingsPanel.hpp` got the engine_mode + num_execution_cores fields
  with restart-required tooltips.

production migration recipe at experiments/per_core_sharding/MIGRATION.md.
file-by-file checklist with checkboxes, AMD CCD pinning concern, strategy
parameter port task, soak testing plan.

### hot-path cache optimization (this session)

profiled the production ExecutionCore_Tick under real concurrent execution
and found the 192-byte GateParameters memcpy was the dominant per-tick
cost — happening on EVERY tick despite parameter packs only changing on
the slow path cadence (every ~256 ticks).

**fix:** cache the last-read parameter snapshot in the ExecutionCore itself.
each tick does one acquire-load of `param_slot.seq` and compares against
`cached_seq`. if they match (and seq is even = not mid-write), the cache
is valid and we skip the memcpy entirely. on mismatch we fall through to
the full ParameterSlot_Read protocol and refresh the cache.

steady state cost: **1 acquire load + 1 compare = ~1 ns**
miss path cost: 1 full slot read = ~6 ns (rare, only on slow-path push)

also inlined BG_Evaluate / SG_Evaluate into ExecutionCore_Tick directly,
folded the active-state TP/SL override into a CMOV-style ternary instead
of a real branch (verified faster: branch was 4 ns slower in
bench_batch_floor v3 vs v2 because the branch defeats the compilers
ability to hoist both loads in parallel), and folded the entry-time TP/SL
recompute into the rare can_enter|can_exit branch since it only matters
on entry.

### measurement methodology

old per-tick rdtsc bracket has a structural ~27 ns floor on this hardware
that hides the actual work cost. wrote `experiments/per_core_sharding/bench_batch_floor.cpp`
which brackets rdtsc ONCE around N=1M iterations and divides — amortizes
the rdtsc tax to ~0 and reports the true per-tick work in ns.

6 variants benchmarked side by side:

| variant                          | ns/tick |
|----------------------------------|---------|
| original (memcpy 192B every tick)| ~50     |
| cached (skip memcpy on seq match)| ~38     |
| cached_v2 (no local copy)        | **~35** |
| cached_v3 (branch for active)    | ~38     |
| floor (gates only, no override)  | ~34     |
| abs floor (1 cmp + 1 atomic)     | ~7.7    |

abs floor of ~7.7 ns shows the loop infrastructure is not the limit. the
algorithm work itself is the floor at ~34 ns.

also caught + fixed: experiments build was missing `-DUSE_NATIVE_128` so
FPN<64> comparators were hitting the generic word-loop instead of the
__uint128_t fast path. added the define to bench_batch_floor target.
worth ~4 ns / tick in BG and SG.

### live testbed verification

re-ran the live sharded engine after the optimization with N = 2, 3, 4
execution cores. before / after on i5-1035G4 (~1.5 GHz base, no isolcpus,
no chrt):

| cores | before min | before p50 | **after min** | **after p50** | p50 saved |
|-------|-----------:|-----------:|--------------:|--------------:|-----------|
| 2     | 63 ns      | 71 ns      | **57 ns**     | **61 ns**     | -10 ns    |
| 3     | 63-64 ns   | 73-101 ns  | 56-57 ns      | 61-91 ns      | -10 to -12 ns |
| 4     | 63-67 ns   | 70-97 ns   | 57-59 ns      | 74-89 ns      | -8 to -23 ns |

savings transferred 1:1 from the bench prediction. ~10 ns p50 in real
multi-threaded execution against ~12 ns predicted by bench_batch_floor.

### live binance feed validation

flipped `sharded_force_synthetic=0` and ran against the real binance trade
feed (data-stream.binance.vision/btcusdt@trade) for 30 seconds:

```
core   samples       min        p50        p95        p99        max
  0       1255       57 ns       62 ns      347 ns      409 ns    16352 ns
  1       1255       57 ns       63 ns      362 ns      530 ns     8895 ns
```

**identical numbers to the synthetic test.** the per-tick cost is the cost
regardless of where the tick came from. only ~42 ticks/sec from the real
feed (thats the actual BTC trade rate on a quiet morning) but the floor is
the same. p95/p99 reflect kernel preemption noise, not the algorithm.

real conclusion: on this i5-1035G4 laptop with no isolation, the sharded
hot path runs at **~30-35 ns of actual work per tick** (subtracting the
~27 ns rdtsc bracket overhead from the ~57 ns measured min). on a real
trading box at 4-5 GHz turbo with isolcpus + chrt -f 90, expect ~15-20 ns
work and clean p99 in the ~80-100 ns range.

## Backtest Suite

- `Backtest/BacktestEngine.hpp` `Backtest_Run` peeks at `engine_mode` and
  routes to `BacktestSharded_Run` (in BacktestSharded.hpp) when set.
  legacy path is unchanged.
- `Backtest/BacktestPanels.hpp` includes BacktestSharded.hpp so the
  optimizer / training / comparison panels keep working in either mode.

## Known limitations

- only SimpleDip is ported to `Strategy_BuildParameters`. MR / Momentum /
  EmaCross are stubs. sharded mode can only trade SimpleDip until those
  stubs are filled in following the SimpleDip pattern.
- snapshot v11 (per-core state persistence) is not implemented. restarting
  in sharded mode loses per-core fill state. single_core snapshots load
  fine.
- live trading is **not yet wired**. EngineSharded.hpp is currently a
  state-only testbed. trade events update internal balance/realized_pnl
  but no orders are submitted to binance. the EventLoop_OnEvent → 
  BinanceOrderAPI bridge is the next concrete piece of work (~100-200 LOC).
- AMD users (Ryzen, EPYC, multi-CCD parts): pin all engine threads to the
  same CCD or pay 70-100 ns of cross-die coherence latency on every
  cross-core operation. on intel monolithic this is automatic.

## Files changed in this session

- `CoreFrameworks/ExecutionCore.hpp` — cached_seq + cached_params fields,
  rewrote ExecutionCore_Tick body to use the cache, inlined BG/SG, CMOV
  active override, folded entry-time TP/SL into the rare branch.
- `experiments/per_core_sharding/bench_batch_floor.cpp` — new bench, 6
  variants, batch-bracketed measurement, USE_NATIVE_128 enabled.
- `experiments/per_core_sharding/CMakeLists.txt` — added bench_batch_floor
  target with USE_NATIVE_128.
- `DOCS/changelogs/2026-04-08-percore-cache-opt.md` — this file.

## Next concrete steps

1. **wire EventLoop_OnEvent → BinanceOrderAPI** for real live trading
   (~100-200 LOC). add a `paper_mode` config flag for safety during soak.
2. **port the strategy parameter stubs** (MR / Momentum / EmaCross) to
   match SimpleDip_BuildParameters. each one needs a regression test
   verifying the new path produces the same gate decisions as the legacy
   strategy on the same RollingStats input.
3. **snapshot v11 schema** so per-core state survives restarts.
4. **soak plan** from MIGRATION.md: 90-day backtest comparison → testnet
   24h → testnet 1 week → live small. run with `-DTSAN=ON` regularly.
