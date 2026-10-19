# Renderer Frontend/Backend Split

Tracking: [#89](https://github.com/corepunch/open-realm/issues/89) (implement),
[#93](https://github.com/corepunch/open-realm/issues/93) (research). Packed shader
contracts: [model-shader.md](architecture/model-shader.md). Platforms:
[build-and-renderer-platforms.md](build-and-renderer-platforms.md).

## Problem

GL state changes (`glEnable`, `glDisable`, `glBlendFunc`, `glDepthMask`, `glDepthFunc`, `glColorMask`, `glCullFace`, `glPolygonOffset`, `glBlendEquation`, etc.) are scattered across every draw function in the renderer. Every 2D draw, every terrain pass, every model render, and every fog-of-war composite sets its own GL state inline via `R_Call(gl...)` before each draw call.

### Current call inventory

| State | Unique call sites |
|-------|-------------------|
| `glEnable(GL_CULL_FACE)` | 6 (engine) + 4 (game) |
| `glDisable(GL_CULL_FACE)` | 8 (engine) + 5 (game) |
| `glEnable(GL_DEPTH_TEST)` | 3 (engine) + 5 (game) |
| `glDisable(GL_DEPTH_TEST)` | 4 (engine) |
| `glDepthMask` | 3 (engine) + 10 (game) |
| `glDepthFunc` | 3 (engine) + 3 (game) |
| `glBlendFunc` | 10 (engine) + 12 (game) |
| `glColorMask` | 3 (engine) + 1 (game) |
| `glEnable(GL_BLEND)` | 6 (engine) + 8 (game) |
| `glPolygonOffset` | 0 (engine) + 4 (game) |
| `glBlendEquation` | 2 (engine) |
| `glEnable(GL_SCISSOR_TEST)` | 2 (engine) |
| **Total** | **~85 scattered GL state calls** |

Consequences:
- Same state is set repeatedly across unrelated draw functions (e.g. `glDisable(GL_CULL_FACE)` + `glEnable(GL_BLEND)` + `glBlendFunc(SRC_ALPHA, ONE_MINUS_SRC_ALPHA)` appears in `R_DrawChar`, `R_DrawFill`, `R_DrawImageBatch`, `R_DrawWireRect`, `R_DrawBoundingBox`, `R_DrawMinimapCameraRect`).
- No way to know what state a function expects vs. what state it leaves behind.
- State changes between consecutive identical surfaces are never skipped.
- Game-specific renderers (WC3, SC2, WoW) duplicate the same boilerplate.

## View ownership

`R_RenderFrame()` (`renderer/r_view.c`) leaves the latest world view in `tr.viewDef` for HUD projection consumers.
Scenes carrying `RDF_NOWORLDMODEL` borrow that state only for their render and restore the complete prior view on
both the entity-camera and general rendering paths. Resetting GL viewport/scissor state is a separate operation.
This preserves the gameplay projection, frustum, viewport, flags, and lighting across portrait draws. The regression
and runtime evidence are documented in [WC3 minimap camera outlines](games/warcraft-3/alerts-and-minimap-pings.md#camera-outline-and-portrait-view-ownership).

## Alpha-key coverage contract

The shared renderer requests the compile-time `MSAA=0/2/4/8` sample count before creating the SDL
window; the bandwidth-safe default is zero. After context creation it logs both SDL's returned attributes and OpenGL's
`GL_SAMPLE_BUFFERS`/`GL_SAMPLES`; the latter determine `tr.msaa_samples`. Context creation
is retried without MSAA only when the requested multisample visual is unavailable, and
that downgrade is logged.

MDX, M2, M3, alpha-key particles, and WoW grass share one material contract:

- an `MSAA=0` fragment shader discards below the authoritative cutoff; an MSAA
  build remaps the edge with `smoothstep`/`fwidth`;
- with a multisampled target, `R_SetAlphaKeyState(true)` disables blending, enables
  `GL_SAMPLE_ALPHA_TO_COVERAGE`, and retains depth writes;
- if an MSAA build cannot obtain a multisampled context, it logs the downgrade and uses
  ordinary alpha blending with depth writes disabled; an `MSAA=0` build uses hard discard
  and depth writes;
- every non-alpha-key material path and frame start disables alpha-to-coverage so the
  state cannot leak into true blended, additive, UI, or terrain passes.

Alpha-to-coverage is for cutout coverage, not general order-independent transparency.
True blended and additive material modes keep their existing blend/depth contracts.
See [Build And Renderer Platforms](build-and-renderer-platforms.md) for build commands and the GLES3 contract.

## Renderer profiling cvars

Use `r_stats 1` to print one averaged line per second:

```text
[R_STATS] fps=... draws=... vertices=... triangles=... instances=...
[WOW_STATS] terrain=drawn/considered ... wmo=instances/models groups=... draws=... textures=... model_batched=... doodads=visible/candidates ...
```

`R_STATS` counts every renderer draw submission, including UI, minimap, fog, particles,
models, terrain, and instanced amplification. `instances` is the sum of each draw's
instance count; non-instanced draws contribute one. Use the WoW pass toggles to isolate
view-dependent costs without changing asset loading or simulation:

| Cvar | Default | Draws |
|---|---:|---|
| `r_grass` | 1 | GroundEffectDoodad instanced batches |
| `r_doodads` | 1 | ADT doodad M2s and map-object debug geometry |
| `r_wmos` | 1 | WMO groups in the world |
| `r_terrain` | 1 | ADT terrain in the world |
| `r_minimap` | 1 | Blizzard minimap tiles (normally 1-4 draws) |
| `r_entities` | 1 | Snapshot entities |
| `r_particles` | 1 | Particle batches |
| `r_fogofwar` | 1 | Fog-of-war passes |
| `r_fog` | 1 | WoW distance fog (turning it off exposes the hard clip) |
| `r_fog_start` | 500 | WoW outdoor fog start in world units |
| `r_fog_end` | 650 | WoW fully opaque fog / WMO CPU-cull distance |
| `r_swapinterval` | 1 | SDL/OpenGL presentation interval (`0` uncapped request, `1` display synchronized) |

For the Human start and left-facing slowdown, launch with `+set r_stats 1`, turn left, then
toggle one pass at a time in the console, for example `set r_wmos 0`, `set r_doodads 0`,
`set r_grass 0`, and `set r_minimap 0`. Compare both FPS and draw counts; restore each
toggle before testing the next so effects do not overlap.

Renderer cvars are registered during common initialization, so the shorter Quake-style
console form is also valid: `r_grass 0`, `r_wmos 0`, and `r_stats 1`. `set` remains useful
for creating an ad-hoc cvar; renderer controls must not rely on that side effect.

### CPU/GPU frame diagnostics

`com_profile 1` adds a one-second `[COM_FRAME]` wall-time report for online,
server and client stages, including the largest single server call. Its `loops`
field counts engine calls, not necessarily rendered frames. `client_ms` includes
graphics/driver waits; these are not sampled CPU-utilization figures. Disabled
profiling takes no extra performance-counter samples and clears a partial window.
Use it with `R_STATS`'s `frames`/`elapsed_ms` to distinguish simulation time from
render/present time. The frame harness covers accumulated windows and disable/reset.

`r_profile 1` enables one-second `[R_CPU]` and `[R_PARTICLES]` reports;
`r_profile 2` also requests asynchronous elapsed GPU timing. The default is zero
and the cvar is not archived. CPU stage names are `other`, `fog`, `world`, `entities`,
`alpha`, `particles`, `scale`, `ui`, `swap` and `sleep`. Each value is milliseconds
per rendered frame, summed over every view/portrait in that frame. `frame_ms` is the
renderer BeginFrame-to-EndFrame interval, including swap/sleep; it excludes server
and client processing before BeginFrame. These are wall times, including driver
blocking, not sampled CPU execution or individual GPU costs.
When shadow maps are compiled in, their pre-view pass is included in `fog`; the
Honor APK used here has shadow maps disabled.
The historical `sleep` field stays zero after removal of the renderer's unconditional
post-swap `SDL_Delay(1)`. Frame caps are paced by `Com_LimitFrameRate` in the main
loop, outside the renderer timer; VSync remains controlled through SDL's swap interval.
The production `R_EndFrame` and limiter paths are covered by `test-renderer-frame`.

The particle report counts surviving particles, batches, texture-limit and blend-change
flushes, mean textures per batch and uploaded KB per frame. It describes the retained
particle-list pass; standalone sprites/ribbons drawn from model passes are separate.
Increasing the texture budget cannot remove blend-change boundaries without changing
transparent ordering.

GPU timing uses one elapsed query per sampled frame, one in sixteen, with eight
pending slots. It ends before swap, polls availability without waiting, and skips a
sample when every slot is occupied. `samples`, `disjoint`, `busy` and `invalid` expose
measurement quality; `frame_ms=-1` means no valid samples. EXT disjoint events discard
all outstanding queries. UINT64_MAX and durations larger than the host submission-to-read
interval plus 1 ms clock tolerance are rejected even if the driver reports ready and
no disjoint. Queries are released on disable and before GL-context destruction.
The optional ES EXT / desktop ARB entry points are resolved per context; missing
support is logged and CPU diagnostics remain available. See the
[EXT specification](https://registry.khronos.org/OpenGL/extensions/EXT/EXT_disjoint_timer_query.txt).

Do not add per-pass GPU elapsed values on a tile renderer. An experimental Honor capture
produced overlapping/frame-sized results for tiny intervals; a subsequent whole-frame
capture returned implausible values despite ready=true/disjoint=false. Those captures
are rejected as GPU evidence. The final bounded query path reports invalid samples
explicitly; use resolution sensitivity and CPU/swap timings when GPU results are absent.
Device observations and the capture tool are in [Android profiling](android.md#next-on-device-capture)
and [WC3 performance](games/warcraft-3/performance.md).

`tests/test_renderer_profile.c` drives production query creation, delayed availability,
64-bit results, disjoint before/after reads, invalid durations, ring saturation,
disable/re-enable, context teardown and unsupported timers using fake GL/SDL boundaries.

### WoW Human-start checkpoint (2026-08-18)

At 2048x1536 with 4x MSAA, the forward-facing Human start submits about 1,744 draws,
6.84 million vertices, 2.28 million triangles, and 467,000 instances per frame. Its
world pass contains 133 visible terrain chunks, 505 WMO batch draws, and 276 visible
doodads. Isolating one pass at a time found:

| Disabled pass | Draws/frame | FPS | Approximate draw reduction |
|---|---:|---:|---:|
| none | 1,744 | 94-95 | - |
| live minimap | 1,378 | 94 | 367 |
| WMO | 992 | 103 | 750 |
| doodads | 1,430 | 104 | 315 |
| grass | 1,731 | 92 | negligible |

The FPS values are directional rather than additive because macOS Metal/OpenGL driver
work varies between runs. The draw deltas are the useful isolation signal. Grass is 14
persistent instanced batches and is not the draw-call bottleneck.

The original WMO pass reused the four-layer terrain shader by binding every WMO texture
to units 0-3 and white to unit 4 for every material. This matched profiler time in
`glActiveTexture`, `glBindTexture`, sampler loading, and Metal pipeline preparation.
The WMO single-texture shader branch now samples unit 0 only; the same scene improved
from roughly 82-90 FPS to 94-95 FPS without changing draw count. The remaining scalable
cost was the number of separately submitted WMO and doodad batches, plus the duplicate
live-minimap world pass.

After static-doodad instancing, authoritative minimap tiles, and hybrid WMO material
batching, the same forward Human-start view measured about 910 total draws at the 120
FPS presentation ceiling. Its WMO work fell from 486 to 207 draws: 10 of 17 visible
instances used model-wide material batches, while sparse instances retained per-group
culling. The minimap fell from roughly 367 duplicated world draws to at most four UI
quads. `r_swapinterval 0` is useful for requesting uncapped presentation, but macOS's
OpenGL-on-Metal path may still present at the display's 120 Hz ceiling.

The FPS overlay uses one batched system-font submission and displays
`FPS ##  Drawcalls ##`; its draw count is captured before the overlay itself.

## Particle submission

`renderer/r_particles.c` uploads surviving particles once per view pass. Packets retain
particle/triangle order and share up to four textures with one blend mode; a fifth
distinct texture or a blend change flushes the packet. A second list walk reconstructs
the same draw ranges without a per-particle packet allocation.

The third texture-coordinate component selects one of four constant-index sampler
lookups. This retains eight shader attributes and works with the GL/GLES shader dialects.
The sampler units are 0/3/4/5: unit 1 stays bound to the model shadow map and unit 2 to
FOW. Immediate billboards/ribbons can execute between model draws, so using unit 1
would corrupt the following model's shadow lookup. Their selectors start at zero and
their immediate upload replaces the shared VBO normally.

Common matrices/FOW and culling are set once per uninterrupted particle pass.
Texture bindings are compared against a stack-local array during that pass; no binding
state survives into the next view or standalone sprite/ribbon. Every immediate draw
starts a fresh binding scope, and the original cull state is restored before returning.
This preserves external model/UI binding changes and reserved shadow/FOW units.
`particle_pass_reuses_bindings_and_restores_culling_before_immediate_sprites` covers
four alternating blend draws, deliberate binding clobbers between passes and an
immediate sprite; the fixture's texture binds fall from twenty to five.

The tests capture real production upload/range/state calls in `tests/test_mdx_ui.c`.
For the Honor measurements and vertex-storage cost, see
[ordered particle batches](games/warcraft-3/performance.md#ordered-particle-batches-with-four-textures).

## Target API shape

Do **not** emulate OpenGL 1.x (`glEnable(GL_LIGHTING)`, fog modes, texture-env, uber-shader permutations).
Call-site `glUniform*` is already gone (`shader_desc_t` + typed `state` + `R_ApplyShader`). The remaining work
follows Sebastian Aaltonen's thin-pipeline / root-struct design, mapped onto the current OpenGL 3.1 / GLES3
backend. Issues: [#89](https://github.com/corepunch/open-realm/issues/89),
[#93](https://github.com/corepunch/open-realm/issues/93). Source:
[SebAaltonen, 2026-09-03](https://x.com/SebAaltonen/status/2095562458467287266).

| Steal | Meaning here |
|-------|----------------|
| Root struct + one push | One `alignas(16)` C struct shared with the shader: transforms, material, texture indices. Fill a stack/bump copy and upload once (UBO / persistent-mapped buffer today; push constants later). |
| Thin pipeline objects | Graphics pipeline = shaders + color/depth formats + a small raster blob (blend, depth write/test, cull, write mask). One short create call. |
| Blend/depth as small objects | Attached at pipeline create. On GL they can also change cheaply; the backend diffs them with a Doom 3-style bitmask. Not a giant opaque PSO. |
| Texture/sampler heap | 32-bit indices in the root struct. No per-draw `glBindTexture` storm. GLES without bindless needs an explicit, logged strategy. |
| 64-bit GPU pointers | Not required on GL/GLES. Do not block #89 on `vkCmdPushDataEXT` or buffer device address. |

Lighting, fog, and grass stay packed data in the root struct (`MODELLIGHTING`, `uGrassParams`, model fog flags).
See [model-shader.md](architecture/model-shader.md). They are not public `lightingEnabled` / `fogMode` bits that pick shader permutations.

Typical draw shape: bind pipeline if changed, push root once, draw.

## Backend state cache (Phase 1 of #89)

### Implemented program binding cache

`r_shader.c` owns the current program observed by this renderer context. Its
GL-call adapter skips only an unchanged `glUseProgram`; typed uniform
validation, comparison and changed-value uploads still run on every submission.
The reusable `gl_shader` module remains byte-identical and binds every time for
external callers. The engine scopes its two GL-call overrides around the unity
include with `push_macro`/`pop_macro`, restoring loader/test macros afterwards.
The paired deletion adapter invalidates a current name before deleting it,
including failed-link cleanup, so a recycled GL name binds again.

`R_BeginFrame` invalidates the binding once per frame and samples the diagnostic
`r_program_cache` cvar once (default 1; 0 forces all binds for matched A/B captures).
Context creation and teardown invalidate as well. All in-renderer program binds
must use this adapter; external GL users must invalidate before returning control.
Portraits and nested views share the context and retain ordinary program switches.
Resetting the binding does not discard program-owned uniform values.

`R_STATS` includes mean `program_requests` and `program_binds` per frame. They
count validated submissions and actual GL binds, not draw calls or uniform
uploads. The in-engine probe uses the paired deletion adapter; standalone
module callers retain its always-bind semantics. Production shader tests cover a long same-
program run with changing uniforms, A/B/A switches, noncurrent/current deletion,
name reuse, reset, uncached mode and rejected submissions. The real frame-entry
test verifies per-frame invalidation and live mode changes. Emulator profiling
and A/B evidence belong in [WC3 performance](games/warcraft-3/performance.md).

### Remaining raster-state work

Doom 3's `R_*` / `RB_*` split and `GL_State(uint64)` XOR remain the **GL implementation** of cheap blend/depth/cull
packets. Frontend never issues `glEnable` / `glBlendFunc` / `glDepthMask`. Backend owns those calls.

```
backEndState_t:
    uint32  glStateBits;      // packed blend/depth/color-mask
    GLenum  faceCulling;
    GLenum  depthFunc;
    DWORD   polygonOffsetScale, polygonOffsetBias;
    DWORD   blendEquation;
    DWORD   activeTextureUnit;
    DWORD   currentPipeline;
    DWORD   currentVAO;
    DWORD   currentFBO;
    RECT    currentScissor;
```

```c
void RB_State(uint32 bits);
void RB_Cull(GLenum mode);
void RB_PolygonOffset(float scale, float bias);
void RB_BlendEquation(GLenum eq);
void RB_Scissor(LPCRECT r);
void RB_BindVAO(DWORD vao);
void RB_BindFBO(DWORD fbo);
void RB_SetViewport(LPCRECT r);
```

Each helper compares against the cached value and only issues the GL call on delta. Later phases in #89
add thin pipeline objects, the root-struct push, and the texture heap. Surface sorting (by pipeline +
blend/depth bits) waits until draws are no longer immediate.

## Vulkan feasibility on Honor (October 18, 2026)

The owner's Vulkan question was assessed against the
[fresh native-resolution profile](games/warcraft-3/performance.md#fresh-post-merge-honor-profile-at-native-resolution-october-18)
and the actual Honor device. Read-only commands, without restarting the game:

```sh
adb -s AE5HCP3B27401902 shell pm list features
adb -s AE5HCP3B27401902 shell cmd gpu vkjson
```

The Android feature reports include Vulkan level 1 and version integer 4198400
(1.1.0). The physical device dump identifies **Mali-G52 MC2**, integrated GPU,
with `apiVersion = 4198577` (**Vulkan 1.1.177**) and raw driver version
134221824. Its reported limits include a 65536-byte uniform-buffer range,
268435456-byte storage-buffer range, 256-byte push constants and four bound
descriptor sets. `multiDrawIndirect` is **false**;
`drawIndirectFirstInstance`, ETC2 and ASTC LDR are true. Do not assume Vulkan
1.3, descriptor indexing or multi-draw indirect from this device name. Capability
enumeration establishes API availability, not performance or rendering correctness.

The current application is GL/GLES-specific beyond one central wrapper:
`renderer/r_buffer.c` submits GL draws/uploads; `renderer/shader_desc.h` uses
the GL shader module; the WC3 MDX renderer directly uploads/binds palettes and
draws instances in `r_mdx_geoset.c`. The thin-pipeline/root-struct API above
describes the target design; the remaining raster-state split is not already
implemented as a complete API-independent backend. A Vulkan port therefore
needs resource lifetime/allocation, pipelines, shader interfaces/SPIR-V,
descriptors, render passes, synchronization and swapchain handling, including
terrain, MDX, decals, fog, particles, transparency and UI. Preserve game/client
ownership and the existing GLES backend while introducing it.

The measured Mali driver leaf share is 22.31/20.87% of active main-thread CPU;
this is **not** the same percentage of wall frame time or a measured Vulkan
speedup. Vulkan could reduce GL validation/state/upload overhead and allow
parallel recording. It does not automatically reduce game simulation, sight
calculations, CPU animation sampling or CPU decal clipping. Existing EGL blocked
spans are not GPU timers, and a different API can change synchronization as well
as CPU cost. The profile does not predict 40–60 FPS from the API change alone.

An appropriate experiment is a selectable second backend with a representative
MDX/material/instancing pilot, including animation palettes and correct depth
and transparent-layer ordering. Compare identical submitted work and framebuffer
output first. A draw-only pilot measures renderer potential; only a subsequent
complete same-scene integration at **100% native resolution** can establish
whole-game FPS. Keep pipeline/descriptor allocation and shader compilation out
of measured steady-state windows, and separate correctness/validation runs from
performance runs. Retain both backends and all results under the
[experiment policy](performance-experiments.md).

Primary guidance:

- [Khronos threading](https://docs.vulkan.org/guide/latest/threading.html):
  independent command pools allow parallel command recording; the application
  still owns synchronization.
- [Khronos command-buffer sample](https://docs.vulkan.org/samples/latest/samples/performance/command_buffer_usage/README.html):
  pool reuse, balanced work and enough draws per secondary buffer matter;
  additional buffers/threads can introduce overhead.
- [Android Vulkan design guidelines](https://developer.android.com/ndk/guides/graphics/design-notes):
  explicit pipeline/resource optimizations and mobile render-pass organization
  are necessary; a poorly structured Vulkan renderer can be slower than GLES.

No backend implementation, APK build/install or game interruption accompanied
that initial feasibility check. The subsequent implementation is recorded below.

## Android graphics API selection through ANGLE

The Android test package now has two restart-time paths, selected by archived
`r_backend`: `gl` (default, system EGL/GLES) and `vulkan` (packaged ANGLE on
the device Vulkan driver). WC3's Video settings reuse the unused mobile gamma
row for **Graphics API**, with **OpenGL ES** / **Vulkan (ANGLE)** and a restart
notice. The new popup clones the native resolution-popup style; existing FDF
label/row anchors remain intact. OK saves the cvar through the existing config
flow. This is a complete GLES-to-Vulkan bridge, not a native Vulkan renderer or
parallel command-recording implementation.

`refExport_t.GraphicsBackends()` is a mandatory generic availability callback.
It returns GL everywhere, and also Vulkan when this Android build includes
`BZ_ANGLE_VULKAN`, the two app-local libraries load, and Vulkan enumerates a
hardware GPU. Optional-package/probe failures are logged and the unavailable
choice is absent. An explicitly requested unsupported/unknown backend fails at
startup; it does not silently run another API.

`R_SelectGraphicsBackend` sets SDL's EGL/GLES library choices before creating
the window. After the context is current, `R_VerifyGraphicsBackend` loads every
required GLES3 entry through `SDL_GL_GetProcAddress` and checks `GL_RENDERER`.
A Vulkan request requires both ANGLE and Vulkan in that string. Android's
renderer has no `libGLESv3` dependency or undefined core `gl*` symbols: typed
`bz_gl` dispatch in `r_gl_api.h`/`.inc` covers the shared renderer, vendor shader
module and WC3 renderer together. Missing required entries clear the entire
dispatch and fail initialization; context teardown clears it too. Never mix
system GL entry points with an ANGLE context.

The initial AOSP prebuilt (`3f647b1bf9a4`) is retained as a failed control. It
caused huge screen-spanning particle triangles and disappearing terrain on
Honor. The diagnostic run caught `GL_INVALID_OPERATION` at `glDrawArrays`;
`KHR_debug` identified **Internal Vulkan error**, rather than a front-end
buffer-range error. Captured coordinates were finite and the active VAO/VBO
range was valid. The simple production-particle readback passed on both system
GLES and the old ANGLE, so it alone did not reproduce the whole-game failure.
The exact failing internal Vulkan operation/result remains unidentified.

The selected replacement is the pinned ARM64 prebuilt from official SDK Android
35 default image revision 2. It loads on Android 12 Honor, reports hardware
Mali/Vulkan, and completes the same diagnostic scenario without the old GL/KHR
error. The embedded ANGLE git hash is unknown; identify it by image fingerprint
and library hashes. See [pins, notices and import recipe](https://github.com/corepunch/open-realm/blob/main/packaging/android/angle/README.md).
Its ELF LOAD alignment is 16 KiB, replacing the legacy binary's 4 KiB limit;
whole-package compatibility on untested Android devices is not established.

`DIAG_OUTPUT` now writes GL error codes and source locations to stderr before
asserting; buffered stdout previously lost the useful diagnostic on Android
abort. The dynamic `glGetError` alias is parenthesized so its dispatch member
does not expand the unrelated function-like `GetError()` macro. Temporary
per-frame geometry dumping is retained only in the experiment archive, not in
the working render loop.

Validation includes menu capability/selection/persistence/reinitialization and
authored-anchor tests, transactional dispatch tests, pinned-import rejection
tests, all game build consumers, Android instrumentation, and device UI/scene
checks. Native GLES-only and ANGLE-enabled renderer builds both link without a
system GL dependency. Performance conclusions and their scene/variance limits
are in [WC3 performance](games/warcraft-3/performance.md#angle-vulkan-bridge-on-honor-october-18).

## Key constraints

- `R_Call(gl...)` stays for diagnostics; `RB_*` uses it internally.
- `refExport_t` is the client/renderer ABI. After changing its layout, run `make build` before launching;
  `make test` may rebuild a renderer dylib without relinking an existing game executable, and mixing those
  artifacts dispatches calls through the wrong function-table slots.
- Pipeline/root types live in the renderer (`r_backend.h` via `r_local.h`).
- Callers never reference `progid` or uniform locations. `shader_desc_t` remains the GLSL grammar until
  the root-struct push replaces per-field `glUniform*`.
- One representation per shader concept: no zero-count modes or parallel fallback uniforms.
- macOS GL 4.1, desktop GL 3.1, and GLES3 Mali-G31 each need an explicit transport. Missing extensions
  are logged; do not silently demote to a bind loop and call it bindless.

## Files for the state-cache step

| File | Action |
|------|--------|
| `renderer/r_backend.h` | **New** — `backEndState_t`, pipeline/root types, `RB_*` |
| `renderer/r_backend.c` | **New** — cache, later pipeline bind + root push |
| `renderer/r_local.h` | `#include "r_backend.h"` |
| `renderer/r_main.c`, `r_draw.c`, `r_fogofwar.c`, `r_particles.c`, `r_ents.c` | `RB_*` instead of raw GL state |
| WC3 / WoW / SC2 game renderers | Same; listed in #89 |

## Verification

1. `make clean && make` — no warnings.
2. Visual: WC3 ROC and TFT, SC2, WoW; load a map. `+screenshot N +com_frame_limit`.
3. `rg 'gl(Enable|Disable|BlendFunc|DepthFunc|DepthMask|ColorMask|CullFace|PolygonOffset|BlendEquation)\b' renderer/ games/*/renderer/` — only inside `r_backend.c`.
4. `r_stats 1` on a known scene so draw count does not regress silently.
