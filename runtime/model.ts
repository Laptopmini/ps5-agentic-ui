// The model behind the agent: any endpoint that speaks OpenAI's chat completions with tool calls
// (OpenRouter by default). `openAICompatible` makes the call, with retries and streaming;
// `StreamedReply` puts a streamed reply back together.
type Json = Record<string, any>;

export const DEFAULT_BASE_URL = "https://openrouter.ai/api/v1";

/** Where the model is: any endpoint that speaks OpenAI's chat completions, with tool calls. */
export interface ModelConfig {
  /** The endpoint, up to and without /chat/completions. OpenRouter when unset. */
  baseUrl?: string;
  apiKey: string;
  /** The model id, as the endpoint names it. */
  model: string;
  /** Extra fields for the request body, for provider-specific switches (e.g. {"reasoning": {...}}). */
  extraBody?: Json;
  /** Give up on a call after this long (5 minutes by default). */
  timeoutMs?: number;
}

export interface CompleteHooks {
  /** Told before each wait (rate limit or transient error), e.g. to show "retrying" in the UI. */
  onRetry?: (waitMs: number, reason: string) => void;
  /** The reply streams: visible text (outside `<think>` blocks) arrives here as it's written. */
  onText?: (delta: string) => void;
}

/**
 * One model call: OpenAI-style messages and tools in, the chat-completion body out
 * ({choices: [{message: {content, tool_calls}}], usage}). AgentThread takes any function of this
 * shape, so a test or another SDK can stand in for `openAICompatible`.
 */
export type Complete = (messages: Json[], tools: Json[], hooks?: CompleteHooks) => Promise<Json>;

/**
 * The model settings from the environment: LLM_API_KEY, LLM_MODEL, LLM_BASE_URL (OpenRouter when
 * unset) and LLM_EXTRA_BODY (a JSON object merged into every request). `overrides` win.
 */
export function modelFromEnv(overrides: Partial<ModelConfig> = {}, env: Record<string, string | undefined> = process.env): ModelConfig {
  const apiKey = overrides.apiKey ?? env.LLM_API_KEY;
  const model = overrides.model ?? env.LLM_MODEL;
  if (!apiKey) throw new Error("Set LLM_API_KEY: a key for any OpenAI-compatible endpoint (OpenRouter by default; set LLM_BASE_URL for another).");
  if (!model) throw new Error("Set LLM_MODEL to the id of a model that supports tool calls, as your endpoint names it.");
  let extraBody = overrides.extraBody;
  if (!extraBody && env.LLM_EXTRA_BODY) {
    try {
      extraBody = JSON.parse(env.LLM_EXTRA_BODY);
    } catch {
      throw new Error("LLM_EXTRA_BODY is not a JSON object.");
    }
  }
  return { ...overrides, baseUrl: overrides.baseUrl ?? env.LLM_BASE_URL ?? DEFAULT_BASE_URL, apiKey, model, extraBody };
}

/**
 * A `Complete` for an OpenAI-compatible endpoint. Rate limits and transient errors are retried
 * (up to five times), but only while nothing has been shown: once text has streamed, a failure
 * is final.
 */
export function openAICompatible(config: ModelConfig): Complete {
  const url = `${(config.baseUrl ?? DEFAULT_BASE_URL).replace(/\/+$/, "")}/chat/completions`;
  return async (messages, tools, hooks = {}) => {
    const { onRetry, onText } = hooks;
    for (let attempt = 0; ; attempt++) {
      let status = 0;
      let body: any;
      let shown = false;
      try {
        const request = {
          method: "POST",
          headers: { "content-type": "application/json", authorization: `Bearer ${config.apiKey}` },
          body: JSON.stringify({ model: config.model, messages, tools, tool_choice: "auto", max_tokens: 8000, ...config.extraBody, ...(onText ? { stream: true } : {}) }),
          signal: AbortSignal.timeout(config.timeoutMs ?? 300_000),
        };
        if (onText) {
          const reply = new StreamedReply((delta) => {
            shown = true;
            onText(delta);
          });
          status = await fetchChunks(url, request, (chunk) => reply.feed(chunk));
          body = reply.result();
        } else {
          const res = await fetch(url, request);
          status = res.status;
          body = await res.json().catch(() => ({}));
        }
      } catch (e) {
        body = { error: String((e as Error).cause ?? e) };
      }
      const ok = status >= 200 && status < 300;
      // Some providers answer 200 with an error inside the body (`error`, or `base_resp.status_code`).
      if (ok && !body?.base_resp?.status_code && !body?.error) return body;
      const limited = status === 429 || body?.base_resp?.status_code === 1002;
      const transient = !status || limited || status >= 500;
      if (!transient || attempt >= 5 || shown) throw new Error(`${status || "network"} ${JSON.stringify(body).slice(0, 300)}`);
      const waitMs = limited ? 15_000 * 2 ** Math.min(attempt, 3) : 2000 * (attempt + 1);
      onRetry?.(waitMs, limited ? "rate_limited" : status ? `http_${status}` : "network");
      await new Promise((r) => setTimeout(r, waitMs));
    }
  };
}

