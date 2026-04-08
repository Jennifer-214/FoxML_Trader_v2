# Per-Core Risk-Sharded Hot Path Experiment

Standalone microbenchmark to validate the proposed per-core risk-sharded
architecture against the current single-core bitmap-walk design.

See `plans/per_core_risk_sharding_experiment.md` for the full design rationale,
success criteria, and what this experiment does and does not prove.

## Build

```bash
cd experiments/per_core_sharding
cmake -B build
cmake --build build
```

## Run

Smoke test (current state — validates the measurement foundation):
```bash
./build/test_rdtsc
```

Expected output: TSC frequency in the 1-2 GHz range and an empty loop body
measured in tens of cycles. If TSC frequency is reported as 0 or wildly
unstable, the rdtsc path is wrong for this CPU and we need to switch to
`clock_gettime(CLOCK_MONOTONIC_RAW)` instead.

## Hardware notes

This experiment was scoped on an Intel i5-1035G4 (4 physical / 8 logical cores,
laptop CPU). The TSC has `constant_tsc`, `nonstop_tsc`, and `rdtscp` available,
so rdtsc-based measurement is viable, but:

- Thermal throttling is real on a laptop. Run short bursts, not long sustained
  loads. Convert cycles to ns using runtime-calibrated TSC frequency.
- Only 4 physical cores means at most 1 worker can be tested with proper
  isolation (1 reader + 1 aggregator + 1 worker + 2 free for OS = 5 cores
  ideal, we'll have to compromise).
- No `isolcpus` kernel param assumed — use `chrt -f 90` and short bursts to
  reduce kernel preemption noise.
- Pin to physical cores only (probably 0-3), avoid sibling logical cores
  (4-7) to prevent execution unit contention.

These constraints mean we can validate the *shape* of the design (whether
sub-100ns per-tick latency is plausible on this architecture) but not produce
production-grade numbers. Production validation would need a server-class CPU
with `isolcpus`, more cores, and a stable thermal envelope.

## Status

- [x] Worktree created (`experiment/per-core-sharding` branch off master)
- [x] CMake skeleton + smoke test
- [ ] Smoke test verified on this hardware
- [ ] TSC frequency calibration validated
- [ ] Common harness (synthetic ticks, measurement macros, percentile reporting)
- [ ] Baseline single-core (16-position bitmap walk)
- [ ] Sharded per-core (1 position per core, mini_balance, SPSC ring)
- [ ] Side-by-side measurement run
- [ ] Results report (`results/REPORT.md`)
- [ ] Decision: production port, redesign, or stay single-core
