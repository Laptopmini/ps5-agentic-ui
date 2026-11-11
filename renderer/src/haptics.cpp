#include "a2ui/haptics.hpp"

#include <algorithm>
#include <cmath>

namespace a2ui {
namespace {

using nlohmann::json;

struct Named {
  const char* name;
  std::vector<HapticStep> steps;
};

// Short and distinct by rhythm as much as strength, so they can be told apart without looking.
// Gaps are steps with both motors off.
const std::vector<Named>& table() {
  static const std::vector<Named> effects = {
      // A light click, for a selection or a small acknowledgment.
      {"tick", {{0, 0.35f, 30}}},
      // One firm pulse: something was accepted.
      {"confirm", {{0.45f, 0.6f, 70}}},
      // Two pulses, the second stronger: done, it worked.
      {"success", {{0, 0.5f, 60}, {0, 0, 70}, {0.5f, 0.8f, 100}}},
      // Two even, deeper pulses: careful, or not quite.
      {"warning", {{0.6f, 0.3f, 140}, {0, 0, 110}, {0.6f, 0.3f, 140}}},
      // Three heavy pulses: it failed.
      {"error", {{0.95f, 0.3f, 110}, {0, 0, 80}, {0.95f, 0.3f, 110}, {0, 0, 80}, {0.95f, 0.3f, 110}}},
      // A rising buzz: look at the screen.
      {"attention", {{0.2f, 0.4f, 120}, {0.4f, 0.6f, 120}, {0.7f, 0.9f, 160}}},
  };
  return effects;
}

float level(const json& v) {
  if (!v.is_number()) return 0;
  double x = v.get<double>();
  return std::isfinite(x) ? float(std::clamp(x, 0.0, 1.0)) : 0;
}

}  // namespace

const std::vector<std::string>& hapticEffects() {
  static const std::vector<std::string> names = [] {
    std::vector<std::string> out;
    for (const auto& e : table()) out.push_back(e.name);
    return out;
  }();
  return names;
}

std::vector<HapticStep> hapticEffect(const std::string& name) {
  for (const auto& e : table())
    if (name == e.name) return e.steps;
  return {};
}

std::optional<std::vector<HapticStep>> hapticSteps(const json& call, std::string* problem) {
  auto fail = [&](const std::string& why) {
    if (problem) *problem = why;
    return std::vector<HapticStep>{};
  };
  const std::string name = call.is_object() ? call.value("@call", "") : "";
  const json args = call.is_object() && call.contains("args") && call["args"].is_object() ? call["args"] : json::object();
  if (name == "haptic") {
    auto steps = hapticEffect(args.value("effect", ""));
    return steps.empty() ? fail("unknown haptic effect") : steps;
  }
  if (name == "rumble") {
    int ms = args.contains("durationMs") && args["durationMs"].is_number() ? args["durationMs"].get<int>() : 200;
    float x = args.contains("intensity") ? level(args["intensity"]) : 0.5f;
    return std::vector<HapticStep>{{x, x, std::clamp(ms, 10, kHapticMaxMs)}};
  }
  if (name != "hapticPattern") return std::nullopt;
  if (!args.contains("steps") || !args["steps"].is_array() || args["steps"].empty()) return fail("hapticPattern needs steps");
  if (args["steps"].size() > size_t(kHapticMaxSteps)) return fail("at most 16 steps");
  std::vector<HapticStep> steps;
  int total = 0;
  for (const json& s : args["steps"]) {
    if (!s.is_object() || !s.contains("ms") || !s["ms"].is_number()) return fail("each step needs ms");
    int ms = std::clamp(s["ms"].get<int>(), 10, kHapticMaxMs);
    // The whole pattern stops at the bound: a safety limit, not a style choice.
    ms = std::min(ms, kHapticMaxMs - total);
    if (ms <= 0) break;
    total += ms;
    steps.push_back({s.contains("low") ? level(s["low"]) : 0, s.contains("high") ? level(s["high"]) : 0, ms});
  }
  return steps;
}

void HapticPlayer::play(std::vector<HapticStep> steps, Clock::time_point now) {
  steps_ = std::move(steps);
  start_ = now;
  sent_ = -1;
}

void HapticPlayer::stop() {
  if (!steps_.empty()) sent_ = -2;  // update() sends one "off"
  steps_.clear();
}

std::optional<HapticPlayer::Output> HapticPlayer::update(Clock::time_point now, int refreshMs) {
  if (sent_ == -2) {
    sent_ = -1;
    return Output{0, 0, 0};
  }
  if (steps_.empty()) return std::nullopt;
  long elapsed = long(std::chrono::duration_cast<std::chrono::milliseconds>(now - start_).count());
  int index = 0, begin = 0;
  while (index < int(steps_.size()) && elapsed >= begin + steps_[index].ms) begin += steps_[index++].ms;
  if (index == int(steps_.size())) {  // finished: motors off once
    steps_.clear();
    sent_ = -1;
    return Output{0, 0, 0};
  }
  const HapticStep& s = steps_[index];
  const int left = int(begin + s.ms - elapsed);
  const bool fresh = index != sent_;
  const bool refresh = !fresh && now - sentAt_ >= std::chrono::milliseconds(refreshMs) && (s.low > 0 || s.high > 0);
  if (!fresh && !refresh) return std::nullopt;
  sent_ = index;
  sentAt_ = now;
  return Output{s.low, s.high, left};
}

}  // namespace a2ui
