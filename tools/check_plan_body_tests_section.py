#!/usr/bin/env python3
"""
tools/check_plan_body_tests_section.py — Check 45 mechanical enforcement.

Verifies that any plan body whose coding sequence MODIFIES engine test files includes a
dedicated "Tests changed" section enumerating the 3 sub-categories per
feedback_test_change_enumeration_per_plan_body:
  (a) modified tests / (b) broken-replaced tests / (c) NEW unit tests added.

Sister to tools/check_plan_body_symbol_existence.py (B-Plus) — same Python CI tool family,
same pre-commit hook integration, same --strict semantics.

M7 9th canonical structural enforcement application per
DESIGN_SPECS/meta-disciplines/structural-enforcement-when-memory-insufficient.md.

Per .D.1 plan body Phase A.5 + /readiness Check 45 (NOT Check 44 — check-44 sidecar
check-44-cfg-field-categorization.md already exists from .B.4 v1.7.6).

=== TRIGGER (when is the section REQUIRED?) ===
A plan body needs the "Tests changed" section iff its body indicates MODIFICATION of an
engine test source file. Signals (any match → required):
  1. tests/<path>.cpp|hpp     — explicit test source path being touched
  2. <name>_test.cpp          — a test source filename (distinct from running the binary)
  3. parity_harness.cpp       — the parity harness source
  4. "tests/ files" / "tests/{...}" / "tests/<reorg-dir>"  — tests/ as an edit target

DELIBERATELY does NOT trigger on:
  - "controller_test" / "depth_recorder_test" without .cpp  (running the binary; baseline)
  - "tests pass" / "test baseline preserved"                 (execution, not modification)
  - "tests/<file>.cpp" with literal <angle-bracket> placeholders (template examples)
  - tools/test_*.py                                          (tool unit tests, not engine tests/)

=== OUTPUT ===
  PASS                              — section present + 3 sub-categories, OR not triggered (N/A)
  VIOLATION-MISSING-SECTION         — triggered but no "Tests changed" section
  VIOLATION-INCOMPLETE-SUBCATEGORIES — section present but missing (a)/(b)/(c) markers
--strict → exit 1 on any VIOLATION.
"""

import argparse
import re
import sys
from pathlib import Path
from typing import List, Tuple


# Trigger regexes — engine test SOURCE FILE modification signals.
# DELIBERATELY PRECISE (per .D.1 Phase A.5 refinement): only SPECIFIC test-source-file
# references trigger. Earlier weak patterns (`tests/ files`, `tests/{unit,...}` reorg-dir
# phrasing) were DROPPED — they false-triggered on plans that merely DISCUSS the tests/
# reorg (D-36) rather than modify tests (e.g., .D.1 discusses `tests/{unit,integration,...}`
# but modifies NO engine test). A real test-modification plan names the .cpp file (.E.1
# triggers via `controller_test.cpp`); generic "tests/ files" without a named source is a
# vague-plan smell that /readiness Check 45 + operator review backstop.
TRIGGER_PATTERNS = [
    re.compile(r"tests/[\w./-]+\.(?:cpp|hpp)"),       # explicit test source path (excludes <placeholder>)
    re.compile(r"\b\w+_test\.cpp\b"),                 # <name>_test.cpp source filename
    re.compile(r"\bparity_harness\.cpp\b"),           # parity harness source
]

# "Tests changed" section header (## or ### etc.).
SECTION_HEADER = re.compile(r"^#{1,4}\s+Tests\s+changed\b", re.IGNORECASE | re.MULTILINE)

# Sub-category markers within the section.
SUBCAT_A = re.compile(r"\(a\)|modified\s+tests", re.IGNORECASE)
SUBCAT_B = re.compile(r"\(b\)|broken|replaced", re.IGNORECASE)
SUBCAT_C = re.compile(r"\(c\)|NEW\s+unit\s+tests?", re.IGNORECASE)


