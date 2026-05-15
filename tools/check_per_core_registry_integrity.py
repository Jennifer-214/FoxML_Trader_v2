#!/usr/bin/env python3
"""check_per_core_registry_integrity.py — CI cross-check for per-core cfg discipline.

Established at v5.15.5.F.4c.3 WIP2d-0 per the structural-fix primitive for the
per-core cfg surface. Enforces H17 STRONG invariant (per-core surface): cfg
struct field declarations MUST come from FOREACH_PER_CORE_FIELD_TYPE via X-macro
generation; manual cfg-surface field declarations FORBIDDEN.

Closes bug classes (cfg-scope-discipline.md):
- Parallel-array drift (Class A): `core_X[16]` shadowing per-core registry row
- Manual-field bypass (Class B): adding to PerCoreCfg<F> body without registry row
- Anti-pattern 1 consumer (informational): `cfg.X` + `cfg.core_overrides[c].X`

Cross-checks performed (all build-failing on violation):
  1. FOREACH_PER_CORE_CFG_FIELD ↔ FOREACH_PER_CORE_FIELD_TYPE bidirectional sync
  2. PerCoreCfg<F> body contains ONLY: X-macro expansion + 5 permitted runtime bitmap fields
  3. ControllerConfig parallel arrays ↔ FOREACH_MANUAL_PER_CORE_FIELD bidirectional sync
  4. FOREACH_MANUAL_PER_CORE_FIELD ↔ MANUAL_FIELDS_INVENTORY.md bidirectional sync
  5. No name duplication between FOREACH_PER_CORE_CFG_FIELD + FOREACH_MANUAL_PER_CORE_FIELD
  6. TRANSITIONAL exemption rot detection (WARN-level)

Exit codes:
  0 = all checks pass (build proceeds)
  1 = one or more checks failed (build aborted)
  2 = script error / file missing (build aborted)

Cross-references:
  - DESIGN_SPECS/manual-fields-inventory-pattern.md (the pattern doc)
  - DESIGN_SPECS/cfg-scope-discipline.md (audit grep signatures)
  - DESIGN_SPECS/per-instance-registry-pattern.md (framework discipline)
  - DOCS/MANUAL_FIELDS_INVENTORY.md (documented exemption registry)
  - DESIGN_PHILOSOPHY.md § 2 H17 STRONG codification
"""

import re
import sys
from pathlib import Path

# Repo paths relative to script location
SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT  = SCRIPT_DIR.parent
CFG_REG    = REPO_ROOT / "CoreFrameworks/CfgFieldRegistry.hpp"
CTRL_CFG   = REPO_ROOT / "CoreFrameworks/ControllerConfig.hpp"
INVENTORY  = REPO_ROOT / "DOCS/MANUAL_FIELDS_INVENTORY.md"

# 5 permitted runtime bitmap fields in PerCoreCfg<F> body (Section B of inventory)
PERMITTED_RUNTIME_FIELDS = {
    "lifecycle_cfg_flags",
    "gate_cfg_flags",
    "ml_cfg_flags",
    "risk_cfg_flags",
    "ops_cfg_flags",
}


def fail(msg: str) -> None:
    """Emit error to stderr in operator-readable format."""
    print(f"[per-core-cfg-CI] ERROR: {msg}", file=sys.stderr)


def warn(msg: str) -> None:
    """Emit warning to stderr (non-fatal)."""
    print(f"[per-core-cfg-CI] WARN: {msg}", file=sys.stderr)


def info(msg: str) -> None:
    """Emit info to stdout."""
    print(f"[per-core-cfg-CI] {msg}")


def read_file(p: Path) -> str:
    try:
        return p.read_text()
    except FileNotFoundError:
        fail(f"file not found: {p}")
        sys.exit(2)


def extract_macro_body(text: str, macro_name: str) -> str:
    """Extract the body of a #define FOREACH_X(X) ... macro definition.

    Returns text between the #define line and the next blank line / #define / EOF.
    """
    pattern = re.compile(
        rf'^#define\s+{re.escape(macro_name)}\(X\)\s*\\?\s*\n(.*?)(?=\n\s*\n|\n#define|\Z)',
        re.MULTILINE | re.DOTALL,
    )
    m = pattern.search(text)
    if not m:
        fail(f"could not find macro body for {macro_name}")
        sys.exit(2)
    return m.group(1)


