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
#   pgo         3-step profile-guided optimization build (v5.11.0.D):
#               instrument → train run on data/pgo_train.csv → profile-use
#               rebuild (build_pgo/). 2-8% latency improvement. GCC-only.
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
#
# engine.cfg is gitignored (user-tuned values never commit). On first
# build after a clone, seed it from engine.cfg.example so the new dev
# has a working config out of the gate.
link_cfg() {
    local dir="$1"
    if [[ ! -f engine.cfg ]] && [[ -f engine.cfg.example ]]; then
        cp engine.cfg.example engine.cfg
        echo "[build] seeded engine.cfg from engine.cfg.example (edit + restart engine)"
    fi
    [[ -d "$dir" ]] && [[ -f engine.cfg ]] && ln -sfn ../engine.cfg "$dir/engine.cfg"
}

# v4.3 — maintain bin/ symlinks to the canonical "latest" binary of each
# user-facing target. Without this, users get confused about which build_*
# directory contains the up-to-date binary (since `./build.sh suite` only
# rebuilds in build_suite/ while `./build.sh gui` rebuilds in build_gui/,
# the same target binary can have different timestamps in each dir).
#
# pick_newest target candidate1 candidate2 ... — symlinks bin/{target} to
# whichever candidate file has the latest mtime. Skips missing candidates.
pick_newest() {
    local target="$1"; shift
    local newest=""
    local newest_mtime=0
    for c in "$@"; do
        [[ -f "$c" ]] || continue
        local mt
        mt=$(stat -c %Y "$c" 2>/dev/null || echo 0)
        if (( mt > newest_mtime )); then
            newest="$c"
            newest_mtime=$mt
        fi
    done
    if [[ -n "$newest" ]]; then
        # newest path is repo-relative (e.g. "build_gui/foxml_suite") — convert
        # to ../{path} for the symlink (since bin/ is one level deep).
        ln -sfn "../$newest" "bin/$target"
    fi
}

update_bin_links() {
    mkdir -p bin
    pick_newest engine          build/engine          build_gui/engine          build_lat/engine
    pick_newest controller_test build/controller_test build_gui/controller_test
    pick_newest engine_gui      build_gui/engine_gui  build_gui_lite/engine_gui
    pick_newest foxml_suite     build_gui/foxml_suite build_suite/foxml_suite   build_gui_lite/foxml_suite
}

build_engine() {
    [[ "$CLEAN_FLAG" == "--clean" ]] && rm -rf build
    cmake -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j"$JOBS"
    link_cfg build
    update_bin_links
}

# v5.11.0.D — Profile-Guided Optimization three-step build:
#   1. Instrumented build (build_pgo_gen/) emits .gcda profile data on run
#   2. Representative training run populates the .gcda files
#   3. Optimized build (build_pgo/) consumes profile, optimizes hot paths
# 2-8% latency improvement typical on the engine's per-tick path.
# GCC-only (the CMake apply_pgo_flags() function is a no-op on clang).
build_pgo() {
    [[ "$CLEAN_FLAG" == "--clean" ]] && { rm -rf build_pgo_gen build_pgo; rm -rf pgo_profile; }
    PROFILE_DIR="$(pwd)/pgo_profile"
    mkdir -p "$PROFILE_DIR"

    echo "[pgo] step 1/3: instrumented build (build_pgo_gen/)"
    cmake -B build_pgo_gen -DCMAKE_BUILD_TYPE=Release \
          -DUSE_PGO_GENERATE=ON \
          -DPGO_PROFILE_DIR="$PROFILE_DIR" \
          -DUSE_NATIVE_128=ON
    cmake --build build_pgo_gen -j"$JOBS" --target engine
    link_cfg build_pgo_gen

    echo "[pgo] step 2/3: profile training run"
    if [[ -f data/pgo_train.csv ]]; then
        # Run engine in backtest mode against the training CSV so .gcda
        # files populate with realistic per-tick code coverage.
        BACKTEST_TICKS=data/pgo_train.csv ./build_pgo_gen/engine backtest.cfg || true
    else
        echo "[pgo] WARNING: data/pgo_train.csv not found; skipping training run."
        echo "[pgo] Operator: provide a representative tick CSV at data/pgo_train.csv,"
        echo "[pgo]   OR run the instrumented binary manually + ensure .gcda files"
        echo "[pgo]   land in $PROFILE_DIR before re-running this target."
        echo "[pgo] Skipping step 3 since no profile data was emitted."
        return 0
    fi

    echo "[pgo] step 3/3: profile-use rebuild (build_pgo/)"
    cmake -B build_pgo -DCMAKE_BUILD_TYPE=Release \
          -DUSE_PGO_USE=ON \
          -DPGO_PROFILE_DIR="$PROFILE_DIR" \
          -DUSE_NATIVE_128=ON
    cmake --build build_pgo -j"$JOBS"
    link_cfg build_pgo
    update_bin_links
    echo "[pgo] OK — optimized engine binary at build_pgo/engine"
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
    update_bin_links
}

