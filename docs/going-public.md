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
for entries with no tagged release yet or an explicitly exploratory scope. With `v1.0.0` tagged
and both npm packages published, **Active** is the right label.

### A note on scope

The listing criteria say client renderers should support core layout, text, input and action
components. This renderer draws a **custom catalog** built for a TV and a controller (the PS5
catalog), not the A2UI basic catalog. The row and the PR body say so plainly rather than leaving a
reviewer to find out.

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
| **[Laptopmini/ps5-agentic-ui](https://github.com/Laptopmini/ps5-agentic-ui)**<br />(CMake `a2ui::a2ui`) · [Docs](https://github.com/Laptopmini/ps5-agentic-ui/blob/main/renderer/README.md) | PlayStation 5 (native C++; also desktop via SDL2) | `v1.0` | Active | Native, controller-driven renderer for a custom TV-and-controller catalog (menus, shelves, dashboards, system keyboard and dialogs). Platform-independent C++ core: you supply a `Canvas`. |
```

### Agent-runtime row (Agent libraries table)

```
| **[Laptopmini/ps5-agentic-ui](https://github.com/Laptopmini/ps5-agentic-ui)**<br />(`ps5-a2ui-agent`) · [Docs](https://github.com/Laptopmini/ps5-agentic-ui/blob/main/runtime/README.md) | TypeScript | Active | AG-UI agent runtime that turns flat intent tools into A2UI v1.0 surfaces, validates surfaces against a JSON-Schema catalog, and gates tools behind user approval. Node or QuickJS, any OpenAI-compatible model. |
```

### Pull request

**Title:** `docs(ecosystem): add ps5-agentic-ui (renderer + agent runtime)`

**Body:**

> Adds **ps5-agentic-ui** to the ecosystem list: a renderer and the agent runtime that drives it.
>
> **Renderer (Community renderers).** A native A2UI **v1.0** renderer for the PlayStation 5. The
> core is platform-independent C++ (you supply a `Canvas`), so it also runs on desktop via SDL2,
> which is how to try it without a console. It draws a **custom catalog** designed for a TV and a
> controller (screens, menus, shelves of tiles, dashboards, text and voice prompts, confirms,
> notices) rather than the basic catalog; input is the DualSense, with haptics, adaptive triggers
> and the system keyboard. Packaged as a CMake target (`a2ui::a2ui`, via FetchContent or
> `find_package`). MIT.
> Runnable example: `renderer/examples/hello` (no dependencies), or
> `a2ui-host renderer/fixtures/home-shelves.jsonl` for a window; steps are in the README.
>
> **Agent runtime (Agent libraries).** `ps5-a2ui-agent` on npm: a TypeScript AG-UI runtime that
> generates A2UI v1.0 surfaces from flat "intent" tool calls, validates surfaces against the same
> JSON-Schema catalog the renderer consumes, and adds an approval gate and secret handles. Runs
> in Node or embedded in QuickJS; works with any OpenAI-compatible model.
> Docs: `runtime/README.md`.
>
> Repo: <https://github.com/Laptopmini/ps5-agentic-ui>
>
> CLA signed.

### GitHub Discussions post (optional)

**Title:** `ps5-agentic-ui — A2UI 1.0 on a PlayStation 5`

> Sharing a native A2UI **v1.0** renderer for the PlayStation 5, plus the agent runtime that drives
> it. Agent-driven surfaces become native, controller-driven PS5 screens, operated entirely from the
> DualSense (haptics, adaptive triggers, the system keyboard, and on-console voice). The renderer is
> a platform-independent C++ core (you supply a `Canvas`) and runs on a desktop too; the agent
> runtime is TypeScript (`ps5-a2ui-agent` on npm; Node or QuickJS, any OpenAI-compatible model).
> MIT-licensed. Repo:
> <https://github.com/Laptopmini/ps5-agentic-ui> — happy to answer questions, and I can add a short
> demo capture.

## After it lands

- [ ] Confirm the frozen `catalogId` URL resolves anonymously once public.

## Launch post (CopilotKit Discord)

Draft. Attach `docs/images/conversation.png`, or a short capture from the console once there is one.

> **ps5-agentic-ui 1.0: agentic UI on a PlayStation 5, with AG-UI, A2UI 1.0 and CopilotKit Channels**
>
> I've been building open-source tooling to put agents on a PS5, and it's public now:
> <https://github.com/Laptopmini/ps5-agentic-ui>
>
> The agent doesn't answer with text. It answers with native, controller-driven screens (menus,
> shelves of tiles, dashboards, the system keyboard, system dialogs, haptics), described in
> A2UI 1.0 and carried over AG-UI the way CopilotKit does it.
>
> What's in it:
> - **`copilotkit-channels-ps5`**: a Channels `PlatformAdapter`, so the PS5 is a channel like Slack
>   or Discord. Channels UI you post is drawn as PS5 screens; while the app is closed, messages
>   arrive as system notifications over whatever game is running.
> - **`ps5-a2ui-agent`**: an AG-UI agent runtime in TypeScript. The same code runs in Node and
>   embedded in QuickJS on the console, with any OpenAI-compatible model.
> - **A C++ A2UI 1.0 renderer** with no platform dependency. It runs on a Mac or Linux desktop
>   too, so you can try all of this without a console.
> - **A demo app** for the PS5 with its own on-console agent and voice input (speech-to-text on
>   the console's GPU).
>
> One finding that may be useful beyond the PS5: small models are bad at writing A2UI trees and
> good at calling flat "intent" tools that a runtime translates into A2UI. Same model, same 16
> scenarios: 15 correct screens with intent tools, 1 when writing the components itself.
>
> No PS5 needed to try it: clone, build the renderer, and open a recorded screen in about two
> minutes. Feedback very welcome, especially on the Channels adapter.
