// Spike: do the PS5 channel's turns reach CopilotKit Intelligence (threads, learning) when a key is
// set? A direct adapter's turns skip Intelligence (docs/channel.md). Here the channel's agent is
// CopilotKit's own runtime endpoint instead of the PS5 agent: the runtime (Intelligence mode,
// loopback only) records each turn as a thread and runs it through IntelligenceAgentRunner,
// which calls the PS5 agent (server/index.ts).
//
//   CPK_INTELLIGENCE_API_KEY=... node eval/intelligence-threads/run.ts [--port 8797] [--runtime-port 8793]
//        [--agent http://127.0.0.1:8790/agent]
//
// Everything used is public API: CopilotRuntime, CopilotKitIntelligence, createCopilotNodeListener
// (@copilotkit/runtime/v2) and ProxiedCopilotRuntimeAgent (@copilotkit/core, the client the
// React SDK uses).
process.env.COPILOTKIT_TELEMETRY_DISABLED ??= "true";
import { createServer } from "node:http";

const { createChannel, HttpAgent } = await import("@copilotkit/channels-core");
const { CopilotKitIntelligence, CopilotRuntime } = await import("@copilotkit/runtime/v2");
const { createCopilotNodeListener } = await import("@copilotkit/runtime/v2/node");
const { ProxiedCopilotRuntimeAgent } = await import("@copilotkit/core");
const { handlePs5Actions, ps5 } = await import("../../channel/adapter.ts");

const arg = (name: string, fallback: string) => {
  const i = process.argv.indexOf(`--${name}`);
  return i > 0 ? process.argv[i + 1] : fallback;
};
const apiKey = process.env.CPK_INTELLIGENCE_API_KEY;
if (!apiKey) throw new Error("Set CPK_INTELLIGENCE_API_KEY.");
const agentUrl = arg("agent", "http://127.0.0.1:8790/agent");
const runtimePort = Number(arg("runtime-port", "8793"));
const runtimeUrl = `http://127.0.0.1:${runtimePort}`;

// The runtime: Intelligence-backed, one agent ("default", the PS5 agent), one user.
const runtime = new CopilotRuntime({
  intelligence: new CopilotKitIntelligence({ apiKey }),
  agents: { default: new HttpAgent({ url: agentUrl }) },
  identifyUser: () => ({ id: "ps5-player", name: "PS5 player" }),
});
const listener = createCopilotNodeListener({ runtime });
await new Promise<void>((resolve) => createServer(listener).listen(runtimePort, "127.0.0.1", resolve));
console.log(`[spike] CopilotKit runtime (Intelligence) on ${runtimeUrl}`);

const channel = createChannel({
  name: "ps5-agentic-ui",
  identifyUser: "platform",
  adapters: [ps5({ port: Number(arg("port", "8797")), agentName: "PS5 A2UI" })],
  agent: (threadId: string) => new ProxiedCopilotRuntimeAgent({ runtimeUrl, agentId: "default", threadId }),
});
channel.onMessage(async ({ thread }) => {
  await thread.runAgent();
});
handlePs5Actions(channel);
await (channel as unknown as { ɵruntime: { start(): Promise<void> } }).ɵruntime.start();
console.log("[spike] channel ready");
