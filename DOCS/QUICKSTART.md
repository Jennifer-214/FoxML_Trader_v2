# Operator Quickstart

This doc gets a fresh operator from `git clone` to a running paper-trading
engine in under 30 minutes. For production deployment with isolcpus +
SCHED_FIFO + IRQ affinity tuning, see [`OPERATOR_DEPLOYMENT.md`](OPERATOR_DEPLOYMENT.md).
For ML training + backtesting workflow, see [`ML_USAGE.md`](ML_USAGE.md) +
[`ML_TRAINING.md`](ML_TRAINING.md).

---

## What this is

`FoxML_Trader_v2` is a per-node sharded crypto trading engine in C++20.
Each execution core runs a self-contained strategy unit (slow + hot pthread
pair); a producer thread fans real Binance ticks across SPSC rings. Hot
path is branchless / lock-free with a 40-400ns p99 budget.

Default deployment is **paper trading**. Live trading requires explicit
config + secrets and is gated on multiple invariants (kill switch,
max-drawdown, max-exposure).

---

## Prerequisites

- Linux (kernel ≥ 5.10). The engine uses Linux-specific syscalls
  (`mmap(MAP_POPULATE)`, `mlockall`, `pthread_setaffinity_np`).
- GCC 11+ (C++17/20 support; v5.11.7 uses AVX-512 intrinsics on capable
  CPUs — gracefully degrades on non-AVX-512 hardware).
- CMake 3.14+.
- OpenSSL (Binance WS uses TLS).
- Optional for the GUI build: SDL2, OpenGL3 (Dear ImGui + implot vendored).
- Optional for ML: XGBoost C library (built from source — see
  [`ML_TRAINING.md`](ML_TRAINING.md) for the build recipe).

Quick install on Arch / Debian:

```bash
# Arch
sudo pacman -S gcc cmake openssl sdl2

# Debian / Ubuntu
sudo apt install build-essential cmake libssl-dev libsdl2-dev
```

---

## Build

The repo ships a `build.sh` wrapper that handles cmake invocation +
flags. Targets:

```bash
./build.sh test          # ANSI engine + controller_test (zero deps beyond OpenSSL)
./build.sh gui           # engine_gui + foxml_suite (SDL2 + OpenGL3 + ImGui + implot)
./build.sh suite         # foxml_suite alone (backtest + ML training workstation)
./build.sh tsan          # ThreadSanitizer build (debug)
./build.sh asan          # AddressSanitizer build (debug)
./build.sh all           # everything
./build.sh clean         # nuke build dirs
```

The wrapper symlinks `engine.cfg` into each build dir + creates `bin/`
shortcuts. After a successful build:

```bash
ls bin/
# controller_test → ../build/controller_test
# engine_gui      → ../build_gui/engine_gui
# foxml_suite     → ../build_gui/foxml_suite
```

If you want raw cmake, see the comments in `CMakeLists.txt`. The
`build.sh` wrapper just invokes cmake with the right per-target flags.

---

## First paper run

```bash
# 1. Build
./build.sh gui

# 2. Edit engine.cfg (the shipped template is sane defaults; minimum
#    inspection: starting_balance, symbol, num_execution_cores)
# Optional but recommended for laptop dev:
#   require_mlockall=0    (skip the FATAL when RLIMIT_MEMLOCK is tight)
#   core_0_strategy=auto
#   core_1_strategy=auto
#   core_2_strategy=auto
#   core_3_strategy=auto

# 3. Run
./bin/engine_gui
```

The GUI opens. Out of the box you'll see:

- **Header** — engine version, build registry hash, cfg path
- **Top Bar** — KB/s + RAM + GPU + perf indicators
- **Market** — live BTCUSDT price + tick rate
- **Account** — paper balance + per-node allocation table
- **Risk** — per-node kill switch state (peak / current / drawdown%)
- **Buy Gate** — per-node gate price + threshold + status
- **Position** — empty until first fill
- **Stats / Latency / Trade History / Engine Log / Engine Topology /
  Per-Node Latency / Strategy Quality** — diagnostic tabs

**The engine starts in WARMUP.** Per-node gates show `WARMUP` until
`warmup_ticks` raw ticks accumulate AND `min_warmup_samples` rolling
samples fill (default both 128; ~10-30 minutes at typical BTC tick
rates depending on volatility / time of day).

