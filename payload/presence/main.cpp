// PS5 A2UI's background presence (M4). A payload with no window, sent to the ELF loader
// (:9021) so it stays resident while the player is in a game: by the app when its agent is a
// channel, by scripts/ps5-channel.sh (PRESENCE=1), or at boot by a payload autoloader. It holds the channel's
// event stream as the console's "presence" (channel/adapter.ts, role=presence). While the app
// isn't open, the adapter queues what the agent says and sends CUSTOM "ps5.notify" {title, text}
// here; this shows it as a system toast whose button opens the app (the home-screen tile,
// PPSA95210, through the deep link psgm:play?id=…). When the app connects, the adapter gives it
// the queued messages.
//
// Which channel, read again before every connection (so a new pairing or session is picked up):
// - a session's agent.json (scripts/ps5-channel.sh: {"channel", "token"}): stops when that channel
//   says it's stopping (CUSTOM "ps5.presence" {stop}), when the file is gone, or after 10 minutes
//   without the channel. The app deletes agent.json at launch, so a session lasts until the next
//   reconnection;
// - else the channel paired from the app's setup screen (credentials.json, provider "channel"):
//   keeps trying, once a minute at most, for as long as the console is on (the computer running
//   the channel may sleep or restart).
// One instance: a new one asks the running one to quit (127.0.0.1:8799, "quit").
// Ends with _exit (docs/packaging.md).
#include <arpa/inet.h>
#include <signal.h>
#include <curl/curl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <vector>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include "nlohmann/json.hpp"
#include <string>
#include <thread>

using json = nlohmann::json;

extern "C" {
int sceNotificationSend(int userId, bool isLogged, const char* payload);
int sceSystemServiceGetAppIdOfRunningBigApp(void);
}

namespace {

constexpr const char* kAppDir = "/data/homebrew/PS5AgenticUI";
constexpr const char* kTitleId = "PPSA95210";  // the home-screen tile (payload/tile)
constexpr const char* kConsoleId = "ps5";
constexpr int kControlPort = 8799;
constexpr int kChoicePort = 8797;  // where a pressed notification choice calls back (docs/channel.md)
constexpr int kGiveUpSeconds = 600;

std::atomic<bool> g_stop{false};
std::atomic<bool> g_channelStopping{false};  // the channel said it's stopping
bool g_announced = false;

// The channel to post a pressed choice's value back to (set in main), and the live choices'
// values by button index (set when a choice toast is shown, read when one is pressed).
std::mutex g_choiceMutex;
std::string g_channelUrl, g_channelToken;
std::vector<std::string> g_choiceValues;

void say(const char* fmt, const std::string& a = "") {
  std::printf("[presence] ");
  std::printf(fmt, a.c_str());
  std::printf("\n");
  std::fflush(stdout);
}

bool exists(const std::string& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}

/** A system toast with the given action buttons (may be empty). */
void sendToast(const std::string& title, const std::string& text, const json& actions) {
  static unsigned serial = 0;
  timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  tm t;
  gmtime_r(&ts.tv_sec, &t);
  char created[40];
  std::snprintf(created, sizeof created, "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour,
                t.tm_min, t.tm_sec, ts.tv_nsec / 1000000);
  const std::string iconPath = std::string("/user/appmeta/") + kTitleId + "/icon0.png";
  json icon = exists(iconPath) ? json{{"type", "Url"}, {"parameters", {{"url", iconPath}}}}
                               : json{{"type", "Predefined"}, {"parameters", {{"icon", "download"}}}};
  json view = {{"icon", icon}, {"message", {{"body", text}}}, {"subMessage", {{"body", title}}}};
  if (!actions.empty()) view["actions"] = actions;
  json body = {
      {"rawData",
       {{"viewTemplateType", "InteractiveToastTemplateB"},
        {"channelType", "Downloads"},
        {"useCaseId", "IDC"},
        {"toastOverwriteType", "No"},
        {"isImmediate", true},
        {"priority", 100},
        {"viewData", view},
        // What the shell shows when notification previews are off.
        {"platformViews",
         {{"previewDisabled", {{"viewData", {{"icon", {{"type", "Predefined"}, {"parameters", {{"icon", "download"}}}}}, {"message", {{"body", title}}}}}}}}}}},
      {"createdDateTime", created},
      {"localNotificationId", std::to_string((unsigned)(ts.tv_sec % 100000000) * 10 + serial++ % 10)},
  };
  const int rc = sceNotificationSend(0xFE, true, body.dump().c_str());
  if (rc < 0) say("sceNotificationSend failed: %s", std::to_string(rc));
}

/** A system toast. With `open`, its button (and the PS button while it shows) opens the app. */
void toast(const std::string& title, const std::string& text, bool open) {
  json actions = json::array();
  if (open) {
    // The app runs as a game-type title: opening it closes whatever game or app is running. Say so.
    const bool somethingRuns = sceSystemServiceGetAppIdOfRunningBigApp() > 0;
    actions.push_back({{"actionName", somethingRuns ? "Open PS5 A2UI (closes what's running)" : "Open PS5 A2UI"},
                       {"actionType", "DeepLink"},
                       {"defaultFocus", true},
                       {"parameters", {{"actionUrl", std::string("psgm:play?id=") + kTitleId}}}});
  }
  sendToast(title, text, actions);
}

/**
 * A toast that asks the player to pick one of a few options (the agent's ask_choices). Each option
 * is a button; pressing it opens the web browser to our choice listener (notifications can't reply
 * silently, docs/channel.md), which posts the option's value back to the channel as the player's
 * reply. Up to five options; others are dropped (a toast shows about five buttons).
 */
void toastChoices(const std::string& title, const std::string& text, const json& options) {
  json actions = json::array();
  std::vector<std::string> values;
  for (const auto& o : options) {
    if (!o.is_object() || !o.value("label", "").size() || actions.size() >= 5) continue;
    const std::string url = "http://127.0.0.1:" + std::to_string(kChoicePort) + "/c?i=" + std::to_string(actions.size());
    actions.push_back({{"actionName", o["label"]}, {"actionType", "DeepLink"}, {"defaultFocus", actions.empty()}, {"parameters", {{"actionUrl", url}}}});
    values.push_back(o.value("value", o.value("label", std::string())));
  }
  if (actions.empty()) {
    toast(title, text, true);
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_choiceMutex);
    g_choiceValues = std::move(values);
  }
  sendToast(title, text, actions);
}

