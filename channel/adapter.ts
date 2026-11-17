// The PS5 as a CopilotKit Channels surface: a PlatformAdapter whose "provider" is PS5 A2UI on
// the console. The console connects to this process over the LAN:
//
//   GET  /events?console=<id>     server-sent events: AG-UI events for the console's Session
//   GET  /events?console=<id>&role=presence
//                                 the console's background payload (payload/presence): while the
//                                 app isn't connected, what the agent says is queued for it, and
//                                 this stream gets CUSTOM "ps5.notify" {title, text} to show a toast
//   POST /hello  {console, client} what this console can do (catalog, functions, haptics, controller)
//   POST /turn   {console, text}   the player sent a message
//   POST /action {console, action} the player pressed something on a screen (an A2UI 1.0 action
//                                 message: {version: "1.0", action: {name, surfaceId, …}})
//   POST /reset  {console}         the player started a new conversation: the agent's history for
//                                 this console is forgotten
//   POST /reply  {console, id, result}  the console's answer to a read the channel asked for
//                                 (CUSTOM "ps5.request" {id, name, args} on /events)
//
// Down the wire it's the same AG-UI the console reads from an agent directly: the agent's own
// events during a run (its A2UI 1.0 screens, text, status, controller feedback) pass through as
// they are, and what the channel posts itself (Channels UI) becomes PS5-catalog A2UI (render.ts),
// sent the way CopilotKit 1.85 sends surfaces (runtime/a2ui.ts).
//
// Presses come back two ways. On a channel control (event "channel.action") they become the
// Channels InteractionEvent the engine bound. On the agent's own screens they become the
// interaction PS5_A2UI_ACTION, and the next run carries the press in forwardedProps.copilotkit.ui
// (copilotkit_action), as CopilotKit 1.85 does; handlePs5Actions(channel) wires that up.
import { createHash, randomUUID, timingSafeEqual } from "node:crypto";
import { createServer, type IncomingMessage, type Server, type ServerResponse } from "node:http";
import type {
  AgentSession,
  ChannelNode,
  ConversationStore,
  IngressSink,
  InteractionEvent,
  PlatformAdapter,
  RunRenderer,
  SurfaceCapabilities,
} from "@copilotkit/channels-core";
import { ASK_CHOICES, CONSOLE_READS, FOLLOW_UPS, NOT_YET } from "./console-reads.ts";
import { CHANNEL_ACTION, toSurface } from "./render.ts";
import { A2UI_VERSION, createSurface, PS5_CATALOG_ID, removedSurface, SURFACE_ACTIVITY, surfaceSnapshot, uiEnvelope } from "../runtime/a2ui.ts";

type Json = Record<string, any>;

/** The interaction id for a press on one of the agent's own A2UI screens. */
export const PS5_A2UI_ACTION = "ps5.a2ui";
/** The PS5 catalog (same id the agent's screens use). */
export { PS5_CATALOG_ID };

/** What a console said it can do (POST /hello). Sent to the agent as forwardedProps.ps5Client. */
export interface Ps5Client {
  name?: string;
  catalogId?: string;
  /** Catalog functions it runs (haptic, hapticPattern, rumble, lightbar, triggerEffect). */
  functions?: string[];
  /** Named haptic effects it can play. */
  haptics?: string[];
  /** A controller is connected (feedback can be felt). */
  controller?: boolean;
  /** What the console can read about itself on request ("installed_titles"): see ps5ConsoleTools. */
  reads?: string[];
}

export interface Ps5Options {
  port?: number;
  /** 127.0.0.1 by default; anything else needs a token. */
  host?: string;
  /** Access code the console sends as "authorization: Bearer …". 16+ characters off loopback. */
  token?: string;
  /** Who speaks through this channel, shown in the console's header ("OpenDots"). Unset: the app's name. */
  agentName?: string;
  log?: (line: string) => void;
}

interface Target {
  conversationKey: string;
}

/**
 * One conversation per console, keyed by a stable UUID made from its id (name-based, SHA-1, the
 * RFC 4122 v5 layout). The engine uses the key as each turn's thread id, and CopilotKit
 * Intelligence only accepts UUID thread ids.
 */
