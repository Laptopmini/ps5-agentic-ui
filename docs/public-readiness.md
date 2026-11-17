# Public-readiness review

Working document for taking the repo public. Phase 1 (review) was done on 2026-11-16 against
`main` at `37dc122`. Phase 2 is in progress; the status columns are current.

## Phase 2 state (read this first after a context reset)

Owner decisions on 2026-11-16:

- Release **1.0.0 today**, not an rc.
- **No model vendor named anywhere** except OpenRouter as the default endpoint. There is no
  default model: `LLM_MODEL` is required. Provider switches go through `LLM_EXTRA_BODY`.
- The agent runtime is **packaged for npm before launch**.
- The icon and background are the owner's own work.
- The prebuilt PS5 zip is in scope for today.

Done, all uncommitted on `main` (nothing pushed, tagged or published):

- `runtime/` is the npm package `ps5-a2ui-agent`: `AgentThread` takes a model and tools,
  `serveAgent` is the HTTP server. The demo agent in `server/` is built on it.
  `runtime/llm.ts` was split into `runtime/model.ts`, `runtime/prompts.ts` and `eval/lib/session.ts`.
- `channel/` is `copilotkit-channels-ps5`. Both packages build, pack, install into a clean
  project and type-check there under `strict`.
- Console pairing: provider `minimax` replaced by `custom` (any OpenAI-compatible base URL), and
  a model id is now required. **This changes the console app and needs the on-device batch.** A
  `credentials.json` saved with the old provider is rejected, so the setup screen comes back.
- CI workflow, README, package READMEs, CONTRIBUTING, SECURITY, CHANGELOG, screenshots in
  `docs/images/`.
- Verified: typecheck; 30 TS tests; renderer 13/13; both packages build; a live conversation
  through the HTTP server and through the QuickJS bundle on the desktop host.

Still to do before the flip: commit; S1 (PS5 zip, licenses in the zip, the libiconv check); the
on-device batch; a Linux build check; the launch post draft; then the public steps in order.

Severity: **B** = launch blocker, **S** = should-fix (before or just after the flip), **N** = nice-to-have.
Effort is rough, for the two of us working together.

## Verdict

The engineering is in better shape than the presentation. Everything that can run off-console
runs and passes, the history is clean, and licensing is handled with unusual care. What falls
short is what an outsider meets first: there is no quickstart they can follow, nothing public
proves the tests pass, the headline eval numbers have no committed evidence, and "four reusable
libraries" overstates two of the four.

Against the three goals in `PLAN.md`:

| Goal | Met? | Notes |
|---|---|---|
| 1. Serve AG-UI from the console (cloud, LAN or on-console model) | Yes | Same `AgentThread` runs in Node and in QuickJS. |
| 2. Agent drives native PS5 UI | Yes | The strongest part: catalog, renderer, system keyboard and dialogs, haptics. |
| 3. A demo others can learn from | Partly | The app works; learning from it is hard without a quickstart, a prebuilt build or pictures. |
| "Libraries clean enough to adopt" | Two of four | Renderer (CMake) and channel adapter (npm) are real packages. The agent runtime is a reference app, and the device layer is reference code. |

## What I ran

| Check | Result |
|---|---|
| `pnpm typecheck` | Pass (note `strict: false` in both tsconfigs) |
| `pnpm test:runtime` | 7/7 pass |
| `pnpm test:channel` | 8/8 pass |
| `pnpm build:agent-js` | Pass, 396 kB bundle |
| `channel`: `pnpm build` + `npm pack --dry-run` | Pass, 8 files, 15.5 kB. pnpm first tried an install inside `channel/`, failed, and left a stray `channel/pnpm-lock.yaml` (removed) |
| Renderer: fresh out-of-tree CMake build + `ctest` | 13/13 pass (1 unit binary with 208 checks, 12 headless fixture renders). 3 warnings in `session.cpp` |
| `cmake --install` + an outside consumer using `find_package(a2ui 1.0 CONFIG)` | Pass: compiles, links, renders a surface into a `Store` |
| `eval/catalog-eval` via OpenRouter, `minimax/minimax-m3`, 16 scenarios | Intent tools **15/16**; freeform `render_a2ui` **1/16** (15 invalid, before the MiniMax argument repair that `rescore.ts` applies) |
| Headless screenshots of fixtures | Work, and look good enough for the README |
| History scan: secrets, personal paths, large blobs | Clean. Only test dummies (`sk-or-1`), example LAN IPs, and the author email in `channel/package.json` |
| GitHub metadata | Private; description, 16 topics and MIT detected. No releases, Discussions off, no `.github/` |
| npm | `copilotkit-ps5` is unclaimed. `@copilotkit/channels-core` latest is 0.12.0, with 0.13.0-rc.1 on `next` |