/** POST {console, text} to the channel's /turn, so a pressed choice becomes the player's reply. */
void postTurn(const std::string& value) {
  std::string url, token;
  {
    std::lock_guard<std::mutex> lock(g_choiceMutex);
    url = g_channelUrl;
    token = g_channelToken;
  }
  if (url.empty()) return;
  CURL* curl = curl_easy_init();
  if (!curl) return;
  const std::string body = json{{"console", kConsoleId}, {"text", value}}.dump();
  curl_slist* headers = curl_slist_append(nullptr, "content-type: application/json");
  if (!token.empty()) headers = curl_slist_append(headers, ("authorization: Bearer " + token).c_str());
  curl_easy_setopt(curl, CURLOPT_URL, (url + "/turn").c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
  const CURLcode rc = curl_easy_perform(curl);
  say("choice posted: %s", rc == CURLE_OK ? value : std::string("failed: ") + curl_easy_strerror(rc));
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
}

/** Listen on the choice port: a pressed choice (GET /c?i=N) posts option N's value as a turn. */
void choiceListener() {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(kChoicePort);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(fd, (sockaddr*)&addr, sizeof addr) != 0 || listen(fd, 4) != 0) {
    say("choice port busy: notification choices won't be heard");
    close(fd);
    return;
  }
  timeval tv{1, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  while (!g_stop) {
    int c = accept(fd, nullptr, nullptr);
    if (c < 0) continue;
    char buf[512] = {};
    ssize_t n = read(c, buf, sizeof buf - 1);
    std::string req(buf, n > 0 ? n : 0);
    // "GET /c?i=N ...": read the index after "i=".
    std::string value;
    if (auto at = req.find("i="); at != std::string::npos) {
      size_t idx = static_cast<size_t>(std::atoi(req.c_str() + at + 2));
      std::lock_guard<std::mutex> lock(g_choiceMutex);
      if (idx < g_choiceValues.size()) value = g_choiceValues[idx];
    }
    const std::string page = "<html><body>Got it. You can return to your game.</body></html>";
    const std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " + std::to_string(page.size()) +
                             "\r\nConnection: close\r\n\r\n" + page;
    (void)!write(c, resp.data(), resp.size());
    close(c);
    if (!value.empty()) postTurn(value);
  }
  close(fd);
}

/** One instance: listen on loopback; a later instance's "quit" stops this one. */
int takeControlPort() {
  for (int attempt = 0; attempt < 20; ++attempt) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kControlPort);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (sockaddr*)&addr, sizeof addr) == 0 && listen(fd, 4) == 0) return fd;
    close(fd);
    if (attempt == 0) {
      int c = socket(AF_INET, SOCK_STREAM, 0);
      if (connect(c, (sockaddr*)&addr, sizeof addr) == 0) {
        say("asking the running instance to quit");
        (void)!write(c, "quit\n", 5);
      }
      close(c);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
  return -1;
}

