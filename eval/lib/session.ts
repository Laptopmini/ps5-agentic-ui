// The scripted-session harness the evals share (eval/hardening): one conversation of several
// tasks against the simulated console, with a scripted player answering the agent's screens.
// One run = model calls until the model shows a screen or answers in text (like CopilotKit's
// BuiltInAgent). Domain tools run within the run; intent tools become A2UI surfaces and end it;
// presses go back the CopilotKit way (log_a2ui_event). What it exercises:
//   - argument checks on intent calls, rejected calls go back as tool errors within the run;
//   - secret handles for password keyboards;
//   - an approval gate: tools marked `approval` pause the run on a runtime-made confirm, then run
//     or are denied, and the run resumes with the tool result;
//   - fault injection on screens (to exercise the retry path).
// The live agent is runtime/thread.ts; this mirrors its loop so the evals can script both sides.
import { deviceTools, intentProblem, intentTools, toA2UI, toFunctionCalls } from "../../runtime/intent-tools.ts";
import { modelFromEnv, openAICompatible } from "../../runtime/model.ts";
import { validateSurface } from "../../runtime/validate.ts";
import { type Console, domainTools, execDomain, WIFI_PASSWORD } from "../../server/console-sim.ts";

export { BASE_PROMPT, CONFIRM_RULE, GATE_RULE, HONESTY_RULE, UI_RULE } from "../../runtime/prompts.ts";

type Json = Record<string, any>;

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

// ---------- session ----------

interface Pending { kind: "surface"; name: string; surfaceId: string; components: Json[] }
interface Approval { kind: "approval"; toolCallId: string; name: string; args: Json; message: string }

export async function runSession(tasks: Task[], c: Console, opts: Options): Promise<SessionTrace> {
  const complete = openAICompatible(modelFromEnv({ model: opts.model }));
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
        const body = await complete(messages, toolDefs);
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
