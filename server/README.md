# Agent runtime

The AG-UI agent that drives the PS5 A2UI catalog: it turns a conversation into **native PS5 screens** (A2UI 1.0 surfaces) and **device feedback** (haptics, light bar, triggers), checks the model's tool calls against the catalog, and runs console/domain tools. The same runtime runs in two places:

- **As a Node HTTP server** (`index.ts`) — `POST /agent` takes an AG-UI `RunAgentInput` and streams AG-UI events as SSE. For development on a Mac/PC, or a LAN agent the console connects to.
- **Embedded in QuickJS on the console** (`embedded.ts` → `qjs/agent.js`) — the exact same `AgentThread`, no HTTP: the host (`renderer/src/js_agent.cpp`) provides `fetch`/`setTimeout`/`crypto` and calls `agent.run(input, emit, model)`, getting the same events. So the console can run its own agent with no server.

## Pieces

| File | Role |
|---|---|
| `thread.ts` | `AgentThread`: one conversation. Keeps the model-facing history, the (simulated or real) console, password secrets and any pending approval **server-side**, so clients send only what's new. Emits the AG-UI event stream. |
| `index.ts` | The HTTP server + SSE, per-thread logging to `logs/<thread>.jsonl`, and the access-code gate. |
| `embedded.ts` | The console entry point (`globalThis.agent.run`), bundled to `qjs/agent.js` by `pnpm build:agent-js`. |
| `../runtime/intent-tools.ts` | The flat **intent tools** the model calls, and their translation: `toA2UI` (→ A2UI surface) and `toFunctionCalls` (→ catalog device calls). |
| `../runtime/llm.ts` | The reference runtime: the system prompt, the run loop, argument checks, secret handles, the approval gate. |
| `../runtime/console-sim.ts` | A simulated console for host testing; on the console the real `__ps5System` is injected. |

## Models

Any **OpenAI-compatible** provider via `LLM_API_KEY` (+ `LLM_MODEL`); **OpenRouter** by default, override the endpoint with `LLM_BASE_URL` (OpenAI, MiniMax direct, LM Studio or Ollama on the LAN, or ProsperoAI on the PS5 itself). Switching providers is configuration, not code.

## Run it (Node server)

```sh
pnpm install
LLM_API_KEY=... node server/index.ts --port 8790 --model minimax/minimax-m3 --host 127.0.0.1
# another OpenAI-compatible endpoint (point LLM_BASE_URL at it):
LLM_BASE_URL=http://localhost:1234/v1 LLM_API_KEY=... LLM_MODEL=... node server/index.ts
```

Listening beyond loopback (`--host 0.0.0.0`, for the console) **requires an access code** in `AGENT_TOKEN` (16+ chars); every request must then carry `authorization: Bearer <code>`, so other devices on the network can't spend your model key. The token is read from the env, not argv, so it doesn't show in `ps`.

## Embed it (console / your own host)

```ts
import "./server/embedded.ts";            // defines globalThis.agent
// host provides fetch, setTimeout, crypto.randomUUID, process.env, and optionally __ps5System
await globalThis.agent.run(runAgentInputJson, (eventJson) => emit(eventJson), "minimax/minimax-m3");
```

## The event contract (AG-UI)

Per run: `RUN_STARTED` → `TEXT_MESSAGE_START/CONTENT/END`, `TOOL_CALL_START/ARGS/END/RESULT`, `ACTIVITY_SNAPSHOT` (`generative-ui-surface` = a surface's A2UI 1.0 operations; `generative-ui-function-call` = a catalog function for the client to run), `CUSTOM "status"` (thinking / working / retrying) → `RUN_FINISHED` or `RUN_ERROR`. A button press comes back as an A2UI 1.0 action message in `forwardedProps.copilotkit.ui`, the way CopilotKit 1.85 sends it. See `../runtime/a2ui.ts` and `../catalog/ps5.catalog.json`.
