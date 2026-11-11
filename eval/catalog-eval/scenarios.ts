// User requests a PS5 agent should answer with native UI.
// `a2ui`: component types of which at least one must appear (empty = any answer, UI optional).
// `intent`: intent tools of which at least one must be called (empty = no tool required).
export interface Scenario {
  id: string;
  prompt: string;
  a2ui: string[];
  intent: string[];
}

export const scenarios: Scenario[] = [
  { id: "home-shelves", prompt: "Show me a home screen for my game library: recently played Astro Bot, Returnal and Hades II, plus a row of three indie recommendations.", a2ui: ["Shelf"], intent: ["show_shelf"] },
  { id: "pick-model", prompt: "Let me pick which local model to load: Qwen 9B (5.4 GB), Mistral 7B (4.1 GB) or MiniCPM 2B (2.2 GB).", a2ui: ["Menu"], intent: ["show_menu"] },
  { id: "save-name", prompt: "Ask me for the name of my new save slot.", a2ui: ["TextPrompt"], intent: ["ask_text"] },
  { id: "delete-save", prompt: "I want to delete my Elden Ring save data (12 MB). Make sure I really mean it first.", a2ui: ["Confirm"], intent: ["confirm"] },
  { id: "wifi-password", prompt: "Ask for my Wi-Fi password for the network Mini-5G.", a2ui: ["TextPrompt"], intent: ["ask_text"] },
  { id: "download-progress", prompt: "Show the download of Qwen 9B: 1.2 GB of 5.4 GB done.", a2ui: ["Progress"], intent: ["show_progress"] },
  { id: "loaded-notice", prompt: "Let me know, briefly and without interrupting what I'm doing, that the model finished loading.", a2ui: ["Notice"], intent: ["notify"] },
  { id: "settings-pages", prompt: "Build a settings screen with three sections I switch between with L1/R1: Model, Voice and About.", a2ui: ["Pages"], intent: ["show_pages"] },
  { id: "dictate", prompt: "I'd rather speak my question into the controller mic than type it.", a2ui: ["VoicePrompt"], intent: ["ask_voice"] },
  { id: "fetch-url", prompt: "Ask me which web page URL you should read for me.", a2ui: ["TextPrompt"], intent: ["ask_text"] },
  { id: "quiz", prompt: "Give me a four-answer multiple-choice question about PlayStation history.", a2ui: ["Menu"], intent: ["show_menu"] },
  { id: "status-screen", prompt: "Show a status screen: model Qwen 9B is ready, 53 tokens/s, GPU at 71°C, with a Triangle shortcut to refresh.", a2ui: ["StatusChip", "ButtonHints"], intent: ["show_info"] },
  { id: "oom-retry", prompt: "Image generation just failed: out of memory. Let me retry at a smaller size or cancel.", a2ui: ["Confirm", "Menu"], intent: ["confirm", "show_menu"] },
  { id: "pick-with-rumble", prompt: "Let me pick a game to launch, Astro Bot or Hades II, and make the controller rumble briefly to confirm.", a2ui: ["Menu", "Shelf"], intent: ["show_menu", "show_shelf"] },
  { id: "plain-question", prompt: "What year did the PS5 launch?", a2ui: [], intent: [] },
  { id: "profile-edit", prompt: "I want to rename my profile and choose an avatar color: red, blue or green.", a2ui: ["Menu", "TextPrompt"], intent: ["show_menu", "ask_text"] },
];
