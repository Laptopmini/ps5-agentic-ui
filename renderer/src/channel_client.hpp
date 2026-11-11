// The console as a CopilotKit Channels surface (channel/adapter.ts on a computer): one long-lived
// event stream down (GET /events, server-sent AG-UI events), short posts up (/hello, /turn,
// /action). Unlike AgentClient, the agent can speak at any time, not only in answer to a run, and
// the stream reconnects by itself.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "agent_client.hpp"

namespace a2ui {

class ChannelClient : public AgentConnection {
 public:
  /**
   * `url`: the adapter's base address (http://<computer>:8792). `token`: its access code.
   * `consoleId`: names this console's conversation. `client`: what it can do, sent as /hello on
   * every (re)connect ({catalogId, functions, haptics, controller, name}).
   */
  ChannelClient(std::string url, std::string token, std::string consoleId, json client);
  ~ChannelClient() override;

  bool sendPrompt(const std::string& text) override;
  bool sendAction(const json& actionMessage) override;
  /** A run is going, or something was sent and nothing has come back yet. */
  bool busy() const override;
  std::vector<json> poll() override;
  const std::string& threadId() const override { return threadId_; }
  /** Asks the channel to forget this console's conversation (POST /reset). */
  bool newConversation() override;

  /** Replace what this console says it can do (e.g. a controller connected) and say it again. */
  void setClient(json client);
  /** The stream is up. */
  bool connected() const { return connected_; }

  /**
   * Answers the channel's reads about this console (CUSTOM "ps5.request" {id, name, args}, e.g.
   * "installed_titles"), posted back to /reply. Runs inside poll(), on the caller's thread. Name the reads in
   * describe(…, reads) so the channel offers them.
   */
  using RequestHandler = std::function<json(const std::string& name, const json& args)>;
  void setRequestHandler(RequestHandler handler);

  /**
   * What this renderer can do, for /hello: the PS5 catalog, the catalog functions a host runs,
   * the named haptic effects, whether a controller is connected (`controller`), and the reads
   * the request handler answers (`reads`).
   */
  static json describe(const std::string& name, bool controller, const std::vector<std::string>& reads = {});

 private:
  void streamLoop();
  void postLoop();
  void enqueuePost(std::string path, json body);
  void push(json event);

  std::string url_, token_, consoleId_, threadId_;
  std::mutex mutex_;  // guards queue_, client_, posts_, unanswered_, onRequest_, requests_
  std::set<std::string> unanswered_;  // tool calls of the current run without a result yet
  std::vector<json> queue_;
  json client_;
  RequestHandler onRequest_;
  std::vector<json> requests_;  // reads asked for, answered in poll()
  std::deque<std::pair<std::string, std::string>> posts_;
  std::condition_variable postReady_;
  std::atomic<bool> stop_{false}, connected_{false};
  std::atomic<int> runsOpen_{0};
  std::atomic<long long> awaitingSince_{0};  // ms since epoch of a send not yet answered; 0 = none
  std::thread stream_, poster_;
};

}  // namespace a2ui
