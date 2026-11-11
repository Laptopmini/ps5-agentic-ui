// Mic capture on the console (M6, docs/voice.md): records the DualSense microphone as 16 kHz
// mono 16-bit PCM, the form speech-to-text wants. The audio-in calls aren't in the payload SDK, so
// the resident libSceAudioIn.sprx is loaded at run time (sceKernelLoadStartModule) and its exports
// resolved by handle (sceKernelDlsym) — how a ps5-payload-dev payload reaches a system module that
// isn't stubbed in the SDK; off the console there's no mic and Recorder::available() is false.
// Recording runs on its own thread so the UI keeps drawing; the host asks for the level() to show a meter.
//
// The recipe (sceAudioInInit, sceAudioInOpen at 16 kHz, sceAudioInInput blocks, sceAudioInClose)
// was learned from quatrixone/ProsperoAI's voice_recorder (read as documentation of the API; our
// own code). What to do with the PCM — cloud STT through the channel, or whisper.cpp on the
// console — is the next step; toWav() packages it either way.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ps5voice {

struct Recording {
  std::vector<int16_t> samples;  // mono
  int sampleRate = 16000;
  double seconds() const { return sampleRate ? double(samples.size()) / sampleRate : 0; }
};

/** Why a recording isn't usable, so the agent can tell the player. */
enum class Status { Ok, NoMic, TooShort, Silent, Failed };

class Recorder {
 public:
  Recorder();
  ~Recorder();
  Recorder(const Recorder&) = delete;
  Recorder& operator=(const Recorder&) = delete;

  /** The mic API is present (the console). False on the host, or if the module can't be loaded. */
  bool available() const;

  /** Begin capturing (on a worker thread). Returns false if unavailable or already recording. */
  bool start();
  /** Stop and return what was captured, with a Status. Safe to call when not recording. */
  Recording stop(Status* status = nullptr);
  bool recording() const { return recording_.load(); }

  /** The most recent block's peak, 0..1, for a level meter while recording. */
  float level() const { return level_.load(); }
  /** Seconds captured so far. */
  double seconds() const;
  const std::string& error() const { return error_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::atomic<bool> recording_{false};
  std::atomic<float> level_{0};
  std::string error_;
};

/** 16-bit mono PCM → a WAV file in memory (RIFF/PCM), ready to save or send to STT. */
std::vector<uint8_t> toWav(const Recording& rec);

/** Route the recorder's step-by-step progress/errors to the host log (console-log.txt). Optional;
 *  used to pinpoint where a mic call fails on the console. Pass nullptr to silence. */
void setLog(void (*fn)(const char* line));

}  // namespace ps5voice
