// The agent runtime the server and the console run: the PS5 server's run loop, in TypeScript for speed of
// iteration. One run = model calls until the model shows a screen or answers in text (like
// CopilotKit's BuiltInAgent). Domain tools run server-side within the run. Intent tools become
// A2UI surfaces and end the run. A scripted player answers surfaces; presses go back the CopilotKit
// way (log_a2ui_event). Features under test:
//   - argument checks on intent calls, rejected calls go back as tool errors within the run;
//   - secret handles for password keyboards;
//   - an approval gate: tools marked `approval` pause the run on a runtime-made confirm, then run
//     or are denied, and the run resumes with the tool result;
//   - fault injection on screens (to exercise the retry path).
import { Ajv } from "ajv";
import type { ValidateFunction } from "ajv";
import { deviceTools, intentTools, toA2UI, toFunctionCalls } from "./intent-tools.ts";
import { validateSurface } from "./validate.ts";
import { type Console, domainTools, execDomain, WIFI_PASSWORD } from "./console-sim.ts";

type Json = Record<string, any>;

export const BASE_PROMPT =
  "You are the assistant built into a PlayStation 5, shown on a TV and operated with a DualSense controller from the couch. " +
  "There is no mouse and no physical keyboard. Answer briefly, and only what the user actually asked — don't volunteer adjacent facts (if they ask about storage, answer about storage, not the model or firmware). " +
  "When a screen, a choice, text entry or a confirmation helps the user, render native UI with your tools instead of describing it in text. " +
  "The TV cannot render markdown: never put a table, a list, stats or structured data in your text or as markdown. Show structured data with a tool — show_stats for numbers and metrics (storage, counts, sizes, times) as a dashboard of value tiles, show_menu for a list of items to choose from, show_info for a few short lines of prose, show_pages for sections, and show_progress with a 0-1 value for a single proportion. Keep any plain text to a sentence or two of explanation around the UI, not the data itself. " +
  "After you show something the user must answer, stop and wait: their answer arrives as a log_a2ui_event result.\n" +
  "Use the console tools to read and change the console. Never say something happened unless a tool result says so; if a tool fails, tell the user and offer a way forward. " +
  "Passwords typed on a password keyboard reach you as a secret:// handle: pass the handle as-is to the tool that needs it, never ask the user to say or show a password.";
export const UI_RULE =
  " `notify` is only for news that needs no answer. After a failure the user can fix (wrong password, invalid input, not enough memory), show something they can act on: a keyboard, a menu or a confirm. Never change something the user didn't choose. When the user picks a game (a tile_select), don't act on it right away: ask what they'd like to do with it, as a show_menu of what your tools can do for that game (for example how long they've played it, its saves, starting it), saying which of those aren't available yet. Offer 2 or 3 next requests the user is likely to make with most answers: in the screen tool's followUps when you show a screen that needs no answer, else by calling suggest_follow_ups in the same message as your text.";
export const HONESTY_RULE =
  " Only show games, saves, networks or models that a tool returned, and only show progress or success for something a tool actually did. If no tool can do what the user asks (for example installing or buying a game), say so in one sentence instead of pretending. When a tool gives a game an icon, pass it as the tile's image so games look the same wherever they appear.";
export const CONFIRM_RULE = " Ask with `confirm` (destructive=true) before deleting anything.";
export const GATE_RULE =
  " Tools that need approval (delete_save, launch_game over a running game) are approved by the console itself: call them directly instead of asking with `confirm` first. If the result is denied_by_user, don't call it again unless the user asks.";

export interface Options {
  model: string;
  system: string;
  approvalGate: boolean;
  actionableErrors: boolean;
  /** Reject the first N screens of the session with a retryable render error. */
  faultScreens?: number;
  maxCallsPerRun?: number;
  maxTurnsPerTask?: number;
}

export interface Player {
  menu?: (labels: string[]) => number;
  text?: (n: number) => string; // n = keyboards answered so far in this task
  confirm?: (message: string, approval: boolean) => boolean;
}
export interface Task { prompt: string; player: Player }

