# Adding a New Strategy

How to add a new trading strategy to the engine.

## Steps

1. **Create `Strategies/NewStrategy.hpp`** — implement 4 functions + state struct:
   - `NewStrategyState<F>` — regression feeders, adaptive parameters
   - `NewStrategy_Init()` — set initial buy conditions from rolling stats
   - `NewStrategy_Adapt()` — P&L regression feedback, idle squeeze
   - `NewStrategy_BuySignal()` — compute buy gate conditions, set gate_direction
   - `NewStrategy_ExitAdjust()` — trailing TP/SL logic

2. **`Strategies/StrategyInterface.hpp`** — add `#define STRATEGY_NEW 2`

3. **`CoreFrameworks/PortfolioController.hpp`** — 3 changes:
   - Add `NewStrategyState<F> new_strat;` to controller struct (warm section)
   - Add `case STRATEGY_NEW:` to `Strategy_Dispatch` switch (~line 525)
   - Add `case STRATEGY_NEW:` to `PortfolioController_Unpause` switch (~line 560)
   - Init the state in `PortfolioController_Init` (~line 115)

4. **`CoreFrameworks/ControllerConfig.hpp`** — add config fields with defaults

5. **`Strategies/RegimeDetector.hpp`** — map regime → strategy in `Regime_ToStrategy`

6. **`engine.cfg`** — add config values with documentation

## Patterns to Follow

- Use `MeanReversion.hpp` as the reference implementation
- All hot-path patterns must be branchless (mask-select, not if/else)
- `gate_direction = 0` for buy-below (dips), `gate_direction = 1` for buy-above (breakouts)
- Adaptation clamps must be configurable (min/max from config)
- Push to `price_feeder` in Adapt for trailing R² computation
- BuySignal sets `gate_direction` on the returned `buy_conds`

## Key Files

| File | What to change |
|------|----------------|
| `Strategies/NewStrategy.hpp` | NEW — strategy implementation |
| `Strategies/StrategyInterface.hpp` | Add enum |
| `CoreFrameworks/PortfolioController.hpp` | State + dispatch + unpause + init |
| `CoreFrameworks/ControllerConfig.hpp` | Config fields + defaults + parser |
| `Strategies/RegimeDetector.hpp` | Regime→strategy mapping |
| `engine.cfg` | Config values |

## What NOT to change

- `OrderGates.hpp` — BuyGate is strategy-agnostic (reads buy_conds)
- `Portfolio.hpp` — Position struct is strategy-agnostic (TP/SL per position)
- `main.cpp` — uses shared functions, no strategy-specific code
- `DataStream/EngineTUI.hpp` — uses shared functions, strategy name auto-detected
