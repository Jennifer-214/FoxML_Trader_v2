#!/bin/bash
# verify_ticks.sh — promote tick recordings from _staging/ to the dataset folder
#
# v4.2.2 — TickRecorder writes to data/{SYMBOL}/_staging/ so partial / corrupt
# recordings can't pollute the training corpus. This script checks each staged
# file for completeness and moves the good ones into data/{SYMBOL}/.
#
# Checks per file:
#   1. File size >= MIN_SIZE_MB (default 30 MB — partial days fail)
#   2. Filename year matches a real recent year (rejects year-58286-style bugs)
#   3. Filename date is NOT today (today's recording is still in progress)
#   4. First and last timestamp span > MIN_HOURS (default 23 — not a half-day)
#
# Usage:
#   scripts/verify_ticks.sh                      # default symbol BTCUSDT
#   scripts/verify_ticks.sh ETHUSDT              # different symbol
#   scripts/verify_ticks.sh BTCUSDT --dry-run    # report only, don't promote
#
# Files that fail any check stay in _staging/ for manual review.

set -e

SYMBOL="${1:-BTCUSDT}"
DRY_RUN=0
[[ "$2" == "--dry-run" ]] && DRY_RUN=1

STAGING_DIR="data/${SYMBOL}/_staging"
DATASET_DIR="data/${SYMBOL}"

MIN_SIZE_MB=30          # full BTC day is 40-145M; <30M is partial
MIN_HOURS=23            # a full day spans ~24h; allow 1h slack for boundary
TODAY_UTC=$(date -u +%Y-%m-%d)
THIS_YEAR=$(date -u +%Y)

if [[ ! -d "$STAGING_DIR" ]]; then
    echo "[verify] no staging dir: $STAGING_DIR — nothing to verify"
    exit 0
fi

cd "$STAGING_DIR" || exit 1
shopt -s nullglob

PROMOTED=0
SKIPPED=0
REJECTED=0

for f in *.csv; do
    [[ -f "$f" ]] || continue

    base="${f%.csv}"
    fail_reason=""

    # 1. size check
    size_mb=$(stat -c %s "$f" 2>/dev/null | awk '{printf "%.0f", $1/1048576}')
    if [[ "$size_mb" -lt "$MIN_SIZE_MB" ]]; then
        fail_reason="size ${size_mb}MB < ${MIN_SIZE_MB}MB"
    fi

    # 2. filename year sanity (date 4-digit year matches current year +/- 1)
    if [[ -z "$fail_reason" ]]; then
        file_year="${base:0:4}"
        if [[ "$file_year" != "$THIS_YEAR" && "$file_year" != "$((THIS_YEAR-1))" ]]; then
            fail_reason="bogus year $file_year (expected $THIS_YEAR or prior)"
        fi
    fi

    # 3. today's file is in-progress, skip (not reject — try again tomorrow)
    if [[ -z "$fail_reason" && "$base" == "$TODAY_UTC" ]]; then
        echo "[verify] $f: SKIP (today's recording still in progress)"
        SKIPPED=$((SKIPPED+1))
        continue
    fi

    # 4. timestamp span check — first vs last DATA row.
    # .E.0.1: TickRecorder CSV is "timestamp_us,price,quantity,is_buyer_maker" (4 cols, ts =
    # column 1) with a header row — NOT the 8-col aggTrades format the old col-6 reference
    # assumed. Reading col 6 of a 4-col file returned empty, so this span check SILENTLY never
    # ran. Fixed: ts is column 1; the awk skips the header by taking the first numeric-col-1 row.
    if [[ -z "$fail_reason" ]]; then
        first_ts=$(awk -F',' '$1 ~ /^[0-9]/ {print $1; exit}' "$f")
        # last_ts uses the SAME numeric-col-1 guard as first_ts (tac = scan from the
        # end, exit at first data row) so a trailing header/blank can't masquerade as a
        # timestamp. Was `tail -1 | $1`, which lacked first_ts's guard (the two checks
        # must be symmetric — .E.0.1 independent-review LOW).
        last_ts=$(tac "$f" | awk -F',' '$1 ~ /^[0-9]/ {print $1; exit}')
        if [[ -n "$first_ts" && -n "$last_ts" ]]; then
            # span in hours = (last - first) / 1e6 / 3600
            span_h=$(awk -v a="$first_ts" -v b="$last_ts" 'BEGIN { printf "%.1f", (b-a)/1e6/3600 }')
            span_int=$(printf "%.0f" "$span_h")
            if [[ "$span_int" -lt "$MIN_HOURS" ]]; then
                fail_reason="timestamp span ${span_h}h < ${MIN_HOURS}h"
            fi
        else
            fail_reason="empty file (no rows)"
        fi
    fi

    if [[ -n "$fail_reason" ]]; then
        echo "[verify] $f: REJECT ($fail_reason)"
        REJECTED=$((REJECTED+1))
        continue
    fi

    # All checks passed. Promote.
    if [[ "$DRY_RUN" -eq 1 ]]; then
        echo "[verify] $f: OK (would promote — dry-run)"
    else
        mv "$f" "../$f"
        echo "[verify] $f: PROMOTED → data/${SYMBOL}/$f"
    fi
    PROMOTED=$((PROMOTED+1))
done

echo ""
echo "[verify] summary: promoted=$PROMOTED skipped=$SKIPPED rejected=$REJECTED"
if [[ "$REJECTED" -gt 0 ]]; then
    echo "[verify] rejected files remain in $STAGING_DIR for manual review"
fi
