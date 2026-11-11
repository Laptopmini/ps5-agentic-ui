// The streamed model reply: text as it comes (without <think> blocks), tool calls put together,
// and bodies that aren't streams (errors) passed through.
import assert from "node:assert/strict";
import { test } from "node:test";
import { StreamedReply } from "./llm.ts";

const sse = (...events: object[]) => events.map((e) => `data: ${JSON.stringify(e)}\n\n`).join("") + "data: [DONE]\n\n";
const delta = (d: object, finish: string | null = null) => ({ choices: [{ index: 0, delta: d, finish_reason: finish }] });

/** Feed `body` in pieces of `size` characters. */
function read(body: string, size: number) {
  const shown: string[] = [];
  const reply = new StreamedReply((t) => shown.push(t));
  for (let i = 0; i < body.length; i += size) reply.feed(body.slice(i, i + size));
  return { shown: shown.join(""), result: reply.result() };
}

test("text streams without think blocks or leading space, whatever the chunking", () => {
  const body = sse(delta({ content: "<thi" }), delta({ content: "nk>plan the shelf</th" }), delta({ content: "ink>\n\nHere are " }), delta({ content: "your games." }, "stop"));
  for (const size of [1, 3, 7, 50, 10000]) {
    const { shown, result } = read(body, size);
    assert.equal(shown, "Here are your games.", `chunks of ${size}`);
    assert.equal(result.choices[0].message.content, "<think>plan the shelf</think>\n\nHere are your games.");
  }
});

test("a lone '<' that isn't a think tag is shown", () => {
  assert.equal(read(sse(delta({ content: "1 <" }), delta({ content: " 2" })), 4).shown, "1 < 2");
});

test("tool calls are put together from their pieces", () => {
  const body = sse(
    delta({ content: "Opening it." }),
    delta({ tool_calls: [{ index: 0, id: "call_1", type: "function", function: { name: "launch_", arguments: "" } }] }),
    delta({ tool_calls: [{ index: 0, function: { name: "game", arguments: '{"id":' } }] }),
    delta({ tool_calls: [{ index: 0, function: { arguments: '"elden-ring"}' } }] }),
    delta({ tool_calls: [{ index: 1, id: "call_2", function: { name: "notify", arguments: '{"text":"ok"}' } }] }, "tool_calls"),
  );
  const { result } = read(body, 5);
  const calls = result.choices[0].message.tool_calls;
  assert.deepEqual(calls.map((c: any) => [c.id, c.function.name, c.function.arguments]), [
    ["call_1", "launch_game", '{"id":"elden-ring"}'],
    ["call_2", "notify", '{"text":"ok"}'],
  ]);
  assert.equal(result.choices[0].finish_reason, "tool_calls");
});

test("a body that isn't a stream (MiniMax's rate limit) comes back as it is", () => {
  const body = JSON.stringify({ base_resp: { status_code: 1002, status_msg: "rate limit" } });
  const { shown, result } = read(body, 8);
  assert.equal(shown, "");
  assert.equal(result.base_resp.status_code, 1002);
});
