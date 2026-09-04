#!/bin/bash
# launch.sh — pre-flight the data corpus, then start the engine
#
# The engine itself never downloads anything: a network fetch or a subprocess inside the
# trading process is a boot-hang hazard on a latency-critical path. The backfill lives HERE,
# in the launcher, so "auto-update at boot" means "before the binary starts", not "inside it".
#
# Steps (in order; every path is repo-root-relative, the script anchors itself so any cwd works):
#   1. scripts/sync_archives.sh $SYMBOL $DAYS   — fill gaps in data/$SYMBOL/ from the daily
#                                                 Binance aggTrades archive (idempotent; today skipped)
#   2. scripts/verify_ticks.sh  $SYMBOL         — promote FULL staged recordings into the corpus
#   3. exec the binary                          — gui (default) | engine | suite
#
# A failed sync/verify is a WARNING, not a launch blocker: the engine trades on the live
# stream, and yesterday's archive is usually published a few hours after UTC midnight.
#
# Usage:
#   scripts/launch.sh                 # engine_gui, symbol from engine.cfg, last 30 days
#   scripts/launch.sh engine          # ANSI engine (build/engine)
#   scripts/launch.sh suite           # foxml_suite (build_suite/foxml_suite)
#   SYNC_DAYS=90 scripts/launch.sh    # widen the backfill window
#   SKIP_SYNC=1 scripts/launch.sh     # offline / airgapped: skip steps 1-2
#
# For the days the engine is NOT launched, the same backfill runs from cron (daily 02:00 UTC,
# after Binance publishes the prior day):
#   0 2 * * * ./scripts/sync_archives.sh BTCUSDT >> logging/sync.log 2>&1

set -u
set -o pipefail
cd "$(dirname "$(readlink -f "$0")")/.."

MODE="${1:-gui}"
SYNC_DAYS="${SYNC_DAYS:-30}"
SKIP_SYNC="${SKIP_SYNC:-0}"

# symbol: engine.cfg `symbol=btcusdt` (lowercase there; the data dirs + scripts use UPPER)
SYMBOL=$(sed -n 's/^symbol=\([A-Za-z0-9]*\).*/\1/p' engine.cfg | head -1 | tr '[:lower:]' '[:upper:]')
SYMBOL="${SYMBOL:-BTCUSDT}"

case "$MODE" in
    gui)    BIN=build_gui/engine_gui;    BUILD_TARGET=gui ;;
    engine) BIN=build/engine;            BUILD_TARGET=test ;;
    suite)  BIN=build_suite/foxml_suite; BUILD_TARGET=suite ;;
    *) echo "[launch] unknown mode '$MODE' (gui | engine | suite)" >&2; exit 2 ;;
esac

if [[ ! -x "$BIN" ]]; then
    echo "[launch] $BIN is missing — build it first: ./build.sh $BUILD_TARGET" >&2
    exit 2
fi

mkdir -p logging
if [[ "$SKIP_SYNC" -eq 1 ]]; then
    echo "[launch] SKIP_SYNC=1 — corpus pre-flight skipped"
else
    echo "[launch] corpus pre-flight: symbol=$SYMBOL window=${SYNC_DAYS}d (log: logging/sync.log)"
    if ! ./scripts/sync_archives.sh "$SYMBOL" "$SYNC_DAYS" 2>&1 | tee -a logging/sync.log; then
        echo "[launch] WARN: sync_archives.sh failed (offline? archive not published yet?) — launching anyway" >&2
    fi
    if ! ./scripts/verify_ticks.sh "$SYMBOL" 2>&1 | tee -a logging/sync.log; then
        echo "[launch] WARN: verify_ticks.sh failed — launching anyway" >&2
    fi
fi

echo "[launch] exec $BIN"
exec "$BIN"
