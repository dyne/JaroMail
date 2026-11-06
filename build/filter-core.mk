# Shared policy objects; opt-in until the native helper integration milestone.
.DEFAULT_GOAL := filter-core
FILTER_BUILD_DIR ?= build/gnu/filter
CCACHE ?= $(shell command -v ccache 2>/dev/null)
CFLAGS ?= -O2
FILTER_SOURCES := policy sha256 classify headers
FILTER_OBJECTS := $(addprefix $(FILTER_BUILD_DIR)/,$(addsuffix .o,$(FILTER_SOURCES)))
FILTER_OBJECTS += $(FILTER_BUILD_DIR)/rfc822.o $(FILTER_BUILD_DIR)/helpers.o
FILTER_DRIVER := $(FILTER_BUILD_DIR)/filter-policy-driver
FILTER_PRODUCT_OBJECTS := $(addprefix $(FILTER_BUILD_DIR)/,$(addsuffix .o,product_util profile regex_subset emit cache snapshot))
FILTER_COMPILER_DRIVER := $(FILTER_BUILD_DIR)/filter-compiler-driver

.PHONY: filter-core filter-policy-driver filter-compiler-driver filter-compile filter-generation-tools
filter-core: $(FILTER_OBJECTS)
filter-policy-driver: $(FILTER_DRIVER)
filter-compiler-driver: $(FILTER_COMPILER_DRIVER)
filter-compile: $(FILTER_BUILD_DIR)/jaro-filter-compile $(FILTER_BUILD_DIR)/jaro-filter-legacy
filter-generation-tools: $(FILTER_BUILD_DIR)/jaro-filter-compile-test $(FILTER_BUILD_DIR)/filter-generation-pin

$(FILTER_BUILD_DIR)/generation-test.o: src/filter/generation.c | $(FILTER_BUILD_DIR)
	$(if $(filter ccache sccache distcc,$(notdir $(firstword $(CC)))),,$(CCACHE)) $(CC) $(CPPFLAGS) $(CFLAGS) -DJF_TESTING -std=c99 -MMD -MP -c "$<" -o "$@"

$(FILTER_BUILD_DIR)/jaro-filter-compile.o: src/jaro-filter-compile.c | $(FILTER_BUILD_DIR)
	$(if $(filter ccache sccache distcc,$(notdir $(firstword $(CC)))),,$(CCACHE)) $(CC) $(CPPFLAGS) $(CFLAGS) -std=c99 -MMD -MP -c "$<" -o "$@"

$(FILTER_BUILD_DIR)/jaro-filter-legacy.o: src/jaro-filter-legacy.c | $(FILTER_BUILD_DIR)
	$(if $(filter ccache sccache distcc,$(notdir $(firstword $(CC)))),,$(CCACHE)) $(CC) $(CPPFLAGS) $(CFLAGS) -std=c99 -MMD -MP -c "$<" -o "$@"

