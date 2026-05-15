# Manual fields inventory — exemptions from registry-driven struct generation

**Established:** v5.15.5.F.4c.3 (2026-05-15)
**Scope at this ship:** per-core surface (`PerCoreCfg<F>` runtime cluster + parallel arrays on `ControllerConfig<F>`)
**Extension at .F.4d:** global cfg surface (`ControllerConfig<F>` global fields + manual global exemptions)

## Discipline

Every parallel-array entry below MUST have a corresponding row in `FOREACH_MANUAL_PER_CORE_FIELD` X-macro at `CoreFrameworks/CfgFieldRegistry.hpp`. Every `PerCoreCfg<F>` runtime-cluster field MUST be in the permitted list below.

CI cross-checks bidirectionally via `tools/check_per_core_registry_integrity.py` (invoked from `build.sh`); missing in either source = BUILD ERROR.

Adding a new exemption:
1. Add row to this table with full justification
2. Add row to `FOREACH_MANUAL_PER_CORE_FIELD` X-macro (parallel arrays) OR to the PerCoreCfg<F> runtime-cluster permitted list (bitmap fields)
3. Commit message explicitly justifies why the field can't fit `FOREACH_PER_CORE_CFG_FIELD` registry
4. If TRANSITIONAL: specify the ship that closes the exemption

Removing an exemption:
1. Confirm migration trigger fired (field now lives in `FOREACH_PER_CORE_CFG_FIELD` or has been deleted from codebase)
2. Delete row from this table + matching X-macro entry / permitted-list entry
3. CI re-runs; build verifies no stray references remain

## Section A — `ControllerConfig<F>` parallel arrays (FOREACH_MANUAL_PER_CORE_FIELD entries)