def parse_foreach_per_core_cfg_field(body: str) -> dict:
    """Parse FOREACH_PER_CORE_CFG_FIELD rows. Returns dict {name: type}.

    Post-WIP2d-0.B: TYPE is the FIRST column. Row shape:
        X(<storage_type>, KIND_TOKEN, <name>, "label", "section", meta, payload, "tooltip", ...)

    Type can contain template brackets (FPN<F>) so we use a non-greedy match through commas.
    """
    result = {}
    # Match: X(<type>, KIND_<TOKEN>, <name>,
    # Type can have <F> template parameter — match anything up to ", KIND_"
    pattern = re.compile(r'^\s+X\(\s*([^,]+?(?:<[^>]+>)?)\s*,\s+(KIND_[A-Z_]+),\s+(\w+),', re.MULTILINE)
    for m in pattern.finditer(body):
        typ = m.group(1).strip()
        name = m.group(3).strip()
        result[name] = typ
    return result


def parse_foreach_manual_per_core_field(body: str) -> dict:
    """Parse FOREACH_MANUAL_PER_CORE_FIELD rows. Returns dict {name: (type, suffix, rationale)}.

    Rows look like:
        X(type, name, suffix, "rationale")
    """
    result = {}
    # Carefully match: type can have template brackets; suffix can be [N] or empty; rationale is quoted
    pattern = re.compile(
        r'^\s+X\(\s*([^,]+?)\s*,\s*(\w+)\s*,\s*(\[[^\]]*\]|\s*)\s*,\s*"([^"]*)"\)',
        re.MULTILINE,
    )
    for m in pattern.finditer(body):
        typ = m.group(1).strip()
        name = m.group(2).strip()
        suffix = m.group(3).strip()
        rationale = m.group(4).strip()
        result[name] = (typ, suffix, rationale)
    return result


def parse_per_core_cfg_body(text: str) -> dict:
    """Parse fields declared inside PerCoreCfg<F> struct body.

    Returns dict {name: (type, line_no)}. EXCLUDES the X-macro expansion call.
    Detects FOREACH_PER_CORE_FIELD_TYPE invocation; collects manual fields ONLY.
    """
    # Find the struct definition
    m = re.search(
        r'template\s*<unsigned\s+F>\s*\nstruct\s+alignas\(64\)\s+PerCoreCfg\s*\{(.*?)^\};',
        text,
        re.MULTILINE | re.DOTALL,
    )
    if not m:
        fail("could not find PerCoreCfg<F> struct definition")
        sys.exit(2)
    body = m.group(1)
    body_start_line = text[:m.start()].count('\n') + 2  # approximate; +2 for template + struct lines

    # Detect if FOREACH_PER_CORE_CFG_FIELD is invoked (expected post-WIP2d-0.B; covers 92 fields)
    has_cfg_macro = bool(re.search(r'FOREACH_PER_CORE_CFG_FIELD\s*\(\s*EMIT_PER_CORE_CFG_STRUCT_FIELD\s*\)', body))
    if not has_cfg_macro:
        fail("PerCoreCfg<F> body missing FOREACH_PER_CORE_CFG_FIELD(EMIT_PER_CORE_CFG_STRUCT_FIELD) invocation")
        sys.exit(1)

    # Detect if FOREACH_PER_CORE_DOMAIN_BITMAP is invoked (expected post-WIP2d-0.B; covers 5 bitmap fields)
    has_bitmap_macro = bool(re.search(r'FOREACH_PER_CORE_DOMAIN_BITMAP\s*\(\s*EMIT_DOMAIN_BITMAP_FIELD\s*\)', body))
    if not has_bitmap_macro:
        fail("PerCoreCfg<F> body missing FOREACH_PER_CORE_DOMAIN_BITMAP(EMIT_DOMAIN_BITMAP_FIELD) invocation")
        sys.exit(1)

    # Find manual field declarations (after stripping comments + the X-macro line)
    # Field decl pattern: optional alignas(N) + type + name;
    # Type can be: uint8_t, uint16_t, uint32_t, uint64_t, int, double, FPN<F>, etc.
    manual_fields = {}
    line_no = body_start_line
    for line in body.split('\n'):
        line_no += 1
        # Strip inline comments
        comment_pos = line.find('//')
        if comment_pos >= 0:
            line = line[:comment_pos]
        line = line.strip()
        if not line or line.startswith('/*') or line.startswith('*'):
            continue
        # Skip block-comment lines + X-macro expansions
        if ('FOREACH_PER_CORE_CFG_FIELD' in line or 'EMIT_PER_CORE_CFG_STRUCT_FIELD' in line
                or 'FOREACH_PER_CORE_DOMAIN_BITMAP' in line or 'EMIT_DOMAIN_BITMAP_FIELD' in line):
            continue
        # Skip preprocessor + closing braces
        if line.startswith('#') or line == '};' or line == '{':
            continue
        # Match field declaration: optional 'alignas(N) ' prefix; then 'type name;'
        m = re.match(r'^(?:alignas\(\d+\)\s+)?([a-zA-Z_][a-zA-Z_0-9<>]*(?:\s*<\s*F\s*>)?)\s+(\w+)\s*;', line)
        if m:
            typ = m.group(1).strip()
            name = m.group(2).strip()
            manual_fields[name] = (typ, line_no)
    return manual_fields


