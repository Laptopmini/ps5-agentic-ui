# Contributing

Issues and pull requests are welcome, whether you're here for the PS5, for A2UI and AG-UI, or for CopilotKit.

## Getting set up

You don't need a PS5 for most changes. With CMake 3.24+, a C++20 compiler, SDL2, Node 24+ and pnpm:

```sh
pnpm install
pnpm typecheck && pnpm test          # the agent runtime and the channel adapter
pnpm build                           # both npm packages

cmake -S renderer -B renderer/build && cmake --build renderer/build -j 8
ctest --test-dir renderer/build --output-on-failure    # unit tests, and every fixture rendered headless
```

CI runs the same commands on macOS and Linux. The README's "Try it" section covers running the demo with and without a model key, and on a console.

## Where things are

| You want to change | Look in |
|---|---|
| How a component is drawn, focus, dialogs | `renderer/src/view.cpp`, tests in `renderer/tests/test_main.cpp` |
| What the agent's tools turn into | `runtime/intent-tools.ts` |
| The agent's run loop, approvals, secrets | `runtime/thread.ts`, tests in `runtime/thread.test.ts` |
| The demo agent's tools | `server/console-tools.ts`, `server/console-sim.ts` |
| The Channels adapter and its wire protocol | `channel/adapter.ts`, `channel/render.ts` |
| Console bindings (DualSense, keyboard, dialogs) | `payload/console/` |
| What the console can read about itself | `renderer/src/ps5_system.cpp` |

## Ground rules

- **The PS5 catalog is frozen for 1.0** (`catalog/ps5.catalog.json`, the `catalog-v1` tag). Changes to components or functions need a new catalog version; open an issue first.
- **Tests come with the change.** `runtime/catalog.test.ts` keeps the catalog, the intent tools and the renderer's fixtures in step; a scripted model (`runtime/thread.test.ts`) covers the run loop without a model key.
- **Changes that touch the console** can't be checked by CI. Say in the pull request what you ran on a console and on which firmware, or that you couldn't.
- **No provider-specific code paths.** The model is any OpenAI-compatible endpoint; provider switches go through `LLM_EXTRA_BODY`.
- Keep secrets out of the repository and out of logs. Model keys come from the environment.

## Reporting bugs

Include what you ran, what you expected and what happened. For the renderer, the JSONL of the surface helps (the agent server logs every event to `server/logs/`; check it for anything private before attaching). For the console app, add the firmware version and how the app was launched.

Security problems: please follow [`SECURITY.md`](SECURITY.md) instead of opening a public issue.
