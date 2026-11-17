// Repairs for provider damage to tool-call arguments, applied by the server before validation.
//
// Seen from some providers: inside arrays of untyped objects, empty arrays arrive as "" and
// non-empty arrays sometimes arrive wrapped as {"item":[...]}. Intent tools avoid it by
// typing every nested array; render_a2ui can't (A2UI `children` is an array OR a template object),
// so full-A2UI mode needs this repair.
import { deepStrictEqual } from "node:assert/strict";

type Json = unknown;

/** A2UI fields whose value is an array (or, for `children`, an array or a template object). */
const ARRAY_FIELDS = new Set(["children", "hints", "pages", "items", "components"]);

/** Undo that damage. Returns a new value; the input isn't modified. */
export function repairToolArgs(value: Json, key = ""): Json {
  if (Array.isArray(value)) return value.map((v) => repairToolArgs(v));
  if (value && typeof value === "object") {
    const o = value as Record<string, Json>;
    const keys = Object.keys(o);
    // Only unwrap where an array is expected, so real data such as {"item":[...]} survives.
    if (ARRAY_FIELDS.has(key) && keys.length === 1 && keys[0] === "item" && Array.isArray(o.item)) return repairToolArgs(o.item, key);
    return Object.fromEntries(keys.map((k) => [k, repairToolArgs(o[k], k)]));
  }
  if (value === "" && ARRAY_FIELDS.has(key)) return [];
  return value;
}

// Self-check: node runtime/provider-quirks.ts
if (import.meta.main) {
  deepStrictEqual(repairToolArgs({ components: [{ id: "root", children: { item: ["a", "b"] } }, { id: "a", children: "" }, { id: "b", text: "" }] }), {
    components: [{ id: "root", children: ["a", "b"] }, { id: "a", children: [] }, { id: "b", text: "" }],
  });
  deepStrictEqual(repairToolArgs({ hints: { item: [{ button: "cross", label: "Go" }] } }), { hints: [{ button: "cross", label: "Go" }] });
  deepStrictEqual(repairToolArgs({ children: { componentId: "tile", path: "/games" } }), { children: { componentId: "tile", path: "/games" } });
  deepStrictEqual(repairToolArgs({ data: { item: ["x"] } }), { data: { item: ["x"] } }); // real data is left alone
  console.log("provider-quirks: ok");
}
