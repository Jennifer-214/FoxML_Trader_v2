#!/usr/bin/env python3
"""
check_tech_debt.py — automate tech-debt SURFACING so managing it needs ~zero standing thought.

THE FRICTION THIS KILLS
-----------------------
The only tech-debt enforcement was `/readiness` Check 25 — a MANUAL scan: "read the OPEN entries
(80+, 3000+ lines), match each entry's surface against your files-touched by hand, decide." Miss a
match and the debt stays hidden — the exact failure Check 25 exists to prevent. This tool mechanizes
the SURFACING: given the files a commit/ship touches, it hands you the overlapping OPEN entries. You
only DECIDE — the finding is automatic.

Wired into the pre-commit hook (`--staged`, ADVISORY/non-blocking), every commit that touches a
tracked debt surface auto-prints "you're in TECH_DEBT-N's surface — subsume / adjacent / defer?"
(per feedback_opportunistic_tech_debt_closure). You stop scanning; the system surfaces.

Closing stays HUMAN (judgment — NEVER auto-close debt; that hides unresolved work). This automates
find + flag-stale + the close-MOVE, not the decision.

MODES
  --staged            match git staged files (the pre-commit ADVISORY; non-blocking, exit 0 always)
  --surface F [F...]  match an explicit file set (a ship's files-touched)
  --diff <ref>        match files changed since <ref> (e.g. the ship's pre-tag)
  --stale [MONTHS]    OPEN entries opened > MONTHS ago (default 6) → flag for re-cost/refresh
  --close N           move TECH_DEBT-N from open.md to closed.md (stamps `closed:`), verify the move
  --dry-run           with --close: print what would move, write nothing
  (default mode: --staged)

Advisory by design: --staged/--surface/--diff ALWAYS exit 0 (inform, never block a commit). --stale
exits 1 only under --strict. Machine-portable: repo root from $FOXML_REPO_ROOT or derived; the
tech-debt dir is resolved by following the DOCS/TECH_DEBT.md symlink into the workspace.
"""
import os, re, sys, subprocess, datetime

REPO_ROOT = os.environ.get("FOXML_REPO_ROOT") or os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


def _tech_debt_dir():
    """Resolve the tech-debt sub-file dir — it lives in the workspace; follow the DOCS/TECH_DEBT.md symlink."""
    idx = os.path.join(REPO_ROOT, "DOCS", "TECH_DEBT.md")
    if os.path.exists(idx):
        d = os.path.join(os.path.dirname(os.path.realpath(idx)), "tech-debt")
        if os.path.isdir(d):
            return d
    for cand in (os.path.join(REPO_ROOT, "DOCS", "tech-debt"), os.environ.get("FOXML_TECH_DEBT_DIR", "")):
        if cand and os.path.isdir(cand):
            return cand
    sys.exit("[tech-debt] dir not found — resolve DOCS/TECH_DEBT.md symlink or set FOXML_TECH_DEBT_DIR")


def _entries(path):
    """Parse '### TECH_DEBT-NNN — title' blocks → {id, title, tags, opened, status, body}."""
    with open(path) as f:
        text = f.read()
    parts = re.split(r'(?m)^### (TECH_DEBT-\d+)\b', text)  # [pre, id, body, id, body, ...]
    out = []
    for i in range(1, len(parts), 2):
        tid, body = parts[i], (parts[i + 1] if i + 1 < len(parts) else "")
        first = body.splitlines()[0] if body.strip() else ""
        title = first.lstrip(" —-").strip()
        tags_m = re.search(r'surface_tags:\s*\[([^\]]*)\]', body)
        tags = [t.strip() for t in tags_m.group(1).split(',') if t.strip()] if tags_m else []
        opened_m = re.search(r'opened:\s*([0-9]{4}-[0-9]{2}-[0-9]{2})', body)
        status_m = re.search(r'status:\s*([^\n]+)', body)
        out.append({"id": tid, "title": title, "tags": tags,
                    "opened": opened_m.group(1) if opened_m else "",
                    "status": status_m.group(1).strip() if status_m else "", "body": body})
    return out


_PATH_RE = re.compile(r'[\w./-]+\.(?:hpp|cpp|h|c|sh|py|md|txt)\b')


def _basenames(files):
    return {os.path.basename(f.strip()) for f in files if f.strip()}


def _cited_basenames(body):
    return {os.path.basename(t) for t in _PATH_RE.findall(body)}


def _git(args):
    r = subprocess.run(["git", *args], cwd=REPO_ROOT, capture_output=True, text=True)
    return [l for l in r.stdout.splitlines() if l.strip()]