export interface DomainCall { task: number; name: string; args: Json; result: Json; via: "direct" | "approved" | "denied" | "blocked" }
export interface TaskTrace { screens: string[][]; outputs: string[]; modelSaw: string[]; turns: number; confirms: { by: "model" | "runtime"; message: string; answer: boolean }[] }
export interface SessionTrace {
  console: Console;
  domain: DomainCall[];
  tasks: TaskTrace[];
  rejected: { name: string; details: string; recovered: boolean }[];
  calls: { task: number; promptTokens: number; completionTokens: number; ms: number }[];
  problems: string[];
  log: string[];
}

// ---------- model ----------

const BASE_URL = process.env.LLM_BASE_URL ?? "https://openrouter.ai/api/v1";
const KEY = process.env.LLM_API_KEY;

/**
 * One model call. `onRetry` is told before each wait (rate limit or transient error), e.g. to show
 * "retrying" in the UI. With `onText`, the reply streams: visible text (outside `<think>` blocks)
 * goes to `onText` as it arrives, and the result has the same shape as without it. A call is only
 * retried while nothing has been shown.
 */
export async function complete(model: string, messages: Json[], tools: Json[], onRetry?: (waitMs: number, reason: string) => void, onText?: (delta: string) => void) {
  if (!KEY) throw new Error("set LLM_API_KEY (and LLM_MODEL); any OpenAI-compatible provider, OpenRouter by default. Override the endpoint with LLM_BASE_URL.");
  for (let attempt = 0; ; attempt++) {
    let status = 0;
    let body: any;
    let shown = false;
    try {
      const request = {
        method: "POST",
        headers: { "content-type": "application/json", authorization: `Bearer ${KEY}` },
        body: JSON.stringify({ model, messages, tools, tool_choice: "auto", max_tokens: 8000, ...(BASE_URL.includes("minimax") ? { reasoning_split: true } : {}), ...(onText ? { stream: true } : {}) }),
        signal: AbortSignal.timeout(300_000),
      };
      if (onText) {
        const reply = new StreamedReply((delta) => {
          shown = true;
          onText(delta);
        });
        status = await fetchChunks(`${BASE_URL}/chat/completions`, request, (chunk) => reply.feed(chunk));
        body = reply.result();
      } else {
        const res = await fetch(`${BASE_URL}/chat/completions`, request);
        status = res.status;
        body = await res.json().catch(() => ({}));
      }
    } catch (e) {
      body = { error: String((e as Error).cause ?? e) };
    }
    const ok = status >= 200 && status < 300;
    if (ok && !body?.base_resp?.status_code && !body?.error) return body;
    const limited = status === 429 || body?.base_resp?.status_code === 1002;
    const transient = !status || limited || status >= 500;
    if (!transient || attempt >= 5 || shown) throw new Error(`${status || "network"} ${JSON.stringify(body).slice(0, 300)}`);
    const waitMs = limited ? 15_000 * 2 ** Math.min(attempt, 3) : 2000 * (attempt + 1);
    onRetry?.(waitMs, limited ? "rate_limited" : status ? `http_${status}` : "network");
    await new Promise((r) => setTimeout(r, waitMs));
  }
}

/**
 * POST and hand the response body to `onChunk` as it arrives; resolves with the HTTP status. On
 * the console (QuickJS) the host's fetch delivers the chunks (__host.fetch's chunk callback).
 */
async function fetchChunks(url: string, init: Json, onChunk: (text: string) => void): Promise<number> {
  const host = (globalThis as any).__fetchChunks;
  if (host) return host(url, init, onChunk);
  const res = await fetch(url, init);
  const reader = res.body!.getReader();
  const decoder = new TextDecoder();
  for (;;) {
    const { value, done } = await reader.read();
    if (done) break;
    onChunk(decoder.decode(value, { stream: true }));
  }
  onChunk(decoder.decode());
  return res.status;
}

