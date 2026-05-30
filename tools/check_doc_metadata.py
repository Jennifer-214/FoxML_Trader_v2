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

# Machine-portable roots (per feedback_machine_portable_resolver_for_committed_tool_paths):
# ENGINE derives from this file's location (<engine>/tools/check_*.py); WORKSPACE via
# env-override -> sibling-default -> .exists()-guard. No $HOME hardcode in a committed,
# public-AGPL tool — runs on any clone / any PC / SSH-grid node.
ENGINE = Path(os.environ.get("FOXML_ENGINE") or Path(__file__).resolve().parent.parent)
def _resolve_workspace_root():
    env = os.environ.get("FOXML_WORKSPACE")
    if env and Path(env).exists():
        return Path(env)
    sibling = ENGINE.parent / "tick-trader-percore-workspace"
    return sibling if sibling.exists() else ENGINE
WORKSPACE = _resolve_workspace_root()

VOCAB_PATH = WORKSPACE / "DESIGN_SPECS" / "meta-disciplines" / "doc-tag-vocabulary.md"
CONVENTION_PATH = WORKSPACE / "DESIGN_SPECS" / "meta-disciplines" / "doc-frontmatter-convention.md"


def _resolve_memory_dir():
    """Resolve the Claude Code institutional-memory dir (machine-portable; D-89 fork 1).

    Order: $FOXML_MEMORY_DIR override -> the local Claude Code projects store
    derived from the engine repo path -> None if absent. Designed so the guard
    runs today on one machine AND a multi-machine / SSH-grid node just exports
    FOXML_MEMORY_DIR (no $HOME hardcode baked into a committed tool). Cross-node
    memory *sync* itself is a separate concern (forward-promise).

    NOTE: memory doc-system fields (tags/sister_specs) live nested under the
    harness-native `metadata:` block (harness-durable). parse_frontmatter is a
    FLAT line parser, so it already surfaces those indented keys as top-level --
    no metadata-aware parsing needed. If parse_frontmatter is ever upgraded to
    real nested YAML, teach it to flatten memory `metadata.*` (D-89).
    """
    override = os.environ.get("FOXML_MEMORY_DIR")
    if override:
        p = Path(override)
        return p if p.exists() else None
    # Claude Code keys its per-project memory dir by the project's absolute path
    # with '/' and '_' both mapped to '-'. Derive from ENGINE; do not hardcode $HOME.
    project_id = str(ENGINE).replace("/", "-").replace("_", "-")
    p = Path.home() / ".claude" / "projects" / project_id / "memory"
    return p if p.exists() else None


MEMORY_DIR = _resolve_memory_dir()


def _memory_ref_exists(ref_clean):
    """True if a sister_specs ref resolves to a memory file (D-89 fork 2 -- dual-tree).

    Memories cross-link to OTHER memories by filename (e.g. `feedback_x` or
    `feedback_x.md`) in the same unified `sister_specs:` field they use for
    DESIGN_SPECS paths. validate_doc tries DESIGN_SPECS/ first, then this.
    """
    if MEMORY_DIR is None:
        return False
    name = Path(ref_clean).name
    if not name.endswith(".md"):
        name += ".md"
    return (MEMORY_DIR / name).exists()

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
    last_list_key = None   # block-style list parent: a `key:` line followed by "  - item" lines
    for raw in fm_body.split("\n"):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("- "):          # block-list item -> append to the parent key.
            # R6 robustness (D-89): the Claude Code harness re-serializes agent-written
            # memory frontmatter, converting inline `[a,b]` to block style; read both so
            # normalization can't silently drop tags/sister_specs. Orphan items after an
            # inline list (malformed) are ignored (last_list_key resets to None on inline).
            if last_list_key is not None:
                cur = fields.get(last_list_key)
                if not isinstance(cur, list):
                    cur = []
                    fields[last_list_key] = cur
                cur.append(line[2:].strip())
            continue
        if ":" in line:
            key, _, val = line.partition(":")
            key, val = key.strip(), val.strip()
            if val.startswith("[") and val.endswith("]"):
                fields[key] = [i.strip() for i in val[1:-1].split(",") if i.strip()]
                last_list_key = None
            elif val == "":                # empty value -> possible block-list parent
                fields[key] = ""
                last_list_key = key
            else:
                fields[key] = val
                last_list_key = None
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
            if not ref_path.exists() and not _memory_ref_exists(ref_clean):
                violations.append(f"BROKEN sister_specs ref '{ref_clean}': {path}")

    return violations


