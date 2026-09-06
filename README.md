# FoxML Trader — Per-Node Sharded Engine

**release v0.3** · pre-1.0 (live-trading hardening + headless decoupling are the road to 1.0)

per-node risk-sharded crypto trading engine in C++17. one position per pinned CPU, branchless fixed-point math, lock-free SPSC queues, seqlock-cached parameters. the executor cores do a fixed, tiny amount of work per tick — the slow path can do whatever it likes (ML inference, regression, regime detection) without the executors ever paying for it.

![per-core latency panel](assets/per-core-latency.png)

built from scratch, self-taught. reusable primitives were extracted into a separate C++20 header-only library (FoxLIB).

> **paper trading by default.** live trading via Binance REST API is implemented but has **not** been run with real capital — treat it as untested on that axis. set `trading_mode=live` and add API keys to `secrets.cfg`. no API key needed for market data feeds.

[![Donate](https://img.shields.io/badge/Donate-PayPal-blue.svg)](https://www.paypal.com/ncp/payment/8M6XLK7M8569C) [![Discord](https://img.shields.io/badge/Discord-Community-5865F2.svg)](https://discord.gg/asSDcYwPz)

---

## size

the size that matters is the hot path, not the total. code lines below exclude blanks and comment-only lines:

| component | code LOC |
|---|---:|
| **hot path** (executor per-tick math: ExecutionCore + OrderGates + ParameterSlot) | **406** |
| engine (CoreFrameworks + Strategies + ML_Headers + DataStream + FixedPoint + MemHeaders) | 37,692 |
| backtest + training pipeline | 8,759 |
| GUI (Dear ImGui panels) | 6,428 |
| tests | 29,086 |
| **total** | **~82,000** |

the hot path is well under 1% of the project. the rest is supporting infrastructure — training pipeline (so models get the right features), parity tests (so train and serve produce bytewise-identical output), backtest harness (so strategies validate before paper), GUI (so the operator sees what's happening), invariant tests (so future changes don't silently break load-bearing rules). all in service of keeping the executor tiny and trustworthy.

---

## measurement

per-call `rdtsc` has structural overhead — on a hot path this fast, the bracket costs more than the thing being measured. on this CPU the bracket is **~25-30 ns**, so a per-tick number that doesn't subtract it is wrong by a wide margin.

the batch-floor bench brackets `rdtsc` **once** around N=1M iterations of `ExecutionCore_Tick` and divides, which amortizes the tax to ~0:

| variant | ns/tick |
|---|---:|
| full 192B parameter memcpy every tick | 13.35 |
| cached (skip memcpy on seq match) | 13.08 |
| **floor (gates + permission load only)** | **11.56** |
| **abs floor (1 cmp + 1 atomic load)** | **2.94** |

**these figures were measured on v5.11.x (May 2026)** on consumer hardware with the GUI running, no `isolcpus`, no `chrt`. the same run put hot-path p50 at 30-40 ns after subtracting the bracket, and p99 in the hundreds of ns. **the current tree has not been re-benched** — the numbers above are a historical measurement, not a claim about HEAD. the design budget the code is held to is p99 ≤ 500 ns hot path and ≤ 100 µs slow path; a regression past those is treated as a ship blocker.

for scale, on the same class of hardware: L3 hit ~10-15 ns, `mfence` 30-50 ns, cross-core cache line bounce 50-100 ns, `getpid()` 50-100 ns, local DRAM 80-100 ns, TCP loopback round trip ~10-20 µs, `recvmsg()` through the kernel network stack 1-5 µs. tail variance here comes from kernel preemption; pinning with `chrt -f 90 taskset -c 4-7` plus `isolcpus` would flatten it.

---

## what it looks like running

![full GUI dashboard](assets/gui-dashboard.png)

> 4 nodes running different strategies (ML / DIP / AUTO / EMA), regime classifier active, partial exits paired across slots `#3.A` and `#3.B`, per-node latency panel, ML Ensemble panel showing per-horizon bandit weights per regime, account + risk panels with kill switch armed per node. all from a single tick stream fanned across SPSC rings.

---

## architecture

```
HOT PATH (every tick, per node, pinned to one CPU):
  ExecutionCore_Tick
    ↓ acquire-load: ParameterSlot.seq
    ↓ if cached_seq matches → skip the 192B parameter memcpy
    ↓ branchless BG/SG evaluation (fixed-point comparisons)
    ↓ on entry/exit: push TradeEvent → SPSC ring

SLOW PATH (per-node pthread, every poll_interval ticks):
  ↓ RollingStats (least-squares regression, R², variance)
  ↓ RegimeSignals (features → score-based classifier)
  ↓ ML inference (XGBoost single-row predict)
  ↓ Strategy_BuildParameters → GateParameters
  ↓ ParameterSlot_Write (seqlock, wait-free producer)

PRODUCER (single thread):
  ↓ tick read from Binance WS (or backtest replay)
  ↓ fan out across N per-node SPSC rings
  ↓ ema_price replication into each node's slow_state
  ↓ GUI snapshot publish

COMPOSER / DRAINER (single thread):
  ↓ pops trade events from every node's SPSC ring
  ↓ drains per-node OMS submit queues (sole OMS_Submit caller)
  ↓ routes through the Order Management System
  ↓ portfolio mutation via the extracted fill handler

GUI (Dear ImGui, SDL2/OpenGL3):
  double-buffered snapshot from the producer thread
  per-node buy gate overlays, ML Ensemble panel, settings hot-swap
```

**the hot path is immune to model complexity.** XGBoost, LightGBM, transformer, no model at all — the executor nodes see only the resulting `GateParameters` struct. swap the model and the per-tick cost is unchanged.

money math is decimal fixed-point (`Money` = `FixedPoint<10,8>`, exact at the venue's 8dp) on every price, quantity, fee and balance; feature math is binary fixed-point (`FPN_Binary<64>`). no floats on either path.

---

## the seqlock story

original design: triple buffer between the slow-path producer and the per-node executors. wait-free, lock-free, three slots, atomic index swap. textbook.

the stress test produced torn reads at high producer rate. switched to a [seqlock](https://en.wikipedia.org/wiki/Seqlock) — the same pattern the Linux kernel uses for `seqcount_t`. wait-free producer increments the seq counter, writes the payload, increments again. lock-free consumer reads seq, payload, re-reads seq, retries on mismatch.

the parameter snapshot is then cached in the executor itself — every tick does one acquire-load of `seq`, compares against `cached_seq`, and skips the memcpy on a match.

trust the stress test over the plan. the plan said triple buffer, the test said torn reads, the test won. full reasoning in `CoreFrameworks/ParameterSlot.hpp`.

---

## ML pipeline

nodes running `node_N_strategy=ml` load XGBoost models via `node_N_model_dir=<base_path>`. the engine auto-detects multi-horizon siblings (`<base>_horizon_<N>/`) and runs a Bandit-Exp3 weighted blend per regime, with runtime IC drift detection that demotes degraded horizons.

**training:** the `foxml_suite` GUI runs walk-forward CV, held-out validation and auto-stamping. label kinds: binary buy_signal, 3-class barrier (PEAK/VALLEY/STABLE), regression. multi-horizon training writes per-horizon model files.

**ML never runs on the hot path.** the model produces gate parameters at slow-path cadence; the executor consumes them. swap the model class entirely and the hot-path cost is unchanged.

train-serve parity is locked by:
- `FEATURE_REGISTRY_HASH` (FOREACH_FEATURE X-macro fingerprint)
- `LABEL_REGISTRY_HASH` (FOREACH_TARGET X-macro fingerprint)
- `scaler_sha256` (FeatureStandardizer sidecar binding)
- an HMAC-signed stamp body — cross-build / cross-cfg / cross-feature drift is refused at engine load

strategy code is role-agnostic: adding a new model role doesn't change the strategy at all.

---

## build

```bash
./build.sh test     # ANSI TUI engine + controller_test
./build.sh gui      # ImGui GUI (engine_gui + foxml_suite)
./build.sh suite    # GUI + XGBoost training (requires the xgboost C lib)
./build.sh asan     # AddressSanitizer build
./build.sh ubsan    # UndefinedBehaviorSanitizer build
./build.sh tsan     # ThreadSanitizer build
./build.sh all      # everything
```

requires g++ (C++17), OpenSSL and CMake 3.14+. the GUI adds SDL2 + OpenGL3. ML adds the XGBoost C library (`-DUSE_XGBOOST=ON`, built from source; off by default).

## run

```bash
./run.sh                              # build if needed, then run the engine (terminal TUI)
./build.sh gui && ./bin/engine_gui    # graphical dashboard
```

---

## config

```ini
# engine.cfg — a few of the keys; see the shipped engine.cfg for the full annotated set
symbol = btcusdt
trading_mode = paper             # 'paper' (default, safe) or 'live' (real orders via REST)
num_execution_nodes = 4          # 1-16

starting_balance = 10000.00
fee_rate = 0.10                  # % per side
risk_pct = 5.00                  # % of balance per position

# per-node strategy + ML model
node_0_strategy = ml
node_0_model_dir = models/classification/my_run
node_0_risk_pct = 25.00          # per-node override of any global knob

# depth feed (top-of-book imbalance into the gates + ML features)
depth_enabled = 1
record_depth = 1

# train-serve parity gate
held_out_gate_strict = 0         # 0 = warn only, 1 = refuse on stamp failure
held_out_stamp_secret =          # HMAC secret (empty = devmode)
gap_acceptable_threshold = 0.05  # WF/held-out gap that fails the stamp
```

press `r` in the TUI to hot-reload the config. per-node strategy and risk can also be changed at runtime from the GUI's Settings panel.

---

## tests

`./build.sh test` builds `controller_test`, which currently runs **4446 assertions** across the engine, ML pipeline, OMS, reconcile and train-serve parity. `depth_recorder_test` covers the depth CSV + gap-marker contract. the suite includes snapshot parity tests and a replay-determinism baseline; the seqlock test catches torn reads at high producer rate, which is how the original triple-buffer plan got rejected.

sanitizer lanes (`asan`, `ubsan`, `tsan`) are part of the pre-ship gate, not an afterthought.

---

## order management system

| phase | what |
|---|---|
| 01 | order state machine (PENDING → SUBMITTED → FILLED/REJECTED) |
| 02 | async REST submission via the Binance adapter (the drainer never blocks) |
| 03 | order event log + portfolio fold (deterministic replay) |
| 04 | user-data websocket for real-time fills |
| 05 | reconciliation poller (balance verification against the venue) |
| 06 | idempotency keys, error codes, rate limits, listen-key hardening |
| 07 | disk persistence (binary event log, survives restarts) |
| 08 | per-node strategy config |

three concurrent SPSC rings feed the OMS drainer: REST results, WebSocket fills, and reconciliation corrections. each has exactly one producer and one consumer; the drainer drains all three in `OrderManager_Tick`.

---

## documentation

the source is documented inline with a structured tag-block schema. detailed operator docs — configuration reference, deployment and kernel-tuning runbook, training pipeline, architecture invariants, sprint changelogs — are operator-private: they capture edge-case design history that isn't useful without the surrounding context.

---

## current state

- ✅ engine architecture, hot path, slow path, OMS, ML pipeline and GUI are stable
- ✅ multi-horizon ensemble training + live deployment work end to end
- ✅ train-serve parity infrastructure complete
- ✅ paper trading runs against the live Binance feed
- 🚧 live capital deployment — not done; gated on the remaining live-readiness work (disconnect-flatten policy, latency staleness gate, a full soak)
- 🚧 testnet soak — pending before any mainnet use

this is an actively developed personal project, not a product. there is no support commitment and no uptime claim.

---

## license

see [LICENSE](LICENSE). commercial use or licensing enquiries: [jenn.lewis5789@gmail.com](mailto:jenn.lewis5789@gmail.com).

**copyright (c) 2026 Jennifer Lewis. all rights reserved.**

---

<a href="https://www.paypal.com/ncp/payment/8M6XLK7M8569C">
  <img src="assets/donate-qr.png" alt="Support development" width="150">
</a>
