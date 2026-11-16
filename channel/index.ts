// copilotkit-ps5: a CopilotKit Channels PlatformAdapter that turns a PlayStation 5 into
// an agent surface — native A2UI screens, the DualSense, haptics and the system keyboard. The
// public API is the adapter (`ps5()` / `Ps5Adapter`) plus the Channels→A2UI render helper.
export * from "./adapter.ts";
export { toSurface, CHANNEL_ACTION } from "./render.ts";
export type { RenderedSurface } from "./render.ts";
