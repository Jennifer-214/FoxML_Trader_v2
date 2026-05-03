# Known Issues

Discovered during 2026-03-27 optimization audit. Ordered by severity.

## Cache Alignment (fixing now)

- **ExitBuffer.count at end of struct** — offset 768, 12 cache lines from records[0]. Hot path reads count first then writes to records[count]. Dependent cache miss every exit check.
- **Portfolio.active_bitmap at end** — offset 2304, after all 16 positions. ExitGate reads bitmap first then chases into positions.
- **DataStream.is_buyer_maker straddles cache line** — offset 64, crosses boundary. Hot fields (price, volume) in CL 0 but is_buyer_maker spills to CL 1.
- **RollingStats.side_buf far from head** — side_buf at end of struct, ~4 cache lines from head/count. Push reads both on every slow-path cycle.

## FPN Saturation (weeks-scale risk)

**Location:** `PortfolioController.hpp` lines 53-62

These FPN accumulators grow monotonically without reset:
- `realized_pnl` — cumulative P&L across all exits
- `total_fees` — cumulative fees paid
- `gross_wins` — cumulative TP gains
- `gross_losses` — cumulative SL losses

FPN_AddSat saturates to MAX on overflow. Over weeks of continuous trading (50k+ trades), these will silently hit the ceiling and all subsequent additions are clamped. TUI balance/P&L display goes garbage.

**Mitigation:** The 24h session lifecycle closes all positions and reconnects, but does NOT reset these accumulators. They persist across sessions via snapshot.

**Fix:** Add a session-boundary reset that zeros these accumulators and logs the final values to CSV before resetting. Or switch to double-precision for display-only accumulators since they don't participate in hot-path math.

## Config Hot-Reload Race Condition (low practical risk)

**Location:** `PortfolioController.hpp` lines 781-844

`PortfolioController_HotReload` copies 45+ config fields one by one. If the engine tick reads config mid-copy, it sees a mix of old and new values (e.g., new fee_rate with old take_profit_pct).

**Current mitigation:** Reload only happens on TUI keypress ('r'), ticks are ms apart. One mixed-config tick won't cause catastrophic behavior. The risk is theoretical at current single-threaded architecture.

**Fix if needed:** Double-buffer pattern — write new config to shadow copy, then swap a single pointer atomically. Only matters if the hot path ever goes multi-threaded.
