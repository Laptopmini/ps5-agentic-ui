// AG-UI client over HTTP (libcurl): POSTs a RunAgentInput and streams the SSE response on a
// worker thread; the UI thread drains events with poll(). One run at a time per thread id.
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "a2ui/model.hpp"

namespace a2ui {

/** A conversation with the agent: over HTTP (AgentClient) or in-process (JsAgent). */
class AgentConnection {
 public:
  virtual ~AgentConnection() = default;
  /** Start a run with a new user message. Returns false if a run is still going. */
  virtual bool sendPrompt(const std::string& text) = 0;
  /** Start a run with a button press: an A2UI 1.0 action message, {"version": "1.0", "action": {...}}. */
  virtual bool sendAction(const json& actionMessage) = 0;
  virtual bool busy() const = 0;
  /** Events received since the last call, in order. */
  virtual std::vector<json> poll() = 0;
  virtual const std::string& threadId() const = 0;
  /** Leave this conversation: the next message starts a new one (a new thread). False while a run is going. */
  virtual bool newConversation() = 0;
  /** Do pending work on the caller's thread (the in-process runtime runs here). Once per frame. */
  virtual void pump() {}
};

class AgentClient : public AgentConnection {
 public:
  /** `token`: the server's access code, sent as "authorization: Bearer …" (empty: none). */
  explicit AgentClient(std::string url, std::string token = "");
  ~AgentClient() override;

  bool sendPrompt(const std::string& text) override;
  bool sendAction(const json& actionMessage) override;
  bool busy() const override { return busy_; }
  std::vector<json> poll() override;
  const std::string& threadId() const override { return threadId_; }
  bool newConversation() override;

 private:
  bool start(json body);
  void push(json event);

  std::string url_, token_, threadId_;
  std::thread worker_;
  std::mutex mutex_;
  std::vector<json> queue_;
  std::atomic<bool> busy_{false};
  int counter_ = 0;
};

}  // namespace a2ui
