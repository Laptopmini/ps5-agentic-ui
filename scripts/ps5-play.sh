#!/usr/bin/env bash
# Play with the agent on the PS5: starts the AG-UI server on this Mac (reachable on the LAN, behind
# a fresh access code), tells the console app where it is, then launches it on the console.
#
#   PS5_HOST=192.168.0.56 scripts/ps5-play.sh
#
# Without the Mac: run scripts/ps5-run.sh console and set the console up from its setup screen
# (a phone, or `pnpm pair --host <console>`). The key is sent encrypted and kept on the console.
#
# Needs ftpsrv and websrv running on the console (see scripts/ps5-run.sh).
# - Provide a model key in LLM_API_KEY (and set LLM_MODEL). Any OpenAI-compatible provider works;
#   OpenRouter by default, override the endpoint with LLM_BASE_URL. Only the set provider vars go to
#   the server; the key never leaves this Mac — the console only gets the server's URL and the code.
# - The access code is random per session, and agent.json is deleted from the console afterwards.
# - Server log: server/logs/ps5-play-server.log; per-thread events: server/logs/<thread>.jsonl.
set -euo pipefail
cd "$(dirname "$0")/.."
HOST="${PS5_HOST:?set PS5_HOST to the console IP}"
PORT=${PORT:-8790}
MODEL=${MODEL:-${LLM_MODEL:-minimax/minimax-m3}}
DIR=/data/homebrew/PS5AgenticUI

if [[ -z "${LLM_API_KEY:-}" ]]; then
  echo "Set LLM_API_KEY (and LLM_MODEL) in the environment. OpenRouter by default; set LLM_BASE_URL for another provider." >&2; exit 1
fi
model_env=()
for v in LLM_API_KEY LLM_BASE_URL LLM_MODEL; do [[ -n "${!v:-}" ]] && model_env+=("$v=${!v}"); done

# This Mac's address on the console's network.
IFACE=$(route -n get "$HOST" 2>/dev/null | awk '/interface:/ {print $2}')
MAC_IP=$(ipconfig getifaddr "$IFACE" 2>/dev/null || true)
[[ -n "$MAC_IP" ]] || { echo "Can't find this Mac's address on the way to $HOST." >&2; exit 1; }

for port in 2121 8080; do
  nc -z -G 3 "$HOST" "$port" 2>/dev/null || { echo "Nothing on $HOST:$port. Start ftpsrv and websrv on the console." >&2; exit 1; }
done

TOKEN=$(openssl rand -hex 16)
mkdir -p server/logs
env -i PATH="$PATH" HOME="$HOME" "${model_env[@]}" AGENT_TOKEN="$TOKEN" \
  node server/index.ts --host 0.0.0.0 --port "$PORT" --model "$MODEL" > server/logs/ps5-play-server.log 2>&1 &
server=$!
cleanup() {
  kill "$server" 2>/dev/null || true
  curl -s -o /dev/null "ftp://$HOST:2121/" -Q "DELE $DIR/agent.json" 2>/dev/null || true
}
trap cleanup EXIT
for _ in $(seq 1 50); do
  curl -s -m 1 -H "authorization: Bearer $TOKEN" "http://127.0.0.1:$PORT/health" >/dev/null && break
  sleep 0.1
done
echo "Agent server on http://$MAC_IP:$PORT/agent (log: server/logs/ps5-play-server.log)" >&2

printf '{"url":"http://%s:%s/agent","token":"%s"}\n' "$MAC_IP" "$PORT" "$TOKEN" |
  curl -sS --ftp-create-dirs -T - "ftp://$HOST:2121$DIR/agent.json"
scripts/ps5-run.sh console
