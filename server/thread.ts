// One conversation with the PS5 agent, driven by AG-UI runs.
//
// A run starts from a new user message or from a button press (an A2UI 1.0 action message in
// forwardedProps.copilotkit.ui, next to the press's visible user message, as CopilotKit 1.85 sends it). The thread keeps the model-facing history, the simulated
// console, password secrets and any pending approval on the server, so clients only send what's
// new (like CopilotKit's server-side threads).
//
// Events emitted per run (AG-UI): RUN_STARTED, TEXT_MESSAGE_START/CONTENT/END, TOOL_CALL_START/
// ARGS/END/RESULT, ACTIVITY_SNAPSHOT "generative-ui-surface" (a surface's A2UI 1.0 operations, as
// CopilotKit 1.85 sends them) and "generative-ui-function-call" (callRendererFunction: a catalog
// function for the client to run, e.g. a controller rumble), CUSTOM "status" (thinking / working /
// retrying), RUN_FINISHED or RUN_ERROR. See runtime/a2ui.ts.
//
// Behaviour follows the evals (eval/, 02-05): intent tools become A2UI surfaces and end
// the run; device tools (controller feedback) become catalog function calls and the run goes on;
// domain tools run here; approval-gated tools pause on a runtime-written confirm; intent
// arguments are checked and bad calls go back to the model; passwords reach the model as handles.
import { randomUUID } from "node:crypto";
import { deviceTools, intentTools, toA2UI, toFunctionCalls } from "../runtime/intent-tools.ts";
import { type Console, domainTools, execDomain, freshConsole } from "../runtime/console-sim.ts";
import { CONSOLE_READS, FOLLOW_UPS, NOT_YET } from "../channel/console-reads.ts";
import { BASE_PROMPT, complete, GATE_RULE, HONESTY_RULE, intentProblem, UI_RULE } from "../runtime/llm.ts";
import { actionOf, createSurface, functionCallEvent, PS5_CATALOG_ID, removedSurface, surfaceSnapshot } from "../runtime/a2ui.ts";

type Json = Record<string, any>;
export type Emit = (event: Json) => void;

export const CATALOG_ID = PS5_CATALOG_ID;
const MAX_CALLS_PER_RUN = 6;

export interface RunAgentInput {
  threadId: string;
  runId?: string;
  messages?: { id?: string; role: string; content?: string; toolCallId?: string }[];
  /** Client tools (AG-UI): the client runs them. One named like a built-in tool replaces it. */
  tools?: { name: string; description?: string; parameters?: Json }[];
  forwardedProps?: Json;
}

/**
 * Reads the console it runs on (the agent embedded in the console app), by name
 * (CONSOLE_READS: installed_titles, system_info, temperatures) → JSON, or {error, reason}.
 * Without it, the console is simulated.
 */
export type ConsoleSystem = (name: string) => Json;

/** With a real console: its reads as tools (replacing simulated ones of the same name), and the actions not available yet. */
const REAL_TOOLS: Record<string, { description: string; parameters?: Json }> = {
  ...Object.fromEntries(CONSOLE_READS.map((r) => [r.tool, { description: r.description }])),
  ...Object.fromEntries(NOT_YET.map((t) => [t.tool, { description: t.description, parameters: t.parameters }])),
};

interface SurfaceInfo { tool: string; args: Json }
interface PendingApproval { toolCallId: string; name: string; args: Json; surfaceId: string }

export class AgentThread {
  readonly console: Console = freshConsole();
  private readonly secrets = new Map<string, string>();
  private readonly surfaces = new Map<string, SurfaceInfo>();
  private readonly seenMessages = new Set<string>();
  private readonly tools = domainTools({ approvalGate: true });
  private readonly toolDefs: Json[];
  private readonly history: Json[] = [{ role: "system", content: BASE_PROMPT + UI_RULE + GATE_RULE + HONESTY_RULE }];
  private pending: PendingApproval | null = null;
  /** This run's client tools, by name. */
  private clientTools = new Map<string, Json>();
  /** Client tool calls the run ended on: their results come with the next run. */
  private readonly clientCalls = new Set<string>();
  /** A client that said no controller is connected (forwardedProps.ps5Client): no controller tools. */
  private noController = false;
  running = false;

  readonly id: string;
  private readonly model: string;
  private readonly system?: ConsoleSystem;