export const consoleKey = (id: string) => {
  const h = createHash("sha1").update(`ps5-agentic-ui:console:${id}`).digest("hex");
  const variant = ((parseInt(h[16], 16) & 0x3) | 0x8).toString(16);
  return `${h.slice(0, 8)}-${h.slice(8, 12)}-5${h.slice(13, 16)}-${variant}${h.slice(17, 20)}-${h.slice(20, 32)}`;
};

export class Ps5Adapter implements PlatformAdapter {
  readonly platform = "ps5";
  readonly capabilities: SurfaceCapabilities = {
    supportsModals: false,
    supportsTyping: false,
    supportsReactions: false,
    supportsStreaming: true,
    // Every message from the console is addressed to the channel (it's a one-to-one surface).
    supportsMessageEvents: true,
  };
  readonly ackDeadlineMs = 3000;
  readonly conversationStore: ConversationStore;

  private readonly options: Required<Omit<Ps5Options, "token" | "agentName">> & { token: string; agentName: string };
  private server?: Server;
  private sink?: IngressSink;
  private readonly streams = new Map<string, Set<ServerResponse>>();
  private readonly presence = new Map<string, Set<ServerResponse>>();
  /** What the agent said while the app wasn't connected, sent when it connects (newest kQueue events). */
  private readonly queued = new Map<string, Json[]>();
  private readonly queuedText = new Map<string, Map<string, string>>();
  private readonly notifyTimers = new Map<string, { timer: NodeJS.Timeout; text: string }>();
  /** Notices nobody got (neither the app nor the background payload was connected): count, latest text. */
  private readonly missed = new Map<string, { count: number; text: string }>();
  private readonly clients = new Map<string, Ps5Client>();
  private readonly pendingAction = new Map<string, Json>();
  private readonly history = new Map<string, unknown[]>();
  private keepAlive?: NodeJS.Timeout;
  private readonly requests = new Map<string, { resolve: (result: Json) => void; timer: NodeJS.Timeout }>();

  constructor(options: Ps5Options = {}) {
    this.options = {
      port: options.port ?? 8792,
      host: options.host ?? "127.0.0.1",
      token: options.token ?? "",
      agentName: options.agentName ?? "",
      log: options.log ?? ((line) => console.log(`[ps5] ${line}`)),
    };
    const loopback = ["127.0.0.1", "::1", "localhost"].includes(this.options.host);
    if (!loopback && this.options.token.length < 16)
      throw new Error(`Refusing to listen on ${this.options.host} without an access code (token, 16+ characters).`);

    // Each turn gets its own agent (the engine isolates turns), seeded with the conversation so
    // far; when the turn ends its messages become the history. Each run also carries what the
    // console can do and, after a press on one of the agent's screens, the press itself.
    this.conversationStore = {
      seedsInboundTurn: false,
      getOrCreate: async (conversationKey, _target, makeAgent) => {
        const agent = makeAgent(conversationKey);
        agent.messages = structuredClone(this.history.get(conversationKey) ?? []) as typeof agent.messages;
        const run = agent.runAgent.bind(agent);
        agent.runAgent = ((params?: Json, subscriber?: unknown) => {
          const extra: Json = {};
          const client = this.clients.get(conversationKey);
          if (client) extra.ps5Client = client;
          const action = this.pendingAction.get(conversationKey);
          if (action) {
            extra.copilotkit = { ...params?.forwardedProps?.copilotkit, ui: uiEnvelope(action) };
            this.pendingAction.delete(conversationKey);
          }
          return run({ ...params, forwardedProps: { ...params?.forwardedProps, ...extra } }, subscriber as never);
        }) as typeof agent.runAgent;
        return {
          agent,
          release: () => {
            // Bounded: the newest messages only.
            this.history.set(conversationKey, agent.messages.slice(-200));
          },
        } satisfies AgentSession;
      },
    };
  }

  // ---------- lifecycle ----------

  async start(sink: IngressSink): Promise<void> {
    this.sink = sink;
    this.server = createServer((req, res) => {
      this.handle(req, res).catch((e) => {
        this.options.log(`request failed: ${(e as Error).message}`);
        if (!res.headersSent) res.writeHead(500).end();
      });
    });
    await new Promise<void>((resolve, reject) => {
      this.server!.once("error", reject);
      this.server!.listen(this.options.port, this.options.host, resolve);
    });
    // Comments keep idle streams open through proxies and let a dead console be noticed.
    this.keepAlive = setInterval(() => {
      for (const map of [this.streams, this.presence]) for (const set of map.values()) for (const res of set) res.write(": keep-alive\n\n");
    }, 15000);
    this.options.log(`listening on http://${this.options.host}:${this.options.port}`);
  }