void controlLoop(int fd) {
  while (!g_stop) {
    int c = accept(fd, nullptr, nullptr);
    if (c < 0) continue;
    char buf[16] = {};
    ssize_t n = read(c, buf, sizeof buf - 1);
    if (n > 0 && std::string(buf).rfind("quit", 0) == 0) g_stop = true;
    close(c);
  }
}

struct Stream {
  std::string buf, title = "PS5 A2UI";
  bool open = false;
};

void onEvent(Stream& s, const std::string& data) {
  json e = json::parse(data, nullptr, false);
  if (!e.is_object() || e.value("type", "") != "CUSTOM") return;
  if (e.value("name", "") == "ps5.presence" && e.contains("value") && e["value"].is_object() && e["value"].value("stop", false)) {
    say("the channel is stopping");
    g_channelStopping = true;  // onProgress ends the transfer
    return;
  }
  if (e.value("name", "") != "ps5.notify") return;
  const json& v = e.contains("value") && e["value"].is_object() ? e["value"] : json::object();
  std::string title = v.contains("title") && v["title"].is_string() ? v["title"].get<std::string>() : s.title;
  std::string text = v.contains("text") && v["text"].is_string() ? v["text"].get<std::string>() : "";
  if (text.size() > 600) {  // the toast shows a line or two; cut on a UTF-8 character boundary
    size_t cut = 600;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
    text.resize(cut);
  }
  if (v.contains("choices") && v["choices"].is_array() && !v["choices"].empty()) {
    say("notify with %s choices", std::to_string(v["choices"].size()));
    toastChoices(title, text.empty() ? "Pick one:" : text, v["choices"]);
    return;
  }
  say("notify: %s", std::to_string(text.size()) + " bytes");
  toast(title, text.empty() ? "Sent you a message." : text, true);
}

size_t onData(char* data, size_t size, size_t count, void* user) {
  auto* s = static_cast<Stream*>(user);
  if (!s->open && !g_announced) {
    toast("PS5 A2UI", "Listening in the background for messages from your agent.", false);
    g_announced = true;
  }
  s->open = true;
  s->buf.append(data, size * count);
  for (size_t end; (end = s->buf.find("\n\n")) != std::string::npos;) {
    std::string chunk = s->buf.substr(0, end);
    s->buf.erase(0, end + 2);
    std::string payload;
    for (size_t pos = 0; pos <= chunk.size();) {
      size_t nl = chunk.find('\n', pos);
      std::string line = chunk.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
      if (line.rfind("data: ", 0) == 0) payload += line.substr(6);
      if (nl == std::string::npos) break;
      pos = nl + 1;
    }
    if (!payload.empty()) onEvent(*s, payload);
  }
  if (s->buf.size() > 1 << 20) s->buf.clear();
  return size * count;
}

int onProgress(void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) { return g_stop || g_channelStopping ? 1 : 0; }

struct Channel {
  std::string url, token;
  bool session = false;  // from a session's agent.json, not a pairing
  bool operator==(const Channel& o) const { return url == o.url && token == o.token && session == o.session; }
};

