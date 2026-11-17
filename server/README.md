# The demo agent

The agent the PS5 A2UI app talks to: the agent runtime ([`../runtime/`](../runtime/README.md), npm `ps5-a2ui-agent`) with a console to read and change as its tools. It runs in two places, from the same code:

- **As a Node HTTP server** (`index.ts`): `POST /agent` takes an AG-UI `RunAgentInput` and streams AG-UI events as server-sent events. For development on a desktop, or as a LAN agent the console connects to.
- **Embedded in QuickJS on the console** (`embedded.ts`, bundled to `qjs/agent.js` by `pnpm build:agent-js`): no HTTP. The host (`renderer/src/js_agent.cpp`) provides `fetch`, `setTimeout` and `crypto`, calls `agent.run(input, emit, model)` and gets the same events.

It doubles as the worked example of building an agent on the runtime.

| File | Role |
|---|---|
| `index.ts` | The server: arguments, the access code, per-thread logs in `logs/<thread>.jsonl`. |
| `embedded.ts` | The console entry point (`globalThis.agent.run`). |
| `console-tools.ts` | The agent's tools. Off the console they act on a simulated one; on the console, the reads the app can really do replace the simulated ones. |
| `console-sim.ts` | The simulated console: Wi-Fi, models, saves, games, a profile. |

## Run it

```sh
pnpm install
LLM_API_KEY=... LLM_MODEL=<model id> node server/index.ts --port 8790
# another OpenAI-compatible endpoint:
LLM_BASE_URL=http://localhost:1234/v1 LLM_API_KEY=... LLM_MODEL=<model id> node server/index.ts
```

`scripts/play.sh` starts it together with the desktop window.

Listening beyond loopback (`--host 0.0.0.0`, for the console) **requires an access code** in `AGENT_TOKEN` (16+ characters); every request must then carry `authorization: Bearer <code>`. The token is read from the environment, not the arguments, so it doesn't show in `ps`.

Every event of every conversation is appended to `server/logs/`, on this computer only. Delete the folder whenever you like.
