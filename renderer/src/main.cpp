// a2ui-host: the PS5 agent's screen on the Mac.
//
//   a2ui-host --agent http://127.0.0.1:8790/agent      talk to the AG-UI server (window)
//   a2ui-host stream.jsonl                              render a recorded A2UI stream (window)
//   ... --keys k1,k2 --screenshot out.bmp               headless: replay buttons, save the last frame
//   ... --agent URL --prompt "text" --keys ... --shots DIR
//                                                       headless conversation, a screenshot per step
//   a2ui-host --pair creds.json [--screenshot out.bmp]  the key setup screen: serve the setup page on
//                                                       :8791, save what's sent, exit (headless with --screenshot)
//   a2ui-host --agent js:server/qjs/agent.js --credentials creds.json
//   a2ui-host --agent channel:http://127.0.0.1:8792     a CopilotKit channel (channel/adapter.ts;
//                                                       PS5_CHANNEL_TOKEN is its access code)
//                                                       the in-process runtime with a key from the setup page
//
// Without --agent, user actions print to stdout as JSON lines (A2UI 1.0 action messages, {"version": "1.0", "action": ...}; the shape
// CopilotKit 1.85 puts in forwardedProps.copilotkit.ui). With --agent they go to the server.
//
// Keyboard: arrows = D-pad, Enter = Cross, Esc = Circle, T = Triangle, Y = Square, Q/E = L1/R1,
// 1/3 = L2/R2, O = Options (jump to the message bar), Page Up/Down = scroll the conversation. While the message bar or a keyboard dialog has
// focus, typing goes into it (arrows still move focus, Enter sends, Esc clears or cancels).
// Key script tokens: up down left right cross circle triangle square l1 r1 l2 r2 options,
// type:<text>, erase, scrollup, scrolldown, scrollby:<px> (smooth, as the stick), shot, wait (until the agent is idle; also implied after any
// key that starts a run).
#include <SDL.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "a2ui/session.hpp"
#include "a2ui/view.hpp"
#include "agent_client.hpp"
#include "channel_client.hpp"
#include "js_agent.hpp"
#include "pairing.hpp"
#include "canvas_gl.hpp"
#include "canvas_sdl.hpp"

using namespace a2ui;

namespace {

// The app's name: the header shows it unless the agent names itself (CUSTOM "ps5.agent").
constexpr const char* kAppName = "PS5 A2UI";

const std::map<std::string, Button> kButtons = {{"up", Button::Up}, {"down", Button::Down}, {"left", Button::Left}, {"right", Button::Right},
                                                {"cross", Button::Cross}, {"circle", Button::Circle}, {"triangle", Button::Triangle}, {"square", Button::Square},
                                                {"l1", Button::L1}, {"r1", Button::R1}, {"l2", Button::L2}, {"r2", Button::R2}, {"options", Button::Options}};

struct Options {
  std::string file, agent, keys, screenshot, shots, prompt, pair, credentials, systemFixture, font = A2UI_DEFAULT_FONT;
  bool panes = false;  // --layout panes: the conversation left, the live screen right
  bool gl = false;     // --gl: draw with OpenGL (GlCanvas, as on the console) instead of SDL_Renderer
  bool trace = false;  // --trace: print each text delta from the agent as it arrives, with its time
  bool scripted() const { return !keys.empty() || !screenshot.empty() || !shots.empty() || !prompt.empty(); }
};

/** Everything one screen needs: the A2UI store, the view, and (with --agent) the conversation. */
struct App {
  Store store;
  View view{store};
  Session session{store};
  std::unique_ptr<AgentConnection> client;
  bool trace = false;
  std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();

