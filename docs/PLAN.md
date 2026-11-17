# Plan & architecture

Agentic UI on a PS5 with AG-UI, A2UI 1.0 and CopilotKit. This is the overview and the
design decisions; the research is in the topical docs below, and open work is in
[`../BACKLOG.md`](../BACKLOG.md).

## Goal

Make the PS5 a first-class place for CopilotKit-style agents, as **open-source tooling anyone can
reuse** plus a proof-of-concept app that uses only that tooling:

1. Serve AG-UI from the console (a cloud model, a LAN model, or a model on the PS5's GPU).
2. Let an agent drive **native PS5 UI** (menus, shelves, the system keyboard, dialogs, button hints,
   haptics), not a web page in the console browser.
3. Ship a demo (the **PS5 A2UI** app) with the DualSense mic as an input, that others can learn from.

A proof of concept, not a product — but the libraries should be clean enough to adopt.

## The libraries

Three packages and one example app, sharing the **PS5 catalog** (the A2UI 1.0 contract). The
[top-level README](../README.md#whats-in-here) maps them; in short: the **a2ui renderer** (C++, the
CMake target `a2ui::a2ui`, which includes the portable haptics model), the **agent runtime** (TS,
npm `ps5-a2ui-agent`, in Node or embedded in QuickJS; see [agent.md](agent.md)) and the
**CopilotKit Channels adapter** (npm `copilotkit-channels-ps5`; see [channel.md](channel.md)). The
example app is `payload/console` (see [packaging.md](packaging.md)); it is also the reference for
the console bindings (DualSense, system keyboard, dialogs), and its agent (`server/`) is the worked
example of building on the runtime.

## Topical research

- [agent.md](agent.md) — A2UI over AG-UI, the catalog, intent tools vs freeform render, what a model
  can drive, native-UI-first.
- [packaging.md](packaging.md) — getting an app onto the console; payload vs native title; the
  toolchain; gotchas. *(Most reusable for other PS5 homebrew.)*
- [rendering.md](rendering.md) — the renderer, software vs GPU, and RADV Vulkan / GPU compute.
- [voice.md](voice.md) — DualSense mic capture and on-console whisper STT on the GPU.
- [channel.md](channel.md) — the PS5 as a CopilotKit Channels channel, and background presence.

## Key decisions

- **Intelligence is optional.** First and foremost this is open tooling that works on its own runner;
  CopilotKit Intelligence is supported for whoever wants it (and because OpenDots needs it), but the
  channel must not depend on it. (The one internal seam is the start hook — see docs/channel.md.)
- **Model strategy:** any OpenAI-compatible endpoint via `LLM_API_KEY` and `LLM_MODEL`, with
  **OpenRouter** as the default endpoint (override it with `LLM_BASE_URL`). No model is built in;
  the design target is a small, inexpensive model with tool calling, and every milestone was
  verified end to end on one. On-console models are supported but optional (STT, an offline
  fallback, routing): realistic for small jobs, not for driving the agent.
- **Intent tools, not freeform render calls** — simple schemas that small models get right; the server
  translates them to A2UI (see [agent.md](agent.md)).
- **Native UI first**, and the model decides *content* while the renderer/runtime own *behaviour*
  (focus, back, dialogs, the keyboard).
- **Secrets** live in the environment, never in the repo, and never reach the client. A server
  listening beyond loopback requires an access code (`AGENT_TOKEN`).
- **License:** MIT, except the GPL-3.0 home-screen tile (it links a GPL boilerplate). See
  [`../THIRD-PARTY-NOTICES.md`](../THIRD-PARTY-NOTICES.md).

## Status

Done: the libraries on the Mac and on the console; the app with its own on-console agent (native
screens, DualSense, system keyboard, dialogs, notifications, HTTPS to the model, a home-screen tile);
the PS5 as a CopilotKit channel; background presence; on-console voice (whisper STT on the GPU). The
**1.0** is cut: a stable catalog (frozen at the `catalog-v1` tag), the CMake target and the two
npm packages, docs and license. What remains of the release is listing the renderer and the agent
runtime on the A2UI ecosystem page (see [`going-public.md`](going-public.md)). Next is **OpenDots
on the PS5** as the external acceptance test. See [`../BACKLOG.md`](../BACKLOG.md) for everything open.

## Platform constraints (worth knowing)

- **Firmware:** the public jailbreak (Relapse) covers 7.00–~13.60; 14.x is closed. The audience is
  people on older firmware. Tested on **12.70**.
- **Toolchain:** C/C++ via the PS5 payload SDK (clang/lld) + an SDL2 port. **No Node, Python, Rust or
  Deno on the console, and no TypeScript runtime** — the agent runs in embedded QuickJS there.
- **Memory:** 16 GiB shared CPU/GPU, ~11–12 GiB usable under jailbreak — realistically 7–12B models at
  Q4, one at a time.
