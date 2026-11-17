#!/usr/bin/env bash
# Build (and optionally send) a PS5 payload from payload/<name>, inside the payload-dev container.
#
#   scripts/ps5-build.sh tls-check              build payload/tls-check/tls-check.elf
#   PS5_HOST=192.168.1.50 scripts/ps5-build.sh tls-check test
#                                               build, then send it to the console's ELF loader (:9021)
#
# The image (Debian + LLVM + the prebuilt ps5-payload-dev bundle) is built on first use.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NAME="${1:?usage: ps5-build.sh <payload> [make target]}"
shift
IMAGE=ps5-agentic-ui/payload-dev:0.41.1

# The console app embeds the agent runtime bundle; esbuild runs here, not in the container.
# It also links ggml + ggml-vulkan (on-console STT); build those libs if missing.
if [[ "$NAME" == console ]]; then
  (cd "$ROOT" && pnpm -s build:agent-js >/dev/null)
  "$ROOT/scripts/ps5-ggml.sh"
fi

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  echo "Building $IMAGE (first run downloads ~230 MB)…" >&2
  docker build -t "$IMAGE" "$ROOT/payload"
fi
# The whole repo is mounted (payloads compile renderer/ too); builds write only under payload/.
docker run --rm \
  -v "$ROOT:/src" \
  -e PS5_HOST="${PS5_HOST:-ps5}" -e PS5_PORT="${PS5_PORT:-9021}" \
  "$IMAGE" make -C "/src/payload/$NAME" "$@"
