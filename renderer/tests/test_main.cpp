// Tests for the model and the view (no SDL: the view draws onto a fake canvas).
#include <cstdio>
#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "a2ui/haptics.hpp"
#include "a2ui/model.hpp"
#include "a2ui/session.hpp"
#include "a2ui/view.hpp"
#include "ps5_voice.hpp"
#include "pairing.hpp"
#include "ps5_system.hpp"

#include <sqlite3.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fstream>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <thread>

extern "C" {
#include "tweetnacl/tweetnacl.h"
void randombytes(unsigned char* out, unsigned long long n);  // pairing.cpp
}

using namespace a2ui;

static int failures = 0;
#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                      \
    }                                                                  \
  } while (0)

/** Fixed-width text, records nothing: enough for layout and focus. */
struct FakeCanvas : Canvas {
  void clear(Color, Color) override {}
  void fillRect(Rect, Color, float) override {}
  void strokeRect(Rect, Color, float, float) override {}
  void fillCircle(float, float, float, Color) override {}
  void strokeCircle(float, float, float, Color, float) override {}
  void fillTriangle(float, float, float, float, float, float, Color) override {}
  float text(float, float, const std::string& s, float size, Color) override { return measure(s, size); }
  float measure(const std::string& s, float size) override { return s.size() * size * 0.5f; }
  float lineHeight(float size) override { return size * 1.3f; }
};

/** Apply a JSON array of messages (easier to write in a test than one-message-per-line JSONL). */
static void applyAll(Store& store, const char* array) {
  for (const json& m : json::parse(array)) {
    std::string error = store.apply(m);
    if (!error.empty()) { std::fprintf(stderr, "apply: %s\n", error.c_str()); ++failures; }
  }
}

static void testEnvelopesAndBindings() {
  Store store;
  auto errors = store.applyJsonl(R"(
{"version":"1.0","createSurface":{"surfaceId":"s","catalogId":"ps5"}}
{"version":"1.0","updateComponents":{"surfaceId":"s","components":[{"id":"root","component":"Text","text":{"@path":"/greeting"}}]}}
{"version":"1.0","updateDataModel":{"surfaceId":"s","path":"/greeting","value":"hello"}}
not json
)");
  CHECK(errors.size() == 1 && errors[0].find("line 5") == 0);
  const Surface* s = store.surface("s");
  CHECK(s && s->components.size() == 1);
  CHECK(resolveString((*s->component("root"))["text"], s->data, nullptr) == "hello");

  store.setData("s", "/a/b/c", 3);
  CHECK(lookup(store.surface("s")->data, "/a/b/c") && *lookup(store.surface("s")->data, "/a/b/c") == 3);

  json item = {{"name", "Astro Bot"}};
  CHECK(resolveString(json{{"@path", "name"}}, s->data, &item) == "Astro Bot");  // relative, in a template
  CHECK(resolve(json{{"@path", "/missing"}}, s->data, nullptr).is_null());
  CHECK(resolve(json{{"@call", "rumble"}}, s->data, nullptr).is_null());
  // 1.0: only "@path" binds; an object with a plain "path" is a literal value.
  {
    const json literal = json::parse(R"({"path":"/greeting"})");
    CHECK(resolve(literal, s->data, nullptr) == literal);
  }

  // null deletes the key at the path (updateDataModel's value is required in 1.0).
  CHECK(store.apply(json::parse(R"({"version":"1.0","updateDataModel":{"surfaceId":"s","path":"/a/b","value":null}})")).empty());
  CHECK(lookup(store.surface("s")->data, "/a") && !lookup(store.surface("s")->data, "/a/b"));
  CHECK(!store.apply(json::parse(R"({"version":"1.0","updateDataModel":{"surfaceId":"s","path":"/a"}})")).empty());
  // Messages from another protocol version are refused, with the reason.
  CHECK(store.apply(json::parse(R"({"version":"v0.9","createSurface":{"surfaceId":"old","catalogId":"ps5"}})")).find("unsupported A2UI version") == 0);
  CHECK(!store.surface("old"));

  store.apply(json::parse(R"({"version":"1.0","deleteSurface":{"surfaceId":"s"}})"));
  CHECK(store.top() == nullptr);
}

static void testGenerativeUISurface() {
  // CopilotKit 1.85's shape: one snapshot per surface, holding all of its operations; components
  // and data can come inline in createSurface (A2UI 1.0).
  Store store;
  json event = json::parse(R"({"type":"ACTIVITY_SNAPSHOT","messageId":"genui:g","activityType":"generative-ui-surface","replace":true,
    "content":{"surfaceId":"g","target":"chat","catalogId":"ps5","status":"ready","rejected":[],"operations":[
      {"version":"1.0","createSurface":{"surfaceId":"g","catalogId":"ps5","components":[{"id":"root","component":"Text","text":{"@path":"/hello"}}],"dataModel":{"hello":"hi"}}}]}})");
  CHECK(store.apply(event).empty());
  const Surface* g = store.surface("g");
  CHECK(g && g->components.size() == 1 && resolveString((*g->component("root"))["text"], g->data, nullptr) == "hi");
  CHECK(store.apply(event).empty() && store.surfaces().size() == 1);  // the next snapshot replaces it
  // "Surface" is the reserved root container: not a component an agent may send.
  CHECK(!store.apply(json::parse(R"({"version":"1.0","createSurface":{"surfaceId":"x","components":[{"id":"root","component":"Surface"}]}})")).empty());

  // A removed surface: its createSurface then its deleteSurface. It isn't a new screen.
  Session session(store);
  session.handle(json::parse(R"({"type":"RUN_STARTED"})"));
  session.handle(json::parse(R"({"type":"ACTIVITY_SNAPSHOT","messageId":"genui:g","activityType":"generative-ui-surface","replace":true,
    "content":{"surfaceId":"g","target":"chat","catalogId":"ps5","status":"ready","rejected":[],"operations":[
      {"version":"1.0","createSurface":{"surfaceId":"g","catalogId":"ps5"}},{"version":"1.0","deleteSurface":{"surfaceId":"g"}}]}})"));
  CHECK(!store.surface("g") && store.top() == nullptr && session.transcript().empty());

  // The press's visible message, as CopilotKit writes it.
  CHECK(actionText(json::parse(R"({"name":"tile_select","userMessage":"Astro Bot","context":{"id":"PPSA01"}})")) == "Astro Bot\n\nid: PPSA01");
  CHECK(actionText(json::parse(R"({"name":"choose_plan","context":{}})")) == "Choose plan");
  CHECK(actionText(json::parse(R"({"name":"x","context":{"label":"Team","seats":12}})")) == "Team\n\nseats: 12");
  // The envelope CopilotKit's readCopilotKitUIEnvelope accepts (format 3), carrying the press.
  json env = uiEnvelope(json{{"version", "1.0"}, {"action", {{"name", "x"}}}});
  CHECK(env["1.0"]["supportedCatalogIds"][0] == kPs5CatalogId && env["metadata"]["extensions"]["copilotkit_format"] == 3 &&
        env["metadata"]["extensions"]["copilotkit_action"]["action"]["name"] == "x");
}

static void testActivitySnapshot() {
  // The AG-UI A2UI middleware's shape (a2ui-surface, content.a2ui_operations), with 1.0 operations.
  Store store;
  json event = json::parse(R"({"type":"ACTIVITY_SNAPSHOT","messageId":"a2ui-surface-1","activityType":"a2ui-surface","replace":true,
    "content":{"a2ui_operations":[
      {"version":"1.0","createSurface":{"surfaceId":"m","catalogId":"ps5"}},
      {"version":"1.0","updateComponents":{"surfaceId":"m","components":[{"id":"root","component":"Text","text":"hi"}]}}]}})");
  CHECK(store.apply(event).empty());
  CHECK(store.top() && store.top()->id == "m");
  // A replacing snapshot re-sends createSurface: it resets instead of failing.
  CHECK(store.apply(event).empty());
  CHECK(store.surfaces().size() == 1);
  // Status-only snapshots are ignored.
  CHECK(store.apply(json::parse(R"({"type":"ACTIVITY_SNAPSHOT","activityType":"a2ui-surface","content":{"status":"building"}})")).empty());
}

