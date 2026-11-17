// One conversation with the agent, driven by AG-UI runs.
//
// A run starts from a new user message or from a button press (an A2UI 1.0 action message in
// forwardedProps.copilotkit.ui, next to the press's visible user message, as CopilotKit 1.85
// sends it). The thread keeps the model-facing history, password secrets and any pending approval
// on its side, so clients only send what's new (like CopilotKit's server-side threads).
//
// Events emitted per run (AG-UI): RUN_STARTED, TEXT_MESSAGE_START/CONTENT/END, TOOL_CALL_START/
// ARGS/END/RESULT, ACTIVITY_SNAPSHOT "generative-ui-surface" (a surface's A2UI 1.0 operations, as
// CopilotKit 1.85 sends them) and "generative-ui-function-call" (callRendererFunction: a catalog
// function for the client to run, e.g. a controller rumble), CUSTOM "status" (thinking / working /
// retrying), RUN_FINISHED or RUN_ERROR. See a2ui.ts.
//
// What happens to a tool call:
// - an intent tool (show_menu, ask_text, …) becomes an A2UI surface and ends the run; its
//   arguments are checked first, and a bad call goes back to the model to fix;
// - a device tool (controller feedback) becomes catalog function calls, and the run goes on;
// - one of the agent's own tools (AgentTool) runs here; one that asks for approval pauses the run
//   on a Confirm the runtime writes itself;
// - a client tool (RunAgentInput.tools) ends the run, and the next run brings its result.
// Text typed into a password keyboard reaches the model as a secret:// handle only.
import { randomUUID } from "node:crypto";
import { actionOf, createSurface, functionCallEvent, removedSurface, surfaceSnapshot } from "./a2ui.ts";
import { deviceTools, intentProblem, intentTools, toA2UI, toFunctionCalls } from "./intent-tools.ts";
import { type Complete, type ModelConfig, openAICompatible } from "./model.ts";
import { PS5_PROMPT } from "./prompts.ts";
import { type AgentTool, FOLLOW_UPS, type ToolContext } from "./tools.ts";

type Json = Record<string, any>;
export type Emit = (event: Json) => void;

export interface RunAgentInput {
  threadId: string;
  runId?: string;
  messages?: { id?: string; role: string; content?: string; toolCallId?: string }[];
  /** Client tools (AG-UI): the client runs them. One named like one of the agent's tools replaces it. */
  tools?: { name: string; description?: string; parameters?: Json }[];
  forwardedProps?: Json;
}

export interface AgentOptions {
  /** The model: settings for an OpenAI-compatible endpoint, or your own `Complete`. */
  model: ModelConfig | Complete;
  /** The agent's own tools, run here. The intent and device tools are always there. */
  tools?: AgentTool[];
  /** The system prompt (PS5_PROMPT by default: the couch rules, and how to use the screens). */
  system?: string;
  /** Model calls allowed in one run before giving up (6 by default). */
  maxCallsPerRun?: number;
}

interface PendingApproval { toolCallId: string; tool: AgentTool; args: Json; surfaceId: string }

export class AgentThread {
  readonly id: string;
  running = false;

  private readonly complete: Complete;
  private readonly tools: AgentTool[];
  private readonly toolDefs: Json[];
  private readonly maxCalls: number;
  private readonly history: Json[];
  private readonly secrets = new Map<string, string>();
  /** The intent call behind each surface shown, to know a password keyboard when its answer comes. */
  private readonly surfaces = new Map<string, { tool: string; args: Json }>();
  private readonly seenMessages = new Set<string>();
  private pending: PendingApproval | null = null;
  /** This run's client tools, by name. */
  private clientTools = new Map<string, Json>();
  /** Client tool calls the run ended on: their results come with the next run. */
  private readonly clientCalls = new Set<string>();
  /** A client that said no controller is connected (forwardedProps.ps5Client): no controller tools. */
  private noController = false;

  constructor(id: string, options: AgentOptions) {
    this.id = id;
    this.complete = typeof options.model === "function" ? options.model : openAICompatible(options.model);
    this.tools = options.tools ?? [];
    this.maxCalls = options.maxCallsPerRun ?? 6;
    this.history = [{ role: "system", content: options.system ?? PS5_PROMPT }];
    const own = this.tools.map((t) => ({ name: t.name, description: t.description, parameters: t.parameters ?? { type: "object", properties: {} } }));
    const followUps = { name: FOLLOW_UPS.tool, description: FOLLOW_UPS.description, parameters: FOLLOW_UPS.parameters };
    this.toolDefs = [...intentTools, ...deviceTools, ...own, followUps].map((t) => ({ type: "function", function: t }));
  }

  async run(input: RunAgentInput, emit: Emit): Promise<void> {
    const runId = input.runId ?? randomUUID();
    this.running = true;
    emit({ type: "RUN_STARTED", threadId: this.id, runId });
    try {
      const started = await this.accept(input, emit);
      if (!started) {
        emit({ type: "RUN_ERROR", message: "Nothing to run: send a new user message or an A2UI action." });
        return;
      }
      await this.loop(emit);
      emit({ type: "RUN_FINISHED", threadId: this.id, runId });
    } catch (e) {
      emit({ type: "RUN_ERROR", message: String((e as Error).message ?? e) });
    } finally {
      this.running = false;
    }
  }

