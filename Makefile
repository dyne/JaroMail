# Compatibility commands; CMake owns compilation, installation and packaging.
BUILD_DIR ?= build/gnu
PREFIX ?= /usr/local
CFLAGS ?= -O2
export CC CFLAGS CPPFLAGS LDFLAGS LDLIBS PREFIX
.PHONY: all install clean test test-strict man sieve-tools filter-core package
all:
	python3 build/cmake-build.py --output-dir "$(BUILD_DIR)"
install:
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