static const char* kMenu = R"([
{"version":"1.0","createSurface":{"surfaceId":"menu","catalogId":"ps5"}},
{"version":"1.0","updateComponents":{"surfaceId":"menu","components":[
  {"id":"root","component":"Screen","title":"Switch model","children":["menu"],"hints":"hints"},
  {"id":"menu","component":"Menu","children":["a","b","c"]},
  {"id":"a","component":"MenuItem","label":"Qwen 9B","action":{"event":{"name":"menu_select","context":{"id":"qwen-9b"}}}},
  {"id":"b","component":"MenuItem","label":"Mistral 7B","action":{"event":{"name":"menu_select","context":{"id":"mistral-7b"}}}},
  {"id":"c","component":"MenuItem","label":"MiniCPM 2B","action":{"event":{"name":"menu_select","context":{"id":"minicpm-2b"}}}},
  {"id":"hints","component":"ButtonHints","hints":[{"button":"triangle","label":"Refresh","action":{"event":{"name":"refresh"}}},
                                                     {"button":"square","label":"Rumble","action":{"functionCall":{"@call":"rumble","args":{"intensity":0.5,"durationMs":200}}}}]}]}}
])";

static void testMenuNavigation() {
  Store store;
  applyAll(store, kMenu);
  View view(store);
  FakeCanvas canvas;
  std::vector<UserAction> actions;
  std::vector<json> functions;
  std::vector<std::string> local;
  view.onAction = [&](const UserAction& a) { actions.push_back(a); };
  view.onFunction = [&](const json& f) { functions.push_back(f); };
  view.onLocal = [&](const std::string& e) { local.push_back(e); };

  view.draw(canvas);
  CHECK((view.focusables() == std::vector<std::string>{"a", "b", "c", View::kComposerKey}));
  CHECK(view.focused() == "a");
  view.press(Button::Down);
  view.press(Button::Down);
  view.draw(canvas);
  CHECK(view.focused() == "c");
  view.press(Button::Down);  // below the screen: the message bar
  CHECK(view.focused() == View::kComposerKey);
  view.press(Button::Up);
  view.press(Button::Up);
  view.draw(canvas);
  view.press(Button::Cross);
  CHECK(actions.size() == 1 && actions[0].name == "menu_select" && actions[0].context["id"] == "mistral-7b");
  CHECK(!actions.empty() && actions[0].toJson()["action"]["surfaceId"] == "menu" && actions[0].sourceComponentId == "b");
  {
    // The A2UI 1.0 action message (renderer_to_agent.json): version, and a timestamp.
    json m = actions[0].toJson();
    CHECK(m["version"] == "1.0" && m["action"]["name"].is_string() && m["action"]["timestamp"].get<std::string>().size() == 24 &&
          m["action"]["timestamp"].get<std::string>().back() == 'Z' && m["action"]["context"].is_object());
  }

  view.press(Button::Triangle);
  CHECK(actions.size() == 2 && actions[1].name == "refresh");
  view.press(Button::Square);
  CHECK(functions.size() == 1 && functions[0]["@call"] == "rumble");
  view.press(Button::Circle);
  CHECK(local.size() == 1 && local[0] == "back");
}

static void testShelfTemplate() {
  Store store;
  applyAll(store, R"([
{"version":"1.0","createSurface":{"surfaceId":"home","catalogId":"ps5"}},
{"version":"1.0","updateComponents":{"surfaceId":"home","components":[
  {"id":"root","component":"Screen","title":"Library","children":["row"]},
  {"id":"row","component":"Shelf","title":"Recent","children":{"componentId":"tile","path":"/games"}},
  {"id":"tile","component":"Tile","title":{"@path":"title"},"action":{"event":{"name":"tile_select","context":{"id":{"@path":"id"}}}}}]}},
{"version":"1.0","updateDataModel":{"surfaceId":"home","path":"/games","value":[{"id":"astro","title":"Astro Bot"},{"id":"hades","title":"Hades II"}]}}
])");
  View view(store);
  FakeCanvas canvas;
  std::vector<UserAction> actions;
  view.onAction = [&](const UserAction& a) { actions.push_back(a); };
  view.draw(canvas);
  CHECK((view.focusables() == std::vector<std::string>{"tile@row#0", "tile@row#1", View::kComposerKey}));
  view.press(Button::Right);
  view.draw(canvas);
  view.press(Button::Cross);
  CHECK(actions.size() == 1 && actions[0].context["id"] == "hades");  // template scope resolved
}

static void testTwoShelvesShareATemplate() {
  // The intent-tool translation reuses one "tile" template for every row: keys must stay unique.
  Store store;
  applyAll(store, R"([
{"version":"1.0","createSurface":{"surfaceId":"home","catalogId":"ps5"}},
{"version":"1.0","updateComponents":{"surfaceId":"home","components":[
  {"id":"root","component":"Screen","title":"Home","children":["row-0","row-1"]},
  {"id":"row-0","component":"Shelf","children":{"componentId":"tile","path":"/rows/0"}},
  {"id":"row-1","component":"Shelf","children":{"componentId":"tile","path":"/rows/1"}},
  {"id":"tile","component":"Tile","title":{"@path":"title"},"action":{"event":{"name":"tile_select","context":{"id":{"@path":"id"}}}}}]}},
{"version":"1.0","updateDataModel":{"surfaceId":"home","path":"/","value":{"rows":[[{"id":"a","title":"A"},{"id":"b","title":"B"}],[{"id":"c","title":"C"},{"id":"d","title":"D"}]]}}}
])");
  View view(store);
  FakeCanvas canvas;
  std::vector<UserAction> actions;
  view.onAction = [&](const UserAction& a) { actions.push_back(a); };
  view.draw(canvas);
  auto keys = view.focusables();
  CHECK(keys.size() == 5 && std::set<std::string>(keys.begin(), keys.end()).size() == 5);
  view.press(Button::Down);
  view.press(Button::Right);
  view.draw(canvas);
  view.press(Button::Cross);
  CHECK(actions.size() == 1 && actions[0].context["id"] == "d");
}

static void testPasswordPromptAndConfirm() {
  Store store;
  applyAll(store, R"([
{"version":"1.0","createSurface":{"surfaceId":"pw","catalogId":"ps5"}},
{"version":"1.0","updateComponents":{"surfaceId":"pw","components":[{"id":"root","component":"TextPrompt","title":"Password for Mini-5G","kind":"password",
  "value":{"@path":"/answer"},"submit":{"event":{"name":"text_submit","context":{"value":{"@path":"/answer"}}}}}]}}
])");
  View view(store);
  FakeCanvas canvas;
  std::vector<UserAction> actions;
  view.onAction = [&](const UserAction& a) { actions.push_back(a); };
  view.draw(canvas);
  view.type("hunter2");
  view.erase();
  view.type("3");
  view.press(Button::Cross);
  CHECK(actions.size() == 1 && actions[0].context["value"] == "hunter3");
  // Answered: the dialog goes away now, not when the agent's next screen arrives.
  CHECK(store.surface("pw") == nullptr && store.top() == nullptr);
  view.press(Button::Cross);  // nothing stale left to fire before the next draw
  CHECK(actions.size() == 1);

  // A destructive confirm starts on Cancel; Circle cancels (and dismisses it).
  const char* confirm = R"([
{"version":"1.0","createSurface":{"surfaceId":"del","catalogId":"ps5"}},
{"version":"1.0","updateComponents":{"surfaceId":"del","components":[{"id":"root","component":"Confirm","message":"Delete?","destructive":true,
  "confirm":{"event":{"name":"confirm_result","context":{"confirmed":true}}},"cancel":{"event":{"name":"confirm_result","context":{"confirmed":false}}}}]}}
])";
  applyAll(store, confirm);
  view.draw(canvas);
  CHECK(view.focused() == "root/cancel");
  view.press(Button::Circle);
  CHECK(actions.size() == 2 && actions[1].context["confirmed"] == false && store.top() == nullptr);
  applyAll(store, confirm);
  view.draw(canvas);
  view.press(Button::Right);
  view.draw(canvas);
  view.press(Button::Cross);
  CHECK(actions.size() == 3 && actions[2].context["confirmed"] == true);
}

