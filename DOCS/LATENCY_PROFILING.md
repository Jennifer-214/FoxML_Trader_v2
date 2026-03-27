# Latency Profiling

## Build modes

```bash
# production — no measurement, TUI enabled, zero profiling overhead
g++ -std=c++17 -O2 -lssl -lcrypto -o engine main.cpp

# profiling — measurement enabled, TUI enabled (shows LATENCY section)
# NOTE: TUI rendering pollutes L1 cache between ticks, inflating hot-path avg
g++ -std=c++17 -O2 -lssl -lcrypto -DLATENCY_PROFILING -o engine_prof main.cpp

# bench — measurement enabled, TUI DISABLED, stats dumped to stderr every 10k ticks
# this is the clean measurement: no TUI cache pollution, no render overhead
# use this mode to get accurate hot-path numbers
g++ -std=c++17 -O2 -lssl -lcrypto -DLATENCY_PROFILING -DLATENCY_BENCH -o engine_bench main.cpp
```

For cleanest results, pin to a core:
```bash
taskset -c 0 ./engine_bench
```

Without these flags, all profiling code is compiled out — zero cost in production builds.

## What it measures

**Hot path** (every tick):
- `BuyGate()` — branchless price + volume check
- `PositionExitGate()` — branchless bitmap walk, TP/SL per position
- `PortfolioController_Tick()` fast portion — fill consumption, spacing, sizing, risk checks

**Slow path** (every `poll_interval` ticks, default 100):
- Exit buffer drain + trade logging
- Rolling stats push (128-tick + 512-tick windows)
- P&L computation
- Strategy adaptation (idle squeeze, regression, filter adjustment)
- Buy signal computation (offset mode selection, regression shift, multi-timeframe gate)

The two paths are separated by checking `tick_count` after the tick — if `tick_count == 0`,
the slow path just ran and the measurement includes both.

## TUI output

A `LATENCY` section appears at the bottom of the TUI:

```
LATENCY (profiling enabled):
    hot path:  avg 58ns  min 42ns  max 127ns  (10000 ticks)
    slow path: avg 4.2us min 3.8us max 8.1us  (100 cycles)
```

Stats are cumulative from engine start. Min shows best-case (warm cache, no fills).
Max shows worst-case (cache misses, multiple fills consumed, context switches).

## How it works

Uses `__rdtscp()` (x86 intrinsic) which reads the CPU timestamp counter in ~1 cycle.
Unlike `clock_gettime()` (~20-30ns syscall overhead), RDTSCP adds negligible measurement
noise to a ~60ns hot path.

TSC is calibrated at startup via a 10ms `usleep()` to determine cycles-per-nanosecond.
Requires `constant_tsc` CPU flag (standard on all modern Intel/AMD — check with
`grep constant_tsc /proc/cpuinfo`).

## Measurement overhead

Two `__rdtscp()` calls per tick add ~2-4ns. On a ~60ns hot path, that's ~5% overhead.
Acceptable for profiling but not for production — hence the compile flag.

## Interpreting results

| Metric | Good | Investigate |
|--------|------|-------------|
| Hot path avg | < 100ns | > 200ns |
| Hot path max | < 500ns | > 1μs (likely context switch or cache miss) |
| Slow path avg | < 20μs | > 50μs |
| Slow path max | < 100μs | > 500μs |

High max values with low avg usually mean OS scheduling interference (context switches,
interrupts). Run with `taskset -c 0` to pin to a core, or `nice -n -20` for priority.

```bash
# pin to core 0 for cleaner measurements
taskset -c 0 ./engine

# reduce scheduling jitter
sudo nice -n -20 ./engine
```
