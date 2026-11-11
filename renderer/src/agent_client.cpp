#include "agent_client.hpp"

#include <curl/curl.h>

#include <chrono>
#include <cstdio>
#include <functional>
#include <random>

#include "a2ui/session.hpp"

namespace a2ui {
namespace {

std::string randomId(const char* prefix) {
  // Seeded once; std::random_device can throw where there's no entropy device (console sandbox).
  static std::mt19937_64 gen = [] {
    uint64_t seed = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    try {
      std::random_device rd;
      seed ^= (uint64_t(rd()) << 32) | rd();
    } catch (...) {
    }
    return std::mt19937_64(seed);
  }();
  char buf[40];
  std::snprintf(buf, sizeof buf, "%s-%016llx", prefix, static_cast<unsigned long long>(gen()));
  return buf;
}

struct Stream {
  SseParser parser;
  AgentClient* client;
  std::function<void(json)> push;
  bool finished = false;
};

size_t onData(char* data, size_t size, size_t count, void* user) {
  auto* s = static_cast<Stream*>(user);
  for (json& e : s->parser.feed(std::string(data, size * count))) {
    std::string type = e.value("type", "");
    if (type == "RUN_FINISHED" || type == "RUN_ERROR") s->finished = true;
    s->push(std::move(e));
  }
  return size * count;
}

}  // namespace

AgentClient::AgentClient(std::string url, std::string token)
    : url_(std::move(url)), token_(std::move(token)), threadId_(randomId("ps5")) {
  curl_global_init(CURL_GLOBAL_DEFAULT);
}

AgentClient::~AgentClient() {
  if (worker_.joinable()) worker_.join();
  curl_global_cleanup();
}

bool AgentClient::newConversation() {
  if (busy_) return false;
  threadId_ = randomId("ps5");
  return true;
}

bool AgentClient::sendPrompt(const std::string& text) {
  json message = {{"id", "msg-" + std::to_string(++counter_)}, {"role", "user"}, {"content", text}};
  return start({{"messages", json::array({message})}});
}

bool AgentClient::sendAction(const json& actionMessage) {
  // As CopilotKit 1.85 does: the press as a visible user message, and the A2UI action message in
  // forwardedProps.copilotkit.ui (copilotkit_action).
  if (!actionMessage.is_object() || !actionMessage.contains("action")) return false;
  json message = {{"id", "msg-" + std::to_string(++counter_)}, {"role", "user"}, {"content", actionText(actionMessage["action"])}};
  return start({{"messages", json::array({message})}, {"forwardedProps", {{"copilotkit", {{"ui", uiEnvelope(actionMessage)}}}}}});
}

std::vector<json> AgentClient::poll() {
  std::lock_guard lock(mutex_);
  std::vector<json> out;
  out.swap(queue_);
  return out;
}

void AgentClient::push(json event) {
  std::lock_guard lock(mutex_);
  queue_.push_back(std::move(event));
}

bool AgentClient::start(json body) {
  if (busy_) return false;
  if (worker_.joinable()) worker_.join();
  busy_ = true;
  // RunAgentInput: the server keeps the thread's history, so only what's new is sent.
  body["threadId"] = threadId_;
  body["runId"] = randomId("run");
  body["tools"] = json::array();
  body["context"] = json::array();
  body["state"] = json::object();
  worker_ = std::thread([this, payload = body.dump()] {
    Stream stream{{}, this, [this](json e) { push(std::move(e)); }};
    CURL* curl = curl_easy_init();
    curl_slist* headers = curl_slist_append(nullptr, "content-type: application/json");
    headers = curl_slist_append(headers, "accept: text/event-stream");
    if (!token_.empty()) headers = curl_slist_append(headers, ("authorization: Bearer " + token_).c_str());
    curl_easy_setopt(curl, CURLOPT_URL, url_.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, onData);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &stream);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);  // a run can include several rate-limit waits
    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (!stream.finished) {
      std::string why = rc != CURLE_OK ? curl_easy_strerror(rc) : "HTTP " + std::to_string(status);
      push({{"type", "RUN_ERROR"}, {"message", status == 401 ? std::string("The agent server refused the access code.")
                                                             : "Can't reach the agent (" + why + ")."}});
    }
    busy_ = false;
  });
  return true;
}

}  // namespace a2ui