static void testHaptics() {
  // Named effects exist and stay short; unknown names give nothing.
  for (const auto& name : hapticEffects()) {
    auto steps = hapticEffect(name);
    int total = 0;
    for (const auto& st : steps) total += st.ms;
    CHECK(!steps.empty() && total < 600);
  }
  CHECK(hapticEffect("party").empty());

  // Calls become steps; patterns are held to the bounds.
  std::string problem;
  auto s = hapticSteps(json::parse(R"({"@call":"haptic","args":{"effect":"success"}})"));
  CHECK(s && s->size() == 3);
  s = hapticSteps(json::parse(R"({"@call":"haptic","args":{"effect":"party"}})"), &problem);
  CHECK(s && s->empty() && !problem.empty());
  s = hapticSteps(json::parse(R"({"@call":"rumble","args":{"intensity":1,"durationMs":5000}})"));
  CHECK(s && s->size() == 1 && (*s)[0].ms == 5000 && (*s)[0].low == 1);
  s = hapticSteps(json::parse(R"({"@call":"hapticPattern","args":{"steps":[{"low":2,"high":0.5,"ms":8000},{"high":1,"ms":8000},{"ms":100}]}})"));
  CHECK(s && s->size() == 2 && (*s)[0].low == 1 && (*s)[1].ms == 2000);  // clamped, and stopped at 10 s
  json many = {{"@call", "hapticPattern"}, {"args", {{"steps", json::array()}}}};
  for (int i = 0; i < 17; ++i) many["args"]["steps"].push_back({{"ms", 10}});
  CHECK(hapticSteps(many, &problem)->empty());
  CHECK(!hapticSteps(json::parse(R"({"@call":"lightbar","args":{"color":"#ff0000"}})")));

  // The player steps through, refreshes a long step, and ends with the motors off.
  HapticPlayer player;
  using ms = std::chrono::milliseconds;
  auto t0 = HapticPlayer::Clock::now();
  player.play({{0.5f, 0.5f, 100}, {0, 0, 50}, {1, 1, 3000}}, t0);
  auto o = player.update(t0);
  CHECK(o && o->low == 0.5f && o->ms == 100);
  CHECK(!player.update(t0 + ms(20)));
  o = player.update(t0 + ms(110));
  CHECK(o && o->low == 0 && o->ms == 40);
  o = player.update(t0 + ms(160));
  CHECK(o && o->low == 1 && o->ms == 2990);
  CHECK(!player.update(t0 + ms(900)));
  o = player.update(t0 + ms(1200));
  CHECK(o && o->low == 1 && o->ms == 1950);  // sent again after a second
  o = player.update(t0 + ms(3200));
  CHECK(o && o->low == 0 && o->high == 0 && !player.playing());
  CHECK(!player.update(t0 + ms(3300)));
  player.play({{1, 1, 5000}}, t0);
  player.update(t0);
  player.stop();
  o = player.update(t0 + ms(10));
  CHECK(o && o->low == 0 && !player.update(t0 + ms(20)));

  // A screen's haptic plays when its action runs, before the event goes to the agent.
  Store store;
  applyAll(store, R"([
{"version":"1.0","createSurface":{"surfaceId":"q","catalogId":"ps5"}},
{"version":"1.0","updateComponents":{"surfaceId":"q","components":[{"id":"root","component":"Confirm","message":"Install the update?","confirmHaptic":"success","cancelHaptic":"nope",
  "confirm":{"event":{"name":"confirm_result","context":{"confirmed":true}}},"cancel":{"event":{"name":"confirm_result","context":{"confirmed":false}}}}]}}
])");
  View view(store);
  FakeCanvas canvas;
  std::vector<std::string> order;
  view.onAction = [&](const UserAction& a) { order.push_back("event " + a.name); };
  view.onFunction = [&](const json& f) { order.push_back(f["@call"].get<std::string>() + " " + f["args"].value("effect", "")); };
  view.draw(canvas);
  view.press(Button::Cross);  // not destructive: focus starts on OK
  CHECK((order == std::vector<std::string>{"haptic success", "event confirm_result"}));
  applyAll(store, R"([{"version":"1.0","createSurface":{"surfaceId":"q2","catalogId":"ps5"}},
{"version":"1.0","updateComponents":{"surfaceId":"q2","components":[{"id":"root","component":"Confirm","message":"Again?","cancelHaptic":"nope",
  "confirm":{"event":{"name":"confirm_result","context":{"confirmed":true}}},"cancel":{"event":{"name":"confirm_result","context":{"confirmed":false}}}}]}}])");
  view.draw(canvas);
  view.press(Button::Circle);
  CHECK(order.size() == 3 && order[2] == "event confirm_result");  // an unknown effect is ignored
}

static void testPages() {
  Store store;
  applyAll(store, R"([
{"version":"1.0","createSurface":{"surfaceId":"set","catalogId":"ps5"}},
{"version":"1.0","updateComponents":{"surfaceId":"set","components":[
  {"id":"root","component":"Screen","title":"Settings","children":["pages"]},
  {"id":"pages","component":"Pages","pages":[{"title":"Model","child":"p0"},{"title":"Voice","child":"p1"}]},
  {"id":"p0","component":"Menu","children":["m"]},
  {"id":"m","component":"MenuItem","label":"Qwen","action":{"event":{"name":"x"}}},
  {"id":"p1","component":"Menu","children":["v"]},
  {"id":"v","component":"MenuItem","label":"Voice","action":{"event":{"name":"y"}}}]}}
])");
  View view(store);
  FakeCanvas canvas;
  view.draw(canvas);
  CHECK((view.focusables() == std::vector<std::string>{"m", View::kComposerKey}));
  view.press(Button::R1);
  view.draw(canvas);
  CHECK((view.focusables() == std::vector<std::string>{"v", View::kComposerKey}));
  view.press(Button::L1);
  view.draw(canvas);
  CHECK((view.focusables() == std::vector<std::string>{"m", View::kComposerKey}));
}

static void testSseParser() {
  SseParser p;
  // Split across chunks, CRLF on one event, a comment line, and an invalid payload.
  auto a = p.feed("data: {\"type\":\"RUN_STA");
  CHECK(a.empty());
  auto b = p.feed("RTED\"}\n\n: keep-alive\n\ndata: {\"type\":\"RUN_FINISHED\"}\r\n\r\ndata: not json\n\n");
  CHECK(b.size() == 2 && b[0]["type"] == "RUN_STARTED" && b[1]["type"] == "RUN_FINISHED");
}

