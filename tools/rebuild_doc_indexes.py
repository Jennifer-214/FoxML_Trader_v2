#!/usr/bin/env python3
"""rebuild_doc_indexes.py — regenerate doc-system indexes from frontmatter.

Implements `/index-rebuild` skill (DESIGN_SPECS/doc-disciplines/) mechanically.

Regenerates:
- CLAUDE.md "Skill suite" table (auto-generated from SKILL.md frontmatter `concern:`)
- DESIGN_SPECS/README.md catalog (grouped by `type:` then `stage:`)
- DESIGN_SPECS/TAG_INDEX.md (tag → files reverse-lookup snapshot)

Sister to:
- tools/check_doc_metadata.py (validation; this tool generates indexes)
- tools/subdivide_design_specs.py (folder subdivision migration)

Usage:
  python3 tools/rebuild_doc_indexes.py                       # regenerate all
  python3 tools/rebuild_doc_indexes.py --target claude-md    # CLAUDE.md skill table only
  python3 tools/rebuild_doc_indexes.py --target readme       # DESIGN_SPECS/README.md only
  python3 tools/rebuild_doc_indexes.py --target tag-index    # TAG_INDEX.md only
  python3 tools/rebuild_doc_indexes.py --dry-run             # print proposed output
"""
import re
import sys
import argparse
from pathlib import Path
from collections import defaultdict

WORKSPACE = Path("/home/caramel/code/tick-trader-percore-workspace")
SPECS_DIR = WORKSPACE / "DESIGN_SPECS"
SKILLS_DIR = WORKSPACE / "claude-skills"
CLAUDE_MD = WORKSPACE / "CLAUDE.md"

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_doc_metadata import _resolve_memory_dir  # SSoT memory-dir resolver (D-89)
MEMORY_DIR = _resolve_memory_dir()

CONCERN_LABEL = {
    "pre-coding-gate": "Pre-coding plan verification",
    "shape-audit": "SHAPE audits (design-layer)",
    "impl-detail-audit": "IMPLEMENTATION-DETAIL audits",
    "domain-audit": "DOMAIN audits",
    "anti-pattern-scan": "Anti-pattern scans",
    "post-coding": "Post-coding",
    "workflow": "Workflow",
    "scaffolding": "Scaffolding",
    "recurrence": "Recurrence",
}
CONCERN_ORDER = [
    "pre-coding-gate", "shape-audit", "impl-detail-audit", "domain-audit",
    "anti-pattern-scan", "post-coding", "workflow", "scaffolding", "recurrence",
]

TYPE_ORDER = [
    "refactor-pattern", "framework-pattern", "feature-pattern",
    "audit-methodology", "data-discipline", "concurrency-pattern",
    "wire-format-pattern", "doc-discipline", "meta-discipline",
    "plan-template", "ledger-template", "architecture-overview",
]


def parse_frontmatter(path):
    try:
        with open(path, encoding="utf-8") as f:
            content = f.read()
    except (IOError, OSError):
        return None
    if not content.startswith("---\n"):
        return None
    end = content.find("\n---\n", 4)
    if end == -1:
        return None
    fields = {}
    for line in content[4:end].split("\n"):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if ":" in line:
            key, _, val = line.partition(":")
            val = val.strip()
            if val.startswith("[") and val.endswith("]"):
                fields[key.strip()] = [i.strip() for i in val[1:-1].split(",") if i.strip()]
            else:
                fields[key.strip()] = val
    return fields


def collect_skills():
    skills = {}
    for skill_md in SKILLS_DIR.rglob("SKILL.md"):
        fm = parse_frontmatter(skill_md)
        if not fm:
            continue
        skill_name = fm.get("name") or skill_md.parent.name
        skills[skill_name] = fm
    return skills


def collect_specs():
    specs = {}
    for md in SPECS_DIR.rglob("*.md"):
        if md.name in {"README.md", "TAG_INDEX.md"}:
            continue
        fm = parse_frontmatter(md)
        if not fm or fm.get("type") == "skill-check":
            continue
        rel = md.relative_to(WORKSPACE)
        specs[str(rel)] = fm
    return specs


def collect_memories():
    """Memories (machine-portable dir; D-89). Flat parser surfaces metadata.tags."""
    mems = {}
    if MEMORY_DIR is None:
        return mems
    for md in sorted(MEMORY_DIR.glob("*.md")):
        if md.name == "MEMORY.md":
            continue
        fm = parse_frontmatter(md)
        if fm:
            mems[f"memory/{md.name}"] = fm
    return mems


def gen_skill_suite_table(skills):
    by_concern = defaultdict(list)
    for name, fm in skills.items():
        concern = fm.get("concern", "workflow")
        by_concern[concern].append(name)
    lines = ["| Concern | Skills |", "|---|---|"]
    for concern in CONCERN_ORDER:
        skills_list = sorted(by_concern.get(concern, []))
        if not skills_list:
            continue
        formatted = " + ".join(f"`/{s}`" for s in skills_list)
        lines.append(f"| **{CONCERN_LABEL[concern]}** | {formatted} |")
    return "\n".join(lines)


def update_claude_md_skill_table(skills):
    new_table = gen_skill_suite_table(skills)
    with open(CLAUDE_MD, encoding="utf-8") as f:
        content = f.read()
    pattern = re.compile(
        r"(## Skill suite \(audit-driven discipline\)\n\nSkills group by concern.*?\n\n)"
        r"\| Concern \| Skills \|\n\|---\|---\|\n"
        r"(?:\|.*\|\n)+",
        re.DOTALL,
    )
    m = pattern.search(content)
    if not m:
        return False, "Could not locate Skill suite table in CLAUDE.md"
    new_content = pattern.sub(m.group(1) + new_table + "\n", content)
    return True, new_content


