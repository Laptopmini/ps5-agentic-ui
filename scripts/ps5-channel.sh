#!/usr/bin/env bash
# The PS5 as a CopilotKit channel: starts the PS5 agent's server (this Mac only) and the example
# channel (channel/example.ts, reachable on the LAN behind a fresh access code), tells the console
# app where the channel is, then launches it on the console.
#
#   PS5_HOST=192.168.0.56 scripts/ps5-channel.sh          a session: the app uses this channel until it closes
#   PS5_HOST=192.168.0.56 scripts/ps5-channel.sh serve    a lasting channel: stays up, with a stable access code
#   PS5_HOST=192.168.0.56 scripts/ps5-channel.sh pair     pair the console with that lasting channel (once;
#                                                         open the setup screen first: L1 + R1)
#
# A paired channel is the app's agent whenever it starts, and the app's background payload listens
# to it while the app is closed (toasts for what the agent says), until you pair something else.
# The lasting channel's access code is kept in ~/.config/ps5-agentic-ui/channel-token (never printed).
#
# On the console, ask anything (the agent answers through Channels), or say "demo" for a message
# built with Channels' own components. For a channel that runs on its own, pair the console with it
# from the setup screen (L1 + R1, "A CopilotKit channel") with its address and access code.
#
# - Provide a model key in LLM_API_KEY (and set LLM_MODEL); any OpenAI-compatible provider,
#   OpenRouter by default (override the endpoint with LLM_BASE_URL). Only the set provider vars go to
#   the agent server; it listens on 127.0.0.1 only. The console gets the channel's address and access
#   code, and agent.json is deleted from the console afterwards.
# - With CPK_INTELLIGENCE_API_KEY, conversations are recorded in
#   CopilotKit Intelligence: the channel runs through CopilotKit's runtime (127.0.0.1:RUNTIME_PORT,
#   default 8793) as code CHANNEL_CODE (default ps5-agentic-ui). Only that key and the code go to
#   the channel. Without it, the channel calls the agent directly (no Intelligence).
# - CopilotKit's anonymous telemetry is off (COPILOTKIT_TELEMETRY_DISABLED=true).
# - PRESENCE=1 (M4 spike): also sends the background payload (payload/presence) to the console's
#   ELF loader (:9021), and keeps the channel up after the app closes (Ctrl-C to stop). With the
#   app closed, what the agent says becomes a toast whose button opens the app; try "remind me in
#   60 seconds to …", then close the app (or start a game).
# - Logs: server/logs/ps5-channel-*.log.
set -euo pipefail
cd "$(dirname "$0")/.."
MODE="${1:-session}"
[[ "$MODE" == session || "$MODE" == serve || "$MODE" == pair ]] || { echo "usage: ps5-channel.sh [serve|pair]" >&2; exit 2; }
HOST="${PS5_HOST:?set PS5_HOST to the console IP}"
AGENT_PORT=${AGENT_PORT:-8790}
PORT=${PORT:-8792}
RUNTIME_PORT=${RUNTIME_PORT:-8793}
MODEL=${MODEL:-${LLM_MODEL:-minimax/minimax-m3}}
DIR=/data/homebrew/PS5AgenticUI

if [[ -z "${LLM_API_KEY:-}" ]]; then
  echo "Set LLM_API_KEY (and LLM_MODEL) in the environment. OpenRouter by default; set LLM_BASE_URL for another provider." >&2; exit 1
fi
model_env=()
for v in LLM_API_KEY LLM_BASE_URL LLM_MODEL; do [[ -n "${!v:-}" ]] && model_env+=("$v=${!v}"); done
CHANNEL_CODE=${CHANNEL_CODE:-ps5-agentic-ui}

IFACE=$(route -n get "$HOST" 2>/dev/null | awk '/interface:/ {print $2}')
MAC_IP=$(ipconfig getifaddr "$IFACE" 2>/dev/null || true)
[[ -n "$MAC_IP" ]] || { echo "Can't find this Mac's address on the way to $HOST." >&2; exit 1; }
TOKEN_FILE=~/.config/ps5-agentic-ui/channel-token
if [[ "$MODE" != session && ! -s "$TOKEN_FILE" ]]; then
  mkdir -p "$(dirname "$TOKEN_FILE")"
  (umask 077 && openssl rand -hex 16 > "$TOKEN_FILE")
