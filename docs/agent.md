# The agent, the catalog, and driving native UI

How an agent speaks "PS5 UI": the A2UI catalog, how it rides AG-UI, the small-model design point, and
what a model can actually drive. Background for the agent runtime (`runtime/`, npm
`ps5-a2ui-agent`), the demo agent built on it (`server/`), the evals (`eval/`) and the catalog
(`catalog/ps5.catalog.json`).

## A2UI 1.0 over AG-UI

The agent describes UI with **A2UI 1.0** (Google's agent-UI protocol): a flat list of components + a
data model + bindings, identified by a **catalog** that says which components a given renderer draws.
CopilotKit 1.85 carries A2UI 1.0 over AG-UI as activity events — `ACTIVITY_SNAPSHOT`
`generative-ui-surface` (a surface's operations) and `generative-ui-function-call`
(`callRendererFunction`, e.g. a controller rumble). A button press goes back as an A2UI 1.0 **action
message** in `forwardedProps.copilotkit.ui`, next to a visible user message. `runtime/a2ui.ts`
is the reference for this envelope; `vendor/a2ui-v1_0/` holds the upstream schemas.

## The PS5 catalog

`catalog/ps5.catalog.json` is a JSON-Schema document describing the native PS5 components — `Screen`,
`Menu`/`MenuItem`, `Shelf`/`Tile`, `Card`, `Text`, `Stats`, `Progress`, `StatusChip`, `Image`,
`ButtonHints`, `TextPrompt` (the system keyboard), `VoicePrompt`, `Confirm` (a system dialog),
`Notice` (a toast) — plus device **functions** (`haptic`, `rumble`, `lightbar`, `triggerEffect`).
It's the enforceable contract: `runtime/validate.ts` validates surfaces against it, and
`catalog.test.ts` keeps it in sync with the renderer and the intent tools (every rendered component
must be in the catalog). Its instructions tell the model the couch constraints: one focused item,
D-pad moves focus, Cross activates, Circle is back; prefer few, large, focusable items; text entry
through `TextPrompt`, confirmation through `Confirm`.

## Intent tools, not freeform render calls (the small-model design point)

CopilotKit's native pattern is a single `render_a2ui`-style tool where the model emits the component
tree itself. For the small/cheap models this project targets, that's too much structure to get right.
Instead the model calls **flat intent tools** — `show_menu`, `show_shelf`, `show_stats`, `show_info`,
`show_pages`, `show_progress`, `ask_text`, `ask_voice`, `confirm`, `notify`, `controller_feedback` —
with simple schemas (type/enum/required/items only). The runtime (`runtime/intent-tools.ts`) translates each
call into A2UI (`toA2UI`) or into catalog device calls (`toFunctionCalls`). A screen-drawing intent
ends the run; a device intent lets the run continue. This keeps the model's job small and the output
valid by construction. The system prompt (`runtime/prompts.ts`) steers it: answer only what was
asked, render structured data with tools (never markdown tables), and offer a couple of likely
follow-ups.

## The runtime

`AgentThread` (`runtime/thread.ts`) is one conversation: it keeps the model history, password
secrets (as `secret://` handles, never shown to the model) and any pending approval on its own
side, so clients send only what's new. It takes a model (any OpenAI-compatible endpoint, or your
own function) and the agent's tools; a tool can ask for approval, which the runtime shows as its
own confirm dialog. `serveAgent` (`runtime/serve.ts`) puts a thread behind HTTP and SSE.

The demo agent (`server/`) is that runtime plus a console as tools. It runs as a Node server
(`server/index.ts`) or **embedded in QuickJS on the console** (`server/embedded.ts` → `agent.js`)
with no HTTP: the same code, so the console can run its own agent. On the console its tools
(`list_games`, system info, saves, screenshots, recently played) read the real console through an
injected `__ps5System`; elsewhere a simulated console stands in, so the agent can be tried
anywhere.

## What a model can drive

- **Any OpenAI-compatible endpoint** works: `LLM_API_KEY` and `LLM_MODEL`, with **OpenRouter** as
  the default endpoint and `LLM_BASE_URL` for any other (a hosted provider, LM Studio or Ollama on
  the network, ProsperoAI on the PS5 itself). `LLM_EXTRA_BODY` adds provider-specific request
  fields. The model needs tool calling; nothing else is assumed.
- **Intent tools are what make small models usable.** On the catalog eval (`eval/catalog-eval`, 16
  scenarios, one run, a small and inexpensive hosted model), the model produced a correct screen in
  **15 of 16** scenarios with intent tools, and in **1 of 16** when given the catalog and asked to
  write the A2UI components itself, the way a single `render_a2ui` tool works. The failures were
  schema errors (wrong action shapes, nested instead of flat children), not provider damage.
  Reproduce with `LLM_API_KEY=… node eval/catalog-eval/run.ts --models <id> --modes intent,a2ui`.
- **Provider damage to tool arguments happens.** Some providers return arrays wrapped as
  `{"item":[…]}` or `""` for `[]` inside untyped objects. The runtime sidesteps this by typing
  every nested array in the intent tools; the eval harness, which also exercises full A2UI, can
  repair it with `repairToolArgs` (`runtime/provider-quirks.ts`) to tell the two kinds of failure
  apart.
- **Small local models (7–9B via ProsperoAI on the PS5)** are realistic for narrow jobs (short
  classification, an offline fallback, speech-to-text) but not for driving the full agent at
  useful latency and context. A supported option, not a dependency.

## Native UI first

A decided principle: when a screen, choice, text entry or confirmation helps, the agent renders
**native** UI (system dialogs, the system keyboard, notifications, controller hints) rather than
describing it in text or drawing its own. The agent's screens appear **inline** as its replies in the
conversation; earlier screens collapse to chips you can reopen.
