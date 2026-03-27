# Contributing & Extending

Guide for adding strategies, regime handlers, and other components to the engine.

## Adding a New Strategy

A strategy is a single header file in `Strategies/` that implements 4 functions and a state struct.

### 1. Create your strategy header

```cpp
// Strategies/Scalping.hpp
#ifndef SCALPING_HPP
#define SCALPING_HPP

#include "StrategyInterface.hpp"
#include "../CoreFrameworks/ControllerConfig.hpp"
#include "../CoreFrameworks/OrderGates.hpp"
#include "../CoreFrameworks/Portfolio.hpp"
#include "../ML_Headers/LinearRegression3X.hpp"
#include "../ML_Headers/ROR_regressor.hpp"
#include "../ML_Headers/RollingStats.hpp"

template <unsigned F> struct ScalpingState {
    // your adaptive parameters, regression feeders, etc
    RegressionFeederX<F> feeder;
    RORRegressor<F> ror;
    FPN<F> live_threshold;
    BuySideGateConditions<F> buy_conds_initial;
    LinearRegression3XResult<F> last_regression;
    int has_regression;
    RegressionFeederX<F> price_feeder;
};

// called at warmup — set initial buy conditions
template <unsigned F>
inline void Scalping_Init(ScalpingState<F> *state,
                           const RollingStats<F> *rolling,
                           BuySideGateConditions<F> *buy_conds) { ... }

// called every slow-path — adapt parameters from P&L feedback
template <unsigned F>
inline void Scalping_Adapt(ScalpingState<F> *state, FPN<F> current_price,
                            FPN<F> portfolio_delta, uint16_t active_bitmap,
                            const BuySideGateConditions<F> *buy_conds,
                            const ControllerConfig<F> *cfg) { ... }

// called every slow-path — return buy gate conditions
template <unsigned F>
inline BuySideGateConditions<F> Scalping_BuySignal(ScalpingState<F> *state,
                                                     const RollingStats<F> *rolling,
                                                     const RollingStats<F, 512> *rolling_long,
                                                     const ControllerConfig<F> *cfg) { ... }

// called every slow-path — trailing TP/SL adjustments
template <unsigned F>
inline void Scalping_ExitAdjust(Portfolio<F> *portfolio, FPN<F> current_price,
                                  const RollingStats<F> *rolling,
                                  ScalpingState<F> *state,
                                  const ControllerConfig<F> *cfg) { ... }
#endif
```

### 2. Register the strategy

In `Strategies/StrategyInterface.hpp`:
```cpp
#define STRATEGY_SCALPING 2
```

### 3. Wire it into the controller

In `CoreFrameworks/PortfolioController.hpp`:

**Add state to struct** (warm section):
```cpp
ScalpingState<F> scalping;
```

**Add to `PortfolioController_Init`**:
```cpp
// init scalping state
ctrl->scalping.feeder = RegressionFeederX_Init<F>();
ctrl->scalping.price_feeder = RegressionFeederX_Init<F>();
// ... etc
```

**Add case to strategy dispatch** (~line 525):
```cpp
case STRATEGY_SCALPING:
    Scalping_Adapt(&ctrl->scalping, ...);
    Scalping_ExitAdjust(&ctrl->portfolio, ...);
    ctrl->buy_conds = Scalping_BuySignal(&ctrl->scalping, ...);
    ctrl->buy_conds.gate_direction = 0;  // or 1 for buy-above
    break;
```

**Add case to `PortfolioController_Unpause`**:
```cpp
case STRATEGY_SCALPING:
    ctrl->buy_conds = Scalping_BuySignal(&ctrl->scalping, ...);
    break;
```

### 4. Add config fields

In `CoreFrameworks/ControllerConfig.hpp`, add your strategy's parameters with defaults
and parser entries. Follow the `momentum_*` pattern.

### 5. Map to a regime (optional)

In `Strategies/RegimeDetector.hpp`, update `Regime_ToStrategy()` if your strategy
should activate under specific market conditions.

### What you DON'T need to change

- `OrderGates.hpp` — BuyGate reads `buy_conds` without knowing which strategy set them
- `Portfolio.hpp` — Position struct is strategy-agnostic
- `main.cpp` — uses shared functions, no strategy-specific code
- `EngineTUI.hpp` — strategy name auto-detected from `strategy_id`
- `TradeLog.hpp` / `MetricsLog.hpp` — auto-logs strategy name per trade

## Key Patterns

### gate_direction
- `0` = buy when price is BELOW `buy_conds.price` (mean reversion, buy dips)
- `1` = buy when price is ABOVE `buy_conds.price` (momentum, buy breakouts)

### Branchless hot path
The hot path must stay branchless. Use mask-select patterns:
```cpp
uint64_t mask = -(uint64_t)condition;  // all 1s if true, all 0s if false
result.w[i] = (a.w[i] & mask) | (b.w[i] & ~mask);
```

### P&L adaptation
Push `portfolio_delta` to a `RegressionFeederX` each slow-path cycle. When the feeder
has enough samples, compute regression and adjust your parameters based on slope
(positive P&L → more aggressive, negative → more defensive). Gate adjustments by
R² confidence.

### Idle squeeze
When portfolio is empty and price is moving away from your gate, gradually squeeze
your threshold toward a minimum. Prevents the strategy from sitting idle forever.

### price_feeder
Push `current_price` to a separate `RegressionFeederX` for trailing TP R² computation.
Without this, `ExitAdjust` trailing will never activate.

## Adding a Regime Handler

The volatile regime currently pauses buying. To add active management:

1. Create `Strategies/VolatileHandler.hpp` following the strategy pattern
2. Register as `STRATEGY_VOLATILE = 2`
3. Update `Regime_ToStrategy()` to map `REGIME_VOLATILE → STRATEGY_VOLATILE`
4. See `DOCS/FUTURE_VOLATILE_STRATEGY.md` for design options

## Project Structure

```
CoreFrameworks/     Engine internals (gates, portfolio, controller, config)
Strategies/         Strategy implementations + regime detector
DataStream/         I/O (websocket, TUI, trade log, metrics log)
FixedPoint/         Fixed-point arithmetic library
ML_Headers/         Rolling stats, regression, ROR
MemHeaders/         Memory allocators
tests/              Test suite (134 assertions)
DOCS/               Documentation
claude-skills/      AI assistant context (for Claude Code)
```

## Build & Test

```bash
make              # multicore production build
make test         # run 101 tests
make profile      # with latency profiling
make clean        # remove binaries
```

## Future: Live Trading

The engine currently paper trades. Live trading requires:
1. Binance REST API order execution (`BinanceOrderAPI.hpp`)
2. API key + secret management (`secrets.cfg`, gitignored)
3. Order status tracking (partial fills, rejections)
4. Slippage simulation for realistic paper trading

See `DOCS/NEXT_STEPS.md` for the full roadmap.

## Future: Model Integration

The strategy interface supports external model-driven conditions. A `ModelDriven`
strategy could read buy conditions from a Python model server over IPC instead of
computing them from rolling stats. See `claude-skills/model-integration.md` for the
architecture sketch.
