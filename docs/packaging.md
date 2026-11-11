# Packaging & running an app on a jailbroken PS5

Reusable notes for PS5 homebrew developers: how to get a C++ app with a window, networking and the
GPU onto a jailbroken console, and the trade-offs between the two distribution shapes. Verified on
firmware **12.70** (Relapse + kstuff + ShadowMountPlus).

## The toolchain

- **PS5 Payload SDK** (`ps5-payload-dev/sdk`): `prospero-clang`/`prospero-clang++` wrappers over host
  clang/lld, a FreeBSD/x86_64 sysroot, and stub `.sprx` imports for the system libraries. A
  CMake toolchain file (`prospero.cmake`) and a make include (`prospero.mk`) are provided.
- **Prebuilt libraries** come from the [pacbrew](https://github.com/ps5-payload-dev/pacbrew-repo)
  bundle: static archives (SDL2, libcurl, OpenSSL, SQLite, libsamplerate, freetype, QuickJS, …) and
  Mesa shared objects. Pin a bundle release and check its sha256.
- We build payloads inside a Debian container (`payload/Dockerfile`) so the toolchain is reproducible
  on any host; `scripts/ps5-build.sh <name>` compiles `payload/<name>` in it.
- **On-console loaders:** `ftpsrv` (:2121) for file transfer, `websrv` (:8080) for launching apps,
  and the ELF loader (:9021) for one-shot payloads (`(cat x.elf; sleep N) | nc <console> 9021`).

## A payload needs websrv to get a window

A bare payload ELF sent to the ELF loader (:9021) runs, but **has no video-out** — it can't open a
window. The one proven way for an SDL/GPU homebrew app to get the display is to be launched through
**websrv's homebrew loader** (`/hbldr`), which sets up the process so SDL's PS5 backend can present
through `sceVideoOut`. So the app runs as a **payload launched by websrv**, not a free `:9021`
payload. websrv also sets the working directory and `LD_LIBRARY_PATH` to the app folder, which the
app relies on to find its data files and the Mesa `.so`s.

**Install layout** (`/data/homebrew/<APP>/`): `eboot.elf`, `homebrew.js` (the launcher entry),
`sce_sys/icon0.png`, the Mesa `.so`s next to the app, and any data files. `scripts/ps5-install.sh`
uploads them over FTP (size-checked for the big ones). The app also `chdir`s to its own folder at
startup in case it's launched from elsewhere.

### Two ways to launch it

1. **websrv's Homebrew Launcher list** — websrv lists every `/data/homebrew/*` folder that has a
   `homebrew.js` or `eboot.elf`, using `sce_sys/icon0.png` as its picture. `homebrew.js` names the
   app and returns the launch request (`path`, `cwd`, `env.LD_LIBRARY_PATH`). No license
   implications; nothing extra to build.
2. **A home-screen tile** (`payload/tile/`) — a tiny **native title** (`PPSA95210`) with the app's
   icon that, when opened, asks websrv on `127.0.0.1:8080` to launch the app (the same request
   `homebrew.js` makes). websrv closes the tile and runs the app in its place. Built from drakmor's
   [ps5-native-app-boilerplate](https://github.com/drakmor/ps5-native-app-boilerplate) and registered
   by ShadowMountPlus (`scripts/ps5-tile.sh`).
   - **Two side effects of the hand-off:** a *double fade* (the tile starts, then websrv closes it
     and launches), and the Switcher/recents show **"Homebrew Launcher"** because websrv hard-codes
     the placeholder title `FAKE00000` in `hbldr.c` and swaps the ELF into it (a `titleId` option on
     `/hbldr` would fix this — an upstream gap).
   - **License:** because the tile links the GPL-3.0 boilerplate's startup code, the tile is
     **GPL-3.0-or-later** — a separate program from the MIT app, which it only asks websrv to start.

## Payload vs. native title (the real distribution choice)

The app above is a **payload**. The alternative is a true **native title** — a signed FSELF
`eboot.bin` registered by ShadowMountPlus and launched straight from the home screen, no websrv, no
hand-off. That's the **ProsperoStore-distributable** form. quatrixone/ProsperoAI (GPL-3.0) ships one:
a full app with libcurl/OpenSSL, an HTTP server, Vulkan graphics, threads and `/data` access, so none
of those are blockers for the native route. Its `native_app_builder` host tool does it in two steps
(`link` an LLVM PIE against the SDK stubs → `self --sign` into a signed FSELF).

The one architectural difference that *is* real work:

> **A native title links everything statically and `dlopen`s nothing.** Its clean-room runtime
> (`libc.prx` + CRT) lists no dynamic-loader imports — no `dlopen`/`dlsym`, no `sceSysmoduleLoadModule`.

Our app's graphics are the opposite shape: SDL2 **`dlopen`s Mesa at runtime** (`libEGL.so.1`,
`libGLESv2.so.2`, `libgallium.so`, `libvulkan.so.1`), found via `LD_LIBRARY_PATH`. And at run time a
payload reaches resident system modules (e.g. `libSceAudioIn`) by `dlopen`ing their `.sprx` — which a
native title's clean-room runtime also can't do. So moving to a native title means **statically
linking the graphics/system backends** (as ProsperoAI does with static RADV), not flipping a flag.
Attempts to package the existing payload as a native title hit **CE-107750-0**: a clean-room CRT
paired with the system `libSceLibcInternal` gives a mismatched startup/TLS model, so the title loads
but initialises wrongly. The correct path is a clean-room `libc.prx` rebuilt with the full import set
the app needs (ProsperoAI's approach). It's a bounded sub-project, kept in the backlog.

**Practical gotchas**

- `_exit()` to quit. A payload that linked libcurl 8.x + OpenSSL 3.x **aborts after `main` returns**
  (`resource deadlock would occur` in a static destructor); ending with `_exit(0)` avoids it. The app
  also asks the system to close itself (`sceSystemServiceKillApp` on its own app id) since there's no
  clean teardown for a websrv-launched payload.
- **No multitasking.** A game-type title can't run in the background; opening the app switches away
  from a running game (and rest mode is refused while it runs). This is a PS5 system limitation, not
  something the native-title work changes.
- **CE-107750-0** can also mean the title folder needs `777` permissions — set them over FTP.
- A tile-launched app has **no stdout receiver** — write logs to a file in the app folder and fetch
  them over FTP.
