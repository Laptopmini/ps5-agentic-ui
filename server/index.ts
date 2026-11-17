// The demo agent as a local AG-UI server: the PS5 agent runtime (runtime/) with a simulated
// console for tools. POST /agent takes a RunAgentInput and streams AG-UI events as server-sent
// events. Every event is also appended to server/logs/<thread>.jsonl (what was asked and
// answered, on this computer only; delete the folder whenever you like).
//
//   LLM_API_KEY=... LLM_MODEL=... node server/index.ts [--port 8790] [--model id] [--host 127.0.0.1]
//
// The model is any OpenAI-compatible endpoint: OpenRouter by default, another with LLM_BASE_URL.
// Listening beyond this computer (--host 0.0.0.0, for the console) requires an access code in
// AGENT_TOKEN: every request must then send "authorization: Bearer <code>", so other devices on
// the network can't spend the model key. (Env, not argv, so it doesn't show in `ps`.)
import { appendFileSync, mkdirSync } from "node:fs";
import { actionOf } from "../runtime/a2ui.ts";
import { modelFromEnv } from "../runtime/model.ts";
import { serveAgent } from "../runtime/serve.ts";
import { AgentThread } from "../runtime/thread.ts";
import { consoleTools } from "./console-tools.ts";

const arg = (name: string, fallback: string) => {
  const i = process.argv.indexOf(`--${name}`);
  return i > 0 ? process.argv[i + 1] : fallback;
};
const port = Number(arg("port", "8790"));
const host = arg("host", "127.0.0.1");
const token = process.env.AGENT_TOKEN ?? "";
const logDir = new URL("./logs/", import.meta.url);
mkdirSync(logDir, { recursive: true });

function summary(e: Record<string, any>): string {
  switch (e.type) {
    case "TEXT_MESSAGE_CONTENT": return `text: ${String(e.delta).slice(0, 120)}`;
    case "TOOL_CALL_START": return `tool: ${e.toolCallName}`;
    case "TOOL_CALL_RESULT": return `result: ${String(e.content).slice(0, 120)}`;
    case "ACTIVITY_SNAPSHOT": return e.content?.functionCallId ? `function: ${e.content.callFunction?.["@call"]}` : `surface: ${e.content?.operations?.map((o: any) => Object.keys(o).find((k) => k !== "version")).join("+")}`;
    case "CUSTOM": return `status: ${JSON.stringify(e.value)}`;
    case "RUN_ERROR": return `ERROR: ${e.message}`;
    default: return e.type;
  }
}

try {
  const model = modelFromEnv(arg("model", "") ? { model: arg("model", "") } : {});
  await serveAgent({
    port,
    host,
    token,
    thread: (id) => new AgentThread(id, { model, tools: consoleTools() }),
    onRun: (input) => {
      const pressed = actionOf(input.forwardedProps);
      const what = pressed ? `action ${JSON.stringify(pressed.name)}` : `message ${JSON.stringify(input.messages?.at(-1)?.content ?? "").slice(0, 80)}`;
      console.error(`[${input.threadId.slice(0, 8)}] ← ${what}`);
    },
    onEvent: (threadId, event) => {
      appendFileSync(new URL(`./${threadId.replace(/[^\w-]/g, "_")}.jsonl`, logDir), `${JSON.stringify({ at: new Date().toISOString(), ...event })}\n`);
      console.error(`[${threadId.slice(0, 8)}] ${summary(event)}`);
    },
    onReject: (reason) => console.error(`rejected ${reason}`),
  });
  console.error(`PS5 agent (AG-UI) on http://${host}:${port}/agent, model ${model.model}${token ? ", access code required" : ""}`);
} catch (e) {
  console.error(String((e as Error).message ?? e));
  process.exit(1);
}
