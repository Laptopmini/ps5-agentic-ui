// ask_choices: an open app gets an in-app menu; a backgrounded app (presence stream) gets a
// notification whose buttons carry each option's value.
import assert from "node:assert/strict";
import { test } from "node:test";
import { consoleKey, ps5 } from "./adapter.ts";
import { ASK_CHOICES } from "./console-reads.ts";

// The first `data:` event on a stream (skipping `:` keep-alive/comment frames).
// The first `data:` event matching `want` (skipping `:` keep-alive and other frames).
const readData = async (res: Response, want: (e: Record<string, any>) => boolean = () => true): Promise<Record<string, any>> => {
  const reader = res.body!.getReader();
  let buf = "";
  for (;;) {
    const { value, done } = await reader.read();
    if (done) throw new Error("stream ended before a matching data frame");
    buf += new TextDecoder().decode(value);
    let i: number;
    while ((i = buf.indexOf("\n\n")) >= 0) {
      const frame = buf.slice(0, i);
      buf = buf.slice(i + 2);
      const line = frame.split("\n").find((l) => l.startsWith("data:"));
      if (line) {
        const e = JSON.parse(line.slice(5).trim());
        if (want(e)) return e;
      }
    }
  }
};

test("ask_choices: open app gets a menu, background app gets a choice notification", async () => {
  const port = 8901;
  const adapter = ps5({ port, agentName: "Agent", log: () => {} });
  await adapter.start({ onTurn: async () => {}, onInteraction: async () => {} } as never);
  const base = `http://127.0.0.1:${port}`;
  const key = consoleKey("t");
  const opts = [{ label: "Snooze", value: "Snooze 10 minutes" }, { label: "Dismiss", value: "Dismiss it" }];
  try {
    // No one connected: can't ask.
    assert.equal(adapter.askChoices(key, "Reminder?", opts), "queued");

    // Background payload connected: a ps5.notify with choices.
    const ctl = new AbortController();
    const presence = await fetch(`${base}/events?console=t&role=presence`, { signal: ctl.signal });
    const frame = readData(presence, (e) => e.name === "ps5.notify");
    assert.equal(adapter.askChoices(key, "Stretch reminder — what now?", opts), "notified");
    const ev = await frame;
    assert.equal(ev.name, "ps5.notify");
    assert.equal(ev.value.title, "Agent");
    assert.deepEqual(ev.value.choices, opts);
    ctl.abort();

    // App open: an in-app menu surface instead.
    const ctl2 = new AbortController();
    const app = await fetch(`${base}/events?console=t`, { signal: ctl2.signal });
    const mframe = readData(app, (e) => e.type === "ACTIVITY_SNAPSHOT");
    assert.equal(adapter.askChoices(key, "Pick a snack", opts), "menu");
    const surf = await mframe;
    const ops = surf.content.operations;
    const comps = ops[0].createSurface.components;
    const items = comps.filter((c: { component: string }) => c.component === "MenuItem");
    assert.equal(items.length, 2);
    assert.equal(items[0].action.event.name, "choice_select");
    assert.equal(items[0].action.event.context.value, "Snooze 10 minutes");
    assert.equal(items[0].action.event.userMessage, "Snooze 10 minutes");
    ctl2.abort();
  } finally {
    await adapter.stop();
  }
});

test("ASK_CHOICES.clean: labels required, value defaults to label, capped at 5", () => {
  const cleaned = ASK_CHOICES.clean({
    options: [{ label: " A ", value: " av " }, { label: "B" }, { value: "no label" }, { label: "C" }, { label: "D" }, { label: "E" }, { label: "F" }],
  });
  assert.equal(cleaned.length, 5);
  assert.deepEqual(cleaned[0], { label: "A", value: "av" });
  assert.deepEqual(cleaned[1], { label: "B", value: "B" });
});