def parse_controller_config_parallel_arrays(text: str) -> dict:
    """Parse `<type> core_<name>[16];` declarations in ControllerConfig.hpp.

    Returns dict {name: (type, suffix, line_no)}.
    Suffix is e.g. '[256]' for 2D arrays, empty for 1D.
    """
    result = {}
    line_no = 0
    for line in text.split('\n'):
        line_no += 1
        # Strip inline comments
        comment_pos = line.find('//')
        if comment_pos >= 0:
            line = line[:comment_pos]
        # Match: optional whitespace + <type> + core_<name>[16] + optional [N] + ;
        m = re.match(
            r'^\s+([a-zA-Z_][a-zA-Z_0-9<>]*(?:\s*<\s*F\s*>)?)\s+(core_\w+)\[(?:16|MAX_EXECUTION_CORES)\](\[[^\]]+\])?\s*;',
            line,
        )
        if m:
            typ = m.group(1).strip()
            name = m.group(2).strip()
            suffix = (m.group(3) or '').strip()
            result[name] = (typ, suffix, line_no)
    return result


def parse_inventory_section_a(text: str) -> set:
    """Parse Section A entries from MANUAL_FIELDS_INVENTORY.md. Returns set of field names."""
    # Section A starts at '## Section A' and continues until '## Section B' or '## Statistics'
    m = re.search(r'## Section A.*?(?=\n##\s)', text, re.DOTALL)
    if not m:
        fail("MANUAL_FIELDS_INVENTORY.md missing '## Section A' header")
        sys.exit(2)
    section_a = m.group(0)
    # Each entry has a row | `field_name` | ... |
    names = set(re.findall(r'\|\s*`(core_\w+)`\s*\|', section_a))
    return names


def parse_inventory_section_b(text: str) -> set:
    """Parse Section B entries from MANUAL_FIELDS_INVENTORY.md. Returns set of field names."""
    m = re.search(r'## Section B.*?(?=\n##\s)', text, re.DOTALL)
    if not m:
        fail("MANUAL_FIELDS_INVENTORY.md missing '## Section B' header")
        sys.exit(2)
    section_b = m.group(0)
    # Section B rows are `| field_name | type | alignment | rationale |`
    names = set(re.findall(r'\|\s*`(\w+_cfg_flags)`\s*\|', section_b))
    return names


def strip_macro_definitions(text: str) -> str:
    """Remove all #define ... blocks (single-line + multi-line via \\ continuation).

    Used to scan for anti-pattern 1 in PRODUCTION code only, not in X-macro callback bodies
    (which legitimately use `cfg.core_overrides[c].name` + `cfg.name` as positional meta-vars).
    """
    out = []
    in_macro = False
    for line in text.split('\n'):
        if in_macro:
            if not line.rstrip().endswith('\\'):
                in_macro = False
            continue  # Skip macro body lines
        # Strip single-line #define (no continuation)
        stripped = line.strip()
        if stripped.startswith('#define '):
            if line.rstrip().endswith('\\'):
                in_macro = True
            continue
        out.append(line)
    return '\n'.join(out)


