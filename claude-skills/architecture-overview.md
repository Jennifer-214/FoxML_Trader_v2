# Architecture Overview

Quick reference for the tick trader engine architecture.

## Hot Path (every tick, ~1μs)

```
BinanceStream_Poll() → data arrives
  ↓
BuyGate()              direction-aware branchless price+volume check
  ↓
PositionExitGate()     bitmap walk, per-position TP/SL check
  ↓
PortfolioController_Tick()
  ├─ consume fills from OrderPool (branchless bitmap diff)
  ├─ position sizing, TP/SL computation (strategy-aware)
  └─ tick_count++ → if slow path: regime detect + strategy dispatch
```

## Slow Path (every poll_interval ticks, ~25μs)

```
Regime_Classify()        → RANGING / TRENDING / VOLATILE
  ↓ (if changed)
Regime_AdjustPositions() → modify TP/SL on old-strategy positions
  ↓
Strategy_Dispatch()      → switch(strategy_id)
  ├─ MeanReversion: Adapt → ExitAdjust → BuySignal (gate_direction=0)
  └─ Momentum:      Adapt → ExitAdjust → BuySignal (gate_direction=1)
```

## Key Conventions

- All hot-path math uses `FPN<F>` fixed point (template parameter F=64)
- Branchless patterns: `uint64_t mask = -(uint64_t)condition;` then word-level select
- Functions follow `Module_FunctionName` naming (e.g. `Portfolio_Init`, `BuyGate`)
- User's inline comments are preserved exactly when editing files
- Structs ordered for L1 cache: hot fields first, cold data last
- `using namespace std;` used throughout, C-style with templates

## Struct Layout (PortfolioController)

```
HOT   Portfolio, prev_bitmap, tick_count, buy_conds, exit_buf    (~3KB)
WARM  config, balance, P&L, stats, entry_ticks, entry_strategy   (~4KB)
      strategy_id, mean_rev, momentum, regime, trade_buf          (~3KB)
COLD  rolling (6KB), rolling_long* (heap, 24KB)
```

## Snapshot (v5)

Binary format: magic "TICK" + version + portfolio + tracking + regime + strategy states.
Backward compatible with v4. Save on slow path + before reconnect + on quit.

## Config

Single `engine.cfg` with key=value parser. Hot-reload via `r` key.
`PortfolioController_HotReload()` is the single function for all reload paths.

## TUI

Multicore (default): engine core 0, TUI core 1, atomic double-buffered snapshot.
Single-threaded: TUI renders inline, same shared functions for input handling.