Not run: anything on the console, the other four eval harnesses (`multiturn`, `domain-tools`,
`hardening`, `intelligence-threads`), the Docker payload build, and a Linux build of the renderer.

## Launch blockers

| # | Item | Effort | Status |
|---|---|---|---|
| B1 | **README has no quickstart an outsider can follow.** "Run it" assumes cmake, SDL2, pnpm and a Node that runs `.ts` directly (23.6+; we're on 24), none of which is stated. The only path shown needs a paid model key. There is no picture. Time to something on screen today: without a PS5 and without a key, about 2 minutes, but only if you find `a2ui-host fixtures/*.jsonl` in `renderer/README.md`; with a PS5, 30+ minutes of Docker and ggml builds, with no steps in the README. Fix: a three-tier quickstart (no key → with key → on a PS5), prerequisites, a hero screenshot, and "who is this for". | 2–3 h | done (README rewritten; Linux lines unverified until CI runs) |
| B2 | **No CI.** The tests pass locally and nothing shows it. Add a GitHub Actions workflow: typecheck, both TS suites, channel build + pack, renderer build + `ctest` on macOS and Linux. Needs a root `pnpm test`. Linux is untested, so expect a fix or two. | 1–2 h | workflow written; first run happens on push. Linux build untested locally (Docker was not running) |
| B3 | **The agent runtime is presented as a reusable library, and the A2UI submission lists it under "Agent libraries".** It isn't installable, and `AgentThread` hard-wires the simulated console and its domain tools; an adopter can only add tools as AG-UI client tools. Every project in that A2UI table is an installable package. Fix for launch: call it the *reference agent* in the README and PLAN, and submit only the renderer row. Packaging it properly is N4. | 30 min | done: packaged as `ps5-a2ui-agent` (owner chose packaging over reframing) |
| B4 | **The README's eval command contradicts the eval's defaults.** The README says OpenRouter is the default and suggests `node eval/catalog-eval/run.ts`. The harnesses default `LLM_BASE_URL` to `https://api.minimax.io/v1` and to model ids `MiniMax-M3,MiniMax-M3.1-Flash`. Someone following the README with an OpenRouter key sends that key to MiniMax and gets a 401. Fix: share the runtime's base URL and model defaults. | 30 min | done |
| B5 | **Public docs that contradict the code.** `renderer/README.md` lists "no images" and "no scrolling" as known gaps (both exist now) and says "the PS5 build will reuse them" in the future tense. `channel/example.ts` shows "Catalog: PS5 v0". BACKLOG says emoji draw as a box while emoji fallback fonts are vendored (needs your confirmation on the console). | 30 min | done, except the emoji backlog item (console check) |
| B6 | **Asset provenance, owner to confirm.** `payload/*/sce_sys/icon0.png` and `renderer/assets/background.png` look original, but nothing says so. Confirm they are yours and I'll add a line to THIRD-PARTY-NOTICES. | 5 min | done (owner confirmed; noted in THIRD-PARTY-NOTICES) |

## Should-fix

| # | Item | Effort | Status |
|---|---|---|---|
| S1 | **No prebuilt PS5 build.** A PS5 owner should be able to download a zip from a GitHub Release, FTP it to `/data/homebrew/PS5AgenticUI` and press Square for the offline demo. The zip must carry the third-party licenses. Check first whether `libiconv` (LGPL) is statically linked into `console.elf`; if so the zip needs a relinking offer or iconv has to go. | 2–3 h + console check | open: needs Docker and the console |
| S2 | **CMake package is awkward to consume from source.** `renderer/CMakeLists.txt` always builds tests, QuickJS, `ps5system` and the host, and `find_package(SQLite3 REQUIRED)` fails a FetchContent consumer who has no SQLite. Gate everything except `a2ui` behind `PROJECT_IS_TOP_LEVEL`, document a FetchContent snippet, and add a 20-line consumer example that CI builds. | 1–2 h | done (`A2UI_BUILD_DEV`, `renderer/examples/hello`) |
| S3 | **Channel package details.** Peer range `>=0.12.0` on a 0.x dependency admits 0.13, which can break; use `^0.12.0` and test against 0.13.0-rc.1. The example depends on the internal `ɵruntime.start()`. The npm README doesn't say what the console side needs or link the wire protocol. `channel/` isn't a pnpm workspace member, which caused the failed install above. | 1–2 h | done: renamed `copilotkit-channels-ps5`, peer `^0.12.0`, workspace member, README. 0.13 rc not tested |
| S4 | **The agent's run loop has no tests.** `AgentThread` (approval gate, secret handles, client tools, action intake) and the server's access-code gate are covered only by evals that need a model key. `complete()` is a module-level function reading the environment at import, so there's no seam for a scripted model. Add the seam and about eight tests. | 3–4 h | done: 15 tests with a scripted model (`runtime/thread.test.ts`) |
| S5 | **Eval evidence isn't in the repo.** `docs/agent.md` cites "~63/64" and raw results are git-ignored. Commit a short results table per harness with model, date and command. The intent-versus-freeform gap (15/16 vs 1/16 here) is the project's most quotable finding and isn't written down anywhere. | 1 h + key spend | open: finding is in README and agent.md; no committed results table (in BACKLOG) |
| S6 | **`runtime/llm.ts` mixes the live runtime with the eval harness** (`runSession`, the scripted `Player`, fault injection, `pool`), and its header still reads "features under test". `server/thread.ts` imports from `channel/`. Split live code from eval code and move the shared tool definitions into `runtime/`. | 2 h | done |
| S7 | **Renderer scope isn't stated.** `View` is the whole assistant screen (conversation, message bar, key-setup screen, voice state), not a generic catalog widget, and it draws only the PS5 catalog, not the A2UI basic catalog. The A2UI criteria mention "core layout, text, input and action components", so say this plainly in `renderer/README.md` and the PR body. | 30 min | done |
| S8 | **Internal jargon in code comments.** 51 references to "spike 01", "finding 25", "Track B", "M6" and the like point at documents that aren't in the repo. | 1 h | done |
| S9 | **Missing setup docs.** How the whisper model (`ggml-base.en.bin`) gets onto the console; which hosts are supported (the scripts use macOS-only `route`, `ipconfig`, `stat -f`, `nc -G`); the Node version; an on-device verification checklist. | 1 h | partly: voice model and Node version documented; on-device checklist below; scripts still macOS-only |
| S10 | **Community files.** `SECURITY.md` matters here because the project listens on the LAN and handles model keys. Also CONTRIBUTING, a CHANGELOG, issue templates, and Discussions on. | 45 min | done except issue templates; Discussions and private vulnerability reporting are repo settings (owner) |
| S11 | **Package name.** `copilotkit-ps5` is an unscoped third-party name carrying CopilotKit's brand. You know the team; one message to confirm they're comfortable before the name is taken. | 1 message | done: follows `@copilotkit/channels-<platform>` |
| S12 | **Server smalls.** Unbounded request body, threads never evicted, every event (including user text) appended to `server/logs/` with no rotation or mention in the README. | 1 h | done (body cap, thread eviction, logs documented) |
| S13 | **Versions disagree.** Tag `v1.0.0-rc.1` is one commit behind `main`; CMake says 1.0.0, npm says 1.0.0-rc.1, the root package has none. Cut `rc.2` at the launch commit. | 15 min | versions set to 1.0.0; tag at launch commit |
| S14 | Three `-Wmissing-field-initializers` warnings in `renderer/src/session.cpp`. | 10 min | done |

## Nice-to-have

| # | Item | Effort |
|---|---|---|
| N1 | A no-key demo for `scripts/play.sh`: a scripted agent that replays the fixtures as a conversation. | 2–3 h |
| N2 | Golden-image render tests. Today the fixture tests only check that nothing errors. | 2 h |
| N3 | Coverage numbers (c8, llvm-cov) in CI. | 1 h |
| N4 | Publish the agent runtime as an npm package with injectable tools and console. This is the real fix for B3 and earns the "Agent libraries" row. | about 1 day |
| N5 | A short GIF of a real conversation, and the app icon as the social preview. | 1 h + console capture |
| N6 | `strict: true` for the TypeScript. | half a day |
| N7 | Linux versions of the `ps5-*.sh` scripts. | 2 h |

## Code quality by part

- **Renderer (C++).** Clean model/view/canvas split, honest abstraction, good test density, and the
  package installs and links from outside. Weak spots: `view.cpp` is 1,500 lines doing layout,
  focus, dialogs and app chrome; `qrcodegen` and the key-setup screen live in the core library.
- **Agent runtime (TS).** Careful where it counts: secrets as handles, an approval gate owned by
  the runtime, retries only before anything is shown, a tolerant stream parser with tests. Held
  back by S4 and S6, and by not being a library (B3).
- **Channel adapter (TS).** The most adoptable piece: small, documented wire protocol, token
  required off loopback, bounded history. Needs S3.
- **Device layer.** Portable haptics model is tested; the console bindings are reference code and
  the README already says so.
- **Demo app.** Good showcase. As a reference it's harder: `payload/console/main.cpp` is 840 lines
  with three agent sources, pairing, voice and rendering in one file. A paragraph in the README
  mapping "where to look for X" would do more than a refactor.

## Publishing strategy

Every step marked ⚑ is a public action that needs its own go-ahead.

1. Commit the Phase 2 work. Run the on-device batch. Build the PS5 zip.
2. Tag `v1.0.0` on the launch commit. ⚑ Push `main` and the tag.
3. ⚑ Flip the repo public. Check that CI is green on macOS and Linux and that the frozen
   `catalogId` URL resolves anonymously. Turn on Discussions and private vulnerability reporting.
4. ⚑ GitHub release `v1.0.0` with the PS5 zip attached.
5. ⚑ `npm publish` for `ps5-a2ui-agent` and `copilotkit-channels-ps5` (the repo must be public
   first so the package pages link somewhere).
6. ⚑ Post on the CopilotKit Discord: the picture, then the adapter, then the eval finding.
7. ⚑ Open the A2UI ecosystem PR (both rows; drafts in `going-public.md`). The Google CLA has to be
   signed first.

CMake distribution stays source-based (the git tag plus FetchContent).

## On-device batch (owner at the console)

Prerequisites: OrbStack/Docker running on the Mac; `ftpsrv` and `websrv` running on the console;
`PS5_HOST` set. Claude can run the builds, uploads and launches; the owner watches the TV and
presses buttons.

| # | Run | Look for |
|---|---|---|
| 1 | `PS5_HOST=… scripts/ps5-run.sh console` | The app builds, installs and opens. Because the stored key used the old provider, the **setup screen** should appear with a QR code and a six-digit code. |
| 2 | Press **Square** on the setup screen | The offline demo: sample screens, D-pad moves focus, Cross activates, Circle goes back. Do emoji in the canned conversation draw as glyphs or boxes? |
| 3 | Open the setup page from a phone (QR) | Four choices: OpenRouter, Another OpenAI-compatible endpoint, My own server, A CopilotKit channel. **Model** is a required field. Send an OpenRouter key and a model id. |
| 4 | Ask "what games do I have?" | A shelf of the real installed games; picking one gives a menu. Haptics on activate. |
| 5 | L1+R1 → setup → choose **Another OpenAI-compatible endpoint** with a base URL | Pairs and answers the same way (any endpoint you have; skip if none). |
| 6 | Hold **R2**, speak, release | Transcript is sent as your message (needs `ggml-base.en.bin` in the app folder). |
| 7 | `PS5_HOST=… LLM_API_KEY=… LLM_MODEL=… scripts/ps5-play.sh` | The app talks to the agent on the Mac; asking it to delete one of the (simulated) saves brings up a confirm dialog first. |
| 8 | `PRESENCE=1 PS5_HOST=… scripts/ps5-channel.sh`, say "remind me in 30 seconds to stretch", close the app | A system notification arrives; opening it brings the app back at that message. |

## Stretch goals: early read

Full recommendations come after Phase 2. Where the review already points:

1. **Non-agentic tooling.** The renderer already draws a JSONL file with no agent involved. What's
   missing is a static example app and LLM-readable docs.
2. **Resident channel service.** `payload/presence/` already is a resident payload holding the
   channel open, with notifications and choices. The question is hardening it, not building it.
3. **README visuals.** Yes to both; folded into B1 and N5.
