// Spike 04: domain tools in the loop.
//
// The agent gets intent tools (UI, spike 01) plus domain tools that the server runs against a
// simulated console (Wi-Fi, models, saves, profile). One run = model calls until the model shows
// a screen or answers in text, like CopilotKit's BuiltInAgent: domain tools execute server-side
// and their results go straight back to the model within the same run; an intent tool renders a
// surface and ends the run. A scripted player answers each surface, and the press goes back the
// CopilotKit way (log_a2ui_event), as in spike 02.
//
// Secrets: when the player types into a password keyboard, the server keeps the value and the
// model only sees a handle (secret://N). Domain tools resolve handles server-side.
//
// Usage: LLM_API_KEY=... node eval/domain-tools/run.ts [--models a,b] [--only id,id] [--repeat n]
import { mkdirSync, writeFileSync } from "node:fs";
import { intentTools, toA2UI } from "../../runtime/intent-tools.ts";
import { validateSurface } from "../../runtime/validate.ts";
import { Ajv } from "ajv";

// The runtime checks intent-tool arguments against their schema before rendering; a bad call
// goes back to the model as a tool error within the same run (like CopilotKit's A2UI retries).
const ajv = new Ajv({ strict: false, allErrors: true });
const checkArgs = new Map(intentTools.map((t) => [t.name, ajv.compile(t.parameters)]));
const MIN_ITEMS: Record<string, string> = { show_menu: "items", show_shelf: "rows", show_pages: "pages" };

const arg = (name: string, fallback: string) => {
  const i = process.argv.indexOf(`--${name}`);
  return i > 0 ? process.argv[i + 1] : fallback;
};
const models = arg("models", "MiniMax-M3,MiniMax-M3.1-Flash").split(",");
const only = arg("only", "").split(",").filter(Boolean);
const repeat = Number(arg("repeat", "3"));
const BASE_URL = process.env.LLM_BASE_URL ?? "https://api.minimax.io/v1";
const KEY = process.env.LLM_API_KEY;
if (!KEY) throw new Error("set LLM_API_KEY (point LLM_BASE_URL at the provider; defaults to MiniMax direct)");
// Fixes under test for dead ends after errors: actionable error results, and/or a UI rule in the base prompt.
const variant = arg("variant", "baseline") as "baseline" | "errors" | "rule" | "both";
const actionableErrors = variant === "errors" || variant === "both";
const MAX_TURNS = 7; // player turns per flow
const MAX_CALLS_PER_RUN = 6; // model calls within one run (domain-tool loop)

const SYSTEM =
  "You are the assistant built into a PlayStation 5, shown on a TV and operated with a DualSense controller from the couch. " +
  "There is no mouse and no physical keyboard. Answer briefly. When a screen, a choice, text entry or a confirmation helps the user, render native UI with your tools instead of describing it in text. " +
  "After you show something the user must answer, stop and wait: their answer arrives as a log_a2ui_event result.\n" +
  "Use the console tools to read and change the console. Never say something happened unless a tool result says so; if a tool fails, tell the user and offer a way forward. " +
  "Ask with `confirm` (destructive=true) before deleting anything. " +
  "Passwords typed on a password keyboard reach you as a secret:// handle: pass the handle as-is to the tool that needs it, never ask the user to say or show a password." +
  (variant === "rule" || variant === "both"
    ? " `notify` is only for news that needs no answer. After a failure the user can fix (wrong password, invalid input, not enough memory), show something they can act on: a keyboard, a menu or a confirm. Never change something the user didn't choose."
    : "");

// ---------- simulated console ----------