  constructor(id: string, model: string, system?: ConsoleSystem) {
    this.id = id;
    this.model = model;
    this.system = system;
    const simulated = this.tools.map(({ approval: _a, progress: _p, ...t }) => t).filter((t) => !(system && REAL_TOOLS[t.name]));
    const real = system ? Object.entries(REAL_TOOLS).map(([name, t]) => ({ name, description: t.description, parameters: t.parameters ?? { type: "object", properties: {} } })) : [];
    const followUps = { name: FOLLOW_UPS.tool, description: FOLLOW_UPS.description, parameters: FOLLOW_UPS.parameters };
    this.toolDefs = [...intentTools, ...deviceTools, ...simulated, ...real, followUps].map((t) => ({ type: "function", function: t }));
  }

  /** A tool on the real console: one of its reads, or an action not available yet. */
  private onConsole(name: string, args: Json): Json {
    const read = CONSOLE_READS.find((r) => r.tool === name);
    if (read) return read.result(this.system!(read.read));
    return NOT_YET.find((t) => t.tool === name)!.result(args);
  }

  async run(input: RunAgentInput, emit: Emit): Promise<void> {
    const runId = input.runId ?? randomUUID();
    this.running = true;
    emit({ type: "RUN_STARTED", threadId: this.id, runId });
    try {
      const started = this.accept(input, emit);
      if (!started) {
        emit({ type: "RUN_ERROR", message: "Nothing to run: send a new user message or an A2UI action." });
        return;
      }
      if (started === "loop") await this.loop(emit);
      emit({ type: "RUN_FINISHED", threadId: this.id, runId });
    } catch (e) {
      emit({ type: "RUN_ERROR", message: String((e as Error).message ?? e) });
    } finally {
      this.running = false;
    }
  }

  /** Take in what the client sent. Returns "loop" to call the model, or false if there was nothing. */
  private accept(input: RunAgentInput, emit: Emit): "loop" | false {
    const client = input.forwardedProps?.ps5Client as Json | undefined;
    if (client && typeof client.controller === "boolean") this.noController = !client.controller;
    this.clientTools = new Map((input.tools ?? []).filter((t) => t && typeof t.name === "string").map((t) => [t.name, t]));
    // The results of client tools the last run ended on: the model goes on from them.
    const results = (input.messages ?? []).filter((m) => m.role === "tool" && m.toolCallId && this.clientCalls.has(m.toolCallId));
    for (const m of results) {
      this.history.push({ role: "tool", tool_call_id: m.toolCallId, content: String(m.content ?? "") });
      this.clientCalls.delete(m.toolCallId!);
      if (m.id) this.seenMessages.add(m.id);
    }
    if (results.length) {
      this.settleClientCalls();
      return "loop";
    }
    // Anything else: calls the client never answered get a result, so the history stays valid.
    this.settleClientCalls();
    const action = actionOf(input.forwardedProps);
    if (action) {
      // The press comes with its visible user message (CopilotKit's way); the action stands for it.
      for (const m of input.messages ?? []) if (m.role === "user" && m.id) this.seenMessages.add(m.id);
      if (this.pending && action.surfaceId === this.pending.surfaceId) {
        this.resolveApproval(action.name === "approval_result" && action.context?.confirmed === true, emit);
        return "loop";
      }
      this.denyPending(emit, "the user did something else instead");
      let context = action.context ?? {};
      const info = this.surfaces.get(action.surfaceId);
      if (info?.tool === "ask_text" && info.args.kind === "password" && typeof context.value === "string") {
        // The password stays here; the model gets a handle that domain tools resolve.
        const handle = `secret://${this.secrets.size + 1}`;
        this.secrets.set(handle, context.value);
        context = { ...context, value: handle };
      }
      const forwarded: Json = { ...action, context };
      const id = `call_a2ui_${randomUUID().slice(0, 8)}`;
      this.history.push({ role: "assistant", content: "", tool_calls: [{ id, type: "function", function: { name: "log_a2ui_event", arguments: JSON.stringify(forwarded) } }] });
      this.history.push({
        role: "tool",
        tool_call_id: id,
        content: `User performed action "${forwarded.name}" on surface "${forwarded.surfaceId}" (component: ${forwarded.sourceComponentId}). Context: ${JSON.stringify(forwarded.context)}`,
      });
      return "loop";
    }
    const fresh = (input.messages ?? []).filter((m) => m.role === "user" && (!m.id || !this.seenMessages.has(m.id)));
    const last = fresh.at(-1);
    if (!last?.content) return false;
    for (const m of fresh) if (m.id) this.seenMessages.add(m.id);
    this.denyPending(emit, "the user asked something else instead");
    this.history.push({ role: "user", content: last.content });
    return "loop";
  }