/**
 * Reads an OpenAI-compatible streamed reply (server-sent `chat.completion.chunk`s) into the
 * non-streamed shape: { choices: [{ message: { content, tool_calls } }] }. Visible text goes to
 * `onText` as it comes, without `<think>…</think>` and leading whitespace. A body that isn't a
 * stream (an error, or MiniMax's base_resp) is returned as it is.
 */
export class StreamedReply {
  private buffer = "";
  private raw = "";
  private events = 0;
  private content = "";
  private toolCalls: Json[] = [];
  private finishReason: string | null = null;
  private pending = "";  // text held back: maybe the start of a <think> tag
  private inThink = false;
  private started = false;

  private readonly onText: (delta: string) => void;

  constructor(onText: (delta: string) => void) {
    this.onText = onText;
  }

  feed(chunk: string) {
    if (this.raw.length < 65536) this.raw += chunk;
    this.buffer += chunk;
    for (let i; (i = this.buffer.indexOf("\n")) >= 0; ) {
      const line = this.buffer.slice(0, i).replace(/\r$/, "");
      this.buffer = this.buffer.slice(i + 1);
      if (!line.startsWith("data:")) continue;
      const data = line.slice(5).trim();
      if (!data || data === "[DONE]") continue;
      let event: Json;
      try {
        event = JSON.parse(data);
      } catch {
        continue;
      }
      this.events++;
      if (event.base_resp?.status_code) this.raw = data;  // an error inside the stream
      const choice = event.choices?.[0];
      if (!choice) continue;
      if (choice.finish_reason) this.finishReason = choice.finish_reason;
      const delta = choice.delta ?? {};
      if (typeof delta.content === "string" && delta.content) {
        this.content += delta.content;
        this.visible(delta.content);
      }
      for (const tc of delta.tool_calls ?? []) {
        const index = tc.index ?? this.toolCalls.length;
        const call = (this.toolCalls[index] ??= { id: "", type: "function", function: { name: "", arguments: "" } });
        if (tc.id) call.id = tc.id;
        if (tc.function?.name) call.function.name += tc.function.name;
        if (tc.function?.arguments) call.function.arguments += tc.function.arguments;
      }
    }
  }

