#!/usr/bin/env bash
# Build a payload and install it in the console's app folder, without launching it. Afterwards the
# console app starts from websrv's Homebrew Launcher (the "Homebrew Launcher" tile → PS5 A2UI),
# no Mac needed.
#
#   PS5_HOST=192.168.0.56 scripts/ps5-install.sh console
#
# Needs ftpsrv (FTP on :2121) running on the console. Uploads to /data/homebrew/PS5AgenticUI:
#   eboot.elf              the build
#   homebrew.js            the launcher entry (path, working folder, library path)
#   sce_sys/icon0.png      the launcher icon
#   libEGL.so.1 …          Mesa for GPU drawing, only when missing or changed
# The stored key (credentials.json) is left alone.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NAME="${1:?usage: PS5_HOST=<ip> ps5-install.sh <payload>}"
HOST="${PS5_HOST:?set PS5_HOST to the console IP}"
DIR=/data/homebrew/PS5AgenticUI
SRC="$ROOT/payload/$NAME"
ELF="$SRC/$NAME.elf"

"$ROOT/scripts/ps5-build.sh" "$NAME"

if ! nc -z -G 3 "$HOST" 2121 2>/dev/null; then
  echo "Nothing on $HOST:2121. Start ftpsrv on the console (Payload Manager → Manage Payloads)." >&2
  exit 1
fi

echo "Uploading $(basename "$ELF") → $DIR/eboot.elf" >&2
curl -sS --ftp-create-dirs -T "$ELF" "ftp://$HOST:2121$DIR/eboot.elf"

# The launcher entry, icon and backdrop (small; sent every time).
[[ -f "$SRC/homebrew.js" ]] && curl -sS -T "$SRC/homebrew.js" "ftp://$HOST:2121$DIR/homebrew.js"
[[ -f "$SRC/sce_sys/icon0.png" ]] && curl -sS --ftp-create-dirs -T "$SRC/sce_sys/icon0.png" "ftp://$HOST:2121$DIR/sce_sys/icon0.png"
[[ -f "$SRC/background.png" ]] && curl -sS -T "$SRC/background.png" "ftp://$HOST:2121$DIR/background.png"

# Shared libraries next to the app (Mesa for GPU drawing): sent only when missing or changed.
if [[ -d "$SRC/libs" ]]; then
  for lib in "$SRC/libs"/*; do
    name=$(basename "$lib")
    local_size=$(stat -f %z "$lib")
    remote_size=$(curl -sI "ftp://$HOST:2121$DIR/$name" 2>/dev/null | awk '/Content-Length/ {print $2}' | tr -d '\r')
    if [[ "$remote_size" != "$local_size" ]]; then
      echo "Uploading $name ($((local_size / 1024 / 1024)) MB)" >&2
      curl -sS -T "$lib" "ftp://$HOST:2121$DIR/$name"
    fi
  done
fi
# The app's background payload (payload/presence) goes next to it: the app starts it when its agent
# is a channel. Uploaded only when it changed (it's ~9 MB).
if [[ "$NAME" == console ]]; then
  "$ROOT/scripts/ps5-build.sh" presence
  P="$ROOT/payload/presence/presence.elf"
  local_size=$(stat -f %z "$P")
  remote_size=$(curl -sI "ftp://$HOST:2121$DIR/presence.elf" 2>/dev/null | awk '/Content-Length/ {print $2}' | tr -d '\r')
  if [[ "$remote_size" != "$local_size" ]]; then
    echo "Uploading presence.elf" >&2
    curl -sS -T "$P" "ftp://$HOST:2121$DIR/presence.elf"
  fi
fi
echo "Installed in $DIR" >&2