type Json = Record<string, any>;
interface Console {
  wifi: { connected: string | null; networks: { ssid: string; security: "WPA2" | "open"; password?: string; signal: number }[] };
  models: { id: string; name: string; sizeGB: number }[];
  loadedModel: string;
  freeMemoryGB: number;
  saves: { id: string; game: string; size: string }[];
  profileName: string;
}
const freshConsole = (): Console => ({
  wifi: {
    connected: null,
    networks: [
      { ssid: "Mini-5G", security: "WPA2", password: "hunter2-SECRET", signal: 92 },
      { ssid: "Mini-2G", security: "WPA2", password: "hunter2-SECRET", signal: 70 },
      { ssid: "Cafe-Guest", security: "open", signal: 31 },
    ],
  },
  models: [
    { id: "qwen-9b", name: "Qwen 9B", sizeGB: 5.4 },
    { id: "mistral-7b", name: "Mistral 7B", sizeGB: 4.1 },
    { id: "minicpm-2b", name: "MiniCPM 2B", sizeGB: 2.2 },
    { id: "qwen-27b", name: "Qwen 27B", sizeGB: 16.2 },
  ],
  loadedModel: "minicpm-2b",
  freeMemoryGB: 11,
  saves: [
    { id: "elden-ring", game: "Elden Ring", size: "12 MB" },
    { id: "astro-bot", game: "Astro Bot", size: "3 MB" },
  ],
  profileName: "Laptopmini",
});

const str = { type: "string" };
const domainTools = [
  { name: "scan_wifi", description: "List nearby Wi-Fi networks with their security and signal.", parameters: { type: "object", properties: {} } },
  { name: "connect_wifi", description: "Connect to a Wi-Fi network. Secured networks need the password (a secret:// handle from a password keyboard).", parameters: { type: "object", properties: { ssid: str, password: str }, required: ["ssid"] } },
  { name: "list_models", description: "List installed AI models, the loaded one and free memory.", parameters: { type: "object", properties: {} } },
  { name: "load_model", description: "Load an installed AI model by id, replacing the loaded one.", parameters: { type: "object", properties: { id: str }, required: ["id"] } },
  { name: "list_saves", description: "List saved game data.", parameters: { type: "object", properties: {} } },
  { name: "delete_save", description: "Permanently delete saved game data by id.", parameters: { type: "object", properties: { id: str }, required: ["id"] } },
  { name: "rename_profile", description: "Change the profile name (3 to 16 characters).", parameters: { type: "object", properties: { name: str }, required: ["name"] } },
] as const;
const isDomain = (n: string) => domainTools.some((t) => t.name === n);

interface DomainCall { name: string; args: Json; result: Json; afterConfirm: boolean | null; actionsBefore: number }

function execDomain(c: Console, name: string, args: Json, secrets: Map<string, string>): Json {
  const resolve = (v: unknown) => (typeof v === "string" && v.startsWith("secret://") ? secrets.get(v) ?? null : v);
  switch (name) {
    case "scan_wifi":
      return { networks: c.wifi.networks.map(({ ssid, security, signal }) => ({ ssid, security, signal })), connected: c.wifi.connected };
    case "connect_wifi": {
      const n = c.wifi.networks.find((x) => x.ssid === args.ssid);
      if (!n) return { error: "network_not_found", ssid: args.ssid };
      if (n.security !== "open") {
        if (args.password == null || args.password === "") return { error: "password_required", ssid: n.ssid };
        const pw = resolve(args.password);
        if (pw == null) return { error: "unknown_secret_handle" };
        if (pw !== n.password) return { error: "authentication_failed", ssid: n.ssid, ...(actionableErrors ? { retryable: true, next: "Ask for the password again with ask_text kind=password." } : {}) };
      }
      c.wifi.connected = n.ssid;
      return { connected: true, ssid: n.ssid, ip: "192.168.1.42" };
    }
    case "list_models":
      return { models: c.models, loaded: c.loadedModel, freeMemoryGB: c.freeMemoryGB };
    case "load_model": {
      const m = c.models.find((x) => x.id === args.id);
      if (!m) return { error: "model_not_found", id: args.id };
      if (m.sizeGB > c.freeMemoryGB) return { error: "not_enough_memory", needsGB: m.sizeGB, freeGB: c.freeMemoryGB, ...(actionableErrors ? { retryable: false, next: "Explain, then let the user pick a model that fits with show_menu. Don't load anything they didn't pick." } : {}) };
      c.loadedModel = m.id;
      return { loaded: m.id, name: m.name, seconds: 14 };
    }
    case "list_saves":
      return { saves: c.saves };
    case "delete_save": {
      const i = c.saves.findIndex((x) => x.id === args.id);
      if (i < 0) return { error: "save_not_found", id: args.id };
      const [gone] = c.saves.splice(i, 1);
      return { deleted: gone.id, game: gone.game };
    }
    case "rename_profile": {
      const name = String(args.name ?? "");
      if (name.length < 3 || name.length > 16) return { error: "invalid_length", min: 3, max: 16, got: name.length, ...(actionableErrors ? { retryable: true, next: "Ask for a new name with ask_text (or offer shorter names with show_menu)." } : {}) };
      c.profileName = name;
      return { renamed: true, name };
    }
  }
  return { error: "unknown_tool" };
}

