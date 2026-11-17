# The agent's screen on the console. The renderer's model/view and SDL canvas, built
# for the PS5 with the ported SDL2: sample screens (renderer/fixtures), DualSense navigation and
# the system keyboard. Online with agent.json next to it (scripts/ps5-play.sh), offline otherwise.
PS5_HOST ?= ps5
PS5_PORT ?= 9021

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined; build with scripts/ps5-build.sh)
endif

R := ../../renderer
ELF := console.elf
SRCS := main.cpp ime.cpp sysui.cpp dualsense.cpp $(R)/src/model.cpp $(R)/src/view.cpp $(R)/src/session.cpp $(R)/src/haptics.cpp $(R)/src/canvas_geometry.cpp $(R)/src/canvas_sdl.cpp $(R)/src/canvas_gl.cpp $(R)/src/agent_client.cpp $(R)/src/channel_client.cpp $(R)/src/js_agent.cpp $(R)/src/ps5_system.cpp $(R)/src/ps5_voice.cpp $(R)/src/ps5_whisper.cpp $(R)/src/pairing.cpp $(R)/vendor/qrcodegen/qrcodegen.cpp
CXXFLAGS := -std=c++20 -O2 -Wall -I$(PS5_SYSROOT)$(PS5_HBROOT)/include -I$(R)/include -I$(R)/vendor -I$(R)/src -Iggml/include $(shell $(PKG_CONFIG) --cflags sdl2 libcurl)

# ggml + ggml-vulkan (+ whisper): on-console speech-to-text on the RADV GPU (docs/voice.md).
# Cross-built by scripts/ps5-ggml.sh into ggml/{lib,include}. The archives are mutually referential
# (backend registry <-> base <-> cpu <-> vulkan), so link them as a group. ggml-vulkan dlopen()s
# the RADV libvulkan.so.1 shipped next to the app (Vulkan-Hpp dynamic dispatch); -lvulkan satisfies
# the few direct references and records the NEEDED entry the loader resolves from the app dir.
GGML_LIBS := -Wl,--start-group ggml/lib/libwhisper.a ggml/lib/libggml.a ggml/lib/libggml-vulkan.a ggml/lib/libggml-cpu.a ggml/lib/libggml-base.a -Wl,--end-group -lvulkan

LIBS := $(shell $(PKG_CONFIG) --static --libs sdl2 libcurl) -lsqlite3 $(PS5_SYSROOT)$(PS5_HBROOT)/lib/quickjs/libquickjs.a $(GGML_LIBS) -lSceImeDialog -lSceUserService -lSceSystemService -lSceNotification -lSceRandom -lSceSysmodule

all: $(ELF) libs background.png

# The console-style backdrop drawn behind the UI (loaded by path at run time, not linked in).
background.png: $(R)/assets/background.png
	cp $< $@

# Mesa (OpenGL ES through zink on RADV) goes next to the app: SDL dlopen()s libEGL.so.1 and
# libGLESv2.so.2 from there; libgallium and libvulkan (RADV) are their dependencies.
MESA := libEGL.so.1 libGLESv2.so.2 libgallium-26.2.0.so libvulkan.so.1
libs: $(addprefix libs/,$(MESA))
libs/%: $(PS5_SYSROOT)$(PS5_HBROOT)/lib/%
	mkdir -p libs && cp -L $< $@

# Linked-in assets: the font, and every fixture in one file ("### name" before each).
fixtures.txt: $(wildcard $(R)/fixtures/*.jsonl)
	for f in $^; do echo "### $$(basename $$f .jsonl)"; cat $$f; echo; done > $@

font.ttf: $(R)/vendor/fonts/NotoSans.ttf
	cp $< $@

# Fallback fonts for characters Noto Sans lacks: emoji (monochrome), symbols, arrows.
FALLBACK_FONTS := NotoEmoji.ttf NotoSansSymbols2.ttf NotoSansSymbols.ttf
$(FALLBACK_FONTS): %.ttf: $(R)/vendor/fonts/%.ttf
	cp $< $@

# The agent runtime (built on the Mac by scripts/ps5-build.sh: pnpm build:agent-js) and the CA
# certificates for HTTPS to the model, both linked in.
agent.js: ../../server/qjs/agent.js
	cp $< $@

ca-bundle.crt: $(PS5_SYSROOT)$(PS5_HBROOT)/etc/ca-bundle.crt
	cp $< $@

# The key setup page and the tweetnacl-js it loads, served by the app while the setup screen is up.
pair.html: $(R)/assets/pair.html
	cp $< $@

nacl.min.js: $(R)/vendor/tweetnacl/nacl.min.js
	cp $< $@

tweetnacl.o: $(R)/vendor/tweetnacl/tweetnacl.c
	$(CC) -O2 -I$(R)/vendor -c -o $@ $<

$(ELF): $(SRCS) ime.hpp sysui.hpp dualsense.hpp assets.S fixtures.txt font.ttf $(FALLBACK_FONTS) agent.js ca-bundle.crt pair.html nacl.min.js tweetnacl.o Makefile
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS) tweetnacl.o assets.S $(LIBS)

clean:
	rm -rf $(ELF) fixtures.txt font.ttf $(FALLBACK_FONTS) agent.js ca-bundle.crt pair.html nacl.min.js tweetnacl.o libs background.png

test: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $<
