#include "pairing.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>

extern "C" {
#include "tweetnacl/tweetnacl.h"

#if defined(__PROSPERO__)
int sceRandomGetRandomNumber(void* buf, size_t size);  // libSceRandom, at most 64 bytes a call
#endif

// TweetNaCl's random source (key pairs).
void randombytes(unsigned char* out, unsigned long long n) {
#if defined(__PROSPERO__)
  while (n > 0) {
    size_t chunk = n > 64 ? 64 : size_t(n);
    if (sceRandomGetRandomNumber(out, chunk) != 0) std::abort();  // never use a weak key
    out += chunk;
    n -= chunk;
  }
#else
  arc4random_buf(out, size_t(n));
#endif
}
}

namespace a2ui {
namespace {

using nlohmann::json;
constexpr int kMaxWrong = 5;
constexpr size_t kMaxRequest = 16 * 1024;

std::string randomCode() {
  unsigned char b[4];
  std::string code;
  while (code.size() < 6) {
    randombytes(b, 1);
    if (b[0] < 250) code += char('0' + b[0] % 10);  // no modulo bias
  }
  return code;
}

bool constantTimeEqual(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  unsigned char diff = 0;
  for (size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
  return diff == 0;
}

// 10/8, 172.16/12, 192.168/16, 169.254/16 (link-local), 127/8. Host byte order.
bool isPrivate(uint32_t ip) {
  return (ip >> 24) == 10 || (ip >> 20) == 0xAC1 || (ip >> 16) == 0xC0A8 || (ip >> 16) == 0xA9FE || (ip >> 24) == 127;
}

bool printable(const std::string& s, bool allowSpace) {
  for (unsigned char c : s)
    if (c < 0x21 || c > 0x7E) {
      if (!(allowSpace && c == ' ')) return false;
    }
  return true;
}

const char* reason(int code) {
  switch (code) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    default: return "Error";
  }
}

}  // namespace

// ---------- Credentials ----------

std::string Credentials::problem() const {
  const bool remote = provider == "server" || provider == "channel";
  const bool custom = provider == "custom";
  if (provider != "openrouter" && !custom && !remote) return "Choose OpenRouter, another OpenAI-compatible endpoint, your own server or a channel.";
  if (key.empty()) return remote ? "Enter the access code." : "Enter the API key.";
  if (key.size() > 512 || !printable(key, false)) return "That key has spaces or characters keys don't have. Paste it again.";
  if (model.size() > 128 || !printable(model, false)) return "That model id doesn't look right.";
  if (!remote && model.empty()) return "Enter the model id.";
  if (remote || custom) {
    const char* what = custom ? "endpoint" : "server";
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) return std::string("The ") + what + " address should start with http:// or https://.";
    if (url.size() > 256 || !printable(url, false)) return std::string("That ") + what + " address doesn't look right.";
  }
  return "";
}

std::string Credentials::label() const {
  if (provider == "openrouter") return "OpenRouter";
  size_t start = url.find("://");
  start = start == std::string::npos ? 0 : start + 3;
  return url.substr(start, url.find('/', start) - start);
}

std::map<std::string, std::string> Credentials::env() const {
  // Any OpenAI-compatible endpoint: the same LLM_API_KEY, only the base URL differs.
  if (provider == "openrouter") return {{"LLM_BASE_URL", "https://openrouter.ai/api/v1"}, {"LLM_API_KEY", key}};
  if (provider == "custom") return {{"LLM_BASE_URL", url}, {"LLM_API_KEY", key}};
  return {};
}

json Credentials::toJson() const {
  json j = {{"provider", provider}, {"key", key}};
  if (!model.empty()) j["model"] = model;
  if (!url.empty()) j["url"] = url;
  return j;
}

std::optional<Credentials> Credentials::fromJson(const json& j) {
  if (!j.is_object()) return std::nullopt;
  auto str = [&](const char* k) { return j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string(); };
  Credentials c{str("provider"), str("key"), str("model"), str("url")};
  if (!c.problem().empty()) return std::nullopt;
  return c;
}

std::optional<Credentials> Credentials::load(const std::string& path) {
  std::ifstream f(path);
  if (!f) return std::nullopt;
  return fromJson(json::parse(f, nullptr, false));
}

bool Credentials::save(const std::string& path) const {
  const std::string tmp = path + ".tmp";
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) return false;
  const std::string text = toJson().dump() + "\n";
  bool ok = ::write(fd, text.data(), text.size()) == ssize_t(text.size());
  ok = ::fsync(fd) == 0 && ok;
  ::close(fd);
  if (ok) ok = ::rename(tmp.c_str(), path.c_str()) == 0;
  if (!ok) ::unlink(tmp.c_str());
  return ok;
}

