#!/usr/bin/env python3
"""
check_capture_audit.py — MECHANICAL half of the /capture-audit skill, as a deterministic tool.

WHY (D-112, operator 2026-05-30): "the CI tools are unit tests for the docs; why aren't the
skills wired to them?" /capture-audit's Checks 1-10 were LLM-INTERPRETED PROSE (the agent
re-checking by hand = the exact failure mode of .E Session-4). This tool mechanizes the
genuinely-MECHANICAL, currently-tool-less checks so the skill INVOKES them instead of
prose-describing them. JUDGMENT checks (6 Stage-6 candidates / 5 handoff-currency-vs-intent /
10 currency) stay agent-driven — they can't be grepped (per feedback_independence_for_judgment_not_mechanical).

Mechanizes:
  Check 1  — MEMORY.md index sync: every memory/<f>.md has an index line in MEMORY.md (no orphans)
  Check 4  — decision-log sentinel matching: every `<!-- D/C/F: <id> -->` has a `<!-- STATUS: ... -->`
  Check 8  — skill -> CLAUDE.md suite linkage: every claude-skills/<x>/SKILL.md named in CLAUDE.md

Already tool-backed elsewhere (invoke those directly; NOT duplicated here):
  Check 1 frontmatter + Check 9/12 bidirectional sisters -> check_doc_metadata.py --bidirectional --memories
  Check 11 forward-promise                                -> check_forward_promise_audit.py
  plan-body symbol existence (B-Plus)                     -> check_plan_body_symbol_existence.py

CONVENTIONS (mirror check_doc_metadata.py): machine-portable resolver (env | script-loc | cwd);
exit 1 on any violation (CI-gating); minimal line-scan (no yaml dep).

USAGE
  python3 tools/check_capture_audit.py                 # all mechanical checks
  python3 tools/check_capture_audit.py --check 1|4|8    # one check
  python3 tools/check_capture_audit.py --quiet          # only failures + summary
"""
import argparse
import os
import re
import sys
from pathlib import Path


def _resolve_dir(env, candidates):
    v = os.environ.get(env)
    if v and Path(v).exists():
        return Path(v)
    for c in candidates:
        if c and Path(c).exists():
            return Path(c)
    return candidates[-1] if candidates else Path.cwd()


def _resolve_workspace():
    return _resolve_dir(
        env="FOXML_WORKSPACE",
        candidates=[Path(__file__).resolve().parent.parent, Path.cwd()],
    )


def _resolve_engine():
    # engine root = tools/.. (this file lives in <engine>/tools/)
    return Path(__file__).resolve().parent.parent


def _resolve_memory_dir():
    env = os.environ.get("FOXML_MEMORY_DIR")
    if env and Path(env).exists():
        return Path(env)
    derived = Path.home() / ".claude" / "projects" / "-home-caramel-code-FoxML-Trader-v2" / "memory"
    if derived.exists():
        return derived
    return None


def _resolve_skills_dir(engine, workspace):
    # skills live in workspace/claude-skills (symlinked to <engine>/.claude/skills)
    for c in [workspace / "claude-skills", engine / ".claude" / "skills"]:
        if c.exists():
            return c
    return None


# ---------------------------------------------------------------------------
# Check 1 — MEMORY.md index sync
# ---------------------------------------------------------------------------
def check_index_sync(quiet):
    mem = _resolve_memory_dir()
    if not mem:
        print("  [Check 1] SKIP — memory dir not found (set FOXML_MEMORY_DIR)")
        return 0
    index = mem / "MEMORY.md"
    if not index.exists():
        print(f"  [Check 1] SKIP — {index} not found")
        return 0
    idx_text = index.read_text(encoding="utf-8")
    orphans = []
    for f in sorted(mem.glob("*.md")):
        if f.name == "MEMORY.md":
            continue
        if not re.match(r"^(feedback_|user_|project_|reference_)", f.name):
            continue
        if f.name not in idx_text:
            orphans.append(f.name)
    if orphans:
        print(f"  [Check 1] FAIL — {len(orphans)} memory file(s) NOT indexed in MEMORY.md:")
        for o in orphans:
            print(f"             orphan: {o}")
        return 1
    if not quiet:
        print("  [Check 1] PASS — MEMORY.md index sync (all memory files indexed)")
    return 0