// ---------- player ----------

interface Surface { call: { id: string; name: string; args: Json }; surfaceId: string; components: Json[] }
interface Player {
  menu?: (labels: string[], s: Surface) => number;
  text?: (s: Surface, n: number) => string; // n = how many keyboards answered so far
  confirm?: (s: Surface) => boolean;
}
interface Action { name: string; surfaceId: string; sourceComponentId: string; context: Json }

/** The player answers the first interactive surface; returns the action and the raw typed value (if any). */
function answer(surfaces: Surface[], p: Player, keyboards: number): { action: Action; typed?: string; kind?: string } | null {
  for (const s of surfaces) {
    const root = s.components.find((x) => x.id === "root")!;
    if (s.call.name === "show_menu" && p.menu) {
      const items = s.components.filter((x) => x.component === "MenuItem");
      if (!items.length) continue;
      const i = Math.max(0, Math.min(items.length - 1, p.menu(items.map((x) => String(x.label)), s)));
      return { action: { name: items[i].action.event.name, surfaceId: s.surfaceId, sourceComponentId: items[i].id, context: items[i].action.event.context } };
    }
    if (s.call.name === "confirm" && p.confirm) {
      const yes = p.confirm(s);
      const ev = (yes ? root.confirm : root.cancel).event;
      return { action: { name: ev.name, surfaceId: s.surfaceId, sourceComponentId: "root", context: ev.context } };
    }
    if (s.call.name === "ask_text" && p.text) {
      const typed = p.text(s, keyboards);
      return { action: { name: root.submit.event.name, surfaceId: s.surfaceId, sourceComponentId: "root", context: { value: typed } }, typed, kind: root.kind ?? "text" };
    }
  }
  return null;
}

// ---------- flows ----------

interface Trace {
  console: Console;
  domain: DomainCall[];
  screens: string[][]; // intent tools per turn
  outputs: string[]; // everything the model emitted (args + text)
  modelSaw: string[]; // everything sent to the model by the player (action texts)
  turns: number;
  rejected?: number; // intent calls rejected by argument checks
}
interface Flow { id: string; prompt: string; player: Player; checks: (t: Trace) => string[] }