static void testSessionLifecycle() {
  Store store;
  Session session(store);
  auto ev = [](const char* s) { return json::parse(s); };
  session.handle(ev(R"({"type":"RUN_STARTED"})"));
  CHECK(session.running() && session.status() == "Thinking…");
  session.handle(ev(R"({"type":"CUSTOM","name":"status","value":{"state":"retrying","retryInSeconds":30}})"));
  CHECK(session.status() == "Busy, retrying in 30 s");
  session.handle(ev(R"({"type":"TEXT_MESSAGE_START","messageId":"m"})"));
  session.handle(ev(R"({"type":"TEXT_MESSAGE_CONTENT","messageId":"m","delta":"Pick one."})"));
  session.handle(ev(R"({"type":"ACTIVITY_SNAPSHOT","activityType":"a2ui-surface","content":{"a2ui_operations":[
    {"version":"1.0","createSurface":{"surfaceId":"s1","catalogId":"ps5"}},{"version":"1.0","updateComponents":{"surfaceId":"s1","components":[{"id":"root","component":"Text","text":"x"}]}}]}})"));
  session.handle(ev(R"({"type":"RUN_FINISHED"})"));
  CHECK(!session.running() && session.caption() == "Pick one." && store.top() && store.top()->id == "s1");

  // A text-only answer clears the stage; the conversation keeps both answers, and notices.
  session.addUser("Rename my profile");
  session.handle(ev(R"({"type":"RUN_STARTED"})"));
  session.handle(ev(R"({"type":"TEXT_MESSAGE_CONTENT","messageId":"n","delta":"What should I call it?"})"));
  session.handle(ev(R"({"type":"RUN_FINISHED"})"));
  CHECK(store.top() == nullptr);
  session.handle(ev(R"({"type":"RUN_STARTED"})"));
  session.handle(ev(R"({"type":"ACTIVITY_SNAPSHOT","activityType":"a2ui-surface","content":{"a2ui_operations":[
    {"version":"1.0","createSurface":{"surfaceId":"s2","catalogId":"ps5"}},{"version":"1.0","updateComponents":{"surfaceId":"s2","components":[{"id":"root","component":"Notice","text":"Renamed","icon":"success"}]}}]}})"));
  session.handle(ev(R"({"type":"RUN_FINISHED"})"));
  const auto& t = session.transcript();
  // The screen is the agent's reply, in order: after its text, before the user's next message.
  CHECK(t.size() == 5);
  if (t.size() == 5) {
    CHECK(t[0].role == ChatLine::Role::Assistant && t[0].text == "Pick one.");
    CHECK(t[1].role == ChatLine::Role::Surface && t[1].surfaceId == "s1");
    CHECK(t[2].role == ChatLine::Role::User && t[2].text == "Rename my profile");
    CHECK(t[3].text == "What should I call it?");
    CHECK(t[4].role == ChatLine::Role::Notice && t[4].text == "Renamed" && t[4].icon == "success");
  }

  // Controller feedback: queued for the host, taken once; malformed calls are dropped.
  session.handle(ev(R"({"type":"ACTIVITY_SNAPSHOT","messageId":"genui-fn:f1","activityType":"generative-ui-function-call","replace":true,
    "content":{"functionCallId":"f1","callFunction":{"@call":"rumble","args":{"intensity":0.4,"durationMs":120},"catalogId":"ps5"}}})"));
  session.handle(ev(R"({"type":"ACTIVITY_SNAPSHOT","messageId":"genui-fn:f2","activityType":"generative-ui-function-call","content":{"functionCallId":"f2","callFunction":{"nope":1}}})"));
  auto calls = session.takeFunctionCalls();
  CHECK(calls.size() == 1 && calls[0]["@call"] == "rumble" && session.takeFunctionCalls().empty());

  // The agent's name for the header: kept; malformed ignored; long names cut on a character.
  CHECK(session.agentName().empty());
  session.handle(ev(R"({"type":"CUSTOM","name":"ps5.agent","value":{"name":"OpenDots"}})"));
  session.handle(ev(R"({"type":"CUSTOM","name":"ps5.agent","value":{"name":7}})"));
  CHECK(session.agentName() == "OpenDots");
  session.handle(json{{"type", "CUSTOM"}, {"name", "ps5.agent"}, {"value", {{"name", std::string(39, 'a') + "\xC3\xA9z"}}}});
  CHECK(session.agentName() == std::string(39, 'a'));

  session.handle(ev(R"({"type":"RUN_STARTED"})"));
  session.handle(ev(R"({"type":"RUN_ERROR","message":"Can't reach the agent"})"));
  CHECK(!session.running() && session.error() == "Can't reach the agent");
}

static void testMessageBar() {
  // Home: the message bar has focus, so typing goes straight into it.
  Store store;
  View view(store);
  FakeCanvas canvas;
  std::vector<std::string> prompts;
  bool accept = true;
  view.onPrompt = [&](const std::string& t) { prompts.push_back(t); return accept; };
  view.draw(canvas);
  CHECK(view.keyboardOpen() && view.focused() == View::kComposerKey);
  view.type("hi");
  view.erase();
  view.type("ello");
  accept = false;  // busy: the draft stays
  view.press(Button::Cross);
  accept = true;
  view.press(Button::Cross);
  CHECK((prompts == std::vector<std::string>{"hello", "hello"}));
  view.press(Button::Cross);  // empty bar: nothing to send
  CHECK(prompts.size() == 2);

  // Up from the bar reaches the suggestions; Cross sends one.
  view.press(Button::Up);
  view.draw(canvas);
  CHECK(view.focused().rfind("suggest#", 0) == 0 && !view.keyboardOpen());
  view.press(Button::Cross);
  CHECK(prompts.size() == 3);

  // With a screen up, its items come first; Options jumps to the bar, Circle clears a draft.
  applyAll(store, kMenu);
  view.draw(canvas);
  CHECK(view.focused() == "a");
  view.press(Button::Options);
  view.draw(canvas);
  CHECK(view.focused() == View::kComposerKey && view.keyboardOpen());
  view.type("draft");
  view.press(Button::Circle);
  view.press(Button::Cross);
  CHECK(prompts.size() == 3);
}

static void testSystemKeyboard() {
  // Console: Cross on a text field asks for the system keyboard; its text is then submitted.
  Store store;
  applyAll(store, R"([
{"version":"1.0","createSurface":{"surfaceId":"pw","catalogId":"ps5"}},
{"version":"1.0","updateComponents":{"surfaceId":"pw","components":[{"id":"root","component":"TextPrompt","title":"Wi-Fi password","kind":"password","value":{"@path":"/answer"},
  "submit":{"event":{"name":"text_submit","context":{"value":{"@path":"/answer"}}}}}]}}
])");
  View view(store);
  FakeCanvas canvas;
  std::vector<View::KeyboardRequest> asked;
  std::vector<UserAction> actions;
  view.onKeyboard = [&](const View::KeyboardRequest& r) { asked.push_back(r); };
  view.onAction = [&](const UserAction& a) { actions.push_back(a); };
  view.draw(canvas);
  view.press(Button::Cross);
  CHECK(asked.size() == 1 && asked[0].title == "Wi-Fi password" && asked[0].password && actions.empty());
  view.submitText("hunter2");
  CHECK(actions.size() == 1 && actions[0].context["value"] == "hunter2" && actions[0].label.find("hunter2") == std::string::npos);

  // The message bar asks too, and submits as a prompt.
  store.clear();
  std::vector<std::string> prompts;
  view.onPrompt = [&](const std::string& p) { prompts.push_back(p); return true; };
  view.draw(canvas);
  view.press(Button::Cross);
  CHECK(asked.size() == 2 && !asked[1].password);
  view.submitText("What can I play?");
  CHECK((prompts == std::vector<std::string>{"What can I play?"}));

  // Options on the bar opens the keyboard too (it used to do nothing there).
  view.draw(canvas);
  view.press(Button::Options);
  CHECK(asked.size() == 3 && !asked[2].password);
}

