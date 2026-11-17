# The PS5 as a CopilotKit Channels channel

Instead of (or as well as) running its own agent, the console can be a **surface for an agent
elsewhere** on the network, through a CopilotKit Channels `PlatformAdapter`
(`channel/`, npm `copilotkit-channels-ps5`). Verified on the console.

## How it works

- The console app connects to the adapter; the **client declares what it can do** on `/hello`: its
  catalog, the device functions it runs, the haptic effects it supports, and whether a controller is
  connected. So an agent only offers what this client can actually play.
- Whatever the channel posts as **Channels UI** (`Message`, `Header`, `Section`, `Fields`, `Actions`,
  `Button`, `Select`, `Input`, `Carousel`, `Table`, …) is rendered to **PS5-catalog A2UI** by
  `channel/render.ts` — a `Screen` of `Text`/`Menu`/`Shelf`/`Image`, with an `Input` becoming a
  `TextPrompt` (the system keyboard). Each control's action is carried back to the adapter as an
  interaction when the player presses it.
- The agent's own generative-UI surfaces (if it produces A2UI directly) pass straight through.

## Intelligence is optional

The channel runs on CopilotKit's **own runner** without CopilotKit Intelligence — that's the default,
and a deliberate requirement (the goal is open tooling that doesn't depend on a hosted product). With
an Intelligence key, the example starts through `CopilotRuntime` (the documented path) and records
conversations; without one it calls the agent directly. The one internal seam the adapter relies on
is the start hook, `channel.ɵruntime.start()`.

Run the example end to end with `pnpm channel` (`channel/example.ts`), or drive a console against it
with `scripts/ps5-channel.sh`.

## Background presence (reaching the player during a game)

The PS5 has no true multitasking — opening the app switches away from a running game — so to reach the
player while they play, a **small resident payload** (`payload/presence/`) keeps the channel
connection open with the app closed. When the agent has something to say it shows a **system
notification with an action** (and multi-option `ask_choices`); opening it brings up the app at that
message (`psgm:play?id=…` deep link, which closes the running game via
`sceSystemServiceGetAppIdOfRunningBigApp`). Queued messages are delivered when the app opens. The
switch-away itself is a PS5 system limitation, out of scope; "background presence works" is the
milestone, and it does. `scripts/ps5-channel.sh` with `PRESENCE=1` sends the resident payload and
keeps the channel up after the app closes.
