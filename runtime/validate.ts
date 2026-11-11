// Validates an A2UI 1.0 surface (flat component list + data) against a catalog:
// JSON Schema per component (the catalog's own props plus the 1.0 envelope, ComponentCommon),
// then the structural rules the spec states in prose
// (one `root`, unique ids, every referenced id exists, no cycles, bindings have data).
// The catalog is imported as JSON (not read from disk) so bundlers can embed it: the same
// runtime also runs inside QuickJS on the console.
import { Ajv2020 } from "ajv/dist/2020.js";
import type { ValidateFunction } from "ajv";
import catalogJson from "../catalog/ps5.catalog.json" with { type: "json" };
import commonTypesJson from "../vendor/a2ui-v1_0/common_types.json" with { type: "json" };

export const ps5Catalog: any = catalogJson;
export const commonTypes: any = commonTypesJson;

const ajv = new Ajv2020({ strict: false, allErrors: true });
// The catalog refers to "common_types.json" relative to its own $id, and common_types.json to the
// active catalog as "catalog.json" relative to its $id: register each under the other's base too.
const base = (id: string) => id.slice(0, id.lastIndexOf("/") + 1);
ajv.addSchema(commonTypes);
ajv.addSchema({ ...commonTypes, $id: `${base(ps5Catalog.$id)}common_types.json` });
ajv.addSchema(ps5Catalog);
ajv.addSchema({ ...ps5Catalog, $id: `${base(commonTypes.$id)}catalog.json` });
ajv.addSchema({ ...ps5Catalog, $id: `${base(ps5Catalog.$id)}catalog.json` });

// A component as the 1.0 envelope has it (agent_to_renderer.json, "Component"): the catalog's
// props plus ComponentCommon (id, catalogId, accessibility, metadata), nothing else.
const validators = new Map<string, ValidateFunction>();
for (const name of Object.keys(ps5Catalog.components)) {
  const fn = ajv.compile({
    allOf: [{ $ref: `${commonTypes.$id}#/$defs/ComponentCommon` }, { $ref: `${ps5Catalog.$id}#/components/${name}` }],
    unevaluatedProperties: false,
  });
  validators.set(name, fn);
}

type Component = Record<string, unknown> & { id?: unknown; component?: unknown };

export interface Report {
  ok: boolean;
  errors: string[];
  warnings: string[];
  types: string[];
}

/** Component ids a component points at (child, children, template, Screen.status/hints, Pages). */
function references(c: Component): string[] {
  const out: string[] = [];
  for (const key of ["child", "status", "hints"]) if (typeof c[key] === "string") out.push(c[key] as string);
  const children = c.children;
  if (Array.isArray(children)) out.push(...children.filter((x): x is string => typeof x === "string"));
  else if (children && typeof children === "object" && typeof (children as Component).componentId === "string")
    out.push((children as Component).componentId as string);
  if (Array.isArray(c.pages)) for (const p of c.pages) if (p && typeof p.child === "string") out.push(p.child);
  return out;
}

/** Every {"@path"} binding inside a value. */
function bindings(value: unknown, out: string[] = []): string[] {
  if (Array.isArray(value)) for (const v of value) bindings(v, out);
  else if (value && typeof value === "object") {
    const o = value as Record<string, unknown>;
    if (typeof o["@path"] === "string" && Object.keys(o).length === 1) out.push(o["@path"]);
    for (const [k, v] of Object.entries(o)) if (k !== "@path") bindings(v, out);
  }
  return out;
}

function resolve(data: unknown, pointer: string): unknown {
  let cur = data;
  for (const part of pointer.split("/").filter(Boolean)) {
    if (cur && typeof cur === "object") cur = (cur as Record<string, unknown>)[part];
    else return undefined;
  }
  return cur;
}

export function validateSurface(components: unknown, data: unknown): Report {
  const errors: string[] = [];
  const warnings: string[] = [];
  if (!Array.isArray(components) || components.length === 0)
    return { ok: false, errors: ["components is not a non-empty array"], warnings, types: [] };

  const byId = new Map<string, Component>();
  const templateIds = new Set<string>();
  for (const [i, raw] of components.entries()) {
    const c = raw as Component;
    if (!c || typeof c !== "object") { errors.push(`#${i}: not an object`); continue; }
    if (typeof c.id !== "string") { errors.push(`#${i}: missing id`); continue; }
    if (byId.has(c.id)) errors.push(`${c.id}: duplicate id`);
    byId.set(c.id, c);
    const type = String(c.component);
    const validate = validators.get(type);
    if (!validate) { errors.push(`${c.id}: unknown component "${type}"`); continue; }
    // Call as a plain predicate: ValidateFunction's `data is T` guard would otherwise narrow `c` away.
    const valid = (validate as (data: unknown) => boolean)(c);
    if (!valid)
      for (const e of validate.errors ?? [])
        errors.push(`${c.id} (${type}): ${e.instancePath || "/"} ${e.message}${e.params && "additionalProperty" in e.params ? ` "${e.params.additionalProperty}"` : ""}`);
    const ch = c.children as Component | undefined;
    if (ch && !Array.isArray(ch) && typeof ch.componentId === "string") templateIds.add(ch.componentId);
  }

  if (!byId.has("root")) errors.push('no component with id "root"');
  for (const c of byId.values())
    for (const ref of references(c)) if (!byId.has(ref)) errors.push(`${c.id}: references missing id "${ref}"`);

  // Cycles and orphans, walking from root.
  const reached = new Set<string>();
  const walk = (id: string, stack: string[]) => {
    if (stack.includes(id)) { errors.push(`cycle: ${[...stack, id].join(" -> ")}`); return; }
    if (reached.has(id)) return;
    reached.add(id);
    const c = byId.get(id);
    if (c) for (const ref of references(c)) walk(ref, [...stack, id]);
  };
  if (byId.has("root")) walk("root", []);
  for (const id of byId.keys()) if (!reached.has(id)) warnings.push(`${id}: not reachable from root`);

  // Bindings outside templates must resolve against the data model.
  for (const c of byId.values()) {
    if (templateIds.has(c.id as string)) continue;
    for (const path of bindings(c)) {
      const isInput = ["TextPrompt", "VoicePrompt"].includes(String(c.component));
      if (!path.startsWith("/")) { if (!isInput) warnings.push(`${c.id}: relative path "${path}" outside a template`); continue; }
      if (resolve(data, path) === undefined && !isInput) warnings.push(`${c.id}: no data at "${path}"`);
    }
  }

  return { ok: errors.length === 0, errors, warnings, types: [...new Set([...byId.values()].map((c) => String(c.component)))] };
}