// ---------- base64 ----------

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(const unsigned char* d, size_t n) {
  std::string out;
  out.reserve((n + 2) / 3 * 4);
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = uint32_t(d[i]) << 16 | (i + 1 < n ? uint32_t(d[i + 1]) << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
    out += kB64[v >> 18 & 63];
    out += kB64[v >> 12 & 63];
    out += i + 1 < n ? kB64[v >> 6 & 63] : '=';
    out += i + 2 < n ? kB64[v & 63] : '=';
  }
  return out;
}

std::optional<std::string> base64Decode(const std::string& s) {
  if (s.size() % 4 != 0) return std::nullopt;
  std::string out;
  uint32_t v = 0;
  int bits = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (c == '=') {
      if (i < s.size() - 2) return std::nullopt;
      break;
    }
    const char* p = std::strchr(kB64, c);
    if (!p || !c) return std::nullopt;
    v = v << 6 | uint32_t(p - kB64);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out += char(v >> bits & 0xFF);
    }
  }
  return out;
}

// ---------- Pairing ----------

Pairing::Pairing(Options options) : options_(std::move(options)) {
  newSession();
  listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listener_ < 0) throw std::runtime_error("socket() failed");
  int one = 1;
  ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(uint16_t(options_.port));
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (::bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || ::listen(listener_, 4) != 0) {
    ::close(listener_);
    throw std::runtime_error("can't listen on port " + std::to_string(options_.port));
  }
  thread_ = std::thread([this] { serve(); });
}

Pairing::~Pairing() {
  stop_ = true;
  if (thread_.joinable()) thread_.join();
  std::memset(sk_, 0, sizeof sk_);
}

void Pairing::newSession() {
  crypto_box_keypair(pk_, sk_);
  code_ = randomCode();
  wrong_ = 0;
}

std::string Pairing::code() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return code_;
}

std::string Pairing::publicKey() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return base64Encode(pk_, sizeof pk_);
}

std::optional<Credentials> Pairing::poll() {
  std::lock_guard<std::mutex> lock(mutex_);
  return result_;
}

std::string Pairing::status() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return status_;
}

std::string Pairing::lanAddress() {
  // The address the default route would use; connect() on UDP sends nothing.
  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return "";
  sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_port = htons(53);
  ::inet_pton(AF_INET, "1.1.1.1", &to.sin_addr);
  std::string out;
  sockaddr_in me{};
  socklen_t len = sizeof me;
  if (::connect(fd, reinterpret_cast<sockaddr*>(&to), sizeof to) == 0 && ::getsockname(fd, reinterpret_cast<sockaddr*>(&me), &len) == 0) {
    char buf[INET_ADDRSTRLEN] = {};
    if (::inet_ntop(AF_INET, &me.sin_addr, buf, sizeof buf) && std::string(buf) != "0.0.0.0") out = buf;
  }
  ::close(fd);
  return out;
}

