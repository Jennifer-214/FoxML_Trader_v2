# Components

Detailed explanation of how each component works internally.

---

## FixedPoint/FixedPointN.hpp

Arbitrary-width fixed-point arithmetic library. Template parameter `F` sets fractional bits (64, 128, 256, etc). The engine uses `FPN<64>` which gives ~19 decimal digits of precision.

**Why fixed-point instead of float:**
- No precision surprises from IEEE 754 rounding
- Comparisons produce exact 0/1 results for branchless mask generation
- No denormals, no NaN propagation, no -0.0 edge cases
- Deterministic across platforms (same bits in = same bits out)

**Key operations:**
- `FPN_FromDouble<F>(x)` / `FPN_FromString<F>(str)` - convert to fixed-point
- `FPN_ToDouble(x)` - convert back for display
- `FPN_Add/Sub/Mul/Div` - arithmetic (Mul uses __uint128_t for carry)
- `FPN_LessThan/GreaterThan/Equal` - return int (0 or 1), not bool
- `FPN_Min/Max` - branchless via word-level mask-select
- `FPN_IsZero` - checks all words

**Storage:** `uint64_t w[N]` array + `int sign`. N = FRAC_BITS / 64 + integer words.

---

## CoreFrameworks/OrderGates.hpp

Contains the buy and sell gates - the hot-path entry/exit decision logic.

### BuyGate

```
Input:  BuySideGateConditions (price threshold, volume threshold)
        DataStream (current tick price + volume)
        OrderPool (bitmap of pending orders)
Output: conditionally writes to next free pool slot
```

Completely branchless. Computes `price_pass & volume_pass` to get a 0/1 result, converts to a 64-bit mask with `-(int64_t)pass`, finds the next free slot with `__builtin_ctzll(~bitmap)`, and conditionally sets the bit and writes the price/qty. When `pass=0`, the mask is all zeros so the OR and writes are no-ops.

### DataStream

Simple struct with two fields: `FPN<F> price` and `FPN<F> volume`. This is the interface between the data source (Binance or mock) and the engine. Everything downstream consumes this struct.

---

## CoreFrameworks/Portfolio.hpp

Bitmap-based position tracking. 16 slots, one bit per position in a `uint16_t active_bitmap`.

### Position struct
Each position stores: quantity, entry_price, take_profit_price, stop_loss_price. All FPN<F>.

### PositionExitGate (hot path)
Runs every tick. Walks the active bitmap with `__builtin_ctz`, checks each position's TP and SL against current price. Uses branchless mask-select to write exit records and clear bits. The exit buffer (`ExitBuffer`) accumulates exits for the slow path to drain and log.

### Persistence
`Portfolio_Save/Load` writes/reads the entire portfolio struct as a binary blob with a magic number and version. ~1.5KB per snapshot.

---

## CoreFrameworks/PortfolioController.hpp

The feedback loop. Consumes fills from the order pool, tracks P&L, runs regression, and adjusts buy gate conditions.

### Two phases

**Warmup (CONTROLLER_WARMUP):**
- Observes market data without trading
- Builds rolling stats (price average, volume average, stddev)
- After `warmup_ticks` samples, computes initial buy gate conditions:
  - Buy price = rolling_avg - (rolling_avg * entry_offset_pct)
  - Buy volume = rolling_avg_volume * volume_multiplier
- Transitions to active

**Active (CONTROLLER_ACTIVE):**
- Every tick: consume fills from pool, check entry spacing, check balance
- Every poll_interval ticks (slow path): update rolling stats, adjust filters, run regression

### Fill consumption (every tick)
1. Detect new fills: `pool->bitmap & ~prev_bitmap`
2. Mask to zero if portfolio full (branchless)
3. For each fill: check consolidation (same price), entry spacing (min distance), balance
4. If new position: compute TP/SL from volatility, deduct balance, log buy
5. Clear consumed fills from pool

