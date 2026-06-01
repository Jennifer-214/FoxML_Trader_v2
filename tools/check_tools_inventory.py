#!/usr/bin/env python3
"""check_tools_inventory.py — two-way CI guard for the tools/ surface.

CHECK 1 (enrollment): every tools/*.{sh,py} on disk MUST have a row in DOCS/TOOLS.md
  (a tool can't exist without a disposition — the gen_code_map/CODE_MAP rot).
CHECK 2 (no broken refs): every `tools/X.{sh,py}` REFERENCED in a SKILL.md / hook /
  always-loaded doc / another tool MUST be accounted for — either on disk, OR enrolled
  as PLANNED (disclaimed future) / RETIRED (deleted, refs being cleaned). An untracked
  reference to a nonexistent tool = a mechanical check that silently never runs
  (the stamp_model.sh / check_amendment_cascade.py class).

Dispositions exempt from the on-disk requirement: PLANNED, RETIRED.
Closes the tool-rot class BOTH WAYS (D-134/D-135/D-136). Canonical-sister of check_meta_registry.py.
Exit 0 = clean. Exit 1 = unenrolled tool / stale row / broken (untracked-nonexistent) reference.

Machine-portable roots (per feedback_machine_portable_resolver_for_committed_tool_paths):
FOXML_REPO_ROOT / FOXML_WORKSPACE env overrides → else derived from this file + sibling default.
"""
import os
import re
import sys
import glob