static void testConversationHistory() {
  // Two screens, then a text-only answer: the earlier screens become chips that reopen in place.
  Store store;
  Session session(store);
  View view(store);
  FakeCanvas canvas;
  std::vector<UserAction> actions;
  std::vector<std::string> local;
  view.onAction = [&](const UserAction& a) { actions.push_back(a); };
  view.onLocal = [&](const std::string& e) { local.push_back(e); };
  auto ev = [](const std::string& s) { return json::parse(s); };
  auto screen = [&](const std::string& id, const std::string& title, const std::string& item) {
    session.handle(ev(R"({"type":"RUN_STARTED"})"));
    session.handle(ev(R"({"type":"ACTIVITY_SNAPSHOT","activityType":"a2ui-surface","content":{"a2ui_operations":[{"version":"1.0","createSurface":{"surfaceId":")" + id +
                      R"(","catalogId":"ps5"}},{"version":"1.0","updateComponents":{"surfaceId":")" + id + R"(","components":[{"id":"root","component":"Screen","title":")" + title +
                      R"(","children":["menu"]},{"id":"menu","component":"Menu","children":[")" + item + R"("]},{"id":")" + item +
                      R"(","component":"MenuItem","label":"Pick","action":{"event":{"name":"pick"}}}]}}]}})"));
    session.handle(ev(R"({"type":"RUN_FINISHED"})"));
  };
  auto frame = [&] {
    view.setTranscript(session.transcript());
    view.draw(canvas);
  };
  screen("games", "Games", "g1");
  session.addUser("Show my saves");
  screen("saves", "Saves", "s1");
  frame();
  auto keys = view.focusables();
  CHECK(std::find(keys.begin(), keys.end(), "chip:games") != keys.end());
  CHECK(view.focused() == "s1" && !view.scrolledBack());

  // Up from the live screen reaches the chip; Cross reopens it, focused on its first item.
  for (int i = 0; i < 4 && view.focused() != "chip:games"; ++i) {
    view.press(Button::Up);
    frame();
  }
  CHECK(view.focused() == "chip:games");
  view.press(Button::Cross);
  frame();
  CHECK(view.scrolledBack() && view.focused() == "g1");
  keys = view.focusables();
  CHECK(std::find(keys.begin(), keys.end(), "chip:saves") == keys.end());  // newer items are hidden below
  view.press(Button::Cross);
  CHECK(actions.size() == 1 && actions[0].surfaceId == "games");  // its actions still work
  // Circle returns to the latest without telling the agent "back".
  view.press(Button::Circle);
  frame();
  CHECK(!view.scrolledBack() && view.focused() == "s1" && local.empty());

  // A text-only answer empties the stage but the screens stay reopenable.
  session.addUser("Thanks");
  session.handle(ev(R"({"type":"RUN_STARTED"})"));
  session.handle(ev(R"({"type":"TEXT_MESSAGE_CONTENT","messageId":"t","delta":"You're welcome."})"));
  session.handle(ev(R"({"type":"RUN_FINISHED"})"));
  frame();
  CHECK(store.top() == nullptr && store.surface("games") && store.surface("saves"));
  keys = view.focusables();
  CHECK(std::find(keys.begin(), keys.end(), "chip:saves") != keys.end());

  // Nothing to scroll while it all fits; once older messages are hidden above, it scrolls back,
  // and a new message returns to the latest.
  view.scroll(2);
  frame();
  CHECK(!view.scrolledBack());
  for (int i = 0; i < 20; ++i) session.addUser("Message " + std::to_string(i));
  frame();
  view.scroll(2);
  frame();
  CHECK(view.scrolledBack());
  // Smooth scrolling (the stick): by pixels, stopping at the latest and at the oldest message.
  view.backToLatest();
  frame();
  view.scrollBy(-50);
  frame();
  CHECK(!view.scrolledBack());
  view.scrollBy(37);
  frame();
  CHECK(view.scrolledBack());
  keys = view.focusables();
  view.scrollBy(1e6);
  frame();
  CHECK(view.scrolledBack());
  view.scroll(1);  // already at the oldest: stays
  frame();
  CHECK(view.scrolledBack());
  view.scrollBy(-1e6);
  frame();
  CHECK(!view.scrolledBack());
  view.scrollBy(37);
  frame();
  session.addUser("One more thing");
  frame();
  CHECK(!view.scrolledBack());
}

/** The agent's follow-up suggestions, starting over with Triangle, and a screen giving way to the user's next message. */
static void testFollowUpsAndNewConversation() {
  Store store;
  Session session(store);
  View view(store);
  FakeCanvas canvas;
  std::vector<std::string> prompts;
  int restarts = 0;
  view.onPrompt = [&](const std::string& t) { prompts.push_back(t); return true; };
  auto ev = [](const std::string& s) { return json::parse(s); };
  auto frame = [&] {
    view.setTranscript(session.transcript());
    view.setFollowUps(session.followUps());
    view.setStatus(session.status());
    view.draw(canvas);
  };
  auto has = [&](const std::string& key) { auto k = view.focusables(); return std::find(k.begin(), k.end(), key) != k.end(); };

  // A screen is drawn in full until the user says something after it: then it's a chip, so a
  // tall screen can't cover their message.
  session.handle(ev(R"({"type":"RUN_STARTED"})"));
  session.handle(ev(R"({"type":"ACTIVITY_SNAPSHOT","activityType":"generative-ui-surface","content":{"surfaceId":"games","operations":[{"version":"1.0","createSurface":{"surfaceId":"games","catalogId":"ps5","components":[{"id":"root","component":"Screen","title":"Games","children":["menu"]},{"id":"menu","component":"Menu","children":["g1"]},{"id":"g1","component":"MenuItem","label":"Astro Bot","action":{"event":{"name":"pick"}}}]}}]}})"));
  // Follow-ups: at most three, kept after the run, shown as buttons that send them.
  session.handle(ev(R"({"type":"CUSTOM","name":"ps5.suggestions","value":{"prompts":["What did I play?","Show my saves","",42,"How much space?","Fourth"]}})"));
  session.handle(ev(R"({"type":"RUN_FINISHED"})"));
  CHECK(session.followUps().size() == 3 && session.followUps()[2] == "How much space?");
  frame();
  CHECK(has("g1") && has("followup#0") && has("followup#2") && !has("chip:games"));
  for (int i = 0; i < 6 && view.focused() != "followup#1"; ++i) {
    view.press(view.focused().rfind("followup#", 0) == 0 ? Button::Right : Button::Up);
    frame();
  }
  CHECK(view.focused() == "followup#1");
  view.press(Button::Cross);
  CHECK(prompts.size() == 1 && prompts[0] == "Show my saves");
  session.addUser(prompts[0]);
  CHECK(session.followUps().empty());
  frame();
  CHECK(has("chip:games") && !has("g1") && !has("followup#0"));
  // A new run clears them too.
  session.handle(ev(R"({"type":"RUN_STARTED"})"));
  session.handle(ev(R"({"type":"CUSTOM","name":"ps5.suggestions","value":{"prompts":["Again"]}})"));
  session.handle(ev(R"({"type":"RUN_FINISHED"})"));
  session.handle(ev(R"({"type":"RUN_STARTED"})"));
  CHECK(session.followUps().empty());
  session.handle(ev(R"({"type":"RUN_FINISHED"})"));

  // Triangle starts over, only where the host can and there's a conversation to leave.
  view.press(Button::Triangle);
  CHECK(restarts == 0);
  view.onNewConversation = [&] {
    ++restarts;
    session.clear();
    store.clear();
  };
  frame();
  view.press(Button::Triangle);
  CHECK(restarts == 1 && session.transcript().empty() && !store.surface("games"));
  frame();
  CHECK(view.focused().rfind("suggest#", 0) == 0 || view.focused() == View::kComposerKey);  // the first screen again
  view.press(Button::Triangle);
  CHECK(restarts == 1);  // nothing to leave
}

static void testNativeDialogs() {
  // With onConfirm set, a Confirm is handed to the platform once and answered via answerConfirm.
  Store store;
  const char* confirm = R"([
{"version":"1.0","createSurface":{"surfaceId":"approval-1","catalogId":"ps5"}},
{"version":"1.0","updateComponents":{"surfaceId":"approval-1","components":[{"id":"root","component":"Confirm","message":"Delete the save?","destructive":true,
  "confirmLabel":"Delete","cancelLabel":"Keep",
  "confirm":{"event":{"name":"approval_result","context":{"confirmed":true}}},"cancel":{"event":{"name":"approval_result","context":{"confirmed":false}}}}]}}
])";
  applyAll(store, confirm);
  View view(store);
  FakeCanvas canvas;
  std::vector<View::ConfirmRequest> asked;
  std::vector<UserAction> actions;
  view.onConfirm = [&](const View::ConfirmRequest& r) { asked.push_back(r); };
  view.onAction = [&](const UserAction& a) { actions.push_back(a); };
  view.draw(canvas);
  view.draw(canvas);
  CHECK(asked.size() == 1 && asked[0].message == "Delete the save?" && asked[0].confirmLabel == "Delete" && asked[0].cancelLabel == "Keep" && asked[0].destructive);
  CHECK(view.confirmPending());
  const auto keys = view.focusables();
  CHECK(std::find(keys.begin(), keys.end(), "root/confirm") == keys.end());  // not drawn
  view.answerConfirm(false);
  CHECK(actions.size() == 1 && actions[0].context["confirmed"] == false && actions[0].label == "Keep");
  CHECK(!view.confirmPending() && store.top() == nullptr);

  // The agent removing the surface ends the wish for a confirm.
  applyAll(store, R"([{"version":"1.0","createSurface":{"surfaceId":"approval-2","catalogId":"ps5"}},
{"version":"1.0","updateComponents":{"surfaceId":"approval-2","components":[{"id":"root","component":"Confirm","message":"Close the game?","confirm":{"event":{"name":"ok"}}}]}}])");
  view.draw(canvas);
  CHECK(asked.size() == 2 && view.confirmPending());
  applyAll(store, R"([{"version":"1.0","deleteSurface":{"surfaceId":"approval-2"}}])");
  CHECK(!view.confirmPending());

  // Notices go to onNotice once instead of being drawn.
  std::vector<std::string> notices;
  view.onNotice = [&](const std::string& text, const std::string& icon) { notices.push_back(icon + ":" + text); };
  applyAll(store, R"([{"version":"1.0","createSurface":{"surfaceId":"n1","catalogId":"ps5"}},
{"version":"1.0","updateComponents":{"surfaceId":"n1","components":[{"id":"root","component":"Notice","text":"Connected","icon":"success"}]}}])");
  view.draw(canvas);
  view.draw(canvas);
  CHECK((notices == std::vector<std::string>{"success:Connected"}));
}