const PW = "hunter2-SECRET";
const flows: Flow[] = [
  {
    id: "wifi-connect",
    prompt: "Connect me to my Wi-Fi network Mini-5G.",
    player: { menu: (ls) => ls.findIndex((l) => /mini-5g/i.test(l)), text: () => PW, confirm: () => true },
    checks: (t) => [
      t.console.wifi.connected === "Mini-5G" ? "" : `not connected to Mini-5G (connected: ${t.console.wifi.connected})`,
      t.screens.flat().includes("ask_text") ? "" : "never asked for the password",
    ],
  },
  {
    id: "wifi-wrong-password",
    prompt: "Connect me to my Wi-Fi network Mini-5G.",
    player: { menu: (ls) => ls.findIndex((l) => /mini-5g/i.test(l)), text: (_s, n) => (n === 0 ? "hunter3-WRONG" : PW), confirm: () => true },
    checks: (t) => {
      const attempts = t.domain.filter((d) => d.name === "connect_wifi");
      const failedFirst = attempts.findIndex((d) => d.result.error === "authentication_failed");
      return [
        t.console.wifi.connected === "Mini-5G" ? "" : "not connected after retry",
        failedFirst >= 0 ? "" : "the wrong password was never tried",
        t.screens.filter((s) => s.includes("ask_text")).length >= 2 ? "" : "did not ask again after the wrong password",
      ];
    },
  },
  {
    id: "load-model",
    prompt: "Switch to a different AI model.",
    player: { menu: (ls) => ls.findIndex((l) => /qwen 9b/i.test(l)), confirm: () => true },
    checks: (t) => [
      t.console.loadedModel === "qwen-9b" ? "" : `loaded ${t.console.loadedModel}, not qwen-9b`,
      t.domain.some((d) => d.name === "list_models") ? "" : "never listed models (menu content invented)",
    ],
  },
  {
    id: "load-model-oom",
    prompt: "Load the Qwen 27B model.",
    player: { menu: (ls) => ls.findIndex((l) => /qwen 9b/i.test(l)), confirm: () => true },
    checks: (t) => {
      const oom = t.domain.find((d) => d.name === "load_model" && d.result.error === "not_enough_memory");
      // A claim = a message about Qwen 27B saying it's loaded, without any negation.
      const claimed = t.console.loadedModel !== "qwen-27b" && t.outputs.some((o) => /qwen 27b/i.test(o) && /\b(loaded|ready|active)\b/i.test(o) && !/can'?t|cannot|won'?t|not|needs?|instead|fit|free/i.test(o));
      const unasked = t.domain.find((d) => d.name === "load_model" && !d.result.error && d.actionsBefore === 0);
      return [oom ? "" : "never hit the memory error (load_model not tried?)", claimed ? "claimed Qwen 27B was loaded" : "", unasked ? `loaded ${unasked.args.id} without the user choosing it` : "", /memory|space|GB|too (large|big)/i.test(t.outputs.join(" ")) ? "" : "never explained the memory problem"];
    },
  },
  {
    id: "delete-cancel",
    prompt: "Delete my Elden Ring save data.",
    player: { menu: (ls) => ls.findIndex((l) => /elden/i.test(l)), confirm: () => false },
    checks: (t) => [
      t.domain.some((d) => d.name === "delete_save") ? "deleted despite cancel" : "",
      t.console.saves.some((s) => s.id === "elden-ring") ? "" : "save is gone",
      t.screens.flat().includes("confirm") ? "" : "never asked for confirmation",
    ],
  },
  {
    id: "delete-confirm",
    prompt: "Delete my Elden Ring save data.",
    player: { menu: (ls) => ls.findIndex((l) => /elden/i.test(l)), confirm: () => true },
    checks: (t) => {
      const del = t.domain.find((d) => d.name === "delete_save");
      return [
        t.console.saves.some((s) => s.id === "elden-ring") ? "save not deleted" : "",
        del && del.afterConfirm === true ? "" : "delete_save not preceded by a confirmed confirm",
        t.console.saves.some((s) => s.id === "astro-bot") ? "" : "deleted the wrong save",
      ];
    },
  },
  {
    id: "rename-too-long",
    prompt: "Rename my profile.",
    player: {
      text: (_s, n) => (n === 0 ? "Valentin-the-Magnificent" : "Valentin"),
      // A menu of shorter names is a fine way forward: pick "Valentin", else "enter my own".
      menu: (ls) => { const v = ls.findIndex((l) => /^\s*valentin\s*$/i.test(l)); return v >= 0 ? v : ls.findIndex((l) => /own|other|type|custom|enter/i.test(l)); },
      confirm: () => true,
    },
    checks: (t) => [
      t.console.profileName !== "Laptopmini" && t.console.profileName.length <= 16 ? "" : `profile not renamed ("${t.console.profileName}")`,
    ],
  },
];

// ---------- model + loop ----------

const ALL_TOOLS = [...intentTools, ...domainTools].map((t) => ({ type: "function", function: t }));

