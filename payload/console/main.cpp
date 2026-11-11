// The agent's screen on the console.
//
// The same model/view/canvas/session code as the Mac host, built for the PS5 with the ported SDL2.
// Where the agent comes from, first match wins:
// - agent.json next to the app ({"url": "http://<mac>:8790/agent", "token": "…"}, written for the
//   session by scripts/ps5-play.sh): AG-UI to the server on the Mac.
//   {"channel": "http://<mac>:8792", "token": "…"} (scripts/ps5-channel.sh): a CopilotKit channel.
//   For one launch only: the app deletes it once read.
// - credentials.json, from the setup screen (Pairing: the key is sent from a phone or computer):
//   a MiniMax or OpenRouter key runs the agent here (JsAgent, QuickJS); "server" talks to a server.
// - Neither: the setup screen. Square there starts the offline demo (sample screens and a canned
//   conversation).
//
// Controls: D-pad or left stick moves, Cross activates, Circle goes back, Options jumps to the message bar,
// touchpad button shows the next sample screen (offline), the right stick scrolls the conversation
// smoothly (further = faster),
// L1 + R1 together opens the setup screen, L3 + R3 together quits.
// Progress and timings go to stdout, which the ELF loader streams back to the sender.
#include <SDL.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iterator>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#include "a2ui/model.hpp"
#include "a2ui/session.hpp"
#include "agent_client.hpp"
#include "channel_client.hpp"
#include "js_agent.hpp"
#include "pairing.hpp"
#include "a2ui/view.hpp"
#include "canvas_gl.hpp"
#include "canvas_sdl.hpp"
#include "dualsense.hpp"
#include "ime.hpp"
#include "sysui.hpp"
#include "ps5_system.hpp"
#include "ps5_voice.hpp"
#include "ps5_whisper.hpp"

#include <mutex>
#include <thread>

extern "C" {
int sceSystemServiceGetAppIdOfRunningBigApp(void);
int sceSystemServiceKillApp(int appId, int how, int reason, int coreDump);
extern const unsigned char font_ttf[], font_ttf_end[];
extern const unsigned char emoji_ttf[], emoji_ttf_end[], symbols2_ttf[], symbols2_ttf_end[], symbols_ttf[], symbols_ttf_end[];
extern const char fixtures_txt[], fixtures_txt_end[];
extern const char agent_js[], agent_js_end[];
extern const char ca_bundle[], ca_bundle_end[];
extern const char pair_html[], pair_html_end[];
extern const char nacl_js[], nacl_js_end[];
}

using namespace a2ui;

