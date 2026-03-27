# CLAUDE.md

## Overview

Tick-level crypto trading engine in C++. Branchless fixed-point arithmetic, bitmap-based portfolio management, regression-driven gate adjustment with regime detection. Single-symbol, single-threaded hot path with multicore TUI.

## Build

CMake with zero-dependency ANSI TUI (default):
```bash
cmake -B build && cmake --build build         # production (ANSI TUI, no deps)
cmake -B build -DUSE_FTXUI=ON && cmake --build build      # FTXUI TUI (auto-fetched)
cmake -B build -DUSE_NOTCURSES=ON && cmake --build build  # notcurses TUI (experimental)
./build/controller_test                        # run tests (134 assertions)
cd build && ./engine                           # run engine (needs engine.cfg symlink)
```

Build options: `-DLATENCY_PROFILING=ON`, `-DLATENCY_LITE=ON`, `-DLATENCY_BENCH=ON`, `-DBUSY_POLL=ON`, `-DUSE_NATIVE_128=ON`. See README.md for details.

## Architecture

```
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
- **DataStream/** - FauxFIX, MockGenerator, TradeLog, BinanceCrypto (websocket), BinanceOrderAPI (REST orders), EngineTUI (snapshot, thread), TUIAnsi (default, zero-dep), TUIWidgets/TUILayout (FTXUI opt-in), TUINotcurses (notcurses, experimental)
- **FixedPoint/** - FPN arbitrary-width fixed-point arithmetic library
- **MemHeaders/** - PoolAllocator (bitmap order pool), BuddyAllocator
- **ML_Headers/** - RollingStats (regression + R²), LinearRegression3X, ROR_regressor (slope-of-slopes), GateControlNetwork
- **tests/** - controller_test.cpp (134 assertions)
- **plans/** - implementation plans (gitignored)

## Versioning

Version string: `engine vX.Y.Z` — hardcoded in 4 TUI files (TUIAnsi.hpp, EngineTUI.hpp ×2, TUINotcurses.hpp). Update ALL four when bumping.

- **X.Y.Z** follows changelog version in `DOCS/CHANGELOG.md` (version summary table, top row)
- **Patch (Z)**: bug fixes, guards, config changes, TUI tweaks
- **Minor (Y)**: new features (strategies, regime types, TUI modes), breaking config changes
- **Major (X)**: architectural rewrites (FPN width change, new hot-path design)

### Release process
1. Update version string in all 4 TUI files (search for `engine v`)
2. Update `DOCS/CHANGELOG.md` version summary table
3. Create detailed changelog in `DOCS/changelogs/YYYY-MM-DD-X.md`
4. Commit, push to main
5. Tag: `git tag v3.0.21 && git push origin v3.0.21`

### Tag conventions
- `vX.Y.Z` — release tags (pushed to remote)
- `pre-*` — rollback points before risky changes (local)
- `rollback-*` — named rollback points (local)
- `backup/*` — branch backups (local)

## Code Conventions

- `using namespace std;` used throughout
- C-style with templates, no classes
- Functions follow Pattern_FunctionName convention (e.g. Portfolio_Init, BuyGate)
- All hot-path math uses FPN<F> fixed point, no floats (F=64 words = 4096-bit precision)
- Branchless patterns: mask tricks with -(uint64_t)pass, word-level mask-select
- Inline comments explain reasoning and learning insights
- User's voice/comments must be preserved exactly when editing existing files

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

## Current State

- Portfolio controller: COMPLETE (140/140 tests passing)
- Post-SL cooldown: pauses buying for N cycles after stop loss (anti-falling-knife)
- Regime detection: score-based with 7 signals, extensible RegimeSignals struct
- Volume spike detection: current/max ratio, spacing relaxation on 5x+ spikes
- RollingStats: real least-squares regression (slope, R², variance)
- Snapshot persistence: v7 (entry_time + session stats survive restarts)
- Binance websocket: WORKING (live market data)
- TUI: ANSI TUI is default (zero deps, diff-based rendering, foxml palette), FTXUI/notcurses opt-in
- TUI snapshot: zero-pollution (full copy on slow path, live price/volume/active_count every tick)
- Momentum TP/SL: adaptive (R²-scaled + ROR acceleration bonus at fill time)
- Slippage simulation: configurable entry/exit price adjustment (slippage_pct in engine.cfg)
- Single-slot mode: max_positions=1 (default), sells entire BTC balance on exit (no dust)
- Paper/live sync: unbacked paper positions are undone, startup recovers orphaned BTC

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
