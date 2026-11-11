// Haptics as part of the UI. The agent (or a screen's action) names how something should feel,
// "success" or "error" like a design token, and the renderer decides what that is on its hardware.
// For what the names don't cover there is a bounded pattern: steps of two motor levels and a length.
//
// The catalog functions (catalog/ps5.catalog.json):
//   {"@call": "haptic",        "args": {"effect": "success"}}
//   {"@call": "hapticPattern", "args": {"steps": [{"low": 0.8, "high": 0.2, "ms": 120}, ...]}}
//   {"@call": "rumble",        "args": {"intensity": 1, "durationMs": 400}}   (one step, both motors)
// A screen attaches an effect to an action with a sibling property: "haptic" next to "action"
// (MenuItem, Tile, a ButtonHints hint), "confirmHaptic"/"cancelHaptic" on Confirm, "submitHaptic"
// on TextPrompt. The view then calls onFunction with a "haptic" call when that action runs.
//
// `low` is the deep (large) motor, `high` the sharp (small) one, as on most gamepads. A host with
// other hardware (a phone, a web gamepad) maps the same names to what it has.
#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace a2ui {

struct HapticStep {
  float low = 0, high = 0;  // 0 to 1
  int ms = 0;
};

/** Safety bounds for any pattern: how many steps, how long one step and the whole pattern can be. */
constexpr int kHapticMaxSteps = 16;
constexpr int kHapticMaxMs = 10000;

/** The named effects, in the catalog's order. */
const std::vector<std::string>& hapticEffects();
/** The steps for a named effect on a two-motor pad; empty if the name is unknown. */
std::vector<HapticStep> hapticEffect(const std::string& name);

/**
 * A catalog call ("haptic", "hapticPattern" or "rumble") as steps within the bounds above.
 * Returns nullopt if the call isn't a haptic one; sets `problem` when it is but can't be played.
 */
std::optional<std::vector<HapticStep>> hapticSteps(const nlohmann::json& call, std::string* problem = nullptr);

/**
 * Plays steps over time. The host calls update() every frame and sends the levels it returns to
 * the motors; a new play() replaces what's playing.
 */
class HapticPlayer {
 public:
  using Clock = std::chrono::steady_clock;
  struct Output {
    float low, high;
    int ms;  // how long these levels last (send it as the rumble duration, so a hang still stops)
  };

  void play(std::vector<HapticStep> steps, Clock::time_point now = Clock::now());
  void stop();
  bool playing() const { return !steps_.empty(); }
  /**
   * Levels to send now, or nullopt if nothing changed since the last call. Unchanged levels are
   * sent again every `refreshMs`, for pads that stop a long rumble on their own.
   */
  std::optional<Output> update(Clock::time_point now = Clock::now(), int refreshMs = 1000);

 private:
  std::vector<HapticStep> steps_;
  Clock::time_point start_;
  int sent_ = -1;  // index of the step last sent; steps_.size() = the final "off"
  Clock::time_point sentAt_;
};

}  // namespace a2ui
