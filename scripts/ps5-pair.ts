// Send a model key to the console's setup screen from this computer: the same encrypted push as
// the setup page (renderer/assets/pair.html), for when the key is already here.
//
//   node scripts/ps5-pair.ts --host 192.168.0.56 [--provider openrouter|minimax|server|channel]
//                            [--model id] [--url http://<mac>:8790/agent | http://<mac>:8792]
//                            [--code 123456]
//
// The key comes from the environment (LLM_API_KEY for a model provider, AGENT_TOKEN for
// "server" or PS5_CHANNEL_TOKEN for "channel"), or from stdin with --key-stdin (e.g. `pbpaste | node scripts/ps5-pair.ts ... --key-stdin`).
// It's never printed. The code is the one on the TV; you're asked for it if --code isn't given.
import { readFileSync } from "node:fs";
import { createInterface } from "node:readline/promises";
import { runInThisContext } from "node:vm";

// The same tweetnacl-js file the console serves to the setup page. It's a browser/CommonJS script
// (this package is ESM), so it runs with a `self` that has Web Crypto, as in a browser.
const naclSource = readFileSync(new URL("../renderer/vendor/tweetnacl/nacl.min.js", import.meta.url), "utf8");
const holder: { crypto: Crypto; nacl?: any } = { crypto: globalThis.crypto };
runInThisContext(`(function (self) {${naclSource}\n})`)(holder);
const nacl = holder.nacl;

const args = process.argv.slice(2);
const arg = (name: string, fallback = "") => {
  const i = args.indexOf(`--${name}`);
  return i >= 0 && i + 1 < args.length ? args[i + 1] : fallback;
};
const host = arg("host", process.env.PS5_HOST ?? "");
const port = Number(arg("port", "8791"));
const provider = arg("provider", "openrouter");
if (!host || !["openrouter", "minimax", "server", "channel"].includes(provider)) {
  console.error("usage: node scripts/ps5-pair.ts --host <console ip> [--provider openrouter|minimax|server|channel] [--model id] [--url url] [--code 123456] [--key-stdin]");
  process.exit(2);
}

async function readStdin() {
  let text = "";
  for await (const chunk of process.stdin) text += chunk;
  return text.trim();
}
const envName = { openrouter: "LLM_API_KEY", minimax: "LLM_API_KEY", server: "AGENT_TOKEN", channel: "PS5_CHANNEL_TOKEN" }[provider]!;
const remote = provider === "server" || provider === "channel";
const key = args.includes("--key-stdin") ? await readStdin() : (process.env[envName] ?? "");
if (!key) {
  console.error(`No key: set ${envName}, or pipe it in with --key-stdin.`);
  process.exit(2);
}

const base = `http://${host}:${port}`;
let pk: Uint8Array;
try {
  const res = await fetch(`${base}/key`, { signal: AbortSignal.timeout(5000) });
  pk = Uint8Array.from(Buffer.from((await res.json()).pk, "base64"));
} catch {
  console.error(`Nothing answered at ${base}. Open the setup screen on the console (it's shown when there's no key).`);
  process.exit(1);
}

let code = arg("code");
if (!code) {
  // The prompt reads from the terminal even when the key came through stdin.
  const tty = process.stdin.isTTY ? process.stdin : (await import("node:fs")).createReadStream("/dev/tty");
  const rl = createInterface({ input: tty, output: process.stderr });
  code = (await rl.question("Code on the TV: ")).replace(/\s/g, "");
  rl.close();
  if (tty !== process.stdin) tty.destroy();
}

const mine = nacl.box.keyPair();
const nonce = nacl.randomBytes(nacl.box.nonceLength);
const message = { code, provider, key, model: remote ? "" : arg("model"), url: remote ? arg("url") : "" };
const box = nacl.box(new TextEncoder().encode(JSON.stringify(message)), nonce, pk, mine.secretKey);
const b64 = (b: Uint8Array) => Buffer.from(b).toString("base64");
const res = await fetch(`${base}/pair`, {
  method: "POST",
  headers: { "content-type": "application/json" },
  body: JSON.stringify({ pk: b64(mine.publicKey), nonce: b64(nonce), box: b64(box) }),
});
const body = await res.json().catch(() => ({}));
if (!res.ok) {
  console.error(body.error ?? `The console answered ${res.status}.`);
  process.exit(1);
}
console.error(`Done: the console is using ${body.provider}.`);