json readJson(const std::string& path) {
  std::ifstream f(path);
  return f ? json::parse(f, nullptr, false) : json();
}

/** The session's channel if there is one, else the paired one; url empty if neither. */
Channel channelToUse() {
  Channel c;
  json session = readJson(std::string(kAppDir) + "/agent.json");
  json paired = readJson(std::string(kAppDir) + "/credentials.json");
  if (session.is_object() && !session.value("channel", "").empty()) c = {session.value("channel", ""), session.value("token", ""), true};
  else if (paired.is_object() && paired.value("provider", "") == "channel") c = {paired.value("url", ""), paired.value("key", ""), false};
  while (!c.url.empty() && c.url.back() == '/') c.url.pop_back();
  return c;
}

}  // namespace

int main() {
  // Output goes to the loader's connection, which may close long before this ends.
  signal(SIGPIPE, SIG_IGN);
  Channel channel = channelToUse();
  if (channel.url.empty()) {
    say("no channel: neither a session's agent.json nor a paired channel");
    toast("PS5 A2UI", "No channel set up for background messages.", false);
    _exit(1);
  }

  int control = takeControlPort();
  if (control < 0) {
    say("port 8799 busy, not starting");
    _exit(1);
  }
  std::thread(controlLoop, control).detach();
  curl_global_init(CURL_GLOBAL_DEFAULT);
  // Where a pressed notification choice posts its value back (updated if the channel changes).
  auto rememberChannel = [](const Channel& c) {
    std::lock_guard<std::mutex> lock(g_choiceMutex);
    g_channelUrl = c.url;
    g_channelToken = c.token;
  };
  rememberChannel(channel);
  std::thread(choiceListener).detach();
  say("listening for messages from %s", channel.url + (channel.session ? " (session)" : " (paired)"));

  auto lastConnected = std::chrono::steady_clock::now();
  int backoffMs = 1000;
  while (!g_stop) {
    Stream s;
    CURL* curl = curl_easy_init();
    curl_slist* headers = curl_slist_append(nullptr, "accept: text/event-stream");
    if (!channel.token.empty()) headers = curl_slist_append(headers, ("authorization: Bearer " + channel.token).c_str());
    const std::string events = channel.url + "/events?console=ps5&role=presence";
    curl_easy_setopt(curl, CURLOPT_URL, events.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, onData);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &s);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, onProgress);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);  // keep-alives every 15 s
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 45L);
    const auto began = std::chrono::steady_clock::now();
    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (s.open && status == 200) lastConnected = std::chrono::steady_clock::now();
    if (g_stop) break;
    say("stream ended: %s", g_channelStopping ? std::string("the channel is stopping") : rc != CURLE_OK ? curl_easy_strerror(rc) : "HTTP " + std::to_string(status));
    // A session's channel stopping ends the session; a paired channel is just restarting.
    if (g_channelStopping.exchange(false) && channel.session) break;
    if (status == 401) {
      toast("PS5 A2UI", "The channel refused the access code; stopped listening.", false);
      break;
    }
    if (channel.session && std::chrono::steady_clock::now() - lastConnected > std::chrono::seconds(kGiveUpSeconds)) {
      toast("PS5 A2UI", "Lost the channel; stopped listening in the background.", false);
      break;
    }
    if (std::chrono::steady_clock::now() - began > std::chrono::seconds(30)) backoffMs = 1000;
    for (int waited = 0; waited < backoffMs && !g_stop; waited += 100) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    backoffMs = std::min(backoffMs * 2, channel.session ? 15000 : 60000);
    // The session may have ended, or the pairing changed.
    Channel now = channelToUse();
    if (now.url.empty()) {
      say("no channel any more");
      break;
    }
    if (!(now == channel)) {
      say("now listening to %s", now.url + (now.session ? " (session)" : " (paired)"));
      channel = now;
      rememberChannel(channel);
      lastConnected = std::chrono::steady_clock::now();
      backoffMs = 1000;
    }
  }
  say("quitting");
  close(control);
  curl_global_cleanup();
  _exit(0);
}
