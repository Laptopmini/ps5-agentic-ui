#!/usr/bin/env bash
# Build a payload, install it on the console (scripts/ps5-install.sh) and launch it as the
# foreground app (so it gets the display and an app's memory), streaming its output here.
#
#   PS5_HOST=192.168.0.56 scripts/ps5-run.sh console
#
# Needs two payloads running on the console (Payload Manager → Manage Payloads, or autoload):
#   ftpsrv  (FTP on :2121)   receives the build
#   websrv  (HTTP on :8080)  launches it with its homebrew loader (/hbldr)
# websrv's loader runs the ELF inside a placeholder app (FAKE00000, created once under
# /system_ex/app) and closes any running game first. See docs/packaging.md.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NAME="${1:?usage: PS5_HOST=<ip> ps5-run.sh <payload>}"
HOST="${PS5_HOST:?set PS5_HOST to the console IP}"
DIR=/data/homebrew/PS5AgenticUI

"$ROOT/scripts/ps5-install.sh" "$NAME"

if ! nc -z -G 3 "$HOST" 8080 2>/dev/null; then
  echo "Nothing on $HOST:8080. Start websrv on the console (Payload Manager → Manage Payloads)." >&2
  exit 1
fi

# The app's environment, as homebrew.js sets it (space-separated, so %20 in the URL). RENDERER=software
# (optional) is passed too, e.g. if GPU drawing fails on the console.
ENV="LD_LIBRARY_PATH=$DIR"
[[ -n "${RENDERER:-}" ]] && ENV+="%20RENDERER=$RENDERER"
echo "Launching (closes any running game)…" >&2
curl -sSN "http://$HOST:8080/hbldr?path=$DIR/eboot.elf&cwd=$DIR&pipe=1&env=$ENV"
