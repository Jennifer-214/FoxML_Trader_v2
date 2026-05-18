#!/usr/bin/env python3
"""check_struct_field_uniqueness.py — cross-walker struct-field uniqueness check.

Implements meta-discipline M4 / Pillar B13 (codified .B.3 v1.12 mid-coding) from
DESIGN_SPECS/implementation-layer-blindspot-taxonomy.md +
DESIGN_SPECS/cross-walker-struct-field-uniqueness-discipline.md.

When MULTIPLE X-macro walkers generate struct fields on the SAME struct (e.g.,
ModelStampResult holds fields from BOTH FOREACH_STAMP_BOUND_MODEL_CONST AND
the 4 master cfg registries via STAMP_RESULT_DERIVED_FIELDS_AUTO_GEN), name
collision across walkers produces duplicate member declarations → compile error.

EXTENDS check_field_name_uniqueness.py (which only covered 4 master cfg registries)
to also scan FOREACH_STAMP_BOUND_MODEL_CONST + future struct-generating registries.

Discipline: any name appearing in both master cfg registries AND
FOREACH_STAMP_BOUND_MODEL_CONST MUST appear in
FOREACH_STAMP_RESULT_FIELD_EXCLUSION sidecar (per H18 SIDECAR pattern).
Failure to register collision in sidecar = CI fail.

Checks performed:
  1. Extract field-name set from each cfg + MODEL_CONST registry
  2. Compute pairwise intersection across walker pairs
  3. Verify any collision is registered in FOREACH_STAMP_RESULT_FIELD_EXCLUSION
  4. Report unregistered collisions as failures

Exit codes:
  0 = all collisions registered in sidecar (or no collisions)
  1 = at least one collision missing from sidecar
  2 = script error / file missing

Cross-references:
  - DESIGN_SPECS/implementation-layer-blindspot-taxonomy.md § B13
  - DESIGN_SPECS/cross-walker-struct-field-uniqueness-discipline.md
  - DESIGN_PHILOSOPHY.md § 11.5 meta-discipline M4
  - claude-skills/blindspot-scan/SKILL.md Pillar B13 (extends B2)
  - MemHeaders/CfgGateRegistry.hpp FOREACH_STAMP_RESULT_FIELD_EXCLUSION sidecar
"""

import re
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT  = SCRIPT_DIR.parent

# Master cfg registries that generate fields on ModelStampResult / StampInferenceCfgInputs
MASTER_REGISTRIES = {
    "FOREACH_PER_CORE_CFG_FIELD": (
        REPO_ROOT / "CoreFrameworks/CfgFieldRegistry.hpp", 2),
    "FOREACH_GLOBAL_CFG_FIELD": (
        REPO_ROOT / "CoreFrameworks/CfgFieldRegistry.hpp", 2),
    "FOREACH_ML_CFG_FLAG": (
        REPO_ROOT / "ML_Headers/MlCfgFlagRegistry.hpp", 1),
    "FOREACH_GATE_CFG_FLAG": (
        REPO_ROOT / "CoreFrameworks/GateCfgFlagRegistry.hpp", 1),
}

# Other struct-generating registries that also contribute fields to the same structs.
# FOREACH_STAMP_BOUND_MODEL_CONST is a UNION macro of PRE_CFG + POST_CFG sub-registries —
# scan PRE_CFG + POST_CFG separately to extract individual row names.
OTHER_STRUCT_REGISTRIES = {
    "FOREACH_STAMP_BOUND_MODEL_CONST_PRE_CFG": (
        REPO_ROOT / "ML_Headers/StampBoundModelConstRegistry.hpp", 0),
    "FOREACH_STAMP_BOUND_MODEL_CONST_POST_CFG": (
        REPO_ROOT / "ML_Headers/StampBoundModelConstRegistry.hpp", 0),
}

# Sidecar that registers known collisions (per H18 SIDECAR pattern)
EXCLUSION_SIDECAR_FILE = REPO_ROOT / "MemHeaders/CfgGateRegistry.hpp"
EXCLUSION_MACRO_NAME = "FOREACH_STAMP_RESULT_FIELD_EXCLUSION"


