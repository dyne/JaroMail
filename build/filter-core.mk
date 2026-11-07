# Compatibility for existing native-suite callers; target definitions are in CMake.
.DEFAULT_GOAL := filter-core
FILTER_BUILD_DIR ?= build/gnu/filter
CFLAGS ?= -O2
export CC CFLAGS CPPFLAGS LDFLAGS LDLIBS
.PHONY: filter-core filter-policy-driver filter-compiler-driver filter-compile filter-generation-tools filter-batch-tools
filter-core filter-policy-driver filter-compiler-driver filter-compile filter-generation-tools filter-batch-tools:
	python3 build/cmake-build.py --output-dir "$(FILTER_BUILD_DIR)" --testing --target "$@"
