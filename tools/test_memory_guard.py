#!/usr/bin/env python3
"""test_memory_guard.py — regression fixture for the memory bidirectional guard (D-89).

Proves `check_doc_metadata.py --bidirectional --memories` red-builds the CP-1/WH-1
family for memories: one-way sister -> RED, reciprocal -> GREEN, broken ref -> RED,
undefined tag -> RED, clean -> GREEN. Builds isolated fixtures via $FOXML_MEMORY_DIR
so it never touches the real memory store. This is the permanent proof the plan's
"Tests changed" section asked for (the manual dogfood, made a standing regression test).

Run: python3 tools/test_memory_guard.py   (exit 0 = all pass; 1 = a case regressed)
"""
import os
import sys
import subprocess
import tempfile
from pathlib import Path

TOOL = str(Path(__file__).resolve().parent / "check_doc_metadata.py")


def run_guard(memdir):
    """Run the guard against memdir via FOXML_MEMORY_DIR; return exit code (0=GREEN, !=0=RED)."""
    env = dict(os.environ, FOXML_MEMORY_DIR=str(memdir))
    r = subprocess.run(
        [sys.executable, TOOL, "--bidirectional", "--memories"],
        env=env, capture_output=True, text=True,
    )
    return r.returncode


def write_mem(memdir, name, tags, sisters, body="A test fact.\n"):
    fm = (
        f"---\nname: {name}\ndescription: test fixture\nmetadata:\n"
        f"  type: feedback\n  tags: [{', '.join(tags)}]\n"
        f"  sister_specs: [{', '.join(sisters)}]\n---\n{body}"
    )
    (memdir / f"{name}.md").write_text(fm, encoding="utf-8")


# Each case: (description, expect_red, setup-fn). 'audit-methodology' is a real vocab tag.
CASES = [
    ("reciprocal sisters -> GREEN", False, lambda d: (
        write_mem(d, "feedback_a", ["audit-methodology"], ["feedback_b.md"]),
        write_mem(d, "feedback_b", ["audit-methodology"], ["feedback_a.md"]))),
    ("one-way sister -> RED", True, lambda d: (
        write_mem(d, "feedback_a", ["audit-methodology"], ["feedback_b.md"]),
        write_mem(d, "feedback_b", ["audit-methodology"], []))),
    ("broken sister ref -> RED", True, lambda d: (
        write_mem(d, "feedback_a", ["audit-methodology"], ["feedback_nonexistent.md"]),)),
    ("undefined tag -> RED", True, lambda d: (
        write_mem(d, "feedback_a", ["totally-not-a-real-tag-xyz"], []),)),
    ("clean, no sisters -> GREEN", False, lambda d: (
        write_mem(d, "feedback_a", ["audit-methodology"], []),)),
]


def main():
    fails = 0
    for desc, expect_red, setup in CASES:
        with tempfile.TemporaryDirectory() as td:
            d = Path(td)
            setup(d)
            rc = run_guard(d)
            got_red = rc != 0
            ok = (got_red == expect_red)
            print(f"[{'PASS' if ok else 'FAIL'}] {desc} (rc={rc})")
            if not ok:
                fails += 1
    total = len(CASES)
    print(f"\n{total - fails}/{total} passed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
