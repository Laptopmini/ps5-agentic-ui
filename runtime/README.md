# ps5-a2ui-agent

An [AG-UI](https://docs.ag-ui.com) agent runtime that answers with **native PlayStation 5 screens**: [A2UI 1.0](https://a2ui.org) surfaces on the PS5 catalog, plus controller feedback (haptics, light bar, adaptive triggers). You bring a model and your own tools; the runtime brings the screens, the run loop and the safety rails.

It speaks A2UI **1.0** over AG-UI the way CopilotKit 1.85 carries it, and works with any model behind an OpenAI-compatible endpoint. It is the agent side of [ps5-agentic-ui](https://github.com/Laptopmini/ps5-agentic-ui); the renderer there draws what it sends, on a PS5 or on a desktop.

## Install

```sh
npm install ps5-a2ui-agent
```

Node 20 or later. ESM only.

## Use

```ts
import { AgentThread, modelFromEnv, serveAgent, type AgentTool } from "ps5-a2ui-agent";

const tools: AgentTool[] = [
  {
    name: "list_saves",
    description: "List saved game data: id, game, size.",
    run: async () => ({ saves: await readSaves() }),
  },
  {
    name: "delete_save",
    description: "Permanently delete saved game data by id. The console itself asks the user to approve this call.",
    parameters: { type: "object", properties: { id: { type: "string" } }, required: ["id"] },
    // The runtime shows its own confirm dialog before `run`. The model can't skip or reword it.
    approval: (args) => ({ message: `Delete save ${args.id}? This can't be undone.`, confirmLabel: "Delete", cancelLabel: "Keep", destructive: true }),
    run: async (args) => ({ deleted: await deleteSave(args.id) }),
  },
];

// LLM_API_KEY and LLM_MODEL from the environment; OpenRouter unless LLM_BASE_URL says otherwise.
const model = modelFromEnv();

// POST /agent takes an AG-UI RunAgentInput and streams AG-UI events (server-sent events).
await serveAgent({ port: 8790, thread: (id) => new AgentThread(id, { model, tools }) });
```

Try it against the desktop renderer: `a2ui-host --agent http://127.0.0.1:8790/agent` (see the repository's README).

Without HTTP, call the thread yourself. This is how the agent runs inside QuickJS on the console:

```ts
const thread = new AgentThread("thread-1", { model, tools });
await thread.run({ threadId: "thread-1", messages: [{ id: "m1", role: "user", content: "Show my saves" }] }, (event) => send(event));
```

## What the runtime does

The model never writes UI trees. It calls flat **intent tools** with simple schemas, and the runtime translates each call into a valid A2UI surface:

| Intent tool | Becomes |
|---|---|
| `show_menu`, `show_shelf`, `show_pages` | a list to pick from, shelves of tiles, tabbed sections |
| `show_info`, `show_stats`, `show_progress` | a few lines, a dashboard of value tiles, a progress bar |
| `ask_text`, `ask_voice` | the system keyboard (text, password, URL…), voice input |
| `confirm`, `notify` | a system dialog, a notification |
| `controller_feedback` | haptics, light bar and adaptive-trigger calls the client runs |

Around that:

- **Argument checks.** A bad intent call goes back to the model as a tool error it can fix within the same run.
- **Approval gate.** A tool with `approval` pauses the run on a confirm dialog the runtime writes itself. Approved, the tool runs; denied, or if the user does something else, the model is told so.
- **Secret handles.** Text typed into a password keyboard never reaches the model. It gets a `secret://` handle, passes it to a tool, and the tool resolves it with `ctx.secret(value)`.
- **Client tools.** Tools in `RunAgentInput.tools` are run by the client, the AG-UI way: the run ends on the call and the next run brings the result.
- **Server-side threads.** History, secrets and pending approvals stay with the thread; clients send only what's new.
- **Streaming and retries.** Text streams as the model writes it (without `<think>` blocks). Rate limits and transient errors are retried, but only while nothing has been shown.

## The model

```ts
modelFromEnv()                                   // LLM_API_KEY, LLM_MODEL, LLM_BASE_URL, LLM_EXTRA_BODY
modelFromEnv({ model: "another-model" })         // overrides win
{ baseUrl: "http://localhost:1234/v1", apiKey: "x", model: "local-model" }   // or spell it out
```

`AgentOptions.model` also takes your own function of type `Complete` (OpenAI-style messages and tools in, a chat-completion body out), which is how the tests script a model and how you'd plug in another SDK.

## Events

Per run: `RUN_STARTED`, then `TEXT_MESSAGE_START/CONTENT/END`, `TOOL_CALL_START/ARGS/END/RESULT`, `ACTIVITY_SNAPSHOT` (`generative-ui-surface`: a surface's A2UI 1.0 operations; `generative-ui-function-call`: a catalog function for the client to run), `CUSTOM "status"` (thinking, working, retrying, waiting for approval) and `CUSTOM "ps5.suggestions"` (follow-up prompts), then `RUN_FINISHED` or `RUN_ERROR`.

A button press comes back as an A2UI 1.0 action message in `forwardedProps.copilotkit.ui`, next to a visible user message.

## Also exported

- `intentTools`, `deviceTools`, `toA2UI`, `toFunctionCalls`, `intentProblem`: the intent tools and their translation, to use in your own loop.
- `validateSurface`, `ps5Catalog`: validate any A2UI 1.0 surface against the PS5 catalog (JSON Schema plus the structural rules: one root, unique ids, no dangling references or cycles).
- `createSurface`, `surfaceSnapshot`, `functionCallEvent`, `actionOf`, `uiEnvelope`, `PS5_CATALOG_ID`: the A2UI-over-AG-UI envelope.
- `PS5_PROMPT` and its parts: the default system prompt. Pass `system` to replace it.

## Security

`serveAgent` listens on `127.0.0.1` by default. Listening on any other address requires a `token` of 16 or more characters, which clients send as `authorization: Bearer …`, so other devices on the network can't spend your model key.

MIT.
