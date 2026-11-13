// A2UI 1.0 renderer state: surfaces, their components and data models, and value resolution.
// Accepts raw A2UI 1.0 messages and AG-UI ACTIVITY_SNAPSHOT events: CopilotKit 1.85's
// "generative-ui-surface" (content.operations, a surface's operations so far) and the AG-UI A2UI
// middleware's "a2ui-surface" (content.a2ui_operations). Spec: vendor/a2ui-v1_0;
// see docs/agent.md.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace a2ui {

using json = nlohmann::json;

/** The A2UI protocol version this renderer speaks ("version" of every message). */
inline constexpr const char* kVersion = "1.0";
/** The PS5 catalog (catalog/ps5.catalog.json). */
inline constexpr const char* kPs5CatalogId = "https://raw.githubusercontent.com/Laptopmini/ps5-agentic-ui/catalog-v1/catalog/ps5.catalog.json";

struct Surface {
  std::string id;
  std::string catalogId;
  std::map<std::string, json> components;  // by component id
  json data = json::object();

  const json* component(const std::string& componentId) const;
};

/** One line of the conversation as the console shows it. */
struct ChatLine {
  enum class Role { User, Assistant, Notice, Surface };
  Role role;
  std::string text;       // Surface: the screen's title
  std::string icon;       // Notice: info, success, warning, error, ...
  std::string surfaceId;  // Surface: which screen (drawn in full while it's the live one)
};

/** A user interaction: becomes an A2UI 1.0 action message (renderer_to_agent.json, "action"). */
struct UserAction {
  std::string name;
  std::string surfaceId;
  std::string sourceComponentId;
  json context = json::object();
  /** What the user picked, as shown ("Mini-5G", "Delete"); not sent, it's for the transcript. */
  std::string label;
  /** The event's userMessage, resolved: what the press says to the agent ("" when none). */
  std::string userMessage;

  /** {"version": "1.0", "action": {name, surfaceId, sourceComponentId, timestamp, context, userMessage?}} */
  json toJson() const;
};

/**
 * The visible user message of a press, as CopilotKit 1.85 writes it: the userMessage, else
 * context.message or context.label, else the event name made readable; then each other context
 * field as a "key: value" line. `action` is the inner object of an action message.
 */
std::string actionText(const json& action);

/**
 * forwardedProps.copilotkit.ui for a run (CopilotKit 1.85's envelope, format 3): the catalog this
 * renderer draws and, after a press, the action message that started the run.
 */
json uiEnvelope(const json& actionMessage = json());

class Store {
 public:
  /** Apply one A2UI envelope or AG-UI activity event. Returns an error message, or "" on success. */
  std::string apply(const json& message);
  /** Apply every non-empty line of a JSONL document; returns the errors (with line numbers). */
  std::vector<std::string> applyJsonl(const std::string& text);

  Surface* surface(const std::string& id);
  const Surface* surface(const std::string& id) const;
  /** The most recently created surface still on the stage (the one on screen), or null. */
  const Surface* top() const;
  /** Surfaces in creation order. */
  std::vector<const Surface*> surfaces() const;

  /** Remove every surface (the stage is empty). */
  void clear() {
    surfaces_.clear();
    order_.clear();
    stageStart_ = 0;
  }
  /**
   * Empty the stage but keep the surfaces: top() is null until a surface is created, and
   * earlier screens can still be reopened from the conversation (surface(id) finds them).
   */
  void clearStage() { stageStart_ = order_.size(); }

  /** Write a value into a surface's data model at a JSON Pointer, creating objects on the way. */
  void setData(const std::string& surfaceId, const std::string& pointer, const json& value);

 private:
  std::string applyEnvelope(const json& envelope);
  std::map<std::string, Surface> surfaces_;
  std::vector<std::string> order_;
  size_t stageStart_ = 0;  // order_ before this index is off the stage (clearStage)
  void forget(const std::string& id);  // drop id from order_, keeping stageStart_ in step
};

/**
 * Resolve a dynamic value: a literal, a {"@path": ...} binding (absolute "/a/b", or relative "b"
 * inside a template item), or a {"@call": ...} function call (not evaluated here: returns null).
 * `scope` is the template item for relative paths, or null outside templates.
 */
json resolve(const json& value, const json& data, const json* scope);
std::string resolveString(const json& value, const json& data, const json* scope);

/** Resolve every value of an action's event context against the data model. */
json resolveContext(const json& context, const json& data, const json* scope);

/** JSON Pointer lookup that returns nullptr instead of throwing. */
const json* lookup(const json& data, const std::string& pointer);

}  // namespace a2ui
