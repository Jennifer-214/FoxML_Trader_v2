BUILD_DIR = build

# `all` builds through build.sh's engine lane, which pins CMAKE_BUILD_TYPE (TECH_DEBT-329: an unpinned
# configure silently differs on -DNDEBUG). Each variant builds in its OWN dir and pins its build type, so
# build/ — the production build that pre-commit Checks N and R and the engine CLI smoke read — never inherits
# a variant's option (CMake keeps a cached option once it is set).
all:
	./build.sh engine

run: all
	cd $(BUILD_DIR) && ./engine

# build variants — each in its own dir; run its binary from there
profile:
	./build.sh latency

profile-fast:
	cmake -B build_lat_fast -DCMAKE_BUILD_TYPE=Release -DLATENCY_PROFILING=ON \
		-DBUSY_POLL=$(if $(filter ON,$(BUSY_POLL)),ON,OFF) && \
	cmake --build build_lat_fast -j$$(nproc)

profile-lite:
	cmake -B build_lat_lite -DCMAKE_BUILD_TYPE=Release -DLATENCY_LITE=ON && cmake --build build_lat_lite -j$$(nproc)

bench:
	cmake -B build_bench -DCMAKE_BUILD_TYPE=Release -DLATENCY_BENCH=ON && cmake --build build_bench -j$$(nproc)

# tests
test: all
	cd $(BUILD_DIR) && ctest --output-on-failure

clean:
	rm -rf $(BUILD_DIR) build_lat build_lat_fast build_lat_lite build_bench

# reconfigure (clears cmake cache)
reconfigure:
	rm -rf $(BUILD_DIR) && ./build.sh engine

.PHONY: all run profile profile-fast profile-lite bench test clean reconfigure