namespace {

struct Fixture {
  std::string name, jsonl;
};

std::vector<Fixture> loadFixtures() {
  std::vector<Fixture> out;
  std::string all(fixtures_txt, fixtures_txt_end);
  size_t pos = 0;
  while ((pos = all.find("### ", pos)) != std::string::npos) {
    size_t eol = all.find('\n', pos);
    size_t next = all.find("\n### ", eol);
    out.push_back({all.substr(pos + 4, eol - pos - 4), all.substr(eol + 1, next == std::string::npos ? std::string::npos : next - eol - 1)});
    pos = eol;
  }
  return out;
}

// SDL's PS5 joystick driver numbers DualSense buttons like SDL's game controller layout.
enum PadButton {
  kCross = 0, kCircle = 1, kSquare = 2, kTriangle = 3, kTouchpad = 4, kOptions = 6, kL3 = 7, kR3 = 8,
  kL1 = 9, kR1 = 10, kUp = 11, kDown = 12, kLeft = 13, kRight = 14, kL2 = 15, kR2 = 16,
};

// A copy of the log, written to a file in the app folder as well as stdout. Launched from the tile
// there's no sender to receive stdout, so this file (fetchable over FTP) is the only way to see what
// happened: mic readiness, R2 captures, errors. Opened (truncated) once at start; best-effort.
std::FILE* g_log = nullptr;

void say(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void say(const char* fmt, ...) {
  static std::mutex mu;  // STT runs on a worker thread; keep each line's writes from interleaving
  std::lock_guard<std::mutex> lock(mu);
  va_list ap;
  va_start(ap, fmt);
  std::printf("[console] ");
  std::vprintf(fmt, ap);
  std::printf("\n");
  std::fflush(stdout);
  if (g_log) {
    va_list ap2;
    va_start(ap2, fmt);
    std::fprintf(g_log, "[console] ");
    std::vfprintf(g_log, fmt, ap2);
    std::fprintf(g_log, "\n");
    std::fflush(g_log);
    va_end(ap2);
  }
  va_end(ap);
}

constexpr const char* kCredentials = "credentials.json";  // in the app's folder (the loader's cwd)
constexpr const char* kAppDir = "/data/homebrew/PS5AgenticUI";

/** The agent for a stored key: in-process for MiniMax/OpenRouter, the AG-UI client for a server,
 * the channel client for a CopilotKit channel. */
bool padConnected = false;
ChannelClient* channelClient = nullptr;  // set while the agent is a channel

/** What this console reads about itself for the agent: its tools on the console, or a channel's. */
const std::vector<std::string> kReads = {"installed_titles", "system_info", "temperatures", "saves", "screenshots", "play_activity"};
json readConsole(const std::string& name) {
  if (name == "installed_titles") {
    auto t = ps5sys::consoleTitles();
    static std::atomic<bool> described{false};  // what the titles table holds, once: what else could be read
    if (!described.exchange(true))
      say("titles: %s, table %s", t.source.empty() ? t.error.c_str() : t.source.c_str(), t.schema.empty() ? "-" : t.schema.c_str());
    size_t installed = 0;
    for (const auto& title : t.titles) installed += title.installed;
    say("titles: %zu installed of %zu (installed by %s)", installed, t.titles.size(), t.installedBy.empty() ? "-" : t.installedBy.c_str());
    return ps5sys::toJson(t);
  }
  if (name == "play_activity") {
    std::string schema;
    json out = ps5sys::playActivity(ps5sys::consoleTitles(), {}, &schema);
    static std::atomic<bool> described{false};  // the play log's columns, once: a date column would give "last played"
    if (!described.exchange(true)) say("play log: %s", schema.empty() ? "-" : schema.c_str());
    say("play_activity: %s", out.dump().substr(0, 300).c_str());
    return out;
  }
  json out = ps5sys::read(name);
  say("%s: %s", name.c_str(), out.dump().substr(0, 300).c_str());
  return out;
}

/** Connects to a TCP port on this console; -1 if nothing listens. */
int connectLocal(int port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

/**
 * The background payload (payload/presence, presence.elf next to the app) shows what the channel's
 * agent says while the app is closed. Started here, through the console's ELF loader (:9021), if
 * it isn't running (its control port, 8799, doesn't answer). It reads the channel from this
 * folder itself, and outlives the app.
 */
void ensurePresence() {
  if (int fd = connectLocal(8799); fd >= 0) {
    ::close(fd);
    say("background payload: running");
    return;
  }
  std::ifstream f("presence.elf", std::ios::binary);
  std::string elf((std::istreambuf_iterator<char>(f)), {});
  if (elf.empty()) {
    say("background payload: no presence.elf next to the app");
    return;
  }
  int fd = connectLocal(9021);
  if (fd < 0) {
    say("background payload: no ELF loader on :9021");
    return;
  }
  size_t sent = 0;
  while (sent < elf.size()) {
    ssize_t n = ::write(fd, elf.data() + sent, elf.size() - sent);
    if (n <= 0) break;
    sent += size_t(n);
  }
  ::close(fd);
  say("background payload: %s", sent == elf.size() ? "started" : "couldn't send it");
}

std::unique_ptr<AgentConnection> connect(const Credentials& creds) {
  channelClient = nullptr;
  if (creds.provider == "channel") {
    ensurePresence();
    // What this console can do goes with every run; a controller connecting later updates it.
    auto client = std::make_unique<ChannelClient>(creds.url, creds.key, "ps5", ChannelClient::describe("PS5", padConnected, kReads));
    client->setRequestHandler([](const std::string& name, const json&) { return readConsole(name); });
    channelClient = client.get();
    say("channel: %s (conversation %s)", creds.url.c_str(), client->threadId().c_str());
    return client;
  }
  if (creds.provider == "server") {
    auto client = std::make_unique<AgentClient>(creds.url, creds.key);
    say("online: agent %s (thread %s)", creds.url.c_str(), client->threadId().c_str());
    return client;
  }
  JsAgent::Options jo;
  jo.bundle.assign(agent_js, agent_js_end);
  jo.env = creds.env();
  jo.model = creds.modelOrDefault();
  jo.caBundle.assign(ca_bundle, ca_bundle_end);
  jo.log = [](const std::string& line) { say("js: %s", line.substr(0, 400).c_str()); };
  // The agent's console tools read this console (installed games so far; the rest is simulated).
  jo.system = readConsole;
  try {
    auto js = std::make_unique<JsAgent>(std::move(jo));
    say("direct: agent runtime in QuickJS, loaded in %.0f ms, %s model %s", js->loadMs(), creds.label().c_str(), creds.modelOrDefault().c_str());
    return js;
  } catch (const std::exception& e) {
    say("direct: %s", e.what());
    return nullptr;
  }
}

}  // namespace

int main() {
  // websrv starts us in our folder (homebrew.js, scripts/ps5-run.sh). Anything that starts us
  // elsewhere (its loader defaults to "/") would lose the key and the Mesa libraries.
  if (::access("eboot.elf", F_OK) != 0 && ::chdir(kAppDir) == 0) say("started outside the app folder; using %s", kAppDir);
  g_log = std::fopen("console-log.txt", "w");  // now in the app folder: a log the tile launch can't stream
  say("ps5-agentic-ui console starting");
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) != 0) {
    say("SDL_Init: %s", SDL_GetError());
    _exit(1);
  }
  std::vector<unsigned char> font(font_ttf, font_ttf_end);
  SDL_Window* window = nullptr;
  SDL_Renderer* renderer = nullptr;
  SDL_Surface* surface = nullptr;
  std::unique_ptr<GlCanvas> glCanvas;
  std::unique_ptr<SdlCanvas> sdlCanvas;

