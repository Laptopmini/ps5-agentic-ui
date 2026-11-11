// Toast test, round 2 (owner's question, 9 Nov 2026): a notification button can carry an answer
// back to a resident payload, but pressing it opens the PS5 web browser to fetch the URL. This
// round tries to get the browser out of the way, so a toast could answer "without opening the
// app". Send it to the ELF loader (:9021):
//
//   PS5_HOST=192.168.0.56 scripts/ps5-build.sh toast-test test
//
// Five toasts, 8 s apart, one button each (so which was pressed is unambiguous). Each button is a
// DeepLink to http://127.0.0.1:8798/<letter>; this payload listens for 5 minutes and answers each
// letter differently, to see which drops the player back cleanly:
//   A  window.close()                 — the page closes its own browser window
//   B  204 No Content                 — nothing for the browser to render
//   C  history.back() then close      — step back, then close
//   D  an empty page                  — baseline: does a blank page just sit in the browser?
//   E  the button is actionType "UriLink" instead of "DeepLink" — does a different type skip the browser?
//
// The log (the loader's connection, and klog) records each press and what was served. What the
// player SEES — back in the game, the browser left open, a blank page — is the owner to report,
// per letter.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>
#include <thread>

#include "nlohmann/json.hpp"

using json = nlohmann::json;

extern "C" {
int sceNotificationSend(int userId, bool isLogged, const char* payload);
}

namespace {

constexpr int kPort = 8798;
constexpr const char* kTitleId = "PPSA95210";

void say(const std::string& s) {
  std::printf("[toast-test] %s\n", s.c_str());
  std::fflush(stdout);
}

/** One toast with a single button: a DeepLink to http://…:8798/<letter>, labelled for the owner. */
json button(const std::string& letter, const std::string& label, const std::string& type = "DeepLink") {
  return {{"actionName", label}, {"actionType", type}, {"defaultFocus", true}, {"parameters", {{"actionUrl", "http://127.0.0.1:" + std::to_string(kPort) + "/" + letter}}}};
}

void send(const std::string& letter, const std::string& text, const json& action) {
  static unsigned serial = 0;
  timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  tm t;
  gmtime_r(&ts.tv_sec, &t);
  char created[40];
  std::snprintf(created, sizeof created, "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min,
                t.tm_sec, ts.tv_nsec / 1000000);
  json icon = {{"type", "Url"}, {"parameters", {{"url", std::string("/user/appmeta/") + kTitleId + "/icon0.png"}}}};
  json view = {{"icon", icon}, {"message", {{"body", text}}}, {"subMessage", {{"body", "Toast test " + letter}}}, {"actions", json::array({action})}};
  json body = {
      {"rawData",
       {{"viewTemplateType", "InteractiveToastTemplateB"},
        {"channelType", "Downloads"},
        {"useCaseId", "IDC"},
        {"toastOverwriteType", "No"},
        {"isImmediate", true},
        {"priority", 100},
        {"viewData", view}}},
      {"createdDateTime", created},
      {"localNotificationId", std::to_string((unsigned)(ts.tv_sec % 100000000) * 10 + serial++ % 10)},
  };
  const int rc = sceNotificationSend(0xFE, true, body.dump().c_str());
  say("toast " + letter + ": sceNotificationSend " + std::to_string(rc));
}

/** The HTTP response for a pressed letter: each a different attempt to get the browser out of the way. */
std::string responseFor(const std::string& letter) {
  auto page = [](const std::string& html) {
    return "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " + std::to_string(html.size()) +
           "\r\nConnection: close\r\n\r\n" + html;
  };
  if (letter == "A") return page("<html><body><script>window.close();</script>Returning…</body></html>");
  if (letter == "B") return "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n";
  if (letter == "C") return page("<html><body><script>history.back();window.close();</script></body></html>");
  if (letter == "D") return page("<html><body></body></html>");
  if (letter == "E") return page("<html><body>Pressed E (UriLink).</body></html>");
  return page("<html><body>?</body></html>");
}

/** Listen on :8798 for `seconds`: log each press (letter + user agent), serve that letter's response. */
void watchPresses(int seconds) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(kPort);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(fd, (sockaddr*)&addr, sizeof addr) != 0 || ::listen(fd, 4) != 0) {
    say("port 8798 busy: presses can't be seen");
    return;
  }
  timeval tv{1, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  while (std::chrono::steady_clock::now() < end) {
    int c = accept(fd, nullptr, nullptr);
    if (c < 0) continue;
    char buf[1024] = {};
    ssize_t n = read(c, buf, sizeof buf - 1);
    std::string req(buf, n > 0 ? n : 0);
    std::string reqLine = req.substr(0, req.find('\r'));
    // The path is "GET /A HTTP/1.1": the letter is the char after "/".
    std::string letter = "?";
    if (auto slash = reqLine.find('/'); slash != std::string::npos && slash + 1 < reqLine.size()) letter = reqLine.substr(slash + 1, 1);
    std::string agent;
    if (auto at = req.find("User-Agent: "); at != std::string::npos) agent = req.substr(at + 12, req.find('\r', at) - at - 12);
    say("pressed " + letter + " (" + reqLine + ") from " + (agent.empty() ? "?" : agent));
    const std::string resp = responseFor(letter);
    (void)!write(c, resp.data(), resp.size());
    close(c);
  }
  close(fd);
}

}  // namespace

int main() {
  signal(SIGPIPE, SIG_IGN);
  std::thread listener(watchPresses, 300);
  send("A", "A: the page runs window.close(). Did it return you to where you were?", button("A", "A: window.close()"));
  std::this_thread::sleep_for(std::chrono::seconds(8));
  send("B", "B: the server answers 204 No Content (nothing to show).", button("B", "B: 204 No Content"));
  std::this_thread::sleep_for(std::chrono::seconds(8));
  send("C", "C: the page runs history.back() then close.", button("C", "C: back + close"));
  std::this_thread::sleep_for(std::chrono::seconds(8));
  send("D", "D: the page is empty (baseline).", button("D", "D: empty page"));
  std::this_thread::sleep_for(std::chrono::seconds(8));
  send("E", "E: the button's type is UriLink, not DeepLink. Did it still open the browser?", button("E", "E: UriLink type", "UriLink"));
  say("listening on 127.0.0.1:8798 for 5 minutes: press each toast's button and note what you see");
  listener.join();
  say("done");
  _exit(0);
}
