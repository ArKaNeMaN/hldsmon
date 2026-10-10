# HldsMonitoring — ReHLDS telemetry plugin (Phase 0 scaffold)
# Build: 32-bit Metamod plugin (Linux). Tests: host-native core + python analyzer.

CXX      ?= g++
CC       ?= gcc
PYTHON   ?= python3

# ---- vendored include dirs (system-style: suppress third-party warnings) ----
VENDOR_INC := \
	-isystem include \
	-isystem include/metamod \
	-isystem include/cssdk/common \
	-isystem include/cssdk/dlls \
	-isystem include/cssdk/engine \
	-isystem include/cssdk/game_shared \
	-isystem include/cssdk/pm_shared \
	-isystem include/cssdk/public \
	-isystem include/cssdk/public/tier0

PLUGIN_DEFS := \
	-D_LINUX -DLINUX -DNDEBUG -D_GLIBCXX_USE_CXX11_ABI=0 \
	-D_stricmp=strcasecmp -D_strnicmp=strncasecmp \
	-D_snprintf=snprintf -D_vsnprintf=vsnprintf \
	-DHAVE_STRONG_TYPEDEF

WARN       := -Wall -Wextra -Werror
BASE_FLAGS := -std=c++17 $(WARN) -O2
PLUGIN_FLAGS := $(BASE_FLAGS) -m32 -fPIC -shared $(PLUGIN_DEFS) $(VENDOR_INC)
HOST_FLAGS := $(BASE_FLAGS) -Isrc -pthread
# 32-bit synthetic GOT/PLT harness (M-P5): driver + target objects, no engine headers.
HARNESS_DIR    := build/modnet_harness
HARNESS_SRCS   := src/modnet_got.cpp src/modnet.cpp
HARNESS_FLAGS  := -std=c++17 $(WARN) -O2 -m32 -Isrc
HARNESS_TFLAGS := $(WARN) -O2 -m32 -fPIC -shared -fno-optimize-sibling-calls

TARGET := HldsMonitoring_mm_i386.so
SRCS   := $(wildcard src/*.cpp)
CORE_SRCS := $(wildcard src/serialization.cpp src/log_writer.cpp src/metrics.cpp src/modnet.cpp src/retention.cpp)
TEST_SRCS := $(wildcard tests/test_*.cpp)

.PHONY: all test test-host-core test-core test-python web verify-exports test-modnet-harness clean

all: $(TARGET)

$(TARGET): $(SRCS) version_script.lds
	$(CXX) $(PLUGIN_FLAGS) $(SRCS) -o $@ \
		-Wl,--version-script=version_script.lds \
		-static-libgcc -static-libstdc++ -ldl -lrt

# host-native compile of the pure core + tests (must NOT need engine headers)
test-host-core:
	@mkdir -p build
	@if [ -z "$(TEST_SRCS)" ]; then echo "test-host-core: no tests yet"; else \
		echo "test-host-core: compiling + running host tests"; \
		for t in $(TEST_SRCS); do \
			name=$$(basename $$t .cpp); \
			$(CXX) $(HOST_FLAGS) $$t $(CORE_SRCS) -o build/$$name || exit 1; \
			./build/$$name || exit 1; \
		done; \
	fi

verify-exports: $(TARGET)
	@mkdir -p build
	@nm -D --defined-only $(TARGET) | awk '{print $$3}' | sed 's/@.*//' | grep -v '^HLDSMON_1.0$$' | LC_ALL=C sort -u > build/exports.got
	@LC_ALL=C sort -u exports.whitelist > build/exports.expected
	@diff build/exports.got build/exports.expected && echo "verify-exports: OK"

# 32-bit synthetic GOT/PLT harness (plan M-P5). Builds synthetic target `.so`s
# (lazy + BIND_NOW), a 32-bit driver linking `src/modnet_got.cpp` +
# `src/modnet.cpp` (no engine headers), and runs it; non-zero on any failure.
# Needs a working i686 toolchain/multilib; NOT part of the host `make test`.
test-modnet-harness:
	@mkdir -p $(HARNESS_DIR)
	$(CC) $(HARNESS_TFLAGS) tests/modnet_harness/target.c -o $(HARNESS_DIR)/target_lazy.so
	$(CC) $(HARNESS_TFLAGS) tests/modnet_harness/target.c -o $(HARNESS_DIR)/target_now.so -Wl,-z,now
	$(CXX) $(HARNESS_FLAGS) tests/modnet_harness/harness.cpp $(HARNESS_SRCS) \
		-o $(HARNESS_DIR)/harness -ldl
	$(HARNESS_DIR)/harness $(HARNESS_DIR)/target_lazy.so $(HARNESS_DIR)/target_now.so

test-python:
	@if [ -f tools/analyzer/tests/test_hldsmon.py ]; then \
		cd tools/analyzer && $(PYTHON) -m unittest discover -s tests -v; \
	else echo "test-python: no analyzer tests yet"; fi

test: test-host-core test-python

# Local offline web UI (stdlib only). Serves tools/analyzer/webui/index.html + JSON API.
web:
	$(PYTHON) tools/analyzer/webui.py --open

clean:
	rm -rf build $(TARGET)