  async stop(): Promise<void> {
    clearInterval(this.keepAlive);
    for (const { resolve, timer } of this.requests.values()) {
      clearTimeout(timer);
      resolve({ error: "channel_stopping" });
    }
    this.requests.clear();
    for (const { timer } of this.notifyTimers.values()) clearTimeout(timer);
    // The background payload stops instead of waiting for a channel that's gone.
    for (const set of this.presence.values()) for (const res of set) res.write(`data: ${JSON.stringify({ type: "CUSTOM", name: "ps5.presence", value: { stop: true } })}\n\n`);
    for (const map of [this.streams, this.presence]) {
      for (const set of map.values()) for (const res of set) res.end();
      map.clear();
    }
    await new Promise<void>((resolve) => (this.server ? this.server.close(() => resolve()) : resolve()));
  }

  // ---------- egress: what the channel shows ----------

  render(ir: ChannelNode[]) {
    return toSurface(ir);
  }

  async post(target: unknown, ir: ChannelNode[]) {
    const { conversationKey } = target as Target;
    const built = toSurface(ir);
    // Only text: a chat bubble, not a screen.
    if (built.plain) {
      const messageId = randomUUID();
      this.sendText(conversationKey, messageId, built.text);
      return { id: messageId, conversationKey };
    }
    const surfaceId = `channel-${randomUUID()}`;
    this.sendSurface(conversationKey, surfaceId, built, true);
    return { id: surfaceId, conversationKey };
  }

  async update(ref: { id: string; [k: string]: unknown }, ir: ChannelNode[]) {
    const conversationKey = String(ref.conversationKey ?? "");
    if (ref.id.startsWith("channel-")) this.sendSurface(conversationKey, ref.id, toSurface(ir), false);
    else this.sendText(conversationKey, ref.id, toSurface(ir).text);
  }

  async delete(ref: { id: string; [k: string]: unknown }) {
    if (!ref.id.startsWith("channel-")) return;  // a chat bubble stays in the conversation
    this.send(String(ref.conversationKey ?? ""), removedSurface(ref.id));
  }

  async stream(target: unknown, chunks: AsyncIterable<string>) {
    const { conversationKey } = target as Target;
    const messageId = randomUUID();
    this.send(conversationKey, { type: "TEXT_MESSAGE_START", messageId, role: "assistant" });
    for await (const delta of chunks) this.send(conversationKey, { type: "TEXT_MESSAGE_CONTENT", messageId, delta });
    this.send(conversationKey, { type: "TEXT_MESSAGE_END", messageId });
    return { id: messageId, conversationKey };
  }

  /** A run's events go to the console as they are; tool calls and interrupts are noted for the run loop. */
  createRunRenderer(target: unknown): RunRenderer {
    const { conversationKey } = target as Target;
    const toolCalls: { toolCallId: string; toolCallName: string; toolCallArgs: Record<string, unknown> }[] = [];
    let interrupt: { eventName: string; value: unknown } | undefined;
    return {
      subscriber: {
        onEvent: ({ event }: { event: Json }) => {
          this.send(conversationKey, event);
        },
        onToolCallEndEvent: ({ event, toolCallName, toolCallArgs }: Json) => {
          toolCalls.push({ toolCallId: event.toolCallId, toolCallName, toolCallArgs: toolCallArgs ?? {} });
        },
        onCustomEvent: ({ event }: Json) => {
          if (event.name === "on_interrupt") interrupt = { eventName: event.name, value: event.value };
        },
      } as never,
      markInterrupted: async () => {},
      getCapturedToolCalls: () => toolCalls,
      getPendingInterrupt: () => interrupt,
      clearPendingInterrupt: () => {
        interrupt = undefined;
      },
    };
  }

  decodeInteraction(raw: unknown): InteractionEvent | undefined {
    return raw && typeof raw === "object" && "id" in raw ? (raw as InteractionEvent) : undefined;
  }

  async lookupUser() {
    return undefined;
  }

  /** What a console said it can do, if it has said hello. */
  client(conversationKey: string): Ps5Client | undefined {
    return this.clients.get(conversationKey);
  }

