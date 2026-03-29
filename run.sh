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

# rotate engine.log — previous session becomes engine.log.prev
cd build
[ -f engine.log ] && mv -f engine.log engine.log.prev 2>/dev/null
touch engine.log 2>/dev/null

# cache sudo credentials before launching anything
sudo -v || exit 1

# NOW launch chart (after sudo prompt is done, no terminal fighting)
cd "$(dirname "$0")"
CHART_PID=""
if [[ "$WANT_CHART" == "1" ]]; then
    .chart-venv/bin/python tools/chart.py >/dev/null 2>&1 &
    CHART_PID=$!
    echo "[foxml] chart window launched (pid $CHART_PID)"
fi

# run engine with CPU pinning + realtime priority + inhibit sleep
cd build
sudo systemd-inhibit \
    --what=idle:sleep:handle-lid-switch \
    --who="fox_ml" \
    --why="trader running" \
    --mode=block \
    taskset -c 1 chrt -f 99 sh -c './engine 2>>engine.log'

# cleanup chart on exit
[ -n "$CHART_PID" ] && kill $CHART_PID 2>/dev/null
