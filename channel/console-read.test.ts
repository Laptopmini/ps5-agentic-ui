// The channel asking the console about itself: a ps5.request down the console's stream, the
// answer posted to /reply.
import assert from "node:assert/strict";
import { test } from "node:test";
import { consoleKey, ps5 } from "./adapter.ts";

test("a console read goes down the stream and comes back from /reply", async () => {
  const port = 8899;
  const adapter = ps5({ port, log: () => {} });
  await adapter.start({ onTurn: async () => {}, onInteraction: async () => {} } as never);
  const base = `http://127.0.0.1:${port}`;
  const key = consoleKey("t");
  try {
    assert.deepEqual(await adapter.consoleRead(key, "installed_titles"), { error: "console_not_connected" });

    // The console: its stream, then a hello naming what it can read.
    const ctl = new AbortController();
    const res = await fetch(`${base}/events?console=t`, { signal: ctl.signal });
    const reader = res.body!.getReader();
    const post = (path: string, body: object) =>
      fetch(`${base}${path}`, { method: "POST", headers: { "content-type": "application/json" }, body: JSON.stringify({ console: "t", ...body }) });
    await post("/hello", { client: { name: "test", reads: [] } });
    assert.equal((await adapter.consoleRead(key, "installed_titles")).error, "not_supported_by_console");
    await post("/hello", { client: { name: "test", reads: ["installed_titles"] } });

    // Answer the first request that arrives.
    const answering = (async () => {
      let buf = "";
      for (;;) {
        const { value, done } = await reader.read();
        if (done) return;
        buf += new TextDecoder().decode(value);
        const m = /data: (\{"type":"CUSTOM","name":"ps5\.request".*)\n/.exec(buf);
        if (m) {
          const { id, name } = JSON.parse(m[1]).value;
          assert.equal(name, "installed_titles");
          await post("/reply", { id, result: { games: [{ id: "PPSA01325", title: "Astro's Playroom", platform: "PS5" }] } });
          return;
        }
      }
    })();
    const result = await adapter.consoleRead(key, "installed_titles");
    await answering;
    assert.equal(result.games[0].title, "Astro's Playroom");
    assert.equal((await post("/reply", { id: "nobody-asked", result: {} })).status, 404);
    ctl.abort();
  } finally {
    await adapter.stop();
  }
});
