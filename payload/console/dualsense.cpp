#include "dualsense.hpp"

#include "a2ui/haptics.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

extern "C" {
int sceUserServiceGetForegroundUser(int32_t* userId);
int scePadGetHandle(int32_t userId, int32_t type, int32_t index);
int scePadResetLightBar(int32_t handle);

// scePadSetTriggerEffect's parameter: which triggers to set (mask), then one 56-byte command per
// trigger (L2 first). Each command is a mode and that mode's byte parameters:
//   feedback  {position 0-9, strength 1-8}
//   weapon    {start 2-7, end start+1-8, strength 0-8}
//   vibration {position 0-9, amplitude 1-8, frequency Hz}
// Layout as used by public PS5 homebrew; not from a Sony header.
struct TriggerCommand {
  uint32_t mode;
  uint8_t reserved[4];
  uint8_t data[48];
};
struct TriggerEffect {
  uint8_t mask;
  uint8_t reserved[7];
  TriggerCommand command[2];
};
static_assert(sizeof(TriggerCommand) == 56 && sizeof(TriggerEffect) == 120, "scePad trigger effect layout");
int scePadSetTriggerEffect(int32_t handle, const TriggerEffect* effect);
}

namespace dualsense {
namespace {

using nlohmann::json;
enum Mode : uint32_t { kOff = 0, kFeedback = 1, kWeapon = 2, kVibration = 3 };

int padHandle() {
  int32_t user = -1;
  if (sceUserServiceGetForegroundUser(&user) != 0) return -1;
  return scePadGetHandle(user, 0, 0);
}

std::string hex(int err) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "0x%08x", unsigned(err));
  return buf;
}

a2ui::HapticPlayer& player() {
  static a2ui::HapticPlayer p;
  return p;
}

std::string setTrigger(const std::string& trigger, const std::string& effect, double strength) {
  const int handle = padHandle();
  if (handle < 0) return "no pad handle (" + hex(handle) + ")";
  TriggerEffect e;
  std::memset(&e, 0, sizeof e);
  const int index = trigger == "l2" ? 0 : 1;
  e.mask = uint8_t(1u << index);
  TriggerCommand& c = e.command[index];
  const uint8_t level = uint8_t(std::clamp(1 + int(std::lround(strength * 7)), 1, 8));
  if (effect == "feedback") {
    c.mode = kFeedback;
    c.data[0] = 2;
    c.data[1] = level;
  } else if (effect == "weapon") {
    c.mode = kWeapon;
    c.data[0] = 2;
    c.data[1] = 6;
    c.data[2] = level;
  } else if (effect == "vibration") {
    c.mode = kVibration;
    c.data[0] = 2;
    c.data[1] = level;
    c.data[2] = 30;
  } else {
    c.mode = kOff;
  }
  int err = scePadSetTriggerEffect(handle, &e);
  return err ? "scePadSetTriggerEffect " + hex(err) : "ok";
}

}  // namespace

std::string run(SDL_Joystick* joystick, const json& call) {
  const std::string name = call.value("@call", "");
  const json args = call.contains("args") && call["args"].is_object() ? call["args"] : json::object();
  std::string problem;
  if (auto steps = a2ui::hapticSteps(call, &problem)) {
    if (!joystick) return name + ": no controller";
    if (steps->empty()) return name + ": " + problem;
    int total = 0;
    for (const auto& st : *steps) total += st.ms;
    player().play(std::move(*steps));
    std::string what = name == "haptic" ? "haptic " + args.value("effect", "") : name;
    return what + " (" + std::to_string(total) + " ms): playing";
  }
  if (name == "lightbar") {
    if (!joystick) return "lightbar: no controller";
    unsigned rgb = 0;
    const std::string color = args.value("color", "");
    if (color.size() != 7 || std::sscanf(color.c_str() + 1, "%6x", &rgb) != 1) return "lightbar: bad color";
    int err = SDL_JoystickSetLED(joystick, Uint8(rgb >> 16), Uint8(rgb >> 8), Uint8(rgb));
    return "lightbar " + color + ": " + (err ? SDL_GetError() : "ok");
  }
  if (name == "triggerEffect") {
    const std::string trigger = args.value("trigger", ""), effect = args.value("effect", "off");
    if (trigger != "l2" && trigger != "r2") return "triggerEffect: bad trigger";
    return "triggerEffect " + trigger + " " + effect + ": " + setTrigger(trigger, effect, args.value("strength", 0.5));
  }
  return "unknown function " + name;
}

void update(SDL_Joystick* joystick) {
  if (!joystick) return;
  if (auto out = player().update()) {
    // The step's own length as the duration: if the app stalls, SDL still stops the motors.
    SDL_JoystickRumble(joystick, Uint16(out->low * 0xFFFF), Uint16(out->high * 0xFFFF), Uint32(std::max(out->ms, 0)));
  }
}

void reset(SDL_Joystick* joystick) {
  player().stop();
  setTrigger("l2", "off", 0);
  setTrigger("r2", "off", 0);
  const int handle = padHandle();
  if (handle >= 0) scePadResetLightBar(handle);
  if (joystick) SDL_JoystickRumble(joystick, 0, 0, 0);
}

}  // namespace dualsense
