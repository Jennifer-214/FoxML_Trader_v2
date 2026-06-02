#!/usr/bin/env bash
# NEGATIVE self-test for check_identifier_retirement.py (the H21 tombstone guard) — proves it has TEETH:
# it goes RED (exit 1) on a renumber / version-decrease / silent-removal, and GREEN on the clean ledger.
# Mirrors the manual verify-by-injection done when the guard was built. Sister: check_determinism_selftest.sh.
# Restores the ledger on exit (trap).
set -u
cd "$(dirname "$0")/.." || exit 2
TOOL="tools/check_identifier_retirement.py"
LEDGER="tools/identifier_ledger.txt"
BAK="$(mktemp)"; cp "$LEDGER" "$BAK"
restore(){ cp "$BAK" "$LEDGER"; rm -f "$BAK"; }
trap restore EXIT
fail=0

# baseline: the clean ledger must be GREEN
python3 "$TOOL" >/dev/null 2>&1 || { echo "SELFTEST FAIL: clean ledger should be GREEN"; fail=1; }

# (1) RENUMBER an enum code -> must be RED
cp "$BAK" "$LEDGER"
sed -i 's/enum:StrategyId|STRATEGY_ML|3/enum:StrategyId|STRATEGY_ML|9/' "$LEDGER"
if python3 "$TOOL" >/dev/null 2>&1; then echo "SELFTEST FAIL: enum renumber should be RED"; fail=1; else echo "  ok: enum renumber -> RED"; fi

# (2) VERSION DECREASE (ledger ahead of code => code looks decreased) -> must be RED
cp "$BAK" "$LEDGER"
sed -i 's/version|SHARDED_SNAPSHOT_VERSION|8/version|SHARDED_SNAPSHOT_VERSION|9/' "$LEDGER"
if python3 "$TOOL" >/dev/null 2>&1; then echo "SELFTEST FAIL: version-decrease should be RED"; fail=1; else echo "  ok: version-decrease -> RED"; fi

# (3) SILENT REMOVAL (ledger has an identifier the code no longer defines) -> must be RED
cp "$BAK" "$LEDGER"
printf 'enum:StrategyId|STRATEGY_GHOST_OLD|9\n' >> "$LEDGER"
if python3 "$TOOL" >/dev/null 2>&1; then echo "SELFTEST FAIL: silent-removal should be RED"; fail=1; else echo "  ok: silent-removal -> RED"; fi

# restore + GREEN again
restore; trap - EXIT
python3 "$TOOL" >/dev/null 2>&1 || { echo "SELFTEST FAIL: restored ledger should be GREEN"; fail=1; }

if [ "$fail" = 0 ]; then echo "check_identifier_retirement_selftest: PASS (guard has teeth)"; else echo "check_identifier_retirement_selftest: FAIL"; fi
exit "$fail"
