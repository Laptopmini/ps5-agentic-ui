// The agent runtime for embedding (QuickJS on the console): the same AgentThread the Node server
// runs, without HTTP. The host provides fetch, setTimeout, crypto.randomUUID and process.env
// (see renderer/src/js_agent.cpp), then calls
//   agent.run(runAgentInputJson, emit(eventJson), model) → Promise
// and gets the same AG-UI events the server streams.
import { AgentThread, type RunAgentInput } from "./thread.ts";

const threads = new Map<string, AgentThread>();

(globalThis as any).agent = {
  run(json: string, emit: (event: string) => void, model: string): Promise<void> {
    const input: RunAgentInput = JSON.parse(json);
    let thread = threads.get(input.threadId);
    if (!thread) {
      // The console app provides __ps5System (renderer/src/js_agent.cpp) to read the console itself.
      thread = new AgentThread(input.threadId, model, (globalThis as any).__ps5System);
      threads.set(input.threadId, thread);
    }
    if (thread.running) {
      emit(JSON.stringify({ type: "RUN_ERROR", message: "A run is already in progress on this thread." }));
      return Promise.resolve();
    }
    return thread.run(input, (event) => emit(JSON.stringify(event)));
  },
};
