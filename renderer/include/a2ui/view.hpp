// Draws the assistant's console screen, laid out like the PS5's own conversation screens: the
// conversation on the left, the agent's top A2UI surface (PS5 catalog) on the right, and a message
// bar along the bottom that is always there. Wide surfaces (shelves, pages) take the full width.
// Controller input: focus moves with the D-pad, Cross activates, Circle goes back, L1/R1 switch
// pages, Options jumps to the message bar, other buttons fire ButtonHints. System dialogs
// (Confirm, TextPrompt, VoicePrompt) are overlays that take focus; Notice is a toast.
#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "a2ui/model.hpp"

namespace a2ui {

struct Color {
  uint8_t r, g, b, a = 255;
};
struct Rect {
  float x, y, w, h;
};

/** Drawing primitives the view needs. One implementation per platform (SDL on the host). */
class Canvas {
 public:
  virtual ~Canvas() = default;
  virtual void clear(Color top, Color bottom) = 0;  // vertical gradient background
  virtual void fillRect(Rect r, Color c, float radius = 0) = 0;
  /** Per-corner radii: top-left, top-right, bottom-right, bottom-left. */
  virtual void fillRectCorners(Rect r, Color c, float tl, float tr, float br, float bl) { fillRect(r, c, std::max({tl, tr, br, bl})); }
  virtual void strokeRect(Rect r, Color c, float radius, float width) = 0;
  virtual void fillCircle(float cx, float cy, float radius, Color c) = 0;
  virtual void strokeCircle(float cx, float cy, float radius, Color c, float width) = 0;
  virtual void fillTriangle(float x1, float y1, float x2, float y2, float x3, float y3, Color c) = 0;
  /** Draw UTF-8 text with its top-left at (x, y); returns the advance width. */
  virtual float text(float x, float y, const std::string& utf8, float size, Color c) = 0;
  virtual float measure(const std::string& utf8, float size) = 0;
  virtual float lineHeight(float size) = 0;
  /** Draw only inside `r` until the next clip; nullptr draws everywhere again. Optional for a backend. */
  virtual void clip(const Rect* r) { (void)r; }
  /**
   * Draw a local picture (PNG or JPEG; `source` is a file:// URL or an absolute path) stretched over
   * `r` with rounded corners. Returns false if it can't be shown (not local, unreadable, or this
   * backend has no images): the caller draws its own stand-in.
   */
  virtual bool image(Rect r, const std::string& source, float radius) {
    (void)r, (void)source, (void)radius;
    return false;
  }
};

enum class Button { Up, Down, Left, Right, Cross, Circle, Triangle, Square, L1, R1, L2, R2, Options };

class View {
 public:
  static constexpr float kWidth = 1920, kHeight = 1080;  // logical TV resolution

  explicit View(Store& store) : store_(store) {}

  /**
   * Inline (default): the agent's screens are its replies in the conversation; the live one is
   * drawn in full, earlier ones collapse to their title. Panes: the conversation on the left,
   * the live screen on the right. Dialogs and notices are overlays (or native) either way.
   */
  enum class Layout { Inline, Panes };
  void setLayout(Layout layout) { layout_ = layout; }

  void draw(Canvas& canvas);
  void press(Button b);
  /** Text typed while a keyboard (TextPrompt / VoicePrompt stand-in) is open. */
  void type(const std::string& utf8);
  void erase();  // backspace in the open keyboard

  std::function<void(const UserAction&)> onAction;
  std::function<void(const json& functionCall)> onFunction;
  std::function<void(const std::string& event)> onLocal;  // "back", "cancel <surface>", ...
  /**
   * Set on platforms with a system keyboard (the PS5's sceImeDialog): Cross on a text field asks
   * for it instead of submitting, and the host calls submitText() with what was typed.
   */
  struct KeyboardRequest {
    std::string title, placeholder;
    bool password = false;
  };
  std::function<void(const KeyboardRequest&)> onKeyboard;
  /** Fill the focused text field (message bar or keyboard dialog) and submit it. */
  void submitText(const std::string& text);

  /**
   * Native dialogs and notifications. When set, the platform shows these itself (on the PS5: the
   * system message dialog and system notifications) instead of the view drawing them.
   * onConfirm is called once per Confirm surface; the host answers with answerConfirm().
   */
  struct ConfirmRequest {
    std::string message, confirmLabel, cancelLabel;
    bool destructive = false;
  };
  std::function<void(const ConfirmRequest&)> onConfirm;
  /** The user's answer to the native confirm: fires the surface's confirm or cancel action. */
  void answerConfirm(bool confirmed);
  /** A native confirm is still wanted (false once answered, or if the agent removed its surface). */
  bool confirmPending() const;
  /** Called once per Notice surface. */
  std::function<void(const std::string& text, const std::string& icon)> onNotice;

  /** A request sent from the message bar or a suggestion; return false if it couldn't be sent (busy). */
  std::function<bool(const std::string& text)> onPrompt;

