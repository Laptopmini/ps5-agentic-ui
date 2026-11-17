// The agent's run loop, with a scripted model: what each kind of tool call turns into, the
// approval gate, secret handles, client tools, and the HTTP server's access code.
import assert from "node:assert/strict";
import { test } from "node:test";
import { uiEnvelope } from "./a2ui.ts";
import type { Complete } from "./model.ts";
import { serveAgent } from "./serve.ts";
import { AgentThread } from "./thread.ts";
import type { AgentTool } from "./tools.ts";
import { validateSurface } from "./validate.ts";

type Json = Record<string, any>;

const say = (content: string): Json => ({ choices: [{ message: { role: "assistant", content } }] });
const call = (name: string, args: Json, id = `call_${name}`): Json => ({
  choices: [{ message: { role: "assistant", content: "", tool_calls: [{ id, type: "function", function: { name, arguments: JSON.stringify(args) } }] } }],
});

/** A model that gives the scripted replies in order, and records what it was sent. */
function scripted(...replies: Json[]) {
  const seen: { messages: Json[]; tools: string[] }[] = [];
  const complete: Complete = async (messages, tools, hooks) => {
    seen.push({ messages: structuredClone(messages), tools: tools.map((t) => t.function.name) });
    const reply = replies.shift();
    assert.ok(reply, "the model was called more often than scripted");
    const text = reply.choices[0].message.content;
    if (text) hooks?.onText?.(text);
    return reply;
  };
  return { complete, seen };
}

async function run(thread: AgentThread, input: Json): Promise<Json[]> {
  const events: Json[] = [];
  await thread.run({ threadId: thread.id, ...input }, (e) => events.push(e));
  return events;
}
const user = (content: string, id?: string) => ({ messages: [{ id, role: "user", content }] });
const press = (surfaceId: string, name: string, context: Json) => ({
  messages: [{ role: "user", content: "pressed" }],
  forwardedProps: { copilotkit: { ui: uiEnvelope({ version: "1.0", action: { name, surfaceId, sourceComponentId: "root", context } }) } },
});
const types = (events: Json[]) => events.map((e) => e.type);
const surfaces = (events: Json[]) => events.filter((e) => e.type === "ACTIVITY_SNAPSHOT" && e.activityType === "generative-ui-surface");
const results = (events: Json[]) => events.filter((e) => e.type === "TOOL_CALL_RESULT").map((e) => JSON.parse(e.content));
const lastToolMessage = (seen: { messages: Json[] }) => JSON.parse(seen.messages.findLast((m) => m.role === "tool")!.content);

test("a text answer streams between run start and finish", async () => {
  const { complete } = scripted(say("Hello from the couch."));
  const events = await run(new AgentThread("t", { model: complete }), user("hi"));
  assert.deepEqual(types(events), ["RUN_STARTED", "CUSTOM", "TEXT_MESSAGE_START", "TEXT_MESSAGE_CONTENT", "TEXT_MESSAGE_END", "RUN_FINISHED"]);
  assert.equal(events[3].delta, "Hello from the couch.");
});

test("nothing new to run is an error, and a message isn't run twice", async () => {
  const { complete } = scripted(say("One."));
  const thread = new AgentThread("t", { model: complete });
  await run(thread, user("hi", "m1"));
  assert.deepEqual(types(await run(thread, user("hi", "m1"))), ["RUN_STARTED", "RUN_ERROR"]);
});

test("an intent tool becomes a valid surface on the PS5 catalog and ends the run", async () => {
  const { complete, seen } = scripted(call("show_menu", { title: "Pick one", items: [{ id: "a", label: "First" }, { id: "b", label: "Second" }], followUps: ["x"] }));
  const events = await run(new AgentThread("t", { model: complete }), user("menu please"));
  const [surface] = surfaces(events);
  assert.equal(surface.content.surfaceId, "surface-call_show_menu");
  const { components, dataModel } = surface.content.operations[0].createSurface;
  assert.deepEqual(validateSurface(components, dataModel).errors, []);
  assert.deepEqual(results(events), [{ status: "rendered" }]);
  assert.equal(seen.length, 1, "the model isn't asked again after a screen");
  assert.equal(events.at(-1).type, "RUN_FINISHED");
});