REPO = (os.environ.get("FOXML_REPO_ROOT") or os.environ.get("FOXML_ENGINE")
        or os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOLS_DIR = os.path.join(REPO, "tools")
INVENTORY = os.path.join(REPO, "DOCS", "TOOLS.md")
BASELINE = os.path.join(TOOLS_DIR, "tools_verification_baseline.txt")  # D-137 known-pending-untested

# `name.sh|py` in col 1, disposition text in col 2
ROW_RE = re.compile(r"^\s*\|\s*`([A-Za-z0-9_]+\.(?:sh|py))`\s*\|\s*([^|]+?)\s*\|")
REF_RE = re.compile(r"tools/([A-Za-z0-9_]+\.(?:sh|py))")
OFF_DISK_DISPOSITIONS = ("PLANNED", "RETIRED")


def _workspace():
    ws = os.environ.get("FOXML_WORKSPACE")
    if ws and os.path.isdir(ws):
        return ws
    sib = os.path.join(os.path.dirname(REPO), "tick-trader-percore-workspace")
    return sib if os.path.isdir(sib) else None


def _on_disk():
    s = set()
    for ext in ("*.sh", "*.py"):
        for p in glob.glob(os.path.join(TOOLS_DIR, ext)):
            s.add(os.path.basename(p))
    return s


def _enrolled():
    """name -> disposition (uppercased, markdown stripped)."""
    d = {}
    with open(INVENTORY, encoding="utf-8") as f:
        for line in f:
            m = ROW_RE.match(line)
            if m:
                d[m.group(1)] = re.sub(r"[*`]", "", m.group(2)).strip().upper()
    return d


def _referenced():
    """name -> set of citing source labels."""
    refs = {}
    scan = []
    ws = _workspace()
    if ws:
        for root, _, files in os.walk(os.path.join(ws, "claude-skills")):
            scan += [os.path.join(root, fn) for fn in files if fn.endswith(".md")]
    hooks = os.path.join(REPO, ".githooks")
    if os.path.isdir(hooks):
        for root, _, files in os.walk(hooks):
            scan += [os.path.join(root, fn) for fn in files]
    for fn in ("CLAUDE.md", "CLAUDE.local.md"):
        fp = os.path.join(REPO, fn)
        if os.path.isfile(fp):
            scan.append(fp)
    for ext in ("*.sh", "*.py"):
        scan += glob.glob(os.path.join(TOOLS_DIR, ext))
    for f in scan:
        if os.path.basename(f).startswith("test_"):
            continue  # test harnesses contain fake tool refs as test PAYLOADS, not real invocations
        try:
            with open(f, encoding="utf-8", errors="ignore") as fh:
                txt = fh.read()
        except Exception:
            continue
        label = os.path.basename(os.path.dirname(f)) if f.endswith("SKILL.md") else os.path.basename(f)
        for m in REF_RE.finditer(txt):
            refs.setdefault(m.group(1), set()).add(label)
    return refs


def _tested(on_disk):
    """name -> has a negative test (test_X.py / X_selftest.sh / an own expect_red marker)."""
    t = set()
    for name in on_disk:
        base = name.rsplit(".", 1)[0]
        if f"test_{base}.py" in on_disk or f"{base}_selftest.sh" in on_disk:
            t.add(name)
            continue
        try:
            with open(os.path.join(TOOLS_DIR, name), encoding="utf-8", errors="ignore") as fh:
                if re.search(r"expect_red|NO TEETH|has teeth", fh.read()):
                    t.add(name)
        except Exception:
            pass
    return t


def main():
    if not os.path.isfile(INVENTORY):
        print("[check_tools_inventory] FAIL — inventory missing: DOCS/TOOLS.md")
        return 1
    on_disk = _on_disk()
    enrolled = _enrolled()
    tracked = set(enrolled)
    off_disk_ok = {n for n, dp in enrolled.items() if any(k in dp for k in OFF_DISK_DISPOSITIONS)}
    real_rows = tracked - off_disk_ok
    rc = 0

    # CHECK 1 — enrollment
    unenrolled = sorted(on_disk - tracked)
    stale = sorted(real_rows - on_disk)  # PLANNED/RETIRED rows are intentionally not-on-disk
    if unenrolled:
        print(f"[check_tools_inventory] FAIL — {len(unenrolled)} tool(s) NOT enrolled in DOCS/TOOLS.md:")
        for t in unenrolled:
            print(f"    {t}  → add a row with a disposition")
        rc = 1
    if stale:
        print(f"[check_tools_inventory] FAIL — {len(stale)} real-disposition row(s) for tools that don't exist:")
        for t in stale:
            print(f"    {t}  → remove the row, or mark it RETIRED if refs linger")
        rc = 1

    # CHECK 2 — no broken (untracked-nonexistent) references
    broken = {t: srcs for t, srcs in _referenced().items() if t not in on_disk and t not in tracked}
    if broken:
        print(f"[check_tools_inventory] FAIL — {len(broken)} reference(s) to an untracked nonexistent tool:")
        for t in sorted(broken):
            print(f"    tools/{t}  ← cited by {', '.join(sorted(broken[t]))}  → build it, fix the ref, or enroll PLANNED/RETIRED")
        rc = 1

    # CHECK 3 — load-bearing tools must be VERIFIED (have a negative self-test) — D-137
    load_bearing = {n for n, dp in enrolled.items() if ("STANDING-CI" in dp or "SKILL-WIRED" in dp)} & on_disk
    tested = _tested(on_disk)
    baseline = set()
    if os.path.isfile(BASELINE):
        with open(BASELINE, encoding="utf-8") as f:
            baseline = {ln.strip() for ln in f if ln.strip() and not ln.lstrip().startswith("#")}
    untested = load_bearing - tested
    new_untested = sorted(untested - baseline)
    pending = sorted(untested & baseline)
    if new_untested:
        print(f"[check_tools_inventory] FAIL — {len(new_untested)} load-bearing tool(s) with NO negative self-test (D-137):")
        for t in new_untested:
            b = t.rsplit(".", 1)[0]
            print(f"    {t}  → add test_{b}.py or {b}_selftest.sh (prove it goes RED on its target), or baseline it w/ rationale")
        rc = 1

    if rc == 0:
        print(f"[check_tools_inventory] CLEAN — {len(on_disk)} tools enrolled; "
              f"{len(off_disk_ok)} PLANNED/RETIRED tracked; no broken refs; "
              f"{len(tested)} verified, {len(pending)} known-pending a self-test (D-137 shrinking).")
    return rc


if __name__ == "__main__":
    sys.exit(main())