fi
if [[ "$MODE" == pair ]]; then
  echo "Pairing the console with the lasting channel at http://$MAC_IP:$PORT (the code on the TV is asked next)." >&2
  exec env -i PATH="$PATH" HOME="$HOME" PS5_CHANNEL_TOKEN="$(cat "$TOKEN_FILE")" \
    node scripts/ps5-pair.ts --host "$HOST" --provider channel --url "http://$MAC_IP:$PORT"
fi

PORTS="2121 8080"; [[ "${PRESENCE:-}" == 1 ]] && PORTS+=" 9021"
[[ "$MODE" == serve ]] && PORTS=""
for port in $PORTS; do
  nc -z -G 3 "$HOST" "$port" 2>/dev/null || { echo "Nothing on $HOST:$port. Start ftpsrv and websrv (and, for PRESENCE=1, the ELF loader) on the console." >&2; exit 1; }
done

if [[ "$MODE" == serve ]]; then TOKEN=$(cat "$TOKEN_FILE"); else TOKEN=$(openssl rand -hex 16); fi
mkdir -p server/logs
env -i PATH="$PATH" HOME="$HOME" "${model_env[@]}" \
  node server/index.ts --port "$AGENT_PORT" --model "$MODEL" > server/logs/ps5-channel-agent.log 2>&1 &
agent=$!
env -i PATH="$PATH" HOME="$HOME" PS5_CHANNEL_TOKEN="$TOKEN" COPILOTKIT_TELEMETRY_DISABLED=true \
  CPK_INTELLIGENCE_API_KEY="${CPK_INTELLIGENCE_API_KEY:-}" CHANNEL_CODE="$CHANNEL_CODE" \
  node channel/example.ts --host 0.0.0.0 --port "$PORT" --agent "http://127.0.0.1:$AGENT_PORT/agent" --runtime-port "$RUNTIME_PORT" > server/logs/ps5-channel.log 2>&1 &
channel=$!
cleanup() {
  kill -INT "$channel" 2>/dev/null && sleep 0.5  # the channel tells the background payload to stop
  kill "$channel" "$agent" ${presence:-} 2>/dev/null || true
  [[ "$MODE" == session ]] && { curl -s -o /dev/null "ftp://$HOST:2121/" -Q "DELE $DIR/agent.json" 2>/dev/null || true; }
}
trap cleanup EXIT
for _ in $(seq 1 100); do
  grep -q "channel ready" server/logs/ps5-channel.log 2>/dev/null && break
  sleep 0.2
done
grep -q "channel ready" server/logs/ps5-channel.log || { echo "The channel didn't start; see server/logs/ps5-channel.log" >&2; exit 1; }
grep -o "channel ready: .*" server/logs/ps5-channel.log | sed "s/^channel ready: /Channel /" >&2
echo "Channel on http://$MAC_IP:$PORT (log: server/logs/ps5-channel.log)" >&2
if [[ "$MODE" == serve ]]; then
  echo "Serving until Ctrl-C. Paired consoles use it whenever the app starts (pair once: ps5-channel.sh pair)." >&2
  wait "$channel"
  exit 0
fi

printf '{"channel":"http://%s:%s","token":"%s"}\n' "$MAC_IP" "$PORT" "$TOKEN" |
  curl -sS --ftp-create-dirs -T - "ftp://$HOST:2121$DIR/agent.json"
if [[ "${PRESENCE:-}" == 1 ]]; then
  # The loader streams the payload's output back for as long as it runs: kept in a log.
  echo "Sending the background payload (log: server/logs/ps5-presence.log)…" >&2
  PS5_HOST="$HOST" scripts/ps5-build.sh presence test > server/logs/ps5-presence.log 2>&1 &
  presence=$!
  for _ in $(seq 1 150); do
    grep -q "listening for messages\|no channel\|busy" server/logs/ps5-presence.log 2>/dev/null && break
    sleep 0.2
  done
  grep "\[presence\]" server/logs/ps5-presence.log >&2 || echo "No word from the background payload yet." >&2
  scripts/ps5-run.sh console || true
  echo "The app closed; the channel stays up for background messages (Ctrl-C to stop)." >&2
  wait "$channel"
else
  scripts/ps5-run.sh console
fi