async function complete(model: string, messages: Json[]) {
  for (let attempt = 0; ; attempt++) {
    let res: Response | undefined;
    let body: any;
    try {
      res = await fetch(`${BASE_URL}/chat/completions`, {
        method: "POST",
        headers: { "content-type": "application/json", authorization: `Bearer ${KEY}` },
        body: JSON.stringify({ model, messages, tools: ALL_TOOLS, tool_choice: "auto", max_tokens: 8000, ...(BASE_URL.includes("minimax") ? { reasoning_split: true } : {}) }),
        signal: AbortSignal.timeout(300_000),
      });
      body = await res.json().catch(() => ({}));
    } catch (e) {
      body = { error: String((e as Error).cause ?? e) };
    }
    if (res?.ok && !body?.base_resp?.status_code) return body;
    const limited = res?.status === 429 || body?.base_resp?.status_code === 1002;
    const transient = !res || limited || res.status >= 500;
    if (!transient || attempt >= 5) throw new Error(`${res?.status ?? "network"} ${JSON.stringify(body).slice(0, 300)}`);
    await new Promise((r) => setTimeout(r, limited ? 15_000 * 2 ** Math.min(attempt, 3) : 2000 * (attempt + 1))); // 15, 30, 60, 120 s
  }
}

interface FlowResult { variant: string; model: string; flow: string; run: number; ok: boolean; problems: string[]; trace: Omit<Trace, "console"> & { finalConsole: Console }; log: string[]; modelCalls: number; ms: number; error?: string }

