#!/bin/bash
# build.sh — build helper, single entry point for common cmake operations
#
# Usage:
#   ./build.sh [target] [--clean]
#
# Targets:
#   engine      ANSI engine + controller_test (build/) — minimal, no ImGui
#   gui         engine_gui + foxml_suite (build_gui/, ImGui+SDL2 + LATENCY +
#               XGBoost) — the "everything on" build with ALL panels visible
#               (Latency, Per-Core, ML Intelligence). Requires libxgboost
#               headers at /usr/local/include/xgboost.
#   gui-lite    engine_gui + foxml_suite (build_gui_lite/, ImGui+SDL2 only,
#               no profiling/XGBoost) — minimal GUI, fastest hot path
#   suite       alias for gui (kept for backward compat)
#   all         engine + gui (skips gui-lite — opt-in)
#   test        engine + run controller_test
#   latency     engine with -DLATENCY_PROFILING=ON (build_lat/, ANSI only,
#               for raw latency benchmarks without ImGui overhead)
#   clean       wipe all build directories
#
# Examples:
#   ./build.sh                # default: 'all' (engine + gui)
#   ./build.sh test           # build engine + run tests
#   ./build.sh gui            # full GUI with all panels visible
#   ./build.sh gui-lite       # minimal GUI (no Latency / no XGBoost)
#   ./build.sh latency        # pure-ANSI latency bench
#   ./build.sh clean          # remove all build dirs
#   ./build.sh engine --clean # clean rebuild of engine

set -e

JOBS="$(nproc 2>/dev/null || echo 4)"
TARGET="${1:-all}"
CLEAN_FLAG="${2:-}"

if [[ "$TARGET" == "--clean" ]]; then
    CLEAN_FLAG="--clean"
    TARGET="all"
fi

# The engine looks for engine.cfg in cwd. Without this symlink, running
# ./engine from build/ gets "config not found, using defaults" — and falls
# back to synthetic ticks instead of the live Binance feed.
link_cfg() {
    local dir="$1"
    [[ -d "$dir" ]] && [[ -f engine.cfg ]] && ln -sfn ../engine.cfg "$dir/engine.cfg"
}

build_engine() {
    [[ "$CLEAN_FLAG" == "--clean" ]] && rm -rf build
    cmake -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j"$JOBS"
    link_cfg build
}

build_gui() {
    [[ "$CLEAN_FLAG" == "--clean" ]] && rm -rf build_gui
    # Default GUI build = "everything on": ImGui + Latency profiling + XGBoost.
    # All dashboard panels visible (Latency, ML Intelligence, Per-Core when
    # engine_mode=sharded). Slight per-tick instrumentation cost from
    # LATENCY_PROFILING (~10-20ns) — acceptable for observation/dev. For
    # raw production performance use `./build.sh gui-lite` instead.
    cmake -B build_gui -DUSE_IMGUI_GUI=ON -DLATENCY_PROFILING=ON -DUSE_XGBOOST=ON
    cmake --build build_gui -j"$JOBS"
    link_cfg build_gui
}

build_gui_lite() {
    [[ "$CLEAN_FLAG" == "--clean" ]] && rm -rf build_gui_lite
    # Minimal GUI: ImGui only, no profiling instrumentation, no XGBoost.
    # No Latency panel, no ML training paths. Use when production perf
    # is the goal and you've already validated latency in build_lat.
    cmake -B build_gui_lite -DUSE_IMGUI_GUI=ON
    cmake --build build_gui_lite -j"$JOBS"
    link_cfg build_gui_lite
}

build_suite() {
    # Backward-compat alias — build_suite is now the same as build_gui.
    [[ "$CLEAN_FLAG" == "--clean" ]] && rm -rf build_suite
    cmake -B build_suite -DUSE_IMGUI_GUI=ON -DLATENCY_PROFILING=ON -DUSE_XGBOOST=ON
    cmake --build build_suite -j"$JOBS" --target foxml_suite
    link_cfg build_suite
}

build_latency() {
    [[ "$CLEAN_FLAG" == "--clean" ]] && rm -rf build_lat
    cmake -B build_lat -DLATENCY_PROFILING=ON
    cmake --build build_lat -j"$JOBS"
    link_cfg build_lat
}

run_tests() {
    build_engine
    echo "--- running controller_test ---"
    ./build/controller_test
}

case "$TARGET" in
    engine)
        build_engine
        ;;
    gui)
        build_gui
        ;;
    gui-lite)
        build_gui_lite
        ;;
    suite)
        build_suite
        ;;
    all)
        build_engine
        build_gui
        ;;
    test)
        run_tests
        ;;
    latency)
        build_latency
        ;;
    clean)
        rm -rf build build_gui build_gui_lite build_suite build_lat
        echo "all build dirs removed"
        ;;
    *)
        echo "unknown target: $TARGET" >&2
        echo "usage: $0 {engine|gui|gui-lite|suite|all|test|latency|clean} [--clean]" >&2
        exit 1
        ;;
esac

echo "--- $TARGET: ok ---"
