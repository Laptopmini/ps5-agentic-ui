# Vulkan compute smoke (finding 25, Track B). Brings up a Vulkan compute device from a payload via
# the RADV libvulkan.so.1 the app already ships, to see if on-console whisper can be ggml-vulkan in
# a payload (no static RADV, no native title). dlopens the driver, so it links no vulkan lib.
# Sent to the ELF loader so it runs without the app; see main.cpp.
PS5_HOST ?= ps5
PS5_PORT ?= 9021

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined; build with scripts/ps5-build.sh)
endif

ELF := vulkan-smoke.elf
CXXFLAGS := -std=c++17 -O2 -Wall -I$(PS5_SYSROOT)$(PS5_HBROOT)/include
LIBS :=

all: $(ELF)

$(ELF): main.cpp Makefile
	$(CXX) $(CXXFLAGS) -o $@ main.cpp $(LIBS)

clean:
	rm -f $(ELF)

test: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $<
