// Channels UI → PS5 catalog A2UI. A posted message (a tree of Channels nodes: Message, Header,
// Section, Fields, Actions, Button, Select, Input, Carousel, Table, Chart...) becomes one surface:
// a Screen whose children are Text, Menu/MenuItem, Shelf/Tile and Image, the PS5 catalog's own
// components, so the console draws it natively. A message with an Input becomes a TextPrompt (the
// system keyboard) asking for it.
//
// Every control's action is the event "channel.action" with the Channels action id the engine
// bound (and the control's value), which the adapter turns back into an InteractionEvent.
import type { ChannelNode } from "@copilotkit/channels-core";

type Json = Record<string, any>;

/** The A2UI event a channel control sends, carried back to the adapter by the console. */
export const CHANNEL_ACTION = "channel.action";

/** A posted Channels message rendered to a PS5 A2UI surface. */
export interface RenderedSurface {
  components: Json[];
  data: Json;
  /** The message as plain text: the conversation's fallback and the agent's history. */
  text: string;
  /** Only text, no title, controls or images: shown as a chat bubble rather than a screen. */
  plain: boolean;
}

/** The action id the engine bound on an event prop (`{ id }` after binding), if any. */
function actionId(handler: unknown): string | undefined {
  if (handler && typeof handler === "object" && typeof (handler as Json).id === "string") return (handler as Json).id;
  return undefined;
}

const isNode = (v: unknown): v is ChannelNode => !!v && typeof v === "object" && "type" in (v as Json) && "props" in (v as Json);

function children(node: ChannelNode): unknown[] {
  const c = node.props?.children;
  if (c === undefined || c === null || c === false) return [];
  return (Array.isArray(c) ? c : [c]).flat(Infinity);
}

/** All the text under a node, in order. */
export function textOf(v: unknown): string {
  if (v === null || v === undefined || v === false || v === true) return "";
  if (typeof v === "string" || typeof v === "number") return String(v);
  if (Array.isArray(v)) return v.map(textOf).join("");
  if (isNode(v)) return v.type === "text" ? String(v.props?.value ?? "") : children(v).map(textOf).join("");
  return "";
}

// userMessage (A2UI 1.0): what the press says in the conversation (the item's label).
const event = (context: Json, userMessage?: unknown) => ({ event: { name: CHANNEL_ACTION, ...(userMessage !== undefined ? { userMessage } : {}), context } });

