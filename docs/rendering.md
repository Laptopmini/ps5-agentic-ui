# Drawing and the GPU on PS5

How the renderer draws, and what we learned about getting the **RADV GPU** (graphics *and* compute)
working from a homebrew app. Verified on firmware **12.70**; GPU is `PlayStation 5 GPU (RADV NAVI21)`,
Mesa 26.2.0.

## The renderer

The A2UI renderer (`renderer/`, library `a2ui`) is platform-independent: it lays out surfaces and
emits draw calls to an abstract **`Canvas`** (see `a2ui/view.hpp`). Everything is geometry + text:
rounded rects, circles, triangles, and glyphs rasterised by `stb_truetype` into a texture atlas
(`GeometryCanvas`). A consumer supplies a `Canvas`; we ship two backends:

- **Software** (`SdlCanvas`): `SDL_RenderGeometry` / `SDL_CreateSoftwareRenderer`, presenting to the
  window's framebuffer via `sceVideoOut`. No Mesa. This is the fallback and is fast enough for a 2D
  UI at 1080p.
- **GPU** (`GlCanvas`): OpenGL ES 2 through **Mesa — zink on RADV** — with EGL presenting to VideoOut.
  SDL `dlopen`s `libEGL.so.1` / `libGLESv2.so.2` from the app folder; `libgallium` and `libvulkan`
  (RADV) are their dependencies.

A payload has **no direct video memory**; the display comes from being launched by websrv's hbldr
(see [packaging](packaging.md)). Once EGL has taken the display in a process, SDL can't also show a
software framebuffer there — pick one backend per launch.

## RADV Vulkan — the useful discoveries

The SDK/pacbrew bundle ships **`libvulkan.so.1`** (≈21 MB). Two things about it are worth knowing if
you want Vulkan or GPU compute in PS5 homebrew:

1. **It is the RADV ICD *and* a self-dispatching Vulkan library.** `llvm-nm -D` shows it exports the
   full Khronos API as ordinary global symbols — `vkCreateInstance`, `vkGetInstanceProcAddr`,
   `vkGetDeviceProcAddr`, ~260 `vk*` in all — alongside the `vk_icd*` entry points. There is **no
   separate Khronos loader** in the SDK, and you don't need one: `dlopen("libvulkan.so.1")` +
   `vkGetInstanceProcAddr` works directly, as does a normal `-lvulkan` link. (This is why a library
   like ggml-vulkan, which dlopens `libvulkan.so.1` via a dynamic dispatcher, links unmodified — no
   `--defsym` aliasing to `radv_*`.)

2. **Memory enumeration needs a live GPU/display context.** A bare `:9021` payload can bring up a
   Vulkan instance, physical device, queue families and a logical device — but
   `vkGetPhysicalDeviceMemoryProperties` returns **0 types / 0 heaps** (accompanied by
   `ac_compute_device_uuid … invalid pci bus info`), so you can't allocate device memory. The same
   call **inside a running app** where the GPU is already up (e.g. zink is rendering) returns the
   full **11 types / 2 heaps / 12 GB**. So Vulkan compute must run **in the app's process with the
   GPU context live**, not in a standalone payload.

With those two facts, **GPU compute works on the stock SDK driver** — no patched `ps5-mesa` fork, no
static RADV build. We cross-compiled **ggml-vulkan** (from whisper.cpp) against the SDK Vulkan 1.4
headers and ran a compute shader in-app: it enumerated the RADV device, saw the 12 GB, dispatched,
and returned the right result, coexisting with zink. See [voice](voice.md) for the whisper use, and
`scripts/ps5-ggml.sh` for the cross-build (the one wrinkle is feeding ggml-vulkan the `glslc`/
SPIRV-Headers it needs — upstream requires `SPIRV-Headers` but doesn't put its include on the target;
logged upstream).

## DualSense input through SDL

SDL's PS5 joystick driver reports the controller as axes/buttons with a couple of surprises worth
noting: **L2/R2 are analog axes 4/5, not buttons** (a held R2 arrives as `SDL_JOYAXISMOTION`, press
> 16000 / release < 8000), the right stick is axes 2/3 and the left stick axes 0/1. Haptics, the
light bar and adaptive triggers go through the device layer (see the catalog's device functions).
