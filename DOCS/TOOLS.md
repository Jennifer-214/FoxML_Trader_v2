# TOOLS.md — `tools/` inventory + disposition registry

**Every `tools/*.{sh,py}` MUST have a row here.** Enforced by `tools/check_tools_inventory.py` (CI guard; a new tool without a row = red build — the canonical-sister of `check_meta_registry.py` for code registries). This closes the rot class: a tool that exists but isn't invoked/dispositioned (the `gen_code_map`/`CODE_MAP.md` failure — built, never wired, silently stale) can no longer hide. Codified `.E` Session-7 (D-134/D-135/D-136).

**Re-audit:** `/tools-audit` (quarterly cadence; sister `/anti-spaghetti` + `/metadata-audit`) — drift + orphan + spec-coverage + reference-vs-invocation. **Key metric:** *invocation* (a real `bash/python tools/x` call-site), NOT a prose mention — per D-115.

## Disposition taxonomy
| Disposition | Meaning |
|---|---|
| **STANDING-CI** | Run by the pre-commit hook or a CI umbrella (`check_determinism.sh`, `check_session_docs.sh`) — directly or transitively |
| **SKILL-WIRED** | Invoked by ≥1 skill (real call-site in the SKILL.md body) |
| **ONE-OFF-MANUAL** | Operator-run analysis/viz; no wiring expected (no spec required) |
| **TEST-HARNESS** | A test for another tool; run by the test runner |
| **ORPHANED(wire\|retire)** | Exists but nothing invokes it — must be wired or retired |
| **PLANNED** | Tracked future tool — referenced (disclaimed) but not yet built; has a fallback now (off-disk OK) |
| **RETIRED** | Deleted tool — tracked only to account for lingering historical references (off-disk OK) |

## Inventory

