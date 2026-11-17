#include "a2ui/session.hpp"

#include <algorithm>

namespace a2ui {

std::vector<json> SseParser::feed(const std::string& bytes) {
  std::vector<json> events;
  buffer_ += bytes;
  // Events end with a blank line; tolerate CRLF.
  for (;;) {
    size_t end = buffer_.find("\n\n");
    size_t crlf = buffer_.find("\r\n\r\n");
    size_t sep = std::min(end, crlf);
    if (sep == std::string::npos) break;
    size_t sepLen = sep == crlf ? 4 : 2;
    std::string block = buffer_.substr(0, sep);
    buffer_.erase(0, sep + sepLen);
    std::string data;
    size_t pos = 0;
    while (pos <= block.size()) {
      size_t nl = block.find('\n', pos);
      std::string line = block.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.rfind("data:", 0) == 0) {
        std::string value = line.substr(5);
        if (!value.empty() && value[0] == ' ') value.erase(0, 1);
        if (!data.empty()) data += "\n";
        data += value;
      }
      if (nl == std::string::npos) break;
      pos = nl + 1;
    }
    if (data.empty()) continue;
    json event = json::parse(data, nullptr, false);
    if (!event.is_discarded()) events.push_back(std::move(event));
  }
  return events;
}

void Session::clear() {
  running_ = false;
  surfacesThisRun_ = 0;
  runStart_ = 0;
  caption_.clear();
  status_.clear();
  error_.clear();
  progressLabel_.clear();
  progress_ = -1;
  transcript_.clear();
  functionCalls_.clear();
  followUps_.clear();
}

void Session::addUser(const std::string& text) {
  if (text.empty()) return;
  followUps_.clear();
  transcript_.push_back({ChatLine::Role::User, text, "", ""});
  if (transcript_.size() > 200) transcript_.erase(transcript_.begin(), transcript_.begin() + 50);
}

void Session::dropNarration() {
  // Text before a console tool is narration ("I'll check…"), not the answer.
  caption_.clear();
  for (size_t i = transcript_.size(); i-- > runStart_;)
    if (transcript_[i].role == ChatLine::Role::Assistant) transcript_.erase(transcript_.begin() + i);
}

