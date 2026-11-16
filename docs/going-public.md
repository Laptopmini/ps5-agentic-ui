# Going public & the A2UI ecosystem submission

The last open M7 step is listing this project on the A2UI community ecosystem page. That is a
**public** action, and it only makes sense once the repo itself is public. This doc is the
checklist for that transition: the gates to clear first, then the exact submission steps.

Nothing here is enacted until the repo is public and each public action has an explicit go-ahead.
Everything below is prepared and reviewable in advance, so when the repo is ready "in your eyes"
there is no scramble.

## Why public is the trigger

Making the repo public is the hinge for several things at once:

- The frozen catalog id — `https://raw.githubusercontent.com/Laptopmini/ps5-agentic-ui/catalog-v1/catalog/ps5.catalog.json`
  (`catalogId`/`$id`, `PS5_CATALOG_ID`, the renderer's `kPs5CatalogId`) — only resolves for anyone
  else once the repo is public. Until then it is correct but returns 404 to anonymous clients.
- The A2UI listing criteria require an open-source repo (ours is MIT) and that reviewers can click
  through to the code, docs, install steps and a runnable example.
- The README, docs and LICENSE are written for an outside reader / LLM; public is the state they
  describe.

## Gates (clear before anything goes out)

- [ ] **Repo is public.** Owner's call, once it reads right from the outside. This also makes the
      frozen `catalogId` URL resolve.
- [ ] **Google CLA signed** by the owner as an individual — <https://cla.developers.google.com/>.
      Required by `a2ui-project/a2ui` CONTRIBUTING for any PR to that repo.
- [ ] **Per-action go-ahead.** Each public step below (fork, push, open PR, post to Discussions) is
      a separate action and needs its own go-ahead. Draft first, show, then act.

## Where it goes

The A2UI project moved from `google/A2UI` to **`a2ui-project/a2ui`** (the old path 301-redirects).
The ecosystem page is <https://a2ui.org/ecosystem/renderers/>, sourced from
`docs/site/src/content/docs/ecosystem/renderers.mdx` in that repo. Both of our entries live in that
one file: a **Community renderers** table and an **Agent libraries** table.

### A note on `Status`

The page has no legend defining its status labels. Observed usage: **Active** is the norm for
anything that has shipped a release and is maintained; **Experimental** / **Early stage** are used
for entries with no tagged release yet or an explicitly exploratory scope. We have a tagged
`v1.0.0-rc.1` and are maintaining it, so **Active** is defensible; **Experimental** is the more
conservative choice while it is still a release candidate. Pick one before filing.

## Submission steps (public actions — each needs a go-ahead)

1. [ ] Fork `a2ui-project/a2ui` to the `Laptopmini` account.
2. [ ] On a branch, add two rows to `docs/site/src/content/docs/ecosystem/renderers.mdx`:
       the renderer row to **Community renderers**, the agent-runtime row to **Agent libraries**
       (exact rows below).
3. [ ] Run the repo's formatter (`./scripts/fix_format.sh`) — it re-pads the whole table, so expect
       neighbouring rows' whitespace to shift. That is normal for these PRs.
4. [ ] Open one PR against `a2ui-project/a2ui` (title and body below). No AI disclaimer — their
       CONTRIBUTING does not require one.
5. [ ] (Optional) Share the project in their GitHub Discussions (draft below).

### Renderer row (Community renderers table)

```
| **[Laptopmini/ps5-agentic-ui](https://github.com/Laptopmini/ps5-agentic-ui)**<br />(CMake `a2ui`) · [Docs](https://github.com/Laptopmini/ps5-agentic-ui/blob/main/renderer/README.md) | PlayStation 5 (native C++) | `v1.0` | Active | Native, controller-driven renderer for the PlayStation 5; platform-independent C++ core (you supply a `Canvas`), with DualSense, haptics, and system-keyboard input. |
```

### Agent-runtime row (Agent libraries table)

```
| **[Laptopmini/ps5-agentic-ui](https://github.com/Laptopmini/ps5-agentic-ui)** · [Docs](https://github.com/Laptopmini/ps5-agentic-ui/blob/main/server/README.md) | TypeScript | Active | AG-UI agent runtime that emits A2UI v1.0 surfaces from a conversation and validates the model's tool-calls against a JSON-Schema catalog; runs as a Node server or embedded in QuickJS, any OpenAI-compatible model. |
```

### Pull request

**Title:** `docs(ecosystem): add ps5-agentic-ui (renderer + agent runtime)`

**Body:**

> Adds **ps5-agentic-ui** to the ecosystem list — a renderer and the agent runtime that drives it.
>
> **Renderer (Community renderers).** A native A2UI **v1.0** renderer for the
> PlayStation 5. The core is platform-independent C++ — you supply a `Canvas` — so it also runs on
> desktop via SDL2/GL. Input is the DualSense, haptics/adaptive triggers and the system keyboard.
> Supports core layout/text/input/action components plus shelves, a stats dashboard and voice
> (on-console STT). Packaged with CMake (`find_package(a2ui 1.0 CONFIG)` → `a2ui::a2ui`). MIT.
> Runnable example: `scripts/play.sh` builds the renderer and runs it against the agent on a
> desktop; `renderer/README.md` has build + integration instructions.
>
> **Agent runtime (Agent libraries).** A TypeScript AG-UI runtime that generates A2UI v1.0 surfaces
> from a conversation and validates the model's tool-calls against the same JSON-Schema catalog the
> renderer consumes, over AG-UI via CopilotKit. Runs as a Node HTTP server or embedded in QuickJS
> on the console; works with any OpenAI-compatible model. Docs: `server/README.md`.
>
> Repo: <https://github.com/Laptopmini/ps5-agentic-ui>
>
> CLA signed.

### GitHub Discussions post (optional)

**Title:** `ps5-agentic-ui — A2UI 1.0 on a PlayStation 5`

> Sharing a native A2UI **v1.0** renderer for the PlayStation 5, plus the agent runtime that drives
> it. Agent-driven surfaces become native, controller-driven PS5 screens, operated entirely from the
> DualSense (haptics, adaptive triggers, the system keyboard, and on-console voice). The renderer is
> a platform-independent C++ core (you supply a `Canvas`); the agent runtime is TypeScript (Node or
> QuickJS-embedded, any OpenAI-compatible model). MIT-licensed. Repo:
> <https://github.com/Laptopmini/ps5-agentic-ui> — happy to answer questions, and I can add a short
> demo capture.

## After it lands

- [ ] Confirm the frozen `catalogId` URL resolves anonymously once public.
- [ ] When 1.0 ships for real (dropping `-rc.1`), revisit the `Status` / `Versions` cells if needed.