### Entry spacing check
Walks the portfolio bitmap, computes absolute distance from fill price to each existing entry price. If any distance < `stddev * spacing_multiplier`, the fill is rejected. Branchless absolute value via word-level mask-select.

### Adaptive filters (slow path)
Four mechanisms adjust the buy gate:

1. **Rolling stats update** - buy gate price and volume are recomputed from the rolling average
   each slow-path tick using the live adaptive offset and multiplier. `buy_conds_initial` also
   tracks the rolling stats so the max_shift clamp window moves with the market (not anchored
   to stale warmup prices).

2. **P&L regression** - fits a line to recent net P&L values (after estimated exit fees). If
   slope is positive (making money), shifts buy gate price up (more aggressive). Gated by R^2
   confidence. Only fires when regression has enough data (MAX_WINDOW samples).

3. **Filter adaptation** - same P&L slope adjusts entry_offset_pct and volume_multiplier.
   Positive P&L -> loosen (smaller offset, lower volume requirement). Negative P&L -> tighten.
   Clamped to configurable min/max ranges.

4. **Idle squeeze** - when portfolio is empty and price is above buy gate, shrinks offset 10%
   toward zero each slow-path tick. Also squeezes volume multiplier toward 1.0x. Goes all the
   way to zero offset (not offset_min) so the gate can reach the rolling average. At zero
   offset, the rolling average catches up to price naturally as old ticks leave the 128-tick
   window. Stops as soon as a position is entered, then P&L-based adaptation takes over and
   pushes offset back up based on performance.

### Paper trading balance
Starts at `starting_balance` from config. Position size = `balance * risk_pct / price`. Cost deducted on buy (including entry fee), net proceeds returned on sell (after exit fee). Balance check is branchless — ANDed into the fill mask alongside spacing and capacity checks.

### Fees
Modeled at 0.1% per trade (configurable). Entry fee deducted from balance on buy, exit fee deducted from proceeds on sell. Realized P&L includes both fees in cost basis. Unrealized P&L in TUI shows both gross and net (after estimated round-trip fees). Regression feeds on net P&L so adaptive filters optimize on real profitability.

### TP fee floor
Minimum take profit = `entry * fee_rate * 3` (round-trip fees + safety margin). Prevents volatility-based TP from being set so tight that a "profitable" exit loses money after fees.

### Risk management
Six branchless checks ANDed into the fill mask (all computed, no branches):
1. Not a duplicate (existing position at same price)
2. Portfolio not full (16 slots)
3. Entry spacing (min distance from existing positions)
4. Can afford (balance >= cost + entry fee)
5. Circuit breaker not tripped (total P&L > -max_drawdown_pct)
6. Under exposure limit (deployed capital < max_exposure_pct)

### Win/loss tracking
Counts TP exits (wins) vs SL exits (losses). Displayed in TUI with win rate percentage. Useful for evaluating whether the strategy's entries are well-timed.

---

## ML_Headers/RollingStats.hpp

128-tick rolling window that tracks market microstructure.

**Outputs:**
- `price_avg` - rolling mean price
- `price_stddev` - approximated as `(price_max - price_min) / 4` (avoids needing sqrt)
- `price_min / price_max` - range over window
- `volume_avg` - rolling mean volume
- `volume_slope` - `(newest_volume - oldest_volume) / count` (volume trend direction)

**Used for:**
- Buy gate price offset (buy below rolling average)
- Volume filter threshold (require N * rolling average)
- Entry spacing (min distance = stddev * multiplier)
- Volatility-based TP/SL (exit distances scale with stddev)

Same ring buffer pattern as `RegressionFeederX` - branchless power-of-2 wrap with `& (ROLLING_WINDOW - 1)`.

---

## ML_Headers/LinearRegression3X.hpp

Ordinary least squares regression on a ring buffer of 8 samples.

**Input:** Ring buffer of FPN values (P&L snapshots from the controller)
**Output:** slope, intercept, R^2

The slope tells you "is P&L trending up or down?" R^2 tells you "how confident is this trend?" The controller only adjusts when R^2 exceeds the threshold - prevents reacting to noise.