  /**
   * Ask the console to read something about itself ("installed_titles"); resolves with its JSON
   * answer, or {error} if it isn't connected, can't, or doesn't answer within `timeoutMs`.
   */
  async consoleRead(conversationKey: string, name: string, args: Json = {}, timeoutMs = 10000): Promise<Json> {
    const open = this.streams.get(conversationKey);
    if (!open?.size) return { error: "console_not_connected" };
    if (!this.clients.get(conversationKey)?.reads?.includes(name)) return { error: "not_supported_by_console", read: name };
    const id = randomUUID();
    return new Promise<Json>((resolve) => {
      const timer = setTimeout(() => {
        this.requests.delete(id);
        resolve({ error: "console_timeout", read: name });
      }, timeoutMs);
      this.requests.set(id, { resolve, timer });
      const line = `data: ${JSON.stringify({ type: "CUSTOM", name: "ps5.request", value: { id, name, args } })}\n\n`;
      for (const res of open) res.write(line);
    });
  }

  // ---------- the wire ----------

  /** Show follow-up suggestions on the console (ps5ConsoleTools' suggest_follow_ups). */
  suggestFollowUps(conversationKey: string, prompts: string[]) {
    // Only to an open app: they're stale by the time a closed one opens.
    if (this.streams.get(conversationKey)?.size) this.send(conversationKey, FOLLOW_UPS.event(prompts));
  }

  /**
   * Ask the player to pick one of a few options (ps5ConsoleTools' ask_choices). App open: an
   * in-app menu; app closed: a notification whose buttons are the options (the background payload
   * posts the chosen value back as a turn). Returns which way it went.
   */
  askChoices(conversationKey: string, prompt: string, options: { label: string; value: string }[]): "menu" | "notified" | "queued" {
    if (this.streams.get(conversationKey)?.size) {
      const items = options.map((o, i) => ({
        id: `choice-${i}`,
        component: "MenuItem",
        label: o.label,
        action: { event: { name: "choice_select", userMessage: o.value, context: { value: o.value } } },
      }));
      const components: Json[] = [
        { id: "root", component: "Screen", title: prompt, children: ["menu"] },
        { id: "menu", component: "Menu", children: items.map((it) => it.id) },
        ...items,
      ];
      this.sendSurface(conversationKey, `choices-${randomUUID().slice(0, 8)}`, { components, data: {} }, true);
      return "menu";
    }
    const listeners = this.presence.get(conversationKey);
    const title = this.options.agentName || "PS5 A2UI";
    if (listeners?.size) {
      const line = `data: ${JSON.stringify(ASK_CHOICES.event(prompt, options, title))}

`;
      for (const res of listeners) res.write(line);
      return "notified";
    }
    return "queued";
  }

  private send(conversationKey: string, event: Json) {
    const open = this.streams.get(conversationKey);
    if (open?.size) {
      const line = `data: ${JSON.stringify(event)}\n\n`;
      for (const res of open) res.write(line);
      return;
    }
    // The app isn't open: keep the event for when it connects, and tell the background payload.
    const queue = this.queued.get(conversationKey) ?? [];
    queue.push(event);
    if (queue.length > kQueue) queue.splice(0, queue.length - kQueue);
    this.queued.set(conversationKey, queue);
    this.watchForNotice(conversationKey, event);
  }

  /** A finished text message or a new screen, while the app is away, becomes a toast (coalesced). */
  private watchForNotice(conversationKey: string, event: Json) {
    const texts = this.queuedText.get(conversationKey) ?? new Map<string, string>();
    this.queuedText.set(conversationKey, texts);
    let notice: string | undefined;
    if (event.type === "TEXT_MESSAGE_START") texts.set(event.messageId, "");
    else if (event.type === "TEXT_MESSAGE_CONTENT") texts.set(event.messageId, (texts.get(event.messageId) ?? "") + (event.delta ?? ""));
    else if (event.type === "TEXT_MESSAGE_END") {
      notice = (texts.get(event.messageId) ?? "").trim();
      texts.delete(event.messageId);
    } else if (event.type === "ACTIVITY_SNAPSHOT" && event.activityType === SURFACE_ACTIVITY && !event.content?.operations?.some((op: Json) => op.deleteSurface)) notice = "";
    if (notice === undefined) return;
    // Several in a row (a run's text, then its screen): one toast, a moment after the last, with the latest text.
    const pending = this.notifyTimers.get(conversationKey);
    if (pending) clearTimeout(pending.timer);
    const text = notice || pending?.text || "";
    const timer = setTimeout(() => {
      this.notifyTimers.delete(conversationKey);
      this.notify(conversationKey, text || "Sent you a screen.");
    }, 1500);
    this.notifyTimers.set(conversationKey, { timer, text });
  }