After warmup, gates flip to active. The first fill creates a position
(visible in Position panel + chart marker). Press `q` or close the
window to shut down — snapshot saves to `data/sharded_snapshot.dat`.

---

## Reading the GUI

### Per-node summary table (Buy Gate panel)

| Column | Meaning |
|---|---|
| `Strat` | Configured strategy (DIP / MOM / MR / EMA / ML / AUTO) |
| `Gate` | Buy-gate price threshold |
| `Dist` | Distance from market price to gate (positive = price above gate) |
| `Status` | `WARMUP` / `KILL` / `AUTO RES` / `blocked: <reason>` / `ok` |

### Status decoder

- **`WARMUP`** — engine collecting initial samples. Resolves automatically
  when `warmup_ticks` + `min_warmup_samples` complete.
- **`KILL`** — per-node kill switch tripped (drawdown exceeded
  `kill_switch_drawdown_pct`). Reset via Risk panel → Reset button.
- **`AUTO RES`** — core configured as AUTO but regime classifier hasn't
  resolved a concrete strategy. Resolves on first slow-path cycle.
- **`blocked: <code>`** — strategy specifically blocked entry. Common
  codes: `spacing` (entry too close to existing), `vwap` (price above VWAP),
  `min-stddev` (volatility too low), `cost-gate` (TP would not cover fees).
- **`off: <code>`** — controller blocked entry pre-strategy. Examples:
  `core-budget`, `core-kill`, `imbalance` (book imbalance gate).

### Latency tab

Shows hot-path + slow-path p50/p95/p99/max per engine. Hot p99 should
land around 200-1000ns on a tuned host. Slow-path p99 in the ~1ms range
on a non-isolated host is OS preemption (scheduler), not engine code.

### Engine Topology tab

Shows per-thread CPU pinning + scheduling state. Useful for verifying
your `slow_path_pin_offset` and `num_execution_cores` cfg matches
deployment expectations.

---

## Tuning

Hot-reloadable cfg fields (press `r` in TUI mode, or restart for fields
marked startup-only):

| Field | Purpose | Hot-reload |
|---|---|---|
| `entry_offset_pct` | Buy gate offset below rolling mean | yes |
| `volume_multiplier` | Tick volume / rolling avg threshold | yes |
| `take_profit_pct` / `stop_loss_pct` | Exit thresholds at fill time | yes |
| `spacing_multiplier` | Min distance between entries (× stddev) | yes |
| `risk_pct` | % of balance per position | yes |
| `max_drawdown_pct` / `max_exposure_pct` | Risk caps | yes |
| `core_N_strategy` | Per-node strategy override | startup-only |
| `num_execution_cores` | How many sharded cores | startup-only |

Full reference: [`CONFIGURATION.md`](CONFIGURATION.md).

---

## Adding a strategy

The codebase uses an X-macro registry to avoid silently-orphaned dispatch
sites. Adding a new strategy = appending one row to `FOREACH_STRATEGY(X)`
in `Strategies/StrategyInterface.hpp` + implementing the 5 lifecycle
functions:

1. `_Init` — per-node state allocation
2. `_BuildParameters` — gate parameter emit (hot-path contract)
3. `_Adapt` — per-cadence feedback adjustment
4. `_ExitAdjustSharded` — trailing TP/SL on existing positions
5. `RegimeAdjust` — on-transition retune

See [`STRATEGY_INTERFACE.md`](STRATEGY_INTERFACE.md) for the full spec.
Existing implementations (`Strategies/MeanReversion.hpp`,
`Strategies/Momentum.hpp`, `Strategies/SimpleDip.hpp`,
`Strategies/EmaCross.hpp`, `Strategies/MLStrategy.hpp`) are the canonical
references.

---

## Backtest workflow

`foxml_suite` is the offline analysis + ML training workstation:

```bash
./bin/foxml_suite
```

Tabs:
- **Run Control** — pick a date range + strategy, kick off a backtest.
  Tick CSVs go in `data/<SYMBOL>/YYYY-MM-DD.csv`. Use
  `scripts/download_data.sh BTCUSDT 2024-05-07 2026-05-06` to fetch.
