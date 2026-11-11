// Re-validate saved results after undoing MiniMax's tool-argument corruption
// ({"item":[...]} -> [...], "" -> [] for array fields), to separate model errors from provider damage.
import { readFileSync, readdirSync } from "node:fs";
import { validateSurface } from "../../runtime/validate.ts";
import { scenarios } from "./scenarios.ts";
import { repairMiniMaxArgs as normalize } from "../../runtime/provider-quirks.ts";

const dir = new URL("./results/raw/", import.meta.url);
const file = process.argv[2] ?? readdirSync(dir).filter((f) => f.endsWith(".json")).sort().pop()!;
const results = JSON.parse(readFileSync(new URL(file, dir), "utf8"));
const rows: Record<string, { n: number; raw: number; fixed: number }> = {};
const remaining: Record<string, number> = {};
for (const r of results) {
  if (r.mode === "intent") continue;
  const sc = scenarios.find((s) => s.id === r.scenario)!;
  const k = `${r.model} ${r.mode}`;
  rows[k] ??= { n: 0, raw: 0, fixed: 0 };
  rows[k].n++;
  if (r.ok) rows[k].raw++;
  const reports = r.calls.map((c: any) => (c.name === "render_a2ui" && c.args ? validateSurface(normalize(c.args.components), normalize(c.args.data ?? {})) : { ok: false, errors: ["bad call"], types: [] }));
  const types = new Set(reports.flatMap((x: any) => x.types));
  const valid = reports.length > 0 && reports.every((x: any) => x.ok);
  const hit = sc.a2ui.length === 0 || sc.a2ui.some((t) => types.has(t));
  if ((sc.a2ui.length === 0 && reports.length === 0) || (valid && hit)) rows[k].fixed++;
  for (const rep of reports) for (const e of rep.errors) {
    const key = e.replace(/^[^ ]+ /, "").replace(/\/\d+/g, "/N").replace(/"[^"]*"/g, '"…"').slice(0, 90);
    remaining[key] = (remaining[key] ?? 0) + 1;
  }
}
console.log("| model + mode | pass as returned | pass after undoing provider corruption |\n|---|---|---|");
for (const [k, v] of Object.entries(rows)) console.log(`| ${k} | ${v.raw}/${v.n} | ${v.fixed}/${v.n} |`);
console.log("\nMost common remaining errors:");
for (const [e, n] of Object.entries(remaining).sort((a, b) => b[1] - a[1]).slice(0, 12)) console.log(`${n}\t${e}`);
