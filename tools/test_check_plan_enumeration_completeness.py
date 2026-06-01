#!/usr/bin/env python3
"""Negative self-test for check_plan_enumeration_completeness.py (tool-verification discipline,
D-137 / meta-anti-pattern WH-4: a load-bearing guard ships a test that PROVES it has teeth —
inject the regression, assert it goes RED). Without this, the under-enumeration guard is itself
the un-verified tool it exists to catch."""
import subprocess
import sys
import tempfile
from pathlib import Path

TOOL = Path(__file__).resolve().parent / "check_plan_enumeration_completeness.py"


def run(plan_text, source_text, section="Relocation set", extra=None):
    with tempfile.TemporaryDirectory() as d:
        plan = Path(d) / "plan.md"
        src = Path(d) / "src.txt"
        plan.write_text(plan_text)
        src.write_text(source_text)
        cmd = [sys.executable, str(TOOL), "--plan", str(plan),
               "--section", section, "--source-file", str(src)] + (extra or [])
        return subprocess.run(cmd, capture_output=True, text=True)


SOURCE = "CoreFrameworks/Portfolio.hpp:180\nCoreFrameworks/Order.hpp:148\nML_Headers/GateControlNetwork.hpp:31\n"

PLAN_COMPLETE = """## Relocation set
- Portfolio.hpp:180 · Order.hpp:148 · GateControlNetwork.hpp:31
## Next section
"""
PLAN_DROPPED = """## Relocation set
- Portfolio.hpp:180  (summarized — Order.hpp + GCN dropped)
## Next section
"""


def main():
    fails = []

    # 1. POSITIVE: a complete plan section → exit 0
    r = run(PLAN_COMPLETE, SOURCE)
    if r.returncode != 0:
        fails.append(f"complete plan should pass (got rc={r.returncode})\n{r.stdout}")

    # 2. NEGATIVE (the teeth): a plan that dropped 2 of 3 files → exit 1 + names them
    r = run(PLAN_DROPPED, SOURCE)
    if r.returncode != 1:
        fails.append(f"dropped-file plan should FAIL rc=1 (got rc={r.returncode})\n{r.stdout}")
    if "Order.hpp" not in r.stdout or "GateControlNetwork.hpp" not in r.stdout:
        fails.append(f"should name the dropped files Order.hpp + GateControlNetwork.hpp\n{r.stdout}")

    # 3. --allow suppresses an intentional exclusion → back to exit 0
    r = run(PLAN_DROPPED, SOURCE, extra=["--allow", "Order.hpp", "GateControlNetwork.hpp"])
    if r.returncode != 0:
        fails.append(f"--allow of the excluded files should pass (got rc={r.returncode})\n{r.stdout}")

    if fails:
        print("FAIL — check_plan_enumeration_completeness has no/broken teeth:")
        for f in fails:
            print("  - " + f)
        sys.exit(1)
    print("PASS — guard has teeth: complete→0, dropped-file→1 (names them), --allow→0")


if __name__ == "__main__":
    main()
