#include "a2ui/view.hpp"

#include "a2ui/haptics.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <random>
#include <functional>

#include "qrcodegen/qrcodegen.hpp"

namespace a2ui {
namespace {

// Palette: dark graphite backdrop (like the console's conversation screens), glass panels, white focus.
constexpr Color kBgTop{46, 48, 56}, kBgBottom{9, 10, 13};
constexpr Color kPanel{255, 255, 255, 18}, kPanelFocus{255, 255, 255, 40}, kBorder{255, 255, 255, 34};
constexpr Color kText{255, 255, 255}, kMuted{172, 176, 186}, kAccent{64, 128, 255}, kWarn{255, 92, 104};
constexpr Color kFocus{255, 255, 255}, kRule{255, 255, 255, 26};
constexpr float kMargin = 120, kHeaderY = 72, kBodyY = 196, kHintsY = 996;
// Two panes: the conversation on the left, the agent's screen on the right; the bar along the bottom.
constexpr float kChatX = 120, kChatW = 880, kPaneX = 1100, kPaneW = 700, kPaneTitleY = 150, kPaneBodyY = 226;
// The message field spans from just past the mic to the right edge of the conversation column
// (kChatX + kInlineW = 1440), so it lines up with the cards, bubbles and follow-up pills above it.
constexpr float kBarCY = 1012, kBarX = 214, kBarW = 1226, kChatBottom = 930;
constexpr float kInlineW = 1320;  // the conversation column in the inline layout

// A free-running seconds clock for the voice feedback animations (mic pulse, "…" dots). The host
// redraws every frame while voice is active (View::voiceActive), so reading wall time each draw
// gives smooth motion without the view holding its own frame counter.
double animClock() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// The agent's badge, in an s × s box: the app icon's conversation bubble (sce_sys/icon.svg)
// holding a small screen.
void drawBadge(Canvas& c, float x, float y, float s) {
  const float bodyH = 0.66f * s, tail = 0.16f * s, top = y + (s - bodyH - tail) / 2;
  c.fillRect({x, top, s, bodyH}, kAccent, 0.114f * s);
  c.fillTriangle(x + 0.24f * s, top + bodyH - 1, x + 0.44f * s, top + bodyH - 1, x + 0.24f * s, top + bodyH + tail, kAccent);
  c.fillRect({x + 0.159f * s, top + 0.125f * s, 0.682f * s, 0.057f * s}, kText, 0.028f * s);
  c.fillRect({x + 0.159f * s, top + 0.244f * s, 0.318f * s, 0.273f * s}, {255, 255, 255, 235}, 0.04f * s);
  c.fillRect({x + 0.523f * s, top + 0.244f * s, 0.318f * s, 0.273f * s}, {255, 255, 255, 115}, 0.04f * s);
}

// A demo of the agentic-UI tooling: each prompt leads the agent to a different PS5-catalog screen
// or controller effect, using the console's real data. A rotating subset is shown (drawSuggestions),
// so the "Try asking" list feels fresh between launches. The row shows the request it sends.
const std::vector<std::pair<std::string, std::string>> kSuggestions = {
    {"My game library", "Show my game library"},                                              // Shelf of tiles
    {"How's my console doing?", "How is my console doing? Model, system software, storage and temperatures."},  // info card + status chip
    {"What have I been playing?", "What did I play recently, and for how long?"},             // Shelf / list
    {"My saves and screenshots", "Which saves do I have, and how many screenshots have I taken?"},
    {"Help me pick a game", "Help me choose something to play tonight from my library."},     // Menu of choices
    {"Try the controller", "Show me what the controller can do"},                             // haptics, light bar, triggers
    {"How full is my console?", "How much storage is left, and what's taking up the most space?"},
    {"Read my temperatures", "What are my console's temperatures right now?"},                // live sensors
    {"What's my most-played game?", "Which of my games have I played the most, and for how long?"},
    {"Remind me to take a break", "Remind me to take a break in 30 minutes."},                // a notification while the app is closed
    {"Surprise me", "Surprise me with something fun about my console or my games."},          // open-ended
    {"A quick console check", "Give me a quick health check of my console: temperature, storage and uptime."},
    {"Something new to play", "Of the games I haven't played much, which should I try next?"},
    {"My newest screenshots", "Show me when I last took screenshots."},
};

Color noticeColor(const std::string& icon) {
  if (icon == "success" || icon == "trophy") return {61, 220, 132};
  if (icon == "warning") return {255, 176, 32};
  if (icon == "error") return {255, 92, 104};
  return {64, 128, 255};
}

Color stateColor(const std::string& state) {
  if (state == "ready") return {61, 220, 132};
  if (state == "busy") return {255, 176, 32};
  if (state == "error") return {255, 77, 94};
  return {140, 148, 166};
}

Color tileColor(const std::string& seed) {
  static const Color palette[] = {{58, 92, 196}, {150, 64, 170}, {28, 140, 140}, {196, 96, 48}, {70, 70, 150}, {170, 52, 84}, {44, 120, 70}};
  size_t h = std::hash<std::string>{}(seed);
  return palette[h % (sizeof(palette) / sizeof(palette[0]))];
}

/** Measures like the real canvas but draws nothing: lets a component know its height first. */
class MeasureCanvas : public Canvas {
 public:
  explicit MeasureCanvas(Canvas& real) : real_(real) {}
  void clear(Color, Color) override {}
  void fillRect(Rect, Color, float) override {}
  void strokeRect(Rect, Color, float, float) override {}
  void fillCircle(float, float, float, Color) override {}
  void strokeCircle(float, float, float, Color, float) override {}
  void fillTriangle(float, float, float, float, float, float, Color) override {}
  float text(float, float, const std::string& s, float size, Color) override { return real_.measure(s, size); }
  float measure(const std::string& s, float size) override { return real_.measure(s, size); }
  float lineHeight(float size) override { return real_.lineHeight(size); }

