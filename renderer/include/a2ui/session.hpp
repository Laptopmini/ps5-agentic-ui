// Client side of an AG-UI conversation, without transport: turns the event stream into screens
// (A2UI surfaces in the Store), a transcript and caption (the assistant's text), a status line (thinking,
// working, retrying), and decides when the stage should be cleared. The host feeds it events from
// its HTTP client; the same class runs on the PS5.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "a2ui/model.hpp"

namespace a2ui {

/** Incremental parser for server-sent events: feed bytes, get complete `data:` payloads as JSON. */
class SseParser {
 public:
  std::vector<json> feed(const std::string& bytes);

 private:
  std::string buffer_;
};

class Session {
 public:
  explicit Session(Store& store) : store_(store) {}

  void handle(const json& event);

  bool running() const { return running_; }
  /** The assistant's latest text in this run (or the last run, until a new one starts). */
  const std::string& caption() const { return caption_; }
  /** Short status for the UI: "Thinking…", "Checking scan_wifi…", "Busy, retrying in 30 s", or "". */
  const std::string& status() const { return status_; }
  /** A console tool's progress, 0 to 1, while it reports one; -1 otherwise. */
  float progress() const { return progress_; }
  /** What that work is ("Loading Qwen 9B"). */
  const std::string& progressLabel() const { return progressLabel_; }
  /** Set when a run failed; cleared when the next run starts. */
  const std::string& error() const { return error_; }
  /** The conversation so far: the user's requests and picks, the assistant's answers, notices. */
  const std::vector<ChatLine>& transcript() const { return transcript_; }
  /**
   * Catalog function calls the agent asked the client to run ({"@call": "rumble", "args": {...}}),
   * from "generative-ui-function-call" activities (callRendererFunction), oldest first. Taking
   * them empties the queue.
   */
  std::vector<json> takeFunctionCalls() { return std::exchange(functionCalls_, {}); }
  /**
   * The agent's name, if it gave one (CUSTOM "ps5.agent" {name}, e.g. a channel saying who
   * speaks through it); "" otherwise. Kept until it changes.
   */
  const std::string& agentName() const { return agentName_; }
  /** Record what the user asked or picked (the host calls this as it sends). Clears the follow-ups. */
  void addUser(const std::string& text);
  /**
   * What the agent suggested asking next (CUSTOM "ps5.suggestions" {prompts}), shown under its
   * reply; empty once the user sends something or a new run starts.
   */
  const std::vector<std::string>& followUps() const { return followUps_; }
  /** Start over: no transcript, status or follow-ups (the host also empties the Store and starts a new thread). */
  void clear();

 private:
  void dropNarration();
  Store& store_;
  bool running_ = false;
  int surfacesThisRun_ = 0;
  size_t runStart_ = 0;  // first transcript line of this run
  std::string caption_, status_, error_, progressLabel_, agentName_;
  float progress_ = -1;
  std::vector<ChatLine> transcript_;
  std::vector<json> functionCalls_;
  std::vector<std::string> followUps_;
};

}  // namespace a2ui
