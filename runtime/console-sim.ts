// A simulated PS5 for the agent runtime and evals: state plus the server-run domain tools.
type Json = Record<string, any>;

export interface Console {
  wifi: { connected: string | null; networks: { ssid: string; security: "WPA2" | "open"; password?: string; signal: number }[] };
  models: { id: string; name: string; sizeGB: number }[];
  loadedModel: string;
  freeMemoryGB: number;
  saves: { id: string; game: string; size: string }[];
  games: { id: string; title: string; sizeGB: number; lastPlayed: string; saveId?: string }[];
  running: string | null;
  profileName: string;
}

export const WIFI_PASSWORD = "hunter2-SECRET";

export const freshConsole = (): Console => ({
  wifi: {
    connected: null,
    networks: [
      { ssid: "Mini-5G", security: "WPA2", password: WIFI_PASSWORD, signal: 92 },
      { ssid: "Mini-2G", security: "WPA2", password: WIFI_PASSWORD, signal: 70 },
      { ssid: "Cafe-Guest", security: "open", signal: 31 },
    ],
  },
  models: [
    { id: "qwen-9b", name: "Qwen 9B", sizeGB: 5.4 },
    { id: "mistral-7b", name: "Mistral 7B", sizeGB: 4.1 },
    { id: "minicpm-2b", name: "MiniCPM 2B", sizeGB: 2.2 },
    { id: "qwen-27b", name: "Qwen 27B", sizeGB: 16.2 },
  ],
  loadedModel: "minicpm-2b",
  freeMemoryGB: 11,
  saves: [
    { id: "elden-ring", game: "Elden Ring", size: "12 MB" },
    { id: "astro-bot", game: "Astro Bot", size: "3 MB" },
  ],
  games: [
    { id: "elden-ring", title: "Elden Ring", sizeGB: 49.1, lastPlayed: "2026-11-02", saveId: "elden-ring" },
    { id: "astro-bot", title: "Astro Bot", sizeGB: 37.8, lastPlayed: "2026-10-28", saveId: "astro-bot" },
    { id: "gran-turismo-7", title: "Gran Turismo 7", sizeGB: 102.4, lastPlayed: "2026-09-14" },
  ],
  running: null,
  profileName: "Laptopmini",
});

const str = { type: "string" };

/** What the console's own confirm says before an approval-gated call runs. */
export interface Approval {
  message: string;
  confirmLabel: string;
  cancelLabel: string;
  destructive: boolean;
}

export interface DomainTool {
  name: string;
  description: string;
  parameters: Json;
  /** The runtime asks the user to approve this call (system confirm) before running it; null: no need this time. */
  approval?: (args: Json, c: Console) => Approval | null;
  /** Work that takes time on a real console: the runtime reports progress while it runs (the evals skip the wait). */
  progress?: (args: Json, c: Console) => { label: string; ms: number } | null;
}

export function domainTools(opts: { approvalGate: boolean }): DomainTool[] {
  const gate = (describe: (a: Json, c: Console) => Approval | null) => (opts.approvalGate ? describe : undefined);
  return [
    { name: "scan_wifi", description: "List nearby Wi-Fi networks with their security and signal.", parameters: { type: "object", properties: {} } },
    { name: "connect_wifi", description: "Connect to a Wi-Fi network. Secured networks need the password (a secret:// handle from a password keyboard).", parameters: { type: "object", properties: { ssid: str, password: str }, required: ["ssid"] } },
    { name: "list_models", description: "List installed AI models, the loaded one and free memory.", parameters: { type: "object", properties: {} } },
    {
      name: "load_model",
      description: "Load an installed AI model by id, replacing the loaded one.",
      parameters: { type: "object", properties: { id: str }, required: ["id"] },
      progress: (a, c) => {
        const m = c.models.find((x) => x.id === a.id);
        return m && m.sizeGB <= c.freeMemoryGB && m.id !== c.loadedModel ? { label: `Loading ${m.name}`, ms: 8000 } : null;
      },
    },
    { name: "list_saves", description: "List saved game data.", parameters: { type: "object", properties: {} } },
    {
      name: "delete_save",
      description: opts.approvalGate
        ? "Permanently delete saved game data by id. The console itself asks the user to approve this call."
        : "Permanently delete saved game data by id.",
      parameters: { type: "object", properties: { id: str }, required: ["id"] },
      approval: gate((a, c) => {
        const save = c.saves.find((s) => s.id === a.id);
        const message = save ? `Delete the ${save.game} save data (${save.size})? This can't be undone.` : `Delete save data "${a.id}"?`;
        return { message, confirmLabel: "Delete", cancelLabel: "Keep", destructive: true };
      }),
    },
    { name: "list_games", description: "List installed games: id, title, size, last played, whether they have save data, and the running game.", parameters: { type: "object", properties: {} } },
    {
      name: "launch_game",
      description: opts.approvalGate
        ? "Start an installed game by id (resumes from its save data if any). If another game is running, the console itself asks the user to close it first."
        : "Start an installed game by id (resumes from its save data if any). Closes the running game.",
      parameters: { type: "object", properties: { id: str }, required: ["id"] },
      approval: gate((a, c) => {
        const next = c.games.find((g) => g.id === a.id);
        const now = c.games.find((g) => g.id === c.running);
        if (!next || !now || now.id === next.id) return null;
        return { message: `Close ${now.title} and start ${next.title}? Unsaved progress in ${now.title} will be lost.`, confirmLabel: "Close and start", cancelLabel: "Keep playing", destructive: true };
      }),
    },
    { name: "rename_profile", description: "Change the profile name (3 to 16 characters).", parameters: { type: "object", properties: { name: str }, required: ["name"] } },
  ];
}

