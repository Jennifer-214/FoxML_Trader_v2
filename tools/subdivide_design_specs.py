#!/usr/bin/env python3
"""subdivide_design_specs.py — folder subdivision + cross-ref sweep.

Moves DESIGN_SPECS/<name>.md files into type-subdirs based on frontmatter `type:` field.
Then sweeps all .md files in workspace + engine + memory for `DESIGN_SPECS/<name>.md`
references and updates to `DESIGN_SPECS/<subdir>/<name>.md`.

Per `DESIGN_SPECS/file-size-split-discipline.md` (file-size-split discipline sister) +
TECH_DEBT-113 (folder subdivision queued).

Type → subdir mapping:
  refactor-pattern        → refactor-patterns/
  feature-pattern         → feature-patterns/
  framework-pattern       → framework-patterns/
  audit-methodology       → audit-methodologies/
  data-discipline         → data-disciplines/
  concurrency-pattern     → concurrency-patterns/
  wire-format-pattern     → wire-format-patterns/
  doc-discipline          → doc-disciplines/
  meta-discipline         → meta-disciplines/
  plan-template           → plan-templates/
  ledger-template         → ledger-templates/
  architecture-overview   → (stays at root; README.md)

Exit codes:
  0 = success
  1 = validation failed
  2 = script error

Usage:
  python3 tools/subdivide_design_specs.py --dry-run   # print plan, no changes
  python3 tools/subdivide_design_specs.py             # execute moves + cross-ref sweep
  python3 tools/subdivide_design_specs.py --verify    # post-execution verification only
"""
import os
import re
import sys
import shutil
import argparse
import subprocess
from pathlib import Path

WORKSPACE = Path("/home/caramel/code/tick-trader-percore-workspace")
ENGINE = Path("/home/caramel/code/FoxML_Trader_v2")
MEMORY = Path("/home/caramel/.claude/projects/-home-caramel-code-FoxML-Trader-v2/memory")
SPECS_DIR = WORKSPACE / "DESIGN_SPECS"

TYPE_TO_SUBDIR = {
    "refactor-pattern": "refactor-patterns",
    "feature-pattern": "feature-patterns",
    "framework-pattern": "framework-patterns",
    "audit-methodology": "audit-methodologies",
    "data-discipline": "data-disciplines",
    "concurrency-pattern": "concurrency-patterns",
    "wire-format-pattern": "wire-format-patterns",
    "doc-discipline": "doc-disciplines",
    "meta-discipline": "meta-disciplines",
    "plan-template": "plan-templates",
    "ledger-template": "ledger-templates",
    "skill-check": None,
    "architecture-overview": None,
}

ROOT_KEEP_NAMES = {"README.md"}


def parse_type(path):
    """Read first ~30 lines, extract `type:` from YAML frontmatter."""
    try:
        with open(path, encoding="utf-8") as f:
            head = "".join(f.readline() for _ in range(30))
    except (IOError, OSError):
        return None
    if not head.startswith("---\n"):
        return None
    for line in head.split("\n")[1:25]:
        m = re.match(r"^type:\s*(\S+)", line)
        if m:
            return m.group(1)
    return None


def build_move_plan():
    """Return list of (src, dest) tuples for files to move."""
    plan = []
    for md in sorted(SPECS_DIR.glob("*.md")):
        if md.name in ROOT_KEEP_NAMES:
            continue
        spec_type = parse_type(md)
        if not spec_type:
            print(f"WARN: no `type:` found in {md.name}; skipping", file=sys.stderr)
            continue
        subdir = TYPE_TO_SUBDIR.get(spec_type)
        if subdir is None:
            continue
        dest = SPECS_DIR / subdir / md.name
        plan.append((md, dest))
    return plan


def execute_moves(plan, dry_run=False):
    """Execute file moves; return list of (src_rel, dest_rel) for cross-ref sweep."""
    moves = []
    for src, dest in plan:
        if dry_run:
            print(f"WOULD mv: {src.name} -> {dest.parent.name}/{dest.name}")
        else:
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.move(str(src), str(dest))
        moves.append((f"DESIGN_SPECS/{src.name}", f"DESIGN_SPECS/{dest.parent.name}/{dest.name}"))
    return moves


def collect_target_files():
    """All .md files to sweep for cross-ref updates."""
    roots = [WORKSPACE, ENGINE / "DOCS", MEMORY]
    files = []
    for root in roots:
        if root.exists():
            for md in root.rglob("*.md"):
                if ".git" in md.parts or "build" in md.parts:
                    continue
                files.append(md)
    return files


def sweep_crossrefs(moves, dry_run=False):
    """Apply move-based path updates across all .md files."""
    by_oldpath = {old: new for old, new in moves}
    files_to_check = collect_target_files()

    updated = 0
    total_replacements = 0
    for md in files_to_check:
        try:
            with open(md, encoding="utf-8") as f:
                content = f.read()
        except (IOError, OSError):
            continue
        new_content = content
        for old_path, new_path in by_oldpath.items():
            if old_path in new_content:
                new_content = new_content.replace(old_path, new_path)
        if new_content != content:
            count_diff = sum(1 for old in by_oldpath if old in content) - sum(1 for old in by_oldpath if old in new_content)
            total_replacements += count_diff
            if not dry_run:
                with open(md, "w", encoding="utf-8") as f:
                    f.write(new_content)
            updated += 1
            if dry_run:
                print(f"WOULD update {md.relative_to(md.parts[0] == '/' and Path('/') or Path('.'))}: {count_diff} refs")
    return updated, total_replacements


def verify():
    """Post-move verification."""
    issues = []
    # Check no DESIGN_SPECS/<name>.md refs to non-existent files
    for md in collect_target_files():
        try:
            with open(md, encoding="utf-8") as f:
                content = f.read()
        except (IOError, OSError):
            continue
        for match in re.finditer(r"DESIGN_SPECS/([\w-]+\.md)\b", content):
            referenced = match.group(1)
            full_path = SPECS_DIR / referenced
            if not full_path.exists() and referenced not in ROOT_KEEP_NAMES:
                issues.append(f"BROKEN ref in {md}: DESIGN_SPECS/{referenced}")
        for match in re.finditer(r"DESIGN_SPECS/([\w-]+)/([\w-]+\.md)\b", content):
            subdir, name = match.group(1), match.group(2)
            full_path = SPECS_DIR / subdir / name
            if not full_path.exists():
                issues.append(f"BROKEN subdir ref in {md}: DESIGN_SPECS/{subdir}/{name}")
    return issues


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--verify", action="store_true")
    args = parser.parse_args()

    if args.verify:
        issues = verify()
        if issues:
            print(f"FOUND {len(issues)} broken refs:")
            for i in issues[:20]:
                print(f"  {i}")
            if len(issues) > 20:
                print(f"  ... and {len(issues) - 20} more")
            return 1
        print("All cross-refs resolve.")
        return 0

    plan = build_move_plan()
    print(f"Plan: {len(plan)} files to move into type-subdirs")
    moves = execute_moves(plan, dry_run=args.dry_run)
    print(f"\nCross-ref sweep:")
    updated_files, total_repl = sweep_crossrefs(moves, dry_run=args.dry_run)
    print(f"  Files updated: {updated_files}")
    print(f"  Total replacements: {total_repl}")

    if not args.dry_run:
        print("\nVerifying post-move state...")
        issues = verify()
        if issues:
            print(f"FOUND {len(issues)} broken refs (showing first 20):")
            for i in issues[:20]:
                print(f"  {i}")
            return 1
        print("All cross-refs resolve.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
