// Multi-turn flows with intent tools, the CopilotKit way.
//
// Each turn is one run. The model answers with text and/or intent tools; the server turns
// intent calls into A2UI (as eval/catalog-eval) and the run ends there, as when CopilotKit renders a
// surface. Each intent call gets the tool result CopilotKit synthesizes: {"status":"rendered"}.
// A scripted player then presses a button on that surface. The action goes back the way
// CopilotKit's runtime does it (runtime-go/a2ui.go processA2UIAction): an assistant tool call
// `log_a2ui_event(<userAction>)` plus a tool result `User performed action "…" on surface "…"
// (component: …). Context: {…}`. Then the next run starts.
//
// Usage: LLM_API_KEY=... LLM_MODEL=... node eval/multiturn/run.ts [--models a,b] [--only id,id] [--repeat n]
//        LLM_BASE_URL=http://localhost:1234/v1 LLM_API_KEY=x node eval/multiturn/run.ts --models <id>
import { mkdirSync, writeFileSync } from "node:fs";
import { intentTools, toA2UI } from "../../runtime/intent-tools.ts";
import { validateSurface } from "../../runtime/validate.ts";
import { arg, BASE_URL, EXTRA_BODY, KEY, models } from "../lib/env.ts";

const only = arg("only", "").split(",").filter(Boolean);
const repeat = Number(arg("repeat", "3"));

const SYSTEM =
  "You are the assistant built into a PlayStation 5, shown on a TV and operated with a DualSense controller from the couch. " +
  "There is no mouse and no physical keyboard. Answer briefly. When a screen, a choice, text entry or a confirmation helps the user, render native UI with your tools instead of describing it in text. " +
  "After you show something the user must answer, stop and wait: their answer arrives as a log_a2ui_event result.\n" +
  "## Context from the application\n" +
  "Console state:\n" +
  JSON.stringify({
    profile: { name: "Laptopmini" },
    installedModels: [
      { id: "qwen-9b", name: "Qwen 9B", size: "5.4 GB" },
      { id: "mistral-7b", name: "Mistral 7B", size: "4.1 GB" },
      { id: "minicpm-2b", name: "MiniCPM 2B", size: "2.2 GB" },
    ],
    loadedModel: "minicpm-2b",
    saves: [{ id: "elden-ring", game: "Elden Ring", size: "12 MB" }],
    network: { connected: false },
  }) +
  "\n";

// ---------- flows ----------

type Json = Record<string, any>;
interface Call { id: string; name: string; args: Json; surfaceId: string; surface: { components: Json[]; data: Json } }
interface Turn { calls: Call[]; text: string }
interface Action { name: string; surfaceId: string; sourceComponentId: string; context: Json }
interface Step {
  /** Allowed kinds of answer: intent tool names, or "text" for a text-only reply. At least one must appear. */
  expect: string[];
  /** Extra check; return an error message or null. */
  check?: (t: Turn) => string | null;
  /** The player's response, or null to end the flow here. */
  respond?: (t: Turn) => Action | null;
  /** Run this step only if it holds for the steps so far (for flows with optional detours). */
  when?: (done: StepResult[]) => boolean;
}
interface Flow { id: string; prompt: string; steps: Step[]; minSteps: number }

const blob = (t: Turn) => JSON.stringify(t.calls.map((c) => c.args)) + " " + t.text;
const mentions = (re: RegExp, what: string) => (t: Turn) => (re.test(blob(t)) ? null : `does not mention ${what}`);
const notMentions = (re: RegExp, what: string) => (t: Turn) => (re.test(blob(t)) ? `mentions ${what}` : null);
/** The quiz question must be on screen: in the menu title or prompt, not only in chat text. */
const questionVisible = (t: Turn) => {
  const m = t.calls.find((c) => c.name === "show_menu")?.args;
  return m && /\?/.test(`${m.title ?? ""} ${m.prompt ?? ""}`) ? null : "question not on screen (no '?' in menu title or prompt)";
};
const all = (...checks: ((t: Turn) => string | null)[]) => (t: Turn) => checks.map((c) => c(t)).find(Boolean) ?? null;