$(FILTER_BUILD_DIR)/jaro-filter-legacy: $(FILTER_BUILD_DIR)/jaro-filter-legacy.o $(FILTER_BUILD_DIR)/generation.o $(FILTER_OBJECTS) $(FILTER_PRODUCT_OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o "$@" $^ $(LDLIBS)

$(FILTER_BUILD_DIR)/generation-hooks.o: extras/test/filter-generation-driver.c | $(FILTER_BUILD_DIR)
	$(if $(filter ccache sccache distcc,$(notdir $(firstword $(CC)))),,$(CCACHE)) $(CC) $(CPPFLAGS) $(CFLAGS) -DJF_TESTING -std=c99 -I src -MMD -MP -c "$<" -o "$@"

$(FILTER_BUILD_DIR)/generation-pin.o: extras/test/filter-generation-driver.c | $(FILTER_BUILD_DIR)
	$(if $(filter ccache sccache distcc,$(notdir $(firstword $(CC)))),,$(CCACHE)) $(CC) $(CPPFLAGS) $(CFLAGS) -DJF_TESTING -DJF_PIN_DRIVER -std=c99 -I src -MMD -MP -c "$<" -o "$@"

$(FILTER_BUILD_DIR)/jaro-filter-compile: $(FILTER_BUILD_DIR)/jaro-filter-compile.o $(FILTER_BUILD_DIR)/generation.o $(FILTER_OBJECTS) $(FILTER_PRODUCT_OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o "$@" $^ $(LDLIBS)

$(FILTER_BUILD_DIR)/jaro-filter-compile-test: $(FILTER_BUILD_DIR)/jaro-filter-compile.o $(FILTER_BUILD_DIR)/generation-test.o $(FILTER_BUILD_DIR)/generation-hooks.o $(FILTER_OBJECTS) $(FILTER_PRODUCT_OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o "$@" $^ $(LDLIBS)

$(FILTER_BUILD_DIR)/filter-generation-pin: $(FILTER_BUILD_DIR)/generation-pin.o $(FILTER_BUILD_DIR)/generation-test.o $(FILTER_OBJECTS) $(FILTER_PRODUCT_OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o "$@" $^ $(LDLIBS)

$(FILTER_BUILD_DIR):
	mkdir -p "$@"

$(FILTER_BUILD_DIR)/%.o: src/filter/%.c | $(FILTER_BUILD_DIR)
	$(if $(filter ccache sccache distcc,$(notdir $(firstword $(CC)))),,$(CCACHE)) $(CC) $(CPPFLAGS) $(CFLAGS) -std=c99 -MMD -MP -c "$<" -o "$@"

$(FILTER_BUILD_DIR)/rfc822.o: src/rfc822.c | $(FILTER_BUILD_DIR)
	$(if $(filter ccache sccache distcc,$(notdir $(firstword $(CC)))),,$(CCACHE)) $(CC) $(CPPFLAGS) $(CFLAGS) -std=c99 -MMD -MP -c "$<" -o "$@"

$(FILTER_BUILD_DIR)/helpers.o: src/helpers.c | $(FILTER_BUILD_DIR)
	$(if $(filter ccache sccache distcc,$(notdir $(firstword $(CC)))),,$(CCACHE)) $(CC) $(CPPFLAGS) $(CFLAGS) -std=c99 -MMD -MP -c "$<" -o "$@"

$(FILTER_BUILD_DIR)/filter-policy-driver.o: extras/test/filter-policy-driver.c | $(FILTER_BUILD_DIR)
	$(if $(filter ccache sccache distcc,$(notdir $(firstword $(CC)))),,$(CCACHE)) $(CC) $(CPPFLAGS) $(CFLAGS) -std=c99 -I src -MMD -MP -c "$<" -o "$@"

$(FILTER_DRIVER): $(FILTER_BUILD_DIR)/filter-policy-driver.o $(FILTER_OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o "$@" $^ $(LDLIBS)

$(FILTER_BUILD_DIR)/filter-compiler-driver.o: extras/test/filter-compiler-driver.c | $(FILTER_BUILD_DIR)
	$(if $(filter ccache sccache distcc,$(notdir $(firstword $(CC)))),,$(CCACHE)) $(CC) $(CPPFLAGS) $(CFLAGS) -std=c99 -I src -MMD -MP -c "$<" -o "$@"

$(FILTER_COMPILER_DRIVER): $(FILTER_BUILD_DIR)/filter-compiler-driver.o $(FILTER_OBJECTS) $(FILTER_PRODUCT_OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o "$@" $^ $(LDLIBS)

-include $(FILTER_OBJECTS:.o=.d) $(FILTER_PRODUCT_OBJECTS:.o=.d) $(FILTER_BUILD_DIR)/filter-policy-driver.d $(FILTER_BUILD_DIR)/filter-compiler-driver.d
-include $(addprefix $(FILTER_BUILD_DIR)/,generation.d generation-test.d jaro-filter-compile.d jaro-filter-legacy.d generation-hooks.d generation-pin.d)
