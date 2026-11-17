# Can a payload reach the cloud model over HTTPS?
PS5_HOST ?= ps5
PS5_PORT ?= 9021

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined; build with scripts/ps5-build.sh)
endif

ELF := tls-check.elf
CFLAGS := -Wall -Werror -O2 $(shell $(PKG_CONFIG) --cflags libcurl)
ifeq ($(SCEHTTP2),1)
    CFLAGS += -DWITH_SCEHTTP2
endif
LIBS := $(shell $(PKG_CONFIG) --static --libs libcurl) -lSceNet -lSceSsl -lSceHttp2 -lSceNotification

all: $(ELF)

# The CA bundle is linked into the payload, so nothing has to be copied to the console first.
ca-bundle.crt: $(PS5_SYSROOT)$(PS5_HBROOT)/etc/ca-bundle.crt
	cp $< $@

$(ELF): main.c ca.S ca-bundle.crt Makefile
	$(CC) $(CFLAGS) -o $@ main.c ca.S $(LIBS)

clean:
	rm -f $(ELF) ca-bundle.crt

test: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $<