| Tool | Disposition | Invoked by | Spec | Purpose |
|---|---|---|---|---|
| `check_determinism.sh` | STANDING-CI | pre-commit (Check F); umbrella | ✓ | `.E.0.1` determinism-net umbrella (runs the FP + locale gates) |
| `check_fp_determinism.sh` | STANDING-CI | `check_determinism.sh` | — | `.E.0.1` FP-determinism gate |
| `check_locale_determinism.sh` | STANDING-CI | `check_determinism.sh` | ✓ | `.E.0.1` locale-determinism guard |
| `check_session_docs.sh` | STANDING-CI | hook; close/accept/precoding/readiness; umbrella | ✓ | one-shot doc/plan CI sweep (runs the doc checks below) |
| `check_capture_audit.py` | STANDING-CI | hook; `check_session_docs`; capture/accept-handoff | — | mechanical decision-capture drift check (11 checks) |
| `check_doc_metadata.py` | STANDING-CI | hook; `check_session_docs`; capture/doc-create/metadata-audit/ship/sync | ✓ | doc frontmatter bidirectional + index check |
| `check_forward_promise_audit.py` | STANDING-CI | hook; `check_session_docs`; +6 skills | ✓ | forward-promise landed-verification (Check 11) |
| `check_plan_body_symbol_existence.py` | STANDING-CI | hook; `check_session_docs`; +4 skills | ✓ | Class-14 plan-body symbol-existence (B-Plus) |
| `check_plan_body_tests_section.py` | STANDING-CI | hook; plan-dive/precoding/readiness | — | Check-45 tests-section presence |
| `check_doc_rename_classification.py` | STANDING-CI | hook | — | B19 prose-token rename-classification guard |
| `check_meta_registry.py` | STANDING-CI | `check_session_docs`; capture/registry-fit | ✓ | `FOREACH_REGISTRY` meta-registry coverage (H15) |
| `check_tools_inventory.py` | STANDING-CI | hook (via `check_session_docs`) | ✓ (this doc) | THE tool-rot guard — every `tools/*.{sh,py}` has a row here (D-136) |
| `check_identifier_retirement.py` | STANDING-CI | pre-commit (Check H); readiness (Check 46); post-ship-audit | ✓ | H21 tombstone guard — persistence/wire identifiers (snapshot/format VERSIONs + persisted enum CODES) append-only + immutable vs the golden `identifier_ledger.txt` (Class 40; `.E` #11 guard-hardening) |
| `check_tech_debt.py` | STANDING-CI | pre-commit (Check J, advisory); post-ship-audit; readiness (Check 25) | — | tech-debt surface advisory — surfaces OPEN TECH_DEBT overlapping the staged/ship files (classify subsume/adjacent/defer per `feedback_opportunistic_tech_debt_closure`); also `--stale` / `--close` modes (`.E` #11) |
| `check_per_core_registry_integrity.py` | SKILL-WIRED | accounting/capture/dod/readiness | ✓ | `PerCoreCfg` X-macro integrity (H17 Check 2) |
| `calls_graph_diff.sh` | SKILL-WIRED | dust/post-ship-audit/readiness/ship | ✓ | strategy/regime orphan-diff (legacy vs sharded) |
| `check_field_name_uniqueness.py` | SKILL-WIRED | blindspot-scan | ✓ | cfg field-name uniqueness |
| `check_struct_field_uniqueness.py` | SKILL-WIRED | readiness | ✓ | struct field-name uniqueness |
| `check_storage_t_coverage.py` | SKILL-WIRED | blindspot-scan | ✓ | `storage_t` coverage check |
| `dedupe_findings.py` | SKILL-WIRED | plan-dive | — | findings dedupe |
| `migrate_memory_frontmatter.py` | SKILL-WIRED | capture-audit/sync-workspace | — | memory frontmatter re-derive + sister-symmetrize |
| `rebuild_doc_indexes.py` | SKILL-WIRED | ship | — | regenerate doc indexes |
| `gen_code_map.sh` | SKILL-WIRED | dust/plan-check/readiness/ship | NEEDS (#24) | code-intelligence index (fn map + `--types`/`--structs`/`--macros`/`--full` type→sites blast-radius). `CODE_MAP.md` is gitignored (generated); now regen-on-use (ship Stage 4 unconditional + readiness regen-first) — no more stale-copy rot |
| `test_check_tools_inventory.py` | TEST-HARNESS | test runner | — | NEGATIVE self-test — proves the rot-guard goes RED on unenrolled-tool + broken-ref violations (teeth) |
| `test_check_doc_rename_classification.py` | TEST-HARNESS | test runner | — | tests for `check_doc_rename_classification.py` |
| `test_check_plan_body_tests_section.py` | TEST-HARNESS | test runner | — | tests for `check_plan_body_tests_section.py` |
| `test_memory_guard.py` | TEST-HARNESS | test runner | — | tests for the memory guard |
| `chart.py` | ONE-OFF-MANUAL | — | n/a | metrics charting (operator viz) |
| `feature_overlay.py` | ONE-OFF-MANUAL | — | n/a | feature-overlay viz (operator) |
| `compare_scalers.sh` | ONE-OFF-MANUAL | — | n/a | scaler-comparison CLI wrapper (manual; `compare_scalers.cpp`) |
| `subdivide_design_specs.py` | ONE-OFF-MANUAL | — | n/a | one-time DESIGN_SPECS subdivision (used once) |
| `check_determinism_selftest.sh` | SKILL-WIRED | /post-ship-audit | ✓ | the net's NEGATIVE self-test — verifies each gate goes RED on its injected regression (the net has teeth) |
| `scan_class_27_full.py` | SKILL-WIRED | /bug-check (Step 3) | ✓ | full Class-27 (scalar cfg-mirror) mechanical scan |
| `validate_feature_mask.sh` | SKILL-WIRED | /ml-audit (Section B) | ✓ | feature-mask e2e validation |
| `check_plan_enumeration_completeness.py` | SKILL-WIRED | precoding-audit-gate (Stage 2.5) | NEEDS (#24) | plan's claimed enumeration SET ⊇ the code-intel tool output (catches AR-1 summarize-and-drop — the tool was run, the summary lost members; D-141 / `.E` Session-8) |
| `test_check_plan_enumeration_completeness.py` | TEST-HARNESS | test runner | — | NEGATIVE self-test — proves the under-enumeration guard goes RED on a dropped-file plan + names it (teeth) |
| `check_identifier_retirement_selftest.sh` | TEST-HARNESS | post-ship-audit; manual | — | NEGATIVE self-test — proves the H21 tombstone guard goes RED on renumber / version-decrease / silent-removal (teeth) |
| `check_tech_debt_selftest.sh` | TEST-HARNESS | post-ship-audit; manual | — | NEGATIVE self-test — proves the tech-debt advisory DETECTS overlaps (+ no false positives) + `--stale --strict` goes RED (teeth) |

*Non-executable helpers (not in the guard's scope): `replay_locale_gate.cpp` / `fp_determinism_golden.cpp` / `compare_scalers.cpp` (compiled by the `.sh` gates), `*_baseline.txt` / `*_golden.txt` / `identifier_ledger.txt` (frozen data).*

## Planned / retired (tracked off-disk — Check 2 accounts for references)

| Tool | Disposition | Status |
|---|---|---|
| `check_amendment_cascade.py` | PLANNED | CP-1 amendment-cascade mechanization (capture-audit Check 12 / precoding-gate). Semi-mechanical fallback runs now; build → wire into `--deep` + pre-commit. Correctly disclaimed "not yet built" in the citing skills. |
| `stamp_model.sh` | RETIRED | bash-CLI stamper DELETED at v5.15.5.F.4d.1.B.3 (Path C) — stamping is now in-process / foxml_suite GUI auto-stamp. Lingering runnable cites in /ship + /parity-check fixed `.E` Session-7. |

## Actionable (status)
1. ✅ **3 orphans WIRED** (`.E` Session-7): `check_determinism_selftest.sh` → /post-ship-audit · `scan_class_27_full.py` → /bug-check Step 3 · `validate_feature_mask.sh` → /ml-audit Section B.
2. ✅ **`gen_code_map` regen FIXED:** `CODE_MAP.md` is gitignored (generated) — now regen-on-use (ship Stage 4 unconditional + readiness regen-first); no more stale-copy rot.
3. ⏳ **Spec gaps among load-bearing tools** (paced, #24): `check_capture_audit.py`, `check_plan_body_tests_section.py`, `migrate_memory_frontmatter.py`, `dedupe_findings.py`, `rebuild_doc_indexes.py`, `gen_code_map.sh` are STANDING/WIRED with no DESIGN_SPEC — not blocking.
