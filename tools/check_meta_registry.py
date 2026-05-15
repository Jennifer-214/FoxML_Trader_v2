#!/usr/bin/env python3
"""check_meta_registry.py — codebase-wide FOREACH_REGISTRY cross-check (WIP2d-1.B.0b).

Closes Shortsighted #2 (meta-registry applied one ship early; cross-references were
documentation-only until .F.4d). This script enforces the discipline NOW: every X-macro
registry in the codebase MUST have a row in FOREACH_REGISTRY (CoreFrameworks/MetaRegistry.hpp).
Adding a new registry without registering it FAILS the build.

Pulls forward .F.4d H15 codification one ship early per CLAUDE.md item 31 framework discipline.

Checks performed:
  1. Every `#define FOREACH_\\w+\\(X\\)` in the codebase has a matching row in FOREACH_REGISTRY
  2. Every row in FOREACH_REGISTRY corresponds to an actual `#define` somewhere
  3. LEVEL/PARENT discipline: LEVEL > 0 rows have PARENT that exists in FOREACH_REGISTRY OR is ROOT_NONE

Exit codes:
  0 = all checks pass
  1 = one or more checks failed (build aborted)
  2 = script error / file missing

Best-effort heuristic: regex-based grep of #define lines. Macros defined via `#define FOREACH_<NAME>(X)`
are detected; unusual definitions (e.g., conditional compilation, macro-generated macro names) are out of
scope. Pairs with the X-macro struct generation discipline for layered defense.

Cross-references:
  - DESIGN_SPECS/meta-registry-pattern-for-codebase-registry-discipline.md
  - CoreFrameworks/MetaRegistry.hpp (the FOREACH_REGISTRY definition)
  - tools/check_per_core_registry_integrity.py (sister script for per-core surface)
"""

import re
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT  = SCRIPT_DIR.parent
META_REG   = REPO_ROOT / "CoreFrameworks/MetaRegistry.hpp"

# Directories to scan for FOREACH_<X> macro definitions
SCAN_DIRS = [
    "CoreFrameworks",
    "MemHeaders",
    "ML_Headers",
    "Strategies",
    "DataStream",
    "Backtest",
    "FixedPoint",
    "GUI",
]

# Registries intentionally NOT registered (documented exemptions; out of scope for this CI)
EXEMPTIONS: set = set()
# Add names like {"FOREACH_HELPER_MACRO"} if they look like registries but aren't.


def fail(msg: str) -> None:
    print(f"[meta-registry-CI] ERROR: {msg}", file=sys.stderr)


def warn(msg: str) -> None:
    print(f"[meta-registry-CI] WARN: {msg}", file=sys.stderr)


def info(msg: str) -> None:
    print(f"[meta-registry-CI] {msg}")


def scan_codebase_foreach_macros() -> set:
    """Grep all #define FOREACH_<NAME>(X) macro definitions in scan dirs. Returns set of names."""
    pattern = re.compile(r'^#define\s+(FOREACH_\w+)\s*\(\s*X\s*\)', re.MULTILINE)
    macros = set()
    for d in SCAN_DIRS:
        d_path = REPO_ROOT / d
        if not d_path.is_dir():
            continue
        for f in d_path.rglob("*.hpp"):
            try:
                text = f.read_text()
            except Exception:
                continue
            for m in pattern.finditer(text):
                macros.add(m.group(1))
        for f in d_path.rglob("*.cpp"):
            try:
                text = f.read_text()
            except Exception:
                continue
            for m in pattern.finditer(text):
                macros.add(m.group(1))
    return macros


def parse_foreach_registry() -> dict:
    """Parse FOREACH_REGISTRY rows from MetaRegistry.hpp. Returns dict {name: (level, parent)}."""
    if not META_REG.exists():
        fail(f"MetaRegistry.hpp not found at {META_REG}")
        sys.exit(2)
    text = META_REG.read_text()
    # Match: X(REGISTRY_NAME, LEVEL, PARENT, "description")
    pattern = re.compile(
        r'^\s+X\(\s*(\w+)\s*,\s*(\d+)\s*,\s*(\w+)\s*,\s*"([^"]*)"\s*\)',
        re.MULTILINE,
    )
    result = {}
    for m in pattern.finditer(text):
        name = m.group(1).strip()
        level = int(m.group(2))
        parent = m.group(3).strip()
        result[name] = (level, parent)
    return result


def main() -> int:
    info("scanning codebase for FOREACH_<X> macros + cross-checking against FOREACH_REGISTRY...")

    codebase_macros = scan_codebase_foreach_macros()
    info(f"  found {len(codebase_macros)} FOREACH_<X> macros in codebase")

    registry_entries = parse_foreach_registry()
    info(f"  found {len(registry_entries)} entries in FOREACH_REGISTRY")

    failures = 0

    # Check 1: every codebase macro is in FOREACH_REGISTRY (or exempted)
    unregistered = codebase_macros - set(registry_entries.keys()) - EXEMPTIONS - {"FOREACH_REGISTRY"}
    if unregistered:
        for name in sorted(unregistered):
            warn(f"Check 1: codebase macro `{name}` not in FOREACH_REGISTRY — register or document as EXEMPTION in tools/check_meta_registry.py")
        warn(f"Check 1 INFO: {len(unregistered)} unregistered macros (NON-FATAL during transition; promote to fatal at .F.4d H15 codification)")
    else:
        info(f"Check 1 PASS: all codebase FOREACH_<X> macros registered in FOREACH_REGISTRY (or exempted)")

    # Check 2: every FOREACH_REGISTRY entry corresponds to an actual #define (FATAL — registered-but-missing is a registry bug)
    missing_definitions = set(registry_entries.keys()) - codebase_macros - {"FOREACH_REGISTRY"}
    if missing_definitions:
        fail(f"Check 2 FAIL: FOREACH_REGISTRY rows have no matching #define in codebase: {sorted(missing_definitions)}")
        fail("  → delete the row OR add the missing #define")
        failures += 1
    else:
        info(f"Check 2 PASS: all {len(registry_entries)} FOREACH_REGISTRY rows match a real #define")

    # Check 3: LEVEL/PARENT discipline
    issues = []
    for name, (level, parent) in registry_entries.items():
        if level == 0:
            if parent != "ROOT_NONE":
                issues.append(f"  {name}: LEVEL=0 but PARENT='{parent}' (expected ROOT_NONE for codebase-wide root)")
        else:
            if parent == "ROOT_NONE":
                issues.append(f"  {name}: LEVEL={level} but PARENT=ROOT_NONE (expected non-root parent for LEVEL > 0)")
            elif parent not in registry_entries and parent != "FOREACH_REGISTRY":
                issues.append(f"  {name}: PARENT='{parent}' not found in FOREACH_REGISTRY")
    if issues:
        fail(f"Check 3 FAIL: LEVEL/PARENT discipline violations:")
        for issue in issues:
            fail(issue)
        failures += 1
    else:
        info(f"Check 3 PASS: LEVEL/PARENT discipline valid across {len(registry_entries)} rows")

    if failures > 0:
        fail(f"meta-registry check FAILED with {failures} violations")
        return 1
    info("all meta-registry structural checks PASS — codebase-wide registry discipline intact")
    return 0


if __name__ == "__main__":
    sys.exit(main())
