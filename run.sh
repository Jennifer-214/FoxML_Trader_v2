#!/bin/bash
# foxml trader launcher
# usage: ./run.sh          — engine only (TUI in terminal)
#        ./run.sh --chart  — engine + chart window

WANT_CHART=0
[[ "$1" == "--chart" ]] && WANT_CHART=1

cd "$(dirname "$0")"

# build if needed
if [ ! -f build/engine ]; then
    echo "[build] compiling..."
    cmake -B build && cmake --build build -j$(nproc)
fi

# ensure config symlink
ln -sf "$(pwd)/engine.cfg" build/engine.cfg 2>/dev/null

# rotate engine.log
cd build
[ -f engine.log ] && mv -f engine.log engine.log.prev 2>/dev/null
touch engine.log 2>/dev/null

# launch chart if requested
cd "$(dirname "$0")"
CHART_PID=""
if [[ "$WANT_CHART" == "1" ]]; then
    .chart-venv/bin/python tools/chart.py &
    CHART_PID=$!
    echo "[foxml] chart window launched (pid $CHART_PID)"
fi

# run engine
cd build
./engine 2>>engine.log

# cleanup chart on exit
[ -n "$CHART_PID" ] && kill $CHART_PID 2>/dev/null
