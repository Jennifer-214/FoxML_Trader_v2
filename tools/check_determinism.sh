#!/usr/bin/env bash
# tools/check_determinism.sh — .E.0.1 DETERMINISM NET (the umbrella).
#
# Runs every determinism/locale gate in one shot. THIS is the standing net that #11
# (FixedPoint<RADIX,FRAC>) + .E.1 (Core->Node rename) run under: any drift = red build.
# Wired into the engine pre-commit hook (Check F, scoped to FP/parse/locale paths) and
# runnable on demand. Add a gate here when a new determinism cluster lands (e.g. F-076
# fingerprint canonicalize).
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; cd "$ROOT"
rc=0

echo "============================================================"
echo " determinism net — $(basename "$ROOT")"
echo "============================================================"

echo "## (1) FP-determinism golden (native==generic, cross-opt-level) ##"
if ./tools/check_fp_determinism.sh; then :; else rc=1; fi

echo ""
echo "## (2) locale-determinism guard (boot pin / no-stray-setlocale / raw-parse baseline) ##"
if ./tools/check_locale_determinism.sh; then :; else rc=1; fi

echo ""
echo "## (3) replay-locale gate (decimal-point honored; C == non-C when available) ##"
if g++ -std=c++17 -O2 -march=native -I"$ROOT" tools/replay_locale_gate.cpp -o "$ROOT/.det_rlg" 2>/tmp/det_rlg.err; then
  "$ROOT/.det_rlg" || rc=1; rm -f "$ROOT/.det_rlg"
else echo "  FAIL replay_locale_gate build:"; head -15 /tmp/det_rlg.err; rc=1; fi

echo ""
echo "============================================================"
[ "$rc" -eq 0 ] && echo " GREEN — determinism net clean." || echo " RED — determinism net violation (see above)."
echo "============================================================"
exit "$rc"
