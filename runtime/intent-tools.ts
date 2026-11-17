// Flat "intent tools" for small models: simple schemas (type, enum, required, items only,
// the subset ProsperoAI's tool-call validator accepts). The server turns each call into
// A2UI 1.0 components on the PS5 catalog; `toA2UI` is that translation.
import { Ajv } from "ajv";
import type { ValidateFunction } from "ajv";

type Json = Record<string, unknown>;
/** The catalog's named haptic effects ($defs.HapticEffect). */
export const HAPTIC_EFFECTS = ["tick", "confirm", "success", "warning", "error", "attention"] as const;

const str = { type: "string" };
const button = { type: "string", enum: ["cross", "circle", "triangle", "square", "l1", "r1"] };  // Options belongs to the assistant
/** On screens that need no answer: what the user might ask next (the runtime sends it as follow-ups). */
const followUps = { type: "array", items: { type: "string" }, description: "Optional: 2 or 3 very short next requests the user is likely to tap, in their words, at most ~6 words each so they fit a small button; shown under the screen" };

export const intentTools = [
  {
    name: "show_menu",
    description: "Show a vertical focus list to pick ONE option among a few (up to 8). The user's pick comes back as event menu_select {id}.",
    parameters: { type: "object", properties: { title: str, prompt: { type: "string", description: "The question or instruction shown above the options. Required when the title alone doesn't say what to choose (e.g. a quiz question)." }, items: { type: "array", items: { type: "object", properties: { id: str, label: str, detail: str }, required: ["id", "label"] } } }, required: ["title", "items"] },
  },
  {
    name: "show_shelf",
    description: "Show home-screen rows of tiles (games, media, apps). The chosen tile comes back as event tile_select {id}; then ask what to do with it before acting.",
    parameters: { type: "object", properties: { title: str, rows: { type: "array", items: { type: "object", properties: { title: str, tiles: { type: "array", items: { type: "object", properties: { id: str, title: str, subtitle: str, badge: str, image: { type: "string", description: "The game's icon URL, as list_games gave it" } }, required: ["id", "title"] } } }, required: ["title", "tiles"] } }, followUps }, required: ["title", "rows"] },
  },
  {
    name: "ask_text",
    description: "Open the system keyboard to ask for text. The answer comes back as event text_submit {value}.",
    parameters: { type: "object", properties: { title: str, placeholder: str, kind: { type: "string", enum: ["text", "number", "url", "email", "password"] } }, required: ["title"] },
  },
  {
    name: "ask_voice",
    description: "Ask the user to hold R2 and speak into the DualSense microphone. The transcript comes back as event voice_submit {value}.",
    parameters: { type: "object", properties: { label: str }, required: ["label"] },
  },
  {
    name: "confirm",
    description: "Ask a yes/no question in a system dialog. Use destructive=true for deleting or losing data. Answer comes back as event confirm_result {confirmed}.",
    parameters: { type: "object", properties: { message: str, confirmLabel: str, cancelLabel: str, destructive: { type: "boolean" }, haptic: { type: "string", enum: HAPTIC_EFFECTS, description: "Optional: felt on the controller when the user confirms" } }, required: ["message"] },
  },
  {
    name: "notify",
    description: "Show a short notice in the top-right corner that disappears by itself. Does not interrupt.",
    parameters: { type: "object", properties: { text: str, icon: { type: "string", enum: ["info", "success", "warning", "error", "download", "trophy"] } }, required: ["text"] },
  },
  {
    name: "show_progress",
    description: "Show a horizontal bar for a proportion: a job a console tool is running, or a fraction such as storage used vs free, battery, or completion. Pass value 0-1 (e.g. used/total). Never to stand in for an action no tool performs.",
    parameters: { type: "object", properties: { label: str, value: { type: "number", description: "0 to 1; omit if unknown" }, detail: str }, required: ["label"] },
  },
  {
    name: "show_info",
    description: "Show an information screen: a title, short lines, an optional status chip and optional controller shortcuts. A pressed shortcut comes back as event hint {id}.",
    parameters: {
      type: "object",
      properties: {
        title: str,
        lines: { type: "array", items: str },
        status: { type: "object", properties: { label: str, state: { type: "string", enum: ["ready", "busy", "error", "offline"] } }, required: ["label", "state"] },
        hints: { type: "array", items: { type: "object", properties: { button, label: str, id: str }, required: ["button", "label", "id"] } },
        followUps,
      },
      required: ["title", "lines"],
    },
  },
  {
    name: "show_pages",
    description: "Show a screen with sections switched with L1/R1 (like Settings). Each section has short lines of text.",
    parameters: { type: "object", properties: { title: str, pages: { type: "array", items: { type: "object", properties: { title: str, lines: { type: "array", items: str } }, required: ["title", "lines"] } }, followUps }, required: ["title", "pages"] },
  },
  {
    name: "show_stats",
    description: "Show numbers and metrics as a small dashboard of labelled value tiles (not lines of text). Prefer this for any set of figures — storage, counts, sizes, times, temperatures. Each stat has a short label and a value (format the value yourself, e.g. '161.2 GB'); add fraction 0-1 to draw a mini bar (e.g. used/total), and an optional one-line detail.",
    parameters: {
      type: "object",
      properties: {
        title: str,
        stats: { type: "array", items: { type: "object", properties: { label: str, value: str, detail: str, fraction: { type: "number", description: "0 to 1: draws a mini bar under the value, e.g. used/total" } }, required: ["label", "value"] } },
        status: { type: "object", properties: { label: str, state: { type: "string", enum: ["ready", "busy", "error", "offline"] } }, required: ["label", "state"] },
        hints: { type: "array", items: { type: "object", properties: { button, label: str, id: str }, required: ["button", "label", "id"] } },
        followUps,
      },
      required: ["title", "stats"],
    },
  },
] as const;