  // GPU first: OpenGL ES 2 through Mesa (zink on RADV), EGL presenting to VideoOut. The Mesa
  // libraries sit next to the app (scripts/ps5-run.sh uploads them). RENDERER=software skips it.
  const char* forced = std::getenv("RENDERER");
  if (!forced || std::string(forced) != "software") {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    window = SDL_CreateWindow("ps5-agentic-ui", 0, 0, 1920, 1080, SDL_WINDOW_FULLSCREEN | SDL_WINDOW_OPENGL);
    SDL_GLContext context = window ? SDL_GL_CreateContext(window) : nullptr;
    if (context) {
      try {
        glCanvas = std::make_unique<GlCanvas>(window, font);
        SDL_GL_SetSwapInterval(1);
        int dw = 0, dh = 0;
        SDL_GL_GetDrawableSize(window, &dw, &dh);
        say("renderer: GPU, %s, drawable %dx%d", glCanvas->info().c_str(), dw, dh);
      } catch (const std::exception& e) {
        say("GL canvas: %s", e.what());
      }
    } else {
      say("GL: %s", SDL_GetError());
    }
    if (!glCanvas) {
      // Once EGL has taken the display, SDL can't show a software framebuffer in this process.
      say("GPU drawing failed; relaunch with RENDERER=software");
      if (window) SDL_DestroyWindow(window);
      window = nullptr;
    }
  }
  if (!glCanvas) {
    // Software: SDL's software renderer into the window's framebuffer (sceVideoOut).
    if (!window) window = SDL_CreateWindow("ps5-agentic-ui", 0, 0, 1920, 1080, SDL_WINDOW_FULLSCREEN);
    if (!window) {
      say("SDL_CreateWindow: %s", SDL_GetError());
      _exit(1);
    }
    renderer = SDL_CreateRenderer(window, -1, 0);
    if (!renderer) {
      surface = SDL_GetWindowSurface(window);
      if (!surface) {
        say("SDL_GetWindowSurface: %s", SDL_GetError());
        _exit(1);
      }
      renderer = SDL_CreateSoftwareRenderer(surface);
    }
    if (!renderer) {
      say("SDL_CreateSoftwareRenderer: %s", SDL_GetError());
      _exit(1);
    }
    SDL_RenderSetLogicalSize(renderer, int(View::kWidth), int(View::kHeight));
    sdlCanvas = std::make_unique<SdlCanvas>(renderer, font);
    say("renderer: software");
  }
  {
    // Characters Noto Sans lacks: emoji (monochrome), symbols, arrows.
    GeometryCanvas& g = glCanvas ? static_cast<GeometryCanvas&>(*glCanvas) : *sdlCanvas;
    g.addFallbackFont({emoji_ttf, emoji_ttf_end});
    g.addFallbackFont({symbols2_ttf, symbols2_ttf_end});
    g.addFallbackFont({symbols_ttf, symbols_ttf_end});
  }
  Canvas& canvas = glCanvas ? static_cast<Canvas&>(*glCanvas) : *sdlCanvas;
  auto present = [&] {
    if (glCanvas) return glCanvas->present();
    SDL_RenderPresent(renderer);
    if (surface && SDL_UpdateWindowSurface(window) != 0) say("SDL_UpdateWindowSurface: %s", SDL_GetError());
  };
  const std::vector<Fixture> fixtures = loadFixtures();
  say("%zu sample screens", fixtures.size());

