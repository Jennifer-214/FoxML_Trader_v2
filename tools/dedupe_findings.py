#!/usr/bin/env python3
"""
dedupe_findings.py — Dedup + aggregate the pre-implementation findings sidecars
into one canonical, deduped index.

WHY: the per-ship sidecars (E.1/E.2/E.3/E.5/E.6/PRE-PAPER-TEST/BACKLOG-STANDALONE) are
NOT deduped. Finding IDs both COLLIDE (the same id labels different findings, e.g.
`fpmem-1` = sqrt-determinism AND strict-aliasing) and SPLIT (the same finding appears
under different ids / in multiple files, e.g. strict-aliasing as both `fpmem-1` and
`fpmem-2`; PRE-PAPER-TEST re-lists per-ship findings). So the IDs are unreliable as
identity. This tool dedups on CONTENT (title-token similarity + primary code location),
reports the real distinct count + the collision/dup structure, and emits a LOSSLESS
canonical index — every source occurrence is cited under its canonical entry, so a wrong
merge is recoverable and nothing is silently dropped.

Conservative by design: it SURFACES dup-groups for confirmation; it never deletes a
source occurrence. Merges only on strong signals (high title similarity, OR shared code
location + moderate title similarity, OR same-id + same-location).

Usage:
  python3 tools/dedupe_findings.py                          # report to stdout
  python3 tools/dedupe_findings.py --emit-index <path.md>   # also write canonical index
  python3 tools/dedupe_findings.py --sim 0.5 --json
"""
import argparse, glob, os, re, json
from collections import defaultdict, Counter

DEFAULT_DIR = "plans/v5.15-live-readiness/plan_checks/E.0-audit-reports/pre-implementation-findings"

SEVS = {"CRITICAL": 4, "HIGH": 3, "MEDIUM": 2, "MED": 2, "LOW": 1, "INFO": 0}
SEV_NAME = {4: "CRITICAL", 3: "HIGH", 2: "MEDIUM", 1: "LOW", 0: "INFO"}

HEADER_RE  = re.compile(r'^#{2,4}\s*\[(?P<sev>[A-Za-z]+)\]\s*`?(?P<id>[A-Za-z0-9._\-]+)`?\s*[—\-:]\s*(?P<title>.+?)\s*$')
SECTION_RE = re.compile(r'section\s+`?(?P<s>[A-Za-z0-9._\-/]+)`?')
FIX_RE     = re.compile(r'\bfix:\s*(?P<f>[^·\n*]+)')
LOC_RE     = re.compile(r'([A-Za-z0-9_]+(?:/[A-Za-z0-9_]+)*\.[ch]pp(?::\d+)?)')   # File.hpp[:line]
SHIP_RE    = re.compile(r'\.E\.[0-9X]+|standalone|\.F\b|paper-?test', re.I)

STOP = set("the a an of to in on for and or is are be by with via as at from this that not no it its "
           "when while which whose then than but also only ever both each into under over per would "
           "could should can may might will shall has have had does do done been being".split())

def toks(text):
    return set(w for w in re.findall(r'[a-z0-9_]+', text.lower()) if w not in STOP and len(w) > 2)

def jac(a, b):
    return len(a & b) / len(a | b) if a and b else 0.0

def norm_ship(fix):
    m = SHIP_RE.search(fix or "")
    if not m:
        return "(unrouted)"
    t = m.group(0).lower()
    if "paper" in t: return "PRE-PAPER-TEST"
    if t == ".f":    return ".F"
    if "standalone" in t: return "standalone(pre-.E.1)"
    return t