  private settleClientCalls() {
    for (const id of this.clientCalls) this.history.push({ role: "tool", tool_call_id: id, content: JSON.stringify({ error: "no_result" }) });
    this.clientCalls.clear();
  }

  private resolveApproval(yes: boolean, emit: Emit) {
    const p = this.pending!;
    this.pending = null;
    const result = yes
      ? { ...execDomain(this.console, p.name, p.args, this.secrets, true), approvedByUser: true }
      : { error: "denied_by_user", next: "Acknowledge; don't call this again unless the user asks." };
    this.history.push({ role: "tool", tool_call_id: p.toolCallId, content: JSON.stringify(result) });
    emit({ type: "TOOL_CALL_RESULT", messageId: randomUUID(), toolCallId: p.toolCallId, role: "tool", content: JSON.stringify(result) });
    emit(removedSurface(p.surfaceId));
  }

  /** A gated tool call must get a result before the model is called again. */
  private denyPending(emit: Emit, why: string) {
    if (!this.pending) return;
    const p = this.pending;
    this.pending = null;
    const result = { error: "denied_by_user", reason: why };
    this.history.push({ role: "tool", tool_call_id: p.toolCallId, content: JSON.stringify(result) });
    emit({ type: "TOOL_CALL_RESULT", messageId: randomUUID(), toolCallId: p.toolCallId, role: "tool", content: JSON.stringify(result) });
    emit(removedSurface(p.surfaceId));
  }

