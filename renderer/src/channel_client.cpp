#include "channel_client.hpp"

#include <curl/curl.h>

#include <chrono>
#include <functional>

#include "a2ui/haptics.hpp"
#include "a2ui/session.hpp"

namespace a2ui {
namespace {

long long nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// A send counts as unanswered for this long; then busy() lets go (the adapter may not answer
// at all, e.g. a press on a control whose handler posts nothing).
constexpr long long kAwaitMs = 30000;

struct StreamState {
  SseParser parser;
  std::function<void(json)> push;
  std::function<void()> opened;  // the first bytes arrived: the stream is up
  const std::atomic<bool>* stop;
  bool open = false;
};

size_t onStreamData(char* data, size_t size, size_t count, void* user) {
  auto* s = static_cast<StreamState*>(user);
  if (!s->open) {
    s->open = true;
    s->opened();
  }
  for (json& e : s->parser.feed(std::string(data, size * count))) s->push(std::move(e));
  return size * count;
}

// Lets the stream be cut when the client is destroyed (libcurl calls this about once a second).
int onProgress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
  return static_cast<StreamState*>(user)->stop->load() ? 1 : 0;
}

size_t discard(char*, size_t size, size_t count, void*) { return size * count; }

}  // namespace

ChannelClient::ChannelClient(std::string url, std::string token, std::string consoleId, json client)
    : url_(std::move(url)), token_(std::move(token)), consoleId_(std::move(consoleId)), threadId_("ps5:" + consoleId_), client_(std::move(client)) {
  while (!url_.empty() && url_.back() == '/') url_.pop_back();
  curl_global_init(CURL_GLOBAL_DEFAULT);
  stream_ = std::thread([this] { streamLoop(); });
  poster_ = std::thread([this] { postLoop(); });
}

ChannelClient::~ChannelClient() {
  stop_ = true;
  postReady_.notify_all();
  if (stream_.joinable()) stream_.join();
  if (poster_.joinable()) poster_.join();
  curl_global_cleanup();
}

bool ChannelClient::busy() const {
  if (runsOpen_ > 0) return true;
  long long since = awaitingSince_;
  return since != 0 && nowMs() - since < kAwaitMs;
}

bool ChannelClient::sendPrompt(const std::string& text) {
  if (busy()) return false;
  awaitingSince_ = nowMs();
  enqueuePost("/turn", {{"console", consoleId_}, {"text", text}});
  return true;
}

bool ChannelClient::newConversation() {
  if (busy()) return false;
  enqueuePost("/reset", {{"console", consoleId_}});
  return true;
}

bool ChannelClient::sendAction(const json& actionMessage) {
  if (!actionMessage.is_object() || !actionMessage.contains("action")) return false;
  awaitingSince_ = nowMs();
  enqueuePost("/action", {{"console", consoleId_}, {"action", actionMessage}});
  return true;
}

json ChannelClient::describe(const std::string& name, bool controller, const std::vector<std::string>& reads) {
  return {{"name", name},
          {"catalogId", a2ui::kPs5CatalogId},
          {"functions", controller ? json::array({"haptic", "hapticPattern", "rumble", "lightbar", "triggerEffect"}) : json::array()},
          {"haptics", controller ? json(hapticEffects()) : json::array()},
          {"controller", controller},
          {"reads", reads}};
}

void ChannelClient::setRequestHandler(RequestHandler handler) {
  std::lock_guard lock(mutex_);
  onRequest_ = std::move(handler);
}

void ChannelClient::setClient(json client) {
  json copy;
  {
    std::lock_guard lock(mutex_);
    client_ = std::move(client);
    copy = client_;
  }
  if (connected_) enqueuePost("/hello", {{"console", consoleId_}, {"client", copy}});
}

std::vector<json> ChannelClient::poll() {
  std::vector<json> requests;
  RequestHandler handler;
  std::vector<json> out;
  {
    std::lock_guard lock(mutex_);
    requests.swap(requests_);
    handler = onRequest_;
    out.swap(queue_);
  }
  for (const json& r : requests) {
    json result;
    try {
      result = handler ? handler(r.value("name", ""), r.contains("args") ? r["args"] : json::object()) : json{{"error", "no_handler"}};
    } catch (const std::exception& e) {
      result = {{"error", "failed"}, {"reason", e.what()}};
    }
    enqueuePost("/reply", {{"console", consoleId_}, {"id", r.value("id", "")}, {"result", result}});
  }
  return out;
}