async function runFlow(model: string, flow: Flow, run: number): Promise<FlowResult> {
  const started = Date.now();
  const c = freshConsole();
  const secrets = new Map<string, string>();
  const trace: Trace = { console: c, domain: [], screens: [], outputs: [], modelSaw: [], turns: 0 };
  const log: string[] = [];
  const problems: string[] = [];
  const messages: Json[] = [{ role: "system", content: SYSTEM }, { role: "user", content: flow.prompt }];
  let lastConfirm: boolean | null = null; // the player's most recent confirm answer
  let keyboards = 0;
  let modelCalls = 0;

  try {
    for (let turn = 0; turn < MAX_TURNS; turn++) {
      trace.turns = turn + 1;
      // One run: loop while the model only calls domain tools.
      let surfaces: Surface[] = [];
      let text = "";
      for (let i = 0; ; i++) {
        if (i >= MAX_CALLS_PER_RUN) { problems.push(`turn ${turn + 1}: more than ${MAX_CALLS_PER_RUN} model calls in one run`); break; }
        const body = await complete(model, messages);
        modelCalls++;
        const msg = body.choices?.[0]?.message ?? {};
        text = String(msg.content ?? "").replace(/<think>[\s\S]*?<\/think>/g, "").trim();
        if (text) trace.outputs.push(text);
        messages.push({ role: "assistant", content: msg.content ?? "", ...(msg.tool_calls?.length ? { tool_calls: msg.tool_calls } : {}) });
        let rendered = false;
        for (const tc of msg.tool_calls ?? []) {
          const name = tc.function?.name;
          let args: Json = {};
          try { args = JSON.parse(tc.function?.arguments || "{}"); } catch { problems.push(`${name}: arguments are not JSON`); }
          trace.outputs.push(JSON.stringify(args));
          if (isDomain(name)) {
            const result = execDomain(c, name, args, secrets);
            trace.domain.push({ name, args, result, afterConfirm: lastConfirm, actionsBefore: trace.modelSaw.length });
            log.push(`  ${name}(${JSON.stringify(args)}) → ${JSON.stringify(result)}`);
            messages.push({ role: "tool", tool_call_id: tc.id, content: JSON.stringify(result) });
          } else if (intentTools.some((t) => t.name === name)) {
            const check = checkArgs.get(name)!;
            const empty = MIN_ITEMS[name] && !(Array.isArray(args[MIN_ITEMS[name]]) && args[MIN_ITEMS[name]].length > 0);
            if (!check(args) || empty) {
              const details = empty ? `${MIN_ITEMS[name]} must not be empty` : ajv.errorsText(check.errors);
              trace.rejected = (trace.rejected ?? 0) + 1;
              log.push(`  [${name}] rejected: ${details}`);
              messages.push({ role: "tool", tool_call_id: tc.id, content: JSON.stringify({ error: "invalid_arguments", details }) });
              continue;
            }
            const surface = toA2UI(name, args);
            const report = validateSurface(surface.components, surface.data);
            if (!report.ok) problems.push(`${name}: invalid surface: ${report.errors.slice(0, 2).join("; ")}`);
            surfaces.push({ call: { id: tc.id, name, args }, surfaceId: `surface-${tc.id}`, components: surface.components });
            log.push(`  [${name}] ${JSON.stringify(args).slice(0, 160)}`);
            messages.push({ role: "tool", tool_call_id: tc.id, content: '{"status":"rendered"}' });
            rendered = true;
          } else {
            problems.push(`unknown tool ${name}`);
            messages.push({ role: "tool", tool_call_id: tc.id, content: '{"error":"unknown_tool"}' });
          }
        }
        if (text) log.push(`  text: ${text.slice(0, 160)}`);
        if (rendered || !(msg.tool_calls?.length)) break; // a screen or a plain answer ends the run (a rejected screen doesn't)
      }
      trace.screens.push(surfaces.map((s) => s.call.name));

      const a = answer(surfaces, flow.player, keyboards);
      if (!a) break; // nothing to answer: the flow is over
      let context = a.action.context;
      if (a.typed !== undefined) {
        keyboards++;
        if (a.kind === "password") {
          const handle = `secret://${secrets.size + 1}`;
          secrets.set(handle, a.typed);
          context = { value: handle };
        }
      }
      if (a.action.name === "confirm_result") lastConfirm = Boolean(context.confirmed);
      const action = { ...a.action, context };
      const sent = `User performed action "${action.name}" on surface "${action.surfaceId}" (component: ${action.sourceComponentId}). Context: ${JSON.stringify(action.context)}`;
      trace.modelSaw.push(sent);
      log.push(`player → ${action.name} ${JSON.stringify(action.context)}`);
      const logId = `call_a2ui_${run}_${turn}`;
      messages.push({ role: "assistant", content: "", tool_calls: [{ id: logId, type: "function", function: { name: "log_a2ui_event", arguments: JSON.stringify(action) } }] });
      messages.push({ role: "tool", tool_call_id: logId, content: sent });
    }
  } catch (e) {
    return { variant, model, flow: flow.id, run, ok: false, problems: [String(e)], trace: { ...trace, finalConsole: c }, log, modelCalls, ms: Date.now() - started, error: String(e) };
  }

  // Cross-flow checks.
  if (trace.modelSaw.some((s) => s.includes(PW))) problems.push("the real password was sent to the model (keyboard not in password mode)");
  if (trace.outputs.some((s) => s.includes(PW))) problems.push("the model repeated the real password");
  const lastScreens = trace.screens.at(-1) ?? [];
  if (trace.turns >= MAX_TURNS && lastScreens.length) problems.push("flow did not finish within the turn limit");
  problems.push(...flow.checks(trace).filter(Boolean));
  return { variant, model, flow: flow.id, run, ok: problems.length === 0, problems, trace: { ...trace, finalConsole: c }, log, modelCalls, ms: Date.now() - started };
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

const median = (xs: number[]) => { const s = [...xs].sort((a, b) => a - b); return s.length ? s[Math.floor(s.length / 2)] : 0; };
console.log(`\nvariant: ${variant}`);
console.log(`\n| model | flows passed | median turns | median model calls | median flow s | rejected screens |\n|---|---|---|---|---|---|`);
for (const m of models) {
  const rs = results.filter((r) => r.model === m);
  console.log(`| ${m} | ${rs.filter((r) => r.ok).length}/${rs.length} | ${median(rs.map((r) => r.trace.turns))} | ${median(rs.map((r) => r.modelCalls))} | ${(median(rs.map((r) => r.ms)) / 1000).toFixed(1)} | ${rs.reduce((n, r) => n + (r.trace.rejected ?? 0), 0)} |`);
}
console.log("\nPer flow:");
for (const r of results) {
  const path = r.trace.screens.map((s) => s.join("+") || "text").join(" → ");
  console.log(`${r.ok ? "✓" : "✗"} ${r.model} ${r.flow}#${r.run}: ${path}${r.problems.length ? ` — ${r.problems.join(" | ")}` : ""}`);
}
if (process.argv.includes("--verbose")) for (const r of results) console.log(`\n### ${r.model} ${r.flow}#${r.run}\n${r.log.join("\n")}`);
console.log(`\nraw: eval/domain-tools/results/raw/${stamp}.json`);
