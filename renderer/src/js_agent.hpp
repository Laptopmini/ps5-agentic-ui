// The agent runtime in-process: the same TypeScript AgentThread the Node server runs, bundled
// (server/embedded.ts → server/qjs/agent.js) and run in QuickJS. The host supplies what Node
// would: fetch (libcurl on worker threads), setTimeout, crypto.randomUUID, process.env, console.
// All JavaScript runs on the caller's thread inside pump(); only network I/O runs elsewhere.
// It speaks AG-UI events like AgentClient, so apps can use either.
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "agent_client.hpp"

namespace a2ui {

class JsAgent : public AgentConnection {
 public:
  struct Options {
    std::string bundle;                     // the agent.js source
    std::map<std::string, std::string> env;  // process.env (e.g. LLM_API_KEY, LLM_BASE_URL)
    std::string model;                      // the model id the runtime asks for
    std::string caBundle;                   // PEM certificates for HTTPS ("" = system default)
    std::function<void(const std::string&)> log;  // console.log / errors
    /**
     * Reads the console the agent runs on (ps5_system.hpp): called with a name ("installed_titles"),
     * returns JSON. Unset: the agent's console tools stay simulated.
     */
    std::function<json(const std::string&)> system;
  };
  /** Throws std::runtime_error if the bundle doesn't load. */
  explicit JsAgent(Options options);
  ~JsAgent() override;

  bool sendPrompt(const std::string& text) override;
  bool sendAction(const json& actionMessage) override;
  bool busy() const override { return busy_; }
  std::vector<json> poll() override;
  const std::string& threadId() const override { return threadId_; }
  bool newConversation() override;
  void pump() override;

  /** Milliseconds the bundle took to load (parse, compile, module init). */
  double loadMs() const { return loadMs_; }

  struct Impl;

 private:
  bool start(json body);
  std::unique_ptr<Impl> impl_;
  std::string threadId_;
  std::atomic<bool> busy_{false};
  int counter_ = 0;
  double loadMs_ = 0;
};

}  // namespace a2ui
