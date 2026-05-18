#!/usr/bin/env python3
"""check_storage_t_coverage.py — STORAGE_T variant coverage in tt:: dispatch.

Implements meta-discipline M4 / Pillar B6 from DESIGN_SPECS/implementation-layer-blindspot-taxonomy.md.

When master cfg registries add a NEW STORAGE_T variant (e.g., `char[N]` for KIND_STRING),
consumer template fns `tt::cfg_*_field<T>` must have a branch covering the new variant —
otherwise X-macro walker hits the row and compile fails at the missing branch.

This script verifies that every STORAGE_T variant used in FOREACH_PER_CORE_CFG_FIELD +
FOREACH_GLOBAL_CFG_FIELD has a covering branch in CfgFieldDispatch.hpp's tt:: family.

Checks performed:
  1. Extract all unique STORAGE_T variants from master cfg registries (col 0 of each X-macro row)
  2. For each variant, verify presence of `if constexpr (std::is_same_v<T, <variant>>` or
     equivalent type-trait branch in CfgFieldDispatch.hpp's template fns
  3. Report uncovered variants

Exit codes:
  0 = every STORAGE_T variant in master registries has a covering branch
  1 = at least one variant is uncovered
  2 = script error / file missing

Cross-references:
  - DESIGN_SPECS/implementation-layer-blindspot-taxonomy.md § B6
  - DESIGN_PHILOSOPHY.md § 11.5 meta-discipline M4
  - claude-skills/blindspot-scan/SKILL.md Pillar B6
  - CoreFrameworks/CfgFieldDispatch.hpp — tt:: family
"""

import re
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT  = SCRIPT_DIR.parent

REGISTRY_FILES = [
    (REPO_ROOT / "CoreFrameworks/CfgFieldRegistry.hpp", "FOREACH_PER_CORE_CFG_FIELD"),
    (REPO_ROOT / "CoreFrameworks/CfgFieldRegistry.hpp", "FOREACH_GLOBAL_CFG_FIELD"),
]

DISPATCH_FILE = REPO_ROOT / "CoreFrameworks/CfgFieldDispatch.hpp"


def extract_storage_t_variants(file_path, foreach_name):
    """Walk file_path; extract STORAGE_T variants (col 0 of FOREACH_<foreach_name> rows)."""
    if not file_path.exists():
        print(f"[FAIL] registry file missing: {file_path}", file=sys.stderr)
        return None

    variants = set()
    in_macro = False
    define_pat = re.compile(rf"^#define\s+{foreach_name}\s*\(X\)")

    with file_path.open() as f:
        for line in f:
            if define_pat.match(line):
                in_macro = True
                continue
            if in_macro:
                stripped = line.strip()
                if not stripped.endswith("\\") and not stripped.startswith("X("):
                    if stripped == "" or stripped.startswith("//"):
                        continue
                    in_macro = False
                    continue
                if stripped.startswith("X("):
                    m = re.match(r"X\s*\(([^,]+),", stripped)
                    if m:
                        variants.add(m.group(1).strip())

    return variants


def variant_has_branch(variant, dispatch_text):
    """Heuristic: check if dispatch_text contains a type-trait branch covering `variant`.

    Looks for patterns like:
      - `std::is_same_v<T, <variant>>`
      - `is_FPN_v<T>`  (for FPN<F=64>)
      - `std::is_integral_v<T>`  (for int / uint{8,16,32,64}_t)
      - Direct mention of <variant> in if constexpr context
    """
    # FPN<F> family
    if variant.startswith("FPN<"):
        return ("is_FPN_v<T>" in dispatch_text) or ("std::is_same_v<T, FPN" in dispatch_text)

    # Bool family (int / uint8_t for KIND_BOOL per H13)
    # Catch-all for integer widths
    if variant in ("int", "uint8_t", "uint16_t", "uint32_t", "uint64_t",
                   "int8_t", "int16_t", "int32_t", "int64_t"):
        # Integer dispatch usually via std::is_integral_v or explicit per-type
        if "std::is_integral_v<T>" in dispatch_text:
            return True
        return f"std::is_same_v<T, {variant}>" in dispatch_text or f", {variant}>" in dispatch_text

    # char[N] family (KIND_STRING; future)
    if variant.startswith("char[") or "[]" in variant or "[N]" in variant:
        return ("std::is_array_v<T>" in dispatch_text) or ("char[" in dispatch_text)

    # Direct match fallback
    return variant in dispatch_text


def main():
    print("[storage-t-coverage-CI] scanning master cfg registries for STORAGE_T variants...")
    all_variants = set()
    for file_path, foreach_name in REGISTRY_FILES:
        variants = extract_storage_t_variants(file_path, foreach_name)
        if variants is None:
            return 2
        print(f"  {foreach_name}: {len(variants)} unique variants — {sorted(variants)}")
        all_variants |= variants

    if not DISPATCH_FILE.exists():
        print(f"[FAIL] dispatch file missing: {DISPATCH_FILE}", file=sys.stderr)
        return 2

    dispatch_text = DISPATCH_FILE.read_text()

    uncovered = []
    for variant in sorted(all_variants):
        if not variant_has_branch(variant, dispatch_text):
            uncovered.append(variant)

    if uncovered:
        print("[storage-t-coverage-CI] FAIL: STORAGE_T variants without tt:: branch:")
        for v in uncovered:
            print(f"  {v}")
        print()
        print("Resolution per DESIGN_SPECS/implementation-layer-blindspot-taxonomy.md § B6:")
        print("  - Add `if constexpr (std::is_same_v<T, <variant>>)` branch to:")
        print("    tt::cfg_parse_field<T> / tt::cfg_emit_field<T> / tt::cfg_drift_compare<T> / tt::cfg_set_field<T>")
        print("  - Each tt:: family member MUST cover the new variant; X-macro walker will hit it")
        return 1

    print(f"[storage-t-coverage-CI] PASS: all {len(all_variants)} STORAGE_T variants covered in tt:: family")
    print("[storage-t-coverage-CI] meta-discipline M4 / Pillar B6 verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