  /** Conversation, set by the host from the Session each frame. A new line returns to the latest. */
  void setTranscript(const std::vector<ChatLine>& lines) {
    if (lines.size() != transcript_.size()) backToLatest();
    transcript_ = lines;
  }
  /**
   * Scroll the conversation (inline layout): positive = back to older messages, by whole items
   * (D-pad past the top, Page Up/Down): the next older or newer message lines up at the bottom.
   * Circle returns to the latest. Earlier screens are chips; Cross on one reopens it in full
   * where it was.
   */
  void scroll(int items);
  /**
   * Smooth scrolling by `px` (logical pixels, positive = older), for an analog stick: the host
   * calls it every frame with speed × frame time. Stops at the oldest message and at the latest.
   */
  // Scroll the live card internally when it's taller than the band (e.g. a library with more rows
  // than fit); otherwise scroll the conversation. px>0 = stick up (reveal content above/older).
  void scrollBy(float px) {
    if (cardMaxScroll_ > 0.5f) { cardScroll_ = std::clamp(cardScroll_ - px, 0.0f, cardMaxScroll_); return; }
    scrollPx_ = std::clamp(scrollPx_ + px, 0.0f, maxScroll_);
  }
  /** Scrolled back, or an earlier screen is reopened (Circle returns to the latest). */
  bool scrolledBack() const { return scrollPx_ > 0.5f || cardScroll_ > 0.5f || !reopened_.empty(); }
  void backToLatest() {
    scrollPx_ = 0;
    cardScroll_ = 0;
    reopened_.clear();
    scrollTo_.clear();
  }
  void setStatus(const std::string& text) { status_ = text; }
  /**
   * Voice capture/transcription state, for the composer's live feedback. Listening animates the
   * mic (R2 held); Transcribing shows a "…" in the message field until the transcript is sent. Kept
   * separate from status_, which a connected client overwrites with the agent's run status each frame.
   */
  enum class Voice { Idle, Listening, Transcribing };
  void setVoice(Voice v) { voice_ = v; }
  Voice voice() const { return voice_; }
  bool voiceActive() const { return voice_ != Voice::Idle; }  // the host keeps redrawing while true
  /** Whether the mic is usable (console only): gates the "R2 Speak" hint. */
  void setVoiceAvailable(bool a) { voiceAvailable_ = a; }
  /**
   * A PNG/JPEG to draw behind everything (a console-style backdrop), as a path or file:// URL.
   * Drawn full-screen over the gradient; if it can't be loaded the gradient shows through.
   */
  void setBackground(std::string source) { background_ = std::move(source); }
  /** The agent's suggested next requests (Session::followUps): buttons under its reply that send them. */
  void setFollowUps(const std::vector<std::string>& prompts) { followUps_ = prompts; }
  /**
   * Set by a host that can start over: Triangle (when no screen claims it) starts a new
   * conversation, and the header shows the hint while there is one to leave.
   */
  std::function<void()> onNewConversation;
  /** The header: who the player is talking to (the agent's own name, else the app's). */
  void setAgentName(const std::string& name) { agentName_ = name; }
  void setError(const std::string& text) { error_ = text; }
  /** A running tool's progress (0 to 1, or -1): drawn under the status line unless the platform shows its own. */
  void setProgress(float value) { progress_ = value; }
  /** Move focus to the message bar (what Options does). */
  void focusComposer() { focus_ = kComposerKey; }
  /** Focus is on a text field (message bar or keyboard dialog) in the last draw: keys are text. */
  bool keyboardOpen() const { return keyboardOpen_; }
  static constexpr const char* kComposerKey = "composer/field";

  /**
   * The key setup screen, drawn by the app before there's an agent (not A2UI): how to send a key
   * from a phone or computer (Pairing), the address as a QR code, and the code to enter.
   */
  struct SetupScreen {
    std::string url;      // http://<console>:8791/ ("" = no network)
    std::string code;     // six digits
    std::string status;   // the last pairing event, "" if none
    std::string current;  // the key in use ("OpenRouter"), "" if none: Circle keeps it, Triangle forgets it
  };
  void drawSetup(Canvas& c, const SetupScreen& s);

  /** For tests: the focus keys laid out in the last draw, in visual order. */
  std::vector<std::string> focusables() const;
  std::string focused() const { return focus_; }

 private:
  void settleFocus(Canvas& c, const std::string& focusBefore);
  float assistHintWidth(Canvas& c);
  float glyphWidth(Canvas& c, const std::string& button);
  float drawHint(Canvas& c, float right, const std::string& button, const std::string& label);
  struct Focusable {
    std::string key;  // component id, or id#index inside a template
    Rect rect;
    std::function<void()> activate;
  };
  struct Ctx {
    Canvas& c;
    const Surface& s;
    const json* scope;  // template item
    std::string keySuffix;
    const Rect* clip = nullptr;  // the active scissor (the card's band): a Shelf clips its row to
                                 // this and restores it, so tiles don't bleed past the card edges
  };

