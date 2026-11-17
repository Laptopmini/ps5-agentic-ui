// Catalog ⇄ renderer ⇄ intent-tools self-consistency. The catalog is the enforceable contract
// between the agent and the renderer; this keeps it from drifting. It checks that every renderer
// fixture (a real surface the renderer draws) validates against the catalog, and that every intent
// tool's own sample output does too — so a component the renderer gains (e.g. Stats) has to be
// added to the catalog or these fail.
import test from "node:test";
import assert from "node:assert/strict";
import { readdirSync, readFileSync } from "node:fs";
import { ps5Catalog, validateSurface } from "./validate.ts";
import { toA2UI } from "./intent-tools.ts";

const fixturesDir = new URL("../renderer/fixtures/", import.meta.url);

function surfaceOf(jsonl: string): { components: unknown[]; data: unknown } {
  const ev = JSON.parse(jsonl.trim().split("\n")[0]);
  const op = (ev.content.operations as any[]).find((o) => o.createSurface)?.createSurface;
  assert.ok(op, "fixture has no createSurface operation");
  return { components: op.components, data: op.dataModel ?? {} };
}

test("every renderer fixture validates against the catalog", () => {
  const files = readdirSync(fixturesDir).filter((f) => f.endsWith(".jsonl"));
  assert.ok(files.length > 0, "no fixtures found");
  for (const f of files) {
    const { components, data } = surfaceOf(readFileSync(new URL(f, fixturesDir), "utf8"));
    const report = validateSurface(components, data);
    assert.ok(report.ok, `${f}: ${report.errors.join("; ")}`);
  }
});

// Sample args for each screen-drawing intent, so toA2UI output is validated against the catalog.
const samples: Record<string, Record<string, unknown>> = {
  show_menu: { title: "Pick", items: [{ id: "a", label: "A" }, { id: "b", label: "B", detail: "d" }] },
  show_shelf: { title: "Library", rows: [{ title: "Games", tiles: [{ id: "g1", title: "Game", subtitle: "s", badge: "PS5" }] }] },
  ask_text: { title: "Name?", placeholder: "e.g. Save 1", kind: "text" },
  ask_voice: { label: "Hold R2 and speak" },
  confirm: { message: "Delete this save?", destructive: true },
  notify: { text: "Done", icon: "success" },
  show_progress: { label: "Storage", value: 0.76, detail: "509 of 670 GB" },
  show_info: { title: "Console", lines: ["Model CFI-1015A", "On for 4.3 hrs"], status: { label: "Ready", state: "ready" } },
  show_pages: { title: "Settings", pages: [{ title: "Drive", lines: ["670 GB"] }, { title: "Saves", lines: ["12 saves"] }] },
  show_stats: { title: "Storage", stats: [{ label: "Free", value: "161 GB", detail: "of 670 GB", fraction: 0.24 }, { label: "Used", value: "509 GB", fraction: 0.76 }] },
};

test("every screen intent's A2UI output validates against the catalog", () => {
  for (const [name, args] of Object.entries(samples)) {
    const { components, data } = toA2UI(name, args as any);
    const report = validateSurface(components, data);
    assert.ok(report.ok, `${name}: ${report.errors.join("; ")}`);
  }
});

test("the catalog carries a schema for every component the samples render", () => {
  const used = new Set<string>();
  for (const [name, args] of Object.entries(samples))
    for (const c of toA2UI(name, args as any).components) used.add(String((c as any).component));
  for (const type of used) assert.ok(ps5Catalog.components[type], `catalog is missing "${type}"`);
});
