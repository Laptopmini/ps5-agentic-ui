// Track A render-smoke (docs/packaging.md): a native FSELF title that proves the software
// SdlCanvas renders on the PS5 with no Mesa (no GL/EGL/Vulkan). It draws a fixed test frame and
// sends a notification on start. No agent, JSON, curl or QuickJS — just SDL (static) + the
// renderer's 2D canvas, the same backend the full app would use as a native title.
//
// Packaged with scripts/ps5-native.sh (ps5-native-tool link + self --sign), installed at
// /data/homebrew/PPSA99501, launched from the home screen.
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <cstdint>
#include <string>
#include <vector>

#include "canvas_sdl.hpp"

extern "C" {
int sceNotificationSend(int userId, bool isLogged, const char* payload);
}

extern const unsigned char font_ttf[], font_ttf_end[];

using namespace a2ui;

namespace {
void notify(const std::string& text) {
  const std::string body =
      "{\"rawData\":{\"viewTemplateType\":\"TemplateA\",\"channelType\":\"Downloads\",\"useCaseId\":\"IDC\","
      "\"toastOverwriteType\":\"No\",\"isImmediate\":true,\"priority\":100,\"viewData\":{\"message\":{\"body\":\"" +
      text + "\"}}},\"createdDateTime\":\"2026-11-10T00:00:00.000Z\",\"localNotificationId\":\"990011\"}";
  sceNotificationSend(0xFE, true, body.c_str());
}
}  // namespace

int main(int, char**) {
  SDL_SetMainReady();
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) != 0) {
    notify(std::string("Smoke: SDL_Init failed: ") + SDL_GetError());
    return 1;
  }
  SDL_Window* window = SDL_CreateWindow("ps5-agent-smoke", 0, 0, 1920, 1080, SDL_WINDOW_FULLSCREEN);
  if (!window) {
    notify(std::string("Smoke: SDL_CreateWindow failed: ") + SDL_GetError());
    return 1;
  }
  SDL_Surface* surface = nullptr;
  SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, 0);
  if (!renderer) {
    surface = SDL_GetWindowSurface(window);
    renderer = surface ? SDL_CreateSoftwareRenderer(surface) : nullptr;
  }
  if (!renderer) {
    notify(std::string("Smoke: no renderer: ") + SDL_GetError());
    return 1;
  }
  SDL_RenderSetLogicalSize(renderer, int(View::kWidth), int(View::kHeight));

  std::vector<unsigned char> font(font_ttf, font_ttf_end);
  SdlCanvas canvas(renderer, font);
  notify("Render-smoke running: software canvas, native title, no Mesa.");

  SDL_GameControllerOpen(0);
  const Color bgTop{20, 22, 34, 255}, bgBottom{12, 14, 22, 255};
  const Color text{235, 238, 245, 255}, muted{170, 178, 196, 255}, accent{80, 128, 255, 255};
  const Color tiles[4] = {{72, 110, 220, 255}, {60, 180, 150, 255}, {210, 120, 80, 255}, {150, 90, 200, 255}};

  bool running = true;
  while (running) {
    SDL_Event e;
    while (SDL_PollEvent(&e))
      if (e.type == SDL_QUIT) running = false;

    canvas.clear(bgTop, bgBottom);
    canvas.fillRect({120, 64, 72, 72}, accent, 16);
    canvas.text(212, 82, "PS5 Agent", 44, text);
    canvas.text(120, 180, "Native title — software renderer — no Mesa", 34, muted);
    for (int i = 0; i < 4; ++i) {
      Rect r{120.0f + i * 320.0f, 320, 280, 280};
      canvas.fillRect(r, tiles[i], 22);
      canvas.text(r.x + 20, r.y + r.h - 50, "Tile " + std::to_string(i + 1), 28, text);
    }
    canvas.text(120, 660, "If you can read this crisply, Track A works.", 30, {160, 200, 255, 255});
    canvas.text(120, 940, "Close from the card switcher to exit.", 26, muted);

    SDL_RenderPresent(renderer);
    if (surface) SDL_UpdateWindowSurface(window);
    SDL_Delay(16);
  }

  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