  float drawComponent(Ctx& ctx, const std::string& id, float x, float y, float w);
  /** `parentKey` keeps template instances unique when two parents share a template component. */
  float drawChildren(Ctx& ctx, const json& children, float x, float y, float w, float gap, const std::string& parentKey = "");
  float drawWrapped(Canvas& c, const std::string& text, float x, float y, float w, float size, Color color);
  /** drawWrapped, at most `maxLines` lines; what doesn't fit ends in "…". */
  float drawClamped(Canvas& c, const std::string& text, float x, float y, float w, float size, Color color, int maxLines);
  static constexpr float kTileSize = 200;  // a Tile's icon square
  /** A Tile's full height: the square, two title lines and a detail line. */
  float tileHeight(Canvas& c) const;
  void drawOverlay(Canvas& c, const Surface& s, const json& comp);
  /** The keyboard dialog (TextPrompt, VoicePrompt stand-in, composer); returns the field rect. */
  Rect drawKeyboard(Canvas& c, const std::string& title, const std::string& placeholder, bool password, bool voice, const std::string& note);
  /** A Screen (or any root) at x/y/w: title, status chip at the right, children. */
  /** Returns the height used. */
  float drawScreen(Ctx& ctx, const json& root, float x, float y, float w, float titleSize);
  void drawTranscript(Canvas& c, float x, float top, float bottom, float w);
  void drawSuggestions(Canvas& c, float x, float y, float w);
  void drawBar(Canvas& c, bool focusable);
  void drawRow(Canvas& c, const std::string& key, Rect r, const std::string& label, const std::string& detail, std::function<void()> activate);
  void drawChrome(Canvas& c);
  void drawHelp(Canvas& c);  // the controls overlay (Square), listing the buttons the player has
  void drawHints(Canvas& c, const Surface& s, const json& hints);
  void drawButtonGlyph(Canvas& c, const std::string& button, float cx, float cy, float r);
  void addFocusable(const std::string& key, Rect r, std::function<void()> activate);
  bool isFocused(const std::string& key) const { return key == focus_; }
  void dismissDialog(const std::string& surfaceId);
  void fire(const Surface& s, const std::string& componentId, const json& action, const json* scope, const std::string& label = "");
  void feel(const std::string& effect);  // a haptic attached to an action, through onFunction
  void move(Button direction);

  Store& store_;
  std::vector<Focusable> focusables_;
  std::string focus_;
  std::string surfaceShown_;
  std::map<std::string, int> pageIndex_;  // Pages component -> selected page
  std::vector<std::pair<std::string, std::function<void()>>> buttonActions_;  // from ButtonHints
  std::string typing_;           // text in the open keyboard dialog
  std::string draft_;            // text in the message bar
  bool keyboardOpen_ = false;    // a text field has focus
  bool dialogKeyboard_ = false;  // ...and it's a keyboard dialog, not the message bar
  KeyboardRequest dialogRequest_;  // what the open keyboard dialog asks for
  std::string nativeConfirmSurface_, nativeConfirmComponent_;  // the confirm handed to onConfirm
  bool nativeConfirmOpen_ = false;
  std::string lastNotice_;  // the Notice surface last handed to onNotice
  std::vector<ChatLine> transcript_;
  Layout layout_ = Layout::Inline;
  std::string liveSurface_;  // the screen drawn in full in the conversation (inline layout)
  float scrollPx_ = 0;       // how far the conversation is moved down, in pixels (0 = the latest)
  size_t suggestOffset_ = 0; // which rotating window of "Try asking" to show (advances on New conversation)
  float maxScroll_ = 0;      // from the last draw: the oldest message's top at the top edge
  std::vector<float> stops_; // from the last draw: offsets that put each item at the bottom
  float clipTop_ = 0, clipBottom_ = 0;  // from the last draw: the conversation's visible band
  float cardScroll_ = 0;     // internal scroll of the live card when it's taller than the band
  float cardMaxScroll_ = 0;  // from the last draw: how far the live card can scroll (0 = it fits)
  float cardTop_ = 0, cardBottom_ = 0;            // the live card's visible band (for focus-follow)
  size_t cardFocusBegin_ = 0, cardFocusEnd_ = 0;  // the live card's focusables, as indices into focusables_
  std::string reopened_;     // an earlier screen reopened from its chip ("" = the latest)
  std::string scrollTo_;     // scroll so this surface is the bottom item, on the next draw
  bool focusCard_ = false;   // give focus to the live card's first item on the next draw
  std::string status_, error_, agentName_;
  std::vector<std::string> followUps_;
  float progress_ = -1;
  bool redrawing_ = false;
  Voice voice_ = Voice::Idle;
  bool voiceAvailable_ = false;
  bool helpOpen_ = false;     // the controls overlay (Square opens it, Circle/Square closes)
  std::string background_;    // optional full-screen backdrop image (path or file://)
};

}  // namespace a2ui