void Session::handle(const json& e) {
  const std::string type = e.value("type", "");
  if (type == "RUN_STARTED") {
    running_ = true;
    surfacesThisRun_ = 0;
    runStart_ = transcript_.size();
    caption_.clear();
    error_.clear();
    followUps_.clear();
    status_ = "Thinking…";
  } else if (type == "TEXT_MESSAGE_START") {
    // Several messages per run: the latest one is the one that goes with the screen.
    caption_.clear();
    transcript_.push_back({ChatLine::Role::Assistant, "", "", ""});
  } else if (type == "TEXT_MESSAGE_CONTENT") {
    if (transcript_.empty() || transcript_.back().role != ChatLine::Role::Assistant || transcript_.size() <= runStart_)
      transcript_.push_back({ChatLine::Role::Assistant, "", "", ""});
    transcript_.back().text += e.value("delta", "");
    caption_ = transcript_.back().text;
  } else if (type == "ACTIVITY_SNAPSHOT" && e.value("activityType", "") == "generative-ui-function-call") {
    // callRendererFunction: a catalog function for this client to run now (a rumble, the light bar).
    const json& c = e.contains("content") ? e["content"] : json();
    if (c.is_object() && c.contains("callFunction") && c["callFunction"].is_object() && c["callFunction"].contains("@call") && c["callFunction"]["@call"].is_string())
      functionCalls_.push_back(c["callFunction"]);
  } else if (type == "ACTIVITY_SNAPSHOT") {
    std::string err = store_.apply(e);
    if (!err.empty()) error_ = "Bad screen from the agent: " + err;
    const json& content = e.contains("content") && e["content"].is_object() ? e["content"] : json::object();
    const char* key = content.contains("operations") ? "operations" : "a2ui_operations";
    if (content.contains(key) && content[key].is_array()) {
      const json& ops = content[key];
      // A removed surface comes as its createSurface then its deleteSurface: not a new screen.
      std::vector<std::string> deleted;
      for (const auto& op : ops)
        if (op.contains("deleteSurface")) deleted.push_back(op["deleteSurface"].value("surfaceId", ""));
      std::vector<std::string> created;
      auto notice = [&](const json& components) {
        // A notice is news: it goes in the conversation too, so it outlives the toast.
        if (!components.is_array()) return;
        for (const auto& comp : components)
          if (comp.value("id", "") == "root" && comp.value("component", "") == "Notice" && comp.contains("text") && comp["text"].is_string())
            transcript_.push_back({ChatLine::Role::Notice, comp["text"].get<std::string>(), comp.value("icon", "info"), ""});
      };
      for (const auto& op : ops) {
        if (op.contains("createSurface")) {
          const std::string id = op["createSurface"].value("surfaceId", "");
          if (std::find(deleted.begin(), deleted.end(), id) != deleted.end()) continue;
          // A snapshot re-sends its createSurface: count a surface once per run.
          bool known = std::any_of(transcript_.begin(), transcript_.end(), [&](const ChatLine& l) { return l.surfaceId == id; });
          if (!known) ++surfacesThisRun_;
          created.push_back(id);
          if (!known && op["createSurface"].contains("components")) notice(op["createSurface"]["components"]);
        }
        if (op.contains("updateComponents")) notice(op["updateComponents"].value("components", json::array()));
      }
      // A new screen is the agent's reply: it takes its place in the conversation (dialogs and
      // notices don't; they're system UI).
      for (const std::string& id : created) {
        const Surface* sf = store_.surface(id);
        const json* root = sf ? sf->component("root") : nullptr;
        std::string type = root ? root->value("component", "") : "";
        if (!root || type == "Notice" || type == "Confirm" || type == "TextPrompt" || type == "VoicePrompt") continue;
        bool known = std::any_of(transcript_.begin(), transcript_.end(), [&](const ChatLine& l) { return l.surfaceId == id; });
        if (known) continue;
        std::string title = root->contains("title") ? resolveString((*root)["title"], sf->data, nullptr) : "";
        transcript_.push_back({ChatLine::Role::Surface, title.empty() ? type : title, "", id});
      }
    }
  } else if (type == "CUSTOM" && e.value("name", "") == "ps5.suggestions") {
    const json& v = e.contains("value") ? e["value"] : json();
    followUps_.clear();
    if (v.is_object() && v.contains("prompts") && v["prompts"].is_array())
      for (const auto& p : v["prompts"])
        if (p.is_string() && !p.get<std::string>().empty() && followUps_.size() < 3) {
          std::string text = p.get<std::string>();
          if (text.size() > 120) {  // a button label; cut on a character boundary
            size_t cut = 120;
            while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
            text.resize(cut);
          }
          followUps_.push_back(text);
        }
  } else if (type == "CUSTOM" && e.value("name", "") == "ps5.agent") {
    const json& v = e.contains("value") ? e["value"] : json();
    if (v.is_object() && v.contains("name") && v["name"].is_string()) {
      std::string name = v["name"].get<std::string>();
      if (name.size() > 40) {  // a header, not a paragraph; cut on a character boundary
        size_t cut = 40;
        while (cut > 0 && (static_cast<unsigned char>(name[cut]) & 0xC0) == 0x80) --cut;
        name.resize(cut);
      }
      agentName_ = name;
    }
  } else if (type == "CUSTOM" && e.value("name", "") == "status") {
    const json& v = e["value"];
    std::string state = v.value("state", "");
    if (state == "working" && v.contains("progress") && v["progress"].is_number()) {
      progress_ = std::clamp(v["progress"].get<float>(), 0.0f, 1.0f);
      progressLabel_ = v.value("detail", std::string());
      status_ = progressLabel_ + "… " + std::to_string(int(progress_ * 100 + 0.5f)) + "%";
      return;
    }
    progress_ = -1;
    progressLabel_.clear();
    if (state == "thinking") status_ = "Thinking…";
    else if (state == "working") {
      status_ = "Working: " + v.value("tool", std::string("console"));
      dropNarration();
    } else if (state == "retrying") status_ = "Busy, retrying in " + std::to_string(v.value("retryInSeconds", 0)) + " s";
    else if (state == "waiting_for_approval") status_ = "";
    else if (state == "gave_up") error_ = "The agent got stuck; try asking again.";
  } else if (type == "RUN_FINISHED" || type == "RUN_ERROR") {
    running_ = false;
    progress_ = -1;
    progressLabel_.clear();
    status_.clear();
    if (type == "RUN_ERROR") error_ = e.value("message", std::string("The agent failed."));
    for (size_t i = transcript_.size(); i-- > runStart_;)
      if (transcript_[i].role == ChatLine::Role::Assistant && transcript_[i].text.empty()) transcript_.erase(transcript_.begin() + i);
    // A text-only answer: the old screen no longer applies (it stays reopenable from its chip).
    if (surfacesThisRun_ == 0 && type == "RUN_FINISHED") store_.clearStage();
  }
}

}  // namespace a2ui
