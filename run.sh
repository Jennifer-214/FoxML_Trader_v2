#!/bin/bash
# foxml trader launcher
# usage: ./run.sh          — engine only (TUI in terminal)
#        ./run.sh --chart  — engine + chart window

cd "$(dirname "$0")"

# build if needed
if [ ! -f build/engine ]; then
    echo "[build] compiling..."
    cmake -B build && cmake --build build -j$(nproc)
fi

# ensure config symlink
ln -sf "$(pwd)/engine.cfg" build/engine.cfg 2>/dev/null

if [[ "$1" == "--chart" ]]; then
    # launch chart in background
    .chart-venv/bin/python tools/chart.py &
    CHART_PID=$!
    echo "[foxml] chart window launched (pid $CHART_PID)"
fi

# run engine with CPU pinning + realtime priority + inhibit sleep
cd build
touch engine.log 2>/dev/null
sudo systemd-inhibit \
    --what=idle:sleep:handle-lid-switch \
    --who="fox_ml" \
    --why="trader running" \
    --mode=block \
    taskset -c 1 chrt -f 99 sh -c './engine 2>>engine.log'

# cleanup chart if running
if [ -n "$CHART_PID" ]; then
    kill $CHART_PID 2>/dev/null
fi
