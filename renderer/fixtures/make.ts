// Build renderer fixtures from real model output (intent-mode runs of eval/catalog-eval): each intent call is
// translated to A2UI 1.0 (as the server does) and wrapped the way CopilotKit 1.85 sends it: one
// AG-UI ACTIVITY_SNAPSHOT "generative-ui-surface" event (runtime/a2ui.ts).
//
// Usage: node renderer/fixtures/make.ts [eval/catalog-eval raw result file]
import { readdirSync, readFileSync, writeFileSync } from "node:fs";
import { toA2UI } from "../../runtime/intent-tools.ts";
import { createSurface, surfaceSnapshot } from "../../runtime/a2ui.ts";

const rawDir = new URL("../../eval/catalog-eval/results/raw/", import.meta.url);
const file = process.argv[2] ?? readdirSync(rawDir).filter((f) => f.endsWith(".json")).sort().pop()!;
const results: any[] = JSON.parse(readFileSync(new URL(file, rawDir), "utf8"));

// scenario -> preferred tool; the first passing call wins.
const wanted: Record<string, string> = {
  "pick-model": "show_menu", "home-shelves": "show_shelf", "save-name": "ask_text", "wifi-password": "ask_text",
  "delete-save": "confirm", "download-progress": "show_progress", "loaded-notice": "notify",
  "settings-pages": "show_pages", "status-screen": "show_info", "dictate": "ask_voice", "quiz": "show_menu",
};
const ordered = results;
let written = 0;
for (const [scenario, tool] of Object.entries(wanted)) {
  const hit = ordered.filter((r) => r.mode === "intent" && r.scenario === scenario).flatMap((r) => r.calls.map((c: any) => ({ r, c }))).find(({ c }) => c.name === tool && c.report?.ok);
  if (!hit) { console.warn(`no ${tool} call for ${scenario} in ${file}`); continue; }
  const surface = toA2UI(hit.c.name, hit.c.args);
  const surfaceId = `${scenario}-1`;
  const event = surfaceSnapshot(surfaceId, createSurface(surfaceId, surface.components, surface.data));
  const meta = { _source: { file, scenario, tool, args: hit.c.args } };
  writeFileSync(new URL(`./${scenario}.jsonl`, import.meta.url), `${JSON.stringify(event)}\n`);
  writeFileSync(new URL(`./${scenario}.source.json`, import.meta.url), `${JSON.stringify(meta, null, 2)}\n`);
  written++;
}
console.log(`${written} fixtures from ${file}`);
