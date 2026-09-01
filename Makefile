# GNDHOG ZERO for M5Stack Cardputer Zero (Raspberry Pi CM0 / ARM64 Debian)
#
#   make            host build (x86-64 dev/test, offscreen + sim FC)
#   make arm64      cross build for the Cardputer Zero
#   make test       run the built-in self tests
#   make package    build and validate the Cardputer Zero AppStore .deb
#   make -j1        on-device native build (respect the 256M ARM split)

NAME     := bfcli
SRCDIR   := src
BUILD    ?= build
CXX      ?= g++
OPT      ?= -O2
STRIP    ?= true

CXXFLAGS := -std=c++17 -Wall -Wextra -Wno-unused-parameter $(OPT) \
            -ffunction-sections -fdata-sections
CPPFLAGS += -I$(BUILD)
LDFLAGS  ?= -Wl,--gc-sections
LDLIBS   :=

SRCS := $(sort $(wildcard $(SRCDIR)/*.cpp))
OBJS := $(patsubst $(SRCDIR)/%.cpp,$(BUILD)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

.PHONY: all arm64 test package store-check clean install-local FORCE

all: $(BUILD)/$(NAME)

$(BUILD)/$(NAME): $(OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/%.o: $(SRCDIR)/%.cpp $(BUILD)/build_info.h
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c -o $@ $<

# Recheck Git even when no .cpp changed (e.g. the commit after a test build).
# The script only updates the header when the identity actually changes.
$(BUILD)/build_info.h: FORCE
	@sh tools/build-info.sh $@

# Cross build for the Cardputer Zero. libstdc++/libgcc are linked statically so
# the binary does not depend on the exact GCC runtime of the vendor image.
arm64:
	$(MAKE) CXX=aarch64-linux-gnu-g++ BUILD=build-arm64 \
	        LDFLAGS="-Wl,--gc-sections -static-libstdc++ -static-libgcc"

test: $(BUILD)/$(NAME)
	$(BUILD)/$(NAME) --selftest
	@sh tools/test-cli-options.sh $(BUILD)/$(NAME)
	@TZ=UTC sh tools/test-build-info.sh

package: arm64
	@sh tools/package.sh

store-check:
	@python3 tools/validate-app-store.py

clean:
	rm -rf build build-arm64

-include $(DEPS)
