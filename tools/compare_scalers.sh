#!/bin/bash
# tools/compare_scalers.sh — wrapper for the compare_scalers CLI.
#
# Usage:
#   tools/compare_scalers.sh <a.scaler> <b.scaler> [--threshold=PCT]
#
# Builds the binary on demand (if missing or stale relative to source)
# and forwards args. Output goes to stdout for easy piping into a log
# / CSV.
#
# v5.11.14 (2026-05-07).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$REPO_ROOT/tools/compare_scalers.cpp"
# Use build/ which is the default ./build.sh test build dir; the CMake
# target compare_scalers lands in there. Falls back to a one-off
# standalone compile if build/ is missing.
BUILT_BIN="$REPO_ROOT/build/compare_scalers"

if [ ! -x "$BUILT_BIN" ] || [ "$SRC" -nt "$BUILT_BIN" ]; then
    if [ -d "$REPO_ROOT/build" ]; then
        echo "[compare_scalers] (re)building via cmake..." >&2
        ( cd "$REPO_ROOT/build" && cmake --build . --target compare_scalers >&2 )
    else
        echo "[compare_scalers] build/ missing — falling back to standalone compile" >&2
        STANDALONE="/tmp/compare_scalers.$$"
        g++ -O2 -std=c++17 -I"$REPO_ROOT/.." -o "$STANDALONE" \
            "$SRC" -lssl -lcrypto
        BUILT_BIN="$STANDALONE"
    fi
fi

exec "$BUILT_BIN" "$@"
