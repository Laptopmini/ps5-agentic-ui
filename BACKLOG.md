# Backlog

Tracked but not yet done. Status as of 1.0. See [`docs/PLAN.md`](docs/PLAN.md) for the
goal and architecture, and the topical docs under [`docs/`](docs/) for the research.

## Milestones remaining

- **A2UI ecosystem listing.** 1.0 is cut and the catalog is frozen at the `catalog-v1` tag
  (`catalogId`/`$id`, `PS5_CATALOG_ID`, the renderer's `kPs5CatalogId`). Submitted to the A2UI
  ecosystem page's catalogs table in
  [a2ui-project/a2ui#4269](https://github.com/a2ui-project/a2ui/pull/4269); left: respond to review
  until it merges.
- **OpenDots on the PS5 (M5).** Plug OpenDots into the channel / AG-UI path on the frozen 1.0 surface
  (text first, voice already landed). The external acceptance test for 1.0.
- **Local-model track (M8, optional).** Intent-tool eval of a 7–9B model (via ProsperoAI on the PS5)
  against a hosted model on the same eval; OpenDots with ProsperoAI as its model. Findings, not a
  dependency.
- **A no-key demo on the desktop.** A scripted agent for `scripts/play.sh` that replays a
  conversation, so the agent path can be tried without a model key (the console app has one).
- **Committed eval results.** A results table per harness (model, date, command) in `eval/`.

## Native title & publishing (a bounded sub-project)

- **Native title (item m).** Package the app as its own signed FSELF title (home-screen launch, no
  websrv, ProsperoStore-distributable) instead of a websrv-launched payload. The real work is a
  clean-room `libc.prx` rebuilt with the full import set, plus statically linking the graphics/system
  backends (see [packaging](docs/packaging.md); stock attempts hit CE-107750-0). WIP in
  `scripts/ps5-native.sh`, `payload/native-smoke/`.
- **Publishing (item n).** A published build installs the native title (distributable on the
  ProsperoStore) and the resident payload into Payload Manager. Development stays as-is.
- **websrv `titleId` on `/hbldr`** (upstream, I-042) would let the tile launch into `PPSA95210`
  (correct Switcher/recents name) instead of the hard-coded `FAKE00000`. The double fade stays.

## Device layer (haptics & controller)

- **HD haptics** (voice-coil actuators, waveform-driven) — can a payload reach them? Spike.
- **Controller speaker** — short sounds from the DualSense. Same unknown as HD haptics.
- **More adaptive-trigger modes** (multi-position resistance, slopes) — not verified on the console.
- **Light-bar animations** (pulse, fade) and player LEDs, as patterns like the haptics.
- **Haptic settings** — app on/off and intensity; read the system vibration setting if a payload can.
- **Other renderers' mappings** for the named effects (Core Haptics on a phone, the Gamepad API's
  `vibrationActuator` on the web).

## App polish

- **PS button overlay → Home does nothing** in the app (a PS5 no-multitasking consequence).
- **Emoji in replies** draw as a box on the console (stb_truetype, one font weight).

## Upstream issues (in the owner's separate issue-tracker)

Found while building; tracked outside this repo. The ones that touch this project:

- **ps5-payload-dev/pacbrew (I-031):** payloads linking libcurl 8 + OpenSSL 3.5 abort after `main`
  returns (`resource deadlock`); `_exit()` avoids it.
- **ps5-payload-dev/websrv (I-042):** `/hbldr` always launches `FAKE00000`; no `titleId` to run
  homebrew under its own title.
- **ggml-org/ggml (I-043):** ggml-vulkan requires `SPIRV-Headers` but doesn't add its include to the
  target, so cross builds fail (`spirv/unified1/spirv.hpp` not found). One-line fix.
- **drakmor/ps5-native-app-boilerplate (I-032):** README/`gh repo create --template` point at a repo
  that no longer resolves.
