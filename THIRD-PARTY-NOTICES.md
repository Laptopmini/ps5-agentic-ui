# Third-party notices

This project is **MIT** licensed (see [`LICENSE`](LICENSE)). It bundles and links third-party
software that keeps its own license, listed below. One component is more restrictive: the
home-screen tile (`payload/tile/`) is **GPL-3.0-or-later** because it links drakmor's native-app
boilerplate — it is a separate program that only asks websrv to start the app, and does not affect
the license of the rest of this project.

Exact versions and checksums for the vendored code are pinned in
[`renderer/vendor/VERSIONS`](renderer/vendor/VERSIONS); the A2UI schemas in
[`vendor/a2ui-v1_0/`](vendor/a2ui-v1_0/).

## Vendored — compiled into the renderer / app (in this repository)

| Component | Version | License | Path |
|---|---|---|---|
| nlohmann/json | 3.12.0 | MIT | `renderer/vendor/nlohmann/` |
| stb (`stb_truetype.h`, `stb_image.h`) | 2.30 | MIT **or** Public Domain | `renderer/vendor/stb/` |
| Noto Sans + fallback fonts (Emoji, Symbols, Symbols 2) | google/fonts (2026-11) | SIL OFL 1.1 | `renderer/vendor/fonts/` |
| QuickJS | 2026-06-04 | MIT | `renderer/vendor/quickjs/` |
| TweetNaCl (C) | 20140427 | Public Domain | `renderer/vendor/tweetnacl/` |
| tweetnacl-js (`nacl.min.js`) | 1.0.3 | Unlicense | `renderer/vendor/tweetnacl/` |
| QR-Code-generator (C++) | 1.8.0 | MIT | `renderer/vendor/qrcodegen/` |
| A2UI 1.0 schemas (a2ui-project/a2ui, formerly google/A2UI) | ee0fe35 (2026-11-09) | Apache-2.0 | `vendor/a2ui-v1_0/` |

## Linked into a console build — distributed alongside the app

Provided by the [ps5-payload-dev](https://github.com/ps5-payload-dev) pacbrew bundle (static archives
linked into `eboot.elf`, and the Mesa shared libraries shipped in the app folder), plus whisper.cpp
cross-built by `scripts/ps5-ggml.sh`. These are not in this repository; a **published build** that
bundles them carries their licenses:

| Component | License |
|---|---|
| SDL2 | Zlib |
| Mesa — RADV Vulkan, zink, EGL, GLES (`libvulkan`, `libEGL`, `libGLESv2`, `libgallium`) | MIT |
| whisper.cpp + ggml (v1.9.5) | MIT |
| Whisper `ggml-base.en` model weights (OpenAI Whisper) | MIT |
| libcurl | curl (MIT/X-style) |
| OpenSSL 3.x | Apache-2.0 |
| SQLite3 | Public Domain |
| libsamplerate (0.2.x) | BSD-2-Clause |
| zlib | Zlib |
| zstd | BSD-3-Clause |
| libpsl | MIT |
| libiconv | LGPL-2.1-or-later |

PlayStation 5 system libraries (`libSce*`, `libkernel`) are Sony's and are resolved on the console at
run time; they are neither bundled nor redistributed here.

## npm dependencies (TypeScript libraries)

The agent runtime and the channel adapter depend on npm packages (`@copilotkit/*`, `@ag-ui/client`,
`ajv`, and build-time `esbuild`, `typescript`) resolved by the package manager; each keeps its own
license as published. `@copilotkit/channels-core` is a **peer** dependency of `copilotkit-ps5`.

## Build / reference only — not distributed

Read as documentation of the API shape while building this project, and **not** redistributed here:

- **ProsperoAI** (quatrixone), GPL-3.0 — consulted for the `sceAudioIn` mic recipe and the
  ggml-vulkan/RADV build approach; this project's code is its own.
- The **ps5-payload-dev SDK** and toolchain, and **drakmor's native-app boilerplate** (used only by
  the GPL-3.0 tile above), keep their own licenses.
