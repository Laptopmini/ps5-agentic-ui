// The smallest use of the a2ui library: one A2UI 1.0 surface in, draw calls and an action out.
//
// A real app implements Canvas for its display (see renderer/src/canvas_sdl.cpp for SDL2 and
// canvas_gl.cpp for OpenGL ES). This one prints what it's asked to draw, so it runs anywhere.
#include <a2ui/model.hpp>
#include <a2ui/view.hpp>

#include <cstdio>

namespace {

/** A Canvas that prints the text it's asked to draw and counts the shapes. */
class PrintCanvas : public a2ui::Canvas {
 public:
  int shapes = 0;
  void clear(a2ui::Color, a2ui::Color) override {}
  void fillRect(a2ui::Rect, a2ui::Color, float) override { ++shapes; }
  void strokeRect(a2ui::Rect, a2ui::Color, float, float) override { ++shapes; }
  void fillCircle(float, float, float, a2ui::Color) override { ++shapes; }
  void strokeCircle(float, float, float, a2ui::Color, float) override { ++shapes; }
  void fillTriangle(float, float, float, float, float, float, a2ui::Color) override { ++shapes; }
  float text(float x, float y, const std::string& utf8, float size, a2ui::Color) override {
    std::printf("  text at (%4.0f, %4.0f): %s\n", x, y, utf8.c_str());
    return measure(utf8, size);
  }
  float measure(const std::string& utf8, float size) override { return utf8.size() * size * 0.5f; }
  float lineHeight(float size) override { return size * 1.3f; }
};

// What an agent sends: a menu on the PS5 catalog, as one A2UI 1.0 createSurface message.
const char* kSurface = R"({"version": "1.0", "createSurface": {"surfaceId": "hello", "catalogId": "https://raw.githubusercontent.com/Laptopmini/ps5-agentic-ui/catalog-v1/catalog/ps5.catalog.json",
  "components": [
    {"id": "root", "component": "Screen", "title": "Hello from A2UI", "children": ["menu"]},
    {"id": "menu", "component": "Menu", "children": ["yes", "no"]},
    {"id": "yes", "component": "MenuItem", "label": "Say hello", "action": {"event": {"name": "greet", "context": {"answer": "hello"}}}},
    {"id": "no", "component": "MenuItem", "label": "Stay quiet", "action": {"event": {"name": "greet", "context": {"answer": "quiet"}}}}
  ]}})";

}  // namespace

int main() {
  a2ui::Store store;
  if (const std::string error = store.apply(a2ui::json::parse(kSurface)); !error.empty()) {
    std::fprintf(stderr, "surface rejected: %s\n", error.c_str());
    return 1;
  }

  a2ui::View view(store);
  int actions = 0;
  view.onAction = [&](const a2ui::UserAction& action) {
    ++actions;
    std::printf("action for the agent: %s\n", action.toJson().dump().c_str());
  };

  PrintCanvas canvas;
  std::printf("first frame:\n");
  view.draw(canvas);
  std::printf("  (%d shapes), focus on \"%s\"\n", canvas.shapes, view.focused().c_str());

  // The player presses Down, then Cross: focus moves to the second item and it fires its action.
  view.press(a2ui::Button::Down);
  view.draw(canvas);
  view.press(a2ui::Button::Cross);
  return actions == 1 ? 0 : 1;
}
