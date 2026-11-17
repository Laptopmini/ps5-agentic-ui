// The agent over HTTP: POST /agent takes an AG-UI RunAgentInput and streams AG-UI events back as
// server-sent events; GET /health answers {ok, threads}. One AgentThread per threadId, made by
// the `thread` you pass, kept in memory (the least recently used are dropped past `maxThreads`).
//
// Listening beyond loopback requires an access code (`token`, 16+ characters): every request
// must then send "authorization: Bearer <code>", so other devices on the network can't spend the
// model key.
import { timingSafeEqual } from "node:crypto";
import { createServer, type Server } from "node:http";
import type { AgentThread, RunAgentInput } from "./thread.ts";

type Json = Record<string, any>;

export interface ServeOptions {
  /** Makes the thread for a new conversation. */
  thread: (threadId: string) => AgentThread;
  port?: number;
  /** 127.0.0.1 by default; anything else needs a token. */
  host?: string;
  /** The access code clients send as "authorization: Bearer …". */
  token?: string;
  /** Conversations kept in memory (200 by default). */
  maxThreads?: number;
  /** The largest request body accepted, in bytes (4 MB by default). */
  maxBodyBytes?: number;
  /** Called with the input of every run as it starts. */
  onRun?: (input: RunAgentInput) => void;
  /** Called with every event sent, e.g. to log it. */
  onEvent?: (threadId: string, event: Json) => void;
  /** Called with requests turned away (wrong access code, bad input). */
  onReject?: (reason: string) => void;
}

const LOOPBACK = ["127.0.0.1", "::1", "localhost"];

/** Start the server; resolves once it's listening. Close it with `server.close()`. */
export function serveAgent(options: ServeOptions): Promise<Server> {
  const host = options.host ?? "127.0.0.1";
  const port = options.port ?? 8790;
  const token = options.token ?? "";
  const maxThreads = options.maxThreads ?? 200;
  const maxBody = options.maxBodyBytes ?? 4 * 1024 * 1024;
  if (!LOOPBACK.includes(host) && token.length < 16) throw new Error(`Refusing to listen on ${host} without an access code (token, 16+ characters).`);

  const authorized = (header: string | undefined) => {
    if (!token) return true;
    const given = Buffer.from(header?.startsWith("Bearer ") ? header.slice(7) : "");
    const want = Buffer.from(token);
    return given.length === want.length && timingSafeEqual(given, want);
  };
  // Insertion order is recency: a thread is re-inserted each time it's used.
  const threads = new Map<string, AgentThread>();

  const server = createServer(async (req, res) => {
    if (!authorized(req.headers.authorization)) {
      options.onReject?.(`${req.method} ${req.url} from ${req.socket.remoteAddress}: missing or wrong access code`);
      res.writeHead(401).end();
      return;
    }
    if (req.method === "GET" && req.url === "/health") {
      res.writeHead(200, { "content-type": "application/json" }).end(JSON.stringify({ ok: true, threads: threads.size }));
      return;
    }
    if (req.method !== "POST" || req.url !== "/agent") {
      res.writeHead(404).end();
      return;
    }
    let raw = "";
    for await (const chunk of req) {
      raw += chunk;
      if (raw.length > maxBody) {
        options.onReject?.(`POST /agent: body over ${maxBody} bytes`);
        res.writeHead(413).end();
        req.destroy();
        return;
      }
    }
    let input: RunAgentInput;
    try {
      input = JSON.parse(raw);
      if (!input || typeof input.threadId !== "string" || !input.threadId) throw new Error("threadId is required");
    } catch (e) {
      res.writeHead(400, { "content-type": "application/json" }).end(JSON.stringify({ error: String((e as Error).message ?? e) }));
      return;
    }

    res.writeHead(200, { "content-type": "text/event-stream", "cache-control": "no-cache", connection: "keep-alive" });
    const emit = (event: Json) => {
      res.write(`data: ${JSON.stringify(event)}\n\n`);
      options.onEvent?.(input.threadId, event);
    };
    let thread = threads.get(input.threadId);
    if (thread?.running) {
      emit({ type: "RUN_ERROR", message: "A run is already in progress on this thread." });
      res.end();
      return;
    }
    try {
      thread ??= options.thread(input.threadId);
    } catch (e) {
      emit({ type: "RUN_ERROR", message: String((e as Error).message ?? e) });
      res.end();
      return;
    }
    threads.delete(input.threadId);
    threads.set(input.threadId, thread);
    for (const [id, t] of threads) {
      if (threads.size <= maxThreads) break;
      if (!t.running) threads.delete(id);
    }
    options.onRun?.(input);
    await thread.run(input, emit);
    res.end();
  });

  return new Promise((resolve, reject) => {
    server.once("error", reject);
    server.listen(port, host, () => resolve(server));
  });
}
