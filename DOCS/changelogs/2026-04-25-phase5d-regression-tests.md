# 2026-04-25 (evening) — Phase 5d regression tests

Branch: `experiment/live-readiness`. First commit on the branch — locks in
this past weekend's eight bug fixes as automated tests before Phase 8/8a/8b
work touches any of the same files.

No new functionality. Pure test additions. controller_test goes from
**279 → 296 assertions**, 0 failures on first commit.

See `plans/phase5d-regression-tests.md` for the full plan and bug-by-bug
mapping. See `plans/live-readiness-master.md` for sequencing within the
broader live-readiness phase.

---

## What's locked in

Each test below guards a specific bug class found and fixed across Saturday
2026-04-24 and Sunday 2026-04-25. Up to this point those fixes were only
verified by manual smoke testing — there were no automated regression tests
guarding against the same class of bug returning. These tests are cheap
permanent insurance.

| Group | Test | Bug class | Original commit |
|---|---|---|---|
| 1 | Reset preserves equity_curve + sample buffers (ptrs + caps), zeroes counts | Adding a new dynamic field to `BacktestResults` without extending `_Reset` (next field added would silently break) | `fcf9616` |
| 1 | EnsureEquityCapacity floor: cap=0 seeds to BACKTEST_EQUITY_INIT (no spin) | Belt-and-suspenders against the same bug class — `while (0 < needed) cap *= 2` infinite spin if `_Reset` regresses | `fcf9616` |
| 1 | EnsureCapacity (samples) floor: cap=0 seeds to BACKTEST_SAMPLES_INIT | Same floor on the sample-buffer side | `fcf9616` |
| 2 | LabelType_NumClasses: WIN_LOSS=0, FORWARD_PNL=1, PEAK_VALLEY_STABLE=3, REGIME=4 | Hardcoding binary classification metrics on regression / multiclass labels | `cd2936d`, `8d175b1` |
| 2 | LabelType_IsBinary identifies WIN_LOSS | Helper-correctness — every metric site reads via these helpers | same |
| 2 | LabelType_IsRegression identifies FORWARD_PNL | same | same |
| 2 | LabelType_IsMulticlass identifies PEAK_VALLEY_STABLE + REGIME | same | same |
| 2 | LabelType_* out-of-bounds defaults to safe binary kind | Future-proofing — adding a new LABEL_* without updating the helpers should fail safe to binary | same |
| 3 | XGBoost_ComputeScalePosWeight basic (n_pos=2, n_neg=4 → w=2.0) | Class-balance compensation for binary imbalance | `38ab41d` |
| 3 | XGBoost_ComputeScalePosWeight zero-positive guard returns 1.0 | Divide-by-zero on degenerate datasets (no positives) | `38ab41d` |
| 3 | XGBoost_ComputeMulticlassWeights: per-class counts {4,1,1} | Multiclass per-sample weight pipeline | `38ab41d` |
| 3 | XGBoost_ComputeMulticlassWeights: inverse-frequency formula {0.5, 2.0, 2.0} | Inverse-frequency math correctness | `38ab41d` |
| 3 | WalkForward_ComputeCorrelation: perfect linear → r=1.0 | Pearson correlation as primary regression metric | `cd2936d` |
| 4 | min_warmup_samples=512 clamps to 128 (rolling window cap) | Silently never-completing warmup when value > rolling window size | `c6aa0cc` |
| 4 | fee_rate=0.10 parses to 0.001 fraction (CFG_PARSE_PCT) | Locks the percentage-vs-multiplier convention before Phase 8 maker/taker split | (existing parser, locked here) |
| 5 | GATE_REASON_TABLE: every entry has a non-empty name | Adding a new GATE_REASON_* without the matching name-table entry — surfaces at test-time, not via NULL-deref in the TUI | `c95ef3f` |
| 5 | REJECT_REASON_NAMES: every non-zero index has a non-empty name | Same shape on the OMS reject-reason side | `c95ef3f` |

The label-buffer accumulation regression (`9155558`) is tracked but
deferred: it's an end-to-end multi-file integration test rather than a unit
test. Documented as a known gap in `phase5d-regression-tests.md` to address
with a synthetic-CSV fixture later.

## Build-system change

`tests/controller_test.cpp` now pulls in `Backtest/BacktestEngine.hpp` for
`BacktestResults_*`, `XGBoost_Compute*`, and `WalkForward_ComputeCorrelation`
— and transitively `LabelType_*` via `LabelFunctions.hpp`. That header
references `TUISnapshot`, which is gated by `MULTICORE_TUI` in
`DataStream/EngineTUI.hpp`.

Added `MULTICORE_TUI` to the `controller_test` target's compile definitions
in `CMakeLists.txt`, plus `Threads::Threads` link for the pthreads pulled in
under that gate. Same approach the `experiments/per_core_sharding`
sub-CMake uses for its test targets, and matches the production engine
target. No change to test semantics — just makes `TUISnapshot` visible so
the include compiles.

## Verification

```
$ ./build.sh test
[...]
RESULTS: 296 passed, 0 failed
```

All four targets build clean: `engine`, `engine_gui`, `foxml_suite`,
`controller_test`. (Pre-existing SPSCRing / FauxFIX warnings are known
background per CLAUDE.md.)

## Tag

`phase5d-regression-tests` set at this commit for cheap rollback before
Phase 8 / 8a / 8b work begins.
