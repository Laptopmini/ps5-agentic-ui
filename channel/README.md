# copilotkit-channels-ps5

A [CopilotKit Channels](https://docs.copilotkit.ai) `PlatformAdapter` that turns a **PlayStation 5** into an agent surface: the agent's generative UI is drawn as **native A2UI screens** (menus, shelves of tiles, info/stat dashboards, the system keyboard, system dialogs), driven with the **DualSense**, with **haptics**, the light bar and adaptive triggers as device feedback.

It's the adapter for the PS5 A2UI app from [ps5-agentic-ui](https://github.com/Laptopmini/ps5-agentic-ui). The app on the console is the client; this adapter is what an agent elsewhere on the network talks to through Channels. It depends only on `@copilotkit/channels-core` 0.12 (a peer), and runs on the Channels runner without CopilotKit Intelligence.

## Install

```sh
npm install copilotkit-channels-ps5 @copilotkit/channels-core
```

## Use

```ts
import { createChannel } from "@copilotkit/channels-core";
import { handlePs5Actions, ps5, ps5ConsoleTools } from "copilotkit-channels-ps5";

// The adapter listens for the console (127.0.0.1:8792 by default; any other host needs a token).
const adapter = ps5({ host: "0.0.0.0", token: process.env.PS5_CHANNEL_TOKEN, agentName: "My agent" });

const channel = createChannel({ name: "my-agent", adapters: [adapter], agent: (threadId) => makeAgent(threadId) });
channel.onMessage(({ thread }) => thread.runAgent());
handlePs5Actions(channel);                                   // presses on the agent's own A2UI screens
for (const tool of ps5ConsoleTools(adapter)) channel.tool(tool);   // the console's own data as tools
```

What reaches the console:

- **Channels UI** you post (`Message`, `Header`, `Section`, `Fields`, `Actions`, `Button`, `Select`, `Input`, `Carousel`, `Table`…) is rendered to the PS5 catalog: a screen of text, menus, shelves and images, with an `Input` becoming the system keyboard.
- **A2UI 1.0 surfaces** the agent produces itself pass straight through.
- **While the app is closed**, messages become system notifications (and `ask_choices` becomes a notification with buttons) through the app's background payload, and are delivered in full when the app opens.

On the console, pair the app with the channel from its setup screen (choose "A CopilotKit channel" and enter this adapter's address and token). The wire protocol between console and adapter is documented at the top of [`adapter.ts`](adapter.ts); background presence is in [`docs/channel.md`](../docs/channel.md).

Also exported: `Ps5Adapter`, `Ps5Options`, `Ps5Client`, `consoleKey`, `handlePs5Actions`, `ps5ConsoleTools`, `toSurface` (Channels UI → A2UI), `PS5_CATALOG_ID`, and the `RenderedSurface` type.

## Example

A runnable end-to-end example — a tiny Channels agent posting UI that the adapter turns into PS5 screens — is in [`example.ts`](example.ts):

```sh
LLM_API_KEY=... LLM_MODEL=... pnpm agent     # the demo agent, on :8790
pnpm channel                                 # channel/example.ts, on :8792
```

## Build

```sh
pnpm install                 # at the repository root (a pnpm workspace)
pnpm -C channel build        # esbuild bundle (dist/index.js) + tsc declarations (dist/**/*.d.ts)
pnpm test:channel
```

MIT.
