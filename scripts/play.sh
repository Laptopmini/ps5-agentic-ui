#!/usr/bin/env bash
# Play with the PS5 agent on the Mac: starts the AG-UI server and the a2ui-host window.
# Provide a model key in LLM_API_KEY (and set LLM_MODEL). Any OpenAI-compatible provider works;
# OpenRouter is the default — override the endpoint with LLM_BASE_URL (OpenAI, LM Studio,
# Ollama, ProsperoAI). Only the provider vars that are set are passed to the server.
# Logs: server/logs/<thread>.jsonl.
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ -z "${LLM_API_KEY:-}" || -z "${MODEL:-${LLM_MODEL:-}}" ]]; then
  echo "Set LLM_API_KEY and LLM_MODEL in the environment: a key and a tool-calling model id for any OpenAI-compatible endpoint. OpenRouter by default; set LLM_BASE_URL for another." >&2; exit 1
fi
PORT=${PORT:-8790}
MODEL=${MODEL:-${LLM_MODEL:-}}

# Pass only the provider vars that are actually set (so an empty var doesn't override a default).
model_env=()
for v in LLM_API_KEY LLM_BASE_URL LLM_MODEL LLM_EXTRA_BODY; do [[ -n "${!v:-}" ]] && model_env+=("$v=${!v}"); done

cmake -S renderer -B renderer/build >/dev/null && cmake --build renderer/build -j 8 >/dev/null
env -i PATH="$PATH" HOME="$HOME" "${model_env[@]}" node server/index.ts --port "$PORT" --model "$MODEL" &
server=$!
trap 'kill $server 2>/dev/null' EXIT
for _ in $(seq 1 50); do curl -s -m 1 "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.1; done
renderer/build/a2ui-host --agent "http://127.0.0.1:$PORT/agent"