void ChannelClient::push(json event) {
  const std::string type = event.value("type", "");
  if (type == "CUSTOM" && event.value("name", "") == "ps5.request") {
    // A read the channel asks for (an agent's tool call): answered from poll(), on the app's
    // thread (the console's sensor calls crash from a thread the app made), not shown.
    const json v = event.contains("value") && event["value"].is_object() ? event["value"] : json::object();
    if (v.value("id", "").empty()) return;
    std::lock_guard lock(mutex_);
    requests_.push_back(v);
    return;
  }
  if (type == "RUN_STARTED") ++runsOpen_;
  if ((type == "RUN_FINISHED" || type == "RUN_ERROR") && runsOpen_ > 0) --runsOpen_;
  awaitingSince_ = 0;  // the adapter answered
  std::lock_guard lock(mutex_);
  // A run that ends on a tool call without a result handed it to the client (a channel tool):
  // another run follows with the result, so this is still busy until it starts.
  // Not when the run waits for the player (the console's approval dialog for a gated tool).
  if (type == "TOOL_CALL_START") unanswered_.insert(event.value("toolCallId", ""));
  else if (type == "TOOL_CALL_RESULT") unanswered_.erase(event.value("toolCallId", ""));
  else if (type == "CUSTOM" && event.value("name", "") == "status" && event.contains("value") && event["value"].is_object() &&
           event["value"].value("state", "") == "waiting_for_approval")
    unanswered_.clear();
  else if (type == "RUN_FINISHED" || type == "RUN_ERROR") {
    if (type == "RUN_FINISHED" && !unanswered_.empty()) awaitingSince_ = nowMs();
    unanswered_.clear();
  }
  queue_.push_back(std::move(event));
}

void ChannelClient::enqueuePost(std::string path, json body) {
  {
    std::lock_guard lock(mutex_);
    posts_.emplace_back(std::move(path), body.dump());
  }
  postReady_.notify_one();
}

void ChannelClient::postLoop() {
  while (!stop_) {
    std::pair<std::string, std::string> next;
    {
      std::unique_lock lock(mutex_);
      postReady_.wait_for(lock, std::chrono::milliseconds(500), [this] { return stop_ || !posts_.empty(); });
      if (stop_) return;
      if (posts_.empty()) continue;
      next = std::move(posts_.front());
      posts_.pop_front();
    }
    CURL* curl = curl_easy_init();
    curl_slist* headers = curl_slist_append(nullptr, "content-type: application/json");
    if (!token_.empty()) headers = curl_slist_append(headers, ("authorization: Bearer " + token_).c_str());
    const std::string url = url_ + next.first;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, next.second.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if ((rc != CURLE_OK || status >= 300) && next.first != "/hello" && next.first != "/reply") {
      awaitingSince_ = 0;
      std::string why = rc != CURLE_OK ? curl_easy_strerror(rc) : "HTTP " + std::to_string(status);
      push({{"type", "RUN_ERROR"}, {"message", "Couldn't reach the channel at " + url_ + " (" + why + ")."}});
    }
  }
}

void ChannelClient::streamLoop() {
  int backoffMs = 1000;
  while (!stop_) {
    StreamState state{SseParser{}, [this](json e) { push(std::move(e)); },
                      [this] {
                        // Say hello on every (re)connect: the adapter may have restarted.
                        connected_ = true;
                        json client;
                        {
                          std::lock_guard lock(mutex_);
                          client = client_;
                        }
                        enqueuePost("/hello", {{"console", consoleId_}, {"client", client}});
                      },
                      &stop_};
    CURL* curl = curl_easy_init();
    curl_slist* headers = curl_slist_append(nullptr, "accept: text/event-stream");
    if (!token_.empty()) headers = curl_slist_append(headers, ("authorization: Bearer " + token_).c_str());
    const std::string url = url_ + "/events?console=" + consoleId_;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, onStreamData);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, onProgress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &state);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    // The adapter writes a keep-alive every 15 s: silence for 45 s means the link is gone.
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 45L);
    auto began = std::chrono::steady_clock::now();
    curl_easy_perform(curl);
    connected_ = false;
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (stop_) return;
    // A run that was open when the link dropped won't finish on this stream.
    runsOpen_ = 0;
    if (std::chrono::steady_clock::now() - began > std::chrono::seconds(30)) backoffMs = 1000;
    for (int waited = 0; waited < backoffMs && !stop_; waited += 100) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    backoffMs = std::min(backoffMs * 2, 15000);
  }
}

}  // namespace a2ui
