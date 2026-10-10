# HldsMonitoring — ReHLDS telemetry plugin (Phase 0 scaffold)
# Build: 32-bit Metamod plugin (Linux). Tests: host-native core + python analyzer.

CXX      ?= g++
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
HOST_FLAGS := $(BASE_FLAGS) -Isrc

TARGET := HldsMonitoring_mm_i386.so
SRCS   := $(wildcard src/*.cpp)
CORE_SRCS := $(wildcard src/serialization.cpp src/log_writer.cpp src/metrics.cpp)
TEST_SRCS := $(wildcard tests/test_*.cpp)

.PHONY: all test test-host-core test-core test-python web verify-exports clean

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
