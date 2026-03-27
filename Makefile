BUILD_DIR = build

all: $(BUILD_DIR)
	cmake --build $(BUILD_DIR) -j$$(nproc)
	@ln -sf ../engine.cfg $(BUILD_DIR)/engine.cfg 2>/dev/null || true

$(BUILD_DIR):
	cmake -B $(BUILD_DIR)

run: all
	cd $(BUILD_DIR) && ./engine

# build variants
ftxui:
	cmake -B $(BUILD_DIR) -DUSE_FTXUI=ON && cmake --build $(BUILD_DIR) -j$$(nproc)

notcurses:
	cmake -B $(BUILD_DIR) -DUSE_NOTCURSES=ON && cmake --build $(BUILD_DIR) -j$$(nproc)

profile:
	cmake -B $(BUILD_DIR) -DLATENCY_PROFILING=ON && cmake --build $(BUILD_DIR) -j$$(nproc)

profile-fast:
	cmake -B $(BUILD_DIR) -DLATENCY_PROFILING=ON -DUSE_NATIVE_128=ON \
		$(if $(filter ON,$(BUSY_POLL)),-DBUSY_POLL=ON) && \
	cmake --build $(BUILD_DIR) -j$$(nproc)

profile-lite:
	cmake -B $(BUILD_DIR) -DLATENCY_LITE=ON && cmake --build $(BUILD_DIR) -j$$(nproc)

bench:
	cmake -B $(BUILD_DIR) -DLATENCY_BENCH=ON && cmake --build $(BUILD_DIR) -j$$(nproc)

# tests
test: all
	cd $(BUILD_DIR) && ctest --output-on-failure

clean:
	rm -rf $(BUILD_DIR)

# reconfigure (clears cmake cache)
reconfigure:
	rm -rf $(BUILD_DIR) && cmake -B $(BUILD_DIR)

.PHONY: all run ftxui notcurses profile profile-lite bench test clean reconfigure
