// The PS5 agent runtime: an AG-UI agent that answers with native PS5 screens (A2UI 1.0 surfaces
// on the PS5 catalog) and controller feedback.
//
//   const thread = new AgentThread(id, { model: modelFromEnv(), tools: [...] });
//   await thread.run(runAgentInput, (event) => …);          // AG-UI events
//   await serveAgent({ thread: (id) => new AgentThread(id, …) });   // the same, over HTTP + SSE
//
// The lower-level pieces are exported too: the intent tools and their translation to A2UI, the
// catalog validator, and the A2UI-over-AG-UI envelope.
export { AgentThread, type AgentOptions, type Emit, type RunAgentInput } from "./thread.ts";
export { serveAgent, type ServeOptions } from "./serve.ts";
export { type AgentTool, type Approval, type ToolContext, FOLLOW_UPS } from "./tools.ts";
export { type Complete, type CompleteHooks, type ModelConfig, DEFAULT_BASE_URL, modelFromEnv, openAICompatible, StreamedReply } from "./model.ts";
export { BASE_PROMPT, CONFIRM_RULE, GATE_RULE, HONESTY_RULE, PS5_PROMPT, UI_RULE } from "./prompts.ts";
export { deviceTools, HAPTIC_EFFECTS, intentProblem, intentTools, toA2UI, toFunctionCalls } from "./intent-tools.ts";
export { ps5Catalog, validateSurface, type Report } from "./validate.ts";
export {
  A2UI_VERSION, FUNCTION_CALL_ACTIVITY, PS5_CATALOG_ID, SURFACE_ACTIVITY,
  actionOf, actionText, createSurface, functionCallEvent, removedSurface, surfaceSnapshot, uiEnvelope,
} from "./a2ui.ts";
