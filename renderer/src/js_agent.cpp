#include "js_agent.hpp"

#include <curl/curl.h>

#include <chrono>
#include <functional>
#include <random>
#include <stdexcept>
#include <mutex>
#include <thread>

extern "C" {
#include "quickjs/quickjs.h"
}

namespace a2ui {
namespace {

// What Node gives the runtime, built on the __host functions below.
const char* kPrelude = R"JS(
globalThis.AbortSignal = { timeout: (ms) => ({ timeoutMs: ms }) };
// Minimal URL (absolute, ASCII hosts): Ajv's URI library uses it to normalise $ref hosts.
globalThis.URL = globalThis.URL || class URL {
  constructor(input, base) {
    const m = /^([a-z][a-z0-9+.-]*:)\/\/(?:([^:@\/]*)(?::([^@\/]*))?@)?([^:\/?#]*)(?::(\d*))?([^?#]*)(\?[^#]*)?(#.*)?$/i.exec(String(input));
    if (!m || base !== undefined || /[^\x00-\x7f]/.test(m[4])) throw new TypeError("Invalid URL (the console runtime's URL takes absolute ASCII URLs): " + input);
    this.protocol = m[1].toLowerCase(); this.username = m[2] || ""; this.password = m[3] || "";
    this.hostname = m[4].toLowerCase(); this.port = m[5] || ""; this.pathname = m[6] || "/";
    this.search = m[7] || ""; this.hash = m[8] || "";
    this.host = this.hostname + (this.port ? ":" + this.port : "");
    this.href = this.protocol + "//" + (this.username ? this.username + (this.password ? ":" + this.password : "") + "@" : "") + this.host + this.pathname + this.search + this.hash;
  }
  toString() { return this.href; }
};
globalThis.fetch = (url, init = {}) => new Promise((resolve, reject) => {
  __host.fetch(String(url), init.method || "GET", JSON.stringify(init.headers || {}),
    init.body == null ? "" : String(init.body), (init.signal && init.signal.timeoutMs) || 0,
    (status, text, headers) => {
      if (status < 0) { reject(new TypeError("fetch failed: " + text)); return; }
      const h = JSON.parse(headers);
      resolve({ ok: status >= 200 && status < 300, status, headers: { get: (n) => h[String(n).toLowerCase()] ?? null },
                text: async () => text, json: async () => JSON.parse(text) });
    });
});
// The response body in pieces as it arrives (a streamed model reply); resolves with the status.
globalThis.__fetchChunks = (url, init, onChunk) => new Promise((resolve, reject) => {
  __host.fetch(String(url), init.method || "GET", JSON.stringify(init.headers || {}),
    init.body == null ? "" : String(init.body), (init.signal && init.signal.timeoutMs) || 0,
    (status, text) => (status < 0 ? reject(new TypeError("fetch failed: " + text)) : resolve(status)),
    (text) => onChunk(text));
});
// Reading the console itself, when the host can (the console app): the agent's real console tools.
if (__host.system) globalThis.__ps5System = (name) => JSON.parse(__host.system(String(name)));
globalThis.setTimeout = (fn, ms, ...args) => __host.setTimeout(() => fn(...args), Number(ms) || 0);
globalThis.clearTimeout = (id) => __host.clearTimeout(id);
globalThis.crypto = { randomUUID: () => __host.uuid() };
const __fmt = (a) => a.map((x) => (typeof x === "string" ? x : (() => { try { return JSON.stringify(x); } catch { return String(x); } })())).join(" ");
globalThis.console = { log: (...a) => __host.log(__fmt(a)), info: (...a) => __host.log(__fmt(a)), warn: (...a) => __host.log(__fmt(a)), error: (...a) => __host.log(__fmt(a)) };
globalThis.process = { env: JSON.parse(__host.env), stderr: { write: (s) => __host.log(String(s)) } };
globalThis.__run = (json, model) => agent.run(json, (e) => __host.emit(e), model)
  .then(() => __host.done(""), (e) => __host.done(String((e && e.stack) || e)));
)JS";

std::string uuid4(std::mt19937_64& gen) {
  uint64_t a = gen(), b = gen();
  a = (a & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;  // version 4
  b = (b & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;  // variant 1
  char s[40];
  std::snprintf(s, sizeof s, "%08x-%04x-%04x-%04x-%012llx", unsigned(a >> 32), unsigned((a >> 16) & 0xFFFF), unsigned(a & 0xFFFF), unsigned(b >> 48),
                static_cast<unsigned long long>(b & 0xFFFFFFFFFFFFULL));
  return s;
}

/** How much of `s` ends on a whole UTF-8 character (a chunk may stop inside one). */
size_t wholeUtf8(const std::string& s) {
  size_t end = s.size(), k = 0;
  while (k < 4 && end > k && (static_cast<unsigned char>(s[end - 1 - k]) & 0xC0) == 0x80) ++k;
  if (end == k) return end;
  const unsigned char lead = static_cast<unsigned char>(s[end - 1 - k]);
  const size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
  return need == k + 1 ? end : end - 1 - k;
}

size_t onHeader(char* data, size_t size, size_t count, void* user) {
  std::string line(data, size * count);
  auto colon = line.find(':');
  if (colon != std::string::npos) {
    std::string name = line.substr(0, colon), value = line.substr(colon + 1);
    for (char& c : name) c = char(std::tolower(static_cast<unsigned char>(c)));
    while (!value.empty() && (value.front() == ' ')) value.erase(0, 1);
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) value.pop_back();
    (*static_cast<json*>(user))[name] = value;
  }
  return size * count;
}

}  // namespace

struct JsAgent::Impl {
  JsAgent* self;
  Options opt;
  JSRuntime* rt = nullptr;
  JSContext* ctx = nullptr;
  std::mt19937_64 gen{std::random_device{}()};

  struct Timer {
    int id;
    std::chrono::steady_clock::time_point due;
    JSValue fn;
  };
  std::vector<Timer> timers;
  int nextTimer = 1;

  struct Fetch {
    int status = -1;
    std::string body, error;
    json headers = json::object();
    std::atomic<bool> finished{false};
    JSValue callback;
    JSValue onChunk = JS_UNDEFINED;  // set: the body goes to it in pieces, not to `callback`
    std::mutex mutex;                // guards body while streaming
    std::thread worker;

    static size_t onBody(char* data, size_t size, size_t count, void* user) {
      auto* f = static_cast<Fetch*>(user);
      std::lock_guard lock(f->mutex);
      f->body.append(data, size * count);
      return size * count;
    }
    /** What has arrived, up to a whole UTF-8 character (everything once `all`). */
    std::string take(bool all) {
      std::lock_guard lock(mutex);
      const size_t n = all ? body.size() : wholeUtf8(body);
      std::string out = body.substr(0, n);
      body.erase(0, n);
      return out;
    }
  };
  std::vector<std::unique_ptr<Fetch>> fetches;

  std::mutex mutex;
  std::vector<json> events;

  static Impl* of(JSContext* ctx) { return static_cast<Impl*>(JS_GetContextOpaque(ctx)); }
  std::string str(JSValueConst v) {
    const char* s = JS_ToCString(ctx, v);
    std::string out = s ? s : "";
    JS_FreeCString(ctx, s);
    return out;
  }
  void reportException(const char* where) {
    JSValue e = JS_GetException(ctx);
    JSValue stack = JS_GetPropertyStr(ctx, e, "stack");
    std::string msg = str(e) + (JS_IsUndefined(stack) ? "" : "\n" + str(stack));
    JS_FreeValue(ctx, stack);
    JS_FreeValue(ctx, e);
    if (opt.log) opt.log(std::string(where) + ": " + msg);
  }

  // ---- __host functions ----
  static JSValue hostLog(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Impl* im = of(ctx);
    if (argc > 0 && im->opt.log) im->opt.log(im->str(argv[0]));
    return JS_UNDEFINED;
  }
  static JSValue hostSystem(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Impl* im = of(ctx);
    json out;
    try {
      out = argc > 0 ? im->opt.system(im->str(argv[0])) : json{{"error", "no name"}};
    } catch (const std::exception& e) {
      out = {{"error", "failed"}, {"reason", e.what()}};
    }
    return JS_NewString(ctx, out.dump().c_str());
  }
  static JSValue hostUuid(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    Impl* im = of(ctx);
    return JS_NewString(ctx, uuid4(im->gen).c_str());
  }
  static JSValue hostEmit(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Impl* im = of(ctx);
    if (argc < 1) return JS_UNDEFINED;
    json e = json::parse(im->str(argv[0]), nullptr, false);
    if (!e.is_discarded()) {
      std::lock_guard lock(im->mutex);
      im->events.push_back(std::move(e));
    }
    return JS_UNDEFINED;
  }
  static JSValue hostDone(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Impl* im = of(ctx);
    std::string err = argc > 0 ? im->str(argv[0]) : "";
    if (!err.empty()) {
      if (im->opt.log) im->opt.log("run failed: " + err);
      std::lock_guard lock(im->mutex);
      im->events.push_back({{"type", "RUN_ERROR"}, {"message", "The agent runtime failed."}});
    }
    im->self->busy_ = false;
    return JS_UNDEFINED;
  }
  static JSValue hostSetTimeout(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Impl* im = of(ctx);
    double ms = 0;
    if (argc > 1) JS_ToFloat64(ctx, &ms, argv[1]);
    int id = im->nextTimer++;
    im->timers.push_back({id, std::chrono::steady_clock::now() + std::chrono::microseconds(int64_t(ms * 1000)), JS_DupValue(ctx, argv[0])});
    return JS_NewInt32(ctx, id);
  }
  static JSValue hostClearTimeout(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Impl* im = of(ctx);
    int id = 0;
    if (argc > 0) JS_ToInt32(ctx, &id, argv[0]);
    for (auto it = im->timers.begin(); it != im->timers.end(); ++it)
      if (it->id == id) {
        JS_FreeValue(ctx, it->fn);
        im->timers.erase(it);
        break;
      }
    return JS_UNDEFINED;
  }
  // fetch(url, method, headersJson, body, timeoutMs, callback(status, text, headersJson)[, onChunk(text)])
  static JSValue hostFetch(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Impl* im = of(ctx);
    if (argc < 6) return JS_ThrowTypeError(ctx, "__host.fetch: 6 arguments");
    auto f = std::make_unique<Fetch>();
    f->callback = JS_DupValue(ctx, argv[5]);
    if (argc > 6 && JS_IsFunction(ctx, argv[6])) f->onChunk = JS_DupValue(ctx, argv[6]);
    std::string url = im->str(argv[0]), method = im->str(argv[1]), body = im->str(argv[3]);
    json headers = json::parse(im->str(argv[2]), nullptr, false);
    double timeoutMs = 0;
    JS_ToFloat64(ctx, &timeoutMs, argv[4]);
    Fetch* raw = f.get();
    std::string ca = im->opt.caBundle;
    f->worker = std::thread([raw, url, method, body, headers, timeoutMs, ca] {
      CURL* curl = curl_easy_init();
      curl_slist* list = nullptr;
      if (headers.is_object())
        for (auto& [k, v] : headers.items()) list = curl_slist_append(list, (k + ": " + (v.is_string() ? v.get<std::string>() : v.dump())).c_str());
      curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
      curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
      if (method != "GET" && method != "HEAD") curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Fetch::onBody);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, raw);
      curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, onHeader);
      curl_easy_setopt(curl, CURLOPT_HEADERDATA, &raw->headers);
      if (timeoutMs > 0) curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, long(timeoutMs));
      curl_blob blob{const_cast<char*>(ca.data()), ca.size(), CURL_BLOB_COPY};
      if (!ca.empty()) curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &blob);
      CURLcode rc = curl_easy_perform(curl);
      long status = 0;
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
      if (rc == CURLE_OK) raw->status = int(status);
      else raw->error = curl_easy_strerror(rc);
      curl_slist_free_all(list);
      curl_easy_cleanup(curl);
      raw->finished = true;
    });
    im->fetches.push_back(std::move(f));
    return JS_UNDEFINED;
  }

  void addHost() {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue host = JS_NewObject(ctx);
    struct { const char* name; JSCFunction* fn; int args; } fns[] = {
        {"log", hostLog, 1}, {"uuid", hostUuid, 0}, {"emit", hostEmit, 1}, {"done", hostDone, 1},
        {"setTimeout", hostSetTimeout, 2}, {"clearTimeout", hostClearTimeout, 1}, {"fetch", hostFetch, 6},
    };
    for (auto& f : fns) JS_SetPropertyStr(ctx, host, f.name, JS_NewCFunction(ctx, f.fn, f.name, f.args));
    if (opt.system) JS_SetPropertyStr(ctx, host, "system", JS_NewCFunction(ctx, hostSystem, "system", 1));
    json env = json::object();
    for (auto& [k, v] : opt.env) env[k] = v;
    JS_SetPropertyStr(ctx, host, "env", JS_NewString(ctx, env.dump().c_str()));
    JS_SetPropertyStr(ctx, global, "__host", host);
    JS_FreeValue(ctx, global);
  }

  bool eval(const std::string& src, const char* name) {
    JSValue r = JS_Eval(ctx, src.c_str(), src.size(), name, JS_EVAL_TYPE_GLOBAL);
    bool ok = !JS_IsException(r);
    if (!ok) reportException(name);
    JS_FreeValue(ctx, r);
    return ok;
  }

  void runJobs() {
    for (;;) {
      JSContext* jobCtx = nullptr;
      int r = JS_ExecutePendingJob(rt, &jobCtx);
      if (r <= 0) {
        if (r < 0) reportException("job");
        break;
      }
    }
  }
};