 private:
  Canvas& real_;
};

void thickLine(Canvas& c, float x1, float y1, float x2, float y2, float width, Color color) {
  float dx = x2 - x1, dy = y2 - y1, len = std::sqrt(dx * dx + dy * dy);
  if (len == 0) return;
  float nx = -dy / len * width / 2, ny = dx / len * width / 2;
  c.fillTriangle(x1 + nx, y1 + ny, x2 + nx, y2 + ny, x2 - nx, y2 - ny, color);
  c.fillTriangle(x1 + nx, y1 + ny, x2 - nx, y2 - ny, x1 - nx, y1 - ny, color);
}

bool isOverlay(const std::string& type) { return type == "Confirm" || type == "TextPrompt" || type == "VoicePrompt"; }

std::vector<std::string> splitWords(const std::string& s) {
  std::vector<std::string> words;
  std::string cur;
  for (char ch : s) {
    if (ch == ' ' || ch == '\n') {
      if (!cur.empty()) words.push_back(cur);
      cur.clear();
      if (ch == '\n') words.push_back("\n");
    } else {
      cur += ch;
    }
  }
  if (!cur.empty()) words.push_back(cur);
  return words;
}

std::string stripMarkdown(std::string s) {
  // The catalog honours **bold**; this renderer has one weight, so just drop the markers.
  for (size_t p; (p = s.find("**")) != std::string::npos;) s.erase(p, 2);
  if (s.rfind("# ", 0) == 0) s.erase(0, 2);
  return s;
}

}  // namespace

// ---------- frame ----------

void View::draw(Canvas& c) {
  const std::string focusBefore = focus_;
  focusables_.clear();
  buttonActions_.clear();
  keyboardOpen_ = dialogKeyboard_ = false;
  c.clear(kBgTop, kBgBottom);
  // A console-style backdrop over the gradient, if one is set and loadable (else the gradient
  // shows). A faint scrim keeps text legible over the image.
  if (!background_.empty() && c.image({0, 0, kWidth, kHeight}, background_, 0))
    c.fillRect({0, 0, kWidth, kHeight}, {9, 10, 13, 90});

  const Surface* s = store_.top();
  const json* root = s ? s->component("root") : nullptr;
  const std::string type = root ? root->value("component", "") : "";
  // A notice on its own is a toast (and a line in the conversation), not a screen.
  if (type == "Notice" || (s && !root)) s = nullptr;
  if (!s) surfaceShown_.clear();
  else if (s->id != surfaceShown_) {  // new surface: reset focus and keyboard, show the latest
    surfaceShown_ = s->id;
    focus_.clear();
    typing_.clear();
    backToLatest();
  }

  // Header: who the player is talking to, like a conversation's title.
  drawBadge(c, kChatX, kHeaderY, 60);
  c.text(kChatX + 80, kHeaderY + 30 - c.lineHeight(40) / 2, agentName_.empty() ? "Agent" : agentName_, 40, kText);
  // Top right, like a conversation's own menu: [Triangle] New conversation, then [Square] Help,
  // right-aligned. Help is always offered; New conversation only when there's one to leave.
  {
    float hr = kWidth - kMargin;
    auto headerHint = [&](const char* glyph, const std::string& label) {
      float lx = hr - c.measure(label, 26);
      c.text(lx, kHeaderY + 30 - c.lineHeight(26) / 2, label, 26, kMuted);
      drawButtonGlyph(c, glyph, lx - 12 - 15, kHeaderY + 30, 15);
      hr = lx - 12 - 30 - 40;  // next hint sits left of this glyph, with a gap
    };
    if (onNewConversation && !transcript_.empty()) headerHint("triangle", "New conversation");
    headerHint("square", "Help");
  }

  // Which layout: a dialog over the conversation, a wide screen, or the two panes.
  std::vector<std::string> overlays;
  bool wide = false;
  if (s && type == "Screen" && root->contains("children") && (*root)["children"].is_array()) {
    for (const auto& child : (*root)["children"]) {
      const json* comp = child.is_string() ? s->component(child.get<std::string>()) : nullptr;
      std::string t = comp ? comp->value("component", "") : "";
      if (isOverlay(t)) overlays.push_back(child.get<std::string>());
      if (t == "Shelf" || t == "Pages") wide = true;
    }
  }
  if (s && (type == "Shelf" || type == "Pages")) wide = true;
  const bool dialog = s && (isOverlay(type) || !overlays.empty());

  // The bar comes first when there's no screen, so it gets focus; after the screen's items otherwise.
  if (!s) drawBar(c, true);
  liveSurface_ = s && !isOverlay(type) ? s->id : "";
  if (!reopened_.empty()) {
    if (store_.surface(reopened_) && layout_ == Layout::Inline && !dialog) liveSurface_ = reopened_;
    else reopened_.clear();  // gone (the agent deleted it), or a dialog is up
  }
  if (layout_ == Layout::Inline) {
    if (!s && transcript_.empty()) drawSuggestions(c, kChatX + 44, kPaneTitleY, kPaneW);
    drawTranscript(c, kChatX, kPaneTitleY, kChatBottom, kInlineW);
  } else if (wide) {
    Ctx ctx{c, *s, nullptr, ""};
    drawScreen(ctx, *root, kMargin, kPaneTitleY, kWidth - 2 * kMargin, 40);
    drawTranscript(c, kChatX, kChatBottom - 120, kChatBottom, kChatW);  // just the latest
  } else {
    drawTranscript(c, kChatX, kPaneTitleY, kChatBottom, kChatW);
    if (!s) drawSuggestions(c, kPaneX, kPaneTitleY, kPaneW);
    else if (!isOverlay(type)) {
      Ctx ctx{c, *s, nullptr, ""};
      drawScreen(ctx, *root, kPaneX, kPaneTitleY, kPaneW, 34);
    }
  }
  if (s) drawBar(c, !dialog);
  drawChrome(c);

  if (dialog) {
    focusables_.clear();  // a dialog takes focus
    buttonActions_.clear();
    drawOverlay(c, *s, isOverlay(type) ? *root : *s->component(overlays.front()));
  }
  // Toast for a fresh notice, over everything but dialogs.
  const Surface* top = store_.top();
  const json* topRoot = top ? top->component("root") : nullptr;
  if (topRoot && topRoot->value("component", "") == "Notice") {
    if (onNotice) {
      // A system notification instead of our toast, once per notice.
      if (lastNotice_ != top->id) {
        lastNotice_ = top->id;
        onNotice(resolveString(topRoot->value("text", json("")), top->data, nullptr), topRoot->value("icon", "info"));
      }
    } else {
      Ctx toast{c, *top, nullptr, ""};
      drawComponent(toast, "root", kMargin, kBodyY, kWidth - 2 * kMargin);
    }
  }
  if (helpOpen_) drawHelp(c);  // the controls overlay, over everything
  settleFocus(c, focusBefore);
}

void View::drawHelp(Canvas& c) {
  // A centered panel over a dimmed screen, listing the controls the player has. The console-only
  // rows (voice, setup, quit, samples) show when the mic is present (voiceAvailable_ == console);
  // the host shows just the navigation rows. A glyph token draws its PlayStation glyph; a key with
  // no glyph (sticks, combos) draws as plain text.
  c.fillRect({0, 0, kWidth, kHeight}, {0, 0, 0, 170});

  struct Row { const char* glyph; const char* key; const char* desc; };
  std::vector<Row> rows = {
    {"cross", "", "Select, or open the keyboard"},
    {"circle", "", "Go back, or return to the latest"},
    {"triangle", "", "Start a new conversation"},
    {"square", "", "Show or hide this help"},
    {"options", "", "Jump to the message bar"},
    {"", "Left stick / D-pad", "Move the focus"},
    {"", "Right stick", "Scroll the conversation"},
  };
  if (voiceAvailable_) {
    rows.push_back({"r2", "", "Hold to speak to the assistant"});
    rows.push_back({"", "L1 + R1", "Set up or change the assistant's key"});
    rows.push_back({"", "Touchpad", "Browse the sample screens"});
    rows.push_back({"", "L3 + R3", "Quit"});
  }

  const float rowH = 62, titleH = 108, padY = 32;
  const float panelW = 1000, panelH = titleH + padY + rows.size() * rowH + 24;
  const float px = (kWidth - panelW) / 2, py = (kHeight - panelH) / 2;
  c.fillRect({px, py, panelW, panelH}, {24, 26, 32, 248}, 28);

  c.text(px + 64, py + 52 - c.lineHeight(40) / 2, "Controls", 40, kText);
  c.text(px + 64, py + 90 - c.lineHeight(24) / 2, "Press Square or Circle to close", 24, kMuted);

  const float keyRight = px + 380;  // right edge of the key column
  const float descX = px + 420;
  float y = py + titleH + padY;
  for (const Row& r : rows) {
    const float cy = y + rowH / 2;
    if (r.glyph[0]) {
      drawButtonGlyph(c, r.glyph, keyRight - 15, cy, 15);
    } else {
      float tw = c.measure(r.key, 26);
      c.text(keyRight - tw, cy - c.lineHeight(26) / 2, r.key, 26, kText);
    }
    c.text(descX, cy - c.lineHeight(26) / 2, r.desc, 26, kMuted);
    y += rowH;
  }
}

float View::drawScreen(Ctx& ctx, const json& root, float x, float y, float w, float titleSize) {
  Canvas& c = ctx.c;
  const Surface& s = ctx.s;
  if (root.value("component", "") != "Screen") return drawComponent(ctx, "root", x, y, w);
  float right = x + w;
  if (root.contains("status") && root["status"].is_string()) {
    const json* chip = s.component(root["status"].get<std::string>());
    if (chip) {
      std::string label = resolveString(chip->value("label", json("")), s.data, nullptr);
      float cw = c.measure(label, 26) + 72;
      right -= cw + 24;
      drawComponent(ctx, root["status"].get<std::string>(), x + w - cw, y - 6, cw);
    }
  }
  MeasureCanvas m(c);
  float th = drawWrapped(m, resolveString(root.value("title", json("")), s.data, nullptr), x, y, right - x, titleSize, kText);
  drawWrapped(c, resolveString(root.value("title", json("")), s.data, nullptr), x, y, right - x, titleSize, kText);
  float cy = std::max(y + th + 28, y + (kPaneBodyY - kPaneTitleY));
  if (root.contains("children") && root["children"].is_array()) {
    for (const auto& child : root["children"]) {
      if (!child.is_string()) continue;
      const json* comp = s.component(child.get<std::string>());
      if (comp && isOverlay(comp->value("component", ""))) continue;
      cy += drawComponent(ctx, child.get<std::string>(), x, cy, w) + 28;
    }
    cy -= 28;
  } else if (root.contains("children")) {
    cy += drawChildren(ctx, root["children"], x, cy, w, 28);
  }
  if (root.contains("hints") && root["hints"].is_string()) {
    const json* hints = s.component(root["hints"].get<std::string>());
    if (hints) drawHints(c, s, *hints);
  }
  return cy - y;
}

void View::drawRow(Canvas& c, const std::string& key, Rect r, const std::string& label, const std::string& detail, std::function<void()> activate) {
  // A list row like the console's action lists: label, detail under it, a thin rule between rows.
  bool f = isFocused(key);
  if (f) {
    c.fillRect(r, kPanelFocus, 12);
    c.strokeRect(r, kFocus, 12, 3);
  } else {
    c.fillRect({r.x + 20, r.y + r.h + 3, r.w - 40, 1}, kRule);
  }
  if (detail.empty()) {
    c.text(r.x + 24, r.y + (r.h - c.lineHeight(30)) / 2, label, 30, kText);
  } else {
    float top = r.y + (r.h - c.lineHeight(30) - c.lineHeight(23) - 2) / 2;
    c.text(r.x + 24, top, label, 30, kText);
    c.text(r.x + 24, top + c.lineHeight(30) + 2, detail, 23, kMuted);
  }
  addFocusable(key, r, std::move(activate));
}

void View::drawSuggestions(Canvas& c, float x, float y, float w) {
  c.text(x, y + 8, "Try asking", 28, kMuted);
  // A rotating subset so the list feels fresh between launches: a stable shuffle picked once, with
  // the window advancing each time the player returns to a blank conversation (New conversation).
  static const std::vector<size_t> order = [] {
    std::vector<size_t> o(kSuggestions.size());
    for (size_t i = 0; i < o.size(); ++i) o[i] = i;
    std::mt19937 gen(static_cast<unsigned>(std::time(nullptr)));
    std::shuffle(o.begin(), o.end(), gen);
    return o;
  }();
  const size_t shown = std::min<size_t>(6, kSuggestions.size());
  float cy = y + 76;
  for (size_t n = 0; n < shown; ++n) {
    const size_t i = order[(suggestOffset_ + n) % order.size()];
    const auto& [label, prompt] = kSuggestions[i];
    drawRow(c, "suggest#" + std::to_string(n), {x, cy, w, 92}, label, prompt, [this, prompt] {
      if (onPrompt) onPrompt(prompt);
    });
    cy += 92 + 8;
  }
}

void View::drawTranscript(Canvas& c, float x, float top, float bottom, float w) {
  // Newest at the bottom; older lines go up until they no longer fit.
  const bool inlineLayout = layout_ == Layout::Inline;
  std::vector<ChatLine> lines;
  for (const auto& l : transcript_)
    if (l.role != ChatLine::Role::Surface || inlineLayout) lines.push_back(l);
  // A live screen the conversation doesn't know about (a recorded stream, no Session): show it last.
  if (inlineLayout && !liveSurface_.empty() &&
      std::none_of(lines.begin(), lines.end(), [&](const ChatLine& l) { return l.surfaceId == liveSurface_; }))
    lines.push_back({ChatLine::Role::Surface, "", "", liveSurface_});
  // The live screen is drawn in full only while it's the latest exchange: once the user has said
  // something after it, it's history (a chip), so their message isn't hidden under a tall card.
  std::string live = liveSurface_;
  if (inlineLayout && !live.empty() && live != reopened_) {
    auto at = std::find_if(lines.begin(), lines.end(), [&](const ChatLine& l) { return l.surfaceId == live; });
    if (std::any_of(at, lines.end(), [](const ChatLine& l) { return l.role == ChatLine::Role::User; })) live.clear();
  }
  struct Item { const ChatLine* line; std::string text; Color color; };
  std::vector<Item> items;
  for (const auto& l : lines) items.push_back({&l, stripMarkdown(l.text), kText});
  ChatLine statusLine{ChatLine::Role::Notice, "", "", ""};
  if (!error_.empty()) items.push_back({&statusLine, error_, kWarn});
  else if (!status_.empty()) items.push_back({&statusLine, status_, kMuted});
  ChatLine followLine{ChatLine::Role::Notice, "", "", ""};
  if (!followUps_.empty() && status_.empty()) items.push_back({&followLine, "", kText});
  // Follow-ups: pills left to right, wrapping; `at` lays them out from (x0, y0), returns the height.
  const float pillSize = 24, pillPad = 22, pillGap = 14, pillH = c.lineHeight(pillSize) + 22;
  auto pills = [&](Canvas& cv, float x0, float y0, float width, bool draw) {
    float px = x0, py = y0;
    for (size_t k = 0; k < followUps_.size(); ++k) {
      const std::string& text = followUps_[k];
      // A few px of slack past the measured text so a pill never clips its own last glyph.
      float pw = std::min(cv.measure(text, pillSize) + 2 * pillPad + 6, width);
      if (px > x0 && px + pw > x0 + width) {
        px = x0;
        py += pillH + pillGap;
      }
      if (draw) {
        const std::string key = "followup#" + std::to_string(k);
        Rect r{px, py, pw, pillH};
        bool f = isFocused(key);
        cv.fillRect(r, {255, 255, 255, uint8_t(f ? 46 : 16)}, pillH / 2);
        cv.strokeRect(r, f ? kFocus : kRule, pillH / 2, f ? 3 : 2);
        drawClamped(cv, text, px + pillPad, py + 11, pw - 2 * pillPad, pillSize, kText, 1);
        addFocusable(key, r, [this, text] {
          if (onPrompt) onPrompt(text);
        });
      }
      px += pw + pillGap;
    }
    return py + pillH - y0;
  };

  const float size = 27, padX = 22, padY = 14, gap = 18, avatar = 44;
  const float maxText = std::min(w - avatar - 2 * padX - 80, 720.0f);
  MeasureCanvas m(c);

  // Pass 1: every item's height (the live screen's by laying it out on a measuring canvas; the
  // focusables and button actions that pass makes are thrown away).
  const float chipH = c.lineHeight(24) + 16;
  const float cardPad = 28, cardW = w - avatar, cardInner = cardW - 2 * cardPad;
  std::vector<float> heights(items.size());
  for (size_t i = 0; i < items.size(); ++i) {
    const Item& it = items[i];
    if (it.line->role == ChatLine::Role::Surface) {
      const Surface* sf = store_.surface(it.line->surfaceId);
      const json* root = sf ? sf->component("root") : nullptr;
      if (root && it.line->surfaceId == live) {
        auto savedFocus = focusables_;
        auto savedButtons = buttonActions_;
        Ctx measure{m, *sf, nullptr, ""};
        heights[i] = drawScreen(measure, *root, 0, 0, cardInner, 34) + 2 * cardPad;
        focusables_ = std::move(savedFocus);
        buttonActions_ = std::move(savedButtons);
      } else {
        heights[i] = chipH;
      }
      continue;
    }
    if (it.line == &followLine) {
      heights[i] = pills(m, 0, 0, w - avatar, false);
      continue;
    }
    const bool bubble = it.line->role != ChatLine::Role::Notice;
    float h = drawWrapped(m, it.text, 0, 0, maxText, size, kText);
    heights[i] = h + (bubble ? 2 * padY : 0) + (it.line == &statusLine && progress_ >= 0 ? 22 : 0);
  }

  // Scrolling is in pixels: the conversation moves down by scrollPx_. While scrolled back, a line
  // at the bottom says newer messages are below, and the conversation is clipped above it.
  // stops_[k] puts item k (counted from the newest) just above that line: whole-item scrolling.
  const float more = c.lineHeight(22) + gap;
  float total = 0;
  stops_.assign(1, 0);
  for (size_t k = 0; k < items.size(); ++k) {
    total += heights[items.size() - 1 - k] + (k ? gap : 0);
    if (k + 1 < items.size()) stops_.push_back(std::max(1.0f, total + gap - more));
  }
  maxScroll_ = inlineLayout ? std::max(0.0f, total - (bottom - top)) : 0;
  // A reopened screen sits at the bottom with newer messages hidden below it, even when the
  // conversation would fit without scrolling.
  for (size_t i = 0; inlineLayout && !reopened_.empty() && i < items.size(); ++i)
    if (items[i].line->surfaceId == reopened_) {
      const float stop = stops_[items.size() - 1 - i];
      maxScroll_ = std::max(maxScroll_, stop);
      if (scrollTo_ == reopened_) scrollPx_ = stop;
    }
  scrollTo_.clear();
  scrollPx_ = std::clamp(scrollPx_, 0.0f, maxScroll_);
  float clipBottom = bottom;
  if (scrollPx_ > 0.5f) {
    const std::string text = "Newer messages below. Circle returns to the latest.";
    float lh = c.lineHeight(22);
    c.fillTriangle(x + avatar + 4, bottom - lh + 6, x + avatar + 20, bottom - lh + 6, x + avatar + 12, bottom - 6, kMuted);
    c.text(x + avatar + 32, bottom - lh, text, 22, kMuted);
    clipBottom = bottom - more;
  }
  clipTop_ = top;
  clipBottom_ = clipBottom;
  cardMaxScroll_ = 0;  // set below only if the live card overflows its band (else scroll the convo)
  cardFocusBegin_ = cardFocusEnd_ = 0;
  // Rings and shadows reach a few pixels outside an item; the band covers the full width.
  const Rect band{x - 8, top - 6, w + 16, clipBottom - top + 6};
  c.clip(&band);

  // Pass 2: newest at the bottom, upwards. Items wholly outside the band aren't drawn and can't
  // take focus; those cut by its edges are drawn clipped.
  float y = bottom + scrollPx_;
  for (size_t i = items.size(); i-- > 0;) {
    const Item& it = items[i];
    const auto role = it.line->role;
    const float h = heights[i];
    float itemY = y - h;
    y = itemY - gap;
    if (itemY >= clipBottom) continue;  // below: a newer message, scrolled away
    if (itemY + h <= top) break;        // above: this and everything older

    if (role == ChatLine::Role::Surface) {
      const Surface* sf = store_.surface(it.line->surfaceId);
      const json* root = sf ? sf->component("root") : nullptr;
      if (root && it.line->surfaceId == live) {
        // The live screen, in full: a card as the assistant's reply. Taller than the band, at the
        // latest: pinned to the top as a fixed panel whose content scrolls inside it (cardScroll_),
        // so lower rows (e.g. a second shelf) are reachable by focus or the stick.
        const bool pinned = scrollPx_ <= 0.5f && itemY < top;
        Rect cut = band;
        float contentOffset = 0;
        float bgH = h;
        if (pinned) {
          itemY = top;
          const float visibleH = clipBottom - top;
          bgH = std::min(h, visibleH);
          cardMaxScroll_ = std::max(0.0f, h - visibleH);
          cardScroll_ = std::clamp(cardScroll_, 0.0f, cardMaxScroll_);
          contentOffset = cardScroll_;
          cardTop_ = top;
          cardBottom_ = top + bgH;
          cut = {band.x, top - 6, band.w, bgH + 12};  // content scrolls within the visible band
          c.clip(&cut);
        }
        drawBadge(c, x, itemY + 6, 32);
        c.fillRect({x + avatar, itemY, cardW, bgH}, {255, 255, 255, 14}, 22);
        c.strokeRect({x + avatar, itemY, cardW, bgH}, kRule, 22, 2);
        Ctx ctx{c, *sf, nullptr, "", pinned ? &cut : &band};
        const size_t firstItem = focusables_.size();
        cardFocusBegin_ = firstItem;
        drawScreen(ctx, *root, x + avatar + cardPad, itemY + cardPad - contentOffset, cardInner, 34);
        cardFocusEnd_ = focusables_.size();
        if (focusCard_ && focusables_.size() > firstItem) {
          focus_ = focusables_[firstItem].key;
          focusCard_ = false;
        }
        if (pinned) {
          // Chevrons where there's more content above / below the fold.
          const float chevX = x + avatar + cardW - 36;
          if (cardScroll_ > 1.0f)
            c.fillTriangle(chevX - 9, top + 22, chevX + 9, top + 22, chevX, top + 11, kMuted);
          if (cardScroll_ < cardMaxScroll_ - 1.0f)
            c.fillTriangle(chevX - 9, clipBottom - 22, chevX + 9, clipBottom - 22, chevX, clipBottom - 11, kMuted);
          c.clip(&band);
        }
        continue;
      }
      // An earlier screen: collapsed to its title.
      std::string title = it.text.empty() ? "Screen" : it.text;
      float tw = c.measure(title, 24);
      Rect chip{x + avatar, itemY, tw + 64, h};
      const std::string id = it.line->surfaceId, key = "chip:" + id;
      const bool reopenable = root != nullptr;  // still in the store
      const bool focused = reopenable && isFocused(key);
      c.fillRect(chip, {255, 255, 255, uint8_t(focused ? 40 : 12)}, h / 2);
      if (focused) c.strokeRect(chip, kFocus, h / 2, 3);
      c.strokeRect({x + avatar + 18, itemY + h / 2 - 7, 14, 14}, focused ? kText : kMuted, 3, 2);
      c.text(x + avatar + 44, itemY + 8, title, 24, focused ? kText : kMuted);
      if (reopenable) addFocusable(key, chip, [this, id] {
        reopened_ = scrollTo_ = id;
        focusCard_ = true;
      });
      continue;
    }

    if (it.line == &followLine) {
      pills(c, x + avatar, itemY, w - avatar, true);
      continue;
    }
    float textW = std::min(maxText, c.measure(it.text, size));
    if (role == ChatLine::Role::User) {
      Rect r{x + w - textW - 2 * padX, itemY, textW + 2 * padX, h};
      c.fillRect(r, {255, 255, 255, 44}, 20);
      drawWrapped(c, it.text, r.x + padX, itemY + padY, maxText, size, kText);
    } else if (role == ChatLine::Role::Assistant) {
      drawBadge(c, x, itemY + 6, 32);
      Rect r{x + avatar, itemY, textW + 2 * padX, h};
      c.fillRect(r, {255, 255, 255, 20}, 20);
      drawWrapped(c, it.text, r.x + padX, itemY + padY, maxText, size, kText);
    } else {  // notice or status: a dot and a muted line
      Color dot = it.line == &statusLine ? (error_.empty() ? kAccent : kWarn) : noticeColor(it.line->icon);
      c.fillCircle(x + avatar + 10, itemY + c.lineHeight(size) / 2, 7, dot);
      float th = drawWrapped(c, it.text, x + avatar + 32, itemY, maxText, size, it.line == &statusLine ? it.color : kMuted);
      if (it.line == &statusLine && progress_ >= 0 && error_.empty()) {
        Rect bar{x + avatar + 32, itemY + th + 10, std::min(maxText, 520.0f), 8};
        c.fillRect(bar, {255, 255, 255, 40}, 4);
        c.fillRect({bar.x, bar.y, bar.w * progress_, bar.h}, kAccent, 4);
      }
    }
  }
  c.clip(nullptr);
}

void View::scroll(int items) {
  // Whole items: to the next stop older (or newer) than where the conversation is now.
  if (stops_.empty()) return;
  float target = scrollPx_;
  for (int n = 0; n < std::abs(items); ++n) {
    if (items > 0) {
      auto next = std::upper_bound(stops_.begin(), stops_.end(), target + 1);
      if (next == stops_.end() || target >= maxScroll_) break;  // the oldest message is on screen
      target = std::min(*next, maxScroll_);
    } else {
      auto prev = std::lower_bound(stops_.begin(), stops_.end(), target - 1);
      target = prev == stops_.begin() ? 0 : *(prev - 1);
    }
  }
  scrollPx_ = std::max(0.0f, target);
}

void View::drawBar(Canvas& c, bool focusable) {
  // Mic, then the message field with an underline. The mic lights up and pulses while voice is
  // live: red (kWarn) while listening to the DualSense mic, blue (kAccent) while transcribing.
  const float r = 30;
  float mx = kChatX + r;
  const bool listening = voice_ == Voice::Listening;
  Color micColor = listening ? kWarn : voice_ == Voice::Transcribing ? kAccent : kMuted;
  if (voiceActive()) {
    // A soft halo behind the mic, pulsing faster while listening than while transcribing.
    const float pulse = 0.5f + 0.5f * std::sin(float(animClock()) * (listening ? 6.0f : 3.5f));
    Color halo = listening ? kWarn : kAccent;
    halo.a = static_cast<uint8_t>(40 + 70 * pulse);
    c.fillCircle(mx, kBarCY, r + 6 + 8 * pulse, halo);
  }
  c.strokeCircle(mx, kBarCY, r, voiceActive() ? Color{micColor.r, micColor.g, micColor.b, 220} : Color{255, 255, 255, 110}, 2);
  c.fillRect({mx - 6, kBarCY - 14, 12, 20}, micColor, 6);
  c.fillRect({mx - 1, kBarCY + 8, 2, 8}, micColor);
  c.fillRect({mx - 7, kBarCY + 15, 14, 2}, micColor);

  Rect field{kBarX, kBarCY - 34, kBarW, 68};
  bool f = focusable && isFocused(kComposerKey);
  if (f) {
    c.fillRect(field, {255, 255, 255, 22}, 10);
    keyboardOpen_ = true;
  }
  c.fillRect({field.x, field.y + field.h - 2, field.w, f ? 3.0f : 2.0f}, f ? kFocus : Color{255, 255, 255, 80});
  float ty = kBarCY - c.lineHeight(28) / 2;
  if (draft_.empty() && voiceActive()) {
    // Live voice feedback in the field: "Listening…" while the mic is open, then a three-dot
    // loader (each dot breathing in turn) while whisper transcribes, until the turn is sent.
    if (voice_ == Voice::Listening) {
      c.text(field.x + 24, ty, "Listening…", 28, kWarn);
    } else {
      for (int i = 0; i < 3; ++i) {
        const float ph = 0.5f + 0.5f * std::sin(float(animClock()) * 4.0f - float(i) * 1.1f);
        Color dot = kAccent;
        dot.a = static_cast<uint8_t>(70 + 185 * ph);
        c.fillCircle(field.x + 30 + i * 22, kBarCY, 6, dot);
      }
    }
  } else if (draft_.empty()) {
    // On the console (a system keyboard, so onKeyboard is set) Cross opens it; the host with a
    // real keyboard types inline and presses Enter.
    const char* hint = !f ? "Ask the assistant…" : onKeyboard ? "Press Cross to open the keyboard" : "Type your request, then press Enter";
    c.text(field.x + 24, ty, hint, 28, kMuted);
    if (f) c.fillRect({field.x + 14, kBarCY - 18, 3, 36}, kText);
  } else {
    // Show the end of a long draft.
    std::string shown = draft_;
    while (c.measure(shown, 28) > field.w - 40 && shown.size() > 1) {
      size_t cut = 1;
      while (cut < shown.size() && (static_cast<unsigned char>(shown[cut]) & 0xC0) == 0x80) ++cut;
      shown.erase(0, cut);
    }
    float tw = c.text(field.x + 16, ty, shown, 28, kText);
    if (f) c.fillRect({field.x + 18 + tw, kBarCY - 18, 3, 36}, kText);
  }
  if (focusable) addFocusable(kComposerKey, field, [this] {
    if (!draft_.empty() && onPrompt && onPrompt(draft_)) draft_.clear();
  });
}

void View::settleFocus(Canvas& c, const std::string& focusBefore) {
  // Keep focus on something that exists.
  bool found = std::any_of(focusables_.begin(), focusables_.end(), [&](const Focusable& f) { return f.key == focus_; });
  if (!found && !focusables_.empty()) focus_ = focusables_.front().key;
  // Focus is only known once laid out: if it was just assigned, draw again so it shows.
  if (focus_ != focusBefore && !redrawing_) {
    redrawing_ = true;
    draw(c);
    redrawing_ = false;
  }
}

std::vector<std::string> View::focusables() const {
  std::vector<std::string> keys;
  for (const auto& f : focusables_) keys.push_back(f.key);
  return keys;
}

void View::addFocusable(const std::string& key, Rect r, std::function<void()> activate) {
  focusables_.push_back({key, r, std::move(activate)});
}

// ---------- components ----------

float View::drawChildren(Ctx& ctx, const json& children, float x, float y, float w, float gap, const std::string& parentKey) {
  float total = 0;
  if (children.is_array()) {
    for (const auto& child : children) {
      if (!child.is_string()) continue;
      total += drawComponent(ctx, child.get<std::string>(), x, y + total, w) + gap;
    }
  } else if (children.is_object() && children.contains("componentId")) {
    // Template: one copy of the component per item of the bound list.
    json list = resolve(json{{"@path", children.value("path", "")}}, ctx.s.data, ctx.scope);
    if (list.is_array()) {
      for (size_t i = 0; i < list.size(); ++i) {
        Ctx item{ctx.c, ctx.s, &list[i], ctx.keySuffix + "@" + parentKey + "#" + std::to_string(i)};
        total += drawComponent(item, children["componentId"].get<std::string>(), x, y + total, w) + gap;
      }
    }
  }
  return total > 0 ? total - gap : 0;
}

float View::drawWrapped(Canvas& c, const std::string& text, float x, float y, float w, float size, Color color) {
  float lh = c.lineHeight(size), cy = y, cx = 0;
  float space = c.measure(" ", size);
  std::string line;
  auto flush = [&] {
    c.text(x, cy, line, size, color);
    line.clear();
    cx = 0;
    cy += lh;
  };
  for (const auto& word : splitWords(text)) {
    if (word == "\n") { flush(); continue; }
    float ww = c.measure(word, size);
    if (cx > 0 && cx + space + ww > w) flush();
    if (cx > 0) { line += " "; cx += space; }
    line += word;
    cx += ww;
  }
  if (!line.empty()) flush();
  return cy - y;
}

float View::drawClamped(Canvas& c, const std::string& text, float x, float y, float w, float size, Color color, int maxLines) {
  // drawWrapped's line breaking, stopping at maxLines; the last line ends in "…" if text was left out.
  float lh = c.lineHeight(size), cy = y, cx = 0;
  float space = c.measure(" ", size);
  std::vector<std::string> lines(1);
  bool cut = false;
  for (const auto& word : splitWords(text)) {
    float ww = word == "\n" ? 0 : c.measure(word, size);
    if (word == "\n" || (cx > 0 && cx + space + ww > w)) {
      if (static_cast<int>(lines.size()) == maxLines) {
        cut = true;
        break;
      }
      lines.emplace_back();
      cx = 0;
      if (word == "\n") continue;
    }
    if (cx > 0) {
      lines.back() += " ";
      cx += space;
    }
    lines.back() += word;
    cx += ww;
  }
  std::string& last = lines.back();
  if (cut || c.measure(last, size) > w) {
    // Drop whole characters (UTF-8) until the line and the ellipsis fit.
    while (!last.empty() && c.measure(last + "…", size) > w) {
      size_t end = last.size() - 1;
      while (end > 0 && (static_cast<unsigned char>(last[end]) & 0xC0) == 0x80) --end;
      last.erase(end);
    }
    while (!last.empty() && last.back() == ' ') last.pop_back();
    last += "…";
  }
  for (const auto& line : lines) {
    c.text(x, cy, line, size, color);
    cy += lh;
  }
  return cy - y;
}

float View::tileHeight(Canvas& c) const { return kTileSize + 14 + 2 * c.lineHeight(24) + 2 + c.lineHeight(20); }

float View::drawComponent(Ctx& ctx, const std::string& id, float x, float y, float w) {
  Canvas& c = ctx.c;
  const json* comp = ctx.s.component(id);
  if (!comp) return 0;
  const json& p = *comp;
  const json& data = ctx.s.data;
  std::string type = p.value("component", "");
  std::string key = id + ctx.keySuffix;
  auto str = [&](const char* field) { return p.contains(field) ? resolveString(p[field], data, ctx.scope) : std::string(); };

  if (type == "Text") {
    std::string variant = p.value("variant", "body");
    float size = variant == "heading" ? 40 : variant == "caption" ? 24 : 30;
    return drawWrapped(c, stripMarkdown(str("text")), x, y, w, size, variant == "caption" ? kMuted : kText);
  }

  if (type == "Card") {
    const float pad = 32;
    std::string title = str("title");
    float titleH = title.empty() ? 0 : c.lineHeight(32) + 12;
    float inner = 0;
    if (p.contains("children")) {
      // First pass measures (and its focusables are discarded), second pass draws.
      MeasureCanvas m(c);
      Ctx measure{m, ctx.s, ctx.scope, ctx.keySuffix};
      size_t nf = focusables_.size(), nb = buttonActions_.size();
      inner = drawChildren(measure, p["children"], x + pad, y + pad + titleH, w - 2 * pad, 16);
      focusables_.resize(nf);
      buttonActions_.resize(nb);
    }
    c.fillRect({x, y, w, inner + 2 * pad + titleH}, kPanel, 20);
    if (!title.empty()) c.text(x + pad, y + pad, title, 32, kText);
    if (p.contains("children")) drawChildren(ctx, p["children"], x + pad, y + pad + titleH, w - 2 * pad, 16);
    return inner + 2 * pad + titleH;
  }

  if (type == "Menu") {
    float cy = y;
    std::string title = str("title");
    if (!title.empty()) {
      c.text(x, cy, title, 30, kMuted);
      cy += c.lineHeight(30) + 12;
    }
    if (p.contains("children")) cy += drawChildren(ctx, p["children"], x, cy, std::min(w, 1100.0f), 8, key);
    return cy - y;
  }

  if (type == "MenuItem") {
    std::string label = str("label"), detail = str("detail");
    float h = detail.empty() ? 76 : 96;
    json scope = ctx.scope ? *ctx.scope : json();
    const Surface& s = ctx.s;
    json action = p.value("action", json());
    std::string haptic = p.value("haptic", "");
    drawRow(c, key, {x, y, w, h}, label, detail, [this, &s, id, action, scope, label, haptic] { feel(haptic); fire(s, id, action, scope.is_null() ? nullptr : &scope, label); });
    return h;
  }

  if (type == "Shelf") {
    float cy = y;
    std::string title = str("title");
    if (!title.empty()) {
      c.text(x, cy, title, 30, kMuted);
      cy += c.lineHeight(30) + 16;
    }
    // Tiles in a row, scrolled so the focused one is visible.
    std::vector<std::pair<std::string, const json*>> tiles;  // (component id, scope)
    std::vector<json> scopes;
    const json& children = p.contains("children") ? p["children"] : json::array();
    if (children.is_array()) {
      for (const auto& ch : children) tiles.push_back({ch.get<std::string>(), nullptr});
    } else if (children.is_object()) {
      json list = resolve(json{{"@path", children.value("path", "")}}, data, ctx.scope);
      if (list.is_array()) {
        scopes.assign(list.begin(), list.end());
        for (size_t i = 0; i < scopes.size(); ++i) tiles.push_back({children["componentId"].get<std::string>(), nullptr});
      }
    }
    const float tile = kTileSize, gap = 28;
    int focusedIndex = -1;
    for (size_t i = 0; i < tiles.size(); ++i) {
      std::string k = tiles[i].first + ctx.keySuffix + (scopes.empty() ? "" : "@" + key + "#" + std::to_string(i));
      if (k == focus_) focusedIndex = static_cast<int>(i);
    }
    float offset = 0;
    if (focusedIndex >= 0) {
      float right = (focusedIndex + 1) * (tile + gap);
      if (right > w) offset = right - w;
    }
    float rowH = tileHeight(c);
    // Clip the row to the shelf's own width (intersected with the card's band) so a tile scrolled
    // past either edge — or a last tile wider than the shelf — is cut at the edge instead of
    // bleeding over the card. The clip is a little larger than the row: the focused tile grows by
    // 8 and its ring is 4 wide, and at either end of the row that must not be cut. Restore the
    // card's clip afterwards.
    const float ring = 14;
    Rect rowClip{x - ring, cy - ring, w + 2 * ring, rowH + 2 * ring};
    if (ctx.clip) {
      const float l = std::max(rowClip.x, ctx.clip->x), r = std::min(rowClip.x + rowClip.w, ctx.clip->x + ctx.clip->w);
      const float t = std::max(rowClip.y, ctx.clip->y), b = std::min(rowClip.y + rowClip.h, ctx.clip->y + ctx.clip->h);
      rowClip = {l, t, std::max(0.0f, r - l), std::max(0.0f, b - t)};
    }
    c.clip(&rowClip);
    for (size_t i = 0; i < tiles.size(); ++i) {
      float tx = x + i * (tile + gap) - offset;
      if (tx > x + w || tx + tile < x) continue;
      Ctx item{ctx.c, ctx.s, scopes.empty() ? ctx.scope : &scopes[i], ctx.keySuffix + (scopes.empty() ? "" : "@" + key + "#" + std::to_string(i)), ctx.clip};
      drawComponent(item, tiles[i].first, tx, cy, tile);
    }
    c.clip(ctx.clip);
    return cy - y + rowH + 12;
  }

  if (type == "Tile") {
    // Like the Home screen's: the game's icon (a colored square with its initials without one),
    // the title and a detail under it, two lines and one at most.
    const float size = w;
    bool f = isFocused(key);
    float grow = f ? 8 : 0;
    Rect r{x - grow, y - grow, size + 2 * grow, size + 2 * grow};
    std::string title = str("title");
    std::string image = str("image");
    if (image.empty() || !c.image(r, image, 18)) {
      c.fillRect(r, tileColor(title), 18);
      std::string initials;
      for (const auto& word : splitWords(title)) {
        unsigned char ch = static_cast<unsigned char>(word[0]);
        if (initials.size() < 2 && std::isalnum(ch)) initials += static_cast<char>(std::toupper(ch));
      }
      float iw = c.measure(initials, 72);
      c.text(r.x + (r.w - iw) / 2, r.y + (r.h - c.lineHeight(72)) / 2, initials, 72, {255, 255, 255, 220});
    }
    std::string badge = str("badge");
    if (!badge.empty()) {
      float bw = c.measure(badge, 18) + 20;
      c.fillRect({r.x + 12, r.y + 12, bw, 30}, {255, 255, 255, 230}, 15);
      c.text(r.x + 22, r.y + 12 + (30 - c.lineHeight(18)) / 2, badge, 18, {20, 24, 40});
    }
    if (f) c.strokeRect(r, kFocus, 18, 4);
    float ty = y + size + 14;
    ty += drawClamped(c, title, x, ty, size, 24, f ? kText : Color{220, 224, 235}, 2);
    std::string subtitle = str("subtitle");
    if (!subtitle.empty()) drawClamped(c, subtitle, x, ty + 2, size, 20, kMuted, 1);
    json scope = ctx.scope ? *ctx.scope : json();
    const Surface& s = ctx.s;
    json action = p.value("action", json());
    std::string haptic = p.value("haptic", "");
    addFocusable(key, r, [this, &s, id, action, scope, title, haptic] { feel(haptic); fire(s, id, action, scope.is_null() ? nullptr : &scope, title); });
    return tileHeight(c);
  }

  if (type == "Progress") {
    float cy = y;
    c.text(x, cy, str("label"), 30, kText);
    cy += c.lineHeight(30) + 14;
    Rect bar{x, cy, std::min(w, 1100.0f), 14};
    c.fillRect(bar, {255, 255, 255, 40}, 7);
    json v = p.contains("value") ? resolve(p["value"], data, ctx.scope) : json();
    if (v.is_number()) c.fillRect({bar.x, bar.y, bar.w * std::clamp(v.get<float>(), 0.0f, 1.0f), bar.h}, kAccent, 7);
    else c.fillRect({bar.x + bar.w * 0.35f, bar.y, bar.w * 0.3f, bar.h}, kAccent, 7);  // indeterminate
    cy += 14 + 12;
    std::string detail = str("detail");
    if (!detail.empty()) {
      c.text(x, cy, detail, 24, kMuted);
      cy += c.lineHeight(24);
    }
    return cy - y;
  }

  if (type == "StatusChip") {
    std::string label = str("label");
    float cw = c.measure(label, 26) + 72;
    Rect r{x, y, cw, 52};
    c.fillRect(r, kPanel, 26);
    c.fillCircle(x + 30, y + 26, 8, stateColor(p.value("state", "")));
    c.text(x + 50, y + (52 - c.lineHeight(26)) / 2, label, 26, kText);
    return 52;
  }

  if (type == "Stats") {
    // A small dashboard: labelled value tiles in a 1-3 column grid, each a big value under a
    // muted caption, with an optional mini bar (fraction 0-1) and an optional detail line.
    const json& items = p.contains("items") && p["items"].is_array() ? p["items"] : json::array();
    if (items.empty()) return 0;
    const float gap = 20, minTileW = 300, padX = 24, padT = 20, padB = 22;
    int cols = std::max(1, std::min(3, int((w + gap) / (minTileW + gap))));
    cols = std::min(cols, int(items.size()));
    const float tileW = (w - (cols - 1) * gap) / cols;
    auto has = [](const json& it, const char* f) { return it.contains(f) && !it[f].is_null(); };
    bool anyDetail = false, anyBar = false;
    for (const auto& it : items) { anyDetail |= has(it, "detail"); anyBar |= (has(it, "fraction") && it["fraction"].is_number()); }
    float tileH = padT + c.lineHeight(22) + 4 + c.lineHeight(44) + padB;
    if (anyBar) tileH += 8 + 12;
    if (anyDetail) tileH += 6 + c.lineHeight(20);
    float cy = y;
    for (size_t i = 0; i < items.size(); ++i) {
      const int col = int(i) % cols;
      if (i > 0 && col == 0) cy += tileH + gap;
      const float tx = x + col * (tileW + gap);
      c.fillRect({tx, cy, tileW, tileH}, kPanel, 16);
      float ty = cy + padT;
      drawClamped(c, items[i].value("label", std::string()), tx + padX, ty, tileW - 2 * padX, 22, kMuted, 1);
      ty += c.lineHeight(22) + 4;
      drawClamped(c, items[i].value("value", std::string()), tx + padX, ty, tileW - 2 * padX, 44, kText, 1);
      ty += c.lineHeight(44);
      if (anyBar) {
        ty += 8;
        Rect bar{tx + padX, ty, tileW - 2 * padX, 12};
        c.fillRect(bar, {255, 255, 255, 40}, 6);
        if (has(items[i], "fraction") && items[i]["fraction"].is_number())
          c.fillRect({bar.x, bar.y, bar.w * std::clamp(items[i]["fraction"].get<float>(), 0.0f, 1.0f), bar.h}, kAccent, 6);
        ty += 12;
      }
      if (anyDetail) {
        ty += 6;
        if (has(items[i], "detail")) drawClamped(c, items[i].value("detail", std::string()), tx + padX, ty, tileW - 2 * padX, 20, kMuted, 1);
      }
    }
    return cy + tileH - y;
  }

  if (type == "Pages") {
    const json& pages = p.contains("pages") ? p["pages"] : json::array();
    if (!pages.is_array() || pages.empty()) return 0;
    int& index = pageIndex_[key];
    index = std::clamp(index, 0, static_cast<int>(pages.size()) - 1);
    float cx = x + 56;
    drawButtonGlyph(c, "l1", x + 20, y + 24, 20);
    for (size_t i = 0; i < pages.size(); ++i) {
      std::string title = resolveString(pages[i].value("title", json("")), data, ctx.scope);
      bool selected = static_cast<int>(i) == index;
      c.text(cx, y, title, 32, selected ? kText : kMuted);
      float tw = c.measure(title, 32);
      if (selected) c.fillRect({cx, y + c.lineHeight(32) + 6, tw, 4}, kText, 2);
      cx += tw + 48;
    }
    drawButtonGlyph(c, "r1", cx + 4, y + 24, 20);
    // L1/R1 switch pages unless a ButtonHints entry claims them.
    int count = static_cast<int>(pages.size());
    buttonActions_.push_back({"l1", [&index] { index = std::max(0, index - 1); }});
    buttonActions_.push_back({"r1", [&index, count] { index = std::min(count - 1, index + 1); }});
    float cy = y + c.lineHeight(32) + 36;
    std::string child = pages[index].value("child", "");
    return cy - y + drawComponent(ctx, child, x, cy, w);
  }

  if (type == "Image") {
    Rect r{x, y, std::min(w, 640.0f), std::min(w, 640.0f) * 9 / 16};
    c.fillRect(r, kPanel, 16);
    c.strokeRect(r, kBorder, 16, 2);
    c.text(r.x + 24, r.y + 20, "Image", 26, kMuted);
    drawWrapped(c, str("url"), r.x + 24, r.y + 60, r.w - 48, 20, kMuted);
    return r.h;
  }

  if (type == "Notice") {
    // Toast, top right, whatever position it was given.
    std::string text = str("text");
    std::string icon = p.value("icon", "info");
    Color ic = icon == "success" ? Color{61, 220, 132} : icon == "warning" ? Color{255, 176, 32} : icon == "error" ? kWarn : kAccent;
    float tw = 620;
    MeasureCanvas m(c);
    float textH = drawWrapped(m, text, 0, 0, tw - 108, 26, kText);
    float th = std::max(96.0f, textH + 48);
    Rect r{kWidth - kMargin - tw, 40, tw, th};
    c.fillRect(r, {30, 40, 70, 235}, 18);
    c.strokeRect(r, kBorder, 18, 2);
    c.fillCircle(r.x + 48, r.y + th / 2, 14, ic);
    drawWrapped(c, text, r.x + 84, r.y + (th - textH) / 2, tw - 108, 26, kText);
    return 0;
  }

  if (type == "ButtonHints") {
    drawHints(c, ctx.s, p);
    return 0;
  }

  if (type == "Screen") {  // nested screen: treat as a section
    float cy = y;
    c.text(x, cy, str("title"), 36, kText);
    cy += c.lineHeight(36) + 16;
    if (p.contains("children")) cy += drawChildren(ctx, p["children"], x, cy, w, 24);
    return cy - y;
  }

  // Unknown component: show it rather than hide it.
  c.text(x, y, "[" + type + "]", 24, kWarn);
  return c.lineHeight(24);
}

// ---------- dialogs ----------

void View::drawOverlay(Canvas& c, const Surface& s, const json& p) {
  c.fillRect({0, 0, kWidth, kHeight}, {0, 0, 0, 150});
  std::string type = p.value("component", "");
  std::string id = p.value("id", "");
  const float w = 980, x = (kWidth - w) / 2;
  float y = 300;

  if (type == "Confirm" && onConfirm) {
    // The platform shows it (system message dialog); ask once per surface.
    if (nativeConfirmSurface_ != s.id) {
      nativeConfirmSurface_ = s.id;
      nativeConfirmComponent_ = id;
      nativeConfirmOpen_ = true;
      onConfirm({resolveString(p.value("message", json("")), s.data, nullptr), p.value("confirmLabel", "OK"), p.value("cancelLabel", "Cancel"),
                 p.value("destructive", false)});
    }
    return;
  }

  if (type == "Confirm") {
    std::string message = resolveString(p.value("message", json("")), s.data, nullptr);
    MeasureCanvas m(c);
    float th = std::max(drawWrapped(m, message, x + 56, y + 56, w - 112, 34, kText), c.lineHeight(34));
    float by = y + 56 + th + 48;
    Rect panel{x, y, w, by - y + 72 + 56};
    c.fillRect(panel, {26, 34, 62, 250}, 24);
    c.strokeRect(panel, kBorder, 24, 2);
    drawWrapped(c, message, x + 56, y + 56, w - 112, 34, kText);
    bool destructive = p.value("destructive", false);
    std::string cancelLabel = p.value("cancelLabel", "Cancel"), okLabel = p.value("confirmLabel", "OK");
    Rect cancel{x + 56, by, (w - 112 - 24) / 2, 72};
    Rect ok{cancel.x + cancel.w + 24, by, cancel.w, 72};
    for (auto [r, label, k, accent] : {std::tuple{cancel, cancelLabel, id + "/cancel", false}, std::tuple{ok, okLabel, id + "/confirm", true}}) {
      bool f = isFocused(k);
      Color fill = accent ? (destructive ? kWarn : kAccent) : Color{255, 255, 255, 30};
      if (!f) fill.a = accent ? 150 : 24;
      c.fillRect(r, fill, 14);
      if (f) c.strokeRect(r, kFocus, 14, 4);
      c.text(r.x + (r.w - c.measure(label, 30)) / 2, r.y + (72 - c.lineHeight(30)) / 2, label, 30, kText);
    }
    json okAction = p.value("confirm", json()), cancelAction = p.value("cancel", json());
    std::string okHaptic = p.value("confirmHaptic", ""), cancelHaptic = p.value("cancelHaptic", "");
    // Destructive dialogs start on Cancel; others on OK (PS5 convention).
    auto addCancel = [&] { addFocusable(id + "/cancel", cancel, [this, &s, id, cancelAction, cancelLabel, cancelHaptic] { feel(cancelHaptic); if (cancelAction.is_null()) { std::string sid = s.id; if (onLocal) onLocal("cancel " + sid); dismissDialog(sid); } else fire(s, id, cancelAction, nullptr, cancelLabel); }); };
    auto addOk = [&] { addFocusable(id + "/confirm", ok, [this, &s, id, okAction, okLabel, okHaptic] { feel(okHaptic); fire(s, id, okAction, nullptr, okLabel); }); };
    addCancel();
    addOk();
    if (focus_ != id + "/cancel" && focus_ != id + "/confirm") focus_ = destructive ? id + "/cancel" : id + "/confirm";
    buttonActions_.push_back({"circle", [this, &s, id, cancelAction, cancelLabel, cancelHaptic] { feel(cancelHaptic); if (cancelAction.is_null()) { std::string sid = s.id; if (onLocal) onLocal("cancel " + sid); dismissDialog(sid); } else fire(s, id, cancelAction, nullptr, cancelLabel); }});
    return;
  }

  if (type == "TextPrompt" || type == "VoicePrompt") {
    bool voice = type == "VoicePrompt";
    std::string title = voice ? p.value("label", std::string("Hold R2 and speak")) : p.value("title", std::string(""));
    std::string placeholder = voice ? "(host stand-in: type what you would say)" : p.value("placeholder", std::string(""));
    bool password = !voice && p.value("kind", "text") == "password";
    Rect field = drawKeyboard(c, title, placeholder, password, voice,
                              voice ? "On the PS5 this listens to the DualSense microphone." : "On the PS5 this opens the system keyboard.");
    json submit = p.value("submit", json());
    std::string path = p.contains("value") && p["value"].is_object() ? p["value"].value("@path", "") : "";
    std::string haptic = p.value("submitHaptic", "");
    addFocusable(id + "/field", field, [this, &s, id, submit, path, password, haptic] {
      feel(haptic);
      if (!path.empty()) store_.setData(s.id, path, typing_);
      // The conversation shows a password as a fixed row of dots: not even its length.
      std::string shown = password ? "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2" : typing_;
      fire(s, id, submit, nullptr, shown);
      typing_.clear();
    });
    buttonActions_.push_back({"circle", [this, &s] { typing_.clear(); std::string sid = s.id; if (onLocal) onLocal("cancel " + sid); dismissDialog(sid); }});
    return;
  }
}

Rect View::drawKeyboard(Canvas& c, const std::string& title, const std::string& placeholder, bool password, bool voice, const std::string& note) {
  dialogRequest_ = {title, placeholder, password};
  const float w = 980, x = (kWidth - w) / 2, y = 300;
  {
    Rect panel{x, y, w, 340};
    c.fillRect(panel, {26, 34, 62, 250}, 24);
    c.strokeRect(panel, kBorder, 24, 2);
    if (voice) {
      c.fillCircle(x + 80, y + 76, 26, kAccent);
      c.text(x + 128, y + 56, title, 34, kText);
    } else {
      c.text(x + 56, y + 56, title, 34, kText);
    }
    Rect field{x + 56, y + 130, w - 112, 76};
    c.fillRect(field, {0, 0, 0, 90}, 12);
    c.strokeRect(field, kFocus, 12, 3);
    std::string shown = typing_;
    if (password) {
      shown.clear();
      for (size_t i = 0; i < typing_.size(); ++i) shown += "\xE2\x80\xA2";  // •
    }
    if (shown.empty()) c.text(field.x + 24, field.y + (76 - c.lineHeight(30)) / 2, placeholder, 30, kMuted);
    else {
      float tw = c.text(field.x + 24, field.y + (76 - c.lineHeight(30)) / 2, shown, 30, kText);
      c.fillRect({field.x + 26 + tw, field.y + 18, 3, 40}, kText);
    }
    c.text(x + 56, y + 236, note, 22, kMuted);
    // Hints inside the dialog.
    float hx = x + w - 56;
    for (auto [button, label] : {std::pair{"circle", "Cancel"}, std::pair{"cross", "Done"}}) {
      float lw = c.measure(label, 26);
      hx -= lw;
      c.text(hx, y + 280, label, 26, kText);
      hx -= 40;
      drawButtonGlyph(c, button, hx + 16, y + 280 + c.lineHeight(26) / 2, 15);
      hx -= 32;
    }
    keyboardOpen_ = dialogKeyboard_ = true;
    return field;
  }
}

void View::drawChrome(Canvas& c) {
  // Right-aligned global hints: Options (always, the assistant's "Ask") rightmost, then the voice
  // trigger "R2 Speak" to its left when the mic is usable (console). Mirrored in assistHintWidth so
  // a surface's own hints sit left of both.
  float right = drawHint(c, kWidth - kMargin, "options", "Ask");
  if (voiceAvailable_) drawHint(c, right, "r2", "Speak");
}

namespace {
constexpr float kGlyphR = 15, kGlyphGap = 12, kHintGap = 40;
}

float View::glyphWidth(Canvas& c, const std::string& b) {
  if (b == "cross" || b == "circle" || b == "triangle" || b == "square") return 2 * kGlyphR;
  std::string name = b;
  std::transform(name.begin(), name.end(), name.begin(), ::toupper);
  return c.measure(name, 18) + 16;
}

float View::drawHint(Canvas& c, float right, const std::string& button, const std::string& label) {
  // One hint, right-aligned at `right`: glyph, gap, label. Returns the right edge for the next hint.
  float x = right - c.measure(label, 26);
  c.text(x, kHintsY, label, 26, kText);
  float gw = glyphWidth(c, button);
  x -= kGlyphGap + gw;
  drawButtonGlyph(c, button, x + gw / 2, kHintsY + c.lineHeight(26) / 2, kGlyphR);
  return x - kHintGap;
}

float View::assistHintWidth(Canvas& c) {
  return kWidth - kMargin - [&] {
    MeasureCanvas m(c);
    float right = drawHint(m, kWidth - kMargin, "options", "Ask");
    return voiceAvailable_ ? drawHint(m, right, "r2", "Speak") : right;
  }();
}

void View::drawHints(Canvas& c, const Surface& s, const json& hints) {
  if (!hints.contains("hints") || !hints["hints"].is_array()) return;
  // Right-aligned, last hint rightmost, left of the assistant's Options hint.
  float x = kWidth - kMargin - assistHintWidth(c);
  std::string id = hints.value("id", "");
  for (auto it = hints["hints"].rbegin(); it != hints["hints"].rend(); ++it) {
    const json& h = *it;
    std::string label = h.is_object() ? resolveString(h.value("label", json("")), s.data, nullptr) : "", button = h.value("button", "");
    if (button == "options") continue;  // reserved for the assistant
    x = drawHint(c, x, button, label);
    json action = h.value("action", json());
    std::string haptic = h.value("haptic", "");
    buttonActions_.insert(buttonActions_.begin(), {button, [this, &s, id, action, label, haptic] { feel(haptic); fire(s, id, action, nullptr, label); }});
  }
}

void View::drawButtonGlyph(Canvas& c, const std::string& b, float cx, float cy, float r) {
  // PlayStation face-button glyphs, drawn rather than taken from a font.
  if (b == "cross") {
    c.strokeCircle(cx, cy, r, {255, 255, 255, 120}, 2);
    float k = r * 0.42f;
    thickLine(c, cx - k, cy - k, cx + k, cy + k, 3, kText);
    thickLine(c, cx + k, cy - k, cx - k, cy + k, 3, kText);
  } else if (b == "circle") {
    c.strokeCircle(cx, cy, r, {255, 255, 255, 120}, 2);
    c.strokeCircle(cx, cy, r * 0.5f, kText, 3);
  } else if (b == "triangle") {
    c.strokeCircle(cx, cy, r, {255, 255, 255, 120}, 2);
    float k = r * 0.55f;
    c.fillTriangle(cx, cy - k, cx - k, cy + k * 0.7f, cx + k, cy + k * 0.7f, kText);
    c.fillTriangle(cx, cy - k + 5, cx - k + 5, cy + k * 0.7f - 3, cx + k - 5, cy + k * 0.7f - 3, {20, 30, 60});
  } else if (b == "square") {
    c.strokeCircle(cx, cy, r, {255, 255, 255, 120}, 2);
    float k = r * 0.42f;
    c.strokeRect({cx - k, cy - k, 2 * k, 2 * k}, kText, 1, 3);
  } else {
    // Shoulder/trigger/options: a small pill with the name.
    std::string name = b;
    std::transform(name.begin(), name.end(), name.begin(), ::toupper);
    float w = c.measure(name, 18) + 16;
    c.fillRect({cx - w / 2, cy - 14, w, 28}, {255, 255, 255, 40}, 8);
    c.text(cx - w / 2 + 8, cy - c.lineHeight(18) / 2, name, 18, kText);
  }
}

// ---------- input ----------

void View::feel(const std::string& effect) {
  // A haptic the screen attached to this action. Unknown names are ignored (a newer catalog).
  if (!effect.empty() && onFunction && !hapticEffect(effect).empty()) onFunction(json{{"@call", "haptic"}, {"args", {{"effect", effect}}}});
}

void View::fire(const Surface& s, const std::string& componentId, const json& action, const json* scope, const std::string& label) {
  if (!action.is_object()) return;
  const std::string surfaceId = s.id;  // `s` may be gone after dismissDialog
  if (action.contains("event")) {
    const json& ev = action["event"];
    UserAction a{ev.value("name", ""), s.id, componentId, resolveContext(ev.value("context", json::object()), s.data, scope), label,
                 ev.contains("userMessage") ? resolveString(ev["userMessage"], s.data, scope) : ""};
    if (onAction) onAction(a);
  } else if (action.contains("functionCall")) {
    if (onFunction) onFunction(action["functionCall"]);
  }
  dismissDialog(surfaceId);
}

bool View::confirmPending() const { return nativeConfirmOpen_ && store_.surface(nativeConfirmSurface_) != nullptr; }

void View::answerConfirm(bool confirmed) {
  if (!confirmPending()) return;
  nativeConfirmOpen_ = false;
  const Surface* s = store_.surface(nativeConfirmSurface_);
  const json* p = s->component(nativeConfirmComponent_);
  if (!p) return;
  json action = p->value(confirmed ? "confirm" : "cancel", json());
  feel(p->value(confirmed ? "confirmHaptic" : "cancelHaptic", ""));
  std::string label = p->value(confirmed ? "confirmLabel" : "cancelLabel", confirmed ? "OK" : "Cancel");
  if (action.is_null()) {  // no cancel action: tell the host, like Circle on the drawn dialog
    std::string sid = s->id;
    if (onLocal) onLocal("cancel " + sid);
    dismissDialog(sid);
    return;
  }
  fire(*s, nativeConfirmComponent_, action, nullptr, label);
}

void View::dismissDialog(const std::string& surfaceId) {
  // A dialog (keyboard, confirm) is a one-shot question: once answered or cancelled it goes away
  // now, instead of staying on screen until the agent's next surface. The conversation and its
  // status line show while the agent works; a new dialog comes if it asks again.
  const Surface* s = store_.surface(surfaceId);
  const json* root = s ? s->component("root") : nullptr;
  if (!root || !isOverlay(root->value("component", ""))) return;
  store_.apply(json{{"version", kVersion}, {"deleteSurface", {{"surfaceId", surfaceId}}}});
  // The handlers laid out for it point at the deleted surface: drop them until the next draw.
  focusables_.clear();
  buttonActions_.clear();
}

void View::move(Button d) {
  auto cur = std::find_if(focusables_.begin(), focusables_.end(), [&](const Focusable& f) { return f.key == focus_; });
  if (cur == focusables_.end()) return;
  float cx = cur->rect.x + cur->rect.w / 2, cy = cur->rect.y + cur->rect.h / 2;
  const Focusable* best = nullptr;
  float bestScore = 1e9f;
  for (const auto& f : focusables_) {
    if (f.key == focus_) continue;
    float fx = f.rect.x + f.rect.w / 2, fy = f.rect.y + f.rect.h / 2;
    float primary = 0, secondary = 0;
    switch (d) {
      case Button::Up: primary = cy - fy; secondary = std::fabs(fx - cx); break;
      case Button::Down: primary = fy - cy; secondary = std::fabs(fx - cx); break;
      case Button::Left: primary = cx - fx; secondary = std::fabs(fy - cy); break;
      case Button::Right: primary = fx - cx; secondary = std::fabs(fy - cy); break;
      default: return;
    }
    if (primary <= 1) continue;
    float score = primary + 2 * secondary;
    if (score < bestScore) { bestScore = score; best = &f; }
  }
  if (best) {
    focus_ = best->key;
    const Rect& r = best->rect;
    const size_t bi = static_cast<size_t>(best - &focusables_[0]);
    if (cardMaxScroll_ > 0.5f && bi >= cardFocusBegin_ && bi < cardFocusEnd_) {
      // An item inside the scrollable live card: scroll the card to reveal it, not the conversation.
      if (r.y < cardTop_ + 14) cardScroll_ -= (cardTop_ + 14 - r.y);
      else if (r.y + r.h > cardBottom_ - 14) cardScroll_ += (r.y + r.h - (cardBottom_ - 14));
      cardScroll_ = std::clamp(cardScroll_, 0.0f, cardMaxScroll_);
    } else if (layout_ == Layout::Inline && r.y < clipBottom_ && r.y + r.h > clipTop_) {
      // Focus on something cut by the conversation's edges: bring it into view (whole, if it fits).
      if (r.y < clipTop_) scrollBy(clipTop_ - r.y + 12);
      else if (r.y + r.h > clipBottom_) scrollBy(-std::min(r.y + r.h - clipBottom_ + 12, r.y - clipTop_));
    }
  } else if (d == Button::Up && layout_ == Layout::Inline) {
    scroll(1);  // past the top: older messages
  }
}

void View::press(Button b) {
  // The controls overlay is modal: Square or Circle dismisses it, everything else is swallowed.
  if (helpOpen_) {
    if (b == Button::Square || b == Button::Circle) helpOpen_ = false;
    return;
  }
  switch (b) {
    case Button::Up: case Button::Down: case Button::Left: case Button::Right:
      move(b);
      return;
    case Button::Cross: {
      // With a system keyboard, Cross on a text field opens it; submitText() finishes the job.
      if (onKeyboard && keyboardOpen_) {
        onKeyboard(dialogKeyboard_ ? dialogRequest_ : KeyboardRequest{"Ask the assistant", "e.g. Connect me to Wi-Fi", false});
        return;
      }
      for (auto& f : focusables_) if (f.key == focus_) { auto act = f.activate; act(); return; }
      break;  // nothing focused: a screen-level Cross action (e.g. "ask" on the home screen)
    }
    default: break;
  }
  // The message bar: Circle clears a draft before it means "back".
  if (b == Button::Circle && focus_ == kComposerKey && !draft_.empty()) {
    draft_.clear();
    return;
  }
  // Then it returns from history (an earlier screen, or scrolled back) to the latest.
  if (b == Button::Circle && scrolledBack()) {
    backToLatest();
    focus_.clear();
    return;
  }
  static const std::map<Button, std::string> names = {{Button::Cross, "cross"}, {Button::Circle, "circle"}, {Button::Triangle, "triangle"}, {Button::Square, "square"}, {Button::L1, "l1"},
                                                       {Button::R1, "r1"}, {Button::L2, "l2"}, {Button::R2, "r2"}, {Button::Options, "options"}};
  const std::string& name = names.at(b);
  for (auto& [button, act] : buttonActions_) {
    if (button == name) { auto run = act; run(); return; }
  }
  if (b == Button::Options) {
    // Already on the message bar: open the keyboard, as Cross does, rather than doing nothing.
    if (focus_ == kComposerKey && onKeyboard && keyboardOpen_) onKeyboard({"Ask the assistant", "e.g. Connect me to Wi-Fi", false});
    else focusComposer();
    return;
  }
  if (b == Button::Triangle && onNewConversation && !transcript_.empty()) {
    auto start = onNewConversation;
    followUps_.clear();
    backToLatest();
    focus_ = kComposerKey;
    suggestOffset_ += 6;  // a different "Try asking" window next time it shows
    start();
    return;
  }
  // Square opens the controls overlay when no surface claimed it (like Triangle = new conversation).
  if (b == Button::Square) { helpOpen_ = true; return; }
  if (b == Button::Circle && onLocal) onLocal("back");
}

void View::submitText(const std::string& text) {
  if (!keyboardOpen_) return;
  (dialogKeyboard_ ? typing_ : draft_) = text;
  for (auto& f : focusables_) if (f.key == focus_) { auto act = f.activate; act(); return; }
}

void View::type(const std::string& utf8) {
  if (dialogKeyboard_) typing_ += utf8;
  else if (keyboardOpen_ && focus_ == kComposerKey) draft_ += utf8;
}

void View::erase() {
  std::string* text = dialogKeyboard_ ? &typing_ : keyboardOpen_ && focus_ == kComposerKey ? &draft_ : nullptr;
  if (!text || text->empty()) return;
  // Remove one UTF-8 code point.
  size_t i = text->size() - 1;
  while (i > 0 && (static_cast<unsigned char>((*text)[i]) & 0xC0) == 0x80) --i;
  text->erase(i);
}

}  // namespace a2ui

