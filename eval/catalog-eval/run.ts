// Spike 01: can a model drive native PS5 UI through the PS5 A2UI catalog?
//
// Mode "a2ui": exactly what CopilotKit's runtime does for A2UI (runtime-go/a2ui.go):
//   the catalog arrives as an "A2UI Component Schema" context entry, the runtime injects a
//   `render_a2ui(surfaceId, components, data)` tool plus a usage-guide context entry, and
//   BuiltInAgent appends context to the system prompt as "description:\nvalue".
// Mode "intent": flat intent tools (intent-tools.ts) that the server turns into A2UI.
//
// Usage: LLM_API_KEY=... node eval/catalog-eval/run.ts [--models a,b] [--modes a2ui,intent] [--only id,id] [--repeat n]
import { mkdirSync, writeFileSync } from "node:fs";
import { scenarios } from "./scenarios.ts";
import { intentTools, toA2UI } from "../../runtime/intent-tools.ts";
import { commonTypes, ps5Catalog, validateSurface, type Report } from "../../runtime/validate.ts";

const arg = (name: string, fallback: string) => {
  const i = process.argv.indexOf(`--${name}`);
  return i > 0 ? process.argv[i + 1] : fallback;
};
const models = arg("models", "MiniMax-M3,MiniMax-M3.1-Flash").split(",");
type Mode = "a2ui" | "a2ui-defs" | "intent";
const modes = arg("modes", "a2ui,a2ui-defs,intent").split(",") as Mode[];
const only = arg("only", "").split(",").filter(Boolean);
const repeat = Number(arg("repeat", "1"));
const BASE_URL = process.env.LLM_BASE_URL ?? "https://api.minimax.io/v1";
const KEY = process.env.LLM_API_KEY;
if (!KEY) throw new Error("set LLM_API_KEY (point LLM_BASE_URL at the provider; defaults to MiniMax direct)");

const SYSTEM =
  "You are the assistant built into a PlayStation 5, shown on a TV and operated with a DualSense controller from the couch. " +
  "There is no mouse and no physical keyboard. Answer briefly. When a screen, a choice, text entry or a confirmation helps the user, render native UI with your tools instead of describing it in text.";

// Strings first copied from CopilotKit runtime-go/a2ui.go (v0.9), moved to A2UI 1.0 spelling.
const SCHEMA_DESCRIPTION = "A2UI Component Schema — available components for generating UI surfaces. Use these component names and properties when creating A2UI operations.";
const GUIDE_DESCRIPTION = "A2UI render tool usage guide — how to call render_a2ui with valid arguments.";
const GUIDE = "Use A2UI 1.0 flat components with unique IDs and root id root. Supply surfaceId and components. Use catalog component types and required properties. Reference child IDs; do not nest components or create cycles. Data bindings use {\"@path\": \"/key\"}; function calls use {\"@call\": \"name\", \"args\": {...}}; repeated children use {componentId: id, path: /items}. Supply data for bindings.";
const RENDER_A2UI = {
  name: "render_a2ui",
  description: "Render a dynamic A2UI 1.0 surface with structured parameters.",
  parameters: { type: "object", properties: { surfaceId: { type: "string" }, components: { type: "array", items: { type: "object" } }, data: { type: "object" } }, required: ["surfaceId", "components"] },
};
const catalogValue = JSON.stringify({ catalogId: ps5Catalog.catalogId, description: ps5Catalog.description, instructions: ps5Catalog.instructions, components: ps5Catalog.components, functions: ps5Catalog.functions });
// Same catalog plus the shared A2UI type definitions its $refs point to, which the CopilotKit
// path never shows the model (refs to common_types.json stay unresolved).
const catalogValueWithDefs = JSON.stringify({
  catalogId: ps5Catalog.catalogId, description: ps5Catalog.description,
  note: "Every $ref to common_types.json#/$defs/X refers to commonTypes.X below.",
  components: ps5Catalog.components, functions: ps5Catalog.functions, commonTypes: commonTypes.$defs,
});

function request(model: string, mode: Mode, prompt: string) {
  const system =
    mode === "intent"
      ? SYSTEM
      : `${SYSTEM}\n## Context from the application\n${SCHEMA_DESCRIPTION}:\n${mode === "a2ui" ? catalogValue : catalogValueWithDefs}\n${GUIDE_DESCRIPTION}:\n${GUIDE}\n`;
  const tools = (mode === "intent" ? intentTools : [RENDER_A2UI]).map((t) => ({ type: "function", function: t }));
  return { model, messages: [{ role: "system", content: system }, { role: "user", content: prompt }], tools, tool_choice: "auto", max_tokens: 12000, ...(BASE_URL.includes("minimax") ? { reasoning_split: true } : {}) };
}

interface Call { name: string; args: unknown; parseError?: string; report?: Report }
interface Result {
  model: string; mode: string; scenario: string; run: number;
  ok: boolean; hit: boolean; verdict: string;
  calls: Call[]; text: string; ms: number;
  promptTokens?: number; completionTokens?: number; error?: string;
}

