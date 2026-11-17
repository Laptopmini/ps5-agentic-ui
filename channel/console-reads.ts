// What the agent can ask about the console itself, as tools. The console answers each read
// (renderer/src/ps5_system.*): the agent on the console calls it in-process, an agent elsewhere
// through the channel (ps5ConsoleTools in adapter.ts). One list, so both offer the same tools.
type Json = Record<string, any>;

export interface ConsoleRead {
  /** The tool the agent sees. A tool of the same name in the agent is replaced. */
  tool: string;
  /** What the console is asked for. */
  read: string;
  description: string;
  /** The console's answer as the tool result (errors become an instruction for the model). */
  result: (answer: Json) => Json;
}

const unreadable = (what: string) => (answer: Json) => ({
  error: `${what}_unreadable`,
  reason: answer.reason ?? answer.error,
  next: "Tell the user this couldn't be read on their console.",
});

const pass = (what: string) => (answer: Json) => (answer.error ? unreadable(what)(answer) : answer);

export const CONSOLE_READS: ConsoleRead[] = [
  {
    tool: "list_games",
    read: "installed_titles",
    description:
      "List the games and apps installed on the player's PS5: id, title, platform (PS5, PS4 or homebrew), icon (use it as a Tile's image), hoursPlayed on this console and lastPlayed (date) when known. notInstalled counts the other titles on the player's account (bought, PS Plus, played before) that aren't installed; don't offer those as playable.",
    result: (a) => (a.error ? unreadable("games")(a) : { games: a.games ?? [], notInstalled: a.notInstalled ?? 0 }),
  },
  {
    tool: "system_info",
    read: "system_info",
    description: "The player's PS5: model, system software version, time since it was turned on (minutes), and internal storage (total, used and free, GB). Fields that couldn't be read are left out.",
    result: pass("system_info"),
  },
  {
    tool: "console_temperatures",
    read: "temperatures",
    description: "The PS5's current temperatures in °C: cpu, soc (the main chip) and ssd (the M.2 expansion slot, only if one is fitted). Fields that couldn't be read are left out.",
    result: pass("temperatures"),
  },
  {
    tool: "list_saves",
    read: "saves",
    description: "Saved game data on the player's PS5, most recently saved first: id, title, platform, icon (use it as a Tile's image when present), sizeMB, lastSaved (date).",
    result: pass("saves"),
  },
  {
    tool: "list_screenshots",
    read: "screenshots",
    description: "How many screenshots the player has taken, and when the newest five were taken (date, sizeMB). Which game a shot is from isn't known.",
    result: pass("screenshots"),
  },
  {
    tool: "recently_played",
    read: "play_activity",
    description: "The games the player played most recently, most recent first, with icon (use it as a Tile's image when present, so covers look the same as list_games), the total hours played in each on this console (the console's own count) and lastPlayed (date) when known.",
    result: pass("play_activity"),
  },
];

/**
 * Actions on what the reads list, not available yet (they come later, behind the console's own
 * approval). They replace the agent's simulated ones, which only know simulated games and saves.
 */
export const NOT_YET = [
  {
    tool: "launch_game",
    description: "Start an installed game by id. Not available on this console yet: it returns not_supported_yet.",
    parameters: { type: "object", properties: { id: { type: "string" } }, required: ["id"] },
    result: (args: Json) => ({ error: "not_supported_yet", id: args.id, next: "Say starting games from here isn't available yet; they can start it from the Home screen." }),
  },
  {
    tool: "delete_save",
    description: "Delete saved game data by id. Not available on this console yet: it returns not_supported_yet.",
    parameters: { type: "object", properties: { id: { type: "string" } }, required: ["id"] },
    result: (args: Json) => ({ error: "not_supported_yet", id: args.id, next: "Say deleting saves from here isn't available yet; they can do it in Settings > Saved Data." }),
  },
];

// Follow-up suggestions are the agent runtime's (runtime/tools.ts); the channel offers the same tool.
export { FOLLOW_UPS } from "../runtime/tools.ts";

/**
 * ask_choices: a short question with up to 5 one-tap options for the player. When the app is open
 * it's an in-app menu; when the app is closed it's a notification whose buttons are the options,
 * and the chosen option's `value` comes back as the player's reply. Pressing a notification choice
 * opens the web browser (notifications can't reply silently, docs/channel.md), so it's for quick
 * triage when the app is backgrounded; prefer show_menu while the app is open.
 */
export const ASK_CHOICES = {
  tool: "ask_choices",
  description:
    "Ask the player a short question with up to 5 one-tap options, so it works even if the app is closed (the options become notification buttons; otherwise an in-app menu). The chosen option's value comes back as the player's reply. Each option: a short label and the value to act on. Use it for a quick choice the player can answer from a game, not for long lists.",
  parameters: {
    type: "object",
    properties: {
      prompt: { type: "string", description: "The question, one short line." },
      options: {
        type: "array",
        description: "2 to 5 options.",
        items: {
          type: "object",
          properties: {
            label: { type: "string", description: "The button text, a few words." },
            value: { type: "string", description: "What comes back as the player's reply if this is chosen (defaults to the label)." },
          },
          required: ["label"],
        },
      },
    },
    required: ["prompt", "options"],
  },
  /** The options as the console takes them: {label, value} pairs, trimmed, at most 5 with a non-empty label. */
  clean: (args: Json): { label: string; value: string }[] =>
    (Array.isArray(args?.options) ? args.options : [])
      .filter((o: Json) => o && typeof o.label === "string" && o.label.trim())
      .map((o: Json) => ({ label: String(o.label).trim().slice(0, 60), value: String(o.value ?? o.label).trim().slice(0, 200) }))
      .slice(0, 5),
  prompt: (args: Json): string => String(args?.prompt ?? "").trim().slice(0, 300),
  /** Sent to the background payload: a notification whose buttons are the options. */
  event: (prompt: string, options: { label: string; value: string }[], title: string) => ({
    type: "CUSTOM",
    name: "ps5.notify",
    value: { title, text: prompt, choices: options },
  }),
};
