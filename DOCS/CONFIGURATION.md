# Configuration

All settings are in `engine.cfg` (key=value format). Lines starting with `#` are comments. The controller and binance configs are parsed from the same file.

## Data Source

| Key | Default | Description |
|---|---|---|
| `symbol` | `btcusdt` | Trading pair (lowercase) |
| `use_testnet` | `0` | 1 = testnet endpoint, 0 = production |
| `use_binance_us` | `0` | 1 = binance.us endpoint |
| `poll_timeout_ms` | `100` | poll() timeout in milliseconds |
| `reconnect_delay` | `5` | seconds to wait before reconnect |

## Session Lifecycle

| Key | Default | Description |
|---|---|---|
| `wind_down_minutes` | `5` | stop buys X minutes before reconnect |

## Warmup

| Key | Default | Description |
|---|---|---|
| `warmup_ticks` | `128` | ticks to observe before trading (fills rolling window) |

## Regression / Gate Adjustment

| Key | Default | Description |
|---|---|---|
| `poll_interval` | `100` | ticks between slow-path runs |
| `r2_threshold` | `0.30` | min R^2 to trust regression |
| `slope_scale_buy` | `0.50` | how much P&L slope shifts buy price |
| `max_shift` | `5.00` | max price drift from initial conditions ($) |

## Position Exit Conditions

| Key | Default | Description |
|---|---|---|
| `take_profit_pct` | `3.00` | TP as % (also used as stddev multiplier for volatility-based exits) |
| `stop_loss_pct` | `1.50` | SL as % (also used as stddev multiplier for volatility-based exits) |

When rolling stats are populated, TP/SL are computed as `entry +/- (stddev * pct * 100)`. Falls back to percentage-based when stats aren't ready. TP has a fee floor: minimum TP = `entry + entry * fee_rate * 3` to ensure profitable exits actually profit after round-trip fees.

## Paper Trading

| Key | Default | Description |
|---|---|---|
| `starting_balance` | `10000.00` | Starting paper trading balance ($) |
| `fee_rate` | `0.10` | Per-trade fee as % (0.10 = 0.1% for Binance) |
| `risk_pct` | `2.00` | Fraction of balance to risk per position (%) |
| `slippage_pct` | `0.00` | Simulated execution slippage (%, 0 = disabled) |

Position quantity is computed as `(balance * risk_pct) / price`. At $10k balance and 2% risk, each position is ~$200.

Slippage adjusts fill prices to simulate real execution: buys fill higher (`price + price * slippage_pct`), sells fill lower (`price - price * slippage_pct`). Applied to all exit paths (TP/SL, time-based). Typical: `0.05` (0.05%).

## Live Trading

| Key | Default | Description |
|---|---|---|
| `use_real_money` | `0` | 0 = paper trading (default), 1 = real orders via Binance REST API |

When enabled, the engine places real market orders on Binance. Requires `secrets.cfg` with API credentials. Defaults to testnet (`use_testnet=1`). Set `use_binance_us=1` for Binance US endpoint.

**Setup:**
1. Get API keys from Binance (testnet or live)
2. Create `secrets.cfg` (gitignored): `api_key=xxx` and `api_secret=xxx`
3. Set `use_real_money=1` in engine.cfg

**Safety:** Engine runs paper logic first (all validation gates), then submits matching real order. If REST fails, paper position is rolled back. 10-second countdown before production trading.

## Risk Management

| Key | Default | Description |
|---|---|---|
| `max_drawdown_pct` | `10.00` | Halt trading when total P&L drops below this % of starting balance |
| `max_exposure_pct` | `50.00` | Max % of starting balance deployed in open positions |
| `max_positions` | `1` | Max simultaneous open positions (1-16). At 1, the engine sells entire BTC balance on exit (no dust). |

Circuit breaker trips when `realized_pnl + unrealized_pnl < -(starting_balance * max_drawdown_pct)`. Exit gates keep running. Resets on restart.

## Market Microstructure Filters

These are initial values that adapt at runtime based on P&L regression.

| Key | Default | Description |
|---|---|---|
| `volume_multiplier` | `3.00` | require tick volume >= this * rolling avg |
| `entry_offset_pct` | `0.15` | buy gate offset below rolling mean (%) |
| `spacing_multiplier` | `2.00` | min entry spacing = stddev * this |