async function runOne(model: string, mode: Mode, sc: (typeof scenarios)[number], run: number): Promise<Result> {
  const started = Date.now();
  const base = { model, mode, scenario: sc.id, run };
  let body: any;
  for (let attempt = 0; ; attempt++) {
    let res: Response | undefined;
    let failure = "";
    try {
      res = await fetch(`${BASE_URL}/chat/completions`, {
        method: "POST",
        headers: { "content-type": "application/json", authorization: `Bearer ${KEY}` },
        body: JSON.stringify(request(model, mode, sc.prompt)),
        signal: AbortSignal.timeout(240_000),
      });
      body = await res.json().catch(() => ({}));
    } catch (e) {
      failure = `network: ${(e as Error).cause ?? e}`;
    }
    const transient = !res || res.status === 429 || res.status >= 500 || body?.base_resp?.status_code === 1002;
    if (res?.ok && !body?.base_resp?.status_code) break;
    if (!transient || attempt >= 4) {
      return { ...base, ok: false, hit: false, verdict: "http-error", calls: [], text: "", ms: Date.now() - started, error: failure || `${res?.status} ${JSON.stringify(body).slice(0, 300)}` };
    }
    await new Promise((r) => setTimeout(r, 2000 * (attempt + 1)));
  }
  const msg = body.choices?.[0]?.message ?? {};
  const text = String(msg.content ?? "").replace(/<think>[\s\S]*?<\/think>/g, "").trim();
  const calls: Call[] = (msg.tool_calls ?? []).map((tc: any) => {
    const name = tc.function?.name;
    let args: any;
    try { args = JSON.parse(tc.function?.arguments || "{}"); } catch (e) { return { name, args: tc.function?.arguments, parseError: String(e) }; }
    if (mode !== "intent") {
      if (name !== "render_a2ui") return { name, args, report: { ok: false, errors: [`unknown tool ${name}`], warnings: [], types: [] } };
      return { name, args, report: validateSurface(args.components, args.data ?? {}) };
    }
    if (!intentTools.some((t) => t.name === name)) return { name, args, report: { ok: false, errors: [`unknown tool ${name}`], warnings: [], types: [] } };
    try {
      const surface = toA2UI(name, args);
      return { name, args, report: validateSurface(surface.components, surface.data) };
    } catch (e) {
      return { name, args, report: { ok: false, errors: [String(e)], warnings: [], types: [] } };
    }
  });

  const expected = mode === "intent" ? sc.intent : sc.a2ui;
  const valid = calls.length > 0 && calls.every((c) => !c.parseError && c.report?.ok);
  const seen = new Set(calls.flatMap((c) => (mode === "intent" ? [c.name] : c.report?.types ?? [])));
  const hit = expected.length === 0 ? true : expected.some((t) => seen.has(t));
  let verdict: string;
  if (expected.length === 0) verdict = calls.length === 0 ? "pass" : valid ? "pass (rendered UI anyway)" : "invalid";
  else if (calls.length === 0) verdict = "no-ui";
  else if (!valid) verdict = "invalid";
  else verdict = hit ? "pass" : "valid-but-wrong-ui";
  return {
    ...base,
    ok: verdict.startsWith("pass"),
    hit,
    verdict,
    calls,
    text: text.slice(0, 400),
    ms: Date.now() - started,
    promptTokens: body.usage?.prompt_tokens,
    completionTokens: body.usage?.completion_tokens,
  };
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

const selected = scenarios.filter((s) => only.length === 0 || only.includes(s.id));
const jobs = models.flatMap((m) => modes.flatMap((mode) => selected.flatMap((sc) => Array.from({ length: repeat }, (_, r) => () => runOne(m, mode, sc, r)))));
const results = await pool(jobs, Number(process.env.CONCURRENCY ?? 4));

const stamp = new Date().toISOString().replace(/[:.]/g, "-");
const dir = new URL("./results/raw/", import.meta.url);
mkdirSync(dir, { recursive: true });
writeFileSync(new URL(`${stamp}.json`, dir), JSON.stringify(results, null, 2));

// Summary
const median = (xs: number[]) => { const s = [...xs].sort((a, b) => a - b); return s.length ? s[Math.floor(s.length / 2)] : 0; };
console.log(`\n| model | mode | pass | invalid | no UI | wrong UI | http err | median prompt tok | median completion tok | median s |`);
console.log(`|---|---|---|---|---|---|---|---|---|---|`);
for (const m of models) for (const mode of modes) {
  const rs = results.filter((r) => r.model === m && r.mode === mode);
  const n = (v: string) => rs.filter((r) => r.verdict === v).length;
  console.log(`| ${m} | ${mode} | ${rs.filter((r) => r.ok).length}/${rs.length} | ${n("invalid")} | ${n("no-ui")} | ${n("valid-but-wrong-ui")} | ${n("http-error")} | ${median(rs.map((r) => r.promptTokens ?? 0))} | ${median(rs.map((r) => r.completionTokens ?? 0))} | ${(median(rs.map((r) => r.ms)) / 1000).toFixed(1)} |`);
}
console.log(`\nPer scenario:`);
for (const r of results) {
  const errs = r.calls.flatMap((c) => [c.parseError, ...(c.report?.errors ?? [])].filter(Boolean)).slice(0, 3);
  console.log(`${r.ok ? "✓" : "✗"} ${r.model} ${r.mode} ${r.scenario}#${r.run}: ${r.verdict} [${r.calls.map((c) => (r.mode === "intent" ? c.name : (c.report?.types ?? []).join("+"))).join(", ")}]${errs.length ? ` — ${errs.join(" | ")}` : ""}${r.error ? ` — ${r.error}` : ""}`);
}
console.log(`\nraw: eval/catalog-eval/results/raw/${stamp}.json`);
