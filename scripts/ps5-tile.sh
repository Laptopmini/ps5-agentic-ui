#!/usr/bin/env bash
# PS5 A2UI's home-screen tile (payload/tile): a native title that ShadowMountPlus registers and
# that asks websrv to start the app. Built with drakmor/ps5-native-app-boilerplate (pinned below)
# in its own container; the tile program is GPL-3.0-or-later because the boilerplate's startup
# code is linked in.
#
#   scripts/ps5-tile.sh build                           → payload/tile/dist/PPSA95210/
#   PS5_HOST=192.168.0.56 scripts/ps5-tile.sh install   build, then upload to /data/homebrew/PPSA95210
#
# After the first install, ShadowMountPlus adds "PS5 A2UI" to the home screen (it scans
# /data/homebrew; restart it or the console if the tile doesn't appear). The tile needs websrv
# running (add it to Payload Manager's autoload) and the app installed (scripts/ps5-install.sh).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ACTION="${1:-build}"
IMAGE=ps5-agentic-ui/tile-dev:1
REPO=https://github.com/drakmor/ps5-native-app-boilerplate.git
PIN=470695e0c557f99ff2df0e36e4df713c5e636526  # 2026-08-28, verified on 12.70 by its authors
TITLE=PPSA95210

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  docker build -t "$IMAGE" "$ROOT/payload/tile"
fi
# The boilerplate checkout and its fetched SDK stay in payload/tile/.cache between builds.
docker run --rm -v "$ROOT:/src" -e REPO="$REPO" -e PIN="$PIN" "$IMAGE" bash -euo pipefail -c '
  cd /src/payload/tile && mkdir -p .cache
  [[ -d .cache/boilerplate/.git ]] || git clone -q "$REPO" .cache/boilerplate
  cd .cache/boilerplate
  git cat-file -e "$PIN^{commit}" 2>/dev/null || git fetch -q origin "$PIN"
  git checkout -q --force "$PIN" && git clean -fdxq -e .deps
  # Ours over the template: the program, identity and icon; none of its demo art or assets.
  cp ../../src/main.cpp src/main.cpp
  cp ../../sce_sys/param.json ../../sce_sys/icon0.png sce_sys/
  rm -f sce_sys/pic0.dds sce_sys/pic1.dds sce_sys/snd0.at9
  rm -rf assets
  CXX=clang++-18 PS5_CLANG=clang-18 make
  rm -rf ../../dist && mkdir -p ../../dist && cp -a dist/. ../../dist/
'
echo "Built payload/tile/dist/$TITLE" >&2

[[ "$ACTION" == install ]] || exit 0
HOST="${PS5_HOST:?set PS5_HOST to the console IP}"
nc -z -G 3 "$HOST" 2121 2>/dev/null || { echo "Nothing on $HOST:2121. Start ftpsrv on the console." >&2; exit 1; }
SRC="$ROOT/payload/tile/dist/$TITLE"
DEST="/data/homebrew/$TITLE"
# eboot.bin and param.json go last, so ShadowMountPlus never sees a half-copied title.
(cd "$SRC" && find . -type f ! -name eboot.bin ! -name param.json | sed 's|^\./||') | while read -r f; do
  curl -sS --ftp-create-dirs -T "$SRC/$f" "ftp://$HOST:2121$DEST/$f"
done
curl -sS --ftp-create-dirs -T "$SRC/eboot.bin" "ftp://$HOST:2121$DEST/eboot.bin"
curl -sS --ftp-create-dirs -T "$SRC/sce_sys/param.json" "ftp://$HOST:2121$DEST/sce_sys/param.json"
echo "Installed in $DEST" >&2