def is_triggered(body: str) -> Tuple[bool, str]:
    """Return (triggered, matched-signal) — does the body modify engine test source?

    Runs against the FULL body (NOT fence-stripped). Fence-stripping was removed at .D.1
    Phase A.5 after it caused a desync bug: plan bodies with unbalanced ``` markers (e.g.,
    .E.1 has 37 = odd) desync the fence-state tracker, wrongly stripping real content after
    the desync point (missed .E.1's `controller_test.cpp` → false N/A). Template-example
    placeholders (`tests/<file>.cpp` with angle brackets) are already excluded by the
    trigger regex char classes (no `<` in `[\\w./-]`), so fence-stripping was redundant.
    A real `tests/foo.cpp` shown inside a code block almost always indicates real
    modification, so conservative over-trigger (requires the section, which may say 'none')
    is acceptable.
    """
    for pat in TRIGGER_PATTERNS:
        m = pat.search(body)
        if m:
            return True, m.group()
    return False, ""


def find_section_body(body: str) -> str:
    """Return the body text of the 'Tests changed' section (until next same-or-higher header)."""
    m = SECTION_HEADER.search(body)
    if not m:
        return ""
    start = m.end()
    # Determine header level
    header_line = body[m.start():m.end()]
    level = len(header_line) - len(header_line.lstrip("#").lstrip()) if False else header_line.count("#", 0, header_line.find(" "))
    # Find next header of same-or-higher level
    rest = body[start:]
    next_header = re.search(r"^#{1," + str(max(level, 1)) + r"}\s+\S", rest, re.MULTILINE)
    if next_header:
        return rest[:next_header.start()]
    return rest


def check_plan_body(path: str) -> Tuple[str, str]:
    """Check one plan body. Returns (verdict, detail)."""
    try:
        with open(path, "r", encoding="utf-8") as f:
            body = f.read()
    except (IOError, UnicodeDecodeError) as e:
        return ("ERROR", f"could not read {path}: {e}")

    triggered, signal = is_triggered(body)

    if not triggered:
        return ("PASS", "N/A — plan body does not modify engine test source files")

    # Triggered → require section
    if not SECTION_HEADER.search(body):
        return (
            "VIOLATION-MISSING-SECTION",
            f"plan body modifies tests (signal: '{signal}') but has no '## Tests changed' section",
        )

    section = find_section_body(body)
    missing = []
    if not SUBCAT_A.search(section):
        missing.append("(a) modified tests")
    if not SUBCAT_B.search(section):
        missing.append("(b) broken/replaced tests")
    if not SUBCAT_C.search(section):
        missing.append("(c) NEW unit tests")

    if missing:
        return (
            "VIOLATION-INCOMPLETE-SUBCATEGORIES",
            f"'Tests changed' section present but missing: {', '.join(missing)}",
        )

    return ("PASS", f"section present + 3 sub-categories (triggered by '{signal}')")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Check 45 — plan body Tests-changed section enumeration (per .D.1 Phase A.5)"
    )
    parser.add_argument("--plan-body", help="Single plan body path to check")
    parser.add_argument(
        "--scope",
        nargs="+",
        help="Multiple plan body paths or dirs to check",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help="Exit 1 on any VIOLATION (for pre-commit hook)",
    )
    args = parser.parse_args()

    targets: List[str] = []
    if args.plan_body:
        targets.append(args.plan_body)
    if args.scope:
        for s in args.scope:
            p = Path(s)
            if p.is_file():
                targets.append(str(p))
            elif p.is_dir():
                targets.extend(str(f) for f in p.rglob("*.md"))
    if not targets:
        print("ERROR: provide --plan-body <path> or --scope <paths>", file=sys.stderr)
        return 2

    any_violation = False
    for target in targets:
        verdict, detail = check_plan_body(target)
        marker = "✅" if verdict == "PASS" else ("⚠️" if verdict == "ERROR" else "❌")
        print(f"{marker} {verdict}: {target}")
        print(f"     {detail}")
        if verdict.startswith("VIOLATION"):
            any_violation = True

    if args.strict and any_violation:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