def surface(files):
    """ADVISORY: print OPEN entries whose body cites a file you touched. Always exit 0."""
    touched = _basenames(files)
    if not touched:
        return 0
    hits = []
    for e in _entries(os.path.join(_tech_debt_dir(), "open.md")):
        ov = touched & _cited_basenames(e["body"])
        if ov:
            hits.append((e, sorted(ov)))
    if not hits:
        return 0
    print(f"[tech-debt] {len(hits)} OPEN item(s) overlap your touched files — classify each "
          f"(subsume → close now / adjacent → cross-link + leave tracked / defer → confirm trigger;\n"
          f"            per feedback_opportunistic_tech_debt_closure + /readiness Check 25):")
    for e, ov in hits:
        print(f"  • {e['id']} — {e['title']}")
        print(f"      matched on: {', '.join(ov)}")
    return 0  # advisory — NEVER blocks a commit/ship


def stale(months, strict):
    cutoff = datetime.date.today() - datetime.timedelta(days=int(months) * 30)
    flagged = []
    for e in _entries(os.path.join(_tech_debt_dir(), "open.md")):
        if not e["opened"]:
            continue
        try:
            opened = datetime.date.fromisoformat(e["opened"])
        except ValueError:
            continue
        if opened < cutoff:
            flagged.append((e, (datetime.date.today() - opened).days))
    if not flagged:
        print(f"[tech-debt] no OPEN entries older than {months} months without refresh.")
        return 0
    print(f"[tech-debt] {len(flagged)} OPEN entries opened > {months} months ago — refresh cost/trigger or close:")
    for e, age in sorted(flagged, key=lambda x: -x[1]):
        print(f"  • {e['id']} ({age//30}mo) — {e['title']}")
    return 1 if strict else 0


def close(n, dry_run):
    d = _tech_debt_dir()
    open_p, closed_p = os.path.join(d, "open.md"), os.path.join(d, "closed.md")
    with open(open_p) as f:
        text = f.read()
    m = re.search(r'(?m)^### TECH_DEBT-' + re.escape(str(n)) + r'\b', text)
    if not m:
        sys.exit(f"[tech-debt] TECH_DEBT-{n} not found in open.md (already closed? wrong id?)")
    nxt = re.search(r'(?m)^### TECH_DEBT-\d+\b', text[m.end():])
    end = m.end() + nxt.start() if nxt else len(text)
    block = text[m.start():end].rstrip() + "\n"
    today = datetime.date.today().isoformat()
    stamped = re.sub(r'(?m)^(status:\s*).*$', r'\1closed', block, count=1)
    stamped = stamped.rstrip() + f"\n- **Closed:** {today} (moved open → closed via check_tech_debt.py --close)\n"
    if dry_run:
        print(f"[tech-debt] DRY-RUN would move TECH_DEBT-{n} (open → closed.md), stamping closed:{today}:\n")
        print(block)
        return 0
    with open(open_p, "w") as f:
        f.write(text[:m.start()] + text[end:])
    with open(closed_p, "a") as f:
        f.write("\n" + stamped)
    # verify
    moved_out = re.search(r'(?m)^### TECH_DEBT-' + re.escape(str(n)) + r'\b', open(open_p).read()) is None
    moved_in = re.search(r'(?m)^### TECH_DEBT-' + re.escape(str(n)) + r'\b', open(closed_p).read()) is not None
    if moved_out and moved_in:
        print(f"[tech-debt] TECH_DEBT-{n} moved open → closed.md (closed:{today}). Verify the closure is real before committing.")
        return 0
    sys.exit(f"[tech-debt] move of TECH_DEBT-{n} FAILED verification (out={moved_out} in={moved_in}) — inspect manually.")


def main(argv):
    strict = "--strict" in argv
    dry = "--dry-run" in argv
    if "--surface" in argv:
        i = argv.index("--surface")
        return surface([a for a in argv[i + 1:] if not a.startswith("-")])
    if "--diff" in argv:
        ref = argv[argv.index("--diff") + 1]
        return surface(_git(["diff", "--name-only", ref]))
    if "--stale" in argv:
        i = argv.index("--stale")
        months = argv[i + 1] if i + 1 < len(argv) and argv[i + 1].isdigit() else "6"
        return stale(months, strict)
    if "--close" in argv:
        return close(argv[argv.index("--close") + 1], dry)
    # default: --staged
    return surface(_git(["diff", "--cached", "--name-only"]))


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