# ---------------------------------------------------------------------------
# Check 4 — decision-log sentinel matching (D/C/F id -> STATUS)
# ---------------------------------------------------------------------------
def check_sentinels(quiet, workspace):
    # scan all decision logs in the active sprint(s); a D/C/F block must be followed by a STATUS
    dlogs = list(workspace.glob("plans/**/decision-logs/*.md"))
    if not dlogs:
        if not quiet:
            print("  [Check 4] PASS — no decision logs found (nothing to check)")
        return 0
    fails = []
    for dl in dlogs:
        lines = dl.read_text(encoding="utf-8").splitlines()
        pending_id = None
        for i, ln in enumerate(lines):
            m = re.search(r"<!--\s*D/C/F:\s*([\w.-]+)\s*-->", ln)
            if m:
                # if a previous D/C/F id is still awaiting its STATUS, it's unmatched
                if pending_id is not None:
                    fails.append((dl.name, pending_id, "no STATUS before next D/C/F marker"))
                pending_id = m.group(1)
                continue
            if pending_id is not None and re.search(r"<!--\s*STATUS:", ln):
                pending_id = None
        if pending_id is not None:
            fails.append((dl.name, pending_id, "no STATUS before end of file"))
    if fails:
        print(f"  [Check 4] FAIL — {len(fails)} decision marker(s) without a matching STATUS:")
        for fn, did, why in fails:
            print(f"             {fn}: D/C/F {did} — {why}")
        return 1
    if not quiet:
        print(f"  [Check 4] PASS — decision sentinels matched across {len(dlogs)} decision log(s)")
    return 0


# ---------------------------------------------------------------------------
# Check 8 — skill -> CLAUDE.md suite linkage
# ---------------------------------------------------------------------------
def check_skill_linkage(quiet, engine, workspace):
    skills_dir = _resolve_skills_dir(engine, workspace)
    if not skills_dir:
        print("  [Check 8] SKIP — skills dir not found")
        return 0
    claude_md = engine / "CLAUDE.md"
    if not claude_md.exists():
        print(f"  [Check 8] SKIP — {claude_md} not found")
        return 0
    cmd_text = claude_md.read_text(encoding="utf-8")
    missing = []
    for sk in sorted(skills_dir.iterdir()):
        if not (sk / "SKILL.md").exists():
            continue
        name = sk.name
        # accept "/name" or "`/name`" or bare name token in the skill-suite tables
        if f"/{name}" not in cmd_text and f"`{name}`" not in cmd_text:
            missing.append(name)
    if missing:
        print(f"  [Check 8] FAIL — {len(missing)} skill(s) not referenced in CLAUDE.md:")
        for m in missing:
            print(f"             unlinked: /{m}")
        return 1
    if not quiet:
        print(f"  [Check 8] PASS — all skills linked in CLAUDE.md ({skills_dir})")
    return 0


def main():
    ap = argparse.ArgumentParser(description="Mechanical half of /capture-audit (Checks 1/4/8).")
    ap.add_argument("--check", type=int, choices=[1, 4, 8], help="run only one check")
    ap.add_argument("--quiet", action="store_true", help="only failures + summary")
    args = ap.parse_args()

    engine = _resolve_engine()
    workspace = _resolve_workspace()

    print("=== check_capture_audit.py — mechanical capture-audit checks ===")
    rc = 0
    if args.check in (None, 1):
        rc |= check_index_sync(args.quiet)
    if args.check in (None, 4):
        rc |= check_sentinels(args.quiet, workspace)
    if args.check in (None, 8):
        rc |= check_skill_linkage(args.quiet, engine, workspace)

    if rc:
        print("=== capture-audit MECHANICAL checks FAILED ===")
        sys.exit(1)
    print("=== capture-audit MECHANICAL checks PASS ===")
    sys.exit(0)


if __name__ == "__main__":
    main()
