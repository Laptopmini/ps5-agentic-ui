#include "ps5_voice.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#ifdef __PROSPERO__
#include <dlfcn.h>
#endif

namespace ps5voice {

namespace {
void (*g_log)(const char*) = nullptr;
[[maybe_unused]] void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
[[maybe_unused]] void logf(const char* fmt, ...) {
  if (!g_log) return;
  char buf[192];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  g_log(buf);
}

constexpr int kRate = 16000;
[[maybe_unused]] constexpr unsigned kBlock = 256;  // samples per sceAudioInInput call (console only)
[[maybe_unused]] constexpr int kMaxSeconds = 30;   // cap a recording (console only)
constexpr unsigned kMinSamples = kRate / 2;  // at least 0.5 s of audio
constexpr int kSilencedPeak = 48;     // below this 16-bit peak, treat as no speech

// WAV (RIFF) little-endian helpers.
void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(x & 0xFF); v.push_back(x >> 8); }
void put32(std::vector<uint8_t>& v, uint32_t x) {
  for (int i = 0; i < 4; ++i) v.push_back((x >> (8 * i)) & 0xFF);
}
void tag(std::vector<uint8_t>& v, const char* s) { v.insert(v.end(), s, s + 4); }

#ifdef __PROSPERO__
extern "C" {
// A payload doesn't get the user service initialized for free, so (like sysui.cpp/ime.cpp) we
// initialize it and ask for the foreground user; sceAudioInOpen needs a real user id, not -1.
int sceUserServiceInitialize(void* params);
int sceUserServiceGetForegroundUser(int* userId);
}
// Audio-in isn't stubbed in the payload SDK, so its resident .sprx is dlopen'd by name and its
// exports resolved with dlsym — the same way sysui.cpp reaches libSceCommonDialog/libSceMsgDialog
// on this console. (ProsperoAI, a native *title*, imports libSceAudioIn through its module import
// table instead; a payload can't, so it loads the module at run time. Both call the same recipe.)

using InitFn = int (*)();
using OpenFn = int (*)(int, unsigned, unsigned, unsigned, unsigned, unsigned);
using InputFn = int (*)(int, void*);
using CloseFn = int (*)(int);
#endif

}  // namespace

struct Recorder::Impl {
  std::thread worker;
  std::atomic<bool> stop{false};
  std::mutex mutex;
  std::vector<int16_t> samples;  // guarded by mutex

#ifdef __PROSPERO__
  InitFn init = nullptr;
  OpenFn open = nullptr;
  InputFn input = nullptr;
  CloseFn close = nullptr;

  void* handle = nullptr;  // the dlopen'd audio-in module
  std::string loadName;    // which candidate name actually opened

  Impl() { resolve(); }
  void resolve() {
    // The resident audio-in module. Its sandbox path varies, so try the plain SONAME (the loader
    // searches its own lib paths) and the usual system locations; the first that opens wins.
    static const char* kNames[] = {
      "libSceAudioIn.sprx",
      "/system/common/lib/libSceAudioIn.sprx",
      "/system/sys/libSceAudioIn.sprx",
    };
    for (const char* name : kNames) {
      handle = dlopen(name, RTLD_LAZY);
      if (handle) { loadName = name; break; }
    }
    if (!handle) return;
    init = reinterpret_cast<InitFn>(dlsym(handle, "sceAudioInInit"));
    open = reinterpret_cast<OpenFn>(dlsym(handle, "sceAudioInOpen"));
    input = reinterpret_cast<InputFn>(dlsym(handle, "sceAudioInInput"));
    close = reinterpret_cast<CloseFn>(dlsym(handle, "sceAudioInClose"));
  }
  bool ok() const { return open && input && close; }
  // What happened, so a failure on the console is diagnosable (module didn't open, or an export name
  // differs) rather than a flat "needs a PS5".
  std::string diagnose() const {
    char buf[200];
    std::snprintf(buf, sizeof(buf),
                  "mic unavailable (dlopen %s=%s; init %c open %c input %c close %c)",
                  loadName.empty() ? "libSceAudioIn.sprx" : loadName.c_str(), handle ? "ok" : "null",
                  init ? 'y' : 'n', open ? 'y' : 'n', input ? 'y' : 'n', close ? 'y' : 'n');
    return buf;
  }
#else
  bool ok() const { return false; }
#endif
};

Recorder::Recorder() : impl_(std::make_unique<Impl>()) {
#ifdef __PROSPERO__
  if (!impl_->ok()) error_ = impl_->diagnose();
#else
  if (!impl_->ok()) error_ = "The microphone needs a PS5.";
#endif
}
Recorder::~Recorder() {
  impl_->stop = true;
  if (impl_->worker.joinable()) impl_->worker.join();
}

