// The model settings the eval harnesses share: any OpenAI-compatible endpoint, the same
// variables as the agent (LLM_API_KEY, LLM_MODEL, LLM_BASE_URL, LLM_EXTRA_BODY).
import { DEFAULT_BASE_URL } from "../../runtime/model.ts";

export const arg = (name: string, fallback: string) => {
  const i = process.argv.indexOf(`--${name}`);
  return i > 0 ? process.argv[i + 1] : fallback;
};

export const BASE_URL = (process.env.LLM_BASE_URL ?? DEFAULT_BASE_URL).replace(/\/+$/, "");
export const KEY = process.env.LLM_API_KEY;
if (!KEY) throw new Error("Set LLM_API_KEY: a key for any OpenAI-compatible endpoint (OpenRouter by default; set LLM_BASE_URL for another).");

/** The models to run: --models a,b, else LLM_MODEL. */
export const models = arg("models", process.env.LLM_MODEL ?? "").split(",").filter(Boolean);
if (!models.length) throw new Error("Name the model(s) to evaluate: --models a,b or LLM_MODEL.");

/** Extra request-body fields for provider-specific switches (LLM_EXTRA_BODY, a JSON object). */
export const EXTRA_BODY: Record<string, unknown> = process.env.LLM_EXTRA_BODY ? JSON.parse(process.env.LLM_EXTRA_BODY) : {};