/**
 * Tools that act on the controller instead of drawing a screen. The run goes on after them, so the
 * model can still answer. The runtime turns each call into the catalog's function calls
 * (`rumble`, `lightbar`, `triggerEffect`), which the console runs on the DualSense.
 */
export const deviceTools = [
  {
    name: "controller_feedback",
    description:
      "Give feedback on the DualSense controller: a haptic (how something feels), the light bar's color, or resistance on the L2/R2 adaptive trigger. Nothing appears on screen. Use it sparingly, to mark a success, an error or something that needs attention, or to show a state for as long as it lasts (then set the trigger back to off).",
    parameters: {
      type: "object",
      properties: {
        haptic: { type: "string", enum: HAPTIC_EFFECTS, description: "A named feel: tick = light click, confirm = one firm pulse, success = done and it worked, warning = careful, error = it failed, attention = look at the screen" },
        pattern: {
          type: "array",
          description: "Only when the user asks for something the named ones don't cover, like a rumble of a given length or a heartbeat. Steps play in order; low = deep motor, high = sharp motor, 0 to 1 (both 0 = a pause); up to 16 steps and 10 seconds in all.",
          items: { type: "object", properties: { low: { type: "number" }, high: { type: "number" }, ms: { type: "number" } }, required: ["ms"] },
        },
        lightbar: { type: "string", description: "A color as #rrggbb, e.g. #00ff66" },
        trigger: { type: "string", enum: ["l2", "r2"] },
        triggerEffect: { type: "string", enum: ["off", "feedback", "weapon", "vibration"], description: "With trigger: feedback = resistance, weapon = a click like a trigger pull, vibration = buzzing, off = back to normal" },
        strength: { type: "number", description: "0 to 1, for the trigger effect" },
      },
    },
  },
] as const;

// The catalog's bounds for a pattern (catalog/ps5.catalog.json, renderer/src/haptics.cpp). They are
// safety limits: the renderer enforces them too.
const MAX_STEPS = 16;
const MAX_MS = 10000;
const level = (v: unknown) => Math.min(1, Math.max(0, Number(v ?? 0) || 0));