  private notify(conversationKey: string, text: string) {
    const listeners = this.presence.get(conversationKey);
    this.options.log(`app not connected: ${listeners?.size ? "notified the console" : "queued (no background payload)"}`);
    if (!listeners?.size) {
      // Told when the background payload connects.
      const m = this.missed.get(conversationKey);
      this.missed.set(conversationKey, { count: (m?.count ?? 0) + 1, text });
      return;
    }
    for (const res of listeners) this.writeNotice(res, text);
  }

  private writeNotice(res: ServerResponse, text: string) {
    const short = Array.from(text.replace(/\s+/g, " ")).slice(0, 300).join("");
    res.write(`data: ${JSON.stringify({ type: "CUSTOM", name: "ps5.notify", value: { title: this.options.agentName || "PS5 A2UI", text: short } })}\n\n`);
  }

  private sendText(conversationKey: string, messageId: string, text: string) {
    this.send(conversationKey, { type: "TEXT_MESSAGE_START", messageId, role: "assistant" });
    this.send(conversationKey, { type: "TEXT_MESSAGE_CONTENT", messageId, delta: text });
    this.send(conversationKey, { type: "TEXT_MESSAGE_END", messageId });
  }

  /** A snapshot replaces the surface, so a first post and an update send the same thing. */
  private sendSurface(conversationKey: string, surfaceId: string, built: { components: Json[]; data: Json }, _create: boolean) {
    this.send(conversationKey, surfaceSnapshot(surfaceId, createSurface(surfaceId, built.components, built.data)));
  }

  private authorized(req: IncomingMessage): boolean {
    const want = this.options.token;
    if (!want) return true;
    const header = req.headers.authorization ?? "";
    const given = Buffer.from(header.startsWith("Bearer ") ? header.slice(7) : "");
    const expected = Buffer.from(want);
    return given.length === expected.length && timingSafeEqual(given, expected);
  }

