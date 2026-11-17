// The demo agent's tools: a console to read and change. Off the console it's simulated
// (console-sim.ts: Wi-Fi, models, saves, games, a profile), so the agent can be tried anywhere.
// On the console, the reads the app can really do (channel/console-reads.ts) replace the
// simulated ones of the same name, and the actions it can't do yet say so.
import type { AgentTool } from "../runtime/tools.ts";
import { CONSOLE_READS, NOT_YET } from "../channel/console-reads.ts";
import { type Console, domainTools, execDomain, freshConsole } from "./console-sim.ts";

type Json = Record<string, any>;

/**
 * Reads the console the agent runs on (the agent embedded in the console app), by name
 * (CONSOLE_READS: installed_titles, system_info, temperatures, …) → JSON, or {error, reason}.
 */
export type ConsoleSystem = (name: string) => Json;

/** The tools for one conversation: a fresh simulated console, plus the real one's reads if given. */
export function consoleTools(system?: ConsoleSystem, console: Console = freshConsole()): AgentTool[] {
  const real: AgentTool[] = system
    ? [
        ...CONSOLE_READS.map((r): AgentTool => ({ name: r.tool, description: r.description, run: () => r.result(system(r.read)) })),
        ...NOT_YET.map((t): AgentTool => ({ name: t.tool, description: t.description, parameters: t.parameters, run: (args) => t.result(args) })),
      ]
    : [];
  const simulated = domainTools({ approvalGate: true })
    .filter((t) => !real.some((r) => r.name === t.name))
    .map((t): AgentTool => ({
      name: t.name,
      description: t.description,
      parameters: t.parameters,
      approval: t.approval ? (args) => t.approval!(args, console) : undefined,
      run: async (args, ctx) => {
        // Work that takes time on a real console reports progress while it "runs".
        const work = t.progress?.(args, console);
        if (work) {
          const steps = Math.max(1, Math.round(work.ms / 400));
          for (let i = 0; i <= steps; i++) {
            ctx.progress(work.label, i / steps);
            if (i < steps) await new Promise((r) => setTimeout(r, work.ms / steps));
          }
        }
        // execDomain resolves secret:// handles itself, from a map: give it one that asks the thread.
        const secrets = { get: (handle: string) => ctx.secret(handle) ?? undefined } as Map<string, string>;
        return execDomain(console, t.name, args, secrets, true);
      },
    }));
  return [...simulated, ...real];
}