  Store store;
  View view(store);
  view.setBackground("background.png");  // console-style backdrop next to the app (finding: themeing)
  Session session(store);
  std::unique_ptr<AgentConnection> client;
  SDL_Joystick* pad = nullptr;  // the first controller: controller feedback goes to it
  // Controller functions bound to a screen's buttons (functionCall actions) run here too.
  view.onFunction = [&](const json& call) { say("controller: %s", dualsense::run(pad, call).c_str()); };
  std::string using_;  // the stored key's provider, for the setup screen ("" = none)
  std::unique_ptr<Pairing> pairing;  // the setup screen is up
  View::SetupScreen setup;
  auto openSetup = [&] {
    Pairing::Options po;
    po.page.assign(pair_html, pair_html_end);
    po.naclJs.assign(nacl_js, nacl_js_end);
    po.log = [](const std::string& line) { say("%s", line.c_str()); };
    try {
      pairing = std::make_unique<Pairing>(std::move(po));
    } catch (const std::exception& e) {
      say("setup: %s", e.what());
      return;
    }
    const std::string ip = Pairing::lanAddress();
    setup = {ip.empty() ? "" : "http://" + ip + ":" + std::to_string(pairing->port()) + "/", pairing->code(), "", using_};
    say("setup screen: %s", setup.url.empty() ? "no network" : setup.url.c_str());  // never the code
  };
  {
    // A session file lasts one launch: deleted once read, so one left behind by a script that didn't
    // clean up can't keep the app on a channel that's gone. The next launch uses credentials.json.
    std::ifstream f("agent.json");  // the loader starts us in the app's folder
    json cfg = f ? json::parse(f, nullptr, false) : json();
    if (f) {
      f.close();
      say("agent.json: this launch only, %s", ::unlink("agent.json") == 0 ? "deleted" : "couldn't delete it");
    }
    if (cfg.is_object() && cfg.value("channel", "") != "") {
      // scripts/ps5-channel.sh: a CopilotKit channel on the computer, for this session.
      client = connect({"channel", cfg.value("token", ""), "", cfg.value("channel", "")});
    } else if (cfg.is_object() && cfg.value("url", "") != "") {
      client = std::make_unique<AgentClient>(cfg.value("url", ""), cfg.value("token", ""));
      say("online: agent %s (thread %s)", cfg.value("url", "").c_str(), client->threadId().c_str());
    } else if (auto creds = Credentials::load(kCredentials)) {
      using_ = creds->label();
      client = connect(*creds);
    } else {
      say("no key yet");
      openSetup();
    }
  }
  std::vector<ChatLine> transcript = {
      {ChatLine::Role::Assistant, "Hi! This is the offline demo. Press the touchpad button to see sample screens, or L1 + R1 to set up the assistant with a model key.", ""},
  };
  auto assistant = [&](const std::string& text) { transcript.push_back({ChatLine::Role::Assistant, text, ""}); };
  int current = -1;
  auto show = [&](int index) {
    store.clear();
    current = index;
    if (index < 0) return;
    auto errors = store.applyJsonl(fixtures[index].jsonl);
    say("screen %d/%zu: %s%s", index + 1, fixtures.size(), fixtures[index].name.c_str(), errors.empty() ? "" : " (with errors)");
  };

  view.onAction = [&](const UserAction& a) {
    if (client) {
      if (!client->busy()) session.addUser(a.label);
      client->sendAction(a.toJson());
      say("action %s", a.name.c_str());
      return;
    }
    if (!a.label.empty()) transcript.push_back({ChatLine::Role::User, a.label, ""});
    // Offline: say what would go to the agent (event name and context keys, never values).
    std::string keys;
    for (auto& [k, v] : a.context.items()) keys += (keys.empty() ? "" : ", ") + k;
    assistant("Offline: the agent would get \"" + a.name + "\"" + (keys.empty() ? "." : " with " + keys + "."));
    say("action %s on %s", a.name.c_str(), a.surfaceId.c_str());
    show(-1);
  };
  view.onPrompt = [&](const std::string& text) {
    if (client) {
      if (!client->sendPrompt(text)) return false;  // a run is still going: keep the draft
      session.addUser(text);
      say("prompt (%zu chars)", text.size());
      return true;
    }
    transcript.push_back({ChatLine::Role::User, text, ""});
    assistant("Offline demo: press L1 + R1 to set up the assistant with a model key.");
    say("prompt (%zu chars)", text.size());
    return true;
  };
  view.onNewConversation = [&] {
    if (!client || !client->newConversation()) return;
    session.clear();
    store.clear();
    say("new conversation");
  };
  view.onLocal = [&](const std::string& e) {
    // Cancelling a keyboard tells the agent, so it doesn't wait for an answer forever.
    if (client && e.rfind("cancel ", 0) == 0)
      client->sendAction(a2ui::UserAction{"dismissed", e.substr(7), "root"}.toJson());
    else if (!client && (e == "back" || e.rfind("cancel ", 0) == 0)) show(-1);
  };
  bool keyboardUp = false;
  view.onKeyboard = [&](const View::KeyboardRequest& r) {
    // Our own sceImeDialog call (centred, titled, password mode); see ime.hpp.
    const bool bar = r.title == "Ask the assistant";
    int err = ime::open({r.title, r.placeholder, r.password, bar});
    say("system keyboard for \"%s\"%s: %s", r.title.c_str(), r.password ? " (password)" : "", err ? "failed" : "open");
    if (err) say("sceImeDialogInit: 0x%08x", unsigned(err));
    keyboardUp = err == 0;
  };

  // The PS5's own message dialog for confirms (the approval gate), and system notifications.
  bool confirmUp = false, nativeFailed = false, progressFailed = false;
  {
    std::string why;
    bool native = sysui::init(why);
    say("system dialogs: %s (%s)", native ? "on" : "off", why.c_str());
    if (native) {
      view.onConfirm = [&](const View::ConfirmRequest& r) {
        std::string how;
        int err = sysui::openConfirm({r.message, r.confirmLabel, r.cancelLabel, r.destructive}, how);
        say("system confirm: %s%s", how.c_str(), err ? " -> drawing it instead" : "");
        confirmUp = err == 0;
        nativeFailed = err != 0;  // dropped after this frame; the view then draws the dialog
      };
      view.onNotice = [&](const std::string& text, const std::string& icon) {
        sysui::notify(text, icon);
        say("system notification (%s)", icon.c_str());
      };
    }
  }

