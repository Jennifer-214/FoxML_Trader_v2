#!/bin/bash
# sync_public.sh — sync shared headers from tick_trader_private to FoxML_Trader
# run from tick_trader_private root: ./scripts/sync_public.sh
#
# what it does:
#   1. rsyncs shared engine headers (excluding private files)
#   2. verifies MODEL_FORMAT_VERSION matches
#   3. shows diff for review before you commit
#
# what it NEVER syncs:
#   - Backtest/ (suite-only, private tooling)
#   - foxml_suite.cpp (private tooling)
#   - data/ (tick recordings)
#   - models/ (trained models)
#   - plans/ (implementation plans)
#   - engine.cfg (private config)
#   - Strategies/private/ (private strategies)
#   - scripts/ (meta, not needed in public)
#   - Licensing.hpp (private licensing)
#
# LICENSE CAVEAT: private repo is all AGPL, public has mixed MIT/AGPL.
# this script syncs code content including license headers. if you want
# to preserve MIT headers on specific public files, review the diff
# before committing and restore them manually.

set -e

PRIVATE="$(cd "$(dirname "$0")/.." && pwd)"
PUBLIC="$HOME/FoxML_Trader"

if [ ! -d "$PUBLIC" ]; then
    echo "ERROR: $PUBLIC not found"
    echo "Clone it first: git clone git@github.com:Jennyfirrr/FoxML_Trader.git ~/FoxML_Trader"
    exit 1
fi

echo "=== Syncing tick_trader_private → FoxML_Trader ==="
echo "From: $PRIVATE"
echo "To:   $PUBLIC"
echo ""

# shared engine headers (exclude private strategies and licensing)
for dir in CoreFrameworks FixedPoint ML_Headers MemHeaders DataStream; do
    echo "  syncing $dir/"
    rsync -av --delete \
        --exclude='private/' \
        --exclude='TickRecorder.hpp' \
        "$PRIVATE/$dir/" "$PUBLIC/$dir/"
done

# public strategies only (exclude private/)
echo "  syncing Strategies/"
rsync -av --delete \
    --exclude='private/' \
    "$PRIVATE/Strategies/" "$PUBLIC/Strategies/"

# shared GUI
echo "  syncing GUI/"
rsync -av --delete "$PRIVATE/GUI/" "$PUBLIC/GUI/"

# top-level shared files
echo "  syncing top-level files"
cp "$PRIVATE/Version.hpp" "$PUBLIC/"
cp "$PRIVATE/Limits.hpp" "$PUBLIC/"
# NOTE: CMakeLists.txt has different defaults (GUI=OFF in public, ON in private)
# only sync if you want to update build targets
# cp "$PRIVATE/CMakeLists.txt" "$PUBLIC/"

# shared test file
echo "  syncing tests/"
rsync -av "$PRIVATE/tests/" "$PUBLIC/tests/"

echo ""
echo "=== VERSION CHECK ==="
echo -n "  Private MODEL_FORMAT_VERSION: "
grep 'MODEL_FORMAT_VERSION' "$PRIVATE/ML_Headers/ModelInference.hpp" | head -1 | awk '{print $3}'
echo -n "  Public  MODEL_FORMAT_VERSION: "
grep 'MODEL_FORMAT_VERSION' "$PUBLIC/ML_Headers/ModelInference.hpp" 2>/dev/null | head -1 | awk '{print $3}' || echo "(not found)"

echo -n "  Private ENGINE_VERSION: "
grep 'ENGINE_VERSION_STRING' "$PRIVATE/Version.hpp" | head -1
echo -n "  Public  ENGINE_VERSION: "
grep 'ENGINE_VERSION_STRING' "$PUBLIC/Version.hpp" | head -1

echo ""
echo "=== Changes in FoxML_Trader ==="
cd "$PUBLIC"
git diff --stat
echo ""
echo "Review the diff, then: cd $PUBLIC && git add -A && git commit -m 'sync from private vX.Y.Z'"