## Adaptive Filter Clamps

Control how far the adaptive filters can drift from initial values.

| Key | Default | Description |
|---|---|---|
| `offset_min` | `0.05` | most aggressive offset (%) |
| `offset_max` | `0.50` | most defensive offset (%) |
| `vol_mult_min` | `1.50` | most aggressive volume multiplier |
| `vol_mult_max` | `6.00` | most defensive volume multiplier |
| `filter_scale` | `0.50` | how fast filters adapt to P&L slope |

## Enhanced Buy Signal (Stddev Mode)

| Key | Default | Description |
|---|---|---|
| `offset_stddev_mult` | `1.50` | stddev multiplier for buy offset (0 = use percentage mode) |
| `offset_stddev_min` | `0.50` | adaptation lower bound (most aggressive) |
| `offset_stddev_max` | `4.00` | adaptation upper bound (most defensive) |
| `min_long_slope` | `-0.00005` | min long-window slope to allow buys (0 = disabled) |

## Trailing Take-Profit

| Key | Default | Description |
|---|---|---|
| `tp_hold_score` | `0.15` | min SNR*R² to hold past TP (0 = disabled) |
| `tp_trail_mult` | `1.00` | trailing TP distance in stddevs below price |
| `sl_trail_mult` | `2.00` | trailing SL distance in stddevs below price |

## Time-Based Exit

| Key | Default | Description |
|---|---|---|
| `max_hold_ticks` | `50000` | close position after this many ticks (0 = disabled) |
| `min_hold_gain_pct` | `0.10` | only time-exit if gain < this % |

## Regime Detection

| Key | Default | Description |
|---|---|---|
| `regime_slope_threshold` | `0.001` | relative slope for TRENDING detection |
| `regime_r2_threshold` | `70.00` | min R² consistency for TRENDING (%) |
| `regime_volatile_stddev` | `0.003` | stddev/price ratio for VOLATILE detection |
| `regime_hysteresis` | `5` | slow-path cycles before regime switch |

## Momentum Strategy

| Key | Default | Description |
|---|---|---|
| `momentum_breakout_mult` | `1.50` | buy when price > avg + stddev * this |
| `momentum_tp_mult` | `3.00` | TP distance in stddevs (wider than MR) |
| `momentum_sl_mult` | `1.00` | SL distance in stddevs (tighter than MR) |

## TUI

| Key | Default | Description |
|---|---|---|
| `tui_enabled` | `1` | 0 = headless mode (no terminal output) |
| `tui_render_interval` | `10` | render every N ticks |

## Hot-Swappable vs Restart-Required

**Hot-swappable** (press `r` in TUI to reload):
- poll_interval, r2_threshold, slope_scale_buy, max_shift
- take_profit_pct, stop_loss_pct (only affects NEW positions)
- volume_multiplier, entry_offset_pct, spacing_multiplier
- All adaptive filter clamps

**Requires restart:**
- symbol (needs websocket reconnect)
- use_testnet, use_binance_us (needs reconnect)
- warmup_ticks (already completed)

## Example Config

```
# data source
symbol=btcusdt
use_testnet=0
use_binance_us=0
poll_timeout_ms=100
reconnect_delay=5

# session lifecycle
wind_down_minutes=5

# warmup phase
warmup_ticks=64

# portfolio-level
poll_interval=100
r2_threshold=0.30
slope_scale_buy=0.50
max_shift=5.00

# per-position exit conditions
take_profit_pct=3.00
stop_loss_pct=1.50

# paper trading
starting_balance=10000.00
fee_rate=0.10
risk_pct=2.00
slippage_pct=0.05

# risk management
max_drawdown_pct=10.00
max_exposure_pct=50.00
max_positions=1

# market microstructure filters
volume_multiplier=3.00
entry_offset_pct=0.15
spacing_multiplier=2.00

# adaptive filter clamps
offset_min=0.05
offset_max=0.50
vol_mult_min=1.50
vol_mult_max=6.00
filter_scale=0.50

# TUI
tui_enabled=1
tui_render_interval=10
```
