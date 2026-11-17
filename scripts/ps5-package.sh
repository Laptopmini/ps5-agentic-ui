#!/usr/bin/env bash
# Package the console app as a zip to attach to a release: the folder a PS5 owner copies to
# /data/homebrew/ with FTP, no build needed.
#
#   scripts/ps5-package.sh [version]        → dist/PS5AgenticUI-<version>.zip
#
# Builds payload/console and payload/presence first (scripts/ps5-build.sh, in the container). The
# zip holds what scripts/ps5-install.sh uploads, plus this project's license, the third-party
# notices and the license texts of everything linked into the build (payload/licenses/).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="${1:-$(node -p "require('$ROOT/package.json').version")}"
SRC="$ROOT/payload/console"
OUT="$ROOT/dist"
STAGE="$OUT/PS5AgenticUI"

"$ROOT/scripts/ps5-build.sh" console
"$ROOT/scripts/ps5-build.sh" presence

rm -rf "$STAGE" && mkdir -p "$STAGE/sce_sys" "$STAGE/licenses"
cp "$SRC/console.elf" "$STAGE/eboot.elf"
cp "$SRC/homebrew.js" "$SRC/background.png" "$STAGE/"
cp "$SRC/sce_sys/icon0.png" "$STAGE/sce_sys/"
cp "$SRC/libs"/* "$STAGE/"
cp "$ROOT/payload/presence/presence.elf" "$STAGE/"
cp "$ROOT/LICENSE" "$STAGE/licenses/ps5-agentic-ui-MIT.txt"
cp "$ROOT/THIRD-PARTY-NOTICES.md" "$STAGE/licenses/"
cp "$ROOT/payload/licenses"/* "$STAGE/licenses/"
cp "$ROOT/renderer/vendor/fonts/OFL.txt" "$STAGE/licenses/NotoFonts-OFL.txt"
cat > "$STAGE/README.txt" <<TXT
PS5 A2UI $VERSION — https://github.com/Laptopmini/ps5-agentic-ui

Install (needs a PS5 that runs homebrew, with ftpsrv and websrv running):

  1. Copy this whole folder to /data/homebrew/PS5AgenticUI on the console (FTP, port 2121).
  2. Open the Homebrew Launcher (websrv) and start "PS5 A2UI".
  3. On the setup screen, press Square for the offline demo, or scan the QR code with a phone to
     give the console a model key (any OpenAI-compatible endpoint).

Voice input (hold R2) needs the speech model: put ggml-base.en.bin (from whisper.cpp's
models/download-ggml-model.sh base.en) in the same folder.

Licenses: this app is MIT. What it links and ships keeps its own license; see licenses/.
TXT

ZIP="$OUT/PS5AgenticUI-$VERSION.zip"
rm -f "$ZIP"
(cd "$OUT" && zip -qr "$(basename "$ZIP")" PS5AgenticUI)
echo "$ZIP ($(du -h "$ZIP" | cut -f1))" >&2
shasum -a 256 "$ZIP"
