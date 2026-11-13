// A2UI 1.0 over AG-UI, the way CopilotKit 1.85 carries it (@copilotkit/shared, generative-ui):
// - a surface is one ACTIVITY_SNAPSHOT per surface (messageId "genui:<surfaceId>", activityType
//   "generative-ui-surface") whose content holds all of its operations so far;
// - a function the agent asks the renderer to run (callRendererFunction) is one ACTIVITY_SNAPSHOT
//   per call ("genui-fn:<functionCallId>", "generative-ui-function-call");
// - a press on a surface starts a run with a visible user message and the A2UI action message in
//   forwardedProps.copilotkit.ui.metadata.extensions.copilotkit_action.
// Spec: vendor/a2ui-v1_0 (google/A2UI specification/v1_0).
import { randomUUID } from "node:crypto";

type Json = Record<string, any>;

export const A2UI_VERSION = "1.0";
export const PS5_CATALOG_ID = "https://raw.githubusercontent.com/Laptopmini/ps5-agentic-ui/catalog-v1/catalog/ps5.catalog.json";
export const SURFACE_ACTIVITY = "generative-ui-surface";
export const FUNCTION_CALL_ACTIVITY = "generative-ui-function-call";
/** CopilotKit's envelope format (COPILOTKIT_UI_ENVELOPE_FORMAT). */
const ENVELOPE_FORMAT = 3;

/** A surface created whole: components and data in the createSurface itself (new in 1.0). */
export function createSurface(surfaceId: string, components: Json[], dataModel: Json = {}): Json[] {
  return [{ version: A2UI_VERSION, createSurface: { surfaceId, catalogId: PS5_CATALOG_ID, components, dataModel } }];
}

/** The snapshot of a surface: every operation it has had (replacing what the client holds). */
export function surfaceSnapshot(surfaceId: string, operations: Json[]): Json {
  return {
    type: "ACTIVITY_SNAPSHOT",
    messageId: `genui:${surfaceId}`,
    activityType: SURFACE_ACTIVITY,
    content: { surfaceId, target: "chat", catalogId: PS5_CATALOG_ID, operations, status: "ready", rejected: [] },
    replace: true,
  };
}

/** A removed surface, as CopilotKit sends it: created, then deleted. */
export function removedSurface(surfaceId: string): Json {
  return surfaceSnapshot(surfaceId, [
    { version: A2UI_VERSION, createSurface: { surfaceId, catalogId: PS5_CATALOG_ID } },
    { version: A2UI_VERSION, deleteSurface: { surfaceId } },
  ]);
}

/** A catalog function for the renderer to run now (callRendererFunction), e.g. a controller rumble. */
export function functionCallEvent(call: Json, functionCallId = `fn-${randomUUID().slice(0, 8)}`): Json {
  return {
    type: "ACTIVITY_SNAPSHOT",
    messageId: `genui-fn:${functionCallId}`,
    activityType: FUNCTION_CALL_ACTIVITY,
    content: { functionCallId, callFunction: { ...call, catalogId: PS5_CATALOG_ID } },
    replace: true,
  };
}

/** The A2UI action message ({version, action}) a run was started with, if any. */
export function actionOf(forwardedProps: unknown): Json | undefined {
  const action = (forwardedProps as Json | undefined)?.copilotkit?.ui?.metadata?.extensions?.copilotkit_action;
  return action && action.version === A2UI_VERSION && action.action && typeof action.action.name === "string" ? action.action : undefined;
}

/** forwardedProps.copilotkit.ui for a run: this client's catalog, and the press that started it. */
export function uiEnvelope(action?: Json): Json {
  return {
    [A2UI_VERSION]: { supportedCatalogIds: [PS5_CATALOG_ID] },
    metadata: {
      extensions: {
        copilotkit_format: ENVELOPE_FORMAT,
        copilotkit_transport: "tool",
        copilotkit_targets: [{ id: "chat", kind: "chat" }],
        copilotkit_surfaces: {},
        copilotkit_contextSent: false,
        ...(action ? { copilotkit_action: action } : {}),
      },
    },
  };
}

/**
 * The visible user message of a press, as CopilotKit writes it (buildActionMessageText): the
 * userMessage, else context.message or context.label, else the event name made readable; then
 * the other context fields, one "key: value" line each.
 */
export function actionText(action: Json): string {
  const context: Json = action.context ?? {};
  const text = [action.userMessage, context.message, context.label].find((v) => typeof v === "string" && v !== "");
  const name = String(action.name ?? "").replace(/[_-]+/g, " ").trim();
  const head = text ?? (name.charAt(0).toUpperCase() + name.slice(1).toLowerCase());
  const lines = Object.entries(context)
    .filter(([k, v]) => k !== "message" && k !== "label" && v !== undefined && v !== null)
    .map(([k, v]) => `${k}: ${typeof v === "object" ? JSON.stringify(v) : String(v)}`);
  return lines.length ? `${head}\n\n${lines.join("\n")}` : head;
}