JsAgent::JsAgent(Options options) : impl_(std::make_unique<Impl>()) {
  Impl& im = *impl_;
  im.self = this;
  im.opt = std::move(options);
  curl_global_init(CURL_GLOBAL_DEFAULT);
  im.rt = JS_NewRuntime();
  JS_SetMemoryLimit(im.rt, 256u << 20);
  JS_SetMaxStackSize(im.rt, 4u << 20);
  im.ctx = JS_NewContext(im.rt);
  JS_SetContextOpaque(im.ctx, &im);
  im.addHost();
  auto t0 = std::chrono::steady_clock::now();
  if (!im.eval(kPrelude, "prelude.js") || !im.eval(im.opt.bundle, "agent.js")) throw std::runtime_error("the agent runtime didn't load (see the log)");
  im.runJobs();
  loadMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  threadId_ = "ps5-" + uuid4(im.gen).substr(0, 16);
}

bool JsAgent::newConversation() {
  if (busy_) return false;
  threadId_ = "ps5-" + uuid4(impl_->gen).substr(0, 16);
  return true;
}

JsAgent::~JsAgent() {
  Impl& im = *impl_;
  for (auto& f : im.fetches)
    if (f->worker.joinable()) f->worker.join();
  for (auto& f : im.fetches) {
    JS_FreeValue(im.ctx, f->callback);
    JS_FreeValue(im.ctx, f->onChunk);
  }
  for (auto& t : im.timers) JS_FreeValue(im.ctx, t.fn);
  JS_FreeContext(im.ctx);
  JS_FreeRuntime(im.rt);
}