  private async handle(req: IncomingMessage, res: ServerResponse) {
    const url = new URL(req.url ?? "/", "http://ps5");
    if (!this.authorized(req)) return void res.writeHead(401).end();

    if (req.method === "GET" && url.pathname === "/events") {
      const id = url.searchParams.get("console") ?? "";
      if (!/^[\w.-]{1,64}$/.test(id)) return void res.writeHead(400).end();
      const key = consoleKey(id);
      const role = url.searchParams.get("role") === "presence" ? "presence" : "app";
      res.writeHead(200, { "content-type": "text/event-stream", "cache-control": "no-cache", connection: "keep-alive" });
      res.write(": connected\n\n");
      const map = role === "presence" ? this.presence : this.streams;
      if (role === "app") {
        if (this.options.agentName) res.write(`data: ${JSON.stringify({ type: "CUSTOM", name: "ps5.agent", value: { name: this.options.agentName } })}\n\n`);
        // What the agent said while the app was away.
        const queue = this.queued.get(key) ?? [];
        for (const event of queue) res.write(`data: ${JSON.stringify(event)}\n\n`);
        if (queue.length) this.options.log(`console ${id}: sent ${queue.length} queued events`);
        this.queued.delete(key);
        this.queuedText.delete(key);
        this.missed.delete(key);
        const pending = this.notifyTimers.get(key);
        if (pending) clearTimeout(pending.timer);
        this.notifyTimers.delete(key);
      }
      if (!map.has(key)) map.set(key, new Set());
      map.get(key)!.add(res);
      // The background payload connecting while the app is closed: what it missed, as one notice.
      const missed = this.missed.get(key);
      if (role === "presence" && missed && !this.streams.get(key)?.size) {
        this.writeNotice(res, missed.count > 1 ? `${missed.count} new messages. Latest: ${missed.text}` : missed.text);
        this.missed.delete(key);
      }
      const what = role === "presence" ? `console ${id} (background)` : `console ${id}`;
      this.options.log(`${what} connected`);
      req.on("close", () => {
        map.get(key)?.delete(res);
        this.options.log(`${what} disconnected`);
      });
      return;
    }

    if (req.method !== "POST") return void res.writeHead(404).end();
    let body: Json;
    try {
      body = JSON.parse(await readBody(req));
    } catch {
      return void res.writeHead(400).end();
    }
    const id = String(body.console ?? "");
    if (!/^[\w.-]{1,64}$/.test(id)) return void res.writeHead(400).end();
    const key = consoleKey(id);
    const base = this.ingress(key, id);
    const sink = this.sink!;

    if (url.pathname === "/hello") {
      const c = (body.client ?? {}) as Json;
      const list = (v: unknown) => (Array.isArray(v) ? v.filter((x) => typeof x === "string").slice(0, 64) : undefined);
      this.clients.set(key, {
        name: typeof c.name === "string" ? c.name.slice(0, 64) : undefined,
        catalogId: typeof c.catalogId === "string" ? c.catalogId : undefined,
        functions: list(c.functions),
        haptics: list(c.haptics),
        controller: typeof c.controller === "boolean" ? c.controller : undefined,
        reads: list(c.reads),
      });
      this.options.log(`console ${id}: ${JSON.stringify(this.clients.get(key))}`);
    } else if (url.pathname === "/turn") {
      const text = typeof body.text === "string" ? body.text.slice(0, 4000) : "";
      if (!text.trim()) return void res.writeHead(400).end();
      const messageId = randomUUID();
      // Not awaited: the engine queues the turn and runs the agent; the reply comes on /events.
      // The engine rejects when a handler throws (e.g. a failed run, already reported to the
      // console as RUN_ERROR); left unhandled, that rejection would end the process.
      this.settle("turn", sink.onTurn({
        ...base,
        platform: this.platform,
        eventId: messageId,
        userText: text,
        operation: { kind: "created", logicalMessageId: messageId, revisionId: messageId, mentioned: true },
      }));
    } else if (url.pathname === "/reset") {
      this.history.delete(key);
      this.pendingAction.delete(key);
      this.queued.delete(key);
      this.queuedText.delete(key);
      this.options.log(`console ${id}: new conversation`);
    } else if (url.pathname === "/reply") {
      const pending = typeof body.id === "string" ? this.requests.get(body.id) : undefined;
      if (!pending) return void res.writeHead(404).end();
      clearTimeout(pending.timer);
      this.requests.delete(body.id);
      pending.resolve(body.result && typeof body.result === "object" ? body.result : { error: "bad_reply" });
    } else if (url.pathname === "/action") {
      const message = body.action as Json | undefined;
      const action = message?.version === A2UI_VERSION ? (message.action as Json | undefined) : undefined;
      if (!action || typeof action.name !== "string") return void res.writeHead(400).end();
      const eventId = randomUUID();
      if (action.name === CHANNEL_ACTION) {
        const ctx = (action.context ?? {}) as Json;
        if (typeof ctx.id !== "string" || !ctx.id) return void res.writeHead(400).end();
        this.settle("action", sink.onInteraction({ ...base, eventId, id: ctx.id, value: ctx.value }));
      } else {
        // A press on the agent's own screen: the next run carries it (see conversationStore).
        this.pendingAction.set(key, message!);
        this.settle("action", sink.onInteraction({ ...base, eventId, id: PS5_A2UI_ACTION, value: action }));
      }
    } else {
      return void res.writeHead(404).end();
    }
    res.writeHead(202, { "content-type": "application/json" }).end('{"ok":true}');
  }

  /** Logs a failed engine call made without awaiting it, so its rejection is handled. */
  private settle(what: string, result: void | Promise<void>) {
    void Promise.resolve(result).catch((e) => this.options.log(`${what} failed: ${(e as Error)?.message ?? e}`));
  }

  private ingress(conversationKey: string, id: string) {
    const actor = { id: conversationKey, kind: "human" as const, name: this.clients.get(conversationKey)?.name ?? "PS5 player" };
    return {
      conversationKey,
      replyTarget: { conversationKey } satisfies Target,
      actor,
      identityContext: {
        tenant: { id: "local" },
        installation: { id: "ps5" },
        conversation: { id: conversationKey, kind: "direct" },
        trigger: "message",
        event: { id: randomUUID() },
        raw: { console: id },
      },
    };
  }
}


