#include "ps5_whisper.hpp"

#include <chrono>
#include <cstdarg>
#include <cstdio>

#ifdef __PROSPERO__
#include "whisper.h"
#endif

namespace ps5whisper {

namespace {
void (*g_log)(const char*) = nullptr;
[[maybe_unused]] void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
[[maybe_unused]] void logf(const char* fmt, ...) {
  if (!g_log) return;
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  g_log(buf);
}

[[maybe_unused]] double now() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

[[maybe_unused]] std::string trim(std::string s) {
  const char* ws = " \t\n\r";
  auto a = s.find_first_not_of(ws);
  auto b = s.find_last_not_of(ws);
  return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}
}  // namespace

void setLog(void (*fn)(const char*)) { g_log = fn; }

struct Transcriber::Impl {
  std::string modelPath;
#ifdef __PROSPERO__
  whisper_context* ctx = nullptr;  // loaded lazily on the first transcribe
  bool loadFailed = false;
#endif
  explicit Impl(std::string p) : modelPath(std::move(p)) {}
};

Transcriber::Transcriber(std::string modelPath)
    : impl_(std::make_unique<Impl>(std::move(modelPath))) {
#ifndef __PROSPERO__
  error_ = "Speech-to-text needs a PS5.";
#endif
}

Transcriber::~Transcriber() {
#ifdef __PROSPERO__
  if (impl_->ctx) whisper_free(impl_->ctx);
#endif
}

bool Transcriber::available() const {
#ifdef __PROSPERO__
  return !impl_->loadFailed;
#else
  return false;
#endif
}

Result Transcriber::transcribe(const std::vector<int16_t>& pcm, int sampleRate) {
  Result r;
#ifdef __PROSPERO__
  if (impl_->loadFailed) { r.error = error_; return r; }
  if (sampleRate != WHISPER_SAMPLE_RATE) {
    r.error = "audio must be 16 kHz";
    return r;
  }

  // Lazy one-time model load, on the GPU (ggml-vulkan / RADV).
  if (!impl_->ctx) {
    logf("whisper: loading %s (GPU)…", impl_->modelPath.c_str());
    double t0 = now();
    whisper_context_params cp = whisper_context_default_params();
    cp.use_gpu = true;
    cp.flash_attn = false;
    impl_->ctx = whisper_init_from_file_with_params(impl_->modelPath.c_str(), cp);
    r.loadSeconds = now() - t0;
    if (!impl_->ctx) {
      impl_->loadFailed = true;
      error_ = "couldn't load the whisper model";
      r.error = error_;
      logf("whisper: model load FAILED (%s)", impl_->modelPath.c_str());
      return r;
    }
    logf("whisper: model loaded in %.1fs, system: %s", r.loadSeconds, whisper_print_system_info());
  }

  // 16-bit PCM → float [-1, 1], which is what whisper_full expects.
  std::vector<float> f32(pcm.size());
  for (size_t i = 0; i < pcm.size(); ++i) f32[i] = pcm[i] / 32768.0f;

  whisper_full_params wp = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
  wp.language = "en";
  wp.n_threads = 4;
  wp.translate = false;
  wp.no_timestamps = true;
  wp.print_progress = false;
  wp.print_realtime = false;
  wp.print_timestamps = false;
  wp.print_special = false;

  double t0 = now();
  int rc = whisper_full(impl_->ctx, wp, f32.data(), int(f32.size()));
  r.transcribeSeconds = now() - t0;
  if (rc != 0) {
    r.error = "transcription failed (rc " + std::to_string(rc) + ")";
    return r;
  }

  std::string text;
  const int n = whisper_full_n_segments(impl_->ctx);
  for (int i = 0; i < n; ++i) {
    const char* seg = whisper_full_get_segment_text(impl_->ctx, i);
    if (seg) text += seg;
  }
  r.text = trim(std::move(text));
  r.ok = true;
#else
  (void)pcm;
  (void)sampleRate;
  r.error = error_;
#endif
  return r;
}

}  // namespace ps5whisper