  explicit App(const Options& o) : trace(o.trace) {
    if (o.panes) view.setLayout(View::Layout::Panes);
    view.setAgentName(kAppName);
    // AGENT_TOKEN: the server's access code, when it listens beyond this Mac (scripts/ps5-play.sh).
    const char* token = std::getenv("AGENT_TOKEN");
    if (o.agent.rfind("js:", 0) == 0) {
      // In-process: the bundled TypeScript runtime in QuickJS (what the console runs). Only the
      // model's settings are passed to it.
      JsAgent::Options jo;
      std::ifstream in(o.agent.substr(3));
      if (!in) throw std::runtime_error("cannot read " + o.agent.substr(3));
      jo.bundle.assign(std::istreambuf_iterator<char>(in), {});
      if (!o.credentials.empty()) {
        auto creds = Credentials::load(o.credentials);
        if (!creds || creds->provider == "server") throw std::runtime_error(o.credentials + ": no MiniMax or OpenRouter key");
        jo.env = creds->env();
        jo.model = creds->modelOrDefault();
        std::cerr << "credentials: " << creds->label() << ", model " << jo.model << "\n";
      } else {
        for (const char* k : {"LLM_BASE_URL", "LLM_API_KEY"})
          if (const char* v = std::getenv(k)) jo.env[k] = v;
        if (const char* m = std::getenv("LLM_MODEL")) jo.model = m;  // e.g. minimax/minimax-m3 on OpenRouter
      }
      jo.log = [](const std::string& line) { std::cerr << "[js] " << line << "\n"; };
      // --system FILE: stand-in answers for the agent's console reads ({"installed_titles": {...}}),
      // since the Mac has no console to read.
      if (!o.systemFixture.empty()) {
        std::ifstream sf(o.systemFixture);
        json answers = json::parse(sf, nullptr, false);
        if (!answers.is_object()) throw std::runtime_error("cannot read " + o.systemFixture);
        jo.system = [answers](const std::string& name) { return answers.contains(name) ? answers[name] : json{{"error", "unknown"}, {"reason", name}}; };
      }
      auto js = std::make_unique<JsAgent>(std::move(jo));
      std::cerr << "agent runtime loaded in QuickJS in " << int(js->loadMs()) << " ms\n";
      client = std::move(js);
    } else if (o.agent.rfind("channel:", 0) == 0) {
      // A CopilotKit channel (channel/adapter.ts): PS5_CHANNEL_TOKEN is its access code. The Mac
      // has no controller here; the keyboard stands in, so no controller feedback is offered.
      const char* ct = std::getenv("PS5_CHANNEL_TOKEN");
      // --system FILE: stand-in answers to the channel's reads about the console.
      json answers = json::object();
      std::vector<std::string> reads;
      if (!o.systemFixture.empty()) {
        std::ifstream sf(o.systemFixture);
        answers = json::parse(sf, nullptr, false);
        if (!answers.is_object()) throw std::runtime_error("cannot read " + o.systemFixture);
        for (auto& [k, v] : answers.items()) reads.push_back(k);
      }
      auto channel = std::make_unique<ChannelClient>(o.agent.substr(8), ct ? ct : "", "mac", ChannelClient::describe("Mac host", false, reads));
      channel->setRequestHandler([answers](const std::string& name, const json&) {
        std::cerr << "  [read] " << name << "\n";
        return answers.contains(name) ? answers[name] : json{{"error", "unknown"}, {"reason", name}};
      });
      client = std::move(channel);
    } else if (!o.agent.empty()) {
      client = std::make_unique<AgentClient>(o.agent, token ? token : "");
    }
    view.onAction = [this](const UserAction& a) {
      if (client && !client->busy()) session.addUser(a.label);
      if (client) client->sendAction(a.toJson());
      else std::cout << a.toJson().dump() << std::endl;
    };
    view.onFunction = [](const json& fc) { std::cout << json{{"functionCall", fc}}.dump() << std::endl; };
    view.onPrompt = [this](const std::string& text) {
      if (!client) {
        std::cout << json{{"prompt", text}}.dump() << std::endl;
        return true;
      }
      if (!client->sendPrompt(text)) return false;  // a run is still going: keep the draft
      session.addUser(text);
      return true;
    };
    view.onNewConversation = [this] {
      if (!client || !client->newConversation()) return;
      session.clear();
      store.clear();
    };
    view.onLocal = [this](const std::string& e) {
      // Cancelling a keyboard tells the agent, so it doesn't wait for an answer forever.
      if (client && e.rfind("cancel ", 0) == 0)
        client->sendAction(a2ui::UserAction{"dismissed", e.substr(7), "root", json::object(), "", ""}.toJson());
      else if (!client) std::cout << json{{"local", e}}.dump() << std::endl;
    };
  }

  /** Apply events from the agent and refresh the conversation chrome. Call once per frame. */
  void update() {
    if (!client) return;
    client->pump();
    for (const json& e : client->poll()) {
      if (trace && e.value("type", "") == "TEXT_MESSAGE_CONTENT")
        std::cerr << "  [text +" << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count()
                  << " ms] " << json(e.value("delta", "")).dump() << "\n";
      session.handle(e);
    }
    // Controller feedback from the agent: the Mac has no DualSense support, so print it like the
    // view's function calls.
    for (const json& call : session.takeFunctionCalls()) view.onFunction(call);
    view.setTranscript(session.transcript());
    view.setFollowUps(session.followUps());
    view.setStatus(session.status());
    view.setError(session.error());
    view.setProgress(session.progress());
    view.setAgentName(session.agentName().empty() ? kAppName : session.agentName());
  }

