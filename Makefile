# BetaflightCLI for M5Stack Cardputer Zero (Raspberry Pi CM0 / ARM64 Debian)
#
#   make            host build (x86-64 dev/test, offscreen + sim FC)
#   make arm64      cross build for the Cardputer Zero
#   make test       run the built-in self tests
#   make -j1        on-device native build (respect the 256M ARM split)

NAME     := bfcli
SRCDIR   := src
BUILD    ?= build
CXX      ?= g++
OPT      ?= -O2
STRIP    ?= true

CXXFLAGS := -std=c++17 -Wall -Wextra -Wno-unused-parameter $(OPT) \
            -ffunction-sections -fdata-sections
LDFLAGS  ?= -Wl,--gc-sections
LDLIBS   :=

SRCS := $(sort $(wildcard $(SRCDIR)/*.cpp))
OBJS := $(patsubst $(SRCDIR)/%.cpp,$(BUILD)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

.PHONY: all arm64 test clean install-local

all: $(BUILD)/$(NAME)

$(BUILD)/$(NAME): $(OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/%.o: $(SRCDIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

# Cross build for the Cardputer Zero. libstdc++/libgcc are linked statically so
# the binary does not depend on the exact GCC runtime of the vendor image.
arm64:
	$(MAKE) CXX=aarch64-linux-gnu-g++ BUILD=build-arm64 \
	        LDFLAGS="-Wl,--gc-sections -static-libstdc++ -static-libgcc"

test: $(BUILD)/$(NAME)
	$(BUILD)/$(NAME) --selftest

clean:
	rm -rf build build-arm64

-include $(DEPS)