function readBody(req: IncomingMessage, limit = 64 * 1024): Promise<string> {
  return new Promise((resolve, reject) => {
    let size = 0;
    const parts: Buffer[] = [];
    req.on("data", (chunk: Buffer) => {
      size += chunk.length;
      if (size > limit) {
        reject(new Error("body too large"));
        req.destroy();
      } else parts.push(chunk);
    });
    req.on("end", () => resolve(Buffer.concat(parts).toString("utf8")));
    req.on("error", reject);
  });
}

/** Events kept per console while the app isn't connected. */
const kQueue = 2000;

/** The adapter, for createChannel({ adapters: [ps5({...})] }). */
export function ps5(options: Ps5Options = {}): Ps5Adapter {
  return new Ps5Adapter(options);
}

/**
 * A press on one of the agent's own screens runs the agent again with that press (the adapter
 * adds it to the run as forwardedProps.copilotkit.ui, copilotkit_action). Call once on the channel.
 */
export function handlePs5Actions(channel: { onInteraction: (id: string, handler: (ctx: { thread: { runAgent: () => Promise<unknown> } }) => unknown) => unknown }) {
  channel.onInteraction(PS5_A2UI_ACTION, async ({ thread }) => {
    await thread.runAgent();
  });
}

/** A tool's parameters as a Standard Schema object (what channel tools take): JSON Schema as is. */
function objectSchema(properties: Json = {}, required: string[] = []) {
  const jsonSchema = () => ({ type: "object", properties, required, additionalProperties: false });
  return {
    "~standard": {
      version: 1 as const,
      vendor: "copilotkit-channels-ps5",
      validate: (value: unknown) =>
        value && typeof value === "object" && !Array.isArray(value) ? { value } : { issues: [{ message: "Expected an object" }] },
      jsonSchema: { input: jsonSchema, output: jsonSchema },
    },
  };
}

/**
 * The console's own data as channel tools (CONSOLE_READS: installed games, system info,
 * temperatures, saves, screenshots, recently played; plus the actions not available yet, NOT_YET): the agent gets them as client tools (a tool of the same
 * name as one of its own replaces it), and the channel answers each call by asking the console
 * (consoleRead). Register them with `for (const t of ps5ConsoleTools(adapter)) channel.tool(t)`.
 */
export function ps5ConsoleTools(adapter: Ps5Adapter) {
  // The adapter makes each console's conversation key its actor id (see ingress).
  const keyOf = (ctx: { actor?: { id?: string } }) => String(ctx.actor?.id ?? "");
  return [
    ...CONSOLE_READS.map((r) => ({
      name: r.tool,
      description: r.description,
      parameters: objectSchema(),
      handler: async (_args: Json, ctx: { actor?: { id?: string } }) => r.result(await adapter.consoleRead(keyOf(ctx), r.read)),
    })),
    ...NOT_YET.map((t) => ({
      name: t.tool,
      description: t.description,
      parameters: objectSchema(t.parameters.properties, t.parameters.required),
      handler: async (args: Json) => t.result(args),
    })),
    {
      name: FOLLOW_UPS.tool,
      description: FOLLOW_UPS.description,
      parameters: objectSchema(FOLLOW_UPS.parameters.properties, FOLLOW_UPS.parameters.required),
      handler: async (args: Json, ctx: { actor?: { id?: string } }) => {
        const prompts = FOLLOW_UPS.clean(args);
        if (!prompts.length) return { error: "invalid_arguments", details: "prompts: 1 to 3 non-empty strings" };
        adapter.suggestFollowUps(keyOf(ctx), prompts);
        return { status: "shown" };
      },
    },
    {
      name: ASK_CHOICES.tool,
      description: ASK_CHOICES.description,
      parameters: objectSchema(ASK_CHOICES.parameters.properties, ASK_CHOICES.parameters.required),
      handler: async (args: Json, ctx: { actor?: { id?: string } }) => {
        const prompt = ASK_CHOICES.prompt(args);
        const options = ASK_CHOICES.clean(args);
        if (!prompt || options.length < 2) return { error: "invalid_arguments", details: "prompt and 2 to 5 options with labels" };
        const how = adapter.askChoices(keyOf(ctx), prompt, options);
        if (how === "queued") return { error: "not_delivered", next: "The app is closed and no background listener is connected, so the player can't be asked right now." };
        return { status: how === "menu" ? "shown_in_app" : "notified", next: "Stop and wait: the player's choice arrives as their next message." };
      },
    },
  ];
}
