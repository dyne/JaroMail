# Shared policy objects; opt-in until the native helper integration milestone.
.DEFAULT_GOAL := filter-core
FILTER_BUILD_DIR ?= build/gnu/filter
CCACHE ?= $(shell command -v ccache 2>/dev/null)
CFLAGS ?= -O2
FILTER_SOURCES := policy sha256 classify headers
FILTER_OBJECTS := $(addprefix $(FILTER_BUILD_DIR)/,$(addsuffix .o,$(FILTER_SOURCES)))
FILTER_OBJECTS += $(FILTER_BUILD_DIR)/rfc822.o $(FILTER_BUILD_DIR)/helpers.o
FILTER_DRIVER := $(FILTER_BUILD_DIR)/filter-policy-driver

.PHONY: filter-core filter-policy-driver
filter-core: $(FILTER_OBJECTS)
filter-policy-driver: $(FILTER_DRIVER)

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

-include $(FILTER_OBJECTS:.o=.d) $(FILTER_BUILD_DIR)/filter-policy-driver.d