  private context(emit: Emit, tool: string): ToolContext {
    return {
      threadId: this.id,
      secret: (value) => (typeof value === "string" && value.startsWith("secret://") ? this.secrets.get(value) ?? null : value),
      progress: (label, fraction) =>
        emit({ type: "CUSTOM", name: "status", value: { state: "working", tool, detail: label, ...(fraction === undefined ? {} : { progress: Math.max(0, Math.min(1, fraction)) }) } }),
    };
  }

  /** Take in what the client sent. Returns true to call the model, false if there was nothing. */
  private async accept(input: RunAgentInput, emit: Emit): Promise<boolean> {
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
      return true;
    }
    // Anything else: calls the client never answered get a result, so the history stays valid.
    this.settleClientCalls();
    const action = actionOf(input.forwardedProps);
    if (action) {
      // The press comes with its visible user message (CopilotKit's way); the action stands for it.
      for (const m of input.messages ?? []) if (m.role === "user" && m.id) this.seenMessages.add(m.id);
      if (this.pending && action.surfaceId === this.pending.surfaceId) {
        await this.resolveApproval(action.name === "approval_result" && action.context?.confirmed === true, emit);
        return true;
      }
      this.denyPending(emit, "the user did something else instead");
      let context = action.context ?? {};
      const info = this.surfaces.get(action.surfaceId);
      if (info?.tool === "ask_text" && info.args.kind === "password" && typeof context.value === "string") {
        // The password stays here; the model gets a handle that tools resolve (ToolContext.secret).
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
      return true;
    }
    const fresh = (input.messages ?? []).filter((m) => m.role === "user" && (!m.id || !this.seenMessages.has(m.id)));
    const last = fresh.at(-1);
    if (!last?.content) return false;
    for (const m of fresh) if (m.id) this.seenMessages.add(m.id);
    this.denyPending(emit, "the user asked something else instead");
    this.history.push({ role: "user", content: last.content });
    return true;
  }

  private settleClientCalls() {
    for (const id of this.clientCalls) this.history.push({ role: "tool", tool_call_id: id, content: JSON.stringify({ error: "no_result" }) });
    this.clientCalls.clear();
  }

  /** Run one of the agent's own tools; a throw becomes a tool error the model can read. */
  private async execute(tool: AgentTool, args: Json, emit: Emit): Promise<Json> {
    try {
      return (await tool.run(args, this.context(emit, tool.name))) ?? {};
    } catch (e) {
      return { error: "tool_failed", details: String((e as Error).message ?? e) };
    }
  }

  private async resolveApproval(yes: boolean, emit: Emit) {
    const p = this.pending!;
    this.pending = null;
    const result = yes
      ? { ...(await this.execute(p.tool, p.args, emit)), approvedByUser: true }
      : { error: "denied_by_user", next: "Acknowledge; don't call this again unless the user asks." };
    this.reply(p.toolCallId, result, emit);
    emit(removedSurface(p.surfaceId));
  }

  /** A gated tool call must get a result before the model is called again. */
  private denyPending(emit: Emit, why: string) {
    if (!this.pending) return;
    const p = this.pending;
    this.pending = null;
    this.reply(p.toolCallId, { error: "denied_by_user", reason: why }, emit);
    emit(removedSurface(p.surfaceId));
  }

  private async loop(emit: Emit) {
    for (let call = 0; call < this.maxCalls; call++) {
      emit({ type: "CUSTOM", name: "status", value: { state: "thinking" } });
      // Client tools replace the agent's own of the same name.
      const own = this.toolDefs.filter((t) => !this.clientTools.has(t.function.name) && !(this.noController && deviceTools.some((d) => d.name === t.function.name)));
      const client = [...this.clientTools.values()].map((t) => ({ type: "function", function: { name: t.name, description: t.description ?? "", parameters: t.parameters ?? { type: "object", properties: {} } } }));
      // The reply's text streams to the client as the model writes it.
      const messageId = randomUUID();
      let streamed = false;
      let body: any;
      try {
        body = await this.complete(this.history, [...own, ...client], {
          onRetry: (waitMs, reason) => emit({ type: "CUSTOM", name: "status", value: { state: "retrying", reason, retryInSeconds: Math.round(waitMs / 1000) } }),
          onText: (delta) => {
            if (!streamed) emit({ type: "TEXT_MESSAGE_START", messageId, role: "assistant" });
            streamed = true;
            emit({ type: "TEXT_MESSAGE_CONTENT", messageId, delta });
          },
        });
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

        const tool = this.tools.find((t) => t.name === name);
        if (tool) {
          const approval = tool.approval?.(args, this.context(emit, name));
          if (approval && this.pending) {
            // One confirm at a time: a second gated call in the same reply waits for another turn.
            this.reply(tc.id, { error: "one_approval_at_a_time", retryable: true }, emit);
            continue;
          }
          if (approval) {
            // Runtime-owned confirm; the tool result is delivered when the user answers.
            const surfaceId = `approval-${tc.id}`;
            this.pending = { toolCallId: tc.id, tool, args, surfaceId };
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
          emit({ type: "CUSTOM", name: "status", value: { state: "working", tool: name } });
          this.reply(tc.id, await this.execute(tool, args, emit), emit);
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
    emit({ type: "CUSTOM", name: "status", value: { state: "gave_up", reason: `more than ${this.maxCalls} model calls in one run` } });
  }

  private reply(toolCallId: string, result: Json, emit: Emit) {
    const content = JSON.stringify(result);
    this.history.push({ role: "tool", tool_call_id: toolCallId, content });
    emit({ type: "TOOL_CALL_RESULT", messageId: randomUUID(), toolCallId, role: "tool", content });
  }
}
