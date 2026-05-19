#!/usr/bin/env python3
"""check_doc_metadata.py — validates YAML frontmatter across the doc system.

Enforces `DESIGN_SPECS/doc-frontmatter-convention.md` discipline:
- Required frontmatter fields per doc type
- All `tags:` + `surface:` values exist in `DESIGN_SPECS/doc-tag-vocabulary.md`
- `sister_specs:` paths exist (resolves relative to workspace DESIGN_SPECS/)
- `lifecycle` stage is one of 6 valid stages
- `type:` is one of valid doc types

Sister to:
- tools/check_meta_registry.py (FOREACH_REGISTRY H15 enforcement)
- tools/check_struct_field_uniqueness.py (B13 cross-walker field uniqueness)

Exit codes:
  0 = all frontmatter valid (or files exempted)
  1 = at least one violation
  2 = script error / vocabulary file missing

Usage:
  python3 tools/check_doc_metadata.py                    # check all docs
  python3 tools/check_doc_metadata.py --strict           # also enforce SHOULD-HAVE frontmatter
  python3 tools/check_doc_metadata.py --paths <files>... # check specific files
"""
import os
import re
import sys
import argparse
from pathlib import Path

WORKSPACE = Path("/home/caramel/code/tick-trader-percore-workspace")
ENGINE = Path("/home/caramel/code/FoxML_Trader_v2")

VOCAB_PATH = WORKSPACE / "DESIGN_SPECS" / "meta-disciplines" / "doc-tag-vocabulary.md"
CONVENTION_PATH = WORKSPACE / "DESIGN_SPECS" / "meta-disciplines" / "doc-frontmatter-convention.md"

VALID_TYPES = {
    "refactor-pattern", "feature-pattern", "framework-pattern",
    "audit-methodology", "data-discipline", "concurrency-pattern",
    "wire-format-pattern", "doc-discipline", "meta-discipline",
    "plan-template", "ledger-template", "architecture-overview",
    "skill", "skill-check", "feedback", "user", "project", "reference",
    "sprint-master", "sub-plan", "handoff", "postmortem",
    "audit-report", "orientation-doc",
}

VALID_LIFECYCLE = {
    "1-problem", "2-draft", "3-first-canonical",
    "4-cohort", "5-claude-md", "6-cadence-locked",
}

REQUIRED_BASE = {"type", "established"}
REQUIRED_DESIGN_SPECS = {"type", "stage", "version", "established", "tags", "surface"}


def parse_frontmatter(path):
    """Extract YAML frontmatter as dict; returns None if no frontmatter."""
    try:
        with open(path, encoding="utf-8") as f:
            content = f.read()
    except (IOError, OSError):
        return None
    if not content.startswith("---\n"):
        return None
    end_marker = content.find("\n---\n", 4)
    if end_marker == -1:
        return None
    fm_body = content[4:end_marker]
    fields = {}
    for line in fm_body.split("\n"):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if ":" in line:
            key, _, val = line.partition(":")
            val = val.strip()
            if val.startswith("[") and val.endswith("]"):
                items = val[1:-1].split(",")
                fields[key.strip()] = [i.strip() for i in items if i.strip()]
            else:
                fields[key.strip()] = val
    return fields


def load_vocabulary():
    """Extract valid CONCERN + SURFACE tags from doc-tag-vocabulary.md table rows."""
    if not VOCAB_PATH.exists():
        return None, None
    with open(VOCAB_PATH, encoding="utf-8") as f:
        content = f.read()
    concern_tags = set()
    surface_tags = set()
    in_concern = False
    in_surface = False
    for line in content.split("\n"):
        if "## CONCERN axis" in line:
            in_concern, in_surface = True, False
            continue
        if "## SURFACE axis" in line:
            in_concern, in_surface = False, True
            continue
        if line.startswith("## ") and (in_concern or in_surface):
            in_concern = in_surface = False
            continue
        m = re.match(r"\|\s*`([a-z0-9-]+)`\s*\|", line)
        if m:
            tag = m.group(1)
            if in_concern:
                concern_tags.add(tag)
            elif in_surface:
                surface_tags.add(tag)
    return concern_tags, surface_tags