/** Press a button on the first surface rendered by `tool`. */
function press(t: Turn, tool: string, pick: (c: Call) => { component: Json; action: Json; values?: Json } | null): Action | null {
  const call = t.calls.find((c) => c.name === tool);
  if (!call) return null;
  const hit = pick(call);
  if (!hit) return null;
  const ev = hit.action.event;
  // Resolve {path} bindings in the event context against what the player typed/said.
  const context = Object.fromEntries(Object.entries(ev.context ?? {}).map(([k, v]: [string, any]) => [k, v && typeof v === "object" && "path" in v ? hit.values?.[v.path] ?? null : v]));
  return { name: ev.name, surfaceId: call.surfaceId, sourceComponentId: hit.component.id, context };
}
const menuPick = (t: Turn, choose: (labels: string[]) => number) =>
  press(t, "show_menu", (c) => {
    const items = c.surface.components.filter((x) => x.component === "MenuItem");
    if (!items.length) return null;
    const i = Math.max(0, Math.min(items.length - 1, choose(items.map((x) => String(x.label)))));
    return { component: items[i], action: items[i].action };
  });
const confirmAnswer = (t: Turn, yes: boolean) =>
  press(t, "confirm", (c) => {
    const d = c.surface.components[0];
    return { component: d, action: yes ? d.confirm : d.cancel };
  });
const typeText = (t: Turn, value: string) =>
  press(t, "ask_text", (c) => ({ component: c.surface.components[0], action: c.surface.components[0].submit, values: { "/answer": value } }));
const speak = (t: Turn, transcript: string) =>
  press(t, "ask_voice", (c) => ({ component: c.surface.components[0], action: c.surface.components[0].submit, values: { "/transcript": transcript } }));