  private async loop(emit: Emit) {
    for (let call = 0; call < MAX_CALLS_PER_RUN; call++) {
      emit({ type: "CUSTOM", name: "status", value: { state: "thinking" } });
      // Client tools replace built-in ones of the same name.
      const own = this.toolDefs.filter((t) => !this.clientTools.has(t.function.name) && !(this.noController && deviceTools.some((d) => d.name === t.function.name)));
      const client = [...this.clientTools.values()].map((t) => ({ type: "function", function: { name: t.name, description: t.description ?? "", parameters: t.parameters ?? { type: "object", properties: {} } } }));
      const tools = [...own, ...client];
      // The reply's text streams to the client as the model writes it.
      const messageId = randomUUID();
      let streamed = false;
      let body: any;
      try {
        body = await complete(
          this.model,
          this.history,
          tools,
          (waitMs, reason) => emit({ type: "CUSTOM", name: "status", value: { state: "retrying", reason, retryInSeconds: Math.round(waitMs / 1000) } }),
          (delta) => {
            if (!streamed) emit({ type: "TEXT_MESSAGE_START", messageId, role: "assistant" });
            streamed = true;
            emit({ type: "TEXT_MESSAGE_CONTENT", messageId, delta });
          },
        );
      } finally {
        if (streamed) emit({ type: "TEXT_MESSAGE_END", messageId });  // also when the call fails midway
      }
      const msg = body.choices?.[0]?.message ?? {};
      this.history.push({ role: "assistant", content: msg.content ?? "", ...(msg.tool_calls?.length ? { tool_calls: msg.tool_calls } : {}) });

      let ends = false;
      for (const tc of msg.tool_calls ?? []) {
        const name: string = tc.function?.name;
        const raw: string = tc.function?.arguments || "{}";
        emit({ type: "TOOL_CALL_START", toolCallId: tc.id, toolCallName: name, parentMessageId: messageId });
        emit({ type: "TOOL_CALL_ARGS", toolCallId: tc.id, delta: raw });
        emit({ type: "TOOL_CALL_END", toolCallId: tc.id });
        if (this.clientTools.has(name)) {
          // The client runs it; this run ends, and the next one brings the result.
          this.clientCalls.add(tc.id);
          ends = true;
          continue;
        }
        let args: Json = {};
        try { args = JSON.parse(raw); } catch { /* checked below */ }
        const domain = this.tools.find((t) => t.name === name);

        const approval = this.system && REAL_TOOLS[name] ? null : domain?.approval?.(args, this.console);
        if (approval) {
          // Runtime-owned confirm; the tool result is delivered when the user answers.
          const surfaceId = `approval-${tc.id}`;
          this.pending = { toolCallId: tc.id, name, args, surfaceId };
          const { message, confirmLabel, cancelLabel, destructive } = approval;
          emit(surfaceSnapshot(surfaceId, createSurface(surfaceId, [{
            id: "root", component: "Confirm", message, destructive, confirmLabel, cancelLabel,
            confirm: { event: { name: "approval_result", userMessage: confirmLabel ?? "Yes", context: { confirmed: true } } },
            cancel: { event: { name: "approval_result", userMessage: cancelLabel ?? "No", context: { confirmed: false } } },
          }])));
          emit({ type: "CUSTOM", name: "status", value: { state: "waiting_for_approval", tool: name } });
          ends = true;
          continue;
        }
        if (domain || (this.system && REAL_TOOLS[name])) {
          emit({ type: "CUSTOM", name: "status", value: { state: "working", tool: name } });
          // Long work reports progress (the console shows its own progress dialog for it).
          const work = domain?.progress?.(args, this.console);
          if (work) {
            const steps = Math.max(1, Math.round(work.ms / 400));
            for (let i = 0; i <= steps; i++) {
              emit({ type: "CUSTOM", name: "status", value: { state: "working", tool: name, detail: work.label, progress: i / steps } });
              if (i < steps) await new Promise((r) => setTimeout(r, work.ms / steps));
            }
          }
          const result = this.system && REAL_TOOLS[name] ? this.onConsole(name, args) : execDomain(this.console, name, args, this.secrets, true);
          this.reply(tc.id, result, emit);
          continue;
        }
        if (intentTools.some((t) => t.name === name)) {
          const problem = intentProblem(name, args);
          if (problem) {
            this.reply(tc.id, { error: "invalid_arguments", retryable: true, details: problem }, emit);
            continue;
          }
          const surfaceId = `surface-${tc.id}`;
          const surface = toA2UI(name, args);
          this.surfaces.set(surfaceId, { tool: name, args });
          emit(surfaceSnapshot(surfaceId, createSurface(surfaceId, surface.components, surface.data)));
          const next = FOLLOW_UPS.clean({ prompts: args.followUps });
          if (next.length) emit(FOLLOW_UPS.event(next));
          this.reply(tc.id, { status: "rendered" }, emit);
          ends = true;
          continue;
        }
        if (name === FOLLOW_UPS.tool) {
          const prompts = FOLLOW_UPS.clean(args);
          if (!prompts.length) {
            this.reply(tc.id, { error: "invalid_arguments", retryable: true, details: "prompts: 1 to 3 non-empty strings" }, emit);
            continue;
          }
          emit(FOLLOW_UPS.event(prompts));
          this.reply(tc.id, { status: "shown" }, emit);
          // Its reply's text came with it: no need to ask the model again.
          if (msg.content) ends = true;
          continue;
        }
        if (deviceTools.some((t) => t.name === name)) {
          // On the controller: the client runs the catalog functions; the run goes on.
          const out = intentProblem(name, args) ?? (() => { const r = toFunctionCalls(name, args); return "problem" in r ? r.problem : r; })();
          if (typeof out === "string") {
            this.reply(tc.id, { error: "invalid_arguments", retryable: true, details: out }, emit);
            continue;
          }
          for (const call of out.calls) emit(functionCallEvent(call));
          this.reply(tc.id, { status: "done" }, emit);
          continue;
        }
        this.reply(tc.id, { error: "unknown_tool" }, emit);
      }
      if (ends || !msg.tool_calls?.length) return;
    }
    emit({ type: "CUSTOM", name: "status", value: { state: "gave_up", reason: `more than ${MAX_CALLS_PER_RUN} model calls in one run` } });
  }

  private reply(toolCallId: string, result: Json, emit: Emit) {
    const content = JSON.stringify(result);
    this.history.push({ role: "tool", tool_call_id: toolCallId, content });
    emit({ type: "TOOL_CALL_RESULT", messageId: randomUUID(), toolCallId, role: "tool", content });
  }
}