void Pairing::serve() {
  while (!stop_) {
    pollfd p{listener_, POLLIN, 0};
    if (::poll(&p, 1, 200) <= 0) continue;
    sockaddr_in peer{};
    socklen_t len = sizeof peer;
    int fd = ::accept(listener_, reinterpret_cast<sockaddr*>(&peer), &len);
    if (fd < 0) continue;
    timeval tv{5, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#ifdef SO_NOSIGPIPE
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    handle(fd, ntohl(peer.sin_addr.s_addr));
    ::close(fd);
  }
  ::close(listener_);  // stop accepting as soon as a key has arrived, not when the screen closes
  listener_ = -1;
}

void Pairing::handle(int fd, uint32_t peer) {
  int statusCode = 200;
  std::string type = "application/json", body;
  if (!isPrivate(peer)) {
    statusCode = 403;
    body = R"({"error":"Only devices on your home network can set up the console."})";
  } else {
    // Read the head, then Content-Length bytes of body.
    std::string req;
    char buf[2048];
    size_t headEnd = std::string::npos, want = 0;
    bool tooBig = false;
    while (true) {
      ssize_t n = ::recv(fd, buf, sizeof buf, 0);
      if (n <= 0) break;
      req.append(buf, size_t(n));
      if (req.size() > kMaxRequest) {
        tooBig = true;
        break;
      }
      if (headEnd == std::string::npos && (headEnd = req.find("\r\n\r\n")) != std::string::npos) {
        std::string head = req.substr(0, headEnd);
        for (char& c : head) c = char(std::tolower(static_cast<unsigned char>(c)));
        size_t cl = head.find("\r\ncontent-length:");
        want = cl == std::string::npos ? 0 : size_t(std::strtoul(head.c_str() + cl + 17, nullptr, 10));
        if (want > kMaxRequest) {
          tooBig = true;
          break;
        }
      }
      if (headEnd != std::string::npos && req.size() >= headEnd + 4 + want) break;
    }
    if (tooBig) {
      statusCode = 413;
      body = R"({"error":"Too large."})";
    } else if (headEnd == std::string::npos) {
      return;  // timed out or closed
    } else {
      size_t sp1 = req.find(' '), sp2 = req.find(' ', sp1 + 1);
      std::string method = req.substr(0, sp1), path = sp1 == std::string::npos ? "" : req.substr(sp1 + 1, sp2 - sp1 - 1);
      body = respond(method, path, req.substr(headEnd + 4, want), statusCode, type);
    }
  }
  std::string head = "HTTP/1.1 " + std::to_string(statusCode) + " " + reason(statusCode) +
                     "\r\nContent-Type: " + type + "\r\nContent-Length: " + std::to_string(body.size()) +
                     "\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer"
                     "\r\nContent-Security-Policy: default-src 'none'; script-src 'self' 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; form-action 'none'; frame-ancestors 'none'"
                     "\r\nConnection: close\r\n\r\n";
  std::string out = head + body;
  int flags = 0;
#ifdef MSG_NOSIGNAL
  flags = MSG_NOSIGNAL;
#endif
  for (size_t sent = 0; sent < out.size();) {
    ssize_t n = ::send(fd, out.data() + sent, out.size() - sent, flags);
    if (n <= 0) break;
    sent += size_t(n);
  }
}

std::string Pairing::respond(const std::string& method, const std::string& path, const std::string& body, int& statusCode, std::string& type) {
  auto error = [&](int code, const std::string& message) {
    statusCode = code;
    return json{{"error", message}}.dump();
  };
  if (method == "GET" && (path == "/" || path == "/index.html")) {
    type = "text/html; charset=utf-8";
    std::string page = options_.page;
    const std::string pk = publicKey();
    for (size_t at; (at = page.find("{{PK}}")) != std::string::npos;) page.replace(at, 6, pk);
    return page;
  }
  if (method == "GET" && path == "/nacl.min.js") {
    type = "text/javascript; charset=utf-8";
    return options_.naclJs;
  }
  if (method == "GET" && path == "/key") return json{{"pk", publicKey()}}.dump();
  if (method != "POST" || path != "/pair") return error(404, "Not found.");

  // {"pk": sender public key, "nonce", "box"}, all base64; the box holds
  // {"code", "provider", "key", "model", "url"}.
  json msg = json::parse(body, nullptr, false);
  auto field = [&](const char* k) -> std::optional<std::string> {
    if (!msg.is_object() || !msg.contains(k) || !msg[k].is_string()) return std::nullopt;
    return base64Decode(msg[k].get<std::string>());
  };
  auto senderPk = field("pk"), nonce = field("nonce"), box = field("box");
  if (!senderPk || senderPk->size() != 32 || !nonce || nonce->size() != 24 || !box || box->size() < 16) return error(400, "Malformed request.");

  std::lock_guard<std::mutex> lock(mutex_);
  if (result_) return error(409, "The console already has a key from this session.");
  auto wrong = [&](const std::string& why) {
    if (++wrong_ >= kMaxWrong) {
      newSession();
      status_ = "Too many wrong tries. The code changed.";
      if (options_.log) options_.log("pairing: too many wrong tries, new code and key");
      return error(403, "Too many wrong tries. The code on the TV has changed: reload this page and use the new one.");
    }
    status_ = why + " " + std::to_string(kMaxWrong - wrong_) + " tries left.";
    if (options_.log) options_.log("pairing: " + why);
    return error(403, why + " " + std::to_string(kMaxWrong - wrong_) + " tries left.");
  };
  // NaCl's C API wants 16 zero bytes before the ciphertext and gives 32 before the message.
  std::string c(16, '\0');
  c += *box;
  std::string m(c.size(), '\0');
  if (crypto_box_open(reinterpret_cast<unsigned char*>(m.data()), reinterpret_cast<const unsigned char*>(c.data()), c.size(),
                      reinterpret_cast<const unsigned char*>(nonce->data()), reinterpret_cast<const unsigned char*>(senderPk->data()), sk_) != 0)
    return wrong("That didn't decrypt (the page may be out of date: reload it).");
  json inner = json::parse(m.substr(32), nullptr, false);
  std::memset(m.data(), 0, m.size());
  if (!inner.is_object() || !inner.contains("code") || !inner["code"].is_string() || !constantTimeEqual(inner["code"].get<std::string>(), code_))
    return wrong("Wrong code.");
  auto str = [&](const char* k) { return inner.contains(k) && inner[k].is_string() ? inner[k].get<std::string>() : std::string(); };
  Credentials creds{str("provider"), str("key"), str("model"), str("url")};
  if (std::string why = creds.problem(); !why.empty()) return error(400, why);
  result_ = creds;
  status_ = "Key received.";
  stop_ = true;  // one key per session
  if (options_.log) options_.log("pairing: received " + creds.label() + " credentials");
  return json{{"ok", true}, {"provider", creds.label()}}.dump();
}

}  // namespace a2ui
