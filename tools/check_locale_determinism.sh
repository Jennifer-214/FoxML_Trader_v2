#!/usr/bin/env bash
# tools/check_locale_determinism.sh — .E.0.1 locale-determinism GUARD (prevention).
#
# Enforces the single-locale-authority discipline (see locale-determinism-discipline.md):
#   (a) the boot pin EXISTS in each production main — the process is pinned
#       LC_NUMERIC=C at boot, the ONE locale authority.
#   (b) NO global setlocale() outside the boot pins — a global setlocale from a
#       non-boot thread is the render-thread race + SSoT violation .E.0.1 closed.
#       Tests are exempt (they deliberately flip locale to verify immunity); the
#       correct in-code pattern for defense-in-depth is thread-local uselocale().
#   (c) NO NEW raw atof/strtod/atoi beyond the KNOWN-PENDING baseline — the .F-paced
#       migration to the tt:: parse family. This list SHRINKS, never grows; a new raw
#       parse call (or a higher count in an existing file) is a red build.
#
# Exit 0 = GREEN. Exit 1 = violation. First run SEEDS the baseline.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; cd "$ROOT"
exec </dev/null   # .E.0.1 hang-class close: the rg's below pass explicit paths, but belt-and-suspenders — a detached run (run_in_background / CI / git hook) gets /dev/null stdin so no stdin-reader can hang it.
BASE="tools/locale_determinism_known_pending.txt"
rc=0

# NOTE: every rg below passes an EXPLICIT path ('.' or "$f"). A path-LESS rg reads
# stdin when stdin isn't a tty (detached / run_in_background / git pre-commit hook) and
# HANGS the gate forever (.E.0.1 close: a backgrounded run slept ~15min at 0% CPU in
# this exact spot). Keep the '.' — it also keeps the check non-vacuous (searches the tree).
BOOT_FILES='main.cpp foxml_suite.cpp GUI/GuiThread.hpp tools/compare_scalers.cpp'
# files excluded from (b)/(c): the boot pins themselves + tests (deliberate flips) + the gates.
EXCLUDE='main\.cpp|foxml_suite\.cpp|GuiThread\.hpp|compare_scalers\.cpp|tests/|controller_test|parity_harness|test_common|replay_locale_gate|fp_determinism|ParseFast'

echo "== (a) boot pins present =="
for f in $BOOT_FILES; do
  if rg -q 'setlocale\(LC_NUMERIC, *"C"\)' "$f" 2>/dev/null; then echo "  OK   $f"; else echo "  FAIL missing boot pin: $f"; rc=1; fi
done

echo "== (b) no stray GLOBAL setlocale (outside boot pins + tests) =="
stray=$(rg -n 'setlocale\s*\(' -g '*.cpp' -g '*.hpp' . 2>/dev/null | sed 's|^\./||' | grep -vE "$EXCLUDE")
if [ -n "$stray" ]; then
  echo "  FAIL stray global setlocale (use thread-local uselocale, or rely on the boot pin):"
  echo "$stray" | sed 's/^/    /'; rc=1
else echo "  OK   none"; fi

echo "== (c) raw atof/strtod/atoi manifest (KNOWN-PENDING baseline; new = violation) =="
cur=$(rg -c '\b(atof|strtod|strtof|atoi|atol|atoll)\s*\(' -g '*.cpp' -g '*.hpp' . 2>/dev/null | sed 's|^\./||' | grep -vE "$EXCLUDE" | sort)
if [ ! -f "$BASE" ]; then
  { echo "# .E.0.1 locale-determinism KNOWN-PENDING baseline (file:count of raw atof/strtod/atoi)."
    echo "# .F-paced migration to the tt:: parse family. This list SHRINKS, never grows."
    echo "# Regenerate deliberately when migrating a file (lower/remove its line)."
    echo "$cur"; } > "$BASE"
  echo "  SEEDED baseline ($(printf '%s\n' "$cur" | grep -c ':') files) -> $BASE"
else
  viol=0
  while IFS= read -r line; do
    [ -z "$line" ] && continue
    file="${line%%:*}"; ccount="${line##*:}"
    bline=$(grep -E "^${file}:" "$BASE" 2>/dev/null || true)
    if [ -z "$bline" ]; then echo "  FAIL NEW raw-parse file (not in baseline): $line"; viol=1
    else bcount="${bline##*:}"; if [ "$ccount" -gt "$bcount" ]; then echo "  FAIL raw-parse count rose in $file: $bcount -> $ccount"; viol=1; fi; fi
  done <<< "$cur"
  [ "$viol" -eq 0 ] && echo "  OK   no new raw atof/strtod/atoi beyond baseline" || rc=1
fi

[ "$rc" -eq 0 ] && echo "GREEN — locale-determinism guard clean." || echo "RED — locale-determinism violation (see above)."
exit "$rc"
