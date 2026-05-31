#!/usr/bin/env bash
# tools/check_determinism_selftest.sh — .E.0.1 NEGATIVE self-test: "does the net catch its regression?"
#
# THE NET THAT GUARDS THE NET. check_determinism.sh proves the tree is clean (GREEN); THIS proves the
# gates have TEETH — each one goes RED when its specific regression is injected. It is the structural
# close of the .E.0.1 §3 lesson: a CI gate verified only by GREEN-on-clean is NOT verified (the dead
# pre-commit hook ran clean for an unknown span because nobody had watched it FAIL on a real trigger).
# Discipline: a standing gate ships with a negative self-test that injects its target regression and
# asserts it's caught — incl. through the REAL trigger, not just direct invocation.
#
# Run on demand, from /post-ship-audit (standing pre-close step), or in CI. Every injection is sandboxed
# and SELF-REVERTING; a trap restores the tree on ANY exit. Run on a clean working tree.
#
# Coverage: (1) FP-golden corruption -> FP gate RED · (2) stray global setlocale -> locale gate RED ·
# (4) the REAL trigger: core.hooksPath still points at the canonical tracked hook AND that hook BLOCKS a
# staged determinism drift. (F-076 padding determinism is guarded at COMPILE time — a runtime
# characterization of "padding is deterministically zero" is fundamentally fragile, see codify notes —
# so it is not a runtime case here.)
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; cd "$ROOT"
exec </dev/null   # .E.0.1 hang-class close: this self-test runs the hook + git detached; /dev/null stdin keeps it (and its children) hang-proof.
GOLDEN="tools/fp_determinism_golden.txt"
SCRATCH="CoreFrameworks/_adversarial_selftest_scratch.hpp"
fail=0

cleanup() {  # belt-and-suspenders: never leave the tree dirty, whatever happens
  git reset -q HEAD "$GOLDEN" 2>/dev/null
  git checkout -- "$GOLDEN" 2>/dev/null
  rm -f "$SCRATCH"
}
trap cleanup EXIT

expect_red() {   # $1=label ; $2..=cmd  -> teeth iff cmd exits NON-zero on the injected regression
  local label="$1"; shift
  if "$@" >/dev/null 2>&1; then echo "  ❌ $label — gate stayed GREEN on an injected regression (NO TEETH)"; fail=1
  else echo "  ✅ $label — gate went RED on the injected regression"; fi
}
expect_green() { # $1=label ; $2..=cmd  -> healthy iff cmd exits zero on the clean tree
  local label="$1"; shift
  if "$@" >/dev/null 2>&1; then echo "  ✅ $label — GREEN on clean tree"
  else echo "  ❌ $label — RED on a CLEAN tree (false positive)"; fail=1; fi
}

echo "============================================================"
echo " determinism-net SELF-TEST (negative: each gate must catch its regression)"
echo "============================================================"

# (0) baseline — clean tree must be GREEN (else the injections below prove nothing)
expect_green "baseline FP gate"     ./tools/check_fp_determinism.sh
expect_green "baseline locale gate" ./tools/check_locale_determinism.sh

# (1) FP-golden drift — append a line the harness output can't reproduce
printf 'SELFTEST_CORRUPT_DO_NOT_COMMIT\n' >> "$GOLDEN"
expect_red   "(1) FP-golden corruption -> FP gate" ./tools/check_fp_determinism.sh
git checkout -- "$GOLDEN"
expect_green "(1) FP gate after restore"           ./tools/check_fp_determinism.sh

# (2) stray GLOBAL setlocale outside the boot pins (grep-gate; the scratch never compiles)
printf '#pragma once\n// .E.0.1 determinism-net self-test scratch — a stray global setlocale.\nstatic inline void _selftest_stray(){ setlocale(LC_NUMERIC, "de_DE"); }\n' > "$SCRATCH"
expect_red   "(2) stray setlocale -> locale gate" ./tools/check_locale_determinism.sh
rm -f "$SCRATCH"
expect_green "(2) locale gate after restore"      ./tools/check_locale_determinism.sh

# (4) THE REAL TRIGGER (the §3 lesson) — (a) git still points at the canonical tracked hook;
#     (b) that hook BLOCKS a staged determinism drift. Run the hook directly on a staged corruption;
#     no commit is created. Skipped (not failed) if the index is already dirty.
hp="$(git config --get core.hooksPath || echo '<unset>')"
if [ "$hp" = ".githooks" ]; then echo "  ✅ (4a) core.hooksPath = .githooks (canonical tracked hook is what git runs)"
else echo "  ❌ (4a) core.hooksPath = '$hp' (NOT .githooks — exactly the §3 dead-hook config drift)"; fail=1; fi
if git diff --cached --quiet 2>/dev/null; then
  printf 'SELFTEST_CORRUPT_DO_NOT_COMMIT\n' >> "$GOLDEN"; git add "$GOLDEN" 2>/dev/null
  if ./.githooks/pre-commit >/dev/null 2>&1; then echo "  ❌ (4b) pre-commit PASSED a staged determinism drift (Check F has no teeth)"; fail=1
  else echo "  ✅ (4b) pre-commit BLOCKED a staged determinism drift (Check F fires through the real hook)"; fi
  git reset -q HEAD "$GOLDEN" 2>/dev/null; git checkout -- "$GOLDEN"
else
  echo "  ⏭  (4b) SKIP real-trigger sub-test — index already has staged changes; re-run on a clean index"
fi

# (5) hang-class guard (the 74bd77b class — closed with a GUARD, not a comment): every detached-run
#     determinism script + the pre-commit hook MUST redirect stdin from /dev/null, so a path-less
#     rg/grep can never block them forever when run detached (run_in_background / CI / git hook).
for s in .githooks/pre-commit tools/check_determinism.sh tools/check_fp_determinism.sh \
         tools/check_locale_determinism.sh tools/check_determinism_selftest.sh; do
  if grep -q '</dev/null' "$s" 2>/dev/null; then echo "  ✅ (5) stdin-guard present: $s"
  else echo "  ❌ (5) MISSING stdin guard (exec </dev/null) -> detached-hang risk: $s"; fail=1; fi
done

echo ""
echo "============================================================"
[ "$fail" -eq 0 ] && echo " GREEN — determinism net HAS TEETH (gates catch regressions) + is HANG-PROOF (stdin guards present)." \
                  || echo " RED — a determinism gate did NOT catch its regression OR a stdin guard is missing (see above)."
echo "============================================================"
exit "$fail"
