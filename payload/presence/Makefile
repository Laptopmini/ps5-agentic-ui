# The background payload (no window). Sent to the ELF loader so it stays resident;
# see main.cpp.
PS5_HOST ?= ps5
PS5_PORT ?= 9021

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined; build with scripts/ps5-build.sh)
endif

R := ../../renderer
ELF := presence.elf
CXXFLAGS := -std=c++20 -O2 -Wall -I$(R)/vendor $(shell $(PKG_CONFIG) --cflags libcurl)
LIBS := $(shell $(PKG_CONFIG) --static --libs libcurl) -lSceNotification -lSceSystemService

all: $(ELF)

$(ELF): main.cpp Makefile
	$(CXX) $(CXXFLAGS) -o $@ main.cpp $(LIBS)

clean:
	rm -f $(ELF)

test: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $<
