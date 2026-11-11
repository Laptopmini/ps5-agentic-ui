# ps5-agentic-ui

Open-source tooling to put **agentic UIs on a jailbroken PlayStation 5** with AG-UI, A2UI 1.0 and CopilotKit — plus a proof-of-concept app (**PS5 A2UI**) built only from that tooling. A2UI surfaces become native, controller-driven PS5 screens; the DualSense, haptics and the system keyboard are the input; the agent runs on a cloud model (any OpenAI-compatible provider via OpenRouter by default), a LAN model, or a model on the PS5's own GPU.

## The libraries (1.0 — in progress, see `docs/PLAN.md` "Libraries 1.0 (M7)")

Four reusable libraries, and one example app that uses only them. They share the **PS5 catalog** (`catalog/ps5.catalog.json`), the A2UI 1.0 contract between agent and renderer.

| Library | What it is | Where | Packaging |
|---|---|---|---|
| **a2ui renderer** (C++) | The A2UI 1.0 renderer for the PS5 catalog: surfaces, data model, focus navigation, dialogs, actions. Platform-independent — you supply a `Canvas`. | [`renderer/`](renderer/README.md) | CMake: `find_package(a2ui 1.0 CONFIG)` → `a2ui::a2ui` |
| **Agent runtime** (TS) | The AG-UI agent that drives the catalog (intent tools → A2UI surfaces + device calls); runs as a Node server or embedded in QuickJS on the console. | [`server/`](server/README.md) + the shared core in [`runtime/`](runtime/) | TS sources; `pnpm build:agent-js` for the console |
| **CopilotKit channel adapter** (TS) | A Channels `PlatformAdapter` so an agent elsewhere reaches the PS5 as a surface. | [`channel/`](channel/README.md) | npm: `copilotkit-ps5` |
| **Device layer** (C++) | Mapping the catalog's device functions to real feedback: haptics, light bar, adaptive triggers (portable model in `renderer/src/haptics.*`), and the console bindings — DualSense, system keyboard, dialogs — as reference. | `renderer/src/haptics.*` (in `a2ui`); `payload/console/{dualsense,sysui,ime}.cpp` | part of `a2ui` + console reference |
| *Example app:* **PS5 A2UI** | The one homebrew app that proves the libraries — its own on-console agent, native screens, voice input, home-screen tile. | `payload/console/` | PS5 payload (its own Makefile) |

## Docs

- [`docs/PLAN.md`](docs/PLAN.md) — goal, architecture and the key design decisions.
- Topical research — condensed and reusable, especially for other PS5 homebrew and agent developers:
  - [`docs/packaging.md`](docs/packaging.md) — getting an app onto the console; payload vs native title; the toolchain. *(The "how to package a PS5 app" write-up.)*
  - [`docs/rendering.md`](docs/rendering.md) — the renderer, software vs GPU, RADV Vulkan and GPU compute.
  - [`docs/voice.md`](docs/voice.md) — DualSense mic capture and on-console whisper STT on the GPU.
  - [`docs/agent.md`](docs/agent.md) — A2UI over AG-UI, the catalog, intent tools, what a model can drive.
  - [`docs/channel.md`](docs/channel.md) — the PS5 as a CopilotKit Channels channel, and background presence.
- [`BACKLOG.md`](BACKLOG.md) — everything still open.
- `catalog/ps5.catalog.json` — the A2UI 1.0 catalog of native PS5 components; `vendor/a2ui-v1_0/` holds the upstream A2UI 1.0 schemas (Apache-2.0).
- `runtime/` — the reference TypeScript agent runtime shared by `server/` and `channel/`: the run loop, argument checks, secret handles, the approval gate, the A2UI-over-AG-UI envelope, the intent tools and the catalog validator.
- `eval/` — research harnesses that produced the findings (e.g. `eval/catalog-eval/run.ts`: can a model drive the PS5 catalog?). Not needed to use the libraries.

## Run it

Set a model key in the environment, then run the agent + the host window on the Mac. The agent speaks to any OpenAI-compatible provider; **OpenRouter is the default**, so you only need a key and a model:

```sh
pnpm install

# OpenRouter (the default):
LLM_API_KEY=...  LLM_MODEL=minimax/minimax-m3  scripts/play.sh

# or any other OpenAI-compatible endpoint (OpenAI, MiniMax direct, LM Studio, Ollama, ProsperoAI on
# the PS5) — point LLM_BASE_URL at it:
LLM_BASE_URL=https://api.minimax.io/v1  LLM_API_KEY=...  LLM_MODEL=MiniMax-M3  scripts/play.sh
```

On the console: `scripts/ps5-play.sh` (its own agent) or `scripts/ps5-channel.sh` (as a channel) — same env vars. A server listening beyond loopback needs an access code in `AGENT_TOKEN`. To reproduce the research, see the harnesses in `eval/` (e.g. `node eval/catalog-eval/run.ts --repeat 2`).

## License

MIT, like AG-UI (`LICENSE`). One exception: the home-screen tile (`payload/tile`) is GPL-3.0-or-later because it links drakmor's boilerplate; it's a separate program that only asks websrv to start the app. Vendored and linked third-party code keeps its own license — see [`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md) (and `vendor/`, `renderer/vendor/`).
