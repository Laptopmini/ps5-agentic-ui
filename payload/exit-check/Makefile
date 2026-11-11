PS5_HOST ?= ps5
PS5_PORT ?= 9021

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined; build with scripts/ps5-build.sh)
endif

ELF := exit-check.elf

all: $(ELF)

$(ELF): main.c Makefile
	$(CC) -Wall -Werror -O2 -o $@ main.c

clean:
	rm -f $(ELF)

test: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $<
