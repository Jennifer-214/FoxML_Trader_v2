# Tick Trader — Per-Core Sharded Engine

**Copyright (c) 2026 Jennifer Lewis. All rights reserved.**

This software is dual-licensed: [AGPL-3.0-or-later](LICENSE) or Commercial. If you use this software without complying with the AGPL (including the requirement to publish your source code for any network-accessible deployment) and without a commercial license, you are infringing copyright.

**Personal use, learning, and paper trading are welcome and encouraged.** Commercial use or deployment for profit requires a commercial license — contact [jenn.lewis5789@gmail.com](mailto:jenn.lewis5789@gmail.com).

**Unauthorized Use — Settlement Terms:** 100% of gross revenue from date of first unauthorized use (up to 10-15 years). Full statutory damages under 17 U.S.C. § 504 (up to $150,000 per work for willful infringement). **Bounty:** 50% of total settlement for reports leading to successful enforcement. See [BOUNTY.md](BOUNTY.md). Contact: [jenn.lewis5789@gmail.com](mailto:jenn.lewis5789@gmail.com)

---

per-core risk-sharded crypto trading engine in C++17. each execution core runs its own strategy on its own pinned CPU at ~57-120ns per tick. branchless fixed-point arithmetic, bitmap portfolio management, full order management system with real-time websocket fills.

built from scratch, self-taught, ~60k lines across engine + backtest suite + ML pipeline.

> **paper trading by default.** live trading via Binance REST API is supported. set `use_real_money=1` in engine.cfg and add API keys to `secrets.cfg`. no API key needed for market data — the public websocket is always used for price feeds.

> **WARNING: live trading is experimental.** use at your own risk. this software is provided as-is with no warranty.

[![Donate](https://img.shields.io/badge/Donate-PayPal-blue.svg)](https://www.paypal.com/ncp/payment/8M6XLK7M8569C) [![Discord](https://img.shields.io/badge/Discord-Community-5865F2.svg)](https://discord.gg/asSDcYwPz)

## architecture

```
EXECUTION CORES (one per pinned CPU, ~57-120ns/tick):
  each core runs one strategy independently
  branchless gate evaluation → trade event → SPSC ring

CONTROLLER (drainer thread):
  pops trade events from all cores
  routes through Order Management System
  portfolio mutation via extracted fill handler

ORDER MANAGEMENT SYSTEM:
  3 SPSC rings: REST fills, WebSocket fills, reconciliation
  async submission via BinanceAdapter worker thread
  idempotency keys, error-aware retry, rate limit tracking
  event log with disk persistence + deterministic fold

GUI (ImGui, SDL2/OpenGL3):
  double-buffered TUISnapshot from producer thread
  per-core buy gate overlays on price chart
  per-strategy settings panel with hot-swap
  paper reset button
```

## per-core strategies

each execution core can run a different strategy with independent tuning:

```
core_0_strategy = simple_dip
core_0_risk_pct = 20.0

core_1_strategy = momentum
core_1_risk_pct = 5.0

core_2_strategy = ml
core_2_model_path = models/trend_model.xgb
core_2_risk_pct = 10.0

core_3_strategy = ema_cross
```

available strategies: `simple_dip`, `momentum`, `mean_reversion`, `ema_cross`, `ml`, `none`

per-strategy TP/SL overrides so different strategies get different tuning:
```
simpledip_tp_pct = 0.15
simpledip_sl_pct = 0.10
momentum_tp_mult = 3.0      # stddev multipliers, not percentage
momentum_sl_mult = 1.5
emacross_tp_pct = 0.20
```

## ML inference

cores running `strategy = ml` load an XGBoost or LightGBM model and run single-row inference on every slow-path cycle (~1-5us). 16 features packed from rolling stats (slope, R², variance, volume delta, VWAP deviation, etc.). train models in the foxml_suite backtest GUI, export to `.xgb`, point config at them.

each core can load a different model — run an aggressive model on one core and a conservative model on another, each with its own risk allocation.

## order management system (OMS)

8 phases, all shipped:

| phase | what |
|-------|------|
| 01 | order state machine (PENDING → SUBMITTED → FILLED/REJECTED) |
| 02 | async REST submission via BinanceAdapter (drainer never blocks) |
| 03 | order event log + portfolio fold (deterministic replay) |
| 04 | user data websocket (real-time fills, 10-50ms instead of REST 50-200ms) |
| 05 | reconciliation poller (self-healing balance verification) |
| 06 | idempotency keys, error codes, rate limits, listen key hardening |
| 07 | disk persistence (binary event log, survives restarts) |
| 08 | per-core strategy config |

## build

```bash
# ANSI TUI (zero deps beyond OpenSSL)
cmake -B build -DUSE_NATIVE_128=ON && cmake --build build

# ImGui GUI (SDL2 + OpenGL3)
cmake -B build_gui -DUSE_IMGUI_GUI=ON -DUSE_NATIVE_128=ON && cmake --build build_gui

# with ML model support
cmake -B build_gui -DUSE_IMGUI_GUI=ON -DUSE_NATIVE_128=ON -DUSE_XGBOOST=ON && cmake --build build_gui

# run
cd build_gui && ./engine_gui
```

requires: g++ (C++17), OpenSSL, CMake 3.14+. GUI adds SDL2 + OpenGL3. ML adds XGBoost C library.

## config

set `engine_mode = sharded` in engine.cfg. key settings:

```
engine_mode = sharded
num_execution_cores = 4
sharded_force_synthetic = 0    # 1 = offline testing with synthetic ticks
use_real_money = 0             # paper trading (default)

starting_balance = 10000.00
fee_rate = 0.10
risk_pct = 15.00               # default per-core risk (override with core_N_risk_pct)

take_profit_pct = 0.15         # shared TP (override per-strategy)
stop_loss_pct = 0.10           # shared SL (override per-strategy)
```

hot-reloadable with `R` in the GUI. per-core strategy and risk can be changed at runtime via the settings panel.

## tests

```bash
./build/controller_test                                              # 279 assertions
./experiments/per_core_sharding/build/test_oms                       # 9 OMS state machine tests
./experiments/per_core_sharding/build/test_oms_concurrent            # 4 TSan-validated stress tests
./experiments/per_core_sharding/build/test_order_event_log           # 8 event log fold tests
./experiments/per_core_sharding/build/test_event_log_head_to_head    # 27 mode 0 vs mode 1 assertions
./experiments/per_core_sharding/build/test_oms_phase04_06            # 31 WS fill + reconcile tests
```

## license

AGPL-3.0-or-later or Commercial. See [BOUNTY.md](BOUNTY.md) for enforcement terms.

---

<a href="https://www.paypal.com/ncp/payment/8M6XLK7M8569C">
  <img src="assets/donate-qr.png" alt="Support development" width="150">
</a>
