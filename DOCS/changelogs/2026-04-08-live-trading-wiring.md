# 2026-04-08 — v3.9.0-experiment: Sharded Live Trading (Phase 1)

scope: experimental branch `experiment/per-core-sharding`. follow-on to the
cache opt commit at `pre-live-trading-wiring` tag. wires the BinanceOrderAPI
into the sharded controller drainer so `use_real_money=1` actually places
real orders. paper mode (the default) is unchanged.

## Execution Engine

### live trading via use_real_money in sharded mode

`CoreFrameworks/EngineSharded.hpp`:

- includes `DataStream/BinanceOrderAPI.hpp`
- new file-static `g_sharded_order_api` (BinanceOrderAPI instance) and
  `g_sharded_order_lat` (atomic latency stats struct: count, failures,
  total_us, min_us, max_us)
- new helpers `ShardedOrderLatency_Reset()` and `ShardedOrderLatency_Sample()`
- startup block (after the sharded banner): when `cfg.use_real_money != 0`,
  loads secrets via `LoadSecrets("secrets.cfg")` and calls
  `BinanceOrderAPI_Init` against testnet.binance.vision or api.binance.us
  per `bcfg.use_testnet`. mirrors the legacy engine pattern at main.cpp:212.
  prints a 10-second SAFETY warning if running against PRODUCTION (not
  testnet) and aborts cleanly on Ctrl+C during the wait.
- drainer thread no longer calls `EventLoop_DrainEvents` directly. its open
  coded so we can hook the order submission AFTER each event:
  - pop event from per-core ring
  - snapshot order qty BEFORE OnEvent (entry: `cores[slot].intended_qty`,
    exit: `portfolio.positions[slot].quantity` since CloseSlot will clear
    it)
  - call `EventLoop_OnEvent` to update internal state
  - if `live_trading` and event is entry/exit and qty > 0:
    - bracket `BinanceOrderAPI_MarketBuy/Sell` with `steady_clock`
    - sample the elapsed_us into the latency stats
    - on failure: log `ORDER FAIL core=N BUY|SELL qty=X` (TODO reset path)
- TUI render loop got a new ORDER LATENCY line below the per-core latency
  table. shows `orders / fail / min / avg / max` in microseconds in live
  mode, or "paper mode (no orders submitted)" in paper mode.
- final dump on shutdown: same numbers, plus `BinanceOrderAPI_Cleanup`.

### why open-code the drainer instead of adding a hook to ControllerEventLoop

ControllerEventLoop.hpp stays generic — no REST dependencies, no per-asset
knowledge, testable in isolation. the live trading concern is contained
entirely in EngineSharded.hpp where it belongs. the drainer is ~15 LOC of
extra logic on top of the existing EventLoop_DrainEvents pattern.

### qty source rule

- entry order qty: `cores[slot].intended_qty` (matches what OnEvent will
  write into portfolio.positions[slot].quantity via Portfolio_OpenSlot)
- exit order qty: `portfolio.positions[slot].quantity` snapshotted BEFORE
  OnEvent runs, since Portfolio_CloseSlot inside OnEvent clears the slot

### paper mode unchanged (verified)

ran `./build/engine /tmp/cfg_n2.cfg` (use_real_money=0, the default) after
the wiring. per-core latency:

```
core   samples       min        p50        p95        p99        max
  0     718528       57 ns       61 ns      319 ns      422 ns    34439 ns
  1     718528       57 ns       62 ns      321 ns      431 ns    43067 ns
```

identical to the post-cache-opt baseline. zero per-tick overhead from the
live trading wiring because the entire order path is gated on
`live_trading` which is false in paper mode. the compiler hoists the dead
branch out of the inner loop.

## Known limitations (this commit)

- **no rejection reset path.** when `BinanceOrderAPI_MarketBuy` or
  `MarketSell` returns failure, the code logs `ORDER FAIL ...` and
  continues. the executor remains in its optimistic state (active=1 after
  a failed entry, active=0 after a failed exit). the portfolio side is
  also out of sync. this matches the legacy engine's fire-and-forget
  pattern (main.cpp also doesnt reconcile per-event) but means a stuck
  core needs slow-path reconciliation to recover. **for testnet
  experimentation this is fine.** for production a phase 2 commit needs
  to add an explicit reset signal channel from the drainer to the
  executor (e.g. an atomic reset_request flag in ExecutionCore that the
  hot path checks at the top of each tick).
- **fill price reconciliation is missing.** OnEvent uses the executor's
  detected price, not the actual fill price returned by binance. on a
  fast moving market the gap can be a few basis points. legacy engine
  has the same issue. proper fix is to call OnEvent AFTER the order
  returns and use the actual fill data, OR run a slow-path reconcile
  that compares portfolio entry_price against actual fills.
- **partial fills not handled.** if binance returns a partial, the
  executor and portfolio both think the full intended_qty is in the
  position. fix is the same reconciliation pass that handles failures.
- **drainer is single-threaded.** with 4 cores generating events, 4
  simultaneous orders × 200ms each = 800ms drainer cycle worst case.
  fine for low-frequency strategies (a few entries per minute), painful
  for HFT. fix is a thread pool for order submission. document for now.

## How to test on binance testnet

1. ensure `secrets.cfg` has your testnet API key + secret
   (separate from production credentials — testnet has its own key set
   from https://testnet.binance.vision/)
2. in your sharded cfg file:
   ```ini
   engine_mode=sharded
   num_execution_cores=2
   sharded_force_synthetic=0
   use_real_money=1
   use_testnet=1
   default_strategy=2          # SimpleDip — only ported strategy
   ```
3. run `./build/engine your_cfg.cfg`
4. watch the live TUI:
   - PER-CORE LATENCY should show ~57 ns min / ~61 ns p50 (unchanged)
   - ORDER LATENCY should populate as soon as the first entry fires —
     expect 50-200 ms range against testnet.binance.vision
5. on Ctrl+C: per-core latency dump + order latency dump

PRODUCTION (use_testnet=0): the engine prints "real money. starting in
10 seconds..." and waits 10 seconds before connecting, so you can Ctrl+C
out if you set the wrong flag.

## Files changed

- `CoreFrameworks/EngineSharded.hpp` — live trading wiring + order
  latency stats + TUI display + final dump + cleanup
- `DOCS/changelogs/2026-04-08-live-trading-wiring.md` — this file

## Next concrete steps

1. **rejection reset path** — atomic reset_request flag in ExecutionCore,
   drainer sets it on order failure, executor hot path checks it at top
   of tick and clears its own active state. ~30 LOC.
2. **fill price reconciliation** — OnEvent runs AFTER the order returns
   so the actual fill price is recorded. needs the executor's tick price
   passed through unchanged for the slow-path RollingStats but the
   PORTFOLIO entry_price comes from the order ack. ~20 LOC of refactor.
3. **strategy parameter ports** — MR / Momentum / EmaCross stubs in
   StrategyParameters.hpp. each ~50 LOC following the SimpleDip pattern.
4. **snapshot v11** — per-core state persistence so sharded mode survives
   restarts.
5. **soak plan** — testnet 1 hour → testnet 24h → testnet 1 week → live
   small. capture order latency stats at each stage.