---

## ML_Headers/ROR_regressor.hpp

Rate of return regression - slope of slopes. Takes the output of LinearRegression3X and tracks whether the *slope itself* is accelerating or decelerating. Computed and stored for future use (trend acceleration detection).

---

## DataStream/BinanceCrypto.hpp

Full network stack for connecting to Binance websocket trade stream.

### Six layers (bottom to top)

1. **TCP** (`binance_tcp_connect`) - POSIX `socket()` + `getaddrinfo()` + `connect()`
2. **TLS** (`binance_tls_setup`) - OpenSSL `SSL_CTX_new()` + `SSL_connect()` with SNI
3. **WebSocket** (`binance_ws_handshake`) - HTTP upgrade with random base64 key, verify 101
4. **Frame parser** (`binance_ws_read_frame`) - reads 2-byte header, extended length, payload. Loops on `SSL_read` until complete frame accumulated. Handles masked/unmasked frames.
5. **JSON parser** (`binance_parse_trade`) - scans for `"p":"` and `"q":"` substrings, extracts values. No general parser needed.
6. **FPN conversion** - `FPN_FromString` converts extracted price/qty strings to fixed-point

### Ping/pong
Binance sends pings every ~3 minutes. Handled transparently inside `BinanceStream_ReadTick` - if a ping frame arrives, pong is sent immediately and the next frame is read. The caller never sees ping/pong frames.

### SSL_pending fix
`BinanceStream_Poll` checks `SSL_pending()` before calling `poll()`. OpenSSL buffers decrypted data internally, so `poll()` can report "no data" while SSL has a complete frame ready. By checking SSL first, we avoid this mismatch entirely.

### Burst draining
During volatile markets, multiple frames buffer between poll cycles. The main loop reads ALL buffered frames, running exit gates on each intermediate price. Only the final price is used for BuyGate. `SSL_pending()` gates the drain loop.

### Session lifecycle
Binance disconnects after 24 hours. The engine reconnects proactively at 23h30m with a clean procedure: disable buys -> drain frames -> force-close all positions -> verify bitmap zero -> reconnect -> restart warmup.

---

## DataStream/EngineTUI.hpp

ANSI terminal dashboard. No framework, no curses, just escape codes.

**Display sections:**
- Header: tick count, state, uptime
- Market: current price/volume
- Market structure: rolling averages, stddev, range, volume slope
- Buy gate: price/volume thresholds, adaptive offset/multiplier, distance to trigger, entry spacing
- Positions: all active positions with entry, qty, TP (with distance), SL (with distance), P&L %
- P&L: balance, realized, unrealized, total with return %
- Controls: quit, pause, reload

**Terminal handling:**
- Raw mode for single-char input (no enter needed)
- Signal handler restores terminal on crash (SIGINT, SIGTERM, SIGSEGV)
- Cursor home `\033[H` instead of clear to reduce flicker
- Render interval configurable (default every 10 ticks)

---

## DataStream/TradeLog.hpp

CSV append logger. Writes to `{symbol}_order_history.csv`. Records every buy and sell with: tick, side, price, quantity, entry_price, delta_pct, exit_reason, gate conditions, TP, SL. `fflush` after every write so nothing is lost on crash.

---

## DataStream/MockGenerator.hpp

Deterministic synthetic market data for testing. LCG random number generator produces a price random walk with configurable drift, volatility, and volume spikes. Same seed = same price series every time.

---

## DataStream/FauxFIX.hpp

FIX 4.4 protocol parser. Parses tag=value messages with SOH delimiter. Used by MockGenerator to produce FIX-formatted market data messages for testing. Not used in the live Binance pipeline.

---

## MemHeaders/PoolAllocator.hpp

Bitmap-based order pool. Same `__builtin_ctzll` pattern as Portfolio. `OrderPool_init` allocates N slots, `bitmap` tracks which are occupied. BuyGate writes to free slots, controller consumes filled slots.
