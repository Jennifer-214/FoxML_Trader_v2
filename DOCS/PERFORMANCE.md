# Performance Guide

## Current measurements (2026-03-23, BTC $70k, laptop)

| Mode | Hot-path avg | Hot-path min | Slow-path avg |
|------|-------------|-------------|---------------|
| TUI on (profiling) | ~100-200ns | 82ns | ~14μs |
| TUI off (bench) | ~90ns | 82ns | ~3-5μs |

The min (82ns) is the true hot-path cost with warm cache. TUI-on avg is now close to
bench mode thanks to zero-pollution snapshot: full `TUI_CopySnapshot` runs on the slow
path (L1 already warm), while the hot path only writes price+volume+active_count to the
active snapshot (1 cache line, ~7ns).

## Hot-path breakdown (82ns, 2 positions, TUI off)

| Component | Cost | Notes |
|-----------|------|-------|
| BuyGate | ~20ns | 2 FPN comparisons + conditional pool write |
| PositionExitGate | ~30ns/position | 2 TP/SL comparisons per position, conditional exit write |
| PortfolioController_Tick | ~20ns | Bitmap fill detection, tick counter, early return |
| RDTSCP measurement | ~8ns | 2 calls, only present in profiling builds |

PositionExitGate dominates — cost scales linearly with position count:
- 0 positions: ~40ns total
- 2 positions: ~100ns total
- 16 positions: ~520ns total

## What actually affects performance

### High impact

**TUI snapshot architecture** — full `TUI_CopySnapshot` now runs on the slow path
(every `poll_interval` ticks), piggybacking on L1 data already loaded by rolling stats,
regime detection, and strategy dispatch. Between slow paths, only price/volume/active_count
are written to the active snapshot (1 cache line, ~7ns). This eliminates TUI-induced L1
cache pollution on the hot path. TUI-on latency is now near bench-mode levels.
To further increase TUI refresh rate, lower `poll_interval` in engine.cfg (e.g. 30).

**Fewer positions per core** — PositionExitGate is O(positions). Each position adds
~30ns (two 128-bit FPN comparisons + bitmap ops). For colocation: one position per core
per symbol eliminates the bitmap walk entirely.

**FPN word count (N)** — every FPN comparison, add, multiply loops over N 64-bit words.
Current FPN<64> has N=2 (128-bit total). If a future FPN design could use N=1 with
sufficient precision for BTC, every FPN operation would be 2x faster. However, N=1
(Q32.32) overflows on BTC price² in regression math, so this requires asymmetric
integer/fractional bit allocation — not a simple change.

**Core pinning** — eliminates context switch spikes (the 27μs+ max values). Prevents
the OS scheduler from preempting the engine mid-tick.
```bash
taskset -c 0 ./engine              # pin to core 0
sudo nice -n -20 ./engine          # high priority scheduling
```
For full isolation, add `isolcpus=0` to kernel boot params so nothing else runs on
that core.

### Low impact (single-digit nanoseconds)

**Struct field reordering** — hot-path fields (portfolio, buy_conds, exit_buf) are
currently scattered across 169 cache lines due to cold data interleaved between them.
Packing them contiguously at offset 0 (~50 cache lines) would improve hardware
prefetching after TUI-induced cache eviction. Helps ~10-20ns per TUI render cycle.
No effect with TUI off.

**FPN sign packing** — each FPN<64> wastes 4 bytes of alignment padding (24 bytes
actual vs 20 bytes raw). Packing the sign bit into the MSB of the highest word would
save 8 bytes per FPN (24 → 16 bytes, 33% reduction). Saves ~7KB on the controller
struct. Requires rewriting every FPN operation — high effort, deferred.
See `plans/fpn_sign_packing.md`.

### No impact

**Heap vs stack allocation** — the controller struct is allocated once at startup.
Whether it's on the heap or stack makes zero difference to hot-path performance. L1
cache loads individual cache lines on demand regardless of where the memory lives.

**Struct total size** — the controller struct is 36KB (110% of L1), which looks
alarming but doesn't matter. The hot path only touches ~3KB. The remaining 33KB
sits in L2/L3 and only enters L1 when the slow path or TUI accesses it. The "110%
of L1" metric is misleading.

## Architecture for colocation (future)

For maximum performance on a colocated server:

```
per core:
  1 symbol
  1 strategy
  1 position
  TUI off
  core pinned + isolated
  poll_interval tuned low (10-20 ticks)

central process:
  portfolio controller aggregating positions across cores
  risk management (circuit breaker, exposure limits)
  TUI / monitoring on a separate core
```

This eliminates:
- Bitmap walks (1 position = no loop)
- TUI cache pollution (TUI on separate core)
- Context switches (isolated core)
- Position spacing checks (1 position = no neighbors)

Expected hot-path cost: ~40-50ns (BuyGate + single TP/SL check + tick counter).

## Build modes

```bash
# production — zero measurement overhead
g++ -std=c++17 -O2 -lssl -lcrypto -o engine main.cpp

# profiling — measurement in TUI (avg inflated by TUI cache pollution)
g++ -std=c++17 -O2 -lssl -lcrypto -DLATENCY_PROFILING -o engine_prof main.cpp

# bench — TUI off, clean measurements to stderr
g++ -std=c++17 -O2 -lssl -lcrypto -DLATENCY_PROFILING -DLATENCY_BENCH -o engine_bench main.cpp
```

See `DOCS/LATENCY_PROFILING.md` for detailed measurement guide.