/** A device call as catalog function calls ({"@call", args}); or a problem to send back to the model. */
export function toFunctionCalls(name: string, a: Json): { calls: Json[] } | { problem: string } {
  if (name !== "controller_feedback") return { problem: `unknown device tool ${name}` };
  const calls: Json[] = [];
  if (a.haptic !== undefined && a.pattern !== undefined) return { problem: "use haptic or pattern, not both" };
  if (a.haptic !== undefined) {
    if (!(HAPTIC_EFFECTS as readonly string[]).includes(String(a.haptic))) return { problem: `haptic must be one of ${HAPTIC_EFFECTS.join(", ")}` };
    calls.push({ "@call": "haptic", args: { effect: String(a.haptic) } });
  }
  if (a.pattern !== undefined) {
    const steps = a.pattern as Json[];
    if (!Array.isArray(steps) || !steps.length) return { problem: "pattern needs at least one step" };
    if (steps.length > MAX_STEPS) return { problem: `pattern has at most ${MAX_STEPS} steps` };
    let total = 0;
    const out: Json[] = [];
    for (const st of steps) {
      const ms = Math.round(Number(st?.ms));
      if (!(ms > 0)) return { problem: "each pattern step needs ms, more than 0" };
      const capped = Math.min(Math.max(ms, 10), MAX_MS - total);
      if (capped <= 0) break;
      total += capped;
      out.push({ low: level(st.low), high: level(st.high), ms: capped });
    }
    calls.push({ "@call": "hapticPattern", args: { steps: out } });
  }
  if (a.lightbar !== undefined) {
    if (!/^#[0-9a-fA-F]{6}$/.test(String(a.lightbar))) return { problem: "lightbar must be a color like #00ff66" };
    calls.push({ "@call": "lightbar", args: { color: String(a.lightbar) } });
  }
  if (a.trigger || a.triggerEffect) {
    if (!a.trigger || !a.triggerEffect) return { problem: "trigger and triggerEffect go together" };
    const strength = Math.min(1, Math.max(0, Number(a.strength ?? 0.5)));
    calls.push({ "@call": "triggerEffect", args: { trigger: String(a.trigger), effect: String(a.triggerEffect), ...(a.triggerEffect === "off" ? {} : { strength }) } });
  }
  if (!calls.length) return { problem: "set at least one of haptic, pattern, lightbar, or trigger with triggerEffect" };
  return { calls };
}

// userMessage (A2UI 1.0): what the press says in the conversation, e.g. the item's label.
const event = (name: string, context?: Json, userMessage?: unknown) => ({ event: { name, ...(userMessage !== undefined ? { userMessage } : {}), ...(context ? { context } : {}) } });
const bind = (path: string) => ({ "@path": path });

/** Translate one intent call into an A2UI surface: { components, data }. */
export function toA2UI(name: string, a: Json): { components: Json[]; data: Json } {
  const s = (v: unknown) => String(v ?? "");
  switch (name) {
    case "show_menu": {
      const items = (a.items as Json[]) ?? [];
      return {
        components: [
          { id: "root", component: "Screen", title: s(a.title), children: a.prompt ? ["prompt", "menu"] : ["menu"] },
          ...(a.prompt ? [{ id: "prompt", component: "Text", text: s(a.prompt) }] : []),
          { id: "menu", component: "Menu", children: items.map((_, i) => `item-${i}`) },
          ...items.map((it, i) => ({ id: `item-${i}`, component: "MenuItem", label: s(it.label), ...(it.detail ? { detail: s(it.detail) } : {}), action: event("menu_select", { id: s(it.id) }, s(it.label)) })),
        ],
        data: {},
      };
    }
    case "show_shelf": {
      const rows = (a.rows as Json[]) ?? [];
      return {
        components: [
          { id: "root", component: "Screen", title: s(a.title), children: rows.map((_, i) => `row-${i}`) },
          ...rows.map((r, i) => ({ id: `row-${i}`, component: "Shelf", title: s(r.title), children: { componentId: "tile", path: `/rows/${i}` } })),
          { id: "tile", component: "Tile", title: bind("title"), subtitle: bind("subtitle"), badge: bind("badge"), image: bind("image"), action: event("tile_select", { id: bind("id"), title: bind("title") }, bind("title")) },
        ],
        data: { rows: rows.map((r) => (r.tiles as Json[]) ?? []) },
      };
    }
    case "ask_text":
      return {
        components: [{ id: "root", component: "TextPrompt", title: s(a.title), ...(a.placeholder ? { placeholder: s(a.placeholder) } : {}), ...(a.kind ? { kind: a.kind } : {}), value: bind("/answer"), submit: event("text_submit", { value: bind("/answer") }) }],
        data: {},
      };
    case "ask_voice":
      return {
        components: [{ id: "root", component: "VoicePrompt", label: s(a.label), value: bind("/transcript"), submit: event("voice_submit", { value: bind("/transcript") }) }],
        data: {},
      };
    case "confirm":
      return {
        components: [{ id: "root", component: "Confirm", message: s(a.message), ...(a.confirmLabel ? { confirmLabel: s(a.confirmLabel) } : {}), ...(a.cancelLabel ? { cancelLabel: s(a.cancelLabel) } : {}), ...(a.destructive !== undefined ? { destructive: Boolean(a.destructive) } : {}), ...((HAPTIC_EFFECTS as readonly string[]).includes(String(a.haptic)) ? { confirmHaptic: String(a.haptic) } : {}), confirm: event("confirm_result", { confirmed: true }), cancel: event("confirm_result", { confirmed: false }) }],
        data: {},
      };
    case "notify":
      return { components: [{ id: "root", component: "Notice", text: s(a.text), ...(a.icon ? { icon: a.icon } : {}) }], data: {} };
    case "show_progress":
      return { components: [{ id: "root", component: "Progress", label: s(a.label), ...(typeof a.value === "number" ? { value: a.value } : {}), ...(a.detail ? { detail: s(a.detail) } : {}) }], data: {} };
    case "show_info": {
      const lines = (a.lines as unknown[]) ?? [];
      const status = a.status as Json | undefined;
      const hints = ((a.hints as Json[]) ?? []).filter((h) => h.button !== "options");
      return {
        components: [
          { id: "root", component: "Screen", title: s(a.title), children: lines.map((_, i) => `line-${i}`), ...(status ? { status: "status" } : {}), ...(hints.length ? { hints: "hints" } : {}) },
          ...lines.map((l, i) => ({ id: `line-${i}`, component: "Text", text: s(l) })),
          ...(status ? [{ id: "status", component: "StatusChip", label: s(status.label), state: status.state }] : []),
          ...(hints.length ? [{ id: "hints", component: "ButtonHints", hints: hints.map((h) => ({ button: h.button, label: s(h.label), action: event("hint", { id: s(h.id) }, s(h.label)) })) }] : []),
        ],
        data: {},
      };
    }
    case "show_stats": {
      const stats = (a.stats as Json[]) ?? [];
      const status = a.status as Json | undefined;
      const hints = ((a.hints as Json[]) ?? []).filter((h) => h.button !== "options");
      return {
        components: [
          { id: "root", component: "Screen", title: s(a.title), children: ["stats"], ...(status ? { status: "status" } : {}), ...(hints.length ? { hints: "hints" } : {}) },
          { id: "stats", component: "Stats", items: stats.map((st) => ({ label: s(st.label), value: s(st.value), ...(st.detail ? { detail: s(st.detail) } : {}), ...(typeof st.fraction === "number" ? { fraction: st.fraction } : {}) })) },
          ...(status ? [{ id: "status", component: "StatusChip", label: s(status.label), state: status.state }] : []),
          ...(hints.length ? [{ id: "hints", component: "ButtonHints", hints: hints.map((h) => ({ button: h.button, label: s(h.label), action: event("hint", { id: s(h.id) }, s(h.label)) })) }] : []),
        ],
        data: {},
      };
    }
    case "show_pages": {
      const pages = (a.pages as Json[]) ?? [];
      return {
        components: [
          { id: "root", component: "Screen", title: s(a.title), children: ["pages"] },
          { id: "pages", component: "Pages", pages: pages.map((p, i) => ({ title: s(p.title), child: `page-${i}` })) },
          ...pages.map((p, i) => ({ id: `page-${i}`, component: "Card", children: ((p.lines as unknown[]) ?? []).map((_, j) => `page-${i}-line-${j}`) })),
          ...pages.flatMap((p, i) => ((p.lines as unknown[]) ?? []).map((l, j) => ({ id: `page-${i}-line-${j}`, component: "Text", text: s(l) }))),
        ],
        data: {},
      };
    }
    default:
      throw new Error(`unknown intent tool ${name}`);
  }
}

// ---------- argument checks ----------

const ajv = new Ajv({ strict: false, allErrors: true });
const checkArgs = new Map<string, ValidateFunction>([...intentTools, ...deviceTools].map((t): [string, ValidateFunction] => [t.name, ajv.compile(t.parameters)]));
const MIN_ITEMS: Record<string, string> = { show_menu: "items", show_shelf: "rows", show_pages: "pages" };

/** What's wrong with an intent or device tool call's arguments (sent back to the model), or null. */
export function intentProblem(name: string, args: Json): string | null {
  const check = checkArgs.get(name)!;
  if (!check(args)) return ajv.errorsText(check.errors);
  const list = MIN_ITEMS[name];
  if (list && !(Array.isArray(args[list]) && (args[list] as unknown[]).length > 0)) return `${list} must not be empty`;
  return null;
}