- **Training** — XGBoost / LightGBM training over a backtest's labeled
  features. Stamps the resulting model with feature registry hash +
  scaler SHA + cfg fingerprint.
- **Walk-Forward** — rolling-origin out-of-sample validation.
- **Held-Out** — final unseen test slice for model acceptance.

Models live in `models/<role>/`. The engine loads them via
`core_N_model_dir=` cfg.

Full ML pipeline doc: [`ML_USAGE.md`](ML_USAGE.md) +
[`ML_TRAINING.md`](ML_TRAINING.md).

---

## Going live (when you're ready)

**Don't until you've paper-tested for ≥30 days at the cfg you intend
to deploy.**

The minimum live setup:

```cfg
use_real_money=1
acknowledge_hardcoded_strategy_in_live=1   # if any core_N_strategy != auto
require_mlockall=1                          # HFT-correct (deployment box)
```

Plus `secrets.cfg`:

```cfg
binance_api_key=<your_key>
binance_secret_key=<your_secret>
```

The engine refuses to boot in live mode without:
- A valid API key with TRADE permission
- Either AUTO strategies on every core OR
  `acknowledge_hardcoded_strategy_in_live=1`
- (When `require_mlockall=1`) successful `mlockall(MCL_CURRENT|MCL_FUTURE)`

Production deployment runbook (isolcpus, SCHED_FIFO, IRQ affinity,
hugepages, intel_pstate, governor, NUMA pinning):
[`OPERATOR_DEPLOYMENT.md`](OPERATOR_DEPLOYMENT.md).

---

## Troubleshooting

### Engine exits silently after cfg parse

Check `logging/engine.log`. Common cause: `mlockall` FATAL when
`RLIMIT_MEMLOCK` is tight (laptop default is 8 MB; engine wants 256 MB).

Fix: either raise the limit (`ulimit -l unlimited` for the shell, or
edit `/etc/security/limits.conf` for persistent), or set
`require_mlockall=0` in `engine.cfg` (laptop-dev only — accepts
page-fault tail latency).

### Per-node gates stuck at WARMUP forever

The warmup gate is `warmup_ticks` raw ticks AND `min_warmup_samples`
slow-path cycles. With default 128 + 128 and a quiet market, that's
~71 minutes. Either wait, or temporarily lower both to a smaller value
in dev cfgs.

### "core_N_strategy missing" WARN at boot

Add `core_0_strategy=auto` (and 1, 2, 3) to `engine.cfg`. Without
explicit assignment, all cores default to SIMPLE_DIP. Documented in
the cfg comments.

### Engine boots but never trades

In order: check Risk panel for KILL state; check Buy Gate panel
status column for `blocked: <reason>` or `off: <reason>`; check
warmup state in the latency tab; check Strategy Quality panel for
sample counts.

### GUI font too dense on 1080p

Settings panel → font scale slider (range 0.7-1.5×). Session-only;
resets on relaunch.

### Tests pass but engine still segfaults

Sanitizer build to catch what the optimized build hides:

```bash
./build.sh asan
./build_asan/controller_test
```

ASAN catches use-after-free / out-of-bounds / leaks the optimized build
silently rolls past.

### Backfilling tick data is slow

`scripts/download_data.sh` fetches Binance's public aggTrades CSVs.
Each day is 50-150 MB; 2 years ≈ 60-80 GB. Run overnight; it skips
existing files on re-run.

---

## Where to read next

- Architecture overview: [`ARCHITECTURE.md`](ARCHITECTURE.md)
- Code map (every `Pattern_FunctionName` with file:line):
  [`CODE_MAP.md`](CODE_MAP.md)
- Cfg field reference: [`CONFIGURATION.md`](CONFIGURATION.md)
- Hot-path discipline: [`HOT_PATH_CHANGELOG.md`](HOT_PATH_CHANGELOG.md) +
  the private `latency-path-discipline.md` (in operator's workspace).
- Train↔serve parity surfaces: [`PARITY_LIFECYCLE.md`](PARITY_LIFECYCLE.md)
- Known issues + workarounds: [`KNOWN_ISSUES.md`](KNOWN_ISSUES.md)