| Field | Type | Suffix | Storage | Rationale | Migration trigger |
|---|---|---|---|---|---|
| `core_model_path` | `char` | `[256]` | `char core_model_path[16][256]` | Per-core legacy single-model path (used when ensemble dir not set). `char[N]` arrays don't fit current KIND enum spectrum (KIND_FILE_PATH ships at `.F.4e`). | `.F.4e` KIND_FILE_PATH cohort |
| `core_model_dir` | `char` | `[256]` | `char core_model_dir[16][256]` | Per-core model directory paths for ensemble auto-discovery. Same KIND_FILE_PATH constraint. | `.F.4e` KIND_FILE_PATH cohort |
| `core_horizon_list` | `char` | `[128]` | `char core_horizon_list[16][128]` | Per-core ensemble horizon CSV string. KIND_STRING constraint. | `.F.4e` KIND_STRING cohort |
| `core_ensemble_blend_mode` | `char` | `[16]` | `char core_ensemble_blend_mode[16][16]` | Per-core ensemble blend mode name string. May migrate to KIND_INT_ENUM at .F.4d if blend modes become enum-driven. | `.F.4d` (KIND_INT_ENUM) OR `.F.4e` (KIND_STRING) |
| `core_disabled_horizons` | `char` | `[128]` | `char core_disabled_horizons[16][128]` | Per-core disabled-horizons CSV string. KIND_STRING constraint. | `.F.4e` KIND_STRING cohort |
| `core_symbol` | `char` | `[32]` | `char core_symbol[16][32]` | **WIP2d-1.A** (.F.4c.3.A partial advance) — per-core symbol string (e.g., "BTCUSDT"). Operator-facing forward-compat for multi-symbol DataStream (when ready). Boot-time uniformity check enforces all active cores share symbol until DataStream multi-symbol support ships. Bridges to legacy BinanceConfig.symbol at boot init (main.cpp pre-EngineSharded_Run). | `.F.4e` KIND_STRING cohort + full .F.4c.3.A migration (UI design + multi-symbol DataStream) |
| `core_feature_mask` | `uint64_t` | (none) | `uint64_t core_feature_mask[16]` | Per-core feature bitmap (64-bit; one bit per FOREACH_FEATURE entry). Currently no Kind for typed hex64 cfg values. | `.F.4e` KIND_HEX64 cohort |
| `core_risk_pct` | `FPN<F>` | (none) | `FPN<F> core_risk_pct[16]` | **TRANSITIONAL** — duplicate source-of-truth during shadow window. `cores[c].risk_pct` (in FOREACH_PER_CORE_CFG_FIELD registry) is authoritative; `core_risk_pct[16]` survives during shadow for legacy `core_<N>_risk_pct=` parser path. `PopulateCoresFromFlat` syncs core_risk_pct[c] → cores[c].risk_pct. | `WIP2g` (this ship) — delete legacy array; rely on `[core N]` parser (Step 3) to write directly to `cores[c].risk_pct` |
| `core_strategies` | `uint8_t` | (none) | `uint8_t core_strategies[16]` | **TRANSITIONAL** — same shadow pattern as `core_risk_pct`. `strategy` row added to FOREACH_PER_CORE_CFG_FIELD at WIP2d-0 (Finding 1 closure); auto-generates `cores[c].strategy`. Legacy array survives shadow for `core_<N>_strategy=` parser path. | `WIP2g` (this ship) — delete legacy array |
| `core_time_exit_ticks` | `uint32_t` | (none) | `uint32_t core_time_exit_ticks[16]` | **TRANSITIONAL** — legacy per-core override of `max_hold_ticks`. `cores[c].max_hold_ticks` is authoritative per registry. Shadow walker syncs. | `WIP2g` (this ship) — delete legacy array |
| `core_max_drawdown_pct` | `FPN<F>` | (none) | `FPN<F> core_max_drawdown_pct[16]` | **TRANSITIONAL** — legacy per-core override of `max_drawdown_pct`. `cores[c].max_drawdown_pct` is authoritative per registry. Shadow walker syncs. | `WIP2g` (this ship) — delete legacy array |
| `core_overrides` | `PerCoreOverrides<F>` | (none) | `PerCoreOverrides<F> core_overrides[16]` | **TRANSITIONAL** — legacy global-default-with-override anti-pattern (cfg-scope-discipline.md § Anti-pattern 1). Entire mechanism (struct + array + ControllerConfig_ResolveForCore resolver) retires at WIP2f. | `WIP2f` (this ship) — delete with the legacy override mechanism |

**Section A total: 11 entries** (6 awaiting `.F.4e` framework support + 4 TRANSITIONAL deleted at WIP2g + 1 TRANSITIONAL deleted at WIP2f).

## Section B — `PerCoreCfg<F>` runtime bitmap cluster (X-macro generated via meta-registry — WIP2d-0.B)

Post-WIP2d-0.B these fields are GENERATED by `FOREACH_PER_CORE_DOMAIN_BITMAP` meta-registry in `CoreFrameworks/CfgFieldRegistry.hpp`. The meta-registry has one row per bitmap domain — `(domain_token, field_name, storage_type, child_registry)` — and drives multiple auto-flows:

1. **Struct field declarations** in `PerCoreCfg<F>` (via `FOREACH_PER_CORE_DOMAIN_BITMAP(EMIT_DOMAIN_BITMAP_FIELD)`)
2. **Bitmap-overflow static_asserts** per domain (auto-generated; defense in depth alongside the per-registry asserts) per `bitmap-overflow-protection-discipline.md`
3. **WIP2e bitmap rebuild walker** (iterates meta-registry; for each domain walks `child_registry` to rebuild from flat KIND_BOOL rows)
4. **CI cross-check** — every domain registered ↔ Section B row + every domain registry has a bitmap field

CI script reads this section + the meta-registry; bidirectional sync enforced. Adding a 6th domain registry without a meta-registry row = BUILD ERROR.

This is the FIRST canonical application of `meta-registry-pattern-for-codebase-registry-discipline.md` (Stage 3 ACTIVE at WIP2d-0.B; one ship before .F.4d H15 codification of the codebase-wide `FOREACH_REGISTRY` meta-registry).

