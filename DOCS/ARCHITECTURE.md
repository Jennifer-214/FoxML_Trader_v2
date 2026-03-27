# Architecture

## Overview

Tick-level crypto trading engine. Regime-adaptive strategy switching between mean reversion and momentum. Branchless fixed-point arithmetic on the hot path. Connects to Binance websocket for live market data, paper trades with simulated fills. Optional multicore TUI (engine core 0, display core 1).

## Data Flow

```
[Binance WSS] -> [TLS Socket] -> [WebSocket Frame Parser] -> [JSON Parser]
                                                                    |
                                                             [FPN_FromString]
                                                                    |
                                                              [DataStream<F>]
                                                                    |
                  +---------------------+---------------------------+
                  |                     |                           |
             [BuyGate]          [PositionExitGate]         [PortfolioController]
          (direction-aware)    (bitmap walk, TP/SL)              |
                  |                     |              +----------+-----------+
             [OrderPool]          [ExitBuffer]         |                     |
                  |                     |         [RegimeDetector]    [Strategy Dispatch]
                  +---------------------+         RANGING/TRENDING    MR or Momentum
                                        |         /VOLATILE           Adapt+BuySignal
                                 [Trade Decision]                    +ExitAdjust
                                        |
                            [TradeLog CSV + MetricsLog + TUI]
```

## Event Loop (main.cpp)

Multicore by default (engine core 0, TUI core 1). One iteration:

1. `BinanceStream_Poll()` — checks SSL_pending first, then poll() on socket
2. **Burst drain** — read ALL buffered frames. Exit gates run on every intermediate price
3. **TUI input** — handle q/p/r/s commands (shared functions: Unpause, CycleRegime, HotReload)
4. **Session lifecycle** — wind-down and reconnect checks
5. **Engine tick** — BuyGate -> PositionExitGate -> PortfolioController_Tick
6. **Metrics logging** — regime changes, strategy switches, fills logged to CSV
7. **TUI snapshot** — every 10 ticks, copy state to double-buffered snapshot

## Hot Path (every tick, p50 ~950ns)

All branchless except unavoidable bitmap loops:

| Function | Description | Cost |
|---|---|---|
| BuyGate | direction-aware price/volume check (0=below, 1=above) | ~80ns |
| PositionExitGate | bitmap walk, per-position TP/SL | ~130ns/pos |
| Fill consumption | pool -> portfolio, strategy-aware TP/SL sizing | ~750ns avg, ~4μs on fill |

## Slow Path (every poll_interval ticks, ~25μs)

1. Drain exit buffer, log sells with full context (strategy, regime, fees)
2. Time-based exit: close positions held too long with low gain
3. Update rolling stats (128-tick short + 512-tick long windows)
4. **Regime detection**: classify market as RANGING, TRENDING, or VOLATILE
5. **Position adjustment**: modify TP/SL on existing positions if regime changed
6. **Strategy dispatch**: run active strategy's Adapt + ExitAdjust + BuySignal
7. Compute unrealized P&L (net of estimated exit fees)
8. VOLATILE regime: pause buying (set buy_conds to zero)

## Regime Detection

Classifies market state from rolling statistics:

| Regime | Condition | Strategy | Action |
|--------|-----------|----------|--------|
| RANGING | low slope OR low R² | Mean Reversion | Buy dips below avg |
| TRENDING | high slope AND high R² | Momentum | Buy breakouts above avg |
| VOLATILE | high stddev AND low R² | (paused) | No new entries |

- Hysteresis: proposed regime must hold N slow-path cycles before switching
- Cold start: stays RANGING until 64+ rolling samples available
- Position adjustment on switch: MR→momentum widens TP/tightens SL; reverse for momentum→MR

## Strategy System

Each strategy implements 4 functions + state struct:

```
PREFIX_Init()       — set initial buy conditions from rolling stats
PREFIX_Adapt()      — P&L regression feedback, idle squeeze
PREFIX_BuySignal()  — compute buy gate conditions + gate_direction
PREFIX_ExitAdjust() — trailing TP/SL logic
```

Dispatch via `switch(strategy_id)`. Both strategies initialized at warmup.
Adding strategy #3: new header + dispatch case + unpause case + config fields.

### Shared Functions (PortfolioController.hpp)

Eliminate duplication between multicore and single-threaded paths:
- `PortfolioController_HotReload()` — config reload
- `PortfolioController_Unpause()` — dispatch to active strategy
- `PortfolioController_CycleRegime()` — manual regime cycle for testing

## Logging

### Trade Log (`{symbol}_order_history.csv`)
Every buy/sell with: tick, price, qty, entry, delta%, exit reason, TP, SL,
buy gate conditions, stddev, avg, balance, fees, spacing, gate distance, strategy, regime.

### Metrics Log (`{symbol}_metrics.csv`)
Every slow-path cycle: full engine state snapshot. Events: REGIME_CHANGE, STRATEGY_SWITCH, FILL.
Append mode — survives restarts.

### Snapshot (`portfolio.snapshot`, v5)
Binary state persistence: positions, entry_ticks, entry_strategy, strategy_id,
regime state, MR + momentum adaptive values, balance, realized P&L.
Backward compatible with v4.

## Network Stack (DataStream/BinanceCrypto.hpp)

Six layers, all in one header: TCP -> TLS (OpenSSL) -> WebSocket -> Ping/Pong -> JSON -> FPN.

## Session Lifecycle (24-hour cycle)

```
CONNECT -> WARMUP (128 ticks, ~43s) -> ACTIVE -> WIND DOWN (5 min) -> CLOSE ALL -> RECONNECT
```

## File Map

```
CoreFrameworks/
  OrderGates.hpp           BuyGate (direction-aware), SellGate
  Portfolio.hpp            Position storage, ExitGate, bitmap ops, snapshot format
  PortfolioController.hpp  Tick function, dispatch, shared functions, snapshot v5
  ControllerConfig.hpp     Config struct, parser, defaults

Strategies/
  StrategyInterface.hpp    Strategy contract + enum definitions
  MeanReversion.hpp        Buy dips, stddev-scaled offset, P&L adaptation
  Momentum.hpp             Buy breakouts, trend-following, tighter SL
  RegimeDetector.hpp       Market classification + position adjustment

DataStream/
  BinanceCrypto.hpp        WebSocket stream (TCP/TLS/WS/JSON)
  BinanceOrderAPI.hpp      REST order execution (HMAC-SHA256, market orders)
  EngineTUI.hpp            Terminal dashboard, multicore snapshot
  TradeLog.hpp             CSV logger + ring buffer
  MetricsLog.hpp           Slow-path diagnostics CSV
  FauxFIX.hpp              FIX protocol parser (mock data)

FixedPoint/
  FixedPointN.hpp          Arbitrary-width fixed-point arithmetic
  FixedPoint64.hpp         Static 128-bit FP (experimental)

ML_Headers/
  RollingStats.hpp         Price/volume moving avg, stddev, slope
  LinearRegression3X.hpp   3-sample rolling regression
  ROR_regressor.hpp        Slope-of-slopes (second derivative)

tests/
  controller_test.cpp      134 assertions
  integration_test.cpp     Full pipeline test
  binance_test.cpp         Live data integration test

main.cpp                   Engine entry point, event loop
engine.cfg                 Annotated configuration
Makefile                   Build targets: make, make profile, make test
```