  result(): Json {
    if (!this.events || /"base_resp"\s*:\s*\{\s*"status_code"\s*:\s*[1-9]/.test(this.raw)) {
      try {
        return JSON.parse(this.raw);
      } catch {
        return { error: this.raw.slice(0, 300) || "empty reply" };
      }
    }
    if (!this.inThink && this.pending) this.emit(this.pending);
    this.pending = "";
    const toolCalls = this.toolCalls.filter(Boolean);
    return { choices: [{ message: { role: "assistant", content: this.content, ...(toolCalls.length ? { tool_calls: toolCalls } : {}) }, finish_reason: this.finishReason }] };
  }

  /** Passes text on, minus <think>…</think> (tags may be split across deltas). */
  private visible(text: string) {
    let s = this.pending + text;
    this.pending = "";
    for (;;) {
      if (this.inThink) {
        const end = s.indexOf("</think>");
        if (end < 0) {
          this.pending = s.slice(-7);  // may hold the start of "</think>"
          return;
        }
        s = s.slice(end + 8);
        this.inThink = false;
      }
      const start = s.indexOf("<think>");
      if (start >= 0) {
        this.emit(s.slice(0, start));
        s = s.slice(start + 7);
        this.inThink = true;
        continue;
      }
      // Hold back a tail that could be the start of "<think>".
      let keep = 0;
      for (let k = Math.min(6, s.length); k > 0; k--)
        if ("<think>".startsWith(s.slice(-k))) {
          keep = k;
          break;
        }
      this.emit(s.slice(0, s.length - keep));
      this.pending = s.slice(s.length - keep);
      return;
    }
  }

  private emit(text: string) {
    if (!this.started) text = text.replace(/^\s+/, "");
    if (!text) return;
    this.started = true;
    this.onText(text);
  }
}

// ---------- argument checks ----------

const ajv = new Ajv({ strict: false, allErrors: true });
const checkArgs = new Map<string, ValidateFunction>([...intentTools, ...deviceTools].map((t): [string, ValidateFunction] => [t.name, ajv.compile(t.parameters)]));
const MIN_ITEMS: Record<string, string> = { show_menu: "items", show_shelf: "rows", show_pages: "pages" };
export function intentProblem(name: string, args: Json): string | null {
  const check = checkArgs.get(name)!;
  if (!check(args)) return ajv.errorsText(check.errors);
  const list = MIN_ITEMS[name];
  if (list && !(Array.isArray(args[list]) && args[list].length > 0)) return `${list} must not be empty`;
  return null;
}

// ---------- session ----------

interface Pending { kind: "surface"; name: string; surfaceId: string; components: Json[] }
interface Approval { kind: "approval"; toolCallId: string; name: string; args: Json; message: string }

export async function runSession(tasks: Task[], c: Console, opts: Options): Promise<SessionTrace> {
  const tools = domainTools({ approvalGate: opts.approvalGate });
  const toolDefs = [...intentTools, ...deviceTools, ...tools.map(({ approval: _a, progress: _p, ...t }) => t)].map((t) => ({ type: "function", function: t }));
  const isDomain = (n: string) => tools.find((t) => t.name === n);
  const maxCalls = opts.maxCallsPerRun ?? 6;
  const maxTurns = opts.maxTurnsPerTask ?? 7;
  const secrets = new Map<string, string>();
  const trace: SessionTrace = { console: c, domain: [], tasks: [], rejected: [], calls: [], problems: [], log: [] };
  const messages: Json[] = [{ role: "system", content: opts.system }];
  let faults = opts.faultScreens ?? 0;
  let lastModelConfirm: boolean | null = null;

  for (const [ti, task] of tasks.entries()) {
    const tt: TaskTrace = { screens: [], outputs: [], modelSaw: [], turns: 0, confirms: [] };
    trace.tasks.push(tt);
    trace.log.push(`user: ${task.prompt}`);
    messages.push({ role: "user", content: task.prompt });
    let keyboards = 0;
    let resume: { toolCallId: string; result: Json } | null = null;

    for (let turn = 0; turn < maxTurns; turn++) {
      tt.turns = turn + 1;
      const pending: (Pending | Approval)[] = [];
      if (resume) { messages.push({ role: "tool", tool_call_id: resume.toolCallId, content: JSON.stringify(resume.result) }); resume = null; }

      // One run.
      for (let i = 0; ; i++) {
        if (i >= maxCalls) { trace.problems.push(`task ${ti + 1}: more than ${maxCalls} model calls in one run`); break; }
        const started = Date.now();
        const body = await complete(opts.model, messages, toolDefs);
        trace.calls.push({ task: ti, promptTokens: body.usage?.prompt_tokens ?? 0, completionTokens: body.usage?.completion_tokens ?? 0, ms: Date.now() - started });
        const msg = body.choices?.[0]?.message ?? {};
        const text = String(msg.content ?? "").replace(/<think>[\s\S]*?<\/think>/g, "").trim();
        if (text) { tt.outputs.push(text); trace.log.push(`  text: ${text.slice(0, 160)}`); }
        messages.push({ role: "assistant", content: msg.content ?? "", ...(msg.tool_calls?.length ? { tool_calls: msg.tool_calls } : {}) });
        let ends = false;
        let retryAfterReject = false;
        for (const tc of msg.tool_calls ?? []) {
          const name = tc.function?.name;
          let args: Json = {};
          try { args = JSON.parse(tc.function?.arguments || "{}"); } catch { trace.problems.push(`${name}: arguments are not JSON`); }
          tt.outputs.push(JSON.stringify(args));
          const domain = isDomain(name);
          if (domain) {
            const approval = domain.approval?.(args, c);
            if (approval) {
              // The runtime gates this call: pause the run on a system confirm.
              pending.push({ kind: "approval", toolCallId: tc.id, name, args, message: approval.message });
              trace.log.push(`  ${name}(${JSON.stringify(args)}) → awaiting approval`);
              ends = true;
              continue;
            }
            const result = execDomain(c, name, args, secrets, opts.actionableErrors);
            trace.domain.push({ task: ti, name, args, result, via: "direct" });
            trace.log.push(`  ${name}(${JSON.stringify(args)}) → ${JSON.stringify(result)}`);
            messages.push({ role: "tool", tool_call_id: tc.id, content: JSON.stringify(result) });
          } else if (intentTools.some((t) => t.name === name)) {
            const problem = intentProblem(name, args) ?? (faults > 0 ? "FAULT" : null);
            if (problem) {
              const details = problem === "FAULT" ? "The screen could not be shown (renderer busy). Call the tool again." : problem;
              if (problem === "FAULT") faults--;
              trace.rejected.push({ name, details, recovered: false });
              trace.log.push(`  [${name}] rejected: ${details}`);
              messages.push({ role: "tool", tool_call_id: tc.id, content: JSON.stringify({ error: problem === "FAULT" ? "render_failed" : "invalid_arguments", retryable: true, details }) });
              retryAfterReject = true;
              continue;
            }
            const surface = toA2UI(name, args);
            const report = validateSurface(surface.components, surface.data);
            if (!report.ok) trace.problems.push(`${name}: invalid surface: ${report.errors.slice(0, 2).join("; ")}`);
            // A rendered screen after a rejection in this run counts as a recovery.
            for (const r of trace.rejected) if (!r.recovered) r.recovered = true;
            pending.push({ kind: "surface", name, surfaceId: `surface-${tc.id}`, components: surface.components });
            trace.log.push(`  [${name}] ${JSON.stringify(args).slice(0, 160)}`);
            messages.push({ role: "tool", tool_call_id: tc.id, content: '{"status":"rendered"}' });
            ends = true;
          } else if (deviceTools.some((t) => t.name === name)) {
            // Runs on the controller; the run goes on.
            const out = intentProblem(name, args) ?? (() => { const r = toFunctionCalls(name, args); return "problem" in r ? r.problem : null; })();
            trace.log.push(`  ${name}(${JSON.stringify(args)})${out ? ` rejected: ${out}` : ""}`);
            messages.push({ role: "tool", tool_call_id: tc.id, content: JSON.stringify(out ? { error: "invalid_arguments", retryable: true, details: out } : { status: "done" }) });
          } else {
            trace.problems.push(`unknown tool ${name}`);
            messages.push({ role: "tool", tool_call_id: tc.id, content: '{"error":"unknown_tool"}' });
          }
        }
        if (ends) break;
        if (!(msg.tool_calls?.length)) break; // plain answer
        if (retryAfterReject) continue;
      }
      tt.screens.push(pending.map((p) => (p.kind === "surface" ? p.name : `approval:${p.name}`)));

      // The player answers: an approval first (it blocks the run), else the first interactive surface.
      const approval = pending.find((p): p is Approval => p.kind === "approval");
      if (approval) {
        // Other gated calls in the same message would need their own answers; deny them for now.
        for (const extra of pending.filter((p): p is Approval => p.kind === "approval" && p !== approval))
          messages.push({ role: "tool", tool_call_id: extra.toolCallId, content: JSON.stringify({ error: "one_approval_at_a_time" }) });
        const yes = task.player.confirm?.(approval.message, true) ?? false;
        tt.confirms.push({ by: "runtime", message: approval.message, answer: yes });
        let result: Json;
        if (yes) {
          result = execDomain(c, approval.name, approval.args, secrets, opts.actionableErrors);
          trace.domain.push({ task: ti, name: approval.name, args: approval.args, result, via: "approved" });
        } else {
          result = { error: "denied_by_user", next: "Acknowledge; don't call this again unless the user asks." };
          trace.domain.push({ task: ti, name: approval.name, args: approval.args, result, via: "denied" });
        }
        trace.log.push(`player → approval ${yes ? "approved" : "denied"}: ${approval.message}`);
        trace.log.push(`  ${approval.name}(${JSON.stringify(approval.args)}) → ${JSON.stringify(result)}`);
        resume = { toolCallId: approval.toolCallId, result };
        continue;
      }
      const answered = answerSurface(pending.filter((p): p is Pending => p.kind === "surface"), task.player, keyboards);
      if (!answered) break; // nothing to answer: task over
      let context = answered.action.context;
      if (answered.typed !== undefined) {
        keyboards++;
        if (answered.kind === "password") {
          const handle = `secret://${secrets.size + 1}`;
          secrets.set(handle, answered.typed);
          context = { value: handle };
        }
      }
      if (answered.action.name === "confirm_result") {
        lastModelConfirm = Boolean(context.confirmed);
        tt.confirms.push({ by: "model", message: answered.message ?? "", answer: lastModelConfirm });
      }
      const action = { ...answered.action, context };
      const sent = `User performed action "${action.name}" on surface "${action.surfaceId}" (component: ${action.sourceComponentId}). Context: ${JSON.stringify(action.context)}`;
      tt.modelSaw.push(sent);
      trace.log.push(`player → ${action.name} ${JSON.stringify(action.context)}`);
      const logId = `call_a2ui_${ti}_${turn}`;
      messages.push({ role: "assistant", content: "", tool_calls: [{ id: logId, type: "function", function: { name: "log_a2ui_event", arguments: JSON.stringify(action) } }] });
      messages.push({ role: "tool", tool_call_id: logId, content: sent });
    }
    if (resume) {
      // The task ended on an approval answer with no further turn budget: deliver the result anyway.
      messages.push({ role: "tool", tool_call_id: resume.toolCallId, content: JSON.stringify(resume.result) });
      trace.problems.push(`task ${ti + 1}: ran out of turns after an approval`);
    }
    const last = tt.screens.at(-1) ?? [];
    if (tt.turns >= maxTurns && last.length) trace.problems.push(`task ${ti + 1}: did not finish within ${maxTurns} turns`);
  }

  for (const tt of trace.tasks) {
    if (tt.modelSaw.some((s) => s.includes(WIFI_PASSWORD))) trace.problems.push("the real password was sent to the model");
    if (tt.outputs.some((s) => s.includes(WIFI_PASSWORD))) trace.problems.push("the model repeated the real password");
  }
  for (const r of trace.rejected) if (!r.recovered) trace.problems.push(`rejected ${r.name} was never re-shown in its run`);
  return trace;
}

function answerSurface(surfaces: Pending[], p: Player, keyboards: number): { action: { name: string; surfaceId: string; sourceComponentId: string; context: Json }; typed?: string; kind?: string; message?: string } | null {
  for (const s of surfaces) {
    const root = s.components.find((x) => x.id === "root")!;
    if (s.name === "show_menu" && p.menu) {
      const items = s.components.filter((x) => x.component === "MenuItem");
      if (!items.length) continue;
      const i = Math.max(0, Math.min(items.length - 1, p.menu(items.map((x) => String(x.label)))));
      return { action: { name: items[i].action.event.name, surfaceId: s.surfaceId, sourceComponentId: items[i].id, context: items[i].action.event.context } };
    }
    if (s.name === "confirm" && p.confirm) {
      const yes = p.confirm(String(root.message), false);
      const ev = (yes ? root.confirm : root.cancel).event;
      return { action: { name: ev.name, surfaceId: s.surfaceId, sourceComponentId: "root", context: ev.context }, message: String(root.message) };
    }
    if (s.name === "ask_text" && p.text) {
      const typed = p.text(keyboards);
      return { action: { name: root.submit.event.name, surfaceId: s.surfaceId, sourceComponentId: "root", context: { value: typed } }, typed, kind: root.kind ?? "text" };
    }
  }
  return null;
}

export async function pool<T>(jobs: (() => Promise<T>)[], size: number): Promise<T[]> {
  const out: T[] = new Array(jobs.length);
  let next = 0;
  await Promise.all(Array.from({ length: size }, async () => {
    while (next < jobs.length) { const i = next++; out[i] = await jobs[i](); process.stderr.write("."); }
  }));
  process.stderr.write("\n");
  return out;
}