/** Build the surface for one posted message. */
export function toSurface(ir: ChannelNode[]): RenderedSurface {
  const components: Json[] = [];
  const data: Json = {};
  const body: string[] = [];  // ids of the Screen's children, in order
  const lines: string[] = [];
  let title = "";
  let n = 0;
  const id = (kind: string) => `${kind}-${n++}`;
  let menu: { id: string; items: string[] } | null = null;
  let input: { id: string; prompt: string; placeholder: string } | null = null;

  const text = (t: string, variant?: string) => {
    if (!t.trim()) return;
    menu = null;
    const cid = id("text");
    components.push({ id: cid, component: "Text", text: t, ...(variant ? { variant } : {}) });
    body.push(cid);
    lines.push(t);
  };
  // Buttons and select options next to each other share one menu.
  const item = (label: string, detail: string | undefined, context: Json) => {
    if (!menu) {
      menu = { id: id("menu"), items: [] };
      components.push({ id: menu.id, component: "Menu", children: menu.items });
      body.push(menu.id);
    }
    const cid = id("item");
    components.push({ id: cid, component: "MenuItem", label, ...(detail ? { detail } : {}), action: event(context, label) });
    menu.items.push(cid);
    lines.push(`- ${label}`);
  };

  const walk = (v: unknown): void => {
    if (typeof v === "string" || typeof v === "number") return text(String(v));
    if (!isNode(v)) {
      if (Array.isArray(v)) v.forEach(walk);
      return;
    }
    const p = v.props ?? {};
    switch (typeof v.type === "string" ? v.type : "") {
      case "text":  // the engine's text leaf
        return text(String(p.value ?? ""));
      case "header": {
        const t = textOf(v);
        if (!title) title = t;
        else text(t, "heading");
        return;
      }
      case "section":
      case "markdown":
        return text(textOf(v));
      case "context":
        return text(textOf(v), "caption");
      case "field":
        return text(p.label ? `**${p.label}:** ${textOf(v)}` : textOf(v));
      case "divider":
        menu = null;
        return;
      case "image": {
        menu = null;
        const cid = id("image");
        components.push({ id: cid, component: "Image", url: String(p.url ?? "") });
        body.push(cid);
        if (p.alt) lines.push(`[image: ${p.alt}]`);
        return;
      }
      case "render":
        return text(String(p.alt ?? ""), "caption");
      case "button": {
        const label = textOf(v) || "OK";
        const handler = actionId(p.onClick);
        if (!handler) return text(p.url ? `${label}: ${p.url}` : label, "caption");  // a link: shown, not followed
        return item(label, p.style === "danger" ? "Careful" : undefined, { id: handler, value: p.value ?? null });
      }
      case "select": {
        const handler = actionId(p.onSelect);
        if (p.placeholder) text(String(p.placeholder), "caption");
        for (const o of (p.options as Json[]) ?? []) {
          if (handler) item(String(o.label), undefined, { id: handler, value: String(o.value) });
          else text(String(o.label));
        }
        return;
      }
      case "input": {
        const handler = actionId(p.onSubmit);
        if (handler && !input) input = { id: handler, prompt: "", placeholder: String(p.placeholder ?? "") };
        return;
      }
      case "carousel": {
        menu = null;
        const cards = children(v).filter(isNode);
        const shelf = id("shelf");
        const tiles = cards.map((card, i) => {
          const parts = children(card).filter(isNode);
          const head = parts.find((c) => c.type === "header");
          const button = parts.find((c) => c.type === "button" && actionId(c.props?.onClick));
          const cardTitle = head ? textOf(head) : `Item ${i + 1}`;
          const subtitle = parts.filter((c) => c.type === "section" || c.type === "markdown").map(textOf).join(" ");
          lines.push(`- ${cardTitle}`);
          return { title: cardTitle, subtitle, actionId: button ? actionId(button.props.onClick) : null, value: button?.props.value ?? null };
        });
        const path = `/${shelf}`;
        data[shelf] = tiles;
        components.push({ id: shelf, component: "Shelf", children: { componentId: `${shelf}-tile`, path } });
        components.push({
          id: `${shelf}-tile`,
          component: "Tile",
          title: { "@path": "title" },
          subtitle: { "@path": "subtitle" },
          action: event({ id: { "@path": "actionId" }, value: { "@path": "value" } }, { "@path": "title" }),
        });
        body.push(shelf);
        return;
      }
      case "table": {
        const cols = ((p.columns as Json[]) ?? []).map((c) => String(c.header));
        if (cols.length) text(cols.join(" · "), "caption");
        for (const row of children(v).filter(isNode)) text(children(row).filter(isNode).map(textOf).join(" · "));
        return;
      }
      case "chart": {
        const points = ((p.data as Json[]) ?? []).map((d) => `${d.label}: ${d.value}`);
        if (p.title) text(String(p.title), "heading");
        return text(points.join("\n"));
      }
      default:
        // message, fields, actions, row, cell, fragments: containers.
        children(v).forEach(walk);
    }
  };
  ir.forEach(walk);

  if (input) {
    // The system keyboard asks for it; what the message says becomes the keyboard's title.
    const prompt = lines.filter((l) => !l.startsWith("- ")).join(" ").trim() || input.placeholder || "Type a reply";
    return {
      components: [{
        id: "root",
        component: "TextPrompt",
        title: prompt,
        ...(input.placeholder ? { placeholder: input.placeholder } : {}),
        value: { "@path": "/answer" },
        submit: event({ id: input.id, value: { "@path": "/answer" } }),
      }],
      data: {},
      text: prompt,
      plain: false,
    };
  }
  const plain = !title && components.every((c) => c.component === "Text");
  components.unshift({ id: "root", component: "Screen", title: title || "Message", children: body });
  return { components, data, text: [title, ...lines].filter(Boolean).join("\n"), plain };
}
