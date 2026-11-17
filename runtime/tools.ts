// Tools an agent brings to an AgentThread, and the one tool the runtime adds itself
// (follow-up suggestions).
type Json = Record<string, any>;

/** What the runtime's own confirm says before an approval-gated call runs. */
export interface Approval {
  message: string;
  confirmLabel?: string;
  cancelLabel?: string;
  destructive?: boolean;
}

/** What a tool gets besides its arguments. */
export interface ToolContext {
  threadId: string;
  /**
   * The value behind a secret:// handle. Text typed into a password keyboard never reaches the
   * model: it gets a handle, passes the handle to a tool, and the tool resolves it here.
   * Anything that isn't a handle comes back as it is; an unknown handle is null.
   */
  secret(value: unknown): unknown;
  /** Report progress on long work: shown as the status line (and a progress bar for 0 to 1). */
  progress(label: string, fraction?: number): void;
}

/** A tool the agent runs itself (as opposed to AG-UI client tools, which the client runs). */
export interface AgentTool {
  name: string;
  description: string;
  /** JSON Schema of the arguments; no arguments when unset. */
  parameters?: Json;
  /** Runs the call; the result goes back to the model as the tool result. */
  run(args: Json, ctx: ToolContext): Json | Promise<Json>;
  /**
   * Return an Approval to make the runtime ask the user first, with its own Confirm dialog (the
   * model can't skip or word it). Approved: `run` is called. Denied: the model is told so.
   */
  approval?(args: Json, ctx: ToolContext): Approval | null | undefined;
}

/**
 * Follow-up suggestions: up to three short things the player might ask next, shown as buttons
 * under the reply (pressing one sends it as their message). The console gets them as CUSTOM
 * "ps5.suggestions" {prompts}; a new message or run clears them.
 */
export const FOLLOW_UPS = {
  tool: "suggest_follow_ups",
  description:
    "Offer up to 3 very short follow-ups the user might tap next, in the user's own words and at most ~6 words each (e.g. \"Forza saves?\", \"How full is my disk?\"), shown as small buttons under your reply. Keep them terse — they must fit a button. Call it in the same message as your reply's text, as the last step, when a next step is likely; only suggest what your tools can do.",
  parameters: {
    type: "object",
    properties: { prompts: { type: "array", items: { type: "string" }, description: "2 or 3 prompts, each under 40 characters" } },
    required: ["prompts"],
  },
  /** The prompts as the console takes them: strings, trimmed, at most 3 of up to 80 characters. */
  clean: (args: Json): string[] =>
    (Array.isArray(args?.prompts) ? args.prompts : []).filter((p: unknown) => typeof p === "string" && p.trim()).map((p: string) => p.trim().slice(0, 44)).slice(0, 3),
  event: (prompts: string[]) => ({ type: "CUSTOM", name: "ps5.suggestions", value: { prompts } }),
};
