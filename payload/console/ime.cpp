#include "ime.hpp"

#include <cstdint>
#include <cstring>
#include <cwchar>

// Layout as in SDL's PS5 driver (SDL_ps5keyboard.c), which works on the console.
namespace {

enum SceImeDialogStatus { kStatusNone, kStatusRunning, kStatusFinished };

typedef int (*SceImeTextFilter)(wchar_t*, uint32_t*, const wchar_t*, uint32_t);

struct SceImeDialogParam {
  int userId;
  int type;  // 0 default, 1 basic latin, 2 URL, 3 mail, 4 number
  uint64_t supportedLanguages;
  int enterLabel;  // 0 default, 1 send, 2 search, 3 go
  int inputMethod;
  SceImeTextFilter filter;
  uint32_t option;
  uint32_t maxTextLength;
  wchar_t* inputTextBuffer;
  float posx;
  float posy;
  int halign;  // 0 left, 1 center, 2 right
  int valign;  // 0 top, 1 center, 2 bottom
  const wchar_t* placeholder;
  const wchar_t* title;
  int8_t reserved[16];
};

// Option bits as in the PS4 SDK (multiline 0x1, no auto-capitalisation 0x2, password 0x4).
// Password is unverified on the PS5: check that the field shows dots.
constexpr uint32_t kOptionPassword = 0x4;

}  // namespace

extern "C" {
int sceImeDialogInit(const SceImeDialogParam*, void*);
SceImeDialogStatus sceImeDialogGetStatus(void);
int sceUserServiceInitialize(void*);
int sceUserServiceGetForegroundUser(int*);
}

namespace ime {
namespace {

wchar_t g_text[0x800];
wchar_t g_title[0x80];
wchar_t g_placeholder[0x100];
bool g_open = false, g_seenRunning = false, g_return = false;

// wchar_t is what Sony's IME reads and writes (SDL relies on the same); UTF-16 if it's 2 bytes.
void toWide(const std::string& in, wchar_t* out, size_t cap) {
  size_t n = 0;
  for (size_t i = 0; i < in.size() && n + 2 < cap;) {
    unsigned char c = in[i];
    uint32_t cp;
    int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
    cp = len == 1 ? c : len == 2 ? c & 0x1f : len == 3 ? c & 0x0f : c & 0x07;
    for (int k = 1; k < len && i + k < in.size(); ++k) cp = (cp << 6) | (in[i + k] & 0x3f);
    i += len;
    if (sizeof(wchar_t) == 2 && cp > 0xffff) {
      cp -= 0x10000;
      out[n++] = wchar_t(0xd800 + (cp >> 10));
      out[n++] = wchar_t(0xdc00 + (cp & 0x3ff));
    } else {
      out[n++] = wchar_t(cp);
    }
  }
  out[n] = 0;
}

std::string toUtf8(const wchar_t* in) {
  std::string out;
  for (size_t i = 0; in[i]; ++i) {
    uint32_t cp = uint32_t(in[i]) & (sizeof(wchar_t) == 2 ? 0xffff : 0x1fffff);
    if (sizeof(wchar_t) == 2 && cp >= 0xd800 && cp < 0xdc00 && in[i + 1]) {
      cp = 0x10000 + ((cp - 0xd800) << 10) + ((uint32_t(in[i + 1]) & 0xffff) - 0xdc00);
      ++i;
    }
    if (cp < 0x80) out += char(cp);
    else if (cp < 0x800) out += char(0xc0 | (cp >> 6)), out += char(0x80 | (cp & 0x3f));
    else if (cp < 0x10000) out += char(0xe0 | (cp >> 12)), out += char(0x80 | ((cp >> 6) & 0x3f)), out += char(0x80 | (cp & 0x3f));
    else out += char(0xf0 | (cp >> 18)), out += char(0x80 | ((cp >> 12) & 0x3f)), out += char(0x80 | ((cp >> 6) & 0x3f)), out += char(0x80 | (cp & 0x3f));
  }
  return out;
}

}  // namespace

int open(const Request& r) {
  sceUserServiceInitialize(nullptr);  // already initialised by SDL: harmless
  int user = 0;
  sceUserServiceGetForegroundUser(&user);
  std::memset(g_text, 0, sizeof g_text);
  toWide(r.title, g_title, sizeof g_title / sizeof g_title[0]);
  toWide(r.placeholder, g_placeholder, sizeof g_placeholder / sizeof g_placeholder[0]);

  SceImeDialogParam p{};
  p.userId = user;
  p.type = r.password ? 1 : 0;
  p.enterLabel = r.send ? 1 : 0;
  p.option = r.password ? kOptionPassword : 0;
  p.maxTextLength = sizeof g_text / sizeof g_text[0] - 1;
  p.inputTextBuffer = g_text;
  p.posx = 1920 / 2.0f;  // centred on the 1080p screen
  p.posy = 1080 / 2.0f;
  p.halign = 1;
  p.valign = 1;
  p.placeholder = g_placeholder;
  p.title = g_title;
  int err = sceImeDialogInit(&p, nullptr);
  g_open = err == 0;
  g_seenRunning = g_return = false;
  return err;
}

void sawReturn() {
  if (g_open) g_return = true;
}

State poll() {
  if (!g_open) return State::Idle;
  // SDL's pump runs before our event loop: on OK it queued the Return we've just seen and
  // terminated the dialog. Without a Return, a dialog that ran and stopped was cancelled.
  if (g_return) {
    g_open = false;
    return State::Done;
  }
  SceImeDialogStatus status = sceImeDialogGetStatus();
  if (status == kStatusRunning) g_seenRunning = true;
  // Finished but not yet collected by SDL's pump: wait. SDL terminates it (status None) either way.
  else if (g_seenRunning && status == kStatusNone) {
    g_open = false;
    return State::Cancelled;
  }
  return State::Open;
}

std::string text() { return toUtf8(g_text); }

}  // namespace ime
