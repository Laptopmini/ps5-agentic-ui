// Local AG-UI server for the PS5 agent: POST /agent takes a RunAgentInput and streams AG-UI events
// as server-sent events. Model: any OpenAI-compatible endpoint via LLM_API_KEY (+ LLM_MODEL);
// OpenRouter by default, override the endpoint with LLM_BASE_URL. Every event is logged to
// server/logs/<thread>.jsonl.
//
//   LLM_API_KEY=... node server/index.ts [--port 8790] [--model minimax/minimax-m3] [--host 127.0.0.1]
//
// Listening beyond this Mac (--host 0.0.0.0, for the console) requires an access code in
// AGENT_TOKEN: every request must then send "authorization: Bearer <code>", so other devices on
// the network can't spend the model key. (Env, not argv, so it doesn't show in `ps`.)
import { timingSafeEqual } from "node:crypto";
import { appendFileSync, mkdirSync } from "node:fs";
import { createServer } from "node:http";
import { AgentThread, type RunAgentInput } from "./thread.ts";
import { actionOf } from "../runtime/a2ui.ts";

const arg = (name: string, fallback: string) => {
  const i = process.argv.indexOf(`--${name}`);
  return i > 0 ? process.argv[i + 1] : fallback;
};
const port = Number(arg("port", "8790"));
const host = arg("host", "127.0.0.1");
const token = process.env.AGENT_TOKEN ?? "";
const loopback = host === "127.0.0.1" || host === "::1" || host === "localhost";
if (!loopback && token.length < 16) {
  console.error(`Refusing to listen on ${host} without an access code: set AGENT_TOKEN (16+ characters).`);
  process.exit(1);
}
function authorized(header: string | undefined): boolean {
  if (!token) return true;
  const given = Buffer.from(header?.startsWith("Bearer ") ? header.slice(7) : "");
  const want = Buffer.from(token);
  return given.length === want.length && timingSafeEqual(given, want);
}
const model = arg("model", process.env.LLM_MODEL ?? "minimax/minimax-m3");
const logDir = new URL("./logs/", import.meta.url);
mkdirSync(logDir, { recursive: true });

const threads = new Map<string, AgentThread>();

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

const server = createServer(async (req, res) => {
  if (!authorized(req.headers.authorization)) {
    console.error(`rejected ${req.method} ${req.url} from ${req.socket.remoteAddress}: missing or wrong access code`);
    res.writeHead(401).end();
    return;
  }
  if (req.method === "GET" && req.url === "/health") {
    res.writeHead(200, { "content-type": "application/json" }).end(JSON.stringify({ ok: true, model, threads: threads.size }));
    return;
  }
  if (req.method !== "POST" || req.url !== "/agent") {
    res.writeHead(404).end();
    return;
  }
  let raw = "";
  for await (const chunk of req) raw += chunk;
  let input: RunAgentInput;
  try {
    input = JSON.parse(raw);
    if (!input.threadId) throw new Error("threadId is required");
  } catch (e) {
    res.writeHead(400, { "content-type": "application/json" }).end(JSON.stringify({ error: String(e) }));
    return;
  }

  res.writeHead(200, { "content-type": "text/event-stream", "cache-control": "no-cache", connection: "keep-alive" });
  const log = new URL(`./${input.threadId.replace(/[^\w-]/g, "_")}.jsonl`, logDir);
  const emit = (event: Record<string, any>) => {
    res.write(`data: ${JSON.stringify(event)}\n\n`);
    appendFileSync(log, `${JSON.stringify({ at: new Date().toISOString(), ...event })}\n`);
    console.error(`[${input.threadId.slice(0, 8)}] ${summary(event)}`);
  };

  let thread = threads.get(input.threadId);
  if (!thread) {
    thread = new AgentThread(input.threadId, model);
    threads.set(input.threadId, thread);
  }
  if (thread.running) {
    emit({ type: "RUN_ERROR", message: "A run is already in progress on this thread." });
    res.end();
    return;
  }
  const pressed = actionOf(input.forwardedProps);
  const what = pressed ? `action ${JSON.stringify(pressed.name)}` : `message ${JSON.stringify(input.messages?.at(-1)?.content ?? "").slice(0, 80)}`;
  console.error(`[${input.threadId.slice(0, 8)}] ← ${what}`);
  await thread.run(input, emit);
  res.end();
});

server.listen(port, host, () =>
  console.error(`PS5 agent (AG-UI) on http://${host}:${port}/agent, model ${model}${token ? ", access code required" : ""}`),
);