test("a bad intent call goes back to the model, which can fix it in the same run", async () => {
  const { complete, seen } = scripted(call("show_menu", { title: "Empty", items: [] }, "bad"), call("show_menu", { title: "Fixed", items: [{ id: "a", label: "A" }] }, "good"));
  const events = await run(new AgentThread("t", { model: complete }), user("menu"));
  assert.deepEqual(results(events)[0], { error: "invalid_arguments", retryable: true, details: "items must not be empty" });
  assert.equal(surfaces(events).length, 1);
  assert.equal(lastToolMessage(seen[1]).error, "invalid_arguments");
});

test("the agent's own tool runs, and its result goes back to the model", async () => {
  const tool: AgentTool = { name: "add", description: "Add two numbers.", run: (a) => ({ sum: a.x + a.y }) };
  const { complete, seen } = scripted(call("add", { x: 2, y: 3 }), say("Five."));
  const events = await run(new AgentThread("t", { model: complete, tools: [tool] }), user("2+3"));
  assert.ok(seen[0].tools.includes("add") && seen[0].tools.includes("show_menu") && seen[0].tools.includes("suggest_follow_ups"));
  assert.deepEqual(results(events), [{ sum: 5 }]);
  assert.deepEqual(lastToolMessage(seen[1]), { sum: 5 });
});

test("a tool that throws becomes a tool error, not a failed run", async () => {
  const tool: AgentTool = { name: "boom", description: "Fails.", run: () => { throw new Error("no disk"); } };
  const { complete } = scripted(call("boom", {}), say("That failed."));
  const events = await run(new AgentThread("t", { model: complete, tools: [tool] }), user("go"));
  assert.deepEqual(results(events), [{ error: "tool_failed", details: "no disk" }]);
  assert.equal(events.at(-1).type, "RUN_FINISHED");
});

function gated() {
  const deleted: string[] = [];
  const tool: AgentTool = {
    name: "delete_save",
    description: "Delete a save.",
    approval: (a) => ({ message: `Delete ${a.id}?`, confirmLabel: "Delete", cancelLabel: "Keep", destructive: true }),
    run: (a) => { deleted.push(a.id); return { deleted: a.id }; },
  };
  return { tool, deleted };
}

test("a gated tool waits on the runtime's own confirm, and runs once approved", async () => {
  const { tool, deleted } = gated();
  const { complete, seen } = scripted(call("delete_save", { id: "s1" }, "c1"), say("Deleted."));
  const thread = new AgentThread("t", { model: complete, tools: [tool] });
  const first = await run(thread, user("delete s1"));
  const [confirm] = surfaces(first);
  const root = confirm.content.operations[0].createSurface.components[0];
  assert.equal(root.component, "Confirm");
  assert.equal(root.message, "Delete s1?");
  assert.deepEqual(validateSurface([root], {}).errors, []);
  assert.deepEqual(deleted, [], "nothing runs before the answer");
  assert.equal(seen.length, 1);

  const second = await run(thread, press("approval-c1", "approval_result", { confirmed: true }));
  assert.deepEqual(deleted, ["s1"]);
  assert.deepEqual(results(second), [{ deleted: "s1", approvedByUser: true }]);
  assert.ok(surfaces(second)[0].content.operations.some((o: Json) => o.deleteSurface), "the confirm is removed");
});

test("a denied or abandoned approval never runs the tool", async () => {
  for (const answer of [press("approval-c1", "approval_result", { confirmed: false }), user("never mind")]) {
    const { tool, deleted } = gated();
    const { complete, seen } = scripted(call("delete_save", { id: "s1" }, "c1"), say("OK."));
    const thread = new AgentThread("t", { model: complete, tools: [tool] });
    await run(thread, user("delete s1"));
    const events = await run(thread, answer);
    assert.deepEqual(deleted, []);
    assert.equal(results(events)[0].error, "denied_by_user");
    // Every tool call in the history has its result before the model is called again.
    const sent = seen[1].messages;
    assert.equal(sent.filter((m) => m.role === "tool" && m.tool_call_id === "c1").length, 1);
  }
});

test("two gated calls in one reply: one confirm, the other is told to wait", async () => {
  const { tool } = gated();
  const reply = call("delete_save", { id: "s1" }, "c1");
  reply.choices[0].message.tool_calls.push({ id: "c2", type: "function", function: { name: "delete_save", arguments: '{"id":"s2"}' } });
  const { complete } = scripted(reply);
  const events = await run(new AgentThread("t", { model: complete, tools: [tool] }), user("delete both"));
  assert.equal(surfaces(events).length, 1);
  assert.deepEqual(results(events), [{ error: "one_approval_at_a_time", retryable: true }]);
});