bool JsAgent::sendPrompt(const std::string& text) {
  json message = {{"id", "msg-" + std::to_string(++counter_)}, {"role", "user"}, {"content", text}};
  return start({{"messages", json::array({message})}});
}

bool JsAgent::sendAction(const json& actionMessage) {
  // As CopilotKit 1.85 does: the press as a visible user message, and the A2UI action message in
  // forwardedProps.copilotkit.ui (copilotkit_action).
  if (!actionMessage.is_object() || !actionMessage.contains("action")) return false;
  json message = {{"id", "msg-" + std::to_string(++counter_)}, {"role", "user"}, {"content", actionText(actionMessage["action"])}};
  return start({{"messages", json::array({message})}, {"forwardedProps", {{"copilotkit", {{"ui", uiEnvelope(actionMessage)}}}}}});
}

bool JsAgent::start(json body) {
  if (busy_) return false;
  Impl& im = *impl_;
  body["threadId"] = threadId_;
  body["runId"] = "run-" + uuid4(im.gen).substr(0, 16);
  body["tools"] = json::array();
  body["context"] = json::array();
  body["state"] = json::object();
  busy_ = true;
  JSValue global = JS_GetGlobalObject(im.ctx);
  JSValue run = JS_GetPropertyStr(im.ctx, global, "__run");
  JSValue args[2] = {JS_NewString(im.ctx, body.dump().c_str()), JS_NewString(im.ctx, im.opt.model.c_str())};
  JSValue r = JS_Call(im.ctx, run, global, 2, args);
  if (JS_IsException(r)) {
    im.reportException("run");
    busy_ = false;
    std::lock_guard lock(im.mutex);
    im.events.push_back({{"type", "RUN_ERROR"}, {"message", "The agent runtime failed."}});
  }
  JS_FreeValue(im.ctx, r);
  JS_FreeValue(im.ctx, args[0]);
  JS_FreeValue(im.ctx, args[1]);
  JS_FreeValue(im.ctx, run);
  JS_FreeValue(im.ctx, global);
  im.runJobs();
  return true;
}

