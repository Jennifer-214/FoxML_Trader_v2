#!/usr/bin/env bash
# NEGATIVE self-test for check_tech_debt.py (the tech-debt surface advisory) — proves it has TEETH.
# --surface is ADVISORY (exit 0 by design), so its "teeth" = it actually DETECTS an overlap (not a silent
# no-op); --stale --strict is the one RED mode. We assert both. Sister: check_determinism_selftest.sh.
set -u
cd "$(dirname "$0")/.." || exit 2
TOOL="tools/check_tech_debt.py"
fail=0

# (1) DETECTION: a file cited in OPEN TECH_DEBT entries must surface >=1 item. OrderManager.hpp is cited
#     by many open entries (008/012/029/071/130/152/...), so this is a stable positive case.
OUT="$(python3 "$TOOL" --surface CoreFrameworks/OrderManager.hpp 2>&1)"
if echo "$OUT" | grep -q 'OPEN item'; then echo "  ok: --surface detects a known overlap"; else
    echo "SELFTEST FAIL: --surface CoreFrameworks/OrderManager.hpp should surface >=1 OPEN item"; fail=1; fi

# (2) NON-MATCH: a file cited by no entry must surface nothing (no false positives).
if python3 "$TOOL" --surface __no_such_file_xyz.hpp 2>&1 | grep -q 'OPEN item'; then
    echo "SELFTEST FAIL: a non-cited file should surface nothing"; fail=1; else echo "  ok: non-cited file -> silent"; fi

# (3) RED mode: --stale 0 --strict must go RED (every entry opened before today is 'stale' at 0 months).
if python3 "$TOOL" --stale 0 --strict >/dev/null 2>&1; then
    echo "SELFTEST FAIL: --stale 0 --strict should be RED"; fail=1; else echo "  ok: --stale 0 --strict -> RED"; fi

if [ "$fail" = 0 ]; then echo "check_tech_debt_selftest: PASS (advisory has teeth)"; else echo "check_tech_debt_selftest: FAIL"; fi
exit "$fail"