  bool busy() const { return client && (client->busy() || session.running()); }
};

bool saveBmp(SDL_Surface* target, const std::string& path) {
  // 24-bit BMP: readable everywhere (32-bit BMPs with alpha confuse some viewers).
  SDL_Surface* rgb = SDL_ConvertSurfaceFormat(target, SDL_PIXELFORMAT_RGB24, 0);
  int rc = rgb ? SDL_SaveBMP(rgb, path.c_str()) : -1;
  if (rgb) SDL_FreeSurface(rgb);
  if (rc != 0) std::fprintf(stderr, "save %s: %s\n", path.c_str(), SDL_GetError());
  return rc == 0;
}

// Characters NotoSans lacks (emoji, symbols, arrows) are drawn from these.
void addFallbackFonts(GeometryCanvas& canvas) {
  for (const char* name : {"NotoEmoji.ttf", "NotoSansSymbols2.ttf", "NotoSansSymbols.ttf"}) {
    try {
      canvas.addFallbackFont(GeometryCanvas::readFont(std::string(A2UI_VENDOR) + "/fonts/" + name));
    } catch (const std::exception& e) {
      std::cerr << e.what() << "\n";
    }
  }
}

int headless(App& app, const Options& o) {
  // Software: SDL_Renderer into a surface. --gl: GlCanvas into an offscreen framebuffer of a
  // hidden GL window, read back for screenshots.
  SDL_Surface* target = nullptr;
  SDL_Renderer* renderer = nullptr;
  SDL_Window* window = nullptr;
  SDL_GLContext context = nullptr;
  std::unique_ptr<SdlCanvas> sdlCanvas;
  std::unique_ptr<GlCanvas> glCanvas;
  if (o.gl) {
    window = SDL_CreateWindow("a2ui-host", 0, 0, 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    context = window ? SDL_GL_CreateContext(window) : nullptr;
    if (!context) { std::fprintf(stderr, "GL: %s\n", SDL_GetError()); return 1; }
    glCanvas = std::make_unique<GlCanvas>(window, GeometryCanvas::readFont(o.font), true);
    addFallbackFonts(*glCanvas);
    std::cerr << "GL: " << glCanvas->info() << "\n";
  } else {
    target = SDL_CreateRGBSurfaceWithFormat(0, int(View::kWidth), int(View::kHeight), 32, SDL_PIXELFORMAT_ARGB8888);
    renderer = SDL_CreateSoftwareRenderer(target);
    if (!renderer) { std::fprintf(stderr, "renderer: %s\n", SDL_GetError()); return 1; }
    sdlCanvas = std::make_unique<SdlCanvas>(renderer, o.font);
    addFallbackFonts(*sdlCanvas);
  }
  Canvas& canvas = glCanvas ? static_cast<Canvas&>(*glCanvas) : *sdlCanvas;
  auto save = [&](const std::string& path) {
    if (!glCanvas) return saveBmp(target, path);
    SDL_Surface* pixels = glCanvas->readPixels();
    bool ok = SDL_SaveBMP(pixels, path.c_str()) == 0;
    SDL_FreeSurface(pixels);
    return ok;
  };
  int step = 0;
  auto frame = [&] {
    app.update();
    app.view.draw(canvas);
    if (glCanvas) glCanvas->present();
  };
  auto shot = [&](const std::string& label) {
    if (o.shots.empty()) return;
    char name[64];
    std::snprintf(name, sizeof name, "/%02d-%s.bmp", step++, label.c_str());
    save(o.shots + name);
  };
  auto waitIdle = [&] {
    using namespace std::chrono;
    auto until = steady_clock::now() + minutes(10);
    do {
      std::this_thread::sleep_for(milliseconds(50));
      frame();
    } while (app.busy() && steady_clock::now() < until);
    frame();
    std::cerr << "  [idle] caption: " << app.session.caption().substr(0, 120) << (app.session.error().empty() ? "" : " | error: " + app.session.error()) << "\n";
  };

  frame();
  if (!o.prompt.empty()) {
    if (!app.client) { std::fprintf(stderr, "--prompt needs --agent\n"); return 2; }
    app.view.onPrompt(o.prompt);  // same path as the message bar (it lands in the transcript)
    waitIdle();
    shot("prompt");
  }
  std::stringstream ss(o.keys);
  for (std::string token; std::getline(ss, token, ',');) {
    if (token.rfind("type:", 0) == 0) app.view.type(token.substr(5));
    else if (token == "erase") app.view.erase();
    else if (token == "scrollup") app.view.scroll(1);
    else if (token == "scrolldown") app.view.scroll(-1);
    else if (token.rfind("scrollby:", 0) == 0) app.view.scrollBy(std::stof(token.substr(9)));  // what the stick does per frame
    else if (token == "shot") { frame(); shot("shot"); continue; }
    else if (token == "wait") {}
    else if (kButtons.count(token)) app.view.press(kButtons.at(token));
    else if (!token.empty()) { std::fprintf(stderr, "unknown key %s\n", token.c_str()); return 2; }
    frame();
    if (app.busy() || token == "wait") {
      waitIdle();
      shot(token.substr(0, token.find(':')));
    }
  }
  frame();
  std::cerr << "focus: " << app.view.focused() << std::endl;
  shot("end");
  if (!o.screenshot.empty() && !save(o.screenshot)) return 1;
  glCanvas.reset();
  sdlCanvas.reset();
  if (context) SDL_GL_DeleteContext(context);
  if (window) SDL_DestroyWindow(window);
  if (renderer) SDL_DestroyRenderer(renderer);
  if (target) SDL_FreeSurface(target);
  return 0;
}

int interactive(App& app, const Options& o) {
  SDL_Window* window = SDL_CreateWindow("a2ui-host (PS5 catalog)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
  SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  SDL_RenderSetLogicalSize(renderer, int(View::kWidth), int(View::kHeight));
  SdlCanvas canvas(renderer, o.font);
  addFallbackFonts(canvas);
  SDL_StartTextInput();
  for (int i = 0; i < SDL_NumJoysticks(); ++i)
    if (SDL_IsGameController(i)) SDL_GameControllerOpen(i);

  static const std::map<SDL_Keycode, Button> keys = {{SDLK_UP, Button::Up}, {SDLK_DOWN, Button::Down}, {SDLK_LEFT, Button::Left}, {SDLK_RIGHT, Button::Right},
                                                     {SDLK_RETURN, Button::Cross}, {SDLK_ESCAPE, Button::Circle}, {SDLK_t, Button::Triangle}, {SDLK_y, Button::Square},
                                                     {SDLK_q, Button::L1}, {SDLK_e, Button::R1}, {SDLK_1, Button::L2}, {SDLK_3, Button::R2}, {SDLK_o, Button::Options}};
  static const std::map<int, Button> pad = {{SDL_CONTROLLER_BUTTON_DPAD_UP, Button::Up}, {SDL_CONTROLLER_BUTTON_DPAD_DOWN, Button::Down},
                                            {SDL_CONTROLLER_BUTTON_DPAD_LEFT, Button::Left}, {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, Button::Right},
                                            {SDL_CONTROLLER_BUTTON_A, Button::Cross}, {SDL_CONTROLLER_BUTTON_B, Button::Circle},
                                            {SDL_CONTROLLER_BUTTON_Y, Button::Triangle}, {SDL_CONTROLLER_BUTTON_X, Button::Square},
                                            {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, Button::L1}, {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, Button::R1},
                                            {SDL_CONTROLLER_BUTTON_START, Button::Options}};
  bool typing = false;  // a keyboard dialog was open in the last frame
  for (bool running = true; running;) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT) running = false;
      else if (e.type == SDL_CONTROLLERDEVICEADDED) SDL_GameControllerOpen(e.cdevice.which);
      else if (e.type == SDL_CONTROLLERBUTTONDOWN && pad.count(e.cbutton.button)) app.view.press(pad.at(e.cbutton.button));
      else if (e.type == SDL_TEXTINPUT && typing) app.view.type(e.text.text);
      else if (e.type == SDL_KEYDOWN && (e.key.keysym.sym == SDLK_PAGEUP || e.key.keysym.sym == SDLK_PAGEDOWN)) {
        app.view.scroll(e.key.keysym.sym == SDLK_PAGEUP ? 1 : -1);
      } else if (e.type == SDL_KEYDOWN) {
        SDL_Keycode k = e.key.keysym.sym;
        if (typing) {  // letters arrive as SDL_TEXTINPUT; only these keys act as buttons
          if (k == SDLK_BACKSPACE) app.view.erase();
          else if (k == SDLK_RETURN) app.view.press(Button::Cross);
          else if (k == SDLK_ESCAPE) app.view.press(Button::Circle);
          else if (k == SDLK_UP || k == SDLK_DOWN || k == SDLK_LEFT || k == SDLK_RIGHT) app.view.press(keys.at(k));
        } else if (keys.count(k)) {
          app.view.press(keys.at(k));
        }
      }
    }
    app.update();
    app.view.draw(canvas);
    typing = app.view.keyboardOpen();
    SDL_RenderPresent(renderer);
  }
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  return 0;
}

