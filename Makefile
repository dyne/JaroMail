# Compatibility commands; CMake owns compilation, installation and packaging.
BUILD_DIR ?= build/gnu
ifeq ($(origin PREFIX),undefined)
PREFIX = $(HOME)/.local
install-system: PREFIX = /usr/local
endif
CFLAGS ?= -O2
export CC CFLAGS CPPFLAGS LDFLAGS LDLIBS PREFIX DESTDIR
.PHONY: all install install-system clean test test-strict man sieve-tools filter-core package
.DEFAULT_GOAL := all
all:
	python3 build/cmake-build.py --output-dir "$(BUILD_DIR)"
install install-system:
	python3 build/cmake-build.py --output-dir "$(BUILD_DIR)" --install
clean:
	python3 build/cmake-build.py --output-dir "$(BUILD_DIR)" --clean
test: all
	zsh extras/test/run-tests.sh
test-strict: all
	JARO_TEST_REQUIRE_ALL=1 zsh extras/test/run-tests.sh
man:
	python3 build/cmake-build.py --output-dir "$(BUILD_DIR)" --target man
sieve-tools:
	python3 build/sieve-tools.py
filter-core:
	python3 build/cmake-build.py --output-dir "$(BUILD_DIR)" --target filter-core
package: all
	cmake --build "$(BUILD_DIR)/cmake" --target package

