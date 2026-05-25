# Thin wrapper over CMake. See CMakeLists.txt.
BUILD ?= build

.PHONY: all baselines clean

# Build the harness + workload generator (binaries land at the repo root).
all:
	@cmake -S . -B $(BUILD) -DCMAKE_BUILD_TYPE=Release
	@cmake --build $(BUILD) -j

# Build the three pre-configured baseline engine adapters (.so).
baselines:
	@bash scripts/build_baselines.sh all

clean:
	@rm -rf $(BUILD) harness generator *_adapter.so *.class \
	        exchange_core.classpath orders_*.bin