def gen_design_specs_readme(specs):
    by_type = defaultdict(list)
    for path, fm in specs.items():
        by_type[fm.get("type", "unknown")].append((path, fm))
    lines = [
        "# DESIGN_SPECS catalog",
        "",
        "**Auto-generated** by `tools/rebuild_doc_indexes.py`. Regenerate after adding/moving specs.",
        "",
        f"Total: {len(specs)} specs across {len(by_type)} types.",
        "",
        "Per-type catalog grouped by lifecycle stage. Cross-ref:",
        "- `doc-frontmatter-convention.md` (frontmatter schema)",
        "- `doc-tag-vocabulary.md` (tag canonical list)",
        "- CLAUDE.md § How to find anything (grep recipes)",
        "",
    ]
    for spec_type in TYPE_ORDER:
        rows = by_type.get(spec_type, [])
        if not rows:
            continue
        lines.append(f"## {spec_type} ({len(rows)} specs)")
        lines.append("")
        lines.append("| Spec | Stage | Tags | Sister count |")
        lines.append("|---|---|---|---|")
        for path, fm in sorted(rows):
            stage = fm.get("stage", "?")
            tags = fm.get("tags", [])
            sisters = fm.get("sister_specs", [])
            name = Path(path).stem
            tag_str = ", ".join(tags[:3])
            if len(tags) > 3:
                tag_str += f", +{len(tags) - 3}"
            lines.append(f"| `{path}` | {stage} | {tag_str} | {len(sisters)} |")
        lines.append("")
    return "\n".join(lines)


def gen_tag_index(specs, skills, memories=None):
    by_tag = defaultdict(set)
    by_surface = defaultdict(set)
    for path, fm in (memories or {}).items():   # memories: concern tags only (no surface axis)
        for t in fm.get("tags", []):
            by_tag[t].add(path)
    for path, fm in specs.items():
        for t in fm.get("tags", []):
            by_tag[t].add(path)
        for s in fm.get("surface", []):
            by_surface[s].add(path)
    for name, fm in skills.items():
        rel = f"claude-skills/{name}/SKILL.md"
        for t in fm.get("tags", []):
            by_tag[t].add(rel)
        for s in fm.get("surface", []):
            by_surface[s].add(rel)
    lines = [
        "# Tag → files index (auto-generated snapshot)",
        "",
        "**Auto-generated** by `tools/rebuild_doc_indexes.py`. Canonical reverse-lookup is `rg`:",
        "",
        "```bash",
        'rg -l "^tags:.*\\bframework-discipline\\b" DESIGN_SPECS/',
        'rg -l "^surface:.*\\bhot-path\\b"',
        "```",
        "",
        "This file is a snapshot for static browsing.",
        "",
        "## CONCERN tags",
        "",
    ]
    for tag in sorted(by_tag.keys()):
        files = sorted(by_tag[tag])
        lines.append(f"### {tag} ({len(files)} files)")
        lines.append("")
        for f in files:
            lines.append(f"- `{f}`")
        lines.append("")
    lines.append("## SURFACE tags")
    lines.append("")
    for tag in sorted(by_surface.keys()):
        files = sorted(by_surface[tag])
        lines.append(f"### {tag} ({len(files)} files)")
        lines.append("")
        for f in files:
            lines.append(f"- `{f}`")
        lines.append("")
    return "\n".join(lines)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--target", choices=["claude-md", "readme", "tag-index", "all"], default="all")
    p.add_argument("--dry-run", action="store_true")
    args = p.parse_args()

    skills = collect_skills()
    specs = collect_specs()
    memories = collect_memories()
    print(f"Loaded {len(skills)} skills + {len(specs)} DESIGN_SPECS + {len(memories)} memories")

    if args.target in ("claude-md", "all"):
        ok, result = update_claude_md_skill_table(skills)
        if not ok:
            print(f"ERROR: {result}", file=sys.stderr)
        elif args.dry_run:
            print("\n=== CLAUDE.md skill suite (proposed) ===")
            print(gen_skill_suite_table(skills))
        else:
            with open(CLAUDE_MD, "w", encoding="utf-8") as f:
                f.write(result)
            print(f"Updated CLAUDE.md skill suite table")

    if args.target in ("readme", "all"):
        readme_content = gen_design_specs_readme(specs)
        readme_path = SPECS_DIR / "README.md"
        if args.dry_run:
            print("\n=== DESIGN_SPECS/README.md (first 60 lines) ===")
            print("\n".join(readme_content.split("\n")[:60]))
        else:
            with open(readme_path, "w", encoding="utf-8") as f:
                f.write(readme_content)
            print(f"Wrote {readme_path}")

    if args.target in ("tag-index", "all"):
        tag_content = gen_tag_index(specs, skills, memories)
        tag_path = SPECS_DIR / "TAG_INDEX.md"
        if args.dry_run:
            print("\n=== TAG_INDEX.md (first 50 lines) ===")
            print("\n".join(tag_content.split("\n")[:50]))
        else:
            with open(tag_path, "w", encoding="utf-8") as f:
                f.write(tag_content)
            print(f"Wrote {tag_path}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