namespace a2ui {

// ---------- key setup ----------

void View::drawSetup(Canvas& c, const SetupScreen& s) {
  focusables_.clear();
  buttonActions_.clear();
  keyboardOpen_ = dialogKeyboard_ = false;
  c.clear(kBgTop, kBgBottom);
  if (!background_.empty() && c.image({0, 0, kWidth, kHeight}, background_, 0))
    c.fillRect({0, 0, kWidth, kHeight}, {9, 10, 13, 90});
  const float x = kMargin, w = 980;
  float y = 130;
  c.text(x, y, "Set up the assistant", 56, kText);
  y += c.lineHeight(56) + 16;
  y += drawWrapped(c, "The assistant needs a model key. Send it from your phone or computer: there's no typing it here.", x, y, w, 30, kMuted) + 44;

  auto step = [&](const std::string& n, const std::string& text) {
    c.fillCircle(x + 22, y + c.lineHeight(32) / 2, 22, kPanelFocus);
    c.text(x + 22 - c.measure(n, 26) / 2, y + (c.lineHeight(32) - c.lineHeight(26)) / 2, n, 26, kText);
    y += drawWrapped(c, text, x + 70, y, w - 70, 32, kText) + 12;
  };
  if (s.url.empty()) {
    step("1", "Connect the console to your network. It needs to be on the same network as your phone or computer.");
  } else {
    step("1", "Scan the QR code with your phone, or open this address on any device on your network:");
    c.text(x + 70, y, s.url, 40, kAccent);
    y += c.lineHeight(40) + 36;
    step("2", "Enter this code on the page:");
    std::string spaced = s.code.size() == 6 ? s.code.substr(0, 3) + " " + s.code.substr(3) : s.code;
    c.text(x + 70, y, spaced, 96, kText);
    y += c.lineHeight(96) + 28;
    step("3", "Paste a key made just for this console. It's stored on the PS5, where other homebrew can read it, so cap its spending where your provider allows.");
  }
  if (!s.status.empty()) c.text(x, 900, s.status, 30, s.status.find("received") != std::string::npos ? kAccent : kWarn);

  if (!s.url.empty()) {
    // The address as a QR code, with the code in the fragment (it never leaves the phone).
    using qrcodegen::QrCode;
    const QrCode qr = QrCode::encodeText((s.url + "#code=" + s.code).c_str(), QrCode::Ecc::MEDIUM);
    const int n = qr.getSize(), quiet = 4;
    const float side = 560, module = side / float(n + 2 * quiet);
    const float qx = kWidth - kMargin - side, qy = 200;
    c.fillRect({qx, qy, side, side}, {255, 255, 255}, 24);
    for (int r = 0; r < n; ++r)
      for (int col = 0; col < n;) {
        if (!qr.getModule(col, r)) {
          ++col;
          continue;
        }
        int run = col;
        while (run < n && qr.getModule(run, r)) ++run;  // one rectangle per run of dark modules
        c.fillRect({qx + (quiet + col) * module, qy + (quiet + r) * module, (run - col) * module + 0.5f, module + 0.5f}, {0, 0, 0});
        col = run;
      }
    const std::string caption = "Scan with your phone's camera";
    c.text(qx + (side - c.measure(caption, 26)) / 2, qy + side + 24, caption, 26, kMuted);
  }

  float right = kWidth - kMargin;
  if (s.current.empty()) {
    drawHint(c, right, "square", "Offline demo");
  } else {
    right = drawHint(c, right, "circle", "Keep using " + s.current);
    drawHint(c, right, "triangle", "Forget this key");
  }
}

}  // namespace a2ui
