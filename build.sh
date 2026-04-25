#!/bin/bash
# build.sh — build helper, single entry point for common cmake operations
#
# Usage:
#   ./build.sh [target] [--clean]
#
# Targets:
#   engine      ANSI engine + controller_test (build/)
#   gui         engine_gui + foxml_suite (build_gui/, with ImGui+SDL2)
#   suite       foxml_suite with XGBoost (build_suite/, requires libxgboost)
#   all         engine + gui (skips suite — opt-in)
#   test        engine + run controller_test
#   latency     engine with -DLATENCY_PROFILING=ON (build_lat/)
#   clean       wipe all build directories
#
# Examples:
#   ./build.sh                # default: 'all' (engine + gui)
#   ./build.sh test           # build engine + run tests
#   ./build.sh suite          # build XGBoost variant
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

build_engine() {
    [[ "$CLEAN_FLAG" == "--clean" ]] && rm -rf build
    cmake -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j"$JOBS"
}

build_gui() {
    [[ "$CLEAN_FLAG" == "--clean" ]] && rm -rf build_gui
    cmake -B build_gui -DUSE_IMGUI_GUI=ON
    cmake --build build_gui -j"$JOBS"
}

build_suite() {
    [[ "$CLEAN_FLAG" == "--clean" ]] && rm -rf build_suite
    cmake -B build_suite -DUSE_IMGUI_GUI=ON -DUSE_XGBOOST=ON
    cmake --build build_suite -j"$JOBS" --target foxml_suite
}

build_latency() {
    [[ "$CLEAN_FLAG" == "--clean" ]] && rm -rf build_lat
    cmake -B build_lat -DLATENCY_PROFILING=ON
    cmake --build build_lat -j"$JOBS"
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
        rm -rf build build_gui build_suite build_lat
        echo "all build dirs removed"
        ;;
    *)
        echo "unknown target: $TARGET" >&2
        echo "usage: $0 {engine|gui|suite|all|test|latency|clean} [--clean]" >&2
        exit 1
        ;;
esac

echo "--- $TARGET: ok ---"