void JsAgent::pump() {
  Impl& im = *impl_;
  // Streamed bodies: what arrived so far. Finished requests: hand the response to its callback.
  auto deliver = [&im](Impl::Fetch& f, bool all) {
    std::string piece = f.take(all);
    if (piece.empty()) return;
    JSValue arg = JS_NewStringLen(im.ctx, piece.data(), piece.size());
    JSValue r = JS_Call(im.ctx, f.onChunk, JS_UNDEFINED, 1, &arg);
    if (JS_IsException(r)) im.reportException("fetch chunk");
    JS_FreeValue(im.ctx, r);
    JS_FreeValue(im.ctx, arg);
  };
  for (size_t i = 0; i < im.fetches.size();) {
    auto& f = im.fetches[i];
    const bool streaming = !JS_IsUndefined(f->onChunk);
    if (!f->finished) {
      if (streaming) deliver(*f, false);
      ++i;
      continue;
    }
    f->worker.join();
    if (streaming && f->status >= 0) deliver(*f, true);
    const std::string text = f->status < 0 ? f->error : streaming ? "" : f->body;
    JSValue args[3] = {JS_NewInt32(im.ctx, f->status), JS_NewStringLen(im.ctx, text.data(), text.size()),
                       JS_NewString(im.ctx, f->headers.dump().c_str())};
    JSValue r = JS_Call(im.ctx, f->callback, JS_UNDEFINED, 3, args);
    if (JS_IsException(r)) im.reportException("fetch callback");
    JS_FreeValue(im.ctx, r);
    for (auto& a : args) JS_FreeValue(im.ctx, a);
    JS_FreeValue(im.ctx, f->callback);
    JS_FreeValue(im.ctx, f->onChunk);
    im.fetches.erase(im.fetches.begin() + long(i));
  }
  // Due timers (a timer may add timers: take the due ones first).
  auto now = std::chrono::steady_clock::now();
  std::vector<Impl::Timer> due;
  for (auto it = im.timers.begin(); it != im.timers.end();) {
    if (it->due <= now) {
      due.push_back(*it);
      it = im.timers.erase(it);
    } else {
      ++it;
    }
  }
  for (auto& t : due) {
    JSValue r = JS_Call(im.ctx, t.fn, JS_UNDEFINED, 0, nullptr);
    if (JS_IsException(r)) im.reportException("timer");
    JS_FreeValue(im.ctx, r);
    JS_FreeValue(im.ctx, t.fn);
  }
  im.runJobs();
}

std::vector<json> JsAgent::poll() {
  std::lock_guard lock(impl_->mutex);
  std::vector<json> out;
  out.swap(impl_->events);
  return out;
}

}  // namespace a2ui