const flows: Flow[] = [
  {
    id: "load-model",
    prompt: "I want to switch to a different model.",
    minSteps: 2,
    steps: [
      { expect: ["show_menu"], check: mentions(/qwen/i, "Qwen"), respond: (t) => menuPick(t, (ls) => ls.findIndex((l) => /qwen/i.test(l))) },
      {
        expect: ["confirm", "show_progress", "notify", "show_info", "text"],
        check: mentions(/qwen/i, "the picked model"),
        respond: (t) => confirmAnswer(t, true),
      },
      { expect: ["show_progress", "notify", "show_info", "text"], check: mentions(/qwen/i, "the picked model") },
    ],
  },
  {
    id: "delete-then-cancel",
    prompt: "Delete my Elden Ring save data.",
    minSteps: 2,
    steps: [
      { expect: ["confirm"], check: (t) => (t.calls.find((c) => c.name === "confirm")?.args.destructive === true ? null : "confirm not marked destructive"), respond: (t) => confirmAnswer(t, false) },
      {
        expect: ["notify", "text", "show_info"],
        check: all(
          (t) => (t.calls.some((c) => c.name === "confirm") ? "asked again after cancel" : null),
          mentions(/cancel|kept|keep|won'?t|not (been )?deleted|safe|untouched|still/i, "that the save was kept"),
        ),
      },
    ],
  },
  {
    id: "rename-profile",
    prompt: "Rename my profile.",
    minSteps: 2,
    steps: [
      { expect: ["ask_text"], respond: (t) => typeText(t, "Valentin") },
      { expect: ["notify", "show_info", "text", "confirm"], check: mentions(/Valentin/, "the new name") },
    ],
  },
  {
    id: "wifi-password-privacy",
    prompt: "Connect me to my Wi-Fi network Mini-5G.",
    minSteps: 2,
    steps: [
      // Either straight to the password keyboard, or a network list first (both are console-like).
      {
        expect: ["ask_text", "show_menu"],
        check: (t) => (t.calls.some((c) => c.name === "ask_text") && t.calls.find((c) => c.name === "ask_text")?.args.kind !== "password" ? "keyboard not in password mode" : null),
        respond: (t) => (t.calls.some((c) => c.name === "ask_text") ? typeText(t, "hunter2-SECRET") : menuPick(t, (ls) => ls.findIndex((l) => /mini-5g/i.test(l)))),
      },
      {
        when: (done) => done.at(-1)?.kinds.includes("show_menu") ?? false,
        expect: ["ask_text"],
        check: (t) => (t.calls.find((c) => c.name === "ask_text")?.args.kind === "password" ? null : "keyboard not in password mode"),
        respond: (t) => typeText(t, "hunter2-SECRET"),
      },
      { expect: ["notify", "show_progress", "show_info", "text"], check: notMentions(/hunter2/i, "the password") },
    ],
  },
  {
    id: "quiz-two-questions",
    prompt: "Quiz me with two multiple-choice questions about PlayStation history, one at a time, then tell me my score.",
    minSteps: 3,
    steps: [
      { expect: ["show_menu"], check: all((t) => (t.calls.filter((c) => c.name === "show_menu").length === 1 ? null : "not exactly one question"), questionVisible), respond: (t) => menuPick(t, () => 0) },
      { expect: ["show_menu"], check: all((t) => (t.calls.filter((c) => c.name === "show_menu").length === 1 ? null : "not exactly one question"), questionVisible), respond: (t) => menuPick(t, (ls) => ls.length - 1) },
      { expect: ["show_info", "notify", "text"], check: all(mentions(/score|\b[0-2]\s*(\/|out of|of)\s*2\b/i, "a score"), (t) => (t.calls.some((c) => c.name === "show_menu") ? "asked a third question" : null)) },
    ],
  },
  {
    id: "voice-question",
    prompt: "I'd rather ask my question by voice.",
    minSteps: 2,
    steps: [
      { expect: ["ask_voice"], respond: (t) => speak(t, "Which PS5 game should I play next if I loved Hades?") },
      { expect: ["show_shelf", "show_menu", "show_info", "text"], check: notMentions(/ask_voice|hold r2/i, "a second voice prompt") },
    ],
  },
];

// ---------- model calls ----------

async function complete(model: string, messages: Json[]) {
  for (let attempt = 0; ; attempt++) {
    let res: Response | undefined;
    let body: any;
    try {
      res = await fetch(`${BASE_URL}/chat/completions`, {
        method: "POST",
        headers: { "content-type": "application/json", authorization: `Bearer ${KEY}` },
        body: JSON.stringify({
          model,
          messages,
          tools: intentTools.map((t) => ({ type: "function", function: t })),
          tool_choice: "auto",
          max_tokens: 8000,
          ...EXTRA_BODY,
        }),
        signal: AbortSignal.timeout(300_000),
      });
      body = await res.json().catch(() => ({}));
    } catch (e) {
      body = { error: String((e as Error).cause ?? e) };
    }
    if (res?.ok && !body?.base_resp?.status_code) return body;
    const transient = !res || res.status === 429 || res.status >= 500 || body?.base_resp?.status_code === 1002;
    if (!transient || attempt >= 4) throw new Error(`${res?.status ?? "network"} ${JSON.stringify(body).slice(0, 300)}`);
    await new Promise((r) => setTimeout(r, 2000 * (attempt + 1)));
  }
}

interface StepResult { step: number; ok: boolean; kinds: string[]; errors: string[]; text: string; calls: { name: string; args: Json }[]; action?: Action; promptTokens?: number; completionTokens?: number; ms: number }
interface FlowResult { model: string; flow: string; run: number; ok: boolean; steps: StepResult[]; error?: string }

async function runFlow(model: string, flow: Flow, run: number): Promise<FlowResult> {
  const messages: Json[] = [{ role: "system", content: SYSTEM }, { role: "user", content: flow.prompt }];
  const steps: StepResult[] = [];
  for (const [i, step] of flow.steps.entries()) {
    if (step.when && !step.when(steps)) continue;
    const started = Date.now();
    let body: any;
    try {
      body = await complete(model, messages);
    } catch (e) {
      return { model, flow: flow.id, run, ok: false, steps, error: String(e) };
    }
    const msg = body.choices?.[0]?.message ?? {};
    const text = String(msg.content ?? "").replace(/<think>[\s\S]*?<\/think>/g, "").trim();
    const errors: string[] = [];
    const calls: Call[] = [];
    for (const tc of msg.tool_calls ?? []) {
      let args: Json = {};
      try { args = JSON.parse(tc.function?.arguments || "{}"); } catch { errors.push(`${tc.function?.name}: arguments are not JSON`); continue; }
      const name = tc.function?.name;
      if (!intentTools.some((t) => t.name === name)) { errors.push(`unknown tool ${name}`); continue; }
      const surface = toA2UI(name, args);
      const report = validateSurface(surface.components, surface.data);
      if (!report.ok) errors.push(`${name}: ${report.errors.slice(0, 2).join("; ")}`);
      calls.push({ id: tc.id, name, args, surfaceId: `surface-${tc.id}`, surface });
    }
    const turn: Turn = { calls, text };
    const kinds = calls.length ? calls.map((c) => c.name) : text ? ["text"] : [];
    if (!kinds.some((k) => step.expect.includes(k))) errors.push(`expected ${step.expect.join("|")}, got ${kinds.join("+") || "nothing"}`);
    const extra = step.check?.(turn);
    if (extra) errors.push(extra);

    // The run ends: assistant message as returned, then CopilotKit's synthetic result per rendered surface.
    messages.push({ role: "assistant", content: msg.content ?? "", ...(msg.tool_calls?.length ? { tool_calls: msg.tool_calls } : {}) });
    for (const tc of msg.tool_calls ?? []) messages.push({ role: "tool", tool_call_id: tc.id, content: '{"status":"rendered"}' });

    const isLast = i === flow.steps.length - 1;
    const action = !isLast && errors.length === 0 ? step.respond?.(turn) ?? null : null;
    steps.push({ step: i + 1, ok: errors.length === 0, kinds, errors, text: text.slice(0, 300), calls: calls.map((c) => ({ name: c.name, args: c.args })), action: action ?? undefined, promptTokens: body.usage?.prompt_tokens, completionTokens: body.usage?.completion_tokens, ms: Date.now() - started });
    if (errors.length || isLast || !action) break;

    // The player's press, as CopilotKit's runtime injects it.
    const logId = `call_a2ui_${run}_${i}`;
    messages.push({ role: "assistant", content: "", tool_calls: [{ id: logId, type: "function", function: { name: "log_a2ui_event", arguments: JSON.stringify(action) } }] });
    messages.push({ role: "tool", tool_call_id: logId, content: `User performed action "${action.name}" on surface "${action.surfaceId}" (component: ${action.sourceComponentId}). Context: ${JSON.stringify(action.context)}` });
  }
  const ok = steps.length >= flow.minSteps && steps.every((s) => s.ok);
  return { model, flow: flow.id, run, ok, steps };
}

async function pool<T>(jobs: (() => Promise<T>)[], size: number): Promise<T[]> {
  const out: T[] = new Array(jobs.length);
  let next = 0;
  await Promise.all(Array.from({ length: size }, async () => {
    while (next < jobs.length) { const i = next++; out[i] = await jobs[i](); process.stderr.write("."); }
  }));
  process.stderr.write("\n");
  return out;
}

const selected = flows.filter((f) => only.length === 0 || only.includes(f.id));
const results = await pool(models.flatMap((m) => selected.flatMap((f) => Array.from({ length: repeat }, (_, r) => () => runFlow(m, f, r)))), Number(process.env.CONCURRENCY ?? 4));

const stamp = new Date().toISOString().replace(/[:.]/g, "-");
const dir = new URL("./results/raw/", import.meta.url);
mkdirSync(dir, { recursive: true });
writeFileSync(new URL(`${stamp}.json`, dir), JSON.stringify(results, null, 2));

console.log(`\n| model | flows passed | steps passed | median step s | median completion tok |\n|---|---|---|---|---|`);
const median = (xs: number[]) => { const s = [...xs].sort((a, b) => a - b); return s.length ? s[Math.floor(s.length / 2)] : 0; };
for (const m of models) {
  const rs = results.filter((r) => r.model === m);
  const st = rs.flatMap((r) => r.steps);
  console.log(`| ${m} | ${rs.filter((r) => r.ok).length}/${rs.length} | ${st.filter((s) => s.ok).length}/${st.length} | ${(median(st.map((s) => s.ms)) / 1000).toFixed(1)} | ${median(st.map((s) => s.completionTokens ?? 0))} |`);
}
console.log("\nPer flow:");
for (const r of results) {
  const path = r.steps.map((s) => `${s.ok ? "" : "✗"}${s.kinds.join("+") || "∅"}`).join(" → ");
  const errs = r.steps.flatMap((s) => s.errors.map((e) => `step ${s.step}: ${e}`));
  console.log(`${r.ok ? "✓" : "✗"} ${r.model} ${r.flow}#${r.run}: ${path}${errs.length ? ` — ${errs.join(" | ")}` : ""}${r.error ? ` — ERROR ${r.error}` : ""}`);
}
console.log(`\nraw: eval/multiturn/results/raw/${stamp}.json`);
