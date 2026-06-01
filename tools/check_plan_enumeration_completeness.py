#!/usr/bin/env python3
"""check_plan_enumeration_completeness.py — a plan's claimed site-set ⊇ the tool's output.

Mechanizes the AR-1 slip (meta-anti-pattern-index): a plan body claims an ENUMERATION
(a relocation set / fee-site set / boundary-cast set), the author RUNS the code-intelligence
tool (gen_code_map / rg) but then HAND-SUMMARIZES its output into the plan — dropping members.
The tool was run; the summary lost members. (#11 step-6 fold: gen_code_map --byte-context FPN
emitted ~18 layout-assert sites; the fold pasted 6. Order.hpp/ExecutionCore.hpp dropped entirely.)

This check makes summarize-and-drop a RED BUILD: every FILE the source tool emits MUST appear
in the named plan section (file-level = the strong check — a dropped file is the real slip);
file:line points missing are reported as advisory (line-granularity).

Usage:
  check_plan_enumeration_completeness.py --plan <md> --section "<header substring>" \
      --source-cmd "<shell cmd emitting file:line tokens>" [--allow <basename> ...]
  # or --source-file <path> to read tokens from a captured file instead of running a cmd

Exit: 0 = every source FILE is present in the plan section · 1 = a source file is MISSING
      (under-enumeration) · 2 = usage / IO error.

Cross-refs: meta-anti-pattern-index AR-1 · gen_code_map.sh · feedback_enumerate_set_before_categorical_claim
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

# file:line token, e.g. CoreFrameworks/ShardedSnapshotPersist.hpp:180  OR  Order.hpp:148
TOKEN = re.compile(r"[\w./\-]+\.(?:hpp|cpp|h|cc|py|sh):\d+")
# bare filename, e.g. Order.hpp  (so a plan that names the FILE without a :line still counts)
FILEREF = re.compile(r"[\w\-]+\.(?:hpp|cpp|h|cc|py|sh)")


def basename(tok: str) -> str:
    return tok.split("/")[-1].split(":")[0]


def extract_tokens(text: str):
    """Return (files set, file:line points set) — files keyed by basename."""
    points = {f"{basename(m)}:{m.rsplit(':',1)[1]}" for m in TOKEN.findall(text)}
    files = {basename(m) for m in TOKEN.findall(text)} | set(FILEREF.findall(text))
    return files, points


def plan_section(plan_path: Path, header_sub: str) -> str:
    lines = plan_path.read_text().splitlines()
    out, capturing = [], False
    for ln in lines:
        if ln.startswith("#") and header_sub.lower() in ln.lower():
            capturing = True
            continue
        if capturing and ln.startswith("## "):   # next top section ends the block
            break
        if capturing:
            out.append(ln)
    if not capturing:
        sys.exit(f"[err] section matching '{header_sub}' not found in {plan_path}")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--plan", required=True)
    ap.add_argument("--section", required=True, help="substring of the plan header whose body holds the claimed set")
    ap.add_argument("--source-cmd", help="shell cmd emitting file:line tokens (e.g. gen_code_map --byte-context FPN)")
    ap.add_argument("--source-file", help="read source tokens from a captured file instead")
    ap.add_argument("--allow", nargs="*", default=[], help="basenames intentionally excluded from the plan set")
    a = ap.parse_args()

    if a.source_cmd:
        src = subprocess.run(a.source_cmd, shell=True, capture_output=True, text=True).stdout
    elif a.source_file:
        src = Path(a.source_file).read_text()
    else:
        sys.exit("[err] need --source-cmd or --source-file")

    src_files, src_points = extract_tokens(src)
    body = plan_section(Path(a.plan), a.section)
    plan_files, plan_points = extract_tokens(body)
    allow = set(a.allow)

    missing_files = sorted(f for f in src_files if f not in plan_files and f not in allow)
    missing_points = sorted(p for p in src_points if p not in plan_points and basename(p) not in allow)

    print(f"source files: {len(src_files)} · plan-section files: {len(plan_files)} · "
          f"allow: {len(allow)}")
    if missing_files:
        print(f"\n❌ {len(missing_files)} source FILE(s) absent from the plan section "
              f"(under-enumeration — summarize-and-drop):")
        for f in missing_files:
            print(f"    - {f}")
    if missing_points:
        print(f"\n⚠  {len(missing_points)} source file:line point(s) not in the plan section (advisory):")
        for p in missing_points[:40]:
            print(f"    - {p}")
    if not missing_files:
        print("\n✅ COMPLETE — every source file is named in the plan section.")
    sys.exit(1 if missing_files else 0)


if __name__ == "__main__":
    main()