std::string readFile(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot read " + path);
  return std::string(std::istreambuf_iterator<char>(in), {});
}

int pairMode(const Options& o) {
  // The key setup screen with the real Pairing server, as on the console.
  Pairing::Options po;
  po.page = readFile(A2UI_ASSETS "/pair.html");
  po.naclJs = readFile(A2UI_VENDOR "/tweetnacl/nacl.min.js");
  po.log = [](const std::string& line) { std::cerr << line << "\n"; };
  Pairing pairing(po);
  const std::string ip = Pairing::lanAddress();
  View::SetupScreen screen{ip.empty() ? "" : "http://" + ip + ":" + std::to_string(pairing.port()) + "/", pairing.code(), "", ""};
  std::cerr << "setup page: " << screen.url << " code " << screen.code << "\n";
  Store store;
  View view(store);
  SDL_Surface* target = SDL_CreateRGBSurfaceWithFormat(0, int(View::kWidth), int(View::kHeight), 32, SDL_PIXELFORMAT_ARGB8888);
  SDL_Renderer* renderer = SDL_CreateSoftwareRenderer(target);
  SdlCanvas canvas(renderer, o.font);
  addFallbackFonts(canvas);
  auto draw = [&] {
    screen.code = pairing.code();
    screen.status = pairing.status();
    view.drawSetup(canvas, screen);
    SDL_RenderPresent(renderer);
  };
  draw();
  if (!o.screenshot.empty()) saveBmp(target, o.screenshot);
  std::string lastStatus;
  for (auto until = std::chrono::steady_clock::now() + std::chrono::minutes(10); std::chrono::steady_clock::now() < until;) {
    if (auto creds = pairing.poll()) {
      if (!creds->save(o.pair)) { std::fprintf(stderr, "cannot write %s\n", o.pair.c_str()); return 1; }
      draw();
      if (!o.shots.empty()) saveBmp(target, o.shots + "/setup-done.bmp");
      std::cerr << "saved " << creds->label() << " credentials to " << o.pair << "\n";
      return 0;
    }
    if (pairing.status() != lastStatus) {
      lastStatus = pairing.status();
      draw();
      if (!o.shots.empty()) saveBmp(target, o.shots + "/setup-status.bmp");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  std::fprintf(stderr, "no key received\n");
  return 1;
}

int usage() {
  std::fprintf(stderr,
               "usage: a2ui-host [stream.jsonl] [--agent URL|js:bundle.js|channel:URL] [--prompt text] [--trace] [--system answers.json] [--keys k1,k2,...]\n"
               "                 [--screenshot out.bmp] [--shots DIR] [--font file.ttf] [--layout inline|panes]\n"
               "                 [--credentials creds.json]\n"
               "       a2ui-host --pair creds.json [--screenshot out.bmp] [--shots DIR]\n");
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--keys") o.keys = next();
    else if (a == "--screenshot") o.screenshot = next();
    else if (a == "--shots") o.shots = next();
    else if (a == "--layout") o.panes = next() == "panes";
    else if (a == "--gl") o.gl = true;
    else if (a == "--agent") o.agent = next();
    else if (a == "--prompt") o.prompt = next();
    else if (a == "--trace") o.trace = true;
    else if (a == "--system") o.systemFixture = next();
    else if (a == "--font") o.font = next();
    else if (a == "--pair") o.pair = next();
    else if (a == "--credentials") o.credentials = next();
    else if (a.rfind("--", 0) == 0) return usage();
    else o.file = a;
  }
  if (!o.pair.empty()) {
    SDL_Init(0);
    int rc = pairMode(o);
    SDL_Quit();
    return rc;
  }
  if (o.file.empty() && o.agent.empty()) return usage();

  App app(o);
  if (!o.file.empty()) {
    std::ifstream in(o.file);
    if (!in) { std::fprintf(stderr, "cannot read %s\n", o.file.c_str()); return 1; }
    std::stringstream buffer;
    buffer << in.rdbuf();
    for (const auto& error : app.store.applyJsonl(buffer.str())) std::fprintf(stderr, "%s: %s\n", o.file.c_str(), error.c_str());
  }

  if (SDL_Init(o.scripted() && !o.gl ? 0 : SDL_INIT_VIDEO | (o.scripted() ? 0 : SDL_INIT_GAMECONTROLLER)) != 0) { std::fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
  int rc = o.scripted() ? headless(app, o) : interactive(app, o);
  SDL_Quit();
  return rc;
}
