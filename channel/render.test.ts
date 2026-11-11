// The Channels UI → PS5 catalog mapping, on trees shaped like the ones the engine hands an adapter
// (text as {type: "text", props: {value}}, event props bound to {id}).
import assert from "node:assert/strict";
import { test } from "node:test";
import { CHANNEL_ACTION, toSurface } from "./render.ts";

const t = (value: string) => ({ type: "text", props: { value } });
const node = (type: string, props: Record<string, unknown> = {}, ...children: unknown[]) => ({ type, props: { ...props, children } });

test("a message becomes a Screen with text and a menu of its buttons", () => {
  const s = toSurface([
    node("message", {},
      node("header", {}, t("Pick a game")),
      node("section", {}, t("Two are installed.")),
      node("actions", {},
        node("button", { onClick: { id: "ck:a" }, value: "astro" }, t("Astro Bot")),
        node("button", { onClick: { id: "ck:b" }, value: "hades", style: "danger" }, t("Hades")))),
  ] as never);
  const root = s.components[0];
  assert.equal(root.component, "Screen");
  assert.equal(root.title, "Pick a game");
  const items = s.components.filter((c) => c.component === "MenuItem");
  assert.deepEqual(items.map((i) => i.label), ["Astro Bot", "Hades"]);
  assert.deepEqual(items[0].action, { event: { name: CHANNEL_ACTION, userMessage: "Astro Bot", context: { id: "ck:a", value: "astro" } } });
  assert.equal(s.plain, false);
  assert.match(s.text, /Pick a game\nTwo are installed\.\n- Astro Bot/);
});

test("plain text is a chat bubble, not a screen", () => {
  assert.equal(toSurface([t("Hello there")] as never).plain, true);
});

test("a select's options are menu items carrying the option value", () => {
  const s = toSurface([node("select", { onSelect: { id: "ck:s" }, placeholder: "Size", options: [{ label: "Small", value: "s" }, { label: "Large", value: "l" }] })] as never);
  const items = s.components.filter((c) => c.component === "MenuItem");
  assert.deepEqual(items.map((i) => i.action.event.context), [{ id: "ck:s", value: "s" }, { id: "ck:s", value: "l" }]);
});

test("a message with an input asks with the system keyboard", () => {
  const s = toSurface([node("message", {}, node("section", {}, t("What should I call you?")), node("input", { onSubmit: { id: "ck:i" }, placeholder: "Name" }))] as never);
  assert.equal(s.components.length, 1);
  const p = s.components[0];
  assert.equal(p.component, "TextPrompt");
  assert.equal(p.title, "What should I call you?");
  assert.deepEqual(p.submit.event.context, { id: "ck:i", value: { "@path": "/answer" } });
});

test("a carousel is a shelf of tiles bound to each card's button", () => {
  const card = (name: string, id: string) => node("carouselCard", {}, node("header", {}, t(name)), node("button", { onClick: { id }, value: name }, t("Open")));
  const s = toSurface([node("carousel", {}, card("Astro Bot", "ck:1"), card("Hades", "ck:2"))] as never);
  const shelf = s.components.find((c) => c.component === "Shelf")!;
  const rows = s.data[shelf.id];
  assert.deepEqual(rows.map((r: Record<string, unknown>) => [r.title, r.actionId]), [["Astro Bot", "ck:1"], ["Hades", "ck:2"]]);
  const tile = s.components.find((c) => c.component === "Tile")!;
  assert.deepEqual(tile.action.event.context, { id: { "@path": "actionId" }, value: { "@path": "value" } });
});