def parse_file(path):
    out = []
    txt = open(path, encoding="utf-8").read().splitlines()
    fname = os.path.basename(path)
    i = 0
    while i < len(txt):
        m = HEADER_RE.match(txt[i])
        if m and m.group("sev").upper() in SEVS:
            sev = m.group("sev").upper()
            sev = "MEDIUM" if sev == "MED" else sev
            j = i + 1
            body = []
            while j < len(txt):
                mj = HEADER_RE.match(txt[j])
                if mj and mj.group("sev").upper() in SEVS:
                    break
                body.append(txt[j]); j += 1
            btext = "\n".join(body)
            sm = SECTION_RE.search(btext); fm = FIX_RE.search(btext)
            section = sm.group("s") if sm else ""
            fix = fm.group("f").strip() if fm else ""
            wm = re.search(r'\*\*Where:\*\*(.+?)(?:\n-\s+\*\*|\Z)', btext, re.S)
            wtext = wm.group(1) if wm else btext[:400]
            locs = LOC_RE.findall(wtext)
            out.append(dict(
                id=m.group("id"), sev=sev, title=m.group("title").strip(),
                section=section, fix=fix, ship=norm_ship(fix),
                loc=(locs[0] if locs else ""), file=fname, line=i + 1,
                ttok=toks(m.group("title")), wtok=toks(wtext[:400]),
            ))
            i = j
        else:
            i += 1
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=DEFAULT_DIR)
    ap.add_argument("--emit-index", default=None)
    ap.add_argument("--sim", type=float, default=0.5)
    ap.add_argument("--merge-map", default=None,
                    help="file of eyes-on confirmed-dup groups (one group/line: short:line tokens)")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()

    files = sorted(glob.glob(os.path.join(a.dir, "*findings*.md")))
    allf, per_file = [], Counter()
    for p in files:
        ff = parse_file(p)
        per_file[os.path.basename(p)] = len(ff)
        allf += ff
    n = len(allf)

    by_id = defaultdict(list)
    for k, f in enumerate(allf):
        by_id[f["id"]].append(k)
    collisions = {k: v for k, v in by_id.items() if len(v) > 1}

    # union-find content dedup
    parent = list(range(n))
    def find(x):
        r = x
        while parent[r] != r: r = parent[r]
        while parent[x] != r: parent[x], x = r, parent[x]
        return r
    def uni(x, y):
        rx, ry = find(x), find(y)
        if rx != ry: parent[rx] = ry
    for x in range(n):
        fx = allf[x]
        for y in range(x + 1, n):
            fy = allf[y]
            ts = jac(fx["ttok"], fy["ttok"])
            same_loc = bool(fx["loc"]) and fx["loc"] == fy["loc"]
            # Merge on CONTENT only — NEVER on id (ids collide AND split, so id is not identity;
            # using it as a merge signal is what over-merged the fixedpoint cluster). Loc-boost
            # requires a line-qualified location (File.hpp:NNN) so distinct findings in the same
            # big file (e.g. FixedPointN.hpp) don't collapse. Bias toward OVER-counting: a wrong
            # split is just an extra entry to reconcile at the dive; a wrong merge could hide a
            # finding. Lossless either way — every source occurrence is cited under its group.
            if ts >= a.sim or (same_loc and ":" in fx["loc"] and ts >= 0.30):
                uni(x, y)

    # operator-confirmed merge-map (eyes-on reconciliation of colliding ids) — forced unions.
    # Each non-comment line lists `<short-file>:<line>` tokens that are the SAME finding.
    # Anything not listed stays a distinct entry. Still lossless: all sources cited under the group.
    forced = 0
    if a.merge_map and os.path.exists(a.merge_map):
        key = {}
        for k, f in enumerate(allf):
            sf = f["file"].replace("-findings.md", "").replace(".md", "")
            key[f"{sf}:{f['line']}"] = k
        for raw in open(a.merge_map, encoding="utf-8"):
            ks = [t for t in raw.split("#")[0].split() if t in key]
            for t in ks[1:]:
                uni(key[ks[0]], key[t]); forced += 1

    groups = defaultdict(list)
    for k in range(n): groups[find(k)].append(k)
    glist = sorted(groups.values(),
                   key=lambda g: (-max(SEVS[allf[i]["sev"]] for i in g), allf[g[0]]["section"]))
    distinct = len(glist)

    def group_sev(g): return max(SEVS[allf[i]["sev"]] for i in g)
    def group_title(g): return max((allf[i]["title"] for i in g), key=len)

    sev_dist = Counter(SEV_NAME[group_sev(g)] for g in glist)
    ship_dist = Counter(allf[sorted(g, key=lambda i:-SEVS[allf[i]["sev"]])[0]]["ship"] for g in glist)
    dup_groups = [g for g in glist if len(g) > 1]
    collapsed = sum(len(g) - 1 for g in dup_groups)

    # ---- report ----
    print(f"=== dedupe_findings: {a.dir} ===")
    print(f"files: {len(files)} | raw finding headers: {n} | DISTINCT (content-deduped): {distinct}  "
          f"({collapsed} duplicate occurrences collapsed across {len(dup_groups)} groups)")
    if forced:
        print(f"  (incl. {forced} operator-confirmed merges from {a.merge_map})")
    print("per-file raw: " + "  ".join(f"{k.replace('-findings.md','').replace('.md','')}={v}"
                                       for k, v in sorted(per_file.items())))
    print("severity (distinct): " + "  ".join(f"{s}={sev_dist.get(s,0)}"
          for s in ("CRITICAL", "HIGH", "MEDIUM", "LOW", "INFO")))
    print("fix_ship (distinct): " + "  ".join(f"{k}={v}" for k, v in sorted(ship_dist.items(), key=lambda x:-x[1])))

    print(f"\n--- ID COLLISIONS: {len(collisions)} ids label >1 finding (ids UNRELIABLE as identity) ---")
    for cid, idxs in sorted(collisions.items(), key=lambda x:-len(x[1])):
        print(f"  {cid} ×{len(idxs)}: " + " | ".join(f"{allf[i]['file'].split('-')[0]}:{allf[i]['line']}" for i in idxs))

    print(f"\n--- CONTENT DUP-GROUPS: {len(dup_groups)} findings appear more than once ---")
    for g in dup_groups:
        rep = group_title(g)[:88]
        print(f"  [{SEV_NAME[group_sev(g)]}] ×{len(g)}  {rep}")
        print("        " + ", ".join(f"{allf[i]['file'].replace('-findings.md','').replace('.md','')}:{allf[i]['line']}({allf[i]['id']})"
                                     for i in sorted(g, key=lambda i: (allf[i]['file'], allf[i]['line']))))

    if a.json:
        print("\n" + json.dumps(dict(raw=n, distinct=distinct, collapsed=collapsed,
              id_collisions=len(collisions), sev=dict(sev_dist), ship=dict(ship_dist)), indent=2))

    # ---- canonical index ----
    if a.emit_index:
        lines = []
        lines.append("---")
        lines.append("type: canonical-findings-index")
        lines.append("source: tools/dedupe_findings.py (content-dedup over the per-ship sidecars)")
        lines.append("note: LOSSLESS — every source occurrence is cited; merges are content-based + recoverable")
        lines.append(f"raw_headers: {n}")
        lines.append(f"distinct_findings: {distinct}")
        lines.append("---\n")
        lines.append("# Canonical (deduped) pre-implementation findings index\n")
        lines.append(f"**{n} raw sidecar headers → {distinct} distinct findings** "
                     f"({collapsed} duplicate occurrences collapsed). IDs reassigned canonically (`F-NNN`) "
                     f"because the original sidecar ids both collide and split. Each entry lists ALL source "
                     f"occurrences (file:line + original-id) so nothing is dropped.\n")
        lines.append("> Generated by `tools/dedupe_findings.py`. Re-run after any sidecar edit.\n")
        # group by fix_ship bucket for the per-plan dives
        ship_order = [".e.1", ".e.2", ".e.3", ".e.5", ".e.6", "standalone(pre-.e.1)",
                      "PRE-PAPER-TEST", ".F", "(unrouted)"]
        def ship_key(g):
            s = allf[sorted(g, key=lambda i:-SEVS[allf[i]["sev"]])[0]]["ship"].lower()
            return ship_order.index(s) if s in ship_order else len(ship_order)
        cid = 0
        for g in sorted(glist, key=lambda g: (ship_key(g), -group_sev(g))):
            cid += 1
            rep = sorted(g, key=lambda i:-SEVS[allf[i]["sev"]])[0]
            f = allf[rep]
            srcs = ", ".join(f"`{allf[i]['file'].replace('-findings.md','').replace('.md','')}:{allf[i]['line']}`({allf[i]['id']})"
                             for i in sorted(g, key=lambda i: (allf[i]['file'], allf[i]['line'])))
            secs = "/".join(sorted({allf[i]['section'] for i in g if allf[i]['section']}))
            ids = sorted({allf[i]['id'] for i in g})
            flag = ""
            if len(ids) > 1: flag += " ⚠SPLIT-IDs"
            if any(allf[i]['id'] in collisions for i in g): flag += " ⚠COLLIDING-ID"
            lines.append(f"### F-{cid:03d} [{SEV_NAME[group_sev(g)]}] {f['title']}")
            lines.append(f"- **fix_ship:** `{f['ship']}` · **section:** `{secs or '?'}` · **occurrences:** ×{len(g)}{flag}")
            lines.append(f"- **orig ids:** {', '.join('`'+x+'`' for x in ids)}")
            lines.append(f"- **sources:** {srcs}\n")
        open(a.emit_index, "w", encoding="utf-8").write("\n".join(lines))
        print(f"\n[emit] canonical index written: {a.emit_index}  ({distinct} entries)")

if __name__ == "__main__":
    main()
