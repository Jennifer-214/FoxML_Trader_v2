#!/usr/bin/env python3
"""check_field_name_uniqueness.py — cross-registry field-name uniqueness check.

Implements meta-discipline M4 / Pillar B2 from DESIGN_SPECS/implementation-layer-blindspot-taxonomy.md.

When struct-gen migrations unconditionally walk multiple heterogeneous registries
(e.g., FOREACH_PER_CORE_CFG_FIELD + FOREACH_GLOBAL_CFG_FIELD + FOREACH_ML_CFG_FLAG +
FOREACH_GATE_CFG_FLAG) and emit one struct field per row, name collision across
registries produces duplicate field declarations → compile error.

Verifies field-name uniqueness across the 4 currently-walked registries.

Checks performed:
  1. Extract field-name set from each registry (per-core + global = `name` col;
     ml/gate cfg_flag = `legacy_field` col)
  2. Compute pairwise intersection across all registry pairs
  3. Emit collision report; nonzero exit on any non-empty intersection

Exit codes:
  0 = all field names unique across all registries
  1 = at least one collision detected
  2 = script error / registry file missing

Cross-references:
  - DESIGN_SPECS/implementation-layer-blindspot-taxonomy.md § B2
  - DESIGN_PHILOSOPHY.md § 11.5 meta-discipline M4
  - claude-skills/blindspot-scan/SKILL.md Pillar B2
"""

import re
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT  = SCRIPT_DIR.parent

REGISTRIES = {
    # registry_name: (file_path, foreach_macro_name, name_col_index, name_col_label)
    "FOREACH_PER_CORE_CFG_FIELD": (
        REPO_ROOT / "CoreFrameworks/CfgFieldRegistry.hpp",
        "FOREACH_PER_CORE_CFG_FIELD",
        2,  # X(STORAGE_T, KIND_TOKEN, name, label, section, meta, payload, tooltip, applies_strat, applies_op, applies_regime, applies_risk, lives_in_struct) — name at idx 2
        "name (col 3)",
    ),
    "FOREACH_GLOBAL_CFG_FIELD": (
        REPO_ROOT / "CoreFrameworks/CfgFieldRegistry.hpp",
        "FOREACH_GLOBAL_CFG_FIELD",
        2,  # same 13-col sig
        "name (col 3)",
    ),
    "FOREACH_ML_CFG_FLAG": (
        REPO_ROOT / "ML_Headers/MlCfgFlagRegistry.hpp",
        "FOREACH_ML_CFG_FLAG",
        1,  # X(NAME, legacy_field, display_label, section, metadata_flags, doc) — legacy_field at idx 1
        "legacy_field (col 2)",
    ),
    "FOREACH_GATE_CFG_FLAG": (
        REPO_ROOT / "CoreFrameworks/GateCfgFlagRegistry.hpp",
        "FOREACH_GATE_CFG_FLAG",
        1,  # same 6-col sig
        "legacy_field (col 2)",
    ),
}


def extract_row_field(line, name_col_idx):
    """Extract field name from a single X(...) row line; returns name or None."""
    m = re.match(r"\s*X\s*\(([^)]+)\)", line)
    if not m:
        return None
    args = [a.strip() for a in m.group(1).split(",")]
    if len(args) <= name_col_idx:
        return None
    return args[name_col_idx]


def extract_registry_fields(file_path, foreach_name, name_col_idx):
    """Walk file_path; extract field names from FOREACH_<foreach_name> rows."""
    if not file_path.exists():
        print(f"[FAIL] registry file missing: {file_path}", file=sys.stderr)
        return None

    fields = set()
    in_macro = False
    define_pat = re.compile(rf"^#define\s+{foreach_name}\s*\(X\)")

    with file_path.open() as f:
        for line in f:
            if define_pat.match(line):
                in_macro = True
                continue
            if in_macro:
                stripped = line.strip()
                # End of macro definition (line without trailing backslash + not X(...))
                if not stripped.endswith("\\") and not stripped.startswith("X("):
                    if stripped == "" or stripped.startswith("//"):
                        continue
                    in_macro = False
                    continue
                if stripped.startswith("X(") or stripped.startswith("/* ") or stripped.startswith("//"):
                    name = extract_row_field(stripped.rstrip("\\").strip(), name_col_idx)
                    if name:
                        fields.add(name)

    return fields


def main():
    print("[field-name-uniqueness-CI] scanning 4 registries for cross-registry name collision...")
    registry_fields = {}
    for reg_name, (file_path, foreach_name, name_col_idx, _label) in REGISTRIES.items():
        fields = extract_registry_fields(file_path, foreach_name, name_col_idx)
        if fields is None:
            return 2
        registry_fields[reg_name] = fields
        print(f"  {reg_name}: {len(fields)} fields")

    # Pairwise intersection check
    reg_names = list(registry_fields.keys())
    collisions = []
    for i in range(len(reg_names)):
        for j in range(i + 1, len(reg_names)):
            a, b = reg_names[i], reg_names[j]
            shared = registry_fields[a] & registry_fields[b]
            if shared:
                collisions.append((a, b, sorted(shared)))

    if collisions:
        print("[field-name-uniqueness-CI] FAIL: cross-registry name collisions detected")
        for a, b, shared in collisions:
            print(f"  {a} ∩ {b}: {shared}")
        print()
        print("Resolution per DESIGN_SPECS/implementation-layer-blindspot-taxonomy.md § B2:")
        print("  - Determine whether collision is intentional sister (rename one) or accidental typo")
        print("  - Unconditional struct-gen across registries produces duplicate field decls → compile error")
        return 1

    total_unique = sum(len(s) for s in registry_fields.values())
    print(f"[field-name-uniqueness-CI] PASS: {total_unique} fields unique across all 4 registries")
    print("[field-name-uniqueness-CI] meta-discipline M4 / Pillar B2 verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