def extract_registry_fields(file_path, foreach_name, name_col_idx):
    """Walk file; collect comma-separated args from each X(...) row.

    Robust to multi-line rows continued via `\\`: accumulates lines until balanced ),
    then splits args + extracts col N.
    """
    if not file_path.exists():
        print(f"[FAIL] registry file missing: {file_path}", file=sys.stderr)
        return None

    fields = set()
    in_macro = False
    define_pat = re.compile(rf"^#define\s+{foreach_name}\s*\(X\)")
    row_pat = re.compile(r"X\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*[,)]")

    with file_path.open() as f:
        for line in f:
            if define_pat.match(line):
                in_macro = True
                continue
            if in_macro:
                stripped = line.rstrip("\n").rstrip()
                # End of macro: line without trailing \ AND not part of multi-line X(...)
                if not stripped.endswith("\\") and stripped.strip() and not stripped.lstrip().startswith(("X(", "/*", "//")):
                    in_macro = False
                    continue
                # Match X(identifier, or X(identifier) — extract first identifier (col 0 only)
                m = row_pat.search(stripped)
                if m:
                    if name_col_idx == 0:
                        fields.add(m.group(1))
                    else:
                        # For col_idx > 0, fall back to full-line comma-split (best-effort for single-line rows)
                        body_match = re.search(r"X\s*\(([^)]+)", stripped)
                        if body_match:
                            args = [a.strip() for a in body_match.group(1).split(",")]
                            if len(args) > name_col_idx:
                                fields.add(args[name_col_idx])
    return fields


def extract_exclusion_sidecar():
    """Extract names from FOREACH_STAMP_RESULT_FIELD_EXCLUSION(X) macro body."""
    if not EXCLUSION_SIDECAR_FILE.exists():
        return set()

    fields = set()
    in_macro = False
    define_pat = re.compile(rf"^#define\s+{EXCLUSION_MACRO_NAME}\s*\(X\)")
    row_pat = re.compile(r"\s*X\(([a-z_][a-z0-9_]*)\)")

    with EXCLUSION_SIDECAR_FILE.open() as f:
        for line in f:
            if define_pat.match(line):
                in_macro = True
                continue
            if in_macro:
                stripped = line.strip().rstrip("\\").strip()
                if stripped == "" or stripped.startswith("//"):
                    continue
                m = row_pat.match(stripped)
                if m:
                    fields.add(m.group(1))
                else:
                    in_macro = False

    return fields


def main():
    print("[struct-field-uniqueness-CI] scanning cross-walker struct-field uniqueness...")

    master_fields_all = set()
    for reg_name, (file_path, name_col_idx) in MASTER_REGISTRIES.items():
        fields = extract_registry_fields(file_path, reg_name, name_col_idx)
        if fields is None:
            return 2
        master_fields_all |= fields
        print(f"  {reg_name}: {len(fields)} fields")

    other_fields_all = set()
    for reg_name, (file_path, name_col_idx) in OTHER_STRUCT_REGISTRIES.items():
        fields = extract_registry_fields(file_path, reg_name, name_col_idx)
        if fields is None:
            return 2
        other_fields_all |= fields
        print(f"  {reg_name}: {len(fields)} fields")

    # Compute cross-walker collisions
    collisions = master_fields_all & other_fields_all
    print(f"  Collisions across walkers: {len(collisions)}")

    if not collisions:
        print(f"[struct-field-uniqueness-CI] PASS: no cross-walker collisions detected")
        print("[struct-field-uniqueness-CI] meta-discipline M4 / Pillar B13 verified")
        return 0

    # Verify each collision is in the exclusion sidecar
    sidecar = extract_exclusion_sidecar()
    print(f"  FOREACH_STAMP_RESULT_FIELD_EXCLUSION sidecar entries: {len(sidecar)}")

    unregistered = collisions - sidecar
    if unregistered:
        print("[struct-field-uniqueness-CI] FAIL: cross-walker collisions NOT in exclusion sidecar:")
        for name in sorted(unregistered):
            print(f"  {name}")
        print()
        print("Resolution per DESIGN_SPECS/cross-walker-struct-field-uniqueness-discipline.md:")
        print(f"  1. Add `X({{name}})` to FOREACH_STAMP_RESULT_FIELD_EXCLUSION at {EXCLUSION_SIDECAR_FILE}")
        print("  2. Add #define/#undef redirect bracket at struct site (ModelStampResult + StampInferenceCfgInputs)")
        print("  3. Per H18 SIDECAR pattern — sparse exclusion list maintains discipline")
        return 1

    # Optional: report sidecar entries that are NOT actually collisions (drift indicator)
    stale_sidecar = sidecar - collisions
    if stale_sidecar:
        print("[struct-field-uniqueness-CI] WARN: sidecar entries no longer collide (drift):")
        for name in sorted(stale_sidecar):
            print(f"  {name}  (consider removing from sidecar)")

    print(f"[struct-field-uniqueness-CI] PASS: {len(collisions)} cross-walker collisions all registered in sidecar")
    print("[struct-field-uniqueness-CI] meta-discipline M4 / Pillar B13 verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