static void testToolProgress() {
  Store store;
  Session session(store);
  session.handle(json::parse(R"({"type":"RUN_STARTED"})"));
  session.handle(json::parse(R"({"type":"CUSTOM","name":"status","value":{"state":"working","tool":"load_model"}})"));
  CHECK(session.progress() < 0);
  session.handle(json::parse(R"({"type":"CUSTOM","name":"status","value":{"state":"working","tool":"load_model","detail":"Loading Qwen 9B","progress":0.4}})"));
  CHECK(session.progress() > 0.39f && session.progress() < 0.41f && session.progressLabel() == "Loading Qwen 9B");
  CHECK(session.status() == "Loading Qwen 9B… 40%");
  session.handle(json::parse(R"({"type":"CUSTOM","name":"status","value":{"state":"thinking"}})"));
  CHECK(session.progress() < 0 && session.progressLabel().empty());
  session.handle(json::parse(R"({"type":"CUSTOM","name":"status","value":{"state":"working","detail":"x","progress":1}})"));
  session.handle(json::parse(R"({"type":"RUN_FINISHED"})"));
  CHECK(session.progress() < 0);
}

static void testNarrationIsNotTheCaption() {
  Store store;
  Session session(store);
  for (const char* e : {R"({"type":"RUN_STARTED"})", R"({"type":"TEXT_MESSAGE_START"})", R"({"type":"TEXT_MESSAGE_CONTENT","delta":"I'll check your saves."})",
                        R"({"type":"CUSTOM","name":"status","value":{"state":"working","tool":"list_saves"}})"})
    session.handle(json::parse(e));
  CHECK(session.caption().empty());
  session.handle(json::parse(R"({"type":"TEXT_MESSAGE_CONTENT","delta":"You have 2 saves."})"));
  session.handle(json::parse(R"({"type":"RUN_FINISHED"})"));
  CHECK(session.caption() == "You have 2 saves.");
}

// ---------- key setup (Pairing) ----------

/** POST /pair to a local Pairing the way the page does; returns the HTTP status. */
int pushKey(int port, const std::string& consolePkB64, const json& message) {
  unsigned char pk[32], sk[32], nonce[24];
  crypto_box_keypair(pk, sk);
  randombytes(nonce, sizeof nonce);
  std::string consolePk = *base64Decode(consolePkB64);
  std::string m(32, '\0');
  m += message.dump();
  std::string c(m.size(), '\0');
  crypto_box(reinterpret_cast<unsigned char*>(c.data()), reinterpret_cast<const unsigned char*>(m.data()), m.size(), nonce,
             reinterpret_cast<const unsigned char*>(consolePk.data()), sk);
  std::string body = json{{"pk", base64Encode(pk, 32)}, {"nonce", base64Encode(nonce, 24)}, {"box", base64Encode(reinterpret_cast<const unsigned char*>(c.data()) + 16, c.size() - 16)}}.dump();
  std::string req = "POST /pair HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(uint16_t(port));
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  timeval tv{3, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) { close(fd); return -1; }
  send(fd, req.data(), req.size(), 0);
  char buf[512] = {};
  ssize_t n = recv(fd, buf, sizeof buf - 1, 0);
  close(fd);
  return n > 12 ? std::atoi(buf + 9) : -1;
}

void testPairing() {
  // Credentials: checked fields, defaults per provider, what the runtime gets.
  CHECK(!Credentials::fromJson(json{{"provider", "minimax"}, {"key", "has space"}}));
  CHECK(!Credentials::fromJson(json{{"provider", "other"}, {"key", "k"}}));
  CHECK(!Credentials::fromJson(json{{"provider", "server"}, {"key", "k"}, {"url", "ftp://x"}}));
  auto or_ = Credentials::fromJson(json{{"provider", "openrouter"}, {"key", "sk-or-1"}});
  CHECK(or_ && or_->modelOrDefault() == "minimax/minimax-m3" && or_->env().at("LLM_BASE_URL") == "https://openrouter.ai/api/v1" && or_->env().at("LLM_API_KEY") == "sk-or-1");
  auto mm = Credentials::fromJson(json{{"provider", "minimax"}, {"key", "mm-1"}});
  CHECK(mm && mm->modelOrDefault() == "MiniMax-M3" && mm->env().at("LLM_BASE_URL") == "https://api.minimax.io/v1" && mm->env().at("LLM_API_KEY") == "mm-1");
  auto srv = Credentials::fromJson(json{{"provider", "server"}, {"key", "token"}, {"url", "http://192.168.0.2:8790/agent"}});
  CHECK(srv && srv->label() == "192.168.0.2:8790" && srv->env().empty());
  CHECK(base64Encode(reinterpret_cast<const unsigned char*>("hello!?"), 7) == "aGVsbG8hPw==" && *base64Decode("aGVsbG8hPw==") == "hello!?");
  CHECK(!base64Decode("a$=="));

  Pairing::Options po;
  po.port = 18791;
  po.page = "<p>{{PK}}</p>";
  Pairing pairing(po);
  const std::string code = pairing.code(), pk = pairing.publicKey();
  CHECK(code.size() == 6 && code.find_first_not_of("0123456789") == std::string::npos);
  json good = {{"code", code}, {"provider", "minimax"}, {"key", "mm-key"}, {"model", ""}, {"url", ""}};
  json wrong = good;
  wrong["code"] = code == "000000" ? "111111" : "000000";
  CHECK(pushKey(po.port, pk, wrong) == 403 && pairing.status().find("4 tries left") != std::string::npos);
  json bad = good;
  bad["key"] = "";
  CHECK(pushKey(po.port, pk, bad) == 400);  // a fixable mistake, not a wrong try
  CHECK(!pairing.poll());
  // Five wrong tries in all: a new code and key pair, and the old key no longer opens.
  for (int i = 0; i < 4; ++i) pushKey(po.port, pk, wrong);
  CHECK(pairing.code() != code || pairing.publicKey() != pk);
  CHECK(pushKey(po.port, pk, good) == 403);
  good["code"] = pairing.code();
  CHECK(pushKey(po.port, pairing.publicKey(), good) == 200);
  auto creds = pairing.poll();
  CHECK(creds && creds->provider == "minimax" && creds->key == "mm-key");
  // One key per session: the server has stopped.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  CHECK(pushKey(po.port, pairing.publicKey(), good) == -1);

  const std::string path = "pairing-test-credentials.json";
  CHECK(creds->save(path));
  auto loaded = Credentials::load(path);
  CHECK(loaded && loaded->key == "mm-key");
  unlink(path.c_str());
}

