# copilotkit-ps5

A [CopilotKit Channels](https://docs.copilotkit.ai) `PlatformAdapter` that turns a jailbroken **PlayStation 5** into an agent surface: the agent's generative UI is drawn as **native A2UI screens** (menus, shelves of tiles, info/stat dashboards, the system keyboard, system dialogs), driven with the **DualSense**, with **haptics**, the light bar and adaptive triggers as device feedback.

It's one adapter for the PS5 A2UI app (`ps5-agentic-ui`). The console runs a small client; this adapter is what an agent elsewhere on the network talks to through Channels. It depends only on `@copilotkit/channels-core` (a peer), and runs on the Channels runner without CopilotKit Intelligence.

## Install

```sh
npm install copilotkit-ps5 @copilotkit/channels-core
```

## Use

```ts
import { ps5 } from "copilotkit-ps5";

// A PlatformAdapter you register with your Channels runtime. Posted Channels UI
// (Message, Section, Fields, Actions, Carousel, Table…) becomes PS5-catalog A2UI;
// controller presses come back as interactions.
const adapter = ps5({ agentName: "PS5 A2UI" });
```

Also exported: `Ps5Adapter`, `Ps5Options`, `Ps5Client`, `consoleKey`, `handlePs5Actions`, `ps5ConsoleTools`, `toSurface` (Channels UI → A2UI), `PS5_CATALOG_ID`, and the `RenderedSurface` type.

## Example

A runnable end-to-end example — a tiny Channels agent posting UI that the adapter turns into PS5 screens — is in [`example.ts`](example.ts):

```sh
pnpm channel      # runs channel/example.ts
```

## Build

```sh
pnpm install
pnpm build        # esbuild bundle (dist/index.js) + tsc declarations (dist/**/*.d.ts)
pnpm test
```

MIT.
