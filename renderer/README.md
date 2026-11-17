# a2ui-ps5 (renderer)

A C++20 A2UI 1.0 renderer for the PS5 catalog (`../catalog/ps5.catalog.json`). It takes raw A2UI 1.0 messages or AG-UI `ACTIVITY_SNAPSHOT` events: CopilotKit 1.85's `generative-ui-surface` (a surface's operations) and `generative-ui-function-call` (`callRendererFunction`), and the AG-UI A2UI middleware's `a2ui-surface`. It draws them as console-style screens and turns controller input into A2UI 1.0 action messages, `{"version": "1.0", "action": {name, surfaceId, sourceComponentId, timestamp, context, userMessage?}}`. A press starts a run the way CopilotKit 1.85 does: a visible user message, and the action message in `forwardedProps.copilotkit.ui` (`copilotkit_action`). Messages of other A2UI versions are refused with the reason.

| Layer | Files | Depends on |
|---|---|---|
| Model: surfaces, data model, bindings, templates | `include/a2ui/model.hpp`, `src/model.cpp` | nlohmann/json |
| View: PS5 catalog layout, focus navigation, dialogs, actions | `include/a2ui/view.hpp`, `src/view.cpp` | the model, an abstract `Canvas` |
| SDL canvas: shapes as triangle geometry, stb_truetype text | `src/canvas_sdl.*` | SDL2 (also ported to the PS5) |
| Host app: Mac window, headless screenshots | `src/main.cpp` | all of the above |

The model and view have no platform dependency. The PS5 app (`payload/console`) builds the same sources with the console's SDL2 port, and replaces the keyboard and dialog stand-ins with the system ones.

**Scope.** This renderer draws the **PS5 catalog** (`Screen`, `Menu`, `Shelf`, `Tile`, `Card`, `Text`, `Stats`, `Progress`, `Image`, `TextPrompt`, `VoicePrompt`, `Confirm`, `Notice`…), a custom A2UI 1.0 catalog designed for a TV and a controller. It does not draw the A2UI basic catalog (`Row`, `Column`, `Button`, `TextField`…). `View` is also more than a component tree: it is the whole assistant screen, with the conversation, a message bar and the agent's surfaces inline.

```sh
cmake -S . -B build && cmake --build build -j 8
(cd build && ctest --output-on-failure)          # unit tests + every fixture rendered headless

./build/a2ui-host fixtures/home-shelves.jsonl    # window: arrows, Enter = ✕, Esc = ○, T = △, Y = □, Q/E = L1/R1
./build/a2ui-host fixtures/pick-model.jsonl --keys down,cross --screenshot /tmp/shot.bmp
```

A DualSense paired with the Mac works in the window too (through SDL's game controller support). Actions print to stdout as JSON lines.

## Use it in your own project

The core library (`a2ui`: the model, view and haptics model, with no platform dependency) is a CMake target. You supply a `Canvas` (see `include/a2ui/view.hpp`) and your own device bindings. [`examples/hello`](examples/hello/) is a complete consumer: one surface in, draw calls and an action out.

From source, with FetchContent (only the `a2ui` target is built; no SDL2, libcurl or SQLite needed):

```cmake
include(FetchContent)
FetchContent_Declare(a2ui
  GIT_REPOSITORY https://github.com/Laptopmini/ps5-agentic-ui.git
  GIT_TAG        v1.0.0
  SOURCE_SUBDIR  renderer)
FetchContent_MakeAvailable(a2ui)
target_link_libraries(your_app PRIVATE a2ui::a2ui)
```

Or installed as a package:

```sh
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/your/prefix
cmake --build build --target a2ui
cmake --install build          # lib + headers (a2ui/ and the vendored nlohmann/json.hpp) + the CMake package
```

Then in your project:

```cmake
find_package(a2ui 1.0 CONFIG REQUIRED)
target_link_libraries(your_app PRIVATE a2ui::a2ui)   # pulls in the include dirs too
```

```cpp
#include <a2ui/model.hpp>   // a2ui::Store — surfaces, data model, bindings
#include <a2ui/view.hpp>    // a2ui::View, a2ui::Canvas (implement it for your display)
```

`fixtures/` holds real model outputs from the catalog eval, made into screens by the runtime's intent translation (`node fixtures/make.ts <eval/catalog-eval raw file>`). Each `.source.json` records the scenario and tool arguments.

Vendored, pinned in `vendor/VERSIONS`: nlohmann/json 3.12.0 (MIT), stb_truetype (MIT or public domain), Noto Sans (OFL 1.1).

**Known gaps:**
- one font weight;
- images are local files only (PNG or JPEG by path or `file://`); remote URLs draw a placeholder;
- `Card` is the only component measured before drawing;
- no animation beyond the voice indicator;
- `Notice` drawn by the view doesn't time out (on the PS5 it's a system notification instead).
