#!/usr/bin/env bash
# tools/check_fp_determinism.sh — .E.0.1 FP-DETERMINISM GATE (the net).
#
# Re-runs tools/fp_determinism_golden.cpp three ways and diffs each against the
# frozen golden (tools/fp_determinism_golden.txt). ANY drift = exit 1 (red build):
#   - native -O3 : cross-run drift + FUTURE-CODE drift — when #11 rewrites
#                  FixedPointN into FixedPoint<2,64>, the binary instantiation MUST
#                  reproduce this byte-for-byte (D-100 binary-epoch golden; D-99
#                  "reuse-certified-bodies" proof).
#   - native -O0 : cross-binary / opt-level determinism (H10 cross-binary).
#   - generic    : ±USE_NATIVE_128 diagnostic — native==generic post-F-056.
#
# Regenerate the golden ONLY as a deliberate D-100 epoch transition (never silent):
#   g++ -std=c++17 -DUSE_NATIVE_128 -O3 -march=native -I. \
#       tools/fp_determinism_golden.cpp -o /tmp/g && /tmp/g > tools/fp_determinism_golden.txt
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
H="$ROOT/tools/fp_determinism_golden.cpp"
GOLDEN="$ROOT/tools/fp_determinism_golden.txt"
rc=0

[ -f "$GOLDEN" ] || { echo "FAIL: golden missing ($GOLDEN)"; exit 1; }
[ -f "$H" ]      || { echo "FAIL: harness missing ($H)"; exit 1; }

# Compile into $ROOT (exec-capable; /tmp is often noexec); write outputs to /tmp.
# Temp names carry $$ (PID) so concurrent runs (e.g. a backgrounded run racing the
# pre-commit hook) can't clobber each other's scratch — .E.0.1 determinism-gate hardening.
check() {  # $1=label  $2=defs  $3=opt
  local label="$1" defs="$2" opt="$3"
  local bin="$ROOT/.fpdet_${label}_$$" out="/tmp/fpdet_${label}_$$.txt" err="/tmp/fpdet_${label}_$$.err"
  if ! g++ -std=c++17 $defs "$opt" -march=native -I"$ROOT" "$H" -o "$bin" 2>"$err"; then
    echo "  ❌ $label BUILD ERROR:"; head -15 "$err"; rm -f "$err"; rc=1; return
  fi
  "$bin" > "$out"; rm -f "$bin"
  if diff -q "$GOLDEN" "$out" >/dev/null; then
    echo "  ✅ $label == golden"
  else
    echo "  ❌ $label DRIFTS from golden:"; diff "$GOLDEN" "$out" | head -20; rc=1
  fi
  rm -f "$out" "$err"
}

echo "=== FP-determinism gate (vs tools/fp_determinism_golden.txt) ==="
check native_o3  "-DUSE_NATIVE_128" "-O3"
check native_o0  "-DUSE_NATIVE_128" "-O0"
check generic_o3 ""                 "-O3"

if [ "$rc" -eq 0 ]; then
  echo "GREEN — FP path byte-deterministic (cross-opt-level + ±native) and matches the golden."
else
  echo "RED — FP-determinism drift. If intentional (epoch transition), regenerate the golden deliberately."
fi
exit "$rc"