/** Titles from a stand-in app.db: the table is found by its columns, empty per-user tables are
 * skipped, system apps and bad ids are left out, installed ones told apart; without a database, appmeta. */
static void testInstalledTitles() {
  const std::string dir = "ps5sys-test";
  const std::string db = dir + "/app.db", meta = dir + "/appmeta";
  std::system(("rm -rf " + dir).c_str());
  mkdir(dir.c_str(), 0755);
  mkdir(meta.c_str(), 0755);
  mkdir((meta + "/PPSA01234").c_str(), 0755);
  std::ofstream(meta + "/PPSA01234/param.json") << R"({"localizedParameters":{"defaultLanguage":"fr-FR","fr-FR":{"titleName":"Jeu Un"},"en-US":{"titleName":"Game One"}}})";

  sqlite3* h = nullptr;
  CHECK(sqlite3_open(db.c_str(), &h) == SQLITE_OK);
  sqlite3_exec(h,
               "CREATE TABLE tbl_iconinfo(iconId, path);"
               "CREATE TABLE tbl_info_10000000(titleId TEXT, titleName TEXT, metaDataPath TEXT);"
               "CREATE TABLE tbl_info_20000000(titleId TEXT, titleName TEXT, metaDataPath TEXT);"
               "INSERT INTO tbl_info_20000000 VALUES('PPSA01234', 'Astro Bot', ''), ('CUSA00127', 'Netflix', ''),"
               " ('NPXS40028', 'Settings', ''), ('bad', 'Nope', ''), ('PPSA01234', 'Astro Bot', ''), ('PPSA05678', NULL, '');",
               nullptr, nullptr, nullptr);
  sqlite3_close(h);

  const std::string noApps = dir + "/no-user-app";
  auto t = ps5sys::consoleTitles(db, meta, noApps);
  CHECK(t.source == "app.db" && t.installedBy.empty());
  CHECK(t.schema == "tbl_info_20000000(titleId, titleName, metaDataPath)");
  CHECK(t.titles.size() == 3);
  if (t.titles.size() == 3) {
    CHECK(t.titles[0].id == "CUSA00127" && t.titles[0].name == "Netflix" && t.titles[0].platform == "PS4");
    CHECK(t.titles[1].id == "PPSA01234" && t.titles[1].name == "Astro Bot" && t.titles[1].platform == "PS5");
    CHECK(t.titles[2].id == "PPSA05678" && t.titles[2].name == "PPSA05678");  // no name anywhere: the id
  }
  auto j = ps5sys::toJson(t);
  CHECK(j["games"].size() == 3 && j["games"][1]["title"] == "Astro Bot" && j["source"] == "console" && j["notInstalled"] == 0);

  // The 12.70 shape: the account's whole library, the per-user iconinfo table preferred over the
  // others; installed = a folder in /user/app, else installStatus 2.
  CHECK(sqlite3_open(db.c_str(), &h) == SQLITE_OK);
  sqlite3_exec(h,
               "CREATE TABLE tbl_iconinfo_30000000(titleId TEXT, titleName TEXT, installStatus INTEGER, playedTimeOnConsole INTEGER,"
               " gltTotalPlayedTime INTEGER, lastPlayedDate TEXT);"
               "INSERT INTO tbl_iconinfo_30000000 VALUES"
               " ('PPSA22327', 'Forza Horizon 5', 2, 60922, NULL, '2026-11-07 19:28:57.136'),"
               " ('PPSA03974', 'Cyberpunk 2077', 0, 0, 440272, '2025-07-01 06:16:48.760'),"
               " ('CUSA18659', 'Crysis Remastered', 2, NULL, NULL, '');",
               nullptr, nullptr, nullptr);
  sqlite3_close(h);
  t = ps5sys::consoleTitles(db, meta, noApps);
  CHECK(t.schema.rfind("tbl_iconinfo_30000000(", 0) == 0 && t.installedBy == "installStatus" && t.titles.size() == 3);
  j = ps5sys::toJson(t);
  CHECK(j["games"].size() == 2 && j["notInstalled"] == 1);
  if (j["games"].size() == 2) {
    CHECK(j["games"][0]["id"] == "CUSA18659" && !j["games"][0].contains("hoursPlayed") && !j["games"][0].contains("lastPlayed"));
    CHECK(j["games"][1]["hoursPlayed"] == 16.9 && j["games"][1]["lastPlayed"] == "2026-11-07");
  }
  mkdir((dir + "/app").c_str(), 0755);
  mkdir((dir + "/app/PPSA03974").c_str(), 0755);  // the folder decides when it can be listed
  t = ps5sys::consoleTitles(db, meta, dir + "/app");
  j = ps5sys::toJson(t);
  CHECK(t.installedBy == dir + "/app" && j["games"].size() == 1 && j["games"][0]["id"] == "PPSA03974" && j["notInstalled"] == 2);
  sqlite3_open(db.c_str(), &h);
  sqlite3_exec(h, "DROP TABLE tbl_iconinfo_30000000;", nullptr, nullptr, nullptr);
  sqlite3_close(h);

  // No database: the appmeta folders, named from param.json in the default language.
  std::remove(db.c_str());
  t = ps5sys::consoleTitles(db, meta, noApps);
  CHECK(t.source == "appmeta" && t.titles.size() == 1 && t.titles[0].name == "Jeu Un");

  // Nothing at all: an error the agent can pass on.
  t = ps5sys::consoleTitles(db, dir + "/nowhere", noApps);
  CHECK(t.titles.empty() && t.error == "can't see " + db);
  CHECK(ps5sys::toJson(t)["error"] == "titles_unreadable");
  std::system(("rm -rf " + dir).c_str());
}

/** System facts off the console: uptime and storage read, the PS5-only parts left out. */
static void testSystemInfo() {
  auto info = ps5sys::systemInfo("/");
  CHECK(info.contains("uptimeMinutes") && info["uptimeMinutes"].get<long>() >= 0);
  CHECK(info.contains("storage") && info["storage"]["totalGB"].get<double>() > 0 && info["storage"]["freeGB"].get<double>() <= info["storage"]["totalGB"].get<double>());
  CHECK(!info.contains("error"));
  CHECK(ps5sys::temperatures()["reason"] == "not a PS5");
  CHECK(ps5sys::read("nope")["error"] == "unknown");
}

