#!/bin/bash
# sync_archives.sh — fill any gaps in data/{SYMBOL}/ from Binance public archives
#
# Run this daily (manually or via cron) to backfill yesterday's archive once
# it's published, or any older missing days. Idempotent — already-present
# files are skipped, only gaps trigger downloads.
#
# Usage:
#   scripts/sync_archives.sh                        # default: BTCUSDT, last 30 days
#   scripts/sync_archives.sh BTCUSDT                # explicit symbol
#   scripts/sync_archives.sh BTCUSDT 60             # last 60 days
#   scripts/sync_archives.sh BTCUSDT 60 --dry-run   # report missing, don't download
#
# Today's date is always skipped (Binance publishes archives next-day).
#
# Cron example (daily at 02:00 UTC, after Binance publishes the prior day):
#   0 2 * * * cd /path/to/tick-trader-percore && ./scripts/sync_archives.sh BTCUSDT >> logging/sync.log 2>&1

set -e

SYMBOL="${1:-BTCUSDT}"
DAYS_BACK="${2:-30}"
DRY_RUN=0
[[ "$3" == "--dry-run" ]] && DRY_RUN=1

DATASET_DIR="data/${SYMBOL}"
TODAY_UTC=$(date -u +%Y-%m-%d)
YESTERDAY=$(date -u -d 'yesterday' +%Y-%m-%d)

mkdir -p "$DATASET_DIR"

echo "[sync] symbol=$SYMBOL  window=last ${DAYS_BACK} days  today=${TODAY_UTC} (skipped)"
echo "[sync] checking for gaps..."

PRESENT=0
MISSING=()

for i in $(seq 1 "$DAYS_BACK"); do
    d=$(date -u -d "$i days ago" +%Y-%m-%d)
    # never try to download today (in-progress) or future
    [[ "$d" == "$TODAY_UTC" ]] && continue
    if [[ -f "$DATASET_DIR/${d}.csv" ]]; then
        PRESENT=$((PRESENT+1))
    else
        MISSING+=("$d")
    fi
done

if [[ "${#MISSING[@]}" -eq 0 ]]; then
    echo "[sync] no gaps in last ${DAYS_BACK} days (${PRESENT} files present)"
    exit 0
fi

echo "[sync] found ${#MISSING[@]} missing day(s):"
printf '  %s\n' "${MISSING[@]}"

if [[ "$DRY_RUN" -eq 1 ]]; then
    echo "[sync] dry-run — exiting without downloading"
    exit 0
fi

echo ""
echo "[sync] downloading missing days..."

DOWNLOADED=0
FAILED=0

for d in "${MISSING[@]}"; do
    # delegate to download_data.sh — it knows the URL pattern + extraction
    if ./scripts/download_data.sh "$SYMBOL" "$d" "$d" 2>&1 | grep -q "done"; then
        DOWNLOADED=$((DOWNLOADED+1))
    else
        FAILED=$((FAILED+1))
        echo "[sync] $d: FAILED (archive may not be published yet)"
    fi
done

echo ""
echo "[sync] summary: present=${PRESENT}  downloaded=${DOWNLOADED}  failed=${FAILED}"

# print updated count for verification
NEW_COUNT=$(ls -1 "$DATASET_DIR"/*.csv 2>/dev/null | wc -l)
echo "[sync] $DATASET_DIR now contains $NEW_COUNT day file(s)"
