#!/bin/bash
# download_data.sh — download Binance historical aggTrades for backtesting
# usage: ./scripts/download_data.sh BTCUSDT 2026-03-01 2026-03-07
#
# downloads from data.binance.vision (public, no API key needed)
# output: data/{SYMBOL}/YYYY-MM-DD.csv (same directory TickRecorder uses)

set -e

SYMBOL="${1:-BTCUSDT}"
START="${2:-$(date -d '7 days ago' +%Y-%m-%d)}"
END="${3:-$(date -d 'yesterday' +%Y-%m-%d)}"

OUTDIR="data/${SYMBOL}"
mkdir -p "$OUTDIR"

echo "Downloading $SYMBOL aggTrades: $START to $END"
echo "Output: $OUTDIR/"
echo ""

current="$START"
while [[ "$current" < "$END" ]] || [[ "$current" == "$END" ]]; do
    outfile="$OUTDIR/${current}.csv"

    if [ -f "$outfile" ]; then
        echo "  $current — already exists, skipping"
    else
        url="https://data.binance.vision/data/spot/daily/aggTrades/${SYMBOL}/${SYMBOL}-aggTrades-${current}.zip"
        zipfile="/tmp/${SYMBOL}-aggTrades-${current}.zip"

        echo -n "  $current — downloading... "
        if curl -sf -o "$zipfile" "$url"; then
            # extract CSV from zip, rename to our format
            unzip -q -o "$zipfile" -d "/tmp/"
            csvname="${SYMBOL}-aggTrades-${current}.csv"
            if [ -f "/tmp/$csvname" ]; then
                mv "/tmp/$csvname" "$outfile"
                size=$(du -h "$outfile" | cut -f1)
                echo "done ($size)"
            else
                echo "failed (no CSV in zip)"
            fi
            rm -f "$zipfile"
        else
            echo "not available (might be too recent)"
        fi
    fi

    current=$(date -d "$current + 1 day" +%Y-%m-%d)
done

echo ""
echo "Done. Files in $OUTDIR/:"
ls -lh "$OUTDIR/"*.csv 2>/dev/null | tail -10
echo ""
echo "Total: $(du -sh "$OUTDIR/" | cut -f1)"