test("a typed password reaches the model as a handle, and the tool as the password", async () => {
  let got: unknown;
  const tool: AgentTool = { name: "connect_wifi", description: "Connect.", run: (a, ctx) => { got = ctx.secret(a.password); return { connected: true }; } };
  const { complete, seen } = scripted(call("ask_text", { title: "Wi-Fi password", kind: "password" }, "k1"), call("connect_wifi", { password: "secret://1" }), say("Connected."));
  const thread = new AgentThread("t", { model: complete, tools: [tool] });
  await run(thread, user("connect"));
  await run(thread, press("surface-k1", "text_submit", { value: "hunter2" }));
  assert.equal(got, "hunter2");
  for (const s of seen) assert.ok(!JSON.stringify(s.messages).includes("hunter2"), "the password never reaches the model");
  assert.ok(JSON.stringify(seen[1].messages).includes("secret://1"));
});

test("a client tool ends the run, and the next run continues from its result", async () => {
  const { complete, seen } = scripted(call("open_browser", { url: "https://example.com" }, "cl1"), say("Opened."));
  const thread = new AgentThread("t", { model: complete });
  const tools = [{ name: "open_browser", description: "Open a page on the client." }];
  const first = await run(thread, { ...user("open it"), tools });
  assert.deepEqual(results(first), [], "the client answers, not the agent");
  assert.equal(first.at(-1).type, "RUN_FINISHED");
  await run(thread, { messages: [{ role: "tool", toolCallId: "cl1", content: '{"opened":true}' }], tools });
  assert.deepEqual(lastToolMessage(seen[1]), { opened: true });
});

test("controller feedback becomes catalog function calls, and isn't offered without a controller", async () => {
  const { complete, seen } = scripted(call("controller_feedback", { haptic: "success" }), say("Done."), say("No controller."));
  const thread = new AgentThread("t", { model: complete });
  const events = await run(thread, user("buzz"));
  const fn = events.find((e) => e.activityType === "generative-ui-function-call");
  assert.deepEqual(fn?.content.callFunction["@call"], "haptic");
  assert.ok(seen[0].tools.includes("controller_feedback"));
  await run(thread, { ...user("again"), forwardedProps: { ps5Client: { controller: false } } });
  assert.ok(!seen[2].tools.includes("controller_feedback"));
});

test("a model that keeps calling tools is stopped", async () => {
  const tool: AgentTool = { name: "spin", description: "Spins.", run: () => ({}) };
  const { complete, seen } = scripted(call("spin", {}, "1"), call("spin", {}, "2"), call("spin", {}, "3"));
  const events = await run(new AgentThread("t", { model: complete, tools: [tool], maxCallsPerRun: 2 }), user("spin"));
  assert.equal(seen.length, 2);
  assert.equal(events.at(-2).value.state, "gave_up");
});

test("the server refuses to listen beyond loopback without an access code", () => {
  const thread = () => new AgentThread("t", { model: scripted().complete });
  assert.throws(() => serveAgent({ host: "0.0.0.0", thread }), /access code/);
  assert.throws(() => serveAgent({ host: "0.0.0.0", token: "short", thread }), /access code/);
});

test("the server checks the access code, the input, and streams a run as server-sent events", async () => {
  const token = "0123456789abcdef";
  const server = await serveAgent({ port: 0, token, thread: (id) => new AgentThread(id, { model: scripted(say("Hi.")).complete }) });
  const { port } = server.address() as { port: number };
  const url = `http://127.0.0.1:${port}`;
  try {
    assert.equal((await fetch(`${url}/health`)).status, 401);
    assert.equal((await fetch(`${url}/health`, { headers: { authorization: "Bearer 0123456789abcdeX" } })).status, 401);
    const auth = { authorization: `Bearer ${token}`, "content-type": "application/json" };
    assert.equal((await fetch(`${url}/health`, { headers: auth })).status, 200);
    assert.equal((await fetch(`${url}/agent`, { method: "POST", headers: auth, body: "{}" })).status, 400);
    assert.equal((await fetch(`${url}/nope`, { headers: auth })).status, 404);
    const res = await fetch(`${url}/agent`, { method: "POST", headers: auth, body: JSON.stringify({ threadId: "t1", messages: [{ role: "user", content: "hi" }] }) });
    assert.equal(res.headers.get("content-type"), "text/event-stream");
    const events = (await res.text()).split("\n\n").filter(Boolean).map((line) => JSON.parse(line.replace(/^data: /, "")));
    assert.deepEqual([events[0].type, events.at(-1).type], ["RUN_STARTED", "RUN_FINISHED"]);
  } finally {
    server.close();
  }
});
