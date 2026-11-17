// On-console speech-to-text (docs/voice.md). Wraps whisper.cpp running on the PS5 GPU
// through ggml-vulkan (the stock SDK RADV). Given 16 kHz mono PCM from the DualSense mic
// (ps5_voice), returns the transcript. The model (ggml-base.en.bin) sits next to the app.
//
// Console only: on the Mac host build whisper isn't linked, so available() is false and the host
// build stubs it out (the renderer unit tests don't need a model). Progress and timings route
// through setLog(), the same way ps5_voice does, so they land in the app's console-log.txt.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ps5whisper {

struct Result {
  std::string text;      // the transcript (trimmed), empty if nothing was recognised
  double loadSeconds = 0;    // one-time model load (0 after the first call)
  double transcribeSeconds = 0;  // record→text for this clip
  bool ok = false;
  std::string error;     // why, when !ok
};

// Routes whisper/ggml progress lines into a sink (e.g. the app log). Call before constructing.
void setLog(void (*fn)(const char* line));

class Transcriber {
 public:
  // modelPath is resolved when the first clip is transcribed (lazy load), so construction is cheap
  // and the 148 MB model only loads if voice is actually used.
  explicit Transcriber(std::string modelPath);
  ~Transcriber();

  bool available() const;          // false on host, or if whisper couldn't be used at all
  const std::string& error() const { return error_; }

  // Transcribe 16 kHz mono 16-bit PCM. Synchronous (blocks the caller). Safe to call repeatedly;
  // the model loads once and is reused.
  Result transcribe(const std::vector<int16_t>& pcm, int sampleRate);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::string error_;
};

}  // namespace ps5whisper