def _norm_sister(ref):
    """Normalize a sister_specs ref to a comparable filename (basename + `.md`).

    Handles DESIGN_SPECS paths (`meta-disciplines/x.md` -> `x.md`), memory
    filenames (`feedback_x.md` -> `feedback_x.md`), and bare inline-link forms
    (`feedback_x` -> `feedback_x.md`). Lets memory<->memory + memory<->spec links
    compare uniformly (D-89 fork 2).
    """
    n = Path(ref.strip('"').strip("'")).name
    return n if n.endswith(".md") else n + ".md"


def check_bidirectional_sisters(files_with_fm):
    """Verify: if A.sister_specs = [B], then B.sister_specs should contain A.

    Covers DESIGN_SPECS, skills, AND memories (memories enter files_with_fm via
    the memory scan root -- D-89 fork 1). Names compared via _norm_sister so a
    bare `feedback_x` and a `feedback_x.md` match the same file.
    """
    violations = []
    by_name = {}
    for path, fm in files_with_fm:
        by_name[Path(path).name] = (path, fm)
    for path, fm in files_with_fm:
        my_name = Path(path).name
        for sister in fm.get("sister_specs", []):
            if not sister.strip('"').strip("'"):
                continue
            sister_name = _norm_sister(sister)
            if sister_name not in by_name:
                continue
            _, sister_fm = by_name[sister_name]
            back_refs = [_norm_sister(s) for s in sister_fm.get("sister_specs", []) if s.strip('"').strip("'")]
            if my_name not in back_refs:
                violations.append(
                    f"BIDIR sister-doc asymmetry: {path} → {sister_name} "
                    f"(but {sister_name} does not point back at {my_name})"
                )
    return violations


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--strict", action="store_true", help="enforce SHOULD-HAVE frontmatter on docs")
    parser.add_argument("--paths", nargs="*", help="specific files to check (default: all)")
    parser.add_argument("--bidirectional", action="store_true", help="verify sister_specs is bidirectional")
    parser.add_argument("--memories", action="store_true", help="scan ONLY the memory dir (cadence guard scope; D-89)")
    args = parser.parse_args()

    concern_vocab, surface_vocab = load_vocabulary()
    if concern_vocab is None or surface_vocab is None:
        print(f"ERROR: vocabulary not loadable at {VOCAB_PATH}", file=sys.stderr)
        return 2

    if args.paths:
        files_to_check = [Path(p) for p in args.paths]
    elif args.memories:                     # D-89: memory-scoped cadence guard
        files_to_check = sorted(MEMORY_DIR.glob("*.md")) if MEMORY_DIR else []
    else:
        files_to_check = []
        scan_roots = [WORKSPACE / "DESIGN_SPECS", WORKSPACE / "claude-skills"]
        if MEMORY_DIR is not None:          # D-89 fork 1: cover memories in the default scan
            scan_roots.append(MEMORY_DIR)
        for root in scan_roots:
            if root.exists():
                files_to_check.extend(root.rglob("*.md"))

    all_violations = []
    files_checked = 0
    files_with_fm = []
    for path in files_to_check:
        if not path.exists():
            continue
        files_checked += 1
        violations = validate_doc(path, concern_vocab, surface_vocab, strict=args.strict)
        all_violations.extend(violations)
        fm = parse_frontmatter(path)
        if fm:
            files_with_fm.append((str(path), fm))

    if args.bidirectional:
        bidir_violations = check_bidirectional_sisters(files_with_fm)
        all_violations.extend(bidir_violations)

    print(f"Checked {files_checked} files; loaded {len(concern_vocab)} concern + {len(surface_vocab)} surface tags")
    if args.bidirectional:
        print(f"Bidirectional sister_specs check: {len(files_with_fm)} files scanned")
    if all_violations:
        print(f"\nVIOLATIONS ({len(all_violations)}):")
        for v in all_violations[:50]:
            print(f"  {v}")
        if len(all_violations) > 50:
            print(f"  ... and {len(all_violations) - 50} more")
        return 1
    print("\nAll frontmatter valid.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