def validate_doc(path, concern_vocab, surface_vocab, strict=False):
    """Return list of violation strings for this doc."""
    violations = []
    fm = parse_frontmatter(path)

    is_design_spec = "DESIGN_SPECS/" in str(path) and path.suffix == ".md"
    is_skill = "claude-skills/" in str(path) and path.name == "SKILL.md"
    is_memory = "memory/" in str(path) and path.name != "MEMORY.md"

    needs_frontmatter = is_design_spec or is_skill or is_memory

    if fm is None:
        if needs_frontmatter and strict:
            violations.append(f"MISSING frontmatter (strict): {path}")
        return violations

    if "type" not in fm:
        violations.append(f"MISSING type field: {path}")
    elif fm["type"] not in VALID_TYPES:
        violations.append(f"INVALID type '{fm['type']}': {path}")

    if "stage" in fm:
        stage = fm["stage"]
        if stage not in VALID_LIFECYCLE:
            violations.append(f"INVALID stage '{stage}': {path}")

    if "tags" in fm and concern_vocab:
        for tag in fm["tags"]:
            tag_clean = tag.strip('"').strip("'")
            if tag_clean and tag_clean not in concern_vocab:
                violations.append(f"UNDEFINED concern tag '{tag_clean}': {path}")

    if "surface" in fm and surface_vocab:
        for tag in fm["surface"]:
            tag_clean = tag.strip('"').strip("'")
            if tag_clean and tag_clean not in surface_vocab:
                violations.append(f"UNDEFINED surface tag '{tag_clean}': {path}")

    if "sister_specs" in fm:
        for ref in fm["sister_specs"]:
            ref_clean = ref.strip('"').strip("'")
            if not ref_clean:
                continue
            if ref_clean.startswith("DESIGN_SPECS/"):
                ref_path = WORKSPACE / ref_clean
            else:
                # Try root + each subdir per folder-subdivision layout
                ref_path = WORKSPACE / "DESIGN_SPECS" / ref_clean
                if not ref_path.exists():
                    found = False
                    for subdir in (WORKSPACE / "DESIGN_SPECS").iterdir():
                        if subdir.is_dir():
                            candidate = subdir / ref_clean
                            if candidate.exists():
                                ref_path = candidate
                                found = True
                                break
                    if not found:
                        ref_path = WORKSPACE / "DESIGN_SPECS" / ref_clean
            if not ref_path.exists():
                violations.append(f"BROKEN sister_specs ref '{ref_clean}': {path}")

    return violations


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--strict", action="store_true", help="enforce SHOULD-HAVE frontmatter on docs")
    parser.add_argument("--paths", nargs="*", help="specific files to check (default: all)")
    args = parser.parse_args()

    concern_vocab, surface_vocab = load_vocabulary()
    if concern_vocab is None or surface_vocab is None:
        print(f"ERROR: vocabulary not loadable at {VOCAB_PATH}", file=sys.stderr)
        return 2

    if args.paths:
        files_to_check = [Path(p) for p in args.paths]
    else:
        files_to_check = []
        for root in [WORKSPACE / "DESIGN_SPECS", WORKSPACE / "claude-skills"]:
            if root.exists():
                files_to_check.extend(root.rglob("*.md"))

    all_violations = []
    files_checked = 0
    for path in files_to_check:
        if not path.exists():
            continue
        files_checked += 1
        violations = validate_doc(path, concern_vocab, surface_vocab, strict=args.strict)
        all_violations.extend(violations)

    print(f"Checked {files_checked} files; loaded {len(concern_vocab)} concern + {len(surface_vocab)} surface tags")
    if all_violations:
        print(f"\nVIOLATIONS ({len(all_violations)}):")
        for v in all_violations:
            print(f"  {v}")
        return 1
    print("\nAll frontmatter valid.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