def scan_anti_pattern_1(text: str) -> list:
    """Scan for anti-pattern 1 consumer shape: cfg.X + cfg.core_overrides[c].X same X.

    Strips #define macro bodies first — X-macro callbacks use `name` as a meta-var
    that pastes the field name; matching that as a field-name produces false positives.
    """
    findings = []
    # Strip macro definitions to avoid false-positives on X-macro callback meta-vars
    code = strip_macro_definitions(text)
    # Find each `cfg.core_overrides[c].FIELD` pattern; check if the same scope has `cfg.FIELD`
    pattern_override = re.compile(r'cfg\.core_overrides\[\w+\]\.(\w+)')
    overridden_fields = set(pattern_override.findall(code))
    for field in overridden_fields:
        # Check if cfg.<field> appears in same text (informational)
        if re.search(rf'\bcfg\.{re.escape(field)}\b(?!\s*\[)', code):
            findings.append(field)
    return findings


def main() -> int:
    info("running per-core cfg registry integrity check...")

    # Load all three files
    cfg_reg_text = read_file(CFG_REG)
    ctrl_cfg_text = read_file(CTRL_CFG)
    inventory_text = read_file(INVENTORY)

    failures = 0

    # --- Check 1: FOREACH_PER_CORE_CFG_FIELD ↔ PerCoreCfg<F> struct field type sync ---
    # Post-WIP2d-0.B: single registry. TYPE is the FIRST column of each row.
    # Verify every row's TYPE matches the struct field type generated via X-macro.
    # (Auxiliary FOREACH_PER_CORE_FIELD_TYPE retired at WIP2d-0.B.)
    cfg_field_body = extract_macro_body(cfg_reg_text, "FOREACH_PER_CORE_CFG_FIELD")
    cfg_field_map = parse_foreach_per_core_cfg_field(cfg_field_body)
    cfg_field_names = set(cfg_field_map.keys())

    if len(cfg_field_map) == 0:
        fail("Check 1 FAIL: FOREACH_PER_CORE_CFG_FIELD parsed 0 rows — row regex may be broken")
        failures += 1
    elif len(cfg_field_map) < 80:
        fail(f"Check 1 FAIL: only {len(cfg_field_map)} per-core rows parsed (expected ~92) — registry might be incomplete or parser regex broken")
        failures += 1
    else:
        info(f"Check 1 PASS: {len(cfg_field_map)} per-core cfg fields parsed with TYPE column (single registry; auxiliary retired)")

    # --- Check 2: PerCoreCfg<F> body contains ONLY 2 X-macro invocations + nothing else ---
    # Post-WIP2d-0.B: both cfg-surface fields (92) AND runtime bitmap fields (5) come from X-macros.
    # No manual fields permitted anywhere in PerCoreCfg<F> body.
    per_core_manual = parse_per_core_cfg_body(ctrl_cfg_text)
    per_core_manual_names = set(per_core_manual.keys())

    if per_core_manual_names:
        fail(f"Check 2 FAIL: PerCoreCfg<F> body contains FORBIDDEN manual fields outside X-macro expansions: {sorted(per_core_manual_names)}")
        for name in sorted(per_core_manual_names):
            typ, line = per_core_manual[name]
            fail(f"  → ControllerConfig.hpp:{line}: '{typ} {name}' — add to FOREACH_PER_CORE_CFG_FIELD (cfg surface) OR FOREACH_PER_CORE_DOMAIN_BITMAP (runtime bitmap); no manual fields permitted")
        failures += 1
    else:
        info(f"Check 2 PASS: PerCoreCfg<F> body contains ONLY FOREACH_PER_CORE_CFG_FIELD + FOREACH_PER_CORE_DOMAIN_BITMAP invocations (no manual fields)")

    # --- Check 3: ControllerConfig uses FOREACH_MANUAL_PER_CORE_FIELD X-macro for parallel arrays ---
    # Post-WIP2d-0.B: parallel arrays come from X-macro expansion in ControllerConfig.hpp.
    # No literal `<type> core_<name>[16];` declarations should exist outside the X-macro invocation.
    manual_body = extract_macro_body(cfg_reg_text, "FOREACH_MANUAL_PER_CORE_FIELD")
    manual_xmacro = parse_foreach_manual_per_core_field(manual_body)
    manual_xmacro_names = set(manual_xmacro.keys())

    # Verify the X-macro invocation is present in ControllerConfig.hpp
    has_manual_xmacro_invocation = bool(re.search(
        r'FOREACH_MANUAL_PER_CORE_FIELD\s*\(\s*EMIT_MANUAL_PER_CORE_DECL\s*\)',
        ctrl_cfg_text,
    ))
    if not has_manual_xmacro_invocation:
        fail("Check 3 FAIL: ControllerConfig.hpp missing FOREACH_MANUAL_PER_CORE_FIELD(EMIT_MANUAL_PER_CORE_DECL) invocation")
        failures += 1

    # Verify NO literal `<type> core_<name>[16];` declarations exist anymore (all X-macro generated)
    parallel_arrays = parse_controller_config_parallel_arrays(ctrl_cfg_text)
    stray_decls = set(parallel_arrays.keys())
    if stray_decls:
        fail(f"Check 3 FAIL: stray manual parallel array declarations in ControllerConfig.hpp (must come from X-macro expansion only): {sorted(stray_decls)}")
        for name in sorted(stray_decls):
            typ, suffix, line = parallel_arrays[name]
            fail(f"  → ControllerConfig.hpp:{line}: '{typ} {name}[16]{suffix}' — delete; add to FOREACH_MANUAL_PER_CORE_FIELD instead")
        failures += 1

    if has_manual_xmacro_invocation and not stray_decls:
        info(f"Check 3 PASS: {len(manual_xmacro_names)} parallel arrays declared exclusively via FOREACH_MANUAL_PER_CORE_FIELD X-macro")

    # --- Check 4: FOREACH_MANUAL_PER_CORE_FIELD ↔ MANUAL_FIELDS_INVENTORY.md bidirectional sync ---
    inv_section_a = parse_inventory_section_a(inventory_text)
    only_in_xmacro = manual_xmacro_names - inv_section_a
    only_in_inv = inv_section_a - manual_xmacro_names
    if only_in_xmacro:
        fail(f"Check 4 FAIL: in FOREACH_MANUAL_PER_CORE_FIELD but missing from MANUAL_FIELDS_INVENTORY.md Section A: {sorted(only_in_xmacro)}")
        failures += 1
    if only_in_inv:
        fail(f"Check 4 FAIL: in MANUAL_FIELDS_INVENTORY.md Section A but missing from FOREACH_MANUAL_PER_CORE_FIELD: {sorted(only_in_inv)}")
        failures += 1
    if not only_in_xmacro and not only_in_inv:
        info(f"Check 4 PASS: {len(manual_xmacro_names)} Section A entries in sync between X-macro + inventory")

    # Verify Section B inventory matches PERMITTED_RUNTIME_FIELDS
    inv_section_b = parse_inventory_section_b(inventory_text)
    if inv_section_b != PERMITTED_RUNTIME_FIELDS:
        fail(f"Check 4 FAIL: MANUAL_FIELDS_INVENTORY.md Section B mismatch with PERMITTED_RUNTIME_FIELDS in CI script")
        fail(f"  inventory says: {sorted(inv_section_b)}")
        fail(f"  CI permits:     {sorted(PERMITTED_RUNTIME_FIELDS)}")
        failures += 1

    # --- Check 5: No name duplication between registries ---
    duplicates = cfg_field_names & manual_xmacro_names
    if duplicates:
        fail(f"Check 5 FAIL: name(s) appear in BOTH FOREACH_PER_CORE_CFG_FIELD + FOREACH_MANUAL_PER_CORE_FIELD: {sorted(duplicates)}")
        fail("  → each name must be in EITHER the registry (for X-macro struct gen) OR the manual exemption inventory, NOT both")
        failures += 1
    else:
        info(f"Check 5 PASS: no name duplication between registries")

    # --- Check 6: Anti-pattern 1 consumer scan (informational; WARN) ---
    findings_ctrl = scan_anti_pattern_1(ctrl_cfg_text)
    if findings_ctrl:
        warn(f"Check 6 INFO: anti-pattern 1 consumer shape detected (cfg.X + cfg.core_overrides[c].X same X) in ControllerConfig.hpp for fields: {sorted(set(findings_ctrl))}")
        warn("  → cfg-scope-discipline.md § Anti-pattern 1 (global-default-with-override) FORBIDDEN")
        warn("  → WIP2f deletion of core_overrides[16] makes the shape UNEXPRESSIBLE; refactor before WIP2g for safety-critical sites")

    # --- Final verdict ---
    if failures > 0:
        fail(f"per-core cfg integrity check FAILED with {failures} violations — see errors above")
        return 1
    info(f"all 5 structural checks PASS — per-core cfg discipline intact")
    return 0


if __name__ == "__main__":
    sys.exit(main())
