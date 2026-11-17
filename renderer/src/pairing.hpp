// Getting a model key onto the console without typing it with the controller. While the setup
// screen is up, the app serves a small page on the LAN (http://<console>:8791). The screen shows
// that address as a QR code plus a six-digit code; the page (on a phone or computer), or
// scripts/ps5-pair.ts on a Mac, encrypts the key to this session's public key (NaCl box: X25519 +
// XSalsa20-Poly1305) together with the code, and posts it. Then the app stores the key and closes
// the server.
//
// What it protects against, and what it doesn't:
// - Someone else on the network can't read the key in transit, and can't push a key without the
//   code on the TV (five wrong codes and the code and session key change).
// - Only private-network (LAN) addresses are served; the server runs only while the screen is up.
// - The page itself is plain HTTP, so an active attacker on the LAN could swap it. That needs
//   control of the home network, which is out of scope here.
// - At rest the key is a file on the console that any homebrew running there can read. Use a key
//   made for this console, capped where the provider allows it (see the setup screen).
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "nlohmann/json.hpp"

namespace a2ui {

/** Which model to use and how to reach it. Stored as JSON (credentials.json on the console). */
struct Credentials {
  std::string provider;  // "openrouter", "custom" (any other OpenAI-compatible endpoint), "server"
                         // (an AG-UI server, e.g. on a Mac) or "channel" (a CopilotKit channel:
                         // channel/adapter.ts)
  std::string key;       // the provider key, or the server's or channel's access code
  std::string model;     // the model id; required for "openrouter" and "custom"
  std::string url;       // "custom": the endpoint's base URL (https://…/v1); "server":
                         // http://<host>:8790/agent; "channel": http://<host>:8792

  /** Checks the fields; returns "" if usable, otherwise what's wrong (shown to the sender). */
  std::string problem() const;
  /** "OpenRouter", or the host of the endpoint, server or channel, for the screen. */
  std::string label() const;
  /** process.env for the agent runtime (JsAgent). */
  std::map<std::string, std::string> env() const;

  nlohmann::json toJson() const;
  static std::optional<Credentials> fromJson(const nlohmann::json& j);
  /** Read a credentials file; nullopt if missing or invalid. */
  static std::optional<Credentials> load(const std::string& path);
  /** Write it (owner-only permissions, replaced atomically). Returns false on failure. */
  bool save(const std::string& path) const;
};

class Pairing {
 public:
  struct Options {
    int port = 8791;
    std::string page;    // the setup page (HTML); "{{PK}}" is replaced with the public key
    std::string naclJs;  // tweetnacl-js, served as /nacl.min.js
    std::function<void(const std::string&)> log;
  };
  /** Starts listening. Throws std::runtime_error if the port can't be opened. */
  explicit Pairing(Options options);
  ~Pairing();
  Pairing(const Pairing&) = delete;
  Pairing& operator=(const Pairing&) = delete;

  /** The six digits shown on the TV. Changes after five wrong tries. */
  std::string code() const;
  int port() const { return options_.port; }
  /** Base64 of this session's X25519 public key. */
  std::string publicKey() const;
  /** Credentials once a valid push has arrived (then the server has stopped). */
  std::optional<Credentials> poll();
  /** The latest event for the screen ("Wrong code, 4 tries left"), "" if none. */
  std::string status() const;

  /** This device's LAN IPv4 address, "" if none found. */
  static std::string lanAddress();

 private:
  void serve();
  void handle(int fd, uint32_t peer);
  void newSession();  // fresh code and keypair (lock held)
  std::string respond(const std::string& method, const std::string& path, const std::string& body, int& statusCode, std::string& type);

  Options options_;
  int listener_ = -1;
  std::atomic<bool> stop_{false};
  std::thread thread_;
  mutable std::mutex mutex_;
  std::string code_, status_;
  unsigned char pk_[32], sk_[32];
  int wrong_ = 0;
  std::optional<Credentials> result_;
};

/** Base64 (standard alphabet, padded). decode returns nullopt on bad input. */
std::string base64Encode(const unsigned char* data, size_t n);
std::optional<std::string> base64Decode(const std::string& text);

}  // namespace a2ui
