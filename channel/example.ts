// A minimal Channels channel for the PS5: the console talks to the PS5 agent (server/index.ts)
// through CopilotKit Channels instead of directly.
//
//   AGENT_TOKEN=... PS5_CHANNEL_TOKEN=... node channel/example.ts [--host 0.0.0.0] [--port 8792]
//        [--agent http://127.0.0.1:8790/agent] [--name "Agent name for the console's header"]
//        [--channel <code>]   (or CHANNEL_CODE) [--runtime-port 8793]   (with Intelligence, see below)
//
// Every message runs the agent; its screens reach the console as they are. "demo" instead posts
// a message built with Channels' own UI components (rendered to the PS5 catalog by the adapter),
// and a press on it answers through a Channels action handler. The agent also gets the console's
// own data as client tools (ps5ConsoleTools: list_games reads the console's installed games). "remind me in 30 seconds to …"
// posts later, outside any turn: the test for background presence. Close the app first and the
// console's background payload shows the reminder as a toast.
//
// Two ways to run it:
// - With CPK_INTELLIGENCE_API_KEY (a CopilotKit Intelligence project): conversations are recorded
//   in Intelligence (threads in the dashboard, and what its memory and learning work from). The
//   channel's agent is then CopilotKit's own runtime endpoint (loopback only, --runtime-port)
//   instead of the PS5 agent: the runtime stores each turn and runs it through Intelligence, which
//   calls the PS5 agent. The same runtime starts the channel as CHANNEL_CODE ("ps5-agentic-ui" by
//   default). Intelligence's dashboard only registers Slack and Teams channels, so it reports the
//   PS5 channel as "setup_required" and the runtime starts the PS5 adapter itself.
// - Without a key: the channel calls the PS5 agent directly, started with the SDK's own runner
//   hook (channel.ɵruntime.start(), marked internal); CopilotRuntime only starts channels with
//   Intelligence configured.
// Telemetry: CopilotKit's anonymous telemetry is off unless COPILOTKIT_TELEMETRY_DISABLED=false.
process.env.COPILOTKIT_TELEMETRY_DISABLED ??= "true";

const { Actions, Button, Context, createChannel, Field, Fields, Header, HttpAgent, Message, Section } = await import("@copilotkit/channels-core");
const { handlePs5Actions, ps5, ps5ConsoleTools } = await import("./adapter.ts");

const arg = (name: string, fallback: string) => {
  const i = process.argv.indexOf(`--${name}`);
  return i > 0 ? process.argv[i + 1] : fallback;
};
const agentUrl = arg("agent", "http://127.0.0.1:8790/agent");
const agentToken = process.env.AGENT_TOKEN ?? "";

const intelligenceKey = process.env.CPK_INTELLIGENCE_API_KEY ?? "";
const channelCode = arg("channel", process.env.CHANNEL_CODE ?? "ps5-agentic-ui");
const runtimePort = Number(arg("runtime-port", "8793"));
const runtimeUrl = `http://127.0.0.1:${runtimePort}`;
const makeAgent = (threadId?: string) =>
  new HttpAgent({ url: agentUrl, threadId, headers: agentToken ? { authorization: `Bearer ${agentToken}` } : {} });
// With Intelligence, the client CopilotKit's React SDK uses, pointed at the runtime below.
const { ProxiedCopilotRuntimeAgent } = intelligenceKey ? await import("@copilotkit/core") : { ProxiedCopilotRuntimeAgent: undefined };

const adapter = ps5({ host: arg("host", "127.0.0.1"), port: Number(arg("port", "8792")), token: process.env.PS5_CHANNEL_TOKEN ?? "", agentName: arg("name", "") || undefined });
const channel = createChannel({
  name: channelCode,
  identifyUser: "platform",
  adapters: [adapter],
  agent: (threadId: string) =>
    ProxiedCopilotRuntimeAgent ? new ProxiedCopilotRuntimeAgent({ runtimeUrl, agentId: "default", threadId }) : makeAgent(threadId),
});