  bool held[17] = {};
  ps5voice::setLog([](const char* line) { say("%s", line); });  // mic steps → console-log.txt
  ps5voice::Recorder recorder;  // hold R2 to record the mic (voice capture, M6)
  say("microphone: %s", recorder.available() ? "ready (hold R2 to speak)" : recorder.error().c_str());
  ps5whisper::setLog([](const char* line) { say("%s", line); });  // whisper/ggml steps → console-log.txt
  ps5whisper::Transcriber transcriber("ggml-base.en.bin");  // on-console STT on the GPU (M6, finding 25)
  view.setVoiceAvailable(recorder.available());  // gates the "R2 Speak" hint and the mic feedback
  // STT runs on a worker thread so the UI keeps animating (mic pulse, the "…" loader) while whisper
  // works; the result is collected and sent on the main thread (client/session aren't thread-safe).
  // One capture at a time — R2 is a single trigger.
  std::thread sttWorker;
  std::atomic<bool> sttDone{false};
  ps5whisper::Result sttResult;
  std::vector<int16_t> sttPcm;
  int sttRate = 16000;
  double sttAudioSec = 0;
  // SDL's PS5 driver reads scePadReadState's bytes in order: axes 0/1 left stick, 2/3 right
  // stick, 4/5 L2/R2. The left stick moves focus like the D-pad: at once, then after 400 ms, then
  // every 150 ms while held. The right stick scrolls the conversation smoothly, by pixels each
  // frame, faster the further it's pushed.
  constexpr int kLeftX = 0, kLeftY = 1, kScrollAxis = 3;
  constexpr int kStickOn = 20000, kStickOff = 12000;  // apart, so a stick near the edge doesn't flicker
  struct Repeat {
    int dir = 0;
    bool repeating = false;
    std::chrono::steady_clock::time_point next;
    void set(int d) {
      if (d == dir) return;
      dir = d;
      repeating = false;
      next = std::chrono::steady_clock::now();
    }
    bool due() {
      auto now = std::chrono::steady_clock::now();
      if (dir == 0 || now < next) return false;
      next = now + std::chrono::milliseconds(repeating ? 150 : 400);
      repeating = true;
      return true;
    }
  };
  Repeat moveStick;
  int leftX = 0, leftY = 0, rawLeft[2] = {}, rightY = 0;
  constexpr int kScrollDead = 7000;        // the stick's resting wobble stays below this
  constexpr float kScrollFastest = 2400;   // pixels per second (1080p layout) at full tilt
  auto lastFrame = std::chrono::steady_clock::now();
  // A direction from one axis value: -1, 0 or 1, keeping the last one between the two thresholds.
  auto axisDir = [&](int value, int was) {
    if (std::abs(value) >= kStickOn) return value < 0 ? -1 : 1;
    return std::abs(value) <= kStickOff ? 0 : was;
  };
  bool axesLogged[8] = {};
  bool running = true, dirty = true;
  // Voice capture, driven from R2. The PS5 SDL driver reports L2/R2 as analog axes (4/5), not
  // buttons, so a held R2 arrives as JOYAXISMOTION, not JOYBUTTONDOWN; we start/stop on the axis
  // crossing a threshold and also from the button, in case a driver does report it. The start/stop
  // guards make whichever arrives second a no-op, so there's no double capture.
  constexpr int kR2Axis = 5;
  bool r2Held = false;
  auto startVoice = [&] {
    if (!client || !recorder.available() || recorder.recording() || sttWorker.joinable()) return;
    recorder.start();
    view.setVoice(View::Voice::Listening);  // the mic pulses while R2 is held
    say("voice: recording (release R2 to stop)");
    dirty = true;
  };
  auto stopVoice = [&] {
    if (!recorder.recording()) return;
    ps5voice::Status st = ps5voice::Status::Ok;
    ps5voice::Recording rec = recorder.stop(&st);
    const char* why = st == ps5voice::Status::Ok ? "ok" : st == ps5voice::Status::TooShort ? "too short" : st == ps5voice::Status::Silent ? "no speech" : "failed";
    say("voice: captured %.1fs (%zu samples): %s", rec.seconds(), rec.samples.size(), why);
    if (st == ps5voice::Status::Ok) {
      std::vector<uint8_t> wav = ps5voice::toWav(rec);
      if (FILE* f = std::fopen("voice-last.wav", "wb")) {
        std::fwrite(wav.data(), 1, wav.size(), f);
        std::fclose(f);
        say("voice: saved voice-last.wav (%zu bytes)", wav.size());
      } else {
        say("voice: couldn't write voice-last.wav (errno %d)", errno);
      }
    }
    // Transcribe on the GPU (ggml-vulkan) off a worker thread; the composer shows the "…" loader
    // until the main loop picks up the result and sends it. The guard means one capture at a time.
    if (st == ps5voice::Status::Ok && transcriber.available() && !sttWorker.joinable()) {
      sttRate = rec.sampleRate;
      sttAudioSec = rec.seconds();
      sttPcm = std::move(rec.samples);
      sttDone.store(false);
      view.setVoice(View::Voice::Transcribing);
      sttWorker = std::thread([&] {
        sttResult = transcriber.transcribe(sttPcm, sttRate);
        sttDone.store(true);
      });
    } else {
      view.setVoice(View::Voice::Idle);
    }
    dirty = true;
  };
  // Timing, logged every 10 s when anything was drawn: frame draw time (average and worst) and the
  // longest agent pump (JavaScript work runs there, on this thread: a long one is a hitch).
  int frames = 0;
  double drawMs = 0, worstDrawMs = 0, worstPumpMs = 0;
  auto timingStart = std::chrono::steady_clock::now();
  using Ms = std::chrono::duration<double, std::milli>;
  // What the system says about the app (PS button overlay, Home), checked 4 times a second and
  // logged when it changes. While the app is in the background it doesn't draw.
  sysui::SystemState system;
  bool systemKnown = false;
  auto systemCheck = std::chrono::steady_clock::now();
  while (running) {
    if (std::chrono::steady_clock::now() - systemCheck >= std::chrono::milliseconds(250)) {
      systemCheck = std::chrono::steady_clock::now();
      sysui::SystemState now = sysui::pollSystem();
      if (!systemKnown || now.status != system.status || now.overlaid != system.overlaid || now.background != system.background) {
        if (now.status != 0) say("system status: error 0x%08x", unsigned(now.status));
        else say("system status: overlay %s, background %s", now.overlaid ? "yes" : "no", now.background ? "yes" : "no");
        if (system.background && !now.background) dirty = true;  // back in front: draw again
      }
      for (unsigned type : now.events) say("system event 0x%08x", type);
      now.events.clear();
      system = now;
      systemKnown = true;
    }
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_WINDOWEVENT) say("window event %d", int(e.window.event));
      // Stick and motion events don't change the screen; don't redraw for them.
      if (e.type != SDL_JOYAXISMOTION && e.type != SDL_JOYHATMOTION) dirty = true;
      if (e.type == SDL_QUIT) running = false;
      else if (e.type == SDL_JOYAXISMOTION) {
        if (e.jaxis.axis < 8 && std::abs(e.jaxis.value) > 24000 && !axesLogged[e.jaxis.axis]) {
          axesLogged[e.jaxis.axis] = true;
          say("stick axis %d moved (%d)", e.jaxis.axis, e.jaxis.value);
        }
        if (e.jaxis.axis == kR2Axis) {
          // R2 as an analog trigger: pressed past ~half starts, back below a quarter stops.
          if (e.jaxis.value > 16000 && !r2Held) { r2Held = true; startVoice(); }
          else if (e.jaxis.value < 8000 && r2Held) { r2Held = false; stopVoice(); }
        } else if (e.jaxis.axis == kScrollAxis) {
          rightY = e.jaxis.value;
        } else if (e.jaxis.axis == kLeftX || e.jaxis.axis == kLeftY) {
          int& d = e.jaxis.axis == kLeftX ? leftX : leftY;
          d = axisDir(e.jaxis.value, d);
          rawLeft[e.jaxis.axis] = e.jaxis.value;
          // One direction at a time: the axis pushed further wins; 1-4 = up, down, left, right.
          int x = leftX, y = leftY;
          if (x && y) (std::abs(rawLeft[kLeftX]) > std::abs(rawLeft[kLeftY]) ? y : x) = 0;
          moveStick.set(y < 0 ? 1 : y > 0 ? 2 : x < 0 ? 3 : x > 0 ? 4 : 0);
        }
      }
      else if (e.type == SDL_JOYDEVICEADDED) {
        SDL_Joystick* j = SDL_JoystickOpen(e.jdevice.which);
        if (j && !pad) pad = j;
        say("controller %d: %s", e.jdevice.which, j ? SDL_JoystickName(j) : SDL_GetError());
        if (j && !padConnected) {
          padConnected = true;
          if (channelClient) channelClient->setClient(ChannelClient::describe("PS5", true, kReads));
        }
      } else if (e.type == SDL_KEYDOWN && keyboardUp && e.key.keysym.scancode == SDL_SCANCODE_RETURN) {
        ime::sawReturn();  // SDL's pump saw the dialog close with OK
      } else if ((e.type == SDL_JOYBUTTONDOWN || e.type == SDL_JOYBUTTONUP) && e.jbutton.button < 17) {
        held[e.jbutton.button] = e.type == SDL_JOYBUTTONDOWN;
        // Hold R2 to record the mic (R2 isn't a controller-hint button). Most PS5 drivers deliver
        // R2 as an analog axis (handled above); this covers a driver that also reports the button.
        // Capture only; the transcript step (STT) comes next. A held R2 records, release saves a WAV
        // next to the app and logs the stats, so the capture can be verified.
        if (e.jbutton.button == kR2) {
          if (e.type == SDL_JOYBUTTONDOWN) startVoice();
          else stopVoice();
          continue;
        }
        if (e.type != SDL_JOYBUTTONDOWN || keyboardUp || confirmUp || sysui::progressOpen()) continue;
        if (held[kL3] && held[kR3]) running = false;
        if (pairing) {  // the setup screen
          if (e.jbutton.button == kSquare && using_.empty()) {
            pairing.reset();
            say("offline demo");
          } else if (e.jbutton.button == kCircle && !using_.empty()) {
            pairing.reset();
            say("setup: keeping the %s key", using_.c_str());
          } else if (e.jbutton.button == kTriangle && !using_.empty()) {
            ::unlink(kCredentials);
            client.reset();
            channelClient = nullptr;
            say("setup: forgot the %s key", using_.c_str());
            sysui::notify("The assistant's key was removed from this console.", "");
            using_.clear();
            setup.current.clear();
          }
          continue;
        }
        if (held[kL1] && held[kR1] && (e.jbutton.button == kL1 || e.jbutton.button == kR1)) {
          openSetup();
          continue;
        }
        switch (e.jbutton.button) {
          case kUp: view.press(Button::Up); break;
          case kDown: view.press(Button::Down); break;
          case kLeft: view.press(Button::Left); break;
          case kRight: view.press(Button::Right); break;
          case kCross: view.press(Button::Cross); break;
          case kCircle: view.press(Button::Circle); break;
          case kTriangle: view.press(Button::Triangle); break;
          case kSquare: view.press(Button::Square); break;
          case kL1: view.press(Button::L1); break;
          case kR1: view.press(Button::R1); break;
          case kL2: view.press(Button::L2); break;
          case kR2: view.press(Button::R2); break;
          case kOptions: view.press(Button::Options); break;
          case kTouchpad:
            if (!client) show(current + 1 < int(fixtures.size()) ? current + 1 : -1);
            break;
          default: break;
        }
      }
    }
    const bool sticksLive = !pairing && !keyboardUp && !confirmUp && !sysui::progressOpen();
    const auto frameNow = std::chrono::steady_clock::now();
    const float dt = std::min(0.05f, std::chrono::duration<float>(frameNow - lastFrame).count());
    lastFrame = frameNow;
    if (sticksLive && std::abs(rightY) > kScrollDead) {
      // Squared, so small pushes are fine-grained and a full push is quick. Up (negative) = older.
      const float t = float(std::abs(rightY) - kScrollDead) / float(32767 - kScrollDead);
      view.scrollBy((rightY < 0 ? 1 : -1) * kScrollFastest * t * t * dt);
      dirty = true;  // redrawn every frame while it moves (vsync paces it)
    }
    if (moveStick.due() && sticksLive) {
      static constexpr Button kMoves[] = {Button::Up, Button::Down, Button::Left, Button::Right};
      view.press(kMoves[moveStick.dir - 1]);
      dirty = true;
    }
    // Voice feedback animates (mic pulse, "…" loader) so keep drawing while it's live; vsync paces it.
    if (view.voiceActive()) dirty = true;
    // A finished transcription (worker thread): send it as the player's turn, on this thread.
    if (sttDone.load()) {
      if (sttWorker.joinable()) sttWorker.join();
      sttDone.store(false);
      view.setVoice(View::Voice::Idle);
      const ps5whisper::Result& tr = sttResult;
      if (tr.ok) {
        say("voice: transcript (load %.1fs, stt %.1fs for %.1fs audio): \"%s\"",
            tr.loadSeconds, tr.transcribeSeconds, sttAudioSec, tr.text.c_str());
        if (!tr.text.empty() && client && client->sendPrompt(tr.text)) {
          session.addUser(tr.text);
          say("voice: sent transcript as the player's message");
        }
      } else {
        say("voice: transcription failed (%s)", tr.error.c_str());
      }
      dirty = true;
    }
    if (pairing) {
      if (auto creds = pairing->poll()) {
        pairing.reset();
        bool saved = creds->save(kCredentials);
        say("setup: %s key received%s", creds->label().c_str(), saved ? ", saved" : ", NOT saved (this session only)");
        using_ = creds->label();
        client = connect(*creds);
        sysui::notify("The assistant is set up with " + creds->label() + ".", "");
        dirty = true;
      } else if (pairing->code() != setup.code || pairing->status() != setup.status) {
        setup.code = pairing->code();
        setup.status = pairing->status();
        dirty = true;
      }
    }
    if (client) {
      auto p0 = std::chrono::steady_clock::now();
      client->pump();
      worstPumpMs = std::max(worstPumpMs, Ms(std::chrono::steady_clock::now() - p0).count());
      for (const json& ev : client->poll()) {
        session.handle(ev);
        dirty = true;
        std::string type = ev.value("type", "");
        if (type == "RUN_FINISHED") say("run finished");
        else if (type == "RUN_ERROR") say("run error: %s", ev.value("message", std::string("?")).c_str());
        else if (type == "TOOL_CALL_START") say("tool %s", ev.value("toolCallName", std::string("?")).c_str());
      }
      for (const json& call : session.takeFunctionCalls()) say("controller: %s", dualsense::run(pad, call).c_str());
    }
    if (confirmUp) {
      sysui::State st = sysui::poll();
      if (st == sysui::State::Confirmed || st == sysui::State::Cancelled) {
        confirmUp = false;
        say("system confirm: %s", st == sysui::State::Confirmed ? "confirmed" : "cancelled");
        view.answerConfirm(st == sysui::State::Confirmed);
        dirty = true;
      } else if (!view.confirmPending()) {  // the agent took the question back
        sysui::close();
        confirmUp = false;
        say("system confirm: closed (no longer needed)");
      }
    }
    // A console tool's progress: the system progress dialog while it runs (drawn bar if it can't open).
    if (client && view.onConfirm) {
      float p = session.progress();
      if (p >= 0 && !sysui::progressOpen() && !progressFailed) {
        int err = sysui::openProgress(session.progressLabel());
        say("system progress \"%s\": %s", session.progressLabel().c_str(), err ? "failed, drawing it" : "open");
        if (err) say("sceMsgDialogOpen (progress): 0x%08x", unsigned(err));
        progressFailed = err != 0;
      }
      if (sysui::progressOpen()) {
        if (p >= 0) sysui::setProgress(p);
        else {
          sysui::closeProgress();
          say("system progress: closed");
          dirty = true;
        }
      }
      if (p < 0) progressFailed = false;
    }
    if (nativeFailed) {
      view.onConfirm = nullptr;
      nativeFailed = false;
      dirty = true;
    }
    if (keyboardUp) {
      ime::State st = ime::poll();
      if (st == ime::State::Done || st == ime::State::Cancelled) {
        keyboardUp = false;
        std::string text = st == ime::State::Done ? ime::text() : "";
        say("system keyboard %s (%zu chars)", st == ime::State::Done ? "done" : "cancelled", text.size());
        if (st == ime::State::Done) view.submitText(text);
        dirty = true;
      }
    }
    if (system.background) {
      SDL_Delay(16);  // the Home screen is showing: nothing to draw
    } else if (dirty && pairing) {
      view.drawSetup(canvas, setup);
      present();
      dirty = false;
    } else if (dirty) {
      // The header names the agent the player talks to: the channel's, if it said, else the app.
      view.setAgentName(client && !session.agentName().empty() ? session.agentName() : "PS5 A2UI");
      if (client) {
        view.setTranscript(session.transcript());
        view.setFollowUps(session.followUps());
        view.setStatus(session.status());
        view.setError(session.error());
        view.setProgress(sysui::progressOpen() ? -1 : session.progress());  // the drawn bar only as a fallback
      } else {
        view.setTranscript(transcript);
        view.setFollowUps({});
        view.setStatus(current < 0 ? "Touchpad: sample screens · L1 + R1: set up · L3 + R3: quit" : "Sample " + std::to_string(current + 1) + "/" + std::to_string(fixtures.size()) + ": " + fixtures[current].name);
      }
      auto t0 = std::chrono::steady_clock::now();
      view.draw(canvas);
      present();
      const double ms = Ms(std::chrono::steady_clock::now() - t0).count();
      drawMs += ms;
      worstDrawMs = std::max(worstDrawMs, ms);
      ++frames;
      dirty = false;
    } else {
      SDL_Delay(16);
    }
    dualsense::update(pad);
    if (std::chrono::steady_clock::now() - timingStart >= std::chrono::seconds(10)) {
      if (frames > 0) say("timing: %d frames, draw %.1f ms average, %.1f ms worst; agent pump %.1f ms worst", frames, drawMs / frames, worstDrawMs, worstPumpMs);
      frames = 0;
      drawMs = worstDrawMs = worstPumpMs = 0;
      timingStart = std::chrono::steady_clock::now();
    }
  }

  // Runs 3–4: neither SDL's teardown nor _exit() ended the app: the process stayed, with a frozen
  // screen, until the owner closed it from the Switcher. So close it the way the Switcher does:
  // ask the system to kill the foreground app (us, launched by websrv's loader as FAKE00000).
  dualsense::reset(pad);  // don't leave the triggers stiff or the light bar recolored
  int appId = sceSystemServiceGetAppIdOfRunningBigApp();
  say("quitting: closing app 0x%x", unsigned(appId));
  if (appId > 0) {
    int err = sceSystemServiceKillApp(appId, -1, 0, 0);
    if (err) say("sceSystemServiceKillApp: 0x%08x", unsigned(err));
    SDL_Delay(5000);  // the system ends us; this only runs if it didn't
  }
  say("still running; exiting the process");
  _exit(0);
}
