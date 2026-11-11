#!/usr/bin/env bash
# Track A (docs/packaging.md): build a native FSELF title from a software-rendered app, no Mesa.
# Compiles in the payload-dev image (prospero toolchain + static SDL2 + Sce stubs) and packages with
# the ps5-native-app-boilerplate's prebuilt ps5-native-tool + clean-room libc.prx + CRT (from the
# tile build cache). First target: payload/native-smoke (SDL software + the 2D canvas, no agent).
#
#   scripts/ps5-native.sh                       -> payload/native-smoke/dist/PPSA99501/
#   PS5_HOST=192.168.0.56 scripts/ps5-native.sh install   build, then FTP to /data/homebrew/PPSA99501
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ACTION="${1:-build}"
cp "$ROOT/renderer/vendor/fonts/NotoSans.ttf" "$ROOT/payload/native-smoke/src/NotoSans.ttf"
IMAGE=ps5-agentic-ui/payload-dev:0.41.1
APP=native-smoke
TITLE=PPSA99501
BP="payload/tile/.cache/boilerplate"   # prebuilt ps5-native-tool, libc.prx, CRT, linker script

[[ -x "$ROOT/$BP/build/host/ps5-native-tool" && -f "$ROOT/$BP/runtime/libc.prx" ]] || {
  echo "Missing boilerplate artifacts. Build the tile once first: scripts/ps5-tile.sh build" >&2
  exit 2
}

docker run --rm -v "$ROOT:/src" "$IMAGE" bash -euo pipefail -c '
  set -x
  SDK=/opt/ps5-payload-sdk
  HB=$SDK/target/user/homebrew
  BP=/src/'"$BP"'
  NATIVE=$BP/tooling/native
  SRC=/src/payload/'"$APP"'/src
  OUT=/src/payload/'"$APP"'/build
  DIST=/src/payload/'"$APP"'/dist/'"$TITLE"'
  CLANGXX=$SDK/bin/prospero-clang++
  LLD=$SDK/bin/prospero-lld
  rm -rf "$OUT" "$DIST"; mkdir -p "$OUT/obj" "$DIST/sce_sys" "$DIST/sce_module"

  INC="-I/src/renderer/include -I/src/renderer/src -I/src/renderer/vendor -I$HB/include -I$HB/include/SDL2"
  CXXFLAGS="-std=c++20 -O2 -Wall -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections -DSDL_MAIN_HANDLED -DA2UI_NO_FONT_FILE $INC"

  # App + the renderer 2D canvas (no model/view/session/json needed for the smoke).
  for f in "$SRC/main.cpp" /src/renderer/src/canvas_sdl.cpp /src/renderer/src/canvas_geometry.cpp; do
    o="$OUT/obj/$(basename "$f").o"; $CLANGXX $CXXFLAGS -c "$f" -o "$o"
  done
  # The embedded font (assets.S .incbin NotoSans.ttf, resolved from SRC).
  $CLANGXX -I"$SRC" -c "$SRC/assets.S" -o "$OUT/obj/assets.o"
  $SDK/bin/prospero-clang -std=c11 -O2 -c "$SRC/native_compat.c" -o "$OUT/obj/native_compat.o"
  # The boilerplate CRT (its _start and C++ init), as it compiles them.
  for f in app_crt.cpp app_cpp_runtime.cpp; do
    $CLANGXX -std=c++20 -O2 -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections -c "$NATIVE/$f" -o "$OUT/obj/$f.o"
  done

  # Route 2, hybrid (docs/packaging.md): link through the prospero-clang++ driver (so it supplies
  # compiler-rt for emulated-TLS, libc++/libc++abi/libunwind and the system libc) BUT with the
  # boilerplate clean-room layout (-nostartfiles -T ps5-pie.ld + the boilerplate CRT, -e _start).
  # That layout is what makes ps5-native-tool rewrite the module names to the .prx form the native
  # loader wants (the SDK stubs all SONAME as .sprx). `-lkernel` makes the driver drop its default
  # `-lkernel_web` (the WebKit-payload kernel); a home-launched title needs libkernel.
  "$CLANGXX" -pie -fPIE -nostartfiles -Wl,-z,notext \
    -T "$SRC/ps5-pie.ld" -Wl,-e,_start -Wl,--version-script,"$SRC/exports.map" \
    -L"$HB/lib" -o "$OUT/llvm-pie.elf" \
    "$OUT/obj/app_crt.cpp.o" "$OUT/obj/app_cpp_runtime.cpp.o" \
    "$OUT/obj/main.cpp.o" "$OUT/obj/canvas_sdl.cpp.o" "$OUT/obj/canvas_geometry.cpp.o" "$OUT/obj/assets.o" "$OUT/obj/native_compat.o" \
    "$HB/lib/libSDL2.a" "$HB/lib/libiconv.a" -lsamplerate \
    -lScePad -lSceAudioOut -lSceVideoOut -lSceUserService -lSceSystemService -lSceNotification \
    -lSceKeyboard -lSceImeDialog -lkernel

  TOOL=$BP/build/host/ps5-native-tool
  "$TOOL" link --in "$OUT/llvm-pie.elf" --out "$OUT/eboot.elf" \
    --stub-dir "$SDK/target/lib" --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf
  "$TOOL" self --sign --in "$OUT/eboot.elf" --out "$DIST/eboot.bin" --magic 0x1D3D154F
  "$TOOL" self --inspect --file "$DIST/eboot.bin"

  cp /src/payload/'"$APP"'/sce_sys/param.json "$DIST/sce_sys/"
  cp /src/payload/'"$APP"'/sce_sys/icon0.png "$DIST/sce_sys/"
  cp "$BP/runtime/libc.prx" "$DIST/sce_module/libc.prx"
  echo "built $DIST"
'
echo "Built payload/$APP/dist/$TITLE" >&2

if [[ "$ACTION" == install ]]; then
  HOST="${PS5_HOST:?set PS5_HOST to the console IP}"
  DIR="/data/homebrew/$TITLE"
  nc -z -G 3 "$HOST" 2121 2>/dev/null || { echo "Nothing on $HOST:2121 (start ftpsrv)." >&2; exit 1; }
  D="$ROOT/payload/$APP/dist/$TITLE"
  curl -sS --ftp-create-dirs -T "$D/sce_sys/icon0.png" "ftp://$HOST:2121$DIR/sce_sys/icon0.png"
  curl -sS --ftp-create-dirs -T "$D/sce_module/libc.prx" "ftp://$HOST:2121$DIR/sce_module/libc.prx"
  curl -sS -T "$D/eboot.bin" "ftp://$HOST:2121$DIR/eboot.bin"
  curl -sS -T "$D/sce_sys/param.json" "ftp://$HOST:2121$DIR/sce_sys/param.json"   # last, so it registers once complete
  echo "Installed to $DIR. Restart ShadowMountPlus (or the console) if the tile doesn't appear." >&2
fi