| Field | Type | Alignment | Rationale |
|---|---|---|---|
| `lifecycle_cfg_flags` | `uint8_t` | `alignas(8)` (cluster boundary) | Runtime bitmap of `FOREACH_LIFECYCLE_CFG_FLAG` bits; rebuilt from flat rows at WIP2e |
| `gate_cfg_flags` | `uint8_t` | natural | Runtime bitmap of `FOREACH_GATE_CFG_FLAG` bits |
| `ml_cfg_flags` | `uint16_t` | natural | Runtime bitmap of `FOREACH_ML_CFG_FLAG` bits (12-bit domain) |
| `risk_cfg_flags` | `uint8_t` | natural | Runtime bitmap of `FOREACH_RISK_CFG_FLAG` bits |
| `ops_cfg_flags` | `uint8_t` | natural | Runtime bitmap of `FOREACH_OPS_CFG_FLAG` bits |

**Section B total: 5 entries** (all stay; the bitmap fields are RUNTIME representations and don't migrate to flat — the FLAT KIND_BOOL rows added at WIP2e are the cfg surface; bitmaps are the rebuilt runtime mirror that hot path mask-dispatches against).

## Statistics

- **Inventory size at v5.15.5.F.4c.3 ship close:** Section A = 11 entries, Section B = 5 entries, total = 16
- **Permanent exemptions (Section A):** 6 (5 string arrays + core_feature_mask awaiting `.F.4e` Kind infrastructure)
- **TRANSITIONAL exemptions (Section A):** 5 (delete at WIP2g: 4 arrays; delete at WIP2f: core_overrides)
- **Section B permanent:** 5 (runtime bitmap cluster; structural fixture)
- **Inventory size projected at `.F.4e` close:** Section A = 0 entries (all migrate to KIND_STRING/_FILE_PATH/_HEX64); Section B unchanged at 5

## Section C — Subsystem-state cfg-mirror exemptions (Class 27 exemption registry; established v5.15.5.F.4c.3 WIP2d-1.B.0c)

Per `DESIGN_SPECS/decision-time-data-binding-pattern.md` + `RECURRING_BUG_PATTERNS.md` Class 27, scalar cfg-mirror fields on subsystem state types are FORBIDDEN by default. Subsystems must either:
- (a) Pre-resolve cfg values onto in-flight objects (Order, Position, Event, TradeEvent) at decision time (PREFERRED), OR
- (b) Use `FOREACH_<SUBSYS>_CFG_CACHE` registry-driven per-instance cache (fallback for genuinely no-in-flight-object cases)

CI Check 7 (`tools/check_per_core_registry_integrity.py`) scans designated subsystem state types for scalar fields whose names match cfg field names; build-fails on unregistered matches. Exemptions to the check live here in Section C.

| Subsystem | Field | Rationale category | Detail | Migration trigger |
|---|---|---|---|---|
| `OrderManagerState` | `fee_rate` | TRANSITIONAL | Legacy scalar cfg-mirror of `cfg.cores[c].fee_rate` (deprecated single-rate field; pre-maker/taker era); Class 27 anti-pattern. Used as fallback when maker/taker zero. DELETED at WIP2d-1.B.1 alongside maker/taker; legacy callers migrate to `oms->per_core_fee_rate_taker` fallback OR Order `effective_fee_rate`. | `WIP2d-1.B.1` (this ship) — delete OMS scalar field; remove this exemption in same commit |
| `OrderManagerState` | `fee_rate_maker` | TRANSITIONAL | Scalar cfg-mirror of `cfg.cores[c].fee_rate_maker`; Class 27 anti-pattern. Migration to Order `effective_fee_rate` pre-resolved at submit; OMS scalar field DELETED at WIP2d-1.B.1. | `WIP2d-1.B.1` (this ship) — delete OMS scalar field; remove this exemption in same commit |
| `OrderManagerState` | `fee_rate_taker` | TRANSITIONAL | Scalar cfg-mirror of `cfg.cores[c].fee_rate_taker`; Class 27 anti-pattern. Same migration as fee_rate_maker. | `WIP2d-1.B.1` (this ship) — delete OMS scalar field; remove this exemption in same commit |
| `OrderManagerState` | `slippage_pct` | TRANSITIONAL | Scalar cfg-mirror of `cfg.cores[c].slippage_pct`; Class 27 anti-pattern. Migration to `Order::pre_resolved.slippage_pct` (OrderPreResolved sub-struct) pre-resolved at submit via Order_BindPreResolved; OMS scalar field DELETED at WIP2d-1.B.1 (folded into B.1 cohort per 2026-05-15 design decision — sub-struct closes entire OMS Class 27 cluster in one commit). | `WIP2d-1.B.1` (this ship) — delete OMS scalar field alongside fee_rate cluster; remove this exemption in same commit |

**Section C total: 4 entries (all TRANSITIONAL).** Inventory zeros out at WIP2d-1.B.1 (all 4 OMS entries close in one commit via OrderPreResolved sub-struct; B.1.b cohort sweep continues with ConfidenceScorer / PortfolioController / other subsystem instances).

**Rationale categories** (one required per exemption):
- `pre-resolve-impossible` — no in-flight object exists at the decision point (rare; document why genuinely impossible)
- `subsystem-internal-aggregate` — value is not a cfg mirror; computed/aggregated from runtime state (false-positive on name match)
- `uniform-by-design` — cfg value is engine-wide by design (global cfg field); shouldn't be on subsystem state either but legacy
- `TRANSITIONAL` — migration in flight; specify ship that completes the closure

Adding an exemption:
1. Add row to this table with full justification
2. Commit message explicitly cites the rationale category
3. If TRANSITIONAL: link the migration plan + target ship
4. CI Check 7 picks up the exemption automatically (parses Section C)

Removing an exemption:
1. Confirm migration to first-line or second-line landed
2. Delete row
3. CI re-runs; build verifies no stray subsystem-state cfg-mirror remains

## Cross-references

- `tick-trader-percore-workspace/DESIGN_SPECS/manual-fields-inventory-pattern.md` — the pattern doc (Stage 2 DRAFT at this ship; Stage 3 ACTIVE at `.F.4c.3` close)
- `tick-trader-percore-workspace/DESIGN_SPECS/cfg-scope-discipline.md` § Anti-pattern 2 — discipline closure (Section A + B)
- `tick-trader-percore-workspace/DESIGN_SPECS/decision-time-data-binding-pattern.md` — Section C principle (Class 27 closure)
- `tick-trader-percore-workspace/DESIGN_SPECS/per-instance-registry-pattern.md` — the framework Section A + B primitive enforces
- `tick-trader-percore-workspace/DOCS/DESIGN_PHILOSOPHY.md` § 2 H17 — STRONG at `.F.4c.3` (per-core surface); HARD at `.F.4d` (full cfg surface)
- `tick-trader-percore-workspace/DOCS/RECURRING_BUG_PATTERNS.md` Class 27 — Section C closure target
- `CoreFrameworks/CfgFieldRegistry.hpp` — `FOREACH_PER_CORE_FIELD_TYPE` + `FOREACH_MANUAL_PER_CORE_FIELD` X-macros
- `tools/check_per_core_registry_integrity.py` — CI cross-check enforcement (Section A + B = Checks 1-6; Section C = Check 7)

---

**Maintained at every per-core cfg field addition + every subsystem-state-cfg-mirror exemption. CI build-fails on drift between this file + FOREACH_MANUAL_PER_CORE_FIELD X-macro + FOREACH_PER_CORE_FIELD_TYPE X-macro + PerCoreCfg<F> body + designated subsystem state types.**
