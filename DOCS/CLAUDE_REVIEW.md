# Plan Review Checklist

**Read this file before starting any multi-day plan. Audit BEFORE coding.** Walk through all 10 items; mark each as PASS / FIXED / GAP / DRIFT / DEFERRED / ACCEPTED.

## The 10 items

1. **Hot path purity** — touches `ExecutionCore_Tick`/`BG_Evaluate`/`SG_Evaluate`? Branchless, FPN-only, alloc-free? p99 ≤500ns. New code defaults to slow path.

2. **Train-serve parity** — touches `RegimeSignals`/`ModelFeatures_Pack`? BOTH `BacktestSharded_Run` AND `EngineSharded_Run` populate with equivalent cadence + inputs. (Post-v5.1.2: both go through `EventLoop_UpdateRollingStateAllCores` + `EventLoop_RebuildAllParameters_PerCore`.)

3. **Surface area / coupling** — minimize files touched. `if (live_trading)` branches = wrong abstraction. New optional state owns lifecycle in ONE place. **Preserve public surface during refactors** — keep field names + signatures stable, change contents not names. Append-only enums, fields-at-end struct layouts.

4. **Pointer init + heap lifecycle** — every `*_Init` caller NULL-inits; `_Init` does `if (ptr) free(ptr)` before realloc; cleanup frees + NULLs; snapshots persist or document session-only.

5. **Backward compat** — `SHARDED_SNAPSHOT_VERSION` bump = old refused. `MODEL_FORMAT_VERSION` bump = old models fail. Saved Runs forward-compat (additions OK). Cfg additions OK, removals break user cfgs.

6. **Multi-threading correctness** — atomic vs not, SPSC ring producer/consumer, slow-path/hot-path on `GateParameters` uses seqlock. Backtest output reproducible. Single-writer rules per `state.cores[c].slow_state` (see Per-Core Data-Plane Single-Writer invariant).

7. **Test coverage** — round-trip hammer test, edge cases (cold start, full window, wraparound, zero, uninit), runs in `controller_test`. Parity verification via `parity_harness` if touching feature collection.

8. **Docs + invariants** — load-bearing rule? Add Safety Invariant to `DOCS/CLAUDE_INVARIANTS.md`. Update `DOCS/CHANGELOG.md`. Dated changelog in `DOCS/changelogs/`.

9. **Forward maintenance** — 30+ sites to extend? redesign. Next similar feature copies code? factor helper. Document brittle assumptions.

10. **Rollback story** — tag `pre-{name}`, push to remote. Multi-week → branch. Each phase individually revertable.

## Verdicts vocabulary

- **PASS** ✅ — item satisfied as-is
- **FIXED** ✅ — patched in same pass
- **GAP** ⚠️ — must address before shipping
- **DRIFT** ⚠️ — pre-existing issue, not blocking but flagged
- **DEFERRED** — explicit "ship without this" decision, with reason
- **ACCEPTED** — divergence chosen, documented (e.g. backtest all-taker fees)

## When to do this check

- Before any plan estimated > 1 day of work
- Before merging a feature branch back to main
- Before any architectural change (per-core / data plane / threading topology / OMS contract)
- After a PR review surfaces a concern about one of the 10 items

## When to skip

- Bug fix < 50 lines, single file
- Cosmetic changes (doc, label rename, formatting)
- Test additions that don't change engine behavior
- Tooling / build system changes
