# Toast test: how many buttons a system toast can show, and what a press does. Sent to the ELF loader so it runs without the app;
# see main.cpp.
PS5_HOST ?= ps5
PS5_PORT ?= 9021

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined; build with scripts/ps5-build.sh)
endif

R := ../../renderer
ELF := toast-test.elf
CXXFLAGS := -std=c++20 -O2 -Wall -I$(R)/vendor
LIBS := -lSceNotification -lSceSystemService

all: $(ELF)

$(ELF): main.cpp Makefile
	$(CXX) $(CXXFLAGS) -o $@ main.cpp $(LIBS)

clean:
	rm -f $(ELF)

test: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $<
