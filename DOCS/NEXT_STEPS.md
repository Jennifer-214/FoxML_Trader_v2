# Next Steps

**This file used to be the canonical roadmap (v3.x era). It's now a
navigation index — the actual roadmap moved to per-sprint planning
docs.**

## Where to look

| For | See |
|---|---|
| Current sprint roadmap (Sprint B / v5.10 epic) | `DOCS/v5.10-design-notes.md` — 19 ideas, ~10 shipped, ~9 in flight |
| What just shipped | `DOCS/CHANGELOG.md` — v5.9.5h–j Sprint A close + v5.10.0/0a Sprint B in progress |
| Known limitations + workarounds | `DOCS/KNOWN_ISSUES.md` |
| Architectural invariants per area | `DOCS/CLAUDE_INVARIANTS.md`, `DOCS/CLAUDE_ML_INVARIANTS.md` |
| Hot path perf changelog | `DOCS/HOT_PATH_CHANGELOG.md` |
| Deferred to next major (v5.11) | `DOCS/v5.10-design-notes.md` Sprint C section |

## Sprint state at a glance (as of 2026-05-06)

- **Sprint A — v5.9 ML Hardening:** SHIPPED + merged 2026-05-03
  (commit `5ea002c`, tag `v5.9.5j-final`).
- **Sprint B — v5.10 epic:** 2/6 shipped on `feat/v5.10-foundation`.
  - ✅ v5.10.0 foundation (perf + RAM + hardware-aware cfg + bug fixes)
  - ✅ v5.10.0a multi-horizon ensemble (parallel sweep + bandit blend)
  - ⏳ v5.10.0b — FPN-end-to-end slow path (next ship)
  - ⏳ v5.10.0c — hot model swap
  - ⏳ v5.10.0d — FOREACH_TARGET label registry
  - ⏳ v5.10.0e — drift detection + auto-retire
- **Sprint C — v5.11+ deferred:** flexible; pick items as operator
  need surfaces. LLM operator helper, multi-symbol stamp binding,
  per-core feature mask, scaler comparison tool, doc gaps.

## Working notes

Per-sprint implementation plans live in `plans/` (gitignored;
operator-private). The public surface is design notes + CHANGELOG +
the Sprint B section above.