/**
 * POST and hand the response body to `onChunk` as it arrives; resolves with the HTTP status. On
 * the console (QuickJS) the host's fetch delivers the chunks (__host.fetch's chunk callback).
 */
async function fetchChunks(url: string, init: Json, onChunk: (text: string) => void): Promise<number> {
  const host = (globalThis as any).__fetchChunks;
  if (host) return host(url, init, onChunk);
  const res = await fetch(url, init);
  const reader = res.body!.getReader();
  const decoder = new TextDecoder();
  for (;;) {
    const { value, done } = await reader.read();
    if (done) break;
    onChunk(decoder.decode(value, { stream: true }));
  }
  onChunk(decoder.decode());
  return res.status;
}

/**
 * Reads an OpenAI-compatible streamed reply (server-sent `chat.completion.chunk`s) into the
 * non-streamed shape: { choices: [{ message: { content, tool_calls } }] }. Visible text goes to
 * `onText` as it comes, without `<think>…</think>` and leading whitespace. A body that isn't a
 * stream (an error body) is returned as it is.
 */
export class StreamedReply {
  private buffer = "";
  private raw = "";
  private events = 0;
  private content = "";
  private toolCalls: Json[] = [];
  private finishReason: string | null = null;
  private pending = "";  // text held back: maybe the start of a <think> tag
  private inThink = false;
  private started = false;

  private readonly onText: (delta: string) => void;

  constructor(onText: (delta: string) => void) {
    this.onText = onText;
  }

  feed(chunk: string) {
    if (this.raw.length < 65536) this.raw += chunk;
    this.buffer += chunk;
    for (let i; (i = this.buffer.indexOf("\n")) >= 0; ) {
      const line = this.buffer.slice(0, i).replace(/\r$/, "");
      this.buffer = this.buffer.slice(i + 1);
      if (!line.startsWith("data:")) continue;
      const data = line.slice(5).trim();
      if (!data || data === "[DONE]") continue;
      let event: Json;
      try {
        event = JSON.parse(data);
      } catch {
        continue;
      }
      this.events++;
      if (event.base_resp?.status_code) this.raw = data;  // an error inside the stream
      const choice = event.choices?.[0];
      if (!choice) continue;
      if (choice.finish_reason) this.finishReason = choice.finish_reason;
      const delta = choice.delta ?? {};
      if (typeof delta.content === "string" && delta.content) {
        this.content += delta.content;
        this.visible(delta.content);
      }
      for (const tc of delta.tool_calls ?? []) {
        const index = tc.index ?? this.toolCalls.length;
        const call = (this.toolCalls[index] ??= { id: "", type: "function", function: { name: "", arguments: "" } });
        if (tc.id) call.id = tc.id;
        if (tc.function?.name) call.function.name += tc.function.name;
        if (tc.function?.arguments) call.function.arguments += tc.function.arguments;
      }
    }
  }

  result(): Json {
    if (!this.events || /"base_resp"\s*:\s*\{\s*"status_code"\s*:\s*[1-9]/.test(this.raw)) {
      try {
        return JSON.parse(this.raw);
      } catch {
        return { error: this.raw.slice(0, 300) || "empty reply" };
      }
    }
    if (!this.inThink && this.pending) this.emit(this.pending);
    this.pending = "";
    const toolCalls = this.toolCalls.filter(Boolean);
    return { choices: [{ message: { role: "assistant", content: this.content, ...(toolCalls.length ? { tool_calls: toolCalls } : {}) }, finish_reason: this.finishReason }] };
  }

  /** Passes text on, minus <think>…</think> (tags may be split across deltas). */
  private visible(text: string) {
    let s = this.pending + text;
    this.pending = "";
    for (;;) {
      if (this.inThink) {
        const end = s.indexOf("</think>");
        if (end < 0) {
          this.pending = s.slice(-7);  // may hold the start of "</think>"
          return;
        }
        s = s.slice(end + 8);
        this.inThink = false;
      }
      const start = s.indexOf("<think>");
      if (start >= 0) {
        this.emit(s.slice(0, start));
        s = s.slice(start + 7);
        this.inThink = true;
        continue;
      }
      // Hold back a tail that could be the start of "<think>".
      let keep = 0;
      for (let k = Math.min(6, s.length); k > 0; k--)
        if ("<think>".startsWith(s.slice(-k))) {
          keep = k;
          break;
        }
      this.emit(s.slice(0, s.length - keep));
      this.pending = s.slice(s.length - keep);
      return;
    }
  }

  private emit(text: string) {
    if (!this.started) text = text.replace(/^\s+/, "");
    if (!text) return;
    this.started = true;
    this.onText(text);
  }
}