// Answers to the demo message: a Channels action handler, no agent involved.
const answer = async ({ thread, action }: { thread: { post: (ui: unknown) => Promise<unknown> }; action: { value?: unknown } }) => {
  await thread.post(`You picked ${String(action.value)}. That answer came back through a Channels action handler.`);
};

channel.onMessage(async ({ thread, message }) => {
  const remind = /^remind me in (\d{1,4})\s*(s|secs?|seconds?|m|mins?|minutes?)\b\s*(?:to\s+)?(.*)$/i.exec(message.text.trim());
  if (remind) {
    const seconds = Number(remind[1]) * (remind[2].toLowerCase().startsWith("m") ? 60 : 1);
    const what = remind[3].trim() || "you asked me to remind you";
    await thread.post(`OK, in ${seconds} seconds. You can close the app; I'll send a notification.`);
    // Channels has no public way to post outside a handler; the thread object still posts.
    setTimeout(() => void thread.post(`Reminder: ${what}`).catch((e: Error) => console.log(`[ps5] reminder failed: ${e.message}`)), seconds * 1000);
    return;
  }
  if (message.text.trim().toLowerCase() === "demo") {
    await thread.post(
      Message({
        children: [
          Header({ children: "Channels on the PS5" }),
          Section({ children: "This message was built with CopilotKit Channels components and drawn by the console as a native screen." }),
          Fields({ children: [Field({ label: "Adapter", children: "copilotkit-channels-ps5" }), Field({ label: "Catalog", children: "PS5 v1" })] }),
          Actions({
            children: [
              Button({ value: "native screens", onClick: answer, children: "Native screens" }),
              Button({ value: "the controller", onClick: answer, children: "The controller" }),
            ],
          }),
          Context({ children: "Pick one." }),
        ],
      }),
    );
    return;
  }
  await thread.runAgent();
});
handlePs5Actions(channel);
// The console's own data (its installed games) as tools the agent can call; the console answers.
for (const tool of ps5ConsoleTools(adapter)) channel.tool(tool as never);

let stop: () => Promise<unknown>;
if (intelligenceKey) {
  const { CopilotKitIntelligence, CopilotRuntime } = await import("@copilotkit/runtime/v2");
  const { createCopilotNodeListener } = await import("@copilotkit/runtime/v2/node");
  const { createServer } = await import("node:http");
  const runtime = new CopilotRuntime({
    // apiUrl and wsUrl default to CopilotKit's hosted Intelligence; override both or neither.
    intelligence: new CopilotKitIntelligence({
      apiKey: intelligenceKey,
      ...(process.env.INTELLIGENCE_API_URL && process.env.INTELLIGENCE_GATEWAY_WS_URL
        ? { apiUrl: process.env.INTELLIGENCE_API_URL, wsUrl: process.env.INTELLIGENCE_GATEWAY_WS_URL }
        : {}),
    }),
    agents: { default: makeAgent() },
    // Who the conversations belong to in Intelligence: one player for this channel.
    identifyUser: () => ({ id: "ps5-player", name: "PS5 player" }),
    channels: [channel],
  });
  // Creating the listener starts the channel; ready() fails loudly on a broken config. Its HTTP
  // endpoints are the channel's agent, on this computer only.
  const listener = createCopilotNodeListener({ runtime });
  const server = createServer(listener);
  await new Promise<void>((resolve) => server.listen(runtimePort, "127.0.0.1", resolve));
  await listener.channels.ready();
  const status = listener.channels.status().channels[channelCode];
  console.log(`[ps5] channel ready: "${channelCode}" through CopilotKit Intelligence (conversations recorded; channel status ${status}); runtime ${runtimeUrl}, agent ${agentUrl}`);
  stop = async () => {
    await listener.channels.stop();
    server.close();
  };
} else {
  const runner = (channel as unknown as { ɵruntime: { start(): Promise<void>; stop(): Promise<void> } }).ɵruntime;
  await runner.start();
  console.log(`[ps5] channel ready: "${channelCode}", run on its own (no Intelligence); agent ${agentUrl}`);
  stop = () => runner.stop();
}
for (const signal of ["SIGINT", "SIGTERM"] as const)
  process.on(signal, async () => {
    await stop();
    process.exit(0);
  });
