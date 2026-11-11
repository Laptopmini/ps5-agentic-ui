# Voice: mic capture and on-console speech-to-text

Hold **R2**, speak, release → the DualSense mic is captured and transcribed on the PS5 GPU, and the
text is sent as the player's turn. Nothing leaves the console. Verified on firmware **12.70**.

## Capturing the DualSense mic (reusable recipe)

The SDK ships only audio **output** (`libSceAudioOut`), but `libSceAudioIn` is resident on the
console. The mechanism to reach it depends on how your app runs:

- A **native title** imports `libSceAudioIn` through its module import table (what ProsperoAI does).
- A **payload** (launched by websrv/the ELF loader) has no import table and can't use the import stub,
  `dlsym(RTLD_DEFAULT, …)` or `sceSysmoduleLoadModule`. What works is `dlopen` of the resident
  `.sprx` by path, then `dlsym` — the same way the app reaches the system dialog modules.

Payload recipe (16 kHz mono 16-bit PCM):

```c
dlopen("/system/common/lib/libSceAudioIn.sprx", RTLD_LAZY);  // plain SONAME doesn't open; full path does
dlsym → sceAudioInInit / sceAudioInOpen / sceAudioInInput / sceAudioInClose
sceUserServiceInitialize(nullptr);                 // a payload doesn't get the user service for free
sceUserServiceGetForegroundUser(&user);            // GetInitialUser returns -1 here → open fails
sceAudioInInit();
int h = sceAudioInOpen(user, /*type*/1, /*idx*/0, /*grain*/256, /*rate*/16000, 0);
while (recording) sceAudioInInput(h, block);       // 256-sample blocks; track peak for a simple VAD
sceAudioInClose(h);
```

Two gotchas that cost us console iterations: the user **must** be the foreground user after
`sceUserServiceInitialize` (not `GetInitialUser`), and because R2 is an **analog axis** (not a button)
capture is driven from `SDL_JOYAXISMOTION`. Run capture on a worker thread and `join()` it before
reuse — a leftover joinable `std::thread` calls `std::terminate()`.

See `renderer/src/ps5_voice.{hpp,cpp}`.

## Speech-to-text on the GPU (on-console, private)

We run **whisper.cpp** on the RADV GPU via **ggml-vulkan**, on the **stock SDK driver** — no patched
Mesa fork, no native title (see [rendering](rendering.md) for why the stock `libvulkan.so.1` is
enough). `scripts/ps5-ggml.sh` cross-compiles whisper.cpp + ggml-vulkan as PS5 static libraries;
they link into the console app and `dlopen` the RADV `libvulkan.so.1` the app already ships.

`renderer/src/ps5_whisper.{hpp,cpp}` wraps it: lazy-load the model on the GPU (`use_gpu = true`),
convert the captured PCM to float, run `whisper_full`, return the transcript. It runs on a worker
thread so the UI keeps animating; the result is sent as the player's turn on the main thread.

**Measured** (firmware 12.70, `ggml-base.en` on the RADV GPU): model load **~0.2 s** (first clip
~1 s with GPU warm-up), transcription **~0.1 s for a ~3 s clip** (~30× realtime). Accuracy is good on
clear speech; `base.en` occasionally slips word order (`small.en` fixes it, with latency to spare).

The key result for other homebrew: **on-device STT is practical on the PS5 GPU today**, and the GPU
compute substrate it needs (ggml-vulkan on stock RADV, in-app) is reusable for any ggml model —
llama, embeddings, TTS — not just whisper.
