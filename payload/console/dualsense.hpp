// The PS5 catalog's controller functions on the DualSense: haptics (named effects, patterns,
// rumble; played by a2ui::HapticPlayer, call update() every frame) and the light bar through SDL
// (its PS5 driver calls scePad), adaptive triggers through scePadSetTriggerEffect. The agent asks
// for them with controller_feedback (callRendererFunction, "generative-ui-function-call"); screens can also bind them to
// buttons (functionCall actions).
#pragma once

#include <SDL.h>

#include <string>

#include "nlohmann/json.hpp"

namespace dualsense {

/** Run one catalog function call ({"call": "rumble", "args": {...}}). Returns a line for the log. */
std::string run(SDL_Joystick* joystick, const nlohmann::json& call);

/** Advance what's playing on the motors. Call once per frame. */
void update(SDL_Joystick* joystick);

/** Motors and triggers off and the light bar back to the system's color (before quitting). */
void reset(SDL_Joystick* joystick);

}  // namespace dualsense