bool Recorder::available() const { return impl_->ok(); }

double Recorder::seconds() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return double(impl_->samples.size()) / kRate;
}

bool Recorder::start() {
  if (recording_.load() || !impl_->ok()) return false;
#ifdef __PROSPERO__
  // Reap a previous run's thread. A failed open returns the worker without the host ever calling
  // stop() (its release path sees recording()==false), so the thread is left joinable; reassigning
  // a joinable std::thread calls std::terminate(). Join it here before starting the next one.
  if (impl_->worker.joinable()) impl_->worker.join();
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->samples.clear();
    impl_->samples.reserve(kRate * 4);
  }
  impl_->stop = false;
  level_ = 0;
  recording_ = true;
  impl_->worker = std::thread([this] {
    Impl& im = *impl_;
    int user = -1;
    sceUserServiceInitialize(nullptr);  // harmless if already initialised (SDL/sysui may have)
    int urc = sceUserServiceGetForegroundUser(&user);
    logf("voice.worker: user rc=0x%x id=%d; init=%p open=%p input=%p close=%p",
         unsigned(urc), user, (void*)im.init, (void*)im.open, (void*)im.input, (void*)im.close);
    if (im.init) { logf("voice.worker: calling sceAudioInInit"); int irc = im.init(); logf("voice.worker: init rc=0x%x", unsigned(irc)); }
    // type 1 = general voice input, 16 kHz mono 16-bit, kBlock samples per read.
    logf("voice.worker: calling sceAudioInOpen(user=%d, type=1, idx=0, grain=%u, rate=%d)", user, kBlock, kRate);
    int handle = im.open(user, 1, 0, kBlock, kRate, 0);
    logf("voice.worker: open handle=0x%x", unsigned(handle));
    if (handle < 0) {
      error_ = "Couldn't open the microphone.";
      recording_ = false;
      return;
    }
    const unsigned cap = kRate * kMaxSeconds;
    int16_t block[kBlock];
    bool firstInput = true;
    while (!im.stop.load()) {
      if (firstInput) { logf("voice.worker: calling first sceAudioInInput"); }
      if (im.input(handle, block) < 0) break;
      if (firstInput) { logf("voice.worker: first input ok"); firstInput = false; }
      int peak = 0;
      for (unsigned i = 0; i < kBlock; ++i) peak = std::max(peak, std::abs(int(block[i])));
      level_ = std::min(1.0f, peak / 32768.0f);
      std::lock_guard<std::mutex> lock(im.mutex);
      if (im.samples.size() >= cap) continue;  // keep draining, don't grow past the cap
      im.samples.insert(im.samples.end(), block, block + kBlock);
    }
    im.close(handle);
  });
  return true;
#else
  return false;
#endif
}

Recording Recorder::stop(Status* status) {
  Status s = Status::Ok;
  if (!impl_->ok()) s = Status::NoMic;
  impl_->stop = true;
  if (impl_->worker.joinable()) impl_->worker.join();
  recording_ = false;
  level_ = 0;

  Recording rec;
  rec.sampleRate = kRate;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    rec.samples = std::move(impl_->samples);
    impl_->samples.clear();
  }
  if (s == Status::Ok) {
    int peak = 0;
    for (int16_t v : rec.samples) peak = std::max(peak, std::abs(int(v)));
    if (rec.samples.size() < kMinSamples) s = Status::TooShort;
    else if (peak < kSilencedPeak) s = Status::Silent;
  }
  if (status) *status = s;
  return rec;
}

void setLog(void (*fn)(const char*)) { g_log = fn; }

std::vector<uint8_t> toWav(const Recording& rec) {
  const uint32_t rate = rec.sampleRate > 0 ? rec.sampleRate : kRate;
  const uint16_t channels = 1, bits = 16;
  const uint32_t byteRate = rate * channels * bits / 8;
  const uint32_t dataBytes = uint32_t(rec.samples.size() * sizeof(int16_t));
  std::vector<uint8_t> w;
  w.reserve(44 + dataBytes);
  tag(w, "RIFF");
  put32(w, 36 + dataBytes);
  tag(w, "WAVE");
  tag(w, "fmt ");
  put32(w, 16);                  // PCM fmt chunk size
  put16(w, 1);                   // PCM
  put16(w, channels);
  put32(w, rate);
  put32(w, byteRate);
  put16(w, channels * bits / 8);  // block align
  put16(w, bits);
  tag(w, "data");
  put32(w, dataBytes);
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(rec.samples.data());
  w.insert(w.end(), bytes, bytes + dataBytes);
  return w;
}

}  // namespace ps5voice