/** Saves, screenshots and play activity from a stand-in console tree. */
static void testSavesShotsAndPlay() {
  const std::string root = "ps5sys-tree";
  std::system(("rm -rf " + root).c_str());
  auto file = [](const std::string& path, size_t bytes, time_t mtime) {
    std::system(("mkdir -p \"" + path.substr(0, path.rfind('/')) + "\"").c_str());
    std::ofstream(path) << std::string(bytes, 'x');
    struct timeval tv[2] = {{mtime, 0}, {mtime, 0}};
    utimes(path.c_str(), tv);
  };
  ps5sys::Paths paths{root + "/home", root + "/photo", root + "/thumbs", root + "/sl2_log.db"};
  const time_t nov3 = 1793707200;  // 2026-11-03 12:00 UTC
  file(paths.home + "/179a0cd8/savedata_prospero/PPSA01325/data", 1500000, nov3);
  file(paths.home + "/179a0cd8/savedata_prospero/PPSA01325/sce_sys/param.json", 500000, nov3);
  file(paths.home + "/179a0cd8/savedata/CUSA00127/data", 1000, nov3 - 86400);
  file(paths.home + "/179a0cd8/savedata/notatitle/data", 1, nov3);
  file(paths.home + "/guest/savedata_prospero/PPSA09999/data", 1, nov3);  // not a user id
  // Folder times are the save's last write.
  struct timeval t1[2] = {{nov3, 0}, {nov3, 0}}, t2[2] = {{nov3 - 86400, 0}, {nov3 - 86400, 0}};
  utimes((paths.home + "/179a0cd8/savedata_prospero/PPSA01325").c_str(), t1);
  utimes((paths.home + "/179a0cd8/savedata/CUSA00127").c_str(), t2);

  ps5sys::Titles names;
  names.titles = {{"PPSA01325", "Astro's Playroom", "PS5"}};
  auto sv = ps5sys::saves(names, paths);
  CHECK(sv["saves"].size() == 2);
  if (sv["saves"].size() == 2) {
    CHECK(sv["saves"][0]["id"] == "PPSA01325" && sv["saves"][0]["title"] == "Astro's Playroom" && sv["saves"][0]["platform"] == "PS5");
    CHECK(sv["saves"][0]["sizeMB"] == 2.0 && sv["saves"][0]["lastSaved"] == "2026-11-03");
    CHECK(sv["saves"][1]["id"] == "CUSA00127" && sv["saves"][1]["title"] == "CUSA00127" && sv["saves"][1]["platform"] == "PS4");
  }

  file(paths.photos + "/u/u/1/a.jxr", 1000000, nov3);
  file(paths.photos + "/u/u/1/a.dat", 5, nov3);
  file(paths.thumbnails + "/u/u/1/a.jxr.jxr", 10, nov3);      // a's thumbnail: not another shot
  file(paths.thumbnails + "/u/u/1/b.jxr.jxr", 10, nov3 - 99);  // b's shot is gone: counts
  auto sh = ps5sys::screenshots(paths);
  CHECK(sh["count"] == 2 && sh["newest"].size() == 2 && sh["newest"][0]["taken"] == "2026-11-03" && sh["newest"][0]["sizeMB"] == 1.0);

  sqlite3* h = nullptr;
  CHECK(sqlite3_open(paths.playLog.c_str(), &h) == SQLITE_OK);
  sqlite3_exec(h,
               "CREATE TABLE tbl_log(id INTEGER PRIMARY KEY, event_id TEXT, log TEXT);"
               "INSERT INTO tbl_log(event_id, log) VALUES"
               " ('ApplicationSessionEndBi', '{\"appTitleId\":\"PPSA01325\",\"totalFgTime\":3600}'),"
               " ('SomethingElse', '{\"appTitleId\":\"PPSA04609\"}'),"
               " ('ApplicationSessionEndBi', '{\"appTitleId\":\"NPXS40028\",\"totalFgTime\":9}'),"
               " ('ApplicationSessionEndBi', '{\"appTitleId\":\"CUSA00127\",\"totalFgTime\":540}'),"
               " ('ApplicationSessionEndBi', '{\"appTitleId\":\"PPSA01325\",\"totalFgTime\":7200}');",
               nullptr, nullptr, nullptr);
  sqlite3_close(h);
  std::string schema;
  auto play = ps5sys::playActivity(names, paths, &schema);
  CHECK(schema == "tbl_log(id, event_id, log)");
  CHECK(play["recentlyPlayed"].size() == 2);
  if (play["recentlyPlayed"].size() == 2) {
    CHECK(play["recentlyPlayed"][0]["id"] == "PPSA01325" && play["recentlyPlayed"][0]["hoursPlayed"] == 3.0);  // each row is a session: summed
    CHECK(play["recentlyPlayed"][1]["id"] == "CUSA00127" && play["recentlyPlayed"][1]["hoursPlayed"] == 0.1);
  }
  // The console's own total, when app.db has it, wins over the log (which may have lost old rows).
  names.titles[0].secondsOnConsole = 36000;
  names.titles[0].lastPlayed = "2026-11-03";
  play = ps5sys::playActivity(names, paths);
  CHECK(play["recentlyPlayed"][0]["hoursPlayed"] == 10.0 && play["recentlyPlayed"][0]["lastPlayed"] == "2026-11-03");
  CHECK(ps5sys::playActivity(names, {root, root, root, root + "/none.db"})["error"] == "unreadable");
  std::system(("rm -rf " + root).c_str());
}

/** Voice capture: off the console there is no mic, and the WAV encoder frames 16 kHz mono PCM. */
static void testVoiceCapture() {
  ps5voice::Recorder rec;
  CHECK(!rec.available());          // the host has no DualSense mic
  CHECK(!rec.start());              // can't record without one
  ps5voice::Status st = ps5voice::Status::Ok;
  auto got = rec.stop(&st);
  CHECK(st == ps5voice::Status::NoMic && got.samples.empty());

  ps5voice::Recording r;
  r.sampleRate = 16000;
  r.samples.assign(16000, 1000);   // 1 s tone-ish
  CHECK(r.seconds() == 1.0);
  auto wav = ps5voice::toWav(r);
  CHECK(wav.size() == 44 + 16000 * 2);                         // header + PCM
  CHECK(std::string(wav.begin(), wav.begin() + 4) == "RIFF");
  CHECK(std::string(wav.begin() + 8, wav.begin() + 12) == "WAVE");
  CHECK(std::string(wav.begin() + 36, wav.begin() + 40) == "data");
  // Sample rate (offset 24) and bits-per-sample (offset 34) are little-endian 16000 and 16.
  CHECK((wav[24] | wav[25] << 8 | wav[26] << 16 | wav[27] << 24) == 16000);
  CHECK((wav[34] | wav[35] << 8) == 16);
}

int main() {
  testInstalledTitles();
  testSavesShotsAndPlay();
  testVoiceCapture();
  testSystemInfo();
  testEnvelopesAndBindings();
  testActivitySnapshot();
  testGenerativeUISurface();
  testMenuNavigation();
  testShelfTemplate();
  testTwoShelvesShareATemplate();
  testPasswordPromptAndConfirm();
  testPages();
  testSseParser();
  testSessionLifecycle();
  testMessageBar();
  testNarrationIsNotTheCaption();
  testSystemKeyboard();
  testNativeDialogs();
  testToolProgress();
  testPairing();
  testConversationHistory();
  testFollowUpsAndNewConversation();
  testHaptics();
  if (failures) std::fprintf(stderr, "%d check(s) failed\n", failures);
  else std::printf("all tests passed\n");
  return failures ? 1 : 0;
}