build_gui_lite() {
    [[ "$CLEAN_FLAG" == "--clean" ]] && rm -rf build_gui_lite
    # Minimal GUI: ImGui only, no profiling instrumentation, no XGBoost.
    # No Latency panel, no ML training paths. Use when production perf
    # is the goal and you've already validated latency in build_lat.
    cmake -B build_gui_lite -DUSE_IMGUI_GUI=ON
    cmake --build build_gui_lite -j"$JOBS"
    link_cfg build_gui_lite
    update_bin_links
}

build_debug() {
    # v5.11.32 — engine debug-logging build. Same as build_gui (ImGui +
    # LATENCY_PROFILING + XGBoost) PLUS -DFOXML_DEBUG_LOGS=ON which
    # enables LOG_DEBUG_ENGINE / LOG_DEBUG_HOT macros (see
    # MemHeaders/DebugLog.hpp). Default release builds compile those
    # macros to ((void)0) — zero bytes, zero cost. The debug build
    # emits HEALTH_DEBUG records to engine.log on every macro call,
    # which is invaluable for reproducing tricky bugs (e.g. the WF
    # 0% accuracy regression that motivated this discipline).
    #
    # Use when reproducing a bug; do NOT use as a default development
    # build (the debug logging adds 5-10µs per fired site, which is
    # unacceptable on engine slow path even though the data is on a
    # separate cache line). Switch back to `./build.sh gui` for normal
    # work after diagnosing.
    [[ "$CLEAN_FLAG" == "--clean" ]] && rm -rf build_debug
    cmake -B build_debug -DUSE_IMGUI_GUI=ON -DLATENCY_PROFILING=ON \
                          -DUSE_XGBOOST=ON \
                          -DCMAKE_CXX_FLAGS="-DFOXML_DEBUG_LOGS=ON"
    cmake --build build_debug -j"$JOBS"
    link_cfg build_debug
    # Note: update_bin_links points bin/ at build_gui by default; for
    # debug runs, invoke build_debug/foxml_suite directly so a normal
    # `./bin/foxml_suite` invocation doesn't surprise-pick the slower
    # debug binary.
    echo "[debug] OK — debug binaries at build_debug/{engine_gui,foxml_suite}"
    echo "[debug] (bin/ symlinks intentionally NOT updated — invoke directly)"
}

build_suite() {
    # Backward-compat alias — build_suite is now the same as build_gui.
    [[ "$CLEAN_FLAG" == "--clean" ]] && rm -rf build_suite
    cmake -B build_suite -DUSE_IMGUI_GUI=ON -DLATENCY_PROFILING=ON -DUSE_XGBOOST=ON
    cmake --build build_suite -j"$JOBS" --target foxml_suite
    link_cfg build_suite
    update_bin_links
}