export function execDomain(c: Console, name: string, args: Json, secrets: Map<string, string>, actionableErrors: boolean): Json {
  const resolve = (v: unknown) => (typeof v === "string" && v.startsWith("secret://") ? secrets.get(v) ?? null : v);
  const hint = (o: Json) => (actionableErrors ? o : {});
  switch (name) {
    case "scan_wifi":
      return { networks: c.wifi.networks.map(({ ssid, security, signal }) => ({ ssid, security, signal })), connected: c.wifi.connected };
    case "connect_wifi": {
      const n = c.wifi.networks.find((x) => x.ssid === args.ssid);
      if (!n) return { error: "network_not_found", ssid: args.ssid };
      if (n.security !== "open") {
        if (args.password == null || args.password === "") return { error: "password_required", ssid: n.ssid, ...hint({ retryable: true, next: "Ask for the password with ask_text kind=password." }) };
        const pw = resolve(args.password);
        if (pw == null) return { error: "unknown_secret_handle" };
        if (pw !== n.password) return { error: "authentication_failed", ssid: n.ssid, ...hint({ retryable: true, next: "Ask for the password again with ask_text kind=password." }) };
      }
      c.wifi.connected = n.ssid;
      return { connected: true, ssid: n.ssid, ip: "192.168.1.42" };
    }
    case "list_models":
      return { models: c.models, loaded: c.loadedModel, freeMemoryGB: c.freeMemoryGB };
    case "load_model": {
      const m = c.models.find((x) => x.id === args.id);
      if (!m) return { error: "model_not_found", id: args.id };
      if (m.sizeGB > c.freeMemoryGB)
        return { error: "not_enough_memory", needsGB: m.sizeGB, freeGB: c.freeMemoryGB, ...hint({ retryable: false, next: "Explain, then let the user pick a model that fits with show_menu. Don't load anything they didn't pick." }) };
      c.loadedModel = m.id;
      return { loaded: m.id, name: m.name, seconds: 8 };
    }
    case "list_saves":
      return { saves: c.saves };
    case "delete_save": {
      const i = c.saves.findIndex((x) => x.id === args.id);
      if (i < 0) return { error: "save_not_found", id: args.id };
      const [gone] = c.saves.splice(i, 1);
      return { deleted: gone.id, game: gone.game };
    }
    case "list_games":
      return { games: c.games.map(({ saveId, ...g }) => ({ ...g, hasSave: !!saveId && c.saves.some((s) => s.id === saveId) })), running: c.running };
    case "launch_game": {
      const g = c.games.find((x) => x.id === args.id);
      if (!g) return { error: "game_not_installed", id: args.id, ...hint({ retryable: false, next: "Call list_games and offer what is installed." }) };
      if (c.running === g.id) return { alreadyRunning: true, title: g.title };
      const closed = c.running;
      c.running = g.id;
      g.lastPlayed = new Date().toISOString().slice(0, 10);
      const resumed = !!g.saveId && c.saves.some((s) => s.id === g.saveId);
      return { launched: g.id, title: g.title, resumedFromSave: resumed, ...(closed ? { closed } : {}) };
    }
    case "rename_profile": {
      const name = String(args.name ?? "");
      if (name.length < 3 || name.length > 16)
        return { error: "invalid_length", min: 3, max: 16, got: name.length, ...hint({ retryable: true, next: "Ask for a new name with ask_text (or offer shorter names with show_menu)." }) };
      c.profileName = name;
      return { renamed: true, name };
    }
  }
  return { error: "unknown_tool" };
}
