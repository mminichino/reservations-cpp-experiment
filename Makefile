# Convenience build for macOS (Homebrew). Also works on Linux without a prefix.
#
#   make          # configure + build Release
#   make debug    # configure + build Debug
#   make clean    # remove build/
#   make reconfigure  # wipe CMake cache and rebuild

BUILD_DIR ?= build
BUILD_TYPE ?= Release
JOBS ?= $(shell sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

# Apple Silicon Homebrew: /opt/homebrew; Intel: /usr/local; Linux: empty
BREW_PREFIX := $(shell brew --prefix 2>/dev/null)
CMAKE_PREFIX_PATH ?= $(BREW_PREFIX)

CMAKE_ARGS := -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)
ifneq ($(CMAKE_PREFIX_PATH),)
  CMAKE_ARGS += -DCMAKE_PREFIX_PATH=$(CMAKE_PREFIX_PATH)
endif

.PHONY: all configure build debug clean reconfigure load exercise help

all: build

help:
	@echo "Targets: all (default), configure, build, debug, clean, reconfigure, load, exercise"
	@echo "Vars:    BUILD_DIR=$(BUILD_DIR) BUILD_TYPE=$(BUILD_TYPE) JOBS=$(JOBS)"
	@echo "         CMAKE_PREFIX_PATH=$(CMAKE_PREFIX_PATH)"

configure:
	cmake -S . -B $(BUILD_DIR) $(CMAKE_ARGS)

build: configure
	cmake --build $(BUILD_DIR) -j$(JOBS)

debug:
	$(MAKE) BUILD_TYPE=Debug build

clean:
	rm -rf $(BUILD_DIR)

reconfigure: clean build

# Rebuild just the app binaries (assumes already configured)
load: configure
	cmake --build $(BUILD_DIR) -j$(JOBS) --target load

exercise: configure
	cmake --build $(BUILD_DIR) -j$(JOBS) --target exercise