build_latency() {
    [[ "$CLEAN_FLAG" == "--clean" ]] && rm -rf build_lat
    cmake -B build_lat -DLATENCY_PROFILING=ON
    cmake --build build_lat -j"$JOBS"
    link_cfg build_lat
    update_bin_links
}

# v5.0.5: TSan build for race detection. -O1 -g keeps usable line info; -fno-omit-
# frame-pointer for nice stacks. Slow runtime (~5-15× hot path) but unrelated to
# correctness checking. Use against engine + controller_test in synthetic mode
# for race / data-race detection.
build_tsan() {
    [[ "$CLEAN_FLAG" == "--clean" ]] && rm -rf build_tsan
    cmake -B build_tsan -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_CXX_FLAGS="-fsanitize=thread -O1 -g -fno-omit-frame-pointer -DUSE_NATIVE_128=ON" \
        -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
    cmake --build build_tsan -j"$JOBS"
    link_cfg build_tsan
}

# v5.0.5: ASan build for memory-error detection. Catches use-after-free,
# double-free, leaks, buffer overflows. Less overhead than TSan (~2-3×) so
# can run more aggressively. Use against engine + controller_test.
build_asan() {
    [[ "$CLEAN_FLAG" == "--clean" ]] && rm -rf build_asan
    cmake -B build_asan -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_CXX_FLAGS="-fsanitize=address -O1 -g -fno-omit-frame-pointer -DUSE_NATIVE_128=ON" \
        -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address"
    cmake --build build_asan -j"$JOBS"
    link_cfg build_asan
}

run_tests() {
    build_engine
    echo "--- running controller_test ---"
    ./build/controller_test
}

# v5.15.5.F.4c.3 WIP2d-0 — per-core cfg registry integrity check (H17 STRONG enforcement).
# Runs BEFORE any build to catch structural drift in:
#   - FOREACH_PER_CORE_CFG_FIELD ↔ FOREACH_PER_CORE_FIELD_TYPE bidirectional sync
#   - PerCoreCfg<F> body manual-field bypass (X-macro must be sole source)
#   - ControllerConfig parallel arrays ↔ FOREACH_MANUAL_PER_CORE_FIELD ↔ MANUAL_FIELDS_INVENTORY.md
#   - Name duplication between registries
# Failure = build aborted with diff suggesting registry migration.
# See tools/check_per_core_registry_integrity.py for full check list.
check_per_core_cfg_integrity() {
    echo "--- per-core cfg integrity check (WIP2d-0) ---"
    if ! python3 tools/check_per_core_registry_integrity.py; then
        echo "[build] ABORT: per-core cfg integrity check FAILED — fix violations above before build"
        exit 1
    fi
}

case "$TARGET" in
    engine)
        check_per_core_cfg_integrity
        build_engine
        ;;
    gui)
        check_per_core_cfg_integrity
        build_gui
        ;;
    gui-lite)
        check_per_core_cfg_integrity
        build_gui_lite
        ;;
    suite)
        check_per_core_cfg_integrity
        build_suite
        ;;
    all)
        check_per_core_cfg_integrity
        build_engine
        build_gui
        ;;
    test)
        check_per_core_cfg_integrity
        run_tests
        ;;
    latency)
        check_per_core_cfg_integrity
        build_latency
        ;;
    pgo)
        check_per_core_cfg_integrity
        build_pgo
        ;;
    tsan)
        check_per_core_cfg_integrity
        build_tsan
        ;;
    asan)
        build_asan
        ;;
    debug)
        build_debug
        ;;
    clean)
        rm -rf build build_gui build_gui_lite build_suite build_lat \
               build_pgo_gen build_pgo pgo_profile \
               build_tsan build_asan build_debug bin
        echo "all build dirs + bin/ symlinks removed"
        ;;
    *)
        echo "unknown target: $TARGET" >&2
        echo "usage: $0 {engine|gui|gui-lite|suite|all|test|latency|pgo|tsan|asan|debug|clean} [--clean]" >&2
        exit 1
        ;;
esac

echo "--- $TARGET: ok ---"
