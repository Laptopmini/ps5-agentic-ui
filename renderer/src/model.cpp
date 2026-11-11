#include "a2ui/model.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <sstream>

namespace a2ui {

const json* Surface::component(const std::string& componentId) const {
  auto it = components.find(componentId);
  return it == components.end() ? nullptr : &it->second;
}

namespace {

std::string isoNow() {
  using namespace std::chrono;
  const auto now = system_clock::now();
  const std::time_t t = system_clock::to_time_t(now);
  std::tm utc{};
  gmtime_r(&t, &utc);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", &utc);
  const long ms = long(duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000);
  char out[40];
  std::snprintf(out, sizeof out, "%s.%03ldZ", buf, ms);
  return out;
}

/** The operations an activity snapshot carries, or null for other activities. */
const json* operationsOf(const json& message) {
  const std::string kind = message.value("activityType", "");
  if (!message.contains("content") || !message["content"].is_object()) return nullptr;
  const json& content = message["content"];
  const char* key = kind == "generative-ui-surface" ? "operations" : kind == "a2ui-surface" ? "a2ui_operations" : nullptr;
  if (!key || !content.contains(key) || !content[key].is_array()) return nullptr;
  return &content[key];
}

std::string addComponents(Surface& s, const json& components) {
  if (!components.is_array()) return "components is not a list";
  for (const json& component : components) {
    if (!component.contains("id") || !component["id"].is_string()) return "component without id";
    if (component.value("component", "") == "Surface") return "the Surface component is reserved";
    s.components[component["id"].get<std::string>()] = component;
  }
  return "";
}

}  // namespace

json UserAction::toJson() const {
  json action = {{"name", name}, {"surfaceId", surfaceId}, {"sourceComponentId", sourceComponentId}, {"timestamp", isoNow()}, {"context", context}};
  if (!userMessage.empty()) action["userMessage"] = userMessage;
  return {{"version", kVersion}, {"action", action}};
}

std::string actionText(const json& action) {
  if (!action.is_object()) return "";
  const json context = action.contains("context") && action["context"].is_object() ? action["context"] : json::object();
  std::string head;
  for (const json* v : {action.contains("userMessage") ? &action["userMessage"] : nullptr, context.contains("message") ? &context["message"] : nullptr,
                        context.contains("label") ? &context["label"] : nullptr})
    if (v && v->is_string() && !v->get<std::string>().empty()) {
      head = v->get<std::string>();
      break;
    }
  if (head.empty()) {  // "choose_plan" → "Choose plan"
    for (char c : action.value("name", std::string())) head += (c == '_' || c == '-') ? ' ' : char(std::tolower(static_cast<unsigned char>(c)));
    if (!head.empty()) head[0] = char(std::toupper(static_cast<unsigned char>(head[0])));
  }
  std::string lines;
  for (auto it = context.begin(); it != context.end(); ++it) {
    if (it.key() == "message" || it.key() == "label" || it.value().is_null()) continue;
    lines += "\n" + it.key() + ": " + (it.value().is_string() ? it.value().get<std::string>() : it.value().dump());
  }
  return lines.empty() ? head : head + "\n" + lines;
}

json uiEnvelope(const json& actionMessage) {
  json extensions = {{"copilotkit_format", 3},
                     {"copilotkit_transport", "tool"},
                     {"copilotkit_targets", json::array({{{"id", "chat"}, {"kind", "chat"}}})},
                     {"copilotkit_surfaces", json::object()},
                     {"copilotkit_contextSent", false}};
  if (actionMessage.is_object()) extensions["copilotkit_action"] = actionMessage;
  return {{kVersion, {{"supportedCatalogIds", json::array({kPs5CatalogId})}}}, {"metadata", {{"extensions", extensions}}}};
}

std::string Store::apply(const json& message) {
  if (!message.is_object()) return "message is not an object";
  // An activity snapshot carries a surface's operations; it replaces what came before.
  if (message.value("type", "") == "ACTIVITY_SNAPSHOT") {
    const json* ops = operationsOf(message);
    if (!ops) return "";  // other activities (function calls, status-only snapshots): not surfaces
    for (const json& op : *ops) {
      std::string error = applyEnvelope(op);
      if (!error.empty()) return error;
    }
    return "";
  }
  return applyEnvelope(message);
}

std::string Store::applyEnvelope(const json& m) {
  if (!m.is_object()) return "operation is not an object";
  if (m.value("version", "") != kVersion) return "unsupported A2UI version \"" + m.value("version", "") + "\" (this renderer speaks " + kVersion + ")";
  if (m.contains("createSurface")) {
    const json& c = m["createSurface"];
    std::string id = c.value("surfaceId", "");
    if (id.empty()) return "createSurface without surfaceId";
    // A snapshot holds every operation of its surface, createSurface first, and replaces the last
    // one: creating an existing surface resets it instead of failing (the spec would require a
    // deleteSurface first).
    Surface fresh{id, c.value("catalogId", ""), {}, json::object()};
    // New in 1.0: the components and the data model can come with the createSurface.
    if (c.contains("components")) {
      std::string error = addComponents(fresh, c["components"]);
      if (!error.empty()) return error;
    }
    if (c.contains("dataModel") && c["dataModel"].is_object()) fresh.data = c["dataModel"];
    surfaces_[id] = std::move(fresh);
    forget(id);
    order_.push_back(id);
    return "";
  }
  if (m.contains("updateComponents")) {
    const json& u = m["updateComponents"];
    Surface* s = surface(u.value("surfaceId", ""));
    if (!s) return "updateComponents for unknown surface";
    if (!u.contains("components")) return "updateComponents without components";
    return addComponents(*s, u["components"]);
  }
  if (m.contains("updateDataModel")) {
    const json& u = m["updateDataModel"];
    std::string id = u.value("surfaceId", "");
    if (!surface(id)) return "updateDataModel for unknown surface";
    if (!u.contains("value")) return "updateDataModel without value (null deletes)";
    setData(id, u.value("path", "/"), u["value"]);
    return "";
  }
  if (m.contains("callRendererFunction")) return "";  // run by the session, not part of a surface
  if (m.contains("deleteSurface")) {
    std::string id = m["deleteSurface"].value("surfaceId", "");
    surfaces_.erase(id);
    forget(id);
    return "";
  }
  return "unknown message";
}

std::vector<std::string> Store::applyJsonl(const std::string& text) {
  std::vector<std::string> errors;
  std::istringstream lines(text);
  std::string line;
  for (int n = 1; std::getline(lines, line); ++n) {
    if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
    json message = json::parse(line, nullptr, false);
    std::string error = message.is_discarded() ? "invalid JSON" : apply(message);
    if (!error.empty()) errors.push_back("line " + std::to_string(n) + ": " + error);
  }
  return errors;
}

Surface* Store::surface(const std::string& id) {
  auto it = surfaces_.find(id);
  return it == surfaces_.end() ? nullptr : &it->second;
}
const Surface* Store::surface(const std::string& id) const {
  auto it = surfaces_.find(id);
  return it == surfaces_.end() ? nullptr : &it->second;
}
const Surface* Store::top() const { return order_.size() > stageStart_ ? surface(order_.back()) : nullptr; }

void Store::forget(const std::string& id) {
  auto it = std::find(order_.begin(), order_.end(), id);
  if (it == order_.end()) return;
  if (size_t(it - order_.begin()) < stageStart_) --stageStart_;
  order_.erase(it);
}
std::vector<const Surface*> Store::surfaces() const {
  std::vector<const Surface*> out;
  for (const auto& id : order_) out.push_back(surface(id));
  return out;
}

void Store::setData(const std::string& surfaceId, const std::string& pointer, const json& value) {
  Surface* s = surface(surfaceId);
  if (!s) return;
  if (pointer.empty() || pointer == "/") {
    s->data = value.is_null() ? json::object() : value;
    return;
  }
  json* cur = &s->data;
  std::string token;
  std::istringstream parts(pointer.substr(1));
  std::vector<std::string> tokens;
  while (std::getline(parts, token, '/')) tokens.push_back(token);
  if (value.is_null()) {
    // 1.0: null deletes the key at the path.
    json* parent = &s->data;
    for (size_t i = 0; i + 1 < tokens.size(); ++i) {
      if (parent->is_object() && parent->contains(tokens[i])) parent = &(*parent)[tokens[i]];
      else if (parent->is_array() && std::all_of(tokens[i].begin(), tokens[i].end(), ::isdigit) && std::stoul(tokens[i]) < parent->size()) parent = &(*parent)[std::stoul(tokens[i])];
      else return;
    }
    if (parent->is_object()) parent->erase(tokens.back());
    else if (parent->is_array() && std::all_of(tokens.back().begin(), tokens.back().end(), ::isdigit) && std::stoul(tokens.back()) < parent->size())
      parent->erase(parent->begin() + long(std::stoul(tokens.back())));
    return;
  }
  for (size_t i = 0; i < tokens.size(); ++i) {
    if (!cur->is_object() && !cur->is_array()) *cur = json::object();
    if (cur->is_array()) {
      size_t index = std::stoul(tokens[i]);
      if (index >= cur->size()) cur->get_ref<json::array_t&>().resize(index + 1);
      cur = &(*cur)[index];
    } else {
      cur = &(*cur)[tokens[i]];
    }
  }
  *cur = value;
}

const json* lookup(const json& data, const std::string& pointer) {
  if (pointer.empty() || pointer == "/") return &data;
  try {
    const json& v = data.at(json::json_pointer(pointer));
    return &v;
  } catch (...) {
    return nullptr;
  }
}

json resolve(const json& value, const json& data, const json* scope) {
  if (value.is_object() && value.contains("@path") && value["@path"].is_string()) {
    std::string path = value["@path"].get<std::string>();
    const json* found = nullptr;
    if (!path.empty() && path[0] == '/') found = lookup(data, path);
    else if (scope) found = lookup(*scope, "/" + path);
    return found ? *found : json();
  }
  if (value.is_object() && value.contains("@call")) return json();  // functions: not evaluated
  return value;
}

std::string resolveString(const json& value, const json& data, const json* scope) {
  json v = resolve(value, data, scope);
  if (v.is_string()) return v.get<std::string>();
  if (v.is_null()) return "";
  if (v.is_number_integer()) return std::to_string(v.get<long long>());
  return v.dump();
}

json resolveContext(const json& context, const json& data, const json* scope) {
  json out = json::object();
  if (!context.is_object()) return out;
  for (auto it = context.begin(); it != context.end(); ++it) out[it.key()] = resolve(it.value(), data, scope);
  return out;
}

}  // namespace a2ui
