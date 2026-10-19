# WC3 Performance Hot Spots

For process footprint, allocation profiling, and RAM reduction priorities, see [WC3 memory](memory.md).
Follow [experiment retention](../../performance-experiments.md): a negative result
must retain its implementation as an indexed branch/archive or a documented
toggle. Historical references below describe what was retained at capture time;
old binary packages may have been cleaned while source/patch evidence is indexed
in the optimization archive (`$OPENREALM_EXPERIMENTS_DIR/warcraft-3/optimization-archive/README.md`, external archive).

## Android credits profiling (October 2026)

The repeatable startup scene is documented in [Android credits test APK](../../android.md#frozen-throne-credits-test-apk).
The API35 arm64 emulator uses ANGLE/Vulkan SwiftShader, with a 1280x800 drawable and
857x536 world rendering at 67%. Emulator submission overhead and software graphics
are not physical Mali-G52 timings.

The first 10-second Simpleperf capture included map registration, inflate and texture
loading; discard it when choosing a steady-frame optimization. A subsequent 15-second
`task-clock:u`, 1000 Hz capture had 991 samples and zero lost samples. Inclusive CPU
shares were 21.19% for `R_W3RenderEventSplat`, 20.69% for `R_DrawParticles`, 9.28% for
`MDLX_BindBoneMatrices` and 0.10% for `R_ConformGroundSurfaces`. Parents and children
overlap. This selected effect submission before the earlier bridge-scan proposal.
Sources: `build/android/optimization/before/steady.data`, `steady-report.txt` and
`steady-capture.log`. Sample shares are not milliseconds or expected FPS savings.

Retained SPL/FPT/UBR effects previously reset, uploaded and drew the terrain splat
buffer separately for every effect. Their solid-phase pass now shares an explicit
batch. Adjacent identical textures/shaders share an upload and draw; material changes
and buffer capacity still flush without reordering. Immediate event presentation,
atlas UVs, colors, clipped terrain geometry and lifetimes are preserved. See
[MDX event batching](mdx-event-objects.md#spl-fpt-data-flow) for ownership and tests.
Two focused tests failed before this change and pass afterward; the A/A/B/B fixture
falls from four uploads/draws to two with identical geometry, while A/B/A/B stays four.

For same-scene A/B captures, keep signed copies of the old/new APKs and let builds and
the full test suite finish before recording. Start each fresh process at 67%, detect
the first `Render scale: 67%` world view, warm up for 40 seconds, then record 15 seconds
with Simpleperf. Save only that interval's `[R_STATS]` lines as well as the full log,
profile, screenshot and APK hashes. `build/android/optimization/capture_pair.py` is the
local capture script; its outputs are under `paired/before` and `paired/after`.
The retail parade and random weather continue evolving, so matching wall-clock windows
still do not guarantee identical particle populations. `SV_Frame` also advances one fixed
tick per eligible main-loop call and clamps a late deadline; below 10 FPS the same
wall-clock interval can cover different simulation intervals. Use deterministic GL-submission
tests to establish removed work; use device captures to assess end-to-end impact.

Two quiet capture pairs were run in opposite install orders (old/new, then new/old):

| Per-run median in the 15-second window | Old APK | Batched APK |
|---|---:|---:|
| Draw submissions, pair 1 | 883.5 | 651.1 |
| FPS, pair 1 | 6.4 | 7.8 |
| Draw submissions, pair 2 | 906.9 | 684.3 |
| FPS, pair 2 | 9.05 | 11.9 |

Submitted vertices stayed around 278–280K per frame. Total draw submissions fell
24.5–26.3% in these windows, but changing particle populations and simulation progress
also contribute to that total; do not attribute the entire difference to decal batching.
The same `R_RenderRectSplatUV` subtree accounted for 15.40%/20.53% inclusive CPU samples
before and 5.54%/6.90% afterward. The post-change particle subtree remained around
22–23%, making particle submission the next measured candidate. These are preliminary
emulator observations, not a predicted Honor FPS increase. The deterministic tests are
the evidence for preserved output and removed adjacent-decal submissions.

Raw data is in `build/android/optimization/paired/{before,after,after-2,before-2}/`;
`comparison.json` collects the per-run summaries. Baseline APK SHA-256:
`460880bad2332e1afe0a861c51f22b2340a06d9988606504fa500af159f6c814`;
batched APK: `348eb5b61bae3f2af0b694c9189bf8dbb0b01cdaecbeb1ded604efdaa988fdf3`.
`make HOMEBREW_PREFIX="$PWD/build/android/sdl-host" test`, the focused renderer suite,
desktop WC3 build, Android `-O2` APK build/signature/alignment checks, and Android
`ImportTests` pass. No shared engine code or protocol/save fields are added by this change.

### Honor particle streaming

The physical AGM3-AL09HN (Mali-G52 MC2) reaches the credits at 67%, with 1286x804
world rendering inside the 1920x1200 drawable. Rootless Simpleperf works with the
debuggable APK; see [Android capture commands](../../android.md#next-on-device-capture).
A 15-second steady capture after a fresh launch and 40-second warmup recorded 15,147
`task-clock:u` samples with no loss. `R_DrawParticles` accounted for 43.14% inclusive
CPU samples and `R_FlushParticles` for 40.76%; these are overlapping shares, not GPU
timings. An earlier live capture had similar shares (40.59%/38.21%). Its call-chain
PCs, resolved against the matching ARM64 renderer disassembly, placed 4.485 sampled
CPU seconds under `glDrawArrays` at `R_FlushParticles+0x24c` and 0.694 under
`glBufferData` at `+0x78`. A slow draw boundary alone does not prove a GPU bottleneck.

`renderer/r_particles.c` previously replaced the same VBO before every adjacent
texture/blend run. The streaming change generated the surviving particles' vertices
once, uploaded the complete pass once, then drew the original material runs using
`first`/`count` ranges, preserving particle population, vertex format, animation and
material state. The following batching step extends that vertex format.
`R_FlushParticles` still uploads immediately for
standalone sprites/ribbons. Expiry happens before choosing the first material, so an
expired head or an entirely expired list no longer produces empty uploads/draws.

Three tests in `tests/test_mdx_ui.c` drive the production pass and capture only the GL
boundary. They fail on the old path and cover one upload with six ordered material
runs, positions/UVs/colors/tails/oriented axes, blend/depth state, expired-head/all-dead
lists, all 10,000 pool entries, and subsequent standalone billboard/ribbon uploads.
`make test-mdx-ui` passes 941 assertions in 21 tests; the full suite, Android build,
WC3/SC2/WoW desktop builds and engine-boundary audit pass.

Two Honor comparisons used the same 40-second warmup and 15-second profile interval,
in old/new then new/old order. Both builds include the earlier retained-splat change:

| Per-run median | Before streaming change | One upload per pass |
|---|---:|---:|
| FPS, pair 1 | 31.3 | 33.5 |
| FPS, pair 2 | 32.2 | 34.3 |
| Draw submissions, pair 1 | 878.8 | 928.3 |
| Draw submissions, pair 2 | 882.9 | 925.6 |
| Submitted vertices, pair 1 | 278,851 | 280,036 |
| Submitted vertices, pair 2 | 279,241 | 279,589 |

The observed FPS gain is 6.5–7.0%. This remains a preliminary scene measurement:
weather/lifetime populations vary with frame partition, and total draw count was
4.8–5.6% higher in the new captures. The regression tests establish the upload
reduction and preserved particle order independently of that population variation.
`R_DrawParticles` inclusive CPU share changed from 43.14%/43.05% to 41.58%/40.87%,
so particle submission remains the largest measured candidate. Battery temperature
was 29.1/30.3 C in pair 1 and 30.5 C for both runs in pair 2; this is not GPU temperature.

Raw captures and summaries: `build/android/optimization/honor/paired/` and
`honor/comparison.json`. The benchmark APKs are `honor/before/baseline.apk`
(SHA-256 `348eb5b61bae3f2af0b694c9189bf8dbb0b01cdaecbeb1ded604efdaa988fdf3`)
and `honor/after/optimized.apk`
(`c3eefe2a1139e9c1c885360d07e1a2890b6898187909d37e67e9eaec3314f37f`).
The final formatting-only rebuild is
`418bbd412f285844030da7658eee41cd2a470789a50ce0fe9a919d852fd5e5fb`;
its renderer `.text` section matches the measured optimized library byte for byte.

### Ordered particle batches with four textures

The next baseline is the streaming APK above. On Honor, particle submission still
accounted for about 41% of CPU samples. The hot Mali PCs near `0x8c5ecc` traverse
linked data structures inside the draw call. The library is stripped: the nearest
exported `clSetContextDestructorCallback` label printed by objdump is not a recovered
function name for these addresses. Disassembly and CPU samples do not measure GPU
execution time.

`R_DrawParticles` now keeps up to four original 2D textures in each ordered packet.
`uv.z` selects the sampler for all six vertices of a particle. The shader uses four
constant-index sampler lookups behind branches, preserving GLSL 120/140 compatibility
without adding a ninth vertex attribute. Particle stride increases from 68 to 72 bytes
(240 KB for the full 60,000-vertex staging buffer). Textures keep their original sizes,
mips, UVs and sampler state; no atlas or texture resampling is introduced.

The first walk assigns slots and generates vertices; after the single upload, a second
walk reconstructs the same packet boundaries for `first`/`count` draws. A blend-mode
change or a fifth distinct texture ends the packet. Repeated textures reuse a slot,
including after all four slots fill. The linked list and triangle order remain intact.
Standalone sprites and ribbons upload immediately, use slot zero and fill unused
sampler bindings with that texture.

Texture units are `{0, 3, 4, 5}`. Keep unit 2 for FOW and unit 1 for the model shadow map:
WoW quest-icon billboards can execute between M2 model draws, whose shadow binding is
installed by `R_SetupGL`. A test with a sentinel shadow binding caught the initial unit-1
allocation; the final mapping preserves it. See [shared particle submission](../../renderer-backend.md#particle-submission).

The three new `mdx_ui` tests capture interleaved A/B/A/C/D/A textures, a fifth texture,
blend boundaries and the return to the earlier blend mode, six vertices' selectors,
FOW/shadow slots, default-texture identity, immediate draws and VAO/sampler-array setup.
They fail on the prior single-texture path. The existing 10,000-particle test now expects
one draw for its two textures and still verifies the subsequent billboard/ribbon ranges.
The focused suite passes 1,182 assertions in 24 tests; the full suite, WC3/SC2/WoW builds,
Android build/signature/alignment checks and `DIAG_OUTPUT` syntax checks pass.
The same suite passes at `-O1 -fsanitize=undefined -fno-sanitize-recover=all`. It initially
caught `FOFS` deriving a member pointer from a null struct during VAO creation;
`common/shared.h` now uses standard `offsetof`, retaining the same byte offsets.
ASan on this macOS 26.6.2 host deadlocked inside its own `InitializeShadowMemory`
before main/test registration; its stack is in `sanitizer-sample.txt`. The ASan run was
terminated and is not reported as a passing memory check.

Local artifacts are under `build/android/optimization/honor-next/`; `before/baseline.apk`
is SHA-256 `418bbd412f285844030da7658eee41cd2a470789a50ce0fe9a919d852fd5e5fb`,
and `after/optimized.apk` is
`84d94515f5070cdbbc90b0f908a16427b23d1d0426e80ca1307c265b741aeeae`.
The earlier unit-1 prototype is retained separately as `unit-1-experiment`; use the
final `after` captures for comparisons.

The same 40-second warmup/15-second profile window at 67% was compared in both
installation orders (old/new, then new/old):

| Per-run median | Single-texture packets | Four-texture packets |
|---|---:|---:|
| FPS, pair 1 | 35.4 | 45.05 |
| FPS, pair 2 | 35.3 | 44.7 |
| Draw submissions, pair 1 | 866.1 | 472.5 |
| Draw submissions, pair 2 | 891.8 | 468.3 |
| Submitted vertices, pair 1 | 279,302 | 278,824 |
| Submitted vertices, pair 2 | 279,304 | 279,301 |

Observed FPS improved 26.6–27.3% and draw submissions fell 45.4–47.5%; vertex counts
changed less than 0.2%. `R_DrawParticles` inclusive CPU samples fell from 37.46%/38.79%
to 19.40%/18.45%. `MDLX_BindBoneMatrices` is now 15.25%/15.82%, a remaining candidate.
All four profiles have zero lost samples. Battery temperatures were 31.9/32.0 C in
pair 1 and 32.3 C for both runs in pair 2. These are repeatable observations for this
credits interval; weather and scene populations still vary, and GPU time was not measured.
Raw reports, logs, screenshots and metadata are in `paired/` and `comparison.json`.

The final APK, including the `offsetof` correction, is SHA-256
`ab737feb111b52deb027cccf563d224aeec06bef7f5a4c263a0c4774570410f6`.
Its renderer `.text` section is byte-identical to the measured four-texture library.

### Lazy attachment poses

After four-texture particle batching, the Honor profile attributes 15.25–15.82%
inclusive CPU samples to `MDLX_BindBoneMatrices`. In the first capture,
`R_W3RenderAttachmentModels` accounts for 5.24% and its
`MDLX_CollectAttachmentPositions` subtree for 5.14%. The collector unconditionally
rebound the full skeleton after geometry rendering, even when no attachment could
produce embedded child art. These percentages overlap and are not expected FPS gains.

`R_W3RenderAttachmentModels` now first checks whether any authored attachment path is
nonempty. Empty paths remain valid effect sockets, so general socket queries are
unchanged. `MDLX_CollectAttachmentPositions` filters prefix/visibility before binding
the skeleton, then binds once on the first visible match. Empty, unmatched and hidden
queries leave the shared pose scratch untouched. There is no retained pose cache or
cross-model lifetime dependency. Actual child transforms, order, Birth timing and
building-fire/socket positions keep their existing contract; see
[MDX attachments](building-damage-rendering.md#mdx-attachment-contract).

Two new tests failed five assertions on the old path and pass after the change.
They drive the real model-draw/socket-query paths, verify skipped pose evaluation
through untouched matrix scratch, and cover adding/removing a later child path,
parent/child Birth mapping, empty-path socket lookup, prefix misses and visibility.
Both normal and shadow-enabled renderer suites pass 8,878 assertions in 176 tests.
The full suite, desktop WC3 build and Android APK/signature/alignment checks pass.
Honor `ImportTests` also passes variant/override checks, importer cases and all four
native smoke tests with clean process exit.

Local artifacts are under `build/android/optimization/honor-bones/`. The baseline APK
is SHA-256 `ab737feb111b52deb027cccf563d224aeec06bef7f5a4c263a0c4774570410f6`;
the lazy-query APK is
`e518732686a20f2d4b2825a0421c94bb0fad80b5de5e67081f9d7451a434286f`.

Two fresh-process Honor pairs used the same 40-second warmup and 15-second
`task-clock:u` capture, in old/new then new/old install order:

| Per-run median / inclusive CPU samples | Before lazy queries | Lazy queries |
|---|---:|---:|
| FPS, pair 1 | 45.0 | 45.3 |
| FPS, pair 2 | 45.3 | 45.0 |
| Draw submissions, pair 1 | 465.1 | 518.3 |
| Draw submissions, pair 2 | 495.0 | 527.1 |
| `MDLX_BindBoneMatrices`, pair 1 | 15.80% | 11.71% |
| `MDLX_BindBoneMatrices`, pair 2 | 15.70% | 11.71% |
| `R_W3RenderAttachmentModels`, pair 1 | 5.18% | 0.19% |
| `R_W3RenderAttachmentModels`, pair 2 | 5.11% | 0.14% |

The unused attachment subtree is removed, but these pairs establish **no repeatable
FPS gain**. New-build draw counts were 6.5–11.4% higher and submitted vertices stayed
within 0.8%, consistent with the known weather/lifetime variation. Particle submission
now accounts for 23.07%/23.00% inclusive CPU samples versus 18.12%/20.14% in these
baseline runs. This does not prove a GPU limit: CPU sample shares are relative,
overlap between parents/children, and do not measure GPU work or waits. Further FPS
work should measure particle packet boundaries and GPU time before expanding batches
or introducing pose reuse across rendering passes.

All four captures have zero lost or truncated samples. Battery temperatures were
33.1/32.7 C in pair 1 and 32.7 C in pair 2; these are not GPU temperatures. Raw logs,
profiles and screenshots are in `honor-bones/paired/`; `comparison.json` records
the APK hashes, capture metadata and per-run results.

### Frame diagnostics and particle state scope

`r_profile 1` now separates renderer CPU wall time into passes, swap and the existing
post-swap sleep; `r_profile 2` additionally attempts asynchronous whole-frame GPU
queries. It is opt-in and not archived. The API, sample rejection rules and regression
tests are described in [renderer diagnostics](../../renderer-backend.md#cpugpu-frame-diagnostics).
The repeatable helper is `tools/android/measure_credits.py`; see
[Android capture commands](../../android.md#next-on-device-capture).

The final Honor resolution sweep used a fresh credits launch, 40-second warmup
from `G_ClientBegin` and 15-second capture, with **CPU-only profiling** so GPU queries
cannot perturb these conclusions. The APK is the measured scoped-state build below;
100%/50% are `final100`/`final50`, and 67% is `paired/after` (repeated in `after-2`):

| Per-run median | 100% | 67% | 50% |
|---|---:|---:|---:|
| FPS | 32.8 | 45.6 | 53.3 |
| Swap wall time, ms/frame | 10.230 | 3.758 | 1.766 |
| Solid entity CPU wall time, ms/frame | 5.964 | 4.830 | 4.620 |
| Alpha CPU wall time, ms/frame | 3.020 | 2.441 | 2.226 |
| Particle CPU wall time, ms/frame | 4.913 | 4.684 | 4.198 |
| Scale-target CPU wall time, ms/frame | 0.013 | 1.055 | 1.058 |

This is a directional sweep, not a repeated matched-work optimization comparison.
Resolution sensitivity and increasing swap blocking suggest a substantial graphics
component. Swap wall time includes driver/window-system waiting and is not GPU
execution time. An earlier particle-off isolation capture reached 49.6 FPS at 67%
with the pre-scope pass (`no-particles/`). Turning particles off removes their
submission/update pass, not all emitter/model work. It changes scene work and is
only a diagnostic, not a quality setting shipped as an optimization.

Do **not** use the experimental GPU values in `scale{100,67,50}` or `frame67` as GPU
timings. Segmented queries returned overlapping/frame-sized intervals or disjoint
events. A whole-frame retry returned UINT64_MAX-like values even with availability
and no disjoint. The final implementation rejects disjoint and implausible durations
and reports quality counters, rather than presenting them as milliseconds. These
device observations do not establish a general Mali limitation.

The first 67% particle window averages roughly 224 batches, 187 blend-change breaks
and 36 texture-budget breaks. A later baseline has 275/246/29 because weather and
lifetime populations change with frame partition. Raising four texture slots would
only address the smaller category, while alpha/additive order must stay intact.
The current shader already caches identical typed uniforms in `gs_apply`, but each
particle batch still rebound FOW and all four textures, queried culling and toggled
it off/on around the draw.

The particle pass now installs common matrices/FOW/culling once and compares texture
bindings against stack-local state. Immediate billboards/ribbons start a new scope;
no GL binding cache survives another model/UI draw. Draw/triangle order, blend/depth
contracts, sampler layout and vertex bytes are unchanged. The four-draw regression
reduces texture binds from twenty to five and cull queries from four to one. Tests
also clobber bindings between calls, render a following immediate sprite and verify
the original cull state with both initial states. Ten assertions fail before the fix.

The full test target passes all 33 suite summaries. Renderer view/profiling passes
3,629 assertions in 19 tests; MDX UI/particle coverage passes 1,295 in 25 tests.
The diagnostic cvar's registration/inverse/non-archived behavior has a command-path
regression that failed before registration. Desktop WC3/SC2/WoW, Android signing/
alignment and the `DIAG_OUTPUT` syntax build pass without compiler warnings.

Two CPU-only (`r_profile 1`) 67% Honor pairs ran in old/new then new/old order:

| Per-run median | Before state scope | Scoped state |
|---|---:|---:|
| FPS, pair 1 | 46.0 | 45.6 |
| FPS, pair 2 | 46.3 | 45.6 |
| Particle batches, pair 1 | 274.7 | 295.3 |
| Particle batches, pair 2 | 220.7 | 298.0 |
| Particle CPU wall time, pair 1, ms/frame | 4.288 | 4.684 |
| Particle CPU wall time, pair 2, ms/frame | 3.831 | 4.771 |

These captures establish **no FPS improvement** from the scoped state change.
Particle batches were 7.5%/35.0% higher in the new runs, while submitted vertices
changed less than 0.4%. Battery temperature stayed 33.4 C in all four windows.
The deterministic test establishes removed GL calls; changing particle populations
prevent attributing the small FPS differences to that change. Raw data and APK hashes
are in `paired/` and `comparison.json`. The before APK is
`06ef8ca84d5e866e834b6f715aca81c4e5754a9d56217236918fdab112870a80`;
the measured scoped-state APK is
`97a2c799cfe1a5a6ad7bcc3161c6a3f341677ceac659b748c0a3300adcaea9a3`.
The final rebuild only adds source comments and is
`304a846a2f68700eaa79aae32e4bf14f811f4b35c51d6482c620ecc54cdef406`;
its renderer `.text` is byte-identical to the measured scoped-state library
(SHA-256 `7cca2dcddf7c30e1bc261b84540a45b0cdd24c2d31c1220dba6180f5d0d3bae0`).
Honor `ImportTests` passes the new default/valid/invalid/smoke-isolation argument
checks, importer cases and all four native smoke tests with clean process exit.

A short final-device query validation (10-second warmup, 6-second window) rejected
16 impossible durations and accepted only two intervals within the host bounds.
Those sparse intervals are not sufficient to establish representative GPU frame time.
`gpu-validation/` is a quality/rejection check, not an FPS benchmark.

The final 67% capture with `r_profile 0` reaches 46.4 FPS and 499.8 draws/frame;
no timing/counter reports appear in its window. CPU-only captures above remain in
the same approximate FPS range, but their particle/draw populations differ, so this
does not establish a precise profiler-overhead percentage. Evidence is in `normal67/`.

Local APKs, CPU windows, screenshots, battery state and summaries are under
`build/android/optimization/honor-profile/`. The remaining measured candidates are:

1. Scale-target bandwidth: the current path copies the entire drawable into the
   scaled target, blits back and retains transient depth. Investigate depth
   invalidation after its last consumer and narrower background-preservation work;
   preserve transparent model/UI backgrounds and viewport/scissor behavior in tests.
   In GLES, invalidate the scaled target's unused depth before switching away from
   its draw framebuffer, rather than waiting for the next frame. This follows
   [Arm's framebuffer lifetime guidance](https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/mali-performance-2-how-to-correctly-handle-framebuffers)
   and is an unmeasured candidate, not an established Honor FPS gain. The color
   attachment must survive until the output blit.
2. Solid/alpha model passes: roughly 7 ms/frame of CPU wall time at 67%. Measure
   useful geoset submissions versus pose evaluations before sharing poses across
   passes; view-dependent billboard matrices and global scratch require explicit lifetime.
   The original `MDX_RenderModel` bound bones and collected lighting before filtering
   geometry for the current phase. The empty-phase optimization below rejects that
   work while preserving particle/ribbon poses; cross-pass pose reuse is still unimplemented.
3. The unconditional `SDL_Delay(1)` after swap measured about 1.1 ms/frame. It is
   removed in the following experiment; frame caps remain owned by the main loop.

Before generalizing credits improvements to gameplay, capture a representative map
with many moving/combat units. Renderer stage timers exclude preceding server/client
work; credits cannot establish the cost of gameplay AI, pathfinding or snapshot/HUD
preparation. Keep the normal APK's startup behavior when doing this comparison.

### Renderer post-swap sleep

`R_EndFrame` previously called `SDL_Delay(1)` after every native swap, including
uncapped and synchronized frames. History traces it to `85197030` (May 17, 2023,
"Added 2 shader programs"); that history does not establish its original motivation.
`Com_LimitFrameRate` already owns `com_maxfps` pacing in the native main loop.
The renderer now presents and returns without an extra sleep; browser presentation
is unchanged. The `r_profile` `sleep_ms` field stays zero for direct old/new comparison.

`tests/test_renderer_frame.c` includes the production renderer and native main entry
translation units, controlling clocks/configuration and capturing OS/profiler calls. Six assertions fail
with the old renderer. After removal, 44 assertions pass in two tests: each frame
presents without a timer sleep, VSync toggles 0/1 in both directions and unchanged
values avoid redundant setters, while the real limiter waits for the remaining budget
at 30/60/120 FPS. Uncapped/negative-rate, dedicated and already-over-budget paths do
not wait. The new suite is part of `make test` (34 passing suite summaries).
WC3/SC2/WoW desktop builds, the Android APK signature/alignment and `DIAG_OUTPUT`
syntax check pass. No runtime API, protocol, save or shader layout changes are needed.

The baseline APK is SHA-256
`304a846a2f68700eaa79aae32e4bf14f811f4b35c51d6482c620ecc54cdef406`;
the no-sleep APK is
`84b06879687fb7e83e6e43bdc7bffff3e42232a96009f1a19dd16450e57d8fea`.
Local artifacts are under `build/android/optimization/honor-nosleep/`.

Two fresh-process Honor pairs used 67%, CPU-only profiling, a 40-second warmup and
15-second capture in old/new then new/old install order:

| Per-run median | With renderer sleep | Without renderer sleep |
|---|---:|---:|
| FPS, pair 1 | 46.2 | 47.1 |
| FPS, pair 2 | 45.7 | 46.2 |
| Sleep wall time, pair 1, ms/frame | 1.097 | 0.000 |
| Sleep wall time, pair 2, ms/frame | 1.100 | 0.000 |
| Swap wall time, pair 1, ms/frame | 4.007 | 5.406 |
| Swap wall time, pair 2, ms/frame | 3.934 | 4.903 |
| Draw submissions, pair 1 | 523.0 | 454.4 |
| Draw submissions, pair 2 | 529.3 | 528.2 |

The observed FPS increase is 1.9%/1.1%, not the roughly 5% suggested by subtracting
1.1 ms from the whole frame. Swap waiting rises by about 1.4/1.0 ms, absorbing most
of the removed sleep. This is driver/presentation wall time, not measured GPU execution.
The first comparison also has fewer particle batches (290.0 to 222.0); the reverse
pair is closer (299.1 to 297.1), with draw/vertex counts within 0.3% and a 0.5 FPS
increase. These are modest scene observations, not a guaranteed FPS percentage.
Battery temperatures are 33.5 C for both pair-1 runs and 33.6 C for both pair-2 runs.
Raw windows, screenshots, hashes and settings are in `paired/` and `comparison.json`.
Honor `ImportTests` also passes importer/argument isolation and all four native
smoke tests with a clean process exit. The installed test build keeps frame caps
in the main loop and removes only the renderer's unconditional extra sleep.

### Gameplay benchmark map candidates

Two author-published maps on Hive list legacy Warcraft III 1.24a–1.28c:

- [Battle Simulator v0.19d](https://www.hiveworkshop.com/threads/battle-simulator-v0-19d.329610/)
  supports chosen squad compositions/counts and starts combat with `-start`.
  Prefer it for controlled unit-combat/movement load, rather than base-building AI.
- [WTii's Unit Tester 1.3](https://www.hiveworkshop.com/threads/wtiis-unit-tester.239333/)
  offers unit/creep scenarios and terrain for micro/macro practice.

Battle Simulator was downloaded through the page's **Legacy download** ZIP and
extracted locally to `build/android/optimization/maps/BattleSimulatorV0.19d.w3x`.
SHA-256: `ce375f3525fcf635c980172f42beb5cc4c926a4387c015272b151b01d21fee99`.
`mpqtool cat war3map.j` succeeds; the script parses squad counts with `S2I`,
registers the `-start` chat trigger and contains no `Blz`/`Dz` API prefixes.
The original Battle Simulator subsequently reached a rendered world on Honor;
the controlled derivative capture below exercises movement and combat. This is
not full map-completion/compatibility coverage; WTii's map remains untested. The site's regular
Download endpoint initially returns an HTML ZIP-generation page, not a map;
validate the downloaded format before attempting to load it.

For a later benchmark, fix squad types/counts, arena, camera and capture timing;
measure stationary, moving and fighting units separately. Use a fresh map load
for each run so deaths, corpses and accumulated triggers do not change the workload.

### Honor 200-unit Battle Simulator capture

The October 16 Honor capture uses the **already installed normal APK**, SHA-256
`aaac4767407038d64feb91629975017566187ddf4c7c7b1f3dc2a989591a09aa`.
The source checkout at capture was `5a50a7eef0fed981fea61728bad268025e314489`
with unrelated tablet/input edits present; the pulled APK and its own native
libraries are retained for matching symbols. No engine source or APK was changed
for this measurement.

The downloaded original remains unchanged. `BattleBench200.w3x` is a diagnostic
copy, SHA-256 `1a985c06f16b5b19dc8481cd35e0cb9d4fc09d6af35fdb1d231915a366fe87d5`.
It adds JASS helpers while retaining all 17 original archive members; extracted
bytes of every member except `war3map.j` were checked against the original.
**`mpqtool pack` creates a new archive**, so copying an MPQ and packing only the
replacement script loses the other map files. Extract and repack every member.

The helpers add 100 Footmen (`hfoo`, Player 2) and 100 Grunts (`ogru`, Player 4)
on two 10x10 grids, 64 world units apart, in arena 6. Original preplaced units,
buildings and triggers remain active outside the view, so this is 200 additional
units, not a 200-entity world. Friendly teams stand, then move toward alternating
formation destinations every six simulation seconds. Combat recreates the roster,
runs the original `Trig_StartBattle_Actions` alliance change and issues attack-move
orders. Status screenshots confirm 200 live units in standing/moving phases and
200 to 194 / 193 during the two combat windows. An attack-order count is not a
count of units simultaneously landing hits.

Camera target is `(4600,-4600)`, interactive distance 3000, edge scrolling disabled;
drawable is 1920x1200, 67% world rendering is 1286x804, MSAA 0, VSync 0, cap disabled,
`r_profile 1`. `jass Bench_*` runs without a current-player context, so its camera
natives do not move the player camera here. Use the existing `camera move 4600 -4600`
command and `zoomkey -20` after closing the console instead. The temporary bindings
use one command each; `camera edge 0; jass Bench_Setup` produced an unknown command
with leading whitespace after the semicolon in this installed build.

Six Simpleperf `task-clock:u`, 1000 Hz recordings ran for 15 seconds each, with zero
lost samples. FPS/renderer logs bracket recorder startup/postprocessing too, covering
about 17 seconds; values below are medians of the one-second reports. The second
sequence recreated the roster in the same process, rather than fresh-loading the
map. Time of day was not frozen, fog exploration persists and draw/vertex counts
drifted; these are baseline scene observations, **not a matched-work optimization
A/B comparison**. Battery temperature rose from 32.1 to 33.1 C across the captures.

| Per-run median | Standing 1 / 2 | Moving 1 / 2 | Fighting 1 / 2 |
|---|---:|---:|---:|
| FPS | 22.7 / 21.5 | 18.8 / 18.3 | 18.8 / 17.9 |
| Solid + alpha CPU wall time, ms/frame | 23.81 / 24.46 | 25.56 / 26.63 | 25.68 / 26.16 |
| Draw submissions/frame | 1067 / 1138 | 1094 / 1160 | 1089 / 1151 |
| `SV_Frame`, inclusive main-thread user CPU samples | 25.33% / 25.34% | 29.97% / 30.60% | 31.06% / 32.96% |
| `MDLX_BindBoneMatrices`, inclusive main-thread user CPU samples | 21.79% / 21.26% | 20.94% / 20.31% | 21.15% / 19.38% |

Main-thread percentages normalize the raw report's all-app share by its `SDL_main`
share. They are sampled CPU activity, not frame/GPU milliseconds; parents overlap
with children and must not be added. `SV_Frame` includes game simulation and snapshot
preparation. Particle counters are zero in these windows with ordinary particles
enabled, so this scene selects model/animation and simulation work for follow-up.

Within simulation, `regen_aura_cache_update` accounts for 4.23–4.48% of **all-app**
user CPU samples and `S_ResolveAbilityAlias` for 4.03–4.11% inclusive, overlapping
shares. In `skills/s_hero_passives.c`, provider discovery runs once per simulation
frame but tries several aura families against each edict; alias resolution parses
authored ability lists and examines runtime/hero abilities. Profile a cheaper provider
discovery path next, preserving add/remove, rank, data-generation and map lifecycle
invalidation. Model-pose reuse/empty-phase rejection remains the other measured
candidate. This test does not cover base-building/economy AI or obstacle-heavy routing.

Artifacts, map/helper sources, pulled APK/libraries, six profiles/reports, screenshots,
temperature and `comparison.json` are under `build/android/optimization/honor-battle/`.
The temporary user autoexec is removed and the saved config restored after capture;
the normal APK still starts through its usual launcher/menu flow.

### Batched aura provider discovery

The 200-unit capture selected repeated owned-ability resolution in
`skills/s_hero_passives.c`. Provider discovery previously invoked the single-family
resolver for 14 distinct aura codes per edict per simulation frame. It now submits
the 19 output slots as one batch, walking native, runtime-added and Hero abilities
once per edict. Matching an owned alias resolves its authored base once for all
queries, while preserving first-match ownership priority and Hero rank. The public
single-family resolver shares the same walker. There is no new retained cache or
network/save-field change; see [provider discovery contract](regeneration-auras.md#provider-discovery-cost).

The work-count regression fails three assertions with the old implementation.
After the change, 155 assertions in three focused tests pass in each ROC/TFT mode,
including mixed families, removal, rank changes and rawcode/base dual matches.
Both baseline and final full runs pass 34 suite summaries. The desktop WC3 build
and Android APK signature/16-KB alignment checks pass.

The on-device comparison uses fresh processes in old/new then new/old order.
Each process loads `BattleBench200Fixed.w3x`, recreates the same 100+100 roster,
freezes time of day at noon and fixes target `(4600,-4600)`, distance 3000, 67%
rendering, VSync off, no FPS cap and CPU-only profiling. Standing warms up for ten
seconds, movement for three; each phase has a 15-second Simpleperf recording with
FPS reports bracketing recorder startup/postprocessing. Both phases keep all 200
units alive. This controls lighting and fresh-map fog exploration more tightly
than the earlier exploratory capture; it is not a comparison against those earlier
FPS values.

The fixed map is SHA-256
`cd4d3c86593b18ef7c62d3f3cf3b01de49ccf76660bd3c56778f07056797eb4c`.
The baseline normal APK is
`b5799db50ed0dcf78cfee440c80ef5a2554ff86818a9263bccf802b505ee5917`;
its game library is byte-identical to the earlier installed/captured APK's library.
The measured optimized APK is
`05736dec223b012c2e0d243ecf262260964f11d00bc188bf07dbc26763d3fda6`.
Raw logs, APKs, fixed map/script, profiles, screenshots, temperatures and
`comparison.json` are under `build/android/optimization/honor-aura/`.

| Per-run median / sampled provider CPU time | Old | Batched |
|---|---:|---:|
| Standing FPS, pair 1 | 21.1 | 21.6 |
| Standing FPS, reverse pair | 21.3 | 21.6 |
| Moving FPS, pair 1 | 18.0 | 18.55 |
| Moving FPS, reverse pair | 17.8 | 18.6 |
| Provider CPU seconds / 15 s, standing pair 1 | 0.6310 | 0.1920 |
| Provider CPU seconds / 15 s, standing reverse pair | 0.6133 | 0.1972 |
| Provider CPU seconds / 15 s, moving pair 1 | 0.6184 | 0.1969 |
| Provider CPU seconds / 15 s, moving reverse pair | 0.6266 | 0.1986 |

Provider CPU seconds estimate the inclusive `regen_aura_cache_update` event count
from user task-clock samples, rather than GPU or renderer wall time. The observed
reduction is 67.8–69.6%; all eight captures have zero lost samples. Standing FPS
increases 1.4–2.4% with identical medians of 1132 draws / 446020 vertices. Movement
increases 3.1% / 4.5%, but pair 1's old capture has 5.3% fewer draws and 3.2% fewer
vertices than new; it is not matched-work evidence for that precise FPS percentage.
The reverse movement pair's vertex counts differ by 0.006% and draws by 0.18%.
Battery temperature progresses from 31.5 to 32.3 C through old/new/new/old order;
thermal/frequency and motion-phase variation still limit end-to-end attribution.
These are modest scene gains, not a universal gameplay FPS multiplier.

The final APK only follows a source line-wrap cleanup and is SHA-256
`30ff5c8659f8256990672b3b8df19d18e537ac444fef3a2dbbb755e1f3e85517`.
Its game-library `.text` is byte-identical to the measured optimized library
(SHA-256 `edb7abe2b7fa13a8909b833d5140e0808202a137826ccbe2398ddc1450d5c897`).
The final source passes the full 34-suite run again. Honor `ImportTests` passes
import/argument isolation and all four native smoke tests with clean process exit.
The optimized **normal** APK is installed; temporary autoexec/bindings/settings are
removed/restored and ordinary startup does not select this benchmark or credits.

### Reject empty MDX render phases

`MDX_RenderModel` now checks for visible geosets belonging to the current phase
before normal matrices, shader setup, bone evaluation and local-light collection.
The October 8 water-ordering change (`f99103bb8`) split solid and alpha submissions,
but the pose remained unconditional even for an opaque-only model's empty alpha pass.
This change removes that empty submission rather than retaining poses across views.

`MDLX_HasGeosetPass` uses the existing material-pass and GEOA visibility rules.
Carried Gold/Lumber sequence remapping happens first; otherwise visibility could be
read from the wrong sequence. Instance tint, including explicit zero alpha, still
moves opaque layers into the alpha phase. The resolved color is passed to
`MDLX_RenderGeosets` so both decisions use the same value. Models with PRE2 emitters
retain the non-alpha pose even without geometry; ribbon models retain the alpha
pose, including fading trails. Local model lights are only consumed by this model's
geometry/ribbons and do not independently require an empty submission. Attachment,
event and detached-ribbon owners remain outside this early return. No retained
cache, model/entity layout, shader, network or save contract changes.

`tests/test_mdx_geoset.c` compiles the production geoset renderer into the existing
renderer-model/shadow suites, intercepting only GPU/map-texture boundaries. Its pose
counter wraps the real `MDLX_BindBoneMatrices`; tests assert both draw counts and
that empty phases leave the global pose scratch untouched. Coverage includes all
blended modes, alpha-key/opaque/mixed materials, zero layers, default/translucent/
explicit-zero tint, animated disappearance/reappearance, carried-resource remapping,
geometry-free PRE2 accumulator updates, ribbon-phase poses and shadow filtering.
The initial five-test regression fails the old implementation in 22 assertions;
the final seven-test fixture also checks a later geoset supplying the phase absent
from the first. It passes with and without `USE_SHADOWMAPS` (9,026 and 9,025
assertions across 183 tests in each complete renderer suite).

The Honor A/B uses the same fixed-noon `BattleBench200Fixed.w3x`, 67% render scale,
200 living units, camera and idle/move warmups as the aura comparison. Each run
starts a new native process. The order is old/new/new/old, two 15-second Simpleperf
recordings per process; the corresponding FPS windows include recording setup and
postprocessing. Baseline normal APK SHA-256 is
`30ff5c8659f8256990672b3b8df19d18e537ac444fef3a2dbbb755e1f3e85517`;
optimized normal APK is
`a897c472b0607d0fcac97c1182405423d8cfd291236625cce5d405da8e80af46`.

| Median FPS | Old, pair 1 | New, pair 1 | New, reverse pair | Old, reverse pair |
|---|---:|---:|---:|---:|
| Idle | 21.3 | 22.3 | 21.7 | 21.7 |
| Moving | 18.55 | 18.65 | 18.60 | 18.70 |

**This scene does not establish an FPS gain from empty-phase rejection.** The first
idle improvement is absent in the reverse pair, and moving differences change sign.
Idle work is exactly 1,132 draws / 446,020 vertices in every run. Moving medians stay
within 1,157.95–1,161 draws and 451,792–451,803.5 vertices. Battery temperature is
32.1 C throughout except the first idle run at 32.4 C; all eight recordings lose zero
samples. Screenshots retain both armies and 200 living units. Particles remain
enabled and their retained-list counter is zero in this scene.

Bone evaluation remains about 19–21% of sampled main-thread user CPU. Dividing
inclusive sampled CPU time by the approximate frame population gives roughly
8.4–8.8 ms/frame idle and 9.3–9.7 ms/frame moving across both APKs; these estimates
mix the 15-second sample with a slightly wider FPS window and are not GPU timings.
The renderer's solid-plus-alpha CPU wall medians likewise overlap (old 24.04–24.83
ms idle / 26.56–26.62 ms moving, new 24.20–24.44 / 26.24–26.50 ms). The fast path
provably removes poses for empty phases in tests, but does not remove a measurable
share of this map's bone cost. Do not report the first idle pair as a general 5% gain.
The next bone investigation should distinguish caller/view duplication and local
track sampling from view-dependent global/billboard matrix construction before
introducing pose reuse.

Local APKs, screenshots, CPU reports, capture scripts and `comparison.json` are in
`build/android/optimization/honor-empty-pass/`; the map's retail members are unchanged
from the previous fixed-noon diagnostic copy. The final change is game-renderer-only;
shared client/network/save contracts are unchanged. The full 34-suite run and desktop
WC3 build pass; Honor `ImportTests` passes argument/import isolation and all four
native smoke tests with clean process exit. The normal optimized APK is installed
and its ordinary menu is visually verified. Temporary autoexec/bindings are removed,
rotation/USB-awake settings restored, and `config.cfg` is byte-identical to the
pre-capture file (SHA-256
`57a2993107e931d421c554ec80f0e8d279930cb01a245311842f923b59a51df9`).

### Honor: 200-actor performance

This result covers only the two-model allied scene. The later
[mixed-army combat benchmark](combat-benchmark.md) measures **19.40 FPS** in
NightElfX01 and **14.19 FPS** in a 20-type battle on the same APK. The broader
campaign/combat goal of at least 40 FPS remains unmet.

The final ordinary APK reaches the agreed **average >= 40 FPS** target in the
fixed moving scene at 67% scale, with all 200 actors and independent animation.
The default texture-palette transport remains enabled; uniform-palette and
texture-orphan experiments remain off. Two fresh-process, nominal 60-second
moving captures without Simpleperf gave:

| Capture | Idle FPS (30 s) | Moving weighted FPS | One-second moving range |
| --- | --- | --- | --- |
| Production transport, before scratch-overflow cleanup | 54.92 | 51.54 | 43.44–53.52 |
| Final ordinary APK | 55.09 | 43.16 | 35.41–47.71 |

The criterion is an average, not a 40-FPS floor for every second. The variance
remains material; these are absolute validation windows, not an estimate of the
small overflow cleanup's gain. Stable 60 FPS is not established. The final APK
SHA-256 is `5dd1cb2f4f3396201cd44a70f824dcb3b5dbf2d9916b2f4fbdf1316b8d1f9cc0`.
Raw data, screenshots and frame/elapsed totals are in
`honor-target40/{production-check,verified}/`. The final moving window contains
2580 frames over 59783 ms of complete `R_STATS` intervals. Earlier opt-in uniform
captures below use a different transport and must not be combined into a gain
estimate for this default configuration.

The target is **average FPS >= 40**, then 60, with the existing 67% render
scale, fixed camera, 100 Footmen + 100 Grunts and independent animation clocks.
Do not report a paused simulation, reduced roster or shared/quantized pose as
meeting this target. The initial APK is preserved under
`build/android/optimization/honor-target40/baseline.apk` (SHA-256
`a897c472b0607d0fcac97c1182405423d8cfd291236625cce5d405da8e80af46`).

The renderer now keeps a bounded, render-clock-scoped MDX pose cache: 1024
lookup slots and 65536 compact node matrices (4 MiB matrix arena). Its exact
inputs are model lifetime, old/current frames, interpolation fraction, render
time and view, the effective per-entity global-sequence clock, plus the instance
matrix if any requested node or ancestor is billboarded. Unrelated hidden billboards do not prevent sharing geometry poses.
Model release invalidates it before addresses can be reused. A full cache
evaluates normally. Geometry-only packets request visible-geoset bones and
their ancestors; later socket/effect/full-pose consumers expand a partial entry.
Zero-time views bypass caching because global sequences then read SDL time.

Held construction can give identical model/frame inputs different global-track
phases. `MDLX_QueueEntityPose` captures the entity animation scope before reserving
work; each pose job carries that effective time. Packet samplers use thread-local
scopes in both worker and owner modes. Cache keys include effective time for models
with global sequences. Batches flush when those clocks differ and restore the
stored actor's scope while assembling palettes or replaying a singleton. Scoped
time is restored on every early return. The held-clock regressions in
`tests/test_mdx_pose_workers.c` and `tests/test_mdx_geoset.c` cover independent held
actors, worker/cache reuse, packet modes 0/1/2, batch boundaries and resume.
Billboard fixtures must supply a valid view/projection matrix; tests that mutate
loaded node flags must invalidate the pose cache before reusing the model.

World MDX draws can form ordered packets of up to 256 compatible actors.
They retain **one complete pose per actor**, uploaded as nearest-filtered
RGBA32F bone texels; material alpha/color, texture and UV state must agree.
Effects, lights, embedded child models, selection/hover presentation and
nonuniform bases retain immediate rendering. Alpha packets require a single
layer per actor to preserve blend ordering. Flush before water, incompatible
draws, map/model replacement and renderer shutdown. The private typed shader
extension and sampler ownership are described in
[model shader](../../architecture/model-shader.md#game-owned-skinned-instances).
The CPU texture staging allocation grows to actual packet demand and is
released with the renderer; it is not a fixed 16 MiB allocation.

Minimap contacts now append their original quads to a 128-marker stream and
flush on texture changes or capacity. Team colors, UVs, order and every marker
remain unchanged. Headless tests compare the submitted production quads,
including hidden/non-contact entries and texture transitions.

Exploratory single captures (15-second Simpleperf windows, not paired final
results) reached 30.0 idle / 26.0 moving FPS after per-actor palette instancing,
then 32.8 / 25.9 after minimap batching. Idle draws fell from roughly 1132 to
150 while keeping 446014 submitted vertices. A subsequent partial-pose run
measured 33.5 / 21.7, with much higher moving swap time; **those exploratory captures did not establish 40 FPS**, and the moving variance
requires repeated paired recordings.
Raw artifacts and each tested APK are in `honor-target40/{pose,shared,
instances,palette,minimap,partial}`. These are local diagnostic artifacts,
not distributable retail assets.

Disease Cloud previously ran target-mask/visibility validation for every
remote edict before comparing distance. The unchanged radius predicate now
runs first. The regression adds 128 units just outside/beyond a non-stock
radius, checks that validation work stays constant, and verifies infection
and the authored damage tick at the inclusive boundary. Existing independent
source/refresh tests retain coverage of infection outside the aura.

Attachment queries now collect visible matching sockets before binding only
their ancestor closure. `MDLX_CollectAttachmentModels` also excludes empty
effect paths before consuming the caller's output capacity; ordinary socket
queries still return those sockets. A four-node sparse/unordered fixture
checks limited results, all results, ancestors, empty-path restoration and
unchanged world transforms. The red test had seven failures; both renderer
variants passed after the change. Exploratory captures reached 37.6 / 29.6
FPS with the Disease Cloud change and 38.9 / 32.9 with attachment subsets.
Queries keep 32 result pointers on the stack and allocate larger requested
outputs rather than capping the attachment count at the number of node IDs.
A 1025-record fixture with repeated node IDs reproduced that truncation; it
also retains the caller's output limit and supports missing-node transforms.

Within one pose evaluation a stack-scoped sampler resolves the two animation
sequences once and reuses interval bounds for consecutive samples of the same
track. It does not retain model/track pointers across calls or alter key times,
interpolation, tail wrapping or global-sequence clocks. Tests cover shared and
different old/current sequences, interpolated matrices and a subsequent public
query after the scope has ended. Existing Hermite/global-sequence tests remain
part of both renderer builds.

The GPU upload subsequently became compact: visible geoset bone IDs map to
consecutive packet slots rather than reserving holes for sparse IDs, helpers
and hidden geosets. Each affine matrix retains all twelve XYZ float values in
three RGBA32F rows, reconstructing only its constant final row on the GPU.
The regression uses node ID 999, different translation frames, a 90-degree
rotation and nonuniform bone scale, checking raw uploaded rows and per-actor
placement. It reproduced the oversized transfer and wrong expected layout
before the transport change; both renderer variants pass afterward.

`R_ConformGroundSurfaces` now collects eligible walkable surfaces once, in
view order, and scans only that list for each conforming actor. Every frame
clears old support flags, including views without any surfaces. The first 64
surface pointers fit on the stack; larger views allocate their actual view
capacity and release it before returning. There is no surface-count truncation.
Tests cover the work bound, no surfaces, hidden/model-less objects, more than
64 surfaces, highest hit, no hit, offsets, elevator snapshot height and UI views.
The old two work-bound assertions failed before the change.

`R_STATS` includes `frames` and `elapsed_ms` so a benchmark can report
`1000 * sum(frames) / sum(elapsed_ms)` instead of a median of rounded FPS
samples. Run FPS captures separately from Simpleperf, and reset the roster
before each moving trial: a sequential 67/50/100/67 resolution experiment
drifted from 36.6 to 29.9 FPS at the same 67% setting. It does not establish
a clean resolution-response curve. At that stage the compact affine upload
measured 44.4 idle / 29.8 moving in a single profiled exploratory run.

`r_mdx_bone_orphan` is an experimental transfer diagnostic (default 0): it
redefines texture storage before each packet upload so the driver can keep
earlier reads independent. It changes no bones, material or geometry. The
headless test checks that each upload has a fresh storage definition; whether
this helps Mali must be established by repeated device trials.

The four moving trials with roster resets gave weighted averages 32.46 / 33.85 /
29.93 / 29.44 FPS in reuse/orphan/orphan/reuse order. This is not convincing
evidence for changing the default. The uniform-palette prototype gave 51.80 idle /
28.98 moving FPS (30 seconds per phase, no Simpleperf). It remains opt-in: the
heavy moving phase did not improve. Both variants preserve each actor's pose;
neither result establishes the 40-FPS moving target.

`com_profile 1` subsequently measured about 199 ms of server work per second
idle and 251–264 ms moving, with moving server calls around 30 ms. A bounded
no-refresh diagnostic capped at 1000 loops/s remained alive for its recording;
it measured about 513 ms/s and 58-ms server calls, illustrating that this mode
also changes CPU scheduling/frequency. Its loop rate is not a gameplay FPS.
The 200-Hz moving Simpleperf capture still identifies `SV_AreaEdicts_r` and
`G_FowUpdate` as meaningful CPU costs. Data is in
`honor-target40/frame-profile/paired/after/{move-profile,simulation-only}`.

The subsequent code change replaces repeated FOW blocker-box scans with an exact
summed-area table rebuilt lazily per update and hoists disk writes to row spans;
see [fog CPU rasterization](fog-and-cinematics.md#exact-cpu-rasterization-and-blocker-queries).
Four work-bound assertions failed before the change, and 435 assertions in 15
focused tests pass in each archive mode.

After reconnecting Honor, four fresh-process trials in old/new/new/old order
used the same uniform-palette setting (`r_mdx_uniform_instancing 1`), 67% scale,
200 actors, camera and warmups. Each phase records 30 seconds without Simpleperf.
The pre-FOW APK is `frame-profile/optimized.apk`; the preserved post-FOW APK is
`fow-paired/optimized.apk`, SHA-256
`5d2d6aef42bd6e8b6eb9a1b58cc995259110b66dc406697decef0a5c115f0a6b`.

| Version | Idle FPS, two trials | Moving FPS, two trials | Pooled moving FPS |
| --- | --- | --- | --- |
| Before exact FOW optimization | 49.79 / 52.39 | 30.66 / 29.92 | 30.29 |
| After exact FOW optimization | 51.54 / 53.75 | 33.80 / 32.91 | 33.36 |

Pooled FPS uses total frames / total elapsed time, not a mean of rounded samples.
The moving gain is about 10.1%; that FOW-only comparison did not meet 40 FPS. Median server work
dropped from 256–259 to 227–233 ms per second. A separate 200-Hz CPU recording
has `G_FowUpdate` at 2.05% inclusive / 0.50% self of all-app sampled CPU, compared
with the earlier 4.93% / 3.98%; these shares are diagnostic, not GPU times.
Screenshots retain all 200 living actors with Move orders and nonzero displacement.
Artifacts and complete per-window values are in `honor-target40/fow-paired/`.

The Move deflection search now reuses one ordered broad-phase candidate list
inside `CM_SlideRoute`. The enclosing box covers every direction at the same
origin and step length; each trial retains its original box and swept-circle
test. The list never survives the search, so later unit movement/relinking
cannot leave stale neighbours. An overflowing enclosing query uses the original
per-trial query rather than truncating candidates. The boxed-unit work test
failed before this change; all 77 assertions in 22 collision tests pass in both
archive modes, including immovability, later-query invalidation and overflow.
The enclosing-query predicate detects the extra neighbour without exhausting
the backend output buffer; optional scratch overflow adds no `MAXCOUNT` logs.
The old output-limit regression failed before that cleanup.
Four fresh-process trials (`collision/`, old/new/new/old, the same capture setup)
measured 29.99 / 29.59 moving FPS before and 33.65 / 35.68 afterward: pooled
29.79 -> 34.68 FPS. Idle varied from 41.87 to 52.30, illustrating device variance;
do not add these separate experiments' percentage gains. Separate CPU recordings
put `SV_AreaEdicts_r` at 4.81% inclusive before and 1.84% after. The new APK hash is
`9bbf8d4ebef2a31150b58fc6015fc147a4bedf15ab4e5e174d56ba77e619d885`.
The attempted external core-affinity experiment failed at the initial `taskset`
operation; no affinity FPS window was recorded. Android reports `Permission
denied` for the `run-as` scheduler probe. It is not evidence of a core-affinity
gain or of a frame-rate cap.

A render-clock-wide track-bound cache prototype used 4096 direct-mapped slots
(128 KiB on arm64). Its work-bound regression passed, but four fresh-process
trials (`ranges/`) gave 46.75 / 36.04 moving FPS without it and 35.86 / 45.56
with it (pooled 41.38 versus 40.72). It was removed completely, including its
wiring and test contract; the stack-scoped sampler above remains. One successful
40+ FPS window does not establish a stable minimum: the same APK/scene varies
substantially, mainly in swap wait. SurfaceFlinger reports a 60-Hz mode, empty
frame-rate override maps and no frame-rate override support. GPU frequency
probes under `/sys/kernel/ged/hal/` and `/proc/gpufreq/` return permission errors;
CPU frequency files are readable. No system frame-rate cap or GPU-frequency
cause has been established by these observations.

`r_norefresh 1` with uncapped main-loop execution is not a simulation FPS
benchmark: skipped renders allow huge loop counts without advancing another
10 Hz simulation tick. A 2-million-frame run can end before a 15-second
Simpleperf recording. The early `pose/paired/*/simulation-only` artifacts
are truncated and must not be used to claim an FPS ceiling.

#### Frame-pacing trace

A separate 10-second `atrace` capture on the normal transport records 412 EGL
swaps over 9.99 seconds. More than 99.9% of the captured SDLThread CPU runtime
is already on cores 6/7, so the failed external affinity experiment does not
identify a useful missing placement policy. `eglSwapBuffersWithDamageKHR` has
a mean wall duration of 4.90 ms and a 95th percentile of 12.10 ms. These waits
are not a GPU execution-time measurement; nested `queueBuffer` spans overlap
and must not be added together. The trace and parsed summaries are in
`production-check/new-default/{frames.trace,trace-summary.json,trace-markers.json}`.
See [Android frame-pacing capture](../../android.md#system-frame-pacing-capture).

The TimeStats dump includes older captures under the same layer name: its raw
`averageFPS` is cumulative, not this scene's FPS. Preserve before/after dumps
and use counter deltas; do not clear existing diagnostic history. The native
layer's frame counter advanced by 622 during the roughly 15-second diagnostic
interval around tracing. Profiling windows are separate from the primary FPS
windows. Native imports/argument isolation and all four smoke tests (six
assertions, clean process exit) pass on Honor with the final APK.


### Offline MDX visibility work reduction (October 16)

The [mixed-army capture](combat-benchmark.md) selected MDX submission for the next offline pass.
`MDLX_IsGeosetVisible` previously sampled the full GEOA RGBA, although visibility
only uses alpha. `MDLX_RenderGeosets` also sampled every geoset while counting
the sort buffer in an alpha-only pass, then sampled them again during collection.
It now shares an exact alpha evaluator with colour rendering, counts without
sampling the inactive solid pass, and checks material-pass membership before
sampling visibility. Colour consumers, including instance-material compatibility,
still evaluate RGB normally. No pose approximation, quality setting, persistent
cache, draw order, shader, vertex format or GPU submission count changes.

The production-entry regression in `tests/test_mdx_geoset.c` failed before the
change: eight visible blended geosets with animated alpha/RGB sampled each track
25 times. Afterwards alpha is sampled 17 times and RGB eight times. Hidden
geosets perform no RGB sampling. Assertions retain interpolated BGR-to-RGB colour,
instance RGBA multiplication, disappear/reappear transitions, global-clock alpha
and the existing finite/nonfinite alpha semantics. The normal and shadow suites
each run 210 tests; release testing also found an older texture-registry fixture
retaining stack texture pointers. Those fixture textures now have static lifetime.

Offline arm64 macOS `-O2` CPU measurements, GPU submission mocked:

| Workload, 200 actors | Before ms / batch | After ms / batch | Observation |
|---|---:|---:|---|
| Eight animated alpha/RGB geosets, alpha pass | 0.18 | 0.11 | 39% lower, synthetic stress case |
| Retail Footman, solid + alpha | 0.635 | 0.620 | 2.4% lower, small relative to run variance |
| Retail Grunt, solid + alpha | 0.750 | 0.705 | 6.0% lower, all six pairs lower |

Values are medians of eight alternating-order pairs for the stress fixture and
six pairs per retail model. Each run has 1000 batches; retail runs vary actor
frames within the first authored sequence and advance the render clock once per
batch. They use the real loader, samplers, full poses and both geometry phases,
with particle/ribbon absence asserted. Retail draw counts are identical before
and after: 600369 Footman draws and 801485 Grunt draws per run. Alpha sample
counts fall from 4000492 to 2200492 and 6202376 to 3202376 respectively; Grunt
RGB samples fall from 1200594 to 594. These are CPU work bounds and local timings,
**not Honor FPS or GPU measurements**. The 40/60 FPS target still needs a device
check on the mixed combat scene and NightElfX01.

The portable stress fixture is opt-in and needs no retail archives:

```sh
make HOMEBREW_PREFIX="$PWD/build/android/sdl-host" test-renderer-model
build/bin/test_renderer_model 'stress_mdx_cpu.*'
```

Use `BUILD=release` with isolated `BIN_DIR`/`LIB_DIR` for timings; the isolated
macOS library directory must also contain the SDL2 dylib (as `build/lib` does).
Before/after source, binary variants, private retail diagnostic harness, raw logs,
JSON summaries and the ordinary APK are in
`build/android/optimization/offline-geoset/` (baseline source is the adjacent
`offline-geoset-before.c`). The APK SHA-256 is
`f00f2346524bdcd3f2130c36ef4c0052c798288a43819e1ff16bc37a8f58ac15`;
`LaunchConfig.CREDITS` is false. Nothing was installed or launched on a tablet.
Full `make test` passes: 34 JUnit reports, 6860 test executions, zero failures or
errors. WC3 debug/release builds, release renderer/shadow suites and Android
build/signature/alignment checks pass. No network or save-format changes.

### Emulator A/B of the MDX visibility change (October 16)

Four fresh-process runs compared the above APK with the prior ordinary APK
`5dd1cb2f4f3396201cd44a70f824dcb3b5dbf2d9916b2f4fbdf1316b8d1f9cc0`
in old/new/new/old order. Native-library and `classes.dex` byte comparison found
only `librealm_renderer.so` changed. Both used the unchanged mixed-army manual
map, 67% rendering (857x536 inside 1280x800), particles 2, fixed
`(4600, -4600, 3000)` camera and the same remaining default quality values.
Each run warmed idle for 20 seconds, measured idle for 25 seconds, reset and
warmed movement for three seconds, measured movement for 25 seconds, then
reset and measured the opening fight for 25 seconds. CPU wall diagnostics
were identical; Simpleperf and GPU timer queries were off.

The API35 arm64 AVD used four cores and ANGLE/Vulkan **SwiftShader software
graphics**. The emulator raised the requested 2048 MB RAM to 2560 MB. These
results cannot be substituted for Honor/Mali measurements.

| Phase | Old run 1 FPS | New run 1 FPS | New run 2 FPS | Old run 2 FPS | Pooled old → new |
|---|---:|---:|---:|---:|---|
| Idle | 14.70 | 11.97 | 12.38 | 11.65 | 13.17 → 12.18 (-7.6%) |
| Moving | 12.18 | 12.57 | 11.99 | 11.66 | 11.92 → 12.28 (+3.0%) |
| Fighting | 11.32 | 11.99 | 11.63 | 11.10 | 11.21 → 11.81 (+5.3%) |

Each run and pooled value uses `1000 * sum(R_STATS.frames) /
sum(R_STATS.elapsed_ms)`. Moving improved 2.8–3.2% and fighting 4.8–5.8% in
both comparison orders. Fight frame time pooled 89.18 → 84.68 ms. Idle changed
sign between pairs (-18.6%, +6.2%); its pooled decrease is not sufficient to
establish a repeatable regression. Retain all windows, including the first old
idle run. With only two runs per version, software rendering and variable combat
populations, this is a small observed improvement under load, not a universal
or statistically established gain. No 40/60-FPS result is demonstrated.

All four framebuffers show 200 living idle/moving actors, accepted movement,
then real spells, HP loss and casualties. Fight end states were at 25/26 ticks,
with 166–170 alive and 75027–78922 HP out of 98290. Draw and particle populations
therefore are not identical between fight windows. No JASS runtime, shader,
unknown-command or `GL_INVALID` errors occurred in the measured processes.
Do not treat changes between `R_CPU.world_ms` and `entities_ms` as a new CPU
bottleneck: both include translated-driver/queue waits.

Artifacts, scripts, screenshots, per-second diagnostics, APK hashes, pooled and
paired summaries are in `build/android/optimization/emulator-geoset/`.
`capture.py VARIANT LABEL` selects `before`/`after`; `summarize.py` retains both
pairs. The excluded pilot reached `R_Screenshot` but failed to write the relative
`screenshots/shot0000.jpg` target on Android. Subsequent captures used native
Android `screencap` outside measurement windows, without changing either APK.
The original emulator APK, autoexec and engine log were restored byte-for-byte;
the original absence of `config.cfg` was restored, only the added map was
removed, and the task-start stopped emulator was shut down. Retail archives,
saves and physical devices were untouched. See `restoration.json` and `review.txt`.

### Emulator CPU follow-up: program bindings and list scratch (October 16)

A fresh Simpleperf capture of the mixed 200-actor scene used `task-clock:u`,
500 Hz, 20 seconds, call graphs, and no concurrent host builds. It contains
2,124 samples / 4.248 sampled CPU seconds. Inclusive shares overlap and are
neither frame milliseconds nor GPU time:

| Stack owner | Inclusive share | Self share |
|---|---:|---:|
| `MDX_RenderModel` | 33.80% | 0.47% |
| `MDLX_BindPose` | 15.49% | 2.50% |
| `MDLX_GetModelKeytrackValue` | 7.91% | 7.91% |
| `gs_apply` | 10.64% | 1.27% |
| emulator `GL2Encoder::s_glUseProgram` | 7.39% | 0.00% |
| game `parse_segment` | 11.16% | 4.76% |
| game `actor_has_skill` | 10.97% | 1.65% |

The first change skips unchanged GL program bindings in the context-owning
renderer, preserving uniform validation and exact per-program uniform caches.
Frame/context invalidation and current-program deletion prevent stale GL names.
The vendored module remains unchanged. See the [binding cache
contract](../../renderer-backend.md#implemented-program-binding-cache).
`r_program_cache 0/1` permits live comparison, and `R_STATS` now reports
`program_requests` and actual `program_binds` per frame.

A single APK (`c249d18f1d7e79139b27ca0b67f218d8a953783a0d989102ee1f02a450d53342`)
was measured in one process with cache modes **0/1/1/0**, independently for
idle, movement and combat. Each window lasted 24 seconds; combat was reset
before each window. The map, 67% resolution, particles and camera matched the
preceding comparison. Frame-weighted work counters confirm the intended reduction:

| Phase | Uncached requests = binds/frame | Cached requests/frame | Cached binds/frame | Elided cached requests |
|---|---:|---:|---:|---:|
| Idle | 754.4 | 748.5 | 50.3 | 93.3% |
| Moving | 823.4 | 822.3 | 104.5 | 87.3% |
| Fighting | 909.6 | 898.7 | 97.5 | 89.1% |

Geometry was retained: idle drew about 515K vertices/frame in both modes;
changing combat populations still vary draw/particle counts. This establishes
less driver submission work, not a reliable FPS gain. During these windows
the host load average reached 51.25 on 10 logical cores, with no task-owned
builds/tests still running. Retain all results, including the unfavorable fight:

| Phase | Uncached FPS, runs 1 / 2 | Cached FPS, runs 1 / 2 | Pooled uncached → cached |
|---|---:|---:|---|
| Idle | 2.68 / 4.04 | 3.54 / 3.77 | 3.35 → 3.66 (+9.1%) |
| Moving | 3.23 / 2.67 | 3.27 / 3.68 | 2.95 → 3.47 (+17.7%) |
| Fighting | 3.22 / 6.65 | 3.31 / 5.03 | 4.93 → 4.18 (-15.3%) |

Idle and fight change sign between comparison orders. Do not present the
pooled percentages as established improvements/regressions or compare these
overloaded windows with the earlier 11–17 FPS sessions. A pilot ANR occurred
during initial asset loading, before any measurement: the native thread waited
in `qemu_pipe_read -> glGenTextures -> R_LoadTextureBLP1 -> R_LoadAssets`;
the Android UI/RenderThread also waited. Its trace is retained separately and
excluded, without disabling Android ANR detection.

The second change removes full scratch-buffer clearing in the two real
`parse_segment` implementations. `G_ActorHasSkill -> actor_has_skill` repeatedly
parses the authoritative `UnitAbilities.abilList` through the **FDF copy** in
the game unity translation unit (1,024-byte scratch), after runtime ability
overrides. The exported JASS lexer uses a separate 16,384-byte scratch, also
consumed by Galaxy. Both already terminate every returned string; bytes after
that terminator are not part of the string contract. No persistent ability
cache or gameplay state was added. `parse_segment2` is unchanged.

`test-list-lexer` compiles both production implementations and counts memory
clears at the libc boundary. Before the fix its work-bound test failed twice;
afterward all 31 assertions pass. It covers a long token followed by short
ones, empty fields, quoted and unterminated strings, oversized quoted values,
cursor advancement and the readers' existing whitespace-tail differences.
The optional `stress_list_lexer.ability_lists` parses 100 × 1,000 four-ability
lists: JASS previously cleared 8,192,000,000 bytes, FDF 512,000,000; both now
clear zero. A local arm64 debug run measured 1.06 → 0.04 ms and 0.08 → 0.05 ms
per thousand lists respectively. Those are instrumented subsystem timings,
not release frame-rate predictions.

```sh
make test-list-lexer test-renderer-model test-renderer-frame test-gl-shader
build/bin/test_list_lexer 'stress_list_lexer.*'
```

The combined ordinary APK has SHA-256
`22579545ca4ea7013ae574be65fd5ff29b6df5e50898cc4b73c651da2c222a2f`.
A follow-up 20-second / 500-Hz CPU capture after its FPS windows contains
2,226 samples / 4.452 sampled CPU seconds. Observed inclusive shares changed
from 7.39% to 1.39% for emulator `glUseProgram`, 10.64% to 4.22% for
`gs_apply`, and 11.16% to 5.03% for game `parse_segment`. The SDL thread's
`__memset_aarch64` self share fell from 4.57% to 0.58%. These support the
selected work reductions; differing host conditions and combat populations
prevent interpreting the percentages as saved frame milliseconds.
`MDLX_BindPose` remains 15.59% inclusive, including the 7.41% self share in
`MDLX_GetModelKeytrackValue`, and is the next substantial sampled owner.

Four fresh-process combined-build runs also used old/new/new/old order,
20-second idle warmups, three-second movement warmups and 25-second windows.
No builds/tests or Simpleperf ran during those windows. The reference APK
was `f00f2346...` and the new APK was `22579545...` above:

| Phase | Old 1 | New 1 | New 2 | Old 2 | Pooled old → new FPS |
|---|---:|---:|---:|---:|---|
| Idle | 8.87 | 7.88 | 10.17 | 9.82 | 9.35 → 9.03 (-3.4%) |
| Moving | 8.42 | 4.34 | 8.83 | 9.16 | 8.79 → 6.56 (-25.4%) |
| Fighting | 8.06 | 4.84 | 9.55 | 9.58 | 8.82 → 7.17 (-18.7%) |

This comparison **does not demonstrate a net FPS improvement**. Keep the
unfavorable first-new run: its idle window itself ranged from 4.3 to 9.4 FPS,
and the second run of that same new APK was roughly twice as fast in movement
and combat. The reverse pair changed +3.6% idle, -3.5% moving, -0.4% fighting.
Host load rose from 4.52 to 20.52; a later snapshot showed the emulator itself
using 740% CPU plus other host work. These observations do not establish the
cause of the variation or prove absence of a regression. Repeat a stable
comparison before claiming frame-rate benefit. The earlier live cache toggle
and exact work tests distinguish verified call reduction from these unresolved
end-to-end timings.

Both versions retained approximately 515K idle and 523K moving vertices/frame.
The reviewed fight framebuffers contain real spells, HP loss and casualties,
with 175 living actors in old 1 versus 189 in new 1: slow wall frames also
advance the combat to different simulation states, so the fight windows are
not identical workloads. Logs contain no shader, JASS runtime or GL-invalid
errors. No 40/60-FPS result is established.

All scripts, raw captures, screenshots, old source, benchmark logs and the
review description are under `build/android/optimization/engine-state/`.
`profile-before/report.txt` owns the sample evidence; `live-ab/rows.json` keeps
all twelve windows, and `combined-comparison.json` retains the complete
four-run results and both comparison pairs. The cache-only A/B APK is retained
separately from the combined ordinary Android build. Full tests pass (35 JUnit
reports, 6,867 test executions); debug/release builds of all three games,
normal/shadow renderer tests and Android signature/alignment checks pass.
Network and save formats are unchanged. The emulator's original APK, autoexec
and engine log were restored byte-for-byte, `config.cfg` restored to absent,
only the added map removed, and the initially stopped emulator shut down.
Retail data, saves and physical devices were untouched; see `restoration.json`.

### MDX neighbour-key sampling (October 17)

The preceding CPU profile still assigns 15.59% inclusive to `MDLX_BindPose`
and 7.41% self to `MDLX_GetModelKeytrackValue`. The sampler previously searched
the start and end of the sequence's entire key range before finding the
current pair. The stack-scoped range cache only saved those two searches for
a consecutive second sample of the same track and sequence.

The sampler now handles a single-key track by checking that key against the
resolved interval and copying it, without a binary search. For longer tracks,
one lower bound on the sample time finds the neighbouring keys. If the right
key and its predecessor belong to the sequence, the existing interpolation
function consumes that exact pair. Before the sequence's first authored key,
copy the right key as before. A key exactly at the sample time can be copied
early only when the next key proves this is not a sequence tail or duplicate
endpoint. Other endpoint, empty-range and wrap cases retain the original
interval-bound path and reuse the already-found time bound.

This adds no retained cache or asset-side lookup table, and does not change
frame precision, interpolation math, sequence selection or the global render
clock. When neighbours prove the sequence contains no keys, leave the output
untouched immediately. When a multi-sequence track has just one key in this
sequence, copy it after that single time search, including an exact match.
This avoids adding a range search to sparse or absent animation channels.
The existing first-global-key-beyond-duration rule runs before the new
constant-track check. Missing sequence/track values still leave the caller's
default unchanged. Network and save formats are unchanged.

Three new production-entry tests cover interior and exact-key poses with
three distinct tracks, constant keys inside/outside sequences and global
durations, duplicate endpoints, empty sequence gaps and tail blending. Empty
intervals before, between and after authored keys and one-key intervals in a
longer track take at most one search. Work
assertions failed before their changes: three bones / two samples previously
required 12 binary searches, now at most 6; single-key tracks require none.
The ordinary and shadow renderer suites pass.

The optional `stress_mdx_pose.sampled_bones` benchmark uses the real pose
binder, 32 bones with a parent chain, T/R/S tracks, constant/global tracks,
200 distinct frame pairs and full-precision interpolation. The pose clock
advances each iteration, so these are fresh evaluations rather than cache
hits. Five before/after/after/before rounds on arm64 macOS `-O2` yielded
medians 0.680 → 0.610 ms per 200 poses (10.3% less time). These instrumented
CPU-only timings are not a claimed whole-frame or Android FPS gain.

```sh
make test-renderer-model test-renderer-shadows
build/bin/test_renderer_model 'stress_mdx_pose.*'
```

Local source snapshots, benchmark binaries/logs, APKs, four-run Android
captures and the review description are under
`build/android/optimization/pose-sampling/`. The reference APK is
`22579545ca4ea7013ae574be65fd5ff29b6df5e50898cc4b73c651da2c222a2f`;
the new ordinary APK is
`ea6687d71580f4c9c8dea209f5d5d83c0a2cba847453b1c4177d4771a69496c5`.
Only `librealm_renderer.so` differs; `LaunchConfig.CREDITS` remains false.

The final emulator comparison retained all four completed runs:

| Phase | Before 1 | After 1 | After 2 | Before 2 | Pooled before → after FPS |
|---|---:|---:|---:|---:|---|
| Idle | 17.49 | 15.68 | 13.18 | 12.40 | 14.98 → 14.43 (-3.7%) |
| Moving | 16.36 | 16.10 | 12.10 | 12.54 | 14.48 → 14.08 (-2.8%) |
| Fighting | 16.00 | 13.82 | 12.41 | 11.03 | 13.49 → 13.12 (-2.8%) |

This does not establish an end-to-end gain. The same reference APK changed
17.49 → 12.40 idle FPS over the session, and idle/fight changes reverse sign
between the two comparison orders. The completed first control preceded the
sparse/empty-interval follow-up builds; the final three runs followed those
builds. The intermediate after-1 pilot was interrupted and is retained under
`excluded-sparse-pilot/`; its incomplete windows overlapped a test build and
are excluded by that protocol reason, not by their FPS value.

Separate 20-second / 500-Hz captures after the last two runs contain 1,930
reference and 1,830 new CPU samples. `MDLX_GetModelKeytrackValue` self shares
are 9.17% → 9.07%, `MDLX_BindPose` inclusive 18.08% → 18.31%. These do not
establish a representative-scene CPU-time reduction either; the verified
benefit is the bounded search work and synthetic pose benchmark above.
Keep this distinction when assessing whether the optimization is useful on
other hardware or animation data. Full tests pass (35 JUnit reports, 6,873
test executions), plus debug/release WC3 builds, ordinary/shadow renderer
suites and Android signature/alignment checks.

The user then requested a physical Honor comparison. That comparison uses
the **currently installed** APK `5dd1cb2f...` versus `ea6687d7...`, so it tests
all accumulated changes since the tablet's last installation, not this
sampler change alone. Its scripts, original APK/config/map backups,
temperature snapshots and results belong in the `honor/` subdirectory.

The Honor AGM3-AL09HN / Mali-G52 MC2 comparison completed in old/new/new/old
order. Native drawable size was 1920x1200, with 1286x804 3D rendering at 67%
and full-resolution UI. The mixed 200-actor map, particles 2, fixed camera,
20-second idle warmup, three-second movement warmup and 25-second windows
matched between versions. CPU sampling ran only after the last two runs'
FPS windows. No completed physical-device window was excluded:

| Phase | Installed 1 | New 1 | New 2 | Installed 2 | Pooled installed → new FPS |
|---|---:|---:|---:|---:|---|
| Idle | 18.62 | 19.83 | 19.14 | 18.42 | 18.52 → 19.48 (+5.2%) |
| Moving | 12.46 | 16.19 | 15.94 | 15.59 | 14.01 → 16.07 (+14.6%) |
| Fighting | 13.45 | 14.83 | 15.06 | 14.17 | 13.80 → 14.94 (+8.2%) |

Every phase improved in both comparison orders. Moving varied substantially
in the reference runs: its paired gains were +30.0% and +2.2%, so the pooled
+14.6% is a preliminary two-run result, not a stable minimum or universal gain.
Idle's paired gains were +6.5% / +3.9%; fighting's +10.2% / +6.2%. No 40/60-FPS
result is demonstrated. Temperature snapshots show 28.2–32.2 °C **battery**
temperature, not the SoC's thermal state or a proven throttling cause.

Idle/moving geometry was retained: about 515.5K / 523.3K vertices per frame
and 754 / 820 draws in both versions. Fight populations and effects vary:
the reviewed first-pair framebuffers show 26 ticks and 167 / 166 living actors,
real spells, HP loss and casualties. Mean fight vertices are 515.7K / 516.8K;
draws 877 / 862. These are equivalent scenarios rather than identical random
combat workloads. All measured logs are free of shader, JASS-runtime,
unknown-command and GL-invalid errors.

Separate 20-second / 200-Hz user-CPU captures contain 4,043 reference and
4,058 new samples (20.215 / 20.290 CPU seconds across app threads). Selected
inclusive shares are `G_RunFrame` 24.51% → 22.18%, game `parse_segment`
4.20% → 2.27%, `actor_has_skill` 4.08% → 2.37%, and `MDLX_RenderGeoset`
20.36% → 20.53%. `MDLX_GetModelKeytrackValue` self is 5.14% → 5.54% and
`MDLX_BindPose` inclusive 10.49% → 11.36%; higher render throughput and differing
combat populations prevent interpreting these shares as per-frame costs.
In particular this comparison does not isolate an Honor gain from the new
sampler. Model/geoset submission remains a substantial sampled owner.

At that point `MDLX_QueueModel` combined only adjacent compatible
**whole-model** packets, including all their geosets, and rejected models
with emitters, ribbons, lights or authored child models. The particle
restriction is investigated in the next section; material/visibility/UV
comparisons and alpha-layer ordering remain constraints.

The new ordinary APK was retained on Honor. The original config, log and
existing benchmark map were restored with hash checks, `autoexec.cfg` returned
to absent, and saves were untouched. The ordinary launcher was opened with
no benchmark extras. See `honor/restoration.json`. The emulator was reopened
without a window solely for cleanup after the tablet FPS captures: its original
app absence was restored and verified, temporary files removed, and it was
shut down (`restoration.json`).

### MDX batching diagnostics and layered-model precision (October 17)

`r_mdx_stats 1` reports `[MDX_BATCH]` totals about once per second,
separately for solid and alpha scopes: attempted/accepted models, packets,
singletons, instanced actors, largest packet, first rejection and first
flush reason. Divide counts by `scopes` to compare work per world pass;
these are not whole-frame draw calls and exclude culled/empty-phase models.
Normal play leaves diagnostics off. Portrait/shadow scopes must not reset
pending world totals; disabling the diagnostic resets them. Scope/counter
regressions use real packet submission and interleaved portrait/shadow calls.

On the mixed-army Honor scene the original solid path attempted 349 models
per scope, accepted 130 and rejected 218 for particle emitters. Alpha
attempted 208, accepted 66 and rejected 141 for emitters. This includes world
props as well as units. An early pilot emitted no counters because portrait
rendering cleared them; a failing scope test reproduced the issue before
fixing it. That pilot's FPS windows also overlap host test/build work and
are retained but excluded from performance claims.

An experiment allowed particle-bearing geometry to instance while each
actor bound its own full pose and enqueued particles immediately in original
order. It also permitted batching in inactive effect phases. Real-entry
particle tests covered animation positions, emission order, singleton
replay, selection barriers and empty geometry. The final ABBA device series
below did not justify keeping that extension: it was removed completely,
including its replay guard and particle-actor counter. Production again
rejects emitter/ribbon models in every batch phase. The tests now guard
immediate geometry, per-actor emission and unchanged inactive accumulators.
No dormant runtime switch or alternate particle pipeline remains.

Lights, authored child models, selected/hovered actors, nonuniform/sheared
placement, material/UV differences, depth flags and multiple alpha layers
retain their existing restrictions. Model structures, public APIs, network
messages and save records are unchanged. No sorting, animation quantization,
particle-quality reduction or scene reduction is introduced.

The named runtime gaps are Android/GLES cost and final framebuffer output
under the real mixed army and combat. Local before/after source snapshots,
APKs, validation logs, capture scripts and review notes are kept under
`build/android/optimization/mdx-batching/`.

During the first paired series the user zoomed in to inspect idle
Headhunters that appeared to jitter. The idle screenshots show different
camera distances, so the control and interrupted new run are retained under
`honor/excluded-camera-series/` and excluded from FPS comparisons. They do
not establish either a gain or regression. A valid repeat must confirm the
same camera before collecting windows.

The shader investigation exposed a precision-contract gap: the numeric
`RGBA32F` bone sampler had `PRECISION_HIGH` metadata, but the portable shader
module intentionally does not emit per-field precision qualifiers and uses
lowp sampler defaults on ES. The MDX texture-palette variant now supplies
`precision highp sampler2D;` in its ES-only compile prelude, before uniform
declarations in both stages. This keeps the portable module and its other
consumers unchanged. Desktop GLSL and the uniform-palette variant retain
their existing sources. A production shader-generation test failed before
the change and now checks both stages. This is a source-level correctness
fix; it is not yet proof that the reported visible jitter is resolved.

A standalone EGL/GLES diagnostic in the local `precision/` artifact directory
uploads fractional `RGBA32F` values, samples them in a vertex shader and
reads back their float bits through an RGBA8 target, comparing lowp and highp
sampling. It uses the actual portable shader compiler and requires no game
installation. After USB reconnection the test ran on the Honor Mali-G52 MC2: all six
lowp samples differed from the uploaded floats, while all six highp samples
were bit-exact, with no GL errors. Examples are `1.00010002 → 1` and
`123.456703 → 123.4375` under lowp, versus unchanged values under highp.
`precision/honor.log` retains all values. The final ordinary highp APK was
installed and the original files were first restored with hash checks before
reopening the idle army for a separate user-visible Headhunter check.
Pixel/animation verification and comparable FPS remain separate requirements;
this numeric test alone does not prove the visible jitter has disappeared.

The subsequent visual isolation found jitter with both texture and uniform
palettes, and no jitter with `r_mdx_instancing 0`. Thus sampler precision was
a real independent defect, but did not explain the remaining jitter. The
Headhunter's material 2 has an opaque base (`blendMode=0`, texture 2) and a
blended overlay (`blendMode=2`, texture 0), used by two geosets with 94 and 9
vertices. Its solid geometry batches; the two alpha layers exceed the
single-layer ordering limit and render immediately. These material values
were read using the production MDX parser, not inferred from screenshots.

The shared shader projected immediate vertices as `(view * model) * position`
and instanced vertices as `view * (instance * position)`. On Mali, their
different rounding changed depth for coplanar layers. A standalone EGL test
using the actual generated vertex programs and transform feedback found
61,431/61,440 different clip positions, including 23 different quantized
24-bit depths in its synthetic projection. Both paths now compute affine
world position first, restore W to one and share projection, texture and
shadow coordinate expressions. The repeat produced zero differing clip
positions/depths, with no GL errors. Source/ordering regressions cover both
shader variants and a two-geoset mixed solid/alpha fixture. No depth offset,
quality reduction or batching disable is involved.

The user confirmed the corrected idle Headhunters no longer jitter, first
with genuine uniform instancing and then with the default texture palette.
Local evidence is in `clip/` and `honor/visual-clip/`. An earlier diagnostic
process faulted during EGL process teardown after completing readback; the
probe now explicitly releases EGL/GL resources. That failure is not a game
crash. Shader tests, the full suite (35 reports, 6,889 test executions),
debug/release builds of WC3/WoW/SC2, Android signing/alignment and renderer
normal/shadow suites pass (223 tests per renderer suite). Wire and save
contracts are unchanged. These precision fixes remain after retiring the
particle-batching experiment.

The corrected experiment's fresh-process ABBA series used 67% rendering,
particles=2, camera `(4600,-4600,3000)`, 20-second idle warmup, 3-second
movement warmup and 25-second windows per phase. GPU probes and Simpleperf
did not overlap FPS windows. Only the renderer library differed between
control and experimental APK executable entries. Pooled FPS uses total
frames divided by total elapsed time across two captures per variant:

| Phase | Control FPS, runs 1 / 2 | Expanded batching FPS, runs 1 / 2 | Pooled change |
| --- | --- | --- | --- |
| Idle | 19.178 / 19.176 | 18.978 / 19.058 | -0.83% |
| Moving | 16.416 / 16.423 | 16.422 / 16.448 | +0.10% |
| Fighting | 15.181 / 14.862 | 13.907 / 13.537 | -8.66% |

These compare the complete experiment (particle eligibility plus both
precision fixes) with the diagnostic control; they do not isolate any one
change's FPS cost. The fixed camera is confirmed by screenshots; combat
damage, spell populations and casualties vary, so fight windows are not
bit-identical simulations. Both pairs have the same direction of the fight
regression. Fewer calls alone do not establish an improvement: idle draws
fell from about 757 to 299 and moving draws from 820 to 364 without a
corresponding gain. Raw windows, screenshots and frame/elapsed totals are
under `honor/clip-{before,after}-{1,2}/`, with `clip-comparison.json` beside
them. Completed negative windows are retained.

The post-window 20-second, 200-Hz `task-clock:u` experimental CPU profile has
3,917 samples. `glTexSubImage2D` accounts for 8.40% inclusive, `MDLX_BindPose`
9.98% inclusive / 2.50% self, and `MDLX_GetModelKeytrackValue` 5.26% self of
all-app sampled CPU. `SV_Frame` is 27.47% inclusive. Parent and child shares
overlap; these are not wall time or GPU time. Bone texture streaming is a
concrete next diagnostic candidate; it does not justify a default transport
change without a separate comparison. Profiles were recorded after the FPS
windows and are in the corresponding `*-2/report.txt` directories.

A separate one-run uniform-palette pilot used the preserved corrected
prototype APK and the same map, camera, scale and timing. It measured
25.125 FPS idle, 16.536 moving and 16.974 fighting
(`honor/clip-uniform-pilot/`). This is promising for idle/combat, but movement
remained near the control's 16.42 FPS; one pilot does not validate changing
the transport default or meet 40 FPS. The final ordinary build retains the
original effect eligibility and default texture transport. Further work
should compare transport/upload costs across these phases before restoring
the broader eligibility experiment.

The final ordinary APK removes the broader eligibility experiment and retains
both shader corrections. A separate 25-second-per-phase validation measured
19.147 FPS idle, 16.425 moving and 14.979 fighting, with 200 starting actors,
the same fixed camera and 67% scale (`honor/final-ordinary/`,
`final-validation.json`). This is an absolute validation, not a repeated
estimate of the small corrections' performance cost. It returns to the
control's approximate frame rate and does not meet 40 FPS. Final APK SHA-256:
`5d78ae08d19003d379c2f4c5b88fe2f3046da072d5bea643f82e595d6f30711a`.
Original Honor config/log/map files and autoexec absence were restored with
hash checks; saves were untouched. The corrected ordinary APK remains
installed, the diagnostic game is stopped and the cold Android launcher is
shown without benchmark extras (`honor/restoration.json`, `device-state.json`).

Target feasibility from the final mixed-army validation: 40 FPS permits
25 ms/frame and 60 FPS permits 16.67 ms/frame. The measured fighting frame
is 66.759 ms, requiring about 2.67x / 4.01x overall throughput respectively
(about 62.6% / 75.0% less frame time). Moving requires about 2.44x / 3.65x.
These are whole-frame budgets, not gains that can be added across overlapping
CPU profiles. The earlier 43–55 FPS two-model scene shows that the port can
reach those rates in a simpler workload; it does not establish the same
capacity for the mixed army or a campaign mission.

Average 40 FPS remains an ambitious engineering target with unproven
attainability under the current full-quality workload. A sustained 60 FPS
or a strict 40-FPS floor has still less supporting evidence. In final
ordinary captures, the median of the one-second maximum `SV_Frame` wall
durations is about 28 ms moving / 34 ms fighting, with larger outliers.
`OpenRealm_Frame` calls `SV_Frame` then `CL_Frame` serially, so these peaks
also matter for minimum FPS. They include scheduling/wait time and are not
pure simulation CPU cost. The uniform pilot's higher idle/combat FPS but
unchanged movement confirms that a single transport change is insufficient;
CPU/GPU timing and simulation peaks need separate investigation. No measured
hardware ceiling or reliable 40/60-FPS delivery estimate is established.

### Honor resolution and CPU baseline (October 17)

The next investigation uses `CombatBench200.w3x`, 200 starting actors,
particles=2, full animation and camera `(4600,-4600,3000)`. Resolution changes
alone produced 16.463/16.710 FPS moving at 67%, 15.194 FPS at 100%, and
17.724 FPS at 50% in a separate fresh process. The attempted 33% window
actually rendered at 100% (15.056 FPS): `r_render_scale` accepts 50..100 and
resets out-of-range values to 100. Keep that window as a mislabeled control,
not evidence for lower resolution. Startup confirms 50% means 960x600 3D
with 1920x1200 UI. These sequential single windows suggest pixel load is
only part of the bottleneck; they do not rule out GPU vertex/driver cost.
Raw logs and the correction are under `build/android/optimization/mdx-stream/`.

Interpret a flat resolution response as evidence against pixel shading being
the sole dominant cost, not as proof of a purely CPU-bound frame. Scaling keeps
WC3 model/pose work, ordered packet count, mesh vertices and simulation work;
CPU submission/driver cost and GPU vertex work can remain limiting. The scale
path also performs two framebuffer blits (preserve the earlier background,
then upscale the 3D view), so its bandwidth/resolve work can offset part of the
pixel saving. The existing measurements do not isolate either blit's GPU cost.

`renderer/r_main.c:R_BeginScaledView` prints `Render scale: ...` only when it
creates/resizes a scaled target. Returning to 100% destroys that target and
returns **before** the log statement; the last 67% line is not evidence of the
current active scale. A saved config is also not a live framebuffer query.
The code bypasses scaling for active MSAA and small viewports (portraits).
On October 17, a read-only inspection during the user's Undead07 session found
an earlier 67% / 1286x804 target log with native 1920x1200 UI, a saved 100%
setting, and no current `R_STATS`/`R_CPU` stream. That confirms a scaled target
was created in the session, but does not establish current per-scale FPS or
identify that mission's limiting stage. The running mission was left intact.

The fresh movement CPU profile has 9,818 samples: `MDLX_BindPose` 11.00%
inclusive / 2.96% self, `MDLX_GetModelKeytrackValue` 4.59% self,
`glTexSubImage2D` 4.54% inclusive and `SV_Frame` 25.97% inclusive of all-app
sampled user CPU. The fight profile has 9,990 samples and 11.70% inclusive
pose cost. Both use 500 Hz `task-clock:u`, 20 seconds, after all FPS windows
(`sweep/*-report.txt`). Parent/child shares overlap; they are neither GPU
milliseconds nor whole-frame percentages. Recording stderr was not retained,
so these artifacts do not establish zero lost samples.

Honor's prior timer-query rejection still applies. Read-only `perfetto
--query` on its v15.0 service reports ftrace/perf/process and SurfaceFlinger
sources but no `gpu.*` producer; `graphics.gpu.profiler.support` is empty.
Do not present an unavailable hardware-counter stream as GPU timing, or
infer support just from an extension string. See the registered-source
check in [Perfetto GPU tracing](https://perfetto.dev/docs/data-sources/gpu).
`--query --long` is unsupported by this device; plain `--query` works.

### Rejected billboard pose-cache basis key (October 17)

A prototype changed `MDLX_BindPose`'s billboard placement key from a complete
matrix to its upper 3x3. `R_GetNodeGlobalMatrix` reads that placement through
`Quaternion_fromMatrix`, so translation-only actors can share model-local
node globals without approximating animation. A production-entry regression
first failed evaluation counts, then compared cached/uncached animated
hierarchies bit-for-bit across translation, rotation and nonuniform scale.
Normal/shadow and full suites passed. Fewer evaluations did not establish
an FPS gain.

Fresh-process ABBA, 25-second windows per phase, compared the ordinary APK
with the prototype at 67% and the fixed camera:

| Phase | Control FPS, runs 1 / 2 | Basis-key FPS, runs 1 / 2 | Pooled change |
| --- | --- | --- | --- |
| Idle | 19.214 / 18.690 | 19.003 / 18.774 | -0.35% |
| Moving | 16.393 / 15.902 | 16.028 / 16.030 | -0.73% |
| Fighting | 15.427 / 14.667 | 14.489 / 14.500 | -3.67% |

Idle/movement pair directions disagree; there is no confirmed improvement.
Battery temperature stayed near 33.2–33.3 C, which does not establish chip
thermal state. The prototype and its dedicated regression were removed
completely; the pre-existing cache remains. Source copies and negative
windows remain in `mdx-stream/basis-experiment/`, `comparison.json` and
the top-level `before-{1,2}/` / `after-{1,2}/` directories. Protocol and save formats were unchanged.

### Bone palette stream investigation (October 17)

The ordinary texture transport rewrites row zero for every MDX packet.
An opt-in prototype appended full-precision affine palettes at non-overlapping
row offsets. Its first implementation renamed a 2 MiB texture at every
scope, which regressed both idle and movement in same-process ABBA:

| Phase | Row-zero control FPS, runs 1 / 2 | Per-scope rename FPS, runs 1 / 2 |
| --- | --- | --- |
| Idle | 18.653 / 18.893 | 12.704 / 13.958 |
| Moving | 16.485 / 16.435 | 12.473 / 12.254 |

Remaining fight windows/profiles were intentionally cancelled after these
complete comparisons. Artifacts are in `mdx-stream/atlas/paired/`; the
negative windows are retained. This rejects that allocation strategy,
not all possible palette transports.

A second version used six persistent textures (three frames of solid/alpha
scopes), allocated lazily at 128 rows / 2 MiB each. Packets appended within
a texture and switched slots on capacity/scope boundaries. A failing test
proved storage was still recreated every scope; the fixed normal/shadow
suites passed 228 tests. Additional cases covered row-origin bone indices,
capacity wrap, large packets, portrait scopes, restoring original small
storage after disabling the mode, and the unchanged uniform transport.
A nonzero instance tag must be reset to affine W=1 in the CPU matrix passed
to `MDX_RenderModel`, otherwise CPU culling divides the position by that tag.
The GPU instance buffer keeps the tag. This was covered by an actual frustum
fixture, not by bypassing culling.

Persistent storage also regressed: idle controls 18.745/18.926 FPS versus
13.049/14.523 FPS with the stream. Movement/fight were cancelled after the
complete idle ABBA (`atlas/ring-paired/`). Both variants and their test-only
wiring are completely removed from production; copies remain in `atlas/`.
The stream's larger texture/read-write pattern is a candidate explanation,
not a measured driver/GPU root cause. Avoid a large per-packet atlas without
first validating its upload cost on the target Mali driver.

On macOS, wrap physical-device automation in task-scoped `caffeinate -i
python3 <script> ...`. Python's host monotonic clock excludes system sleep,
while Android continues running. Validate each window using the device's
sum of `R_STATS.elapsed_ms` (25 seconds, tolerance for the one-second edges)
and a continuous host wall clock; reject a duration mismatch instead of
silently pooling it. A later fetch experiment exposed one invalid capture:
901 rows / 923.189 device seconds versus 25.173 host-monotonic seconds.
That complete run is excluded and preserved as `fetch/after-1-excluded/`;
an interrupted loading run is `after-2-excluded/`. The source of sleep was
not traced; the temporary assertion prevents idle sleep only during the
retry. `duration-audit.json` finds no other completed window with this
mismatch. This guard does not change game settings or simulation speed.

### Integer bone texel lookup investigation (October 17)

A separate prototype kept the original small texture, row-zero uploads,
particle/ribbon eligibility, highp sampler and common affine projection.
Only modern GLSL bone addressing changed: integer `texelFetch` coordinates
replace normalized float coordinates and `textureLod`. GLSL 120 retained the
original `texture2DLod` path. The width constant is 1024 texels in both the
CPU allocation and shader address. This targeted shader arithmetic without removing texture uploads or
changing palette precision. See the
[Khronos ES 3.0 shader specification](https://registry.khronos.org/OpenGL/specs/es/3.0/GLSL_ES_Specification_3.00.pdf).

A diagnostic exports the real instanced descriptor through the production
GLSL generator, then compiles both versions on Honor. Transform feedback
compares 61,440 animated vertex positions, including palette indices around
row boundaries and near the end of a 1024x32 RGBA32F texture: zero bitwise
clip differences, zero quantized 24-bit depth differences, zero GL errors.
This checks numerical equivalence, not performance or an entire game frame.
`mdx-stream/fetch/{before,after}.vs`, `probe.c`, `honor-proof.log` and the
recorded export compiler arguments preserve the exact diagnostic. Only
`librealm_renderer.so` differs among APK executable entries. Debug/full tests
(35 reports, 6,889 executions), release normal/shadow suites (223 each),
Android signature/alignment and both boundary audits pass.

Repeated 25-second runs at 67%, particles=2 and the same 200-actor scene
produced the following results. The first control has valid device-duration
windows; sleep-corrupted/incomplete attempts between it and the experimental
pair were excluded and retried with `caffeinate`. The retained logical order
is ABBA, but it was not an uninterrupted series. Battery readings span
30–33 C and do not establish chip temperature or clock state.

| Phase | Control FPS, runs 1 / 2 | Integer-fetch FPS, runs 1 / 2 | Pooled change |
| --- | --- | --- | --- |
| Idle | 19.110 / 19.051 | 18.840 / 19.158 | -0.44% |
| Moving | 16.150 / 16.198 | 16.203 / 16.246 | +0.31% |
| Fighting | 15.011 / 15.129 | 15.037 / 14.996 | -0.35% |

There is no compelling whole-frame improvement. The tiny movement difference
alone does not justify changing the shader default. The prototype was removed,
including its generated-source export hooks (those hooks existed only in an
artifact copy of the test harness). All four renderer/animation and test sources
match their start-of-investigation copies byte-for-byte. `fetch/comparison.json`
retains every valid window, pair direction and the interrupted-series note;
`retirement-audit.json` verifies removal. The existing highp sampler and common
affine projection fixes remain. No quality reduction or game/runtime setting
is adopted from these experiments.

A more substantial unmeasured candidate is preparing a complete frame's bone
palette before draw submission and uploading it once, rather than updating
texture storage for each ordered packet. That needs renderer-owned frame/view
invalidation and actor palette lookup, plus culling, material ordering,
portrait/immediate fallback and release tests. Neither the rejected larger
per-packet atlas nor fewer draw calls prove that this design will win. The
40/60-FPS target still requires whole-frame improvements, including simulation
peaks; these experiments have not established an achievable delivery bound.

When restoring source snapshots, refresh their mtimes before incremental
builds. `copy2` retained an older mtime, so the first attempted final Android
build reused an experimental unity object despite restored source. That APK
is marked superseded. Touching restored dependencies and rebuilding produced
an ordinary APK whose every ZIP entry's content, including the renderer,
exactly matches the original control; container SHA differs after repackaging.
Final SHA-256:
`2a256ac5b7c550b389dee02a5e552e93e47fc946e183f3ea692c0cbb6a5d6ced`.
`final-apk-comparison.json` and `stale-build-note.txt` preserve this check.
Do not infer successful retirement from source equality or `make` exit status
alone. Debug/release renderer tests and Android signing/alignment were rerun
after refreshing dependencies. The full final suite also passes (35 reports,
6,889 executions, zero errors/failures).

The verified ordinary APK is installed on Honor. Original config/log/map
contents and autoexec absence were restored against the new investigation's
`original/manifest.json`; saves were untouched. The cold launcher has no
benchmark extras and no native game process (`restoration.json`). The final
production code adopts none of the four prototypes. Existing shader precision
corrections remain. Network and save contracts are unchanged.

### Automatic MDX uniform transport on Android (October 17)

`r_mdx_uniform_auto` defaults to **1 on Android, 0 elsewhere**. At an ordered
multi-actor packet flush, it selects the existing uniform instanced shader only
when every geoset's complete actor palettes fit in `BZ_BONE_PALETTE_MAX` (128
matrices). A zero-size palette or a larger packet retains the existing texture
transport. It does not split, reorder, drop actors, change animation frames,
reduce effects or alter render scale. Each packet restores the scope's forced
uniform setting after drawing, so a small uniform packet cannot change a later
large packet's capacity. `r_mdx_uniform_instancing` remains the explicit forced
mode (default 0), with its original capacity splits.

The change lives entirely in `mdx/r_mdx_geoset.c`; it uses the existing CPU
staging buffer and both existing shaders. There is no new view cache, public
API, network field or persistent state. `r_mdx_stats 1` adds `uniform_packets`
and `palette_uploads` to `MDX_BATCH`: multi-actor uniform draws and legacy
per-packet bone texture uploads respectively. Disable the selection with
`set r_mdx_uniform_auto 0` for comparison.

Production-entry regressions cover 128 versus 129 complete matrices, zero-size
palettes, a later geoset that exceeds capacity, small/large/small packet order
in solid and alpha phases, actual transport counters and bit-exact independent
sparse-node poses with fractional interpolation. The auto-only tests failed
18 assertions before the selection was implemented. GPU transform feedback on
Honor compares the real generated ordinary and uniform shaders: 122,880 vertex
positions, zero clip/depth differences and zero GL errors. The existing shared
shader depth/precision corrections are unchanged. This Mali reports 4,096 vertex
uniform vectors and a 65,536-byte uniform block limit with 16-byte offset
alignment; these are device facts, not portable limits or an implemented UBO path.

The final comparison uses one identical APK, Honor AGM3-AL09HN / Mali-G52 MC2,
67% render scale, particles=2, full animations and camera `(4600,-4600,3000)`.
`compare-fresh.py` starts a fresh native process for each A/C/E/E/C/A arm and
measures 25 seconds each of idle, movement, then **only its first combat**.
A is legacy texture transport; E is the retained automatic selection; C also
prepares a whole-view palette (retired prototype).

| Phase | Legacy A | Adopted E | Change | Retired C |
|---|---:|---:|---:|---:|
| Idle | 19.07 FPS | 19.91 FPS | +4.4% | 19.93 FPS |
| Move | 16.09 FPS | 16.91 FPS | +5.1% | 17.12 FPS |
| Fight | 14.55 FPS | 16.52 FPS | +13.6% | 16.80 FPS |

Each arm has two windows; every window passes transport, elapsed-duration and
particle-workload checks. Combat medians are 612/576 live particles in A,
586/564 in E and 638/622 in C. Spell, random and casualty histories are not
bit-identical, so these are device comparisons, not deterministic GPU timings.
The retained E combat windows are 16.44 and 16.61 FPS versus 14.58 and 14.52 in
A. The 40-FPS target is not reached. Lower CPU stage wall time and higher swap
wait do not measure GPU execution time. No builds or profilers overlap FPS
windows; host sleep and Android elapsed time are checked.

The final ordinary APK was separately exercised without MDX transport overrides:
21.17 FPS idle, 17.13 moving and 16.66 in first combat, with live particles and
uniform/texture counters confirming the Android default. This single run
validates the final build; use the paired table for gain claims. Full tests
pass (35 JUnit reports, 6,897 executions, zero failures/errors/skips), as do
debug/release normal and shadow renderer suites (227 tests each). The signed,
aligned APK SHA-256 is
`0905ada59c79e764365b07c24cf99aaa7e9a25f866cd401d399374ee112f0da1`.
It is installed on Honor. Original config/log/map hashes and autoexec absence
were verified after opening the cold ordinary launcher; no native game process
remains. See `final-default/metadata.json` and `restoration.json`.

The retired whole-view prototype evaluated sparse geometry poses once per
actor, shared one texture between solid and alpha phases, and added up to
3 MiB each of CPU/GPU palette storage plus actor metadata. Its additional
1-2% over automatic packet transport was close to window variation. Its cvar,
cache, budget, lifetime hooks, API and tests were removed completely; original
world/alpha/model-release code was restored with fresh mtimes. Source snapshots,
negative evidence and the separate 61,440-position texture proof are retained
under `source-prototype/`, `texture-only-source/` and `proof/` in the artifact root.

#### Measurement traps and unresolved particle reset

- `Key_SetBinding` stores `UINAME[80]`. Keep diagnostic bind commands below
  80 bytes. The first four-mode run used 83-byte commands, truncating the final
  setting; D did not enable forced uniforms. It was interrupted and retained
  as `four-mode-truncated-bindings`, excluded from mode conclusions. Corrected
  scripts use short cvar commands and validate actual transport counters.
- Same-process `Combat_Fight` resets lose particles after the first fight,
  including after returning to legacy transport. The cause is unresolved.
  First ABBA raw combat FPS was 14.66 to 17.40, but its first control had median
  621 live particles and later controls/enabled windows had zero. The corrected
  four-mode run reproduced this (first control 696, every later combat window
  zero). **Do not use either series to claim a verified full-effects combat
  percentage.** All windows and warnings are retained. Fresh-process runs avoid
  this state and require median live particles above 50 and nonzero particles
  in at least 90% of diagnostic rows. These checks detect an empty workload;
  they do not diagnose or fix the reset failure.
- Force-stop after installation and use `am start -S -W`: Honor's OEM package
  preload can otherwise start the launcher before diagnostic extras arrive.
- Preserve fresh source mtimes after prototype retirement and inspect the
  packaged renderer; stale unity objects previously retained retired code.

Artifacts: `build/android/optimization/mdx-frame-palette/` (including
`fresh-comparison.json`, `fresh/metadata.json`, `review.txt` and restoration records).

### Effect geometry modes and retained experiments (October 18)

`r_mdx_effect_instancing` preserves three complete paths. Android defaults to
**2**, other platforms to **0**:

| Value | Behavior |
|---|---|
| `0` | Original eligibility: emitter/ribbon models remain immediate; existing palette controls apply |
| `1` | Batch eligible effect-model geometry with the existing automatic/explicit palette selection |
| `2` | Same eligibility extension, using complete uniform palettes with capacity splits throughout the world batch scope |

Mode 2 selects uniform transport for the whole batching scope, including
non-effect actors; it is not a per-model transport heuristic. The standalone
`r_mdx_uniform_instancing 1` still forces uniforms in modes 0/1. With that
override at zero, modes 0/1 retain automatic selection controlled by
`r_mdx_uniform_auto`. Changing the effect mode takes effect at the next scope.

Each source actor binds its complete pose and enqueues particles immediately
in the original actor/RNG order. Only geometry is deferred. Packet replay,
including a singleton, suppresses effect advancement so particles are not
duplicated. Active alpha ribbons remain immediate to preserve compositing;
their solid geometry can batch. Empty geometry, selection/attachment/light
barriers, depth flags and multi-layer alpha restrictions retain normal drawing.
The preparation collector still requests complete effect poses, including
emitter-only nodes absent from geometry palettes. No animation rate, particle
quality, model count, shader depth expression or render scale is reduced.

`MDX_BATCH effect_actors` counts accepted source actors with emitters or ribbons,
including eventual singletons; it is not the number of GPU-instanced actors or
emitted particles. Geometry and per-source emitter tests compare positions,
velocities and the subsequent RNG value with mode 0. They cover packet overflow,
singleton replay, empty geometry, selected actors, phase ownership, inactive
emitters, and the mode-2 uniform boundary.

The earlier effect-batching experiment above regressed under its then-current
pipeline. This revisit keeps the later affine/depth correction, automatic
uniform transport and three pose workers fixed. One experimental APK,
SHA-256 `9bfde8728469a595c775bed86cb5ca07db2f92a98b877db67f55bc63fc5e1691`,
compares fresh native processes and only their first combat: Honor Mali-G52 MC2,
200 starting units, 67% scale, particles 2, camera `(4600,-4600,3000)`, approximately
25-second windows, no builds/profilers during measurement. Order: B/Z/Z/B/U/U/V/V.

| Mode in experimental APK | Runs FPS | Total frames / time FPS | Change vs B |
|---|---|---:|---:|
| B: old eligibility, automatic palettes | 17.012 / 17.108 | 17.060 | reference |
| Z: expanded eligibility, automatic palettes | 17.095 / 17.068 | 17.082 | +0.13%, no meaningful gain |
| V: old eligibility, explicit uniform palettes | 17.722 / 17.558 | 17.640 | +3.40% |
| U: expanded eligibility, explicit uniform palettes | 18.208 / 18.234 | 18.221 | +6.81% |

Mode 2 expresses U's combination as a default without requiring diagnostic
overrides. Z remains selectable as mode 1 rather than being discarded. U's
increment over V is about 3.3%; the complete gain cannot be attributed to
geometry batching alone. Median draw counts fall from approximately 866 in B
to 475 in Z and 580 in U: fewer calls alone do not predict better frame time.
Sampled geometry remains approximately 511–512K submitted vertices; particle
medians stay populated (about 608 in B1 versus 670–679 in U), without hiding
effects. Random combat histories are not bit-identical, and these are not GPU
duration measurements. The 40 FPS goal remains unmet.

The **final ordinary APK** is separately checked in B/D/D/B order, with no
effect/palette overrides in D. It confirms the default mode-2 uniform path,
approximately 216–217 accepted effect actors per solid scope and zero bone
texture uploads:

| Final APK | Runs FPS | Total frames / time FPS |
|---|---|---:|
| B: mode 0 control | 17.101 / 17.338 | 17.220 |
| D: Android defaults | 17.935 / 17.971 | 17.953 |

The final comparison is **+4.26% FPS**. Keep it separate from the prototype's
+6.81%; do not pool different APK series or attribute their difference to
unmeasured GPU time or thermal state. Final APK SHA-256:
`674d5c9d613922e48f7243b4a68ccedeccd0274531a288f701a6fc1b7dac33a1`.
Only the native renderer entry differs from the prior pose-worker APK, and
the packaged renderer matches the local Android build byte for byte.

Baseline and final full suites pass. Final output contains 35 JUnit reports,
6,915 test executions and zero failures/errors/skips; normal/shadow renderer
suites pass 236 cases each in debug/release. Two updated emitter cases and
two new cases cover the expanded geometry contract and uniform mode. The
headless particle pool now covers the 257-actor overflow fixture and rejects
test-pool overflow safely. The test import supplies mandatory `CvarString`:
the prior null import caused reproducible failures through both deferred-solid
and alpha world entry points, now fixed in the harness rather than hidden
behind a production callback guard. Android signing/alignment and boundary
audits pass. Shader precision/depth expressions remain unchanged.
The ordinary APK remains installed; original config, engine log, benchmark
map, campaign progress and save hashes match the pre-test backup. Autoexec
returns to absent, trace tags are zero and the native game process is stopped.
The cold ordinary launcher was visually checked (`restoration.json`).

Local raw windows, source snapshots, test/build logs and cleanup inventory:
`build/android/optimization/mdx-effects-batching/`. The final default-build check
is recorded separately from the four-way experimental comparison. Production
changes are private to the WC3 renderer; network/save formats and module APIs
are unchanged.

The owner's retention rule is in [optimization experiment retention](../../performance-experiments.md).
The archive (`$OPENREALM_EXPERIMENTS_DIR/warcraft-3/optimization-archive/README.md`, external archive)
preserves historical source/patches, result summaries and a complete working-tree
context for this trial outside disposable build directories. The initial
cleanup removed 381 old binary/cache files (1,496,809,933 logical bytes), with
about 1.50 GB additional filesystem availability. SDK/NDK, AVD, SDL, game archives,
saves, signing keys, the active control APK, source, screenshots and raw profiles
were preserved. `make clean` removes all of `build/` and is unsuitable here.

### MDX submission bindings and concurrency audit (October 18)

The next experiment removes per-draw mutation of matrix attributes and replaces
one repeatedly overwritten global matrix VBO with model-owned streams. A red
entry-point test reproduced repeated divisor changes before the implementation.
`r_mdx_submit_cache` retains three complete modes:

| Mode | Matrix stream | Instance attributes |
|---|---|---|
| 0 (initial/default) | Original global VBO | Set/reset for every draw |
| 1 | One VBO per batched model | Set/reset for every draw |
| 2 | One VBO per batched model | Separate persistent VAOs per UV channel |

Extra GPU objects are allocated lazily on the owner thread after pose workers
have joined, on the first positive-mode packet. Mode 0 in a fresh process creates
none of them. Both VAO families use one geometry-layout builder, preserving
secondary UVs, skinning, vertex bases and shared indices. GPU caches are mutable
renderer-owned records; authored tracks/geometry stay unchanged. Model streams
and VAOs share the model lifetime, so deletion/reuse of the old global stream
cannot leave a stale reference behind. Fitted terrain copies clear borrowed GPU
metadata. The complete legacy path remains selectable for later comparisons.

Mode 2 removes 21 GL requests per instanced geoset-layer draw (one array-buffer
bind, four enables, four pointers, four divisor sets and eight reset/disable
calls). It does not remove draw calls, change palette capacity, reorder alpha,
advance particles on workers, or quantize animation. `MDX_BATCH` now reports
`stream_packets`, `vao_draws` and `attrib_sets` so captures prove actual use.
The GLES state contract is specified in
[Khronos OpenGL ES 3.0.6, sections 2.10.2 and 2.11](https://registry.khronos.org/OpenGL/specs/es/3.0/es_spec_3.0.pdf):
VAOs retain vertex bindings and `BufferData` replaces a buffer object's store.
This permits stable bindings while packet data changes; it does not promise a
driver speedup. See [memory/lifetime costs](memory.md#optional-mdx-submission-streams-october-18-2026).

The same test APK (`a428614199bb9818118268d988a36aac975a9033fdd3be3e3fce03b99fab0512`)
was measured on Honor AGM3-AL09HN/Mali-G52 MC2, 67% rendering, full particles,
fixed mixed-200-unit camera, fresh process/first combat and 25-second windows.
Order was 0/2/2/0/1/1, with no overlapping host builds/profilers:

| Mode | Separate windows, FPS | Weighted FPS | Change from mode 0 |
|---|---|---|---|
| 0 | 18.400 / 18.342 | 18.371 | control |
| 1 | 18.783 / 18.872 | 18.828 | +2.49% |
| 2 | 18.775 / 18.686 | 18.731 | +1.96% |

Mode 2 removes approximately 3942 GL requests per world view according to
the actual binding counters. It provides no demonstrated additional FPS gain
over mode 1; the two positive modes differ by only about 0.51%. Do not select
the best window or claim a large renderer speedup. Mode 1's two windows were
collected after the interleaved 0/2 comparison, so order/workload variation
limits that comparison. Combat begins with 200 live units and ends with
161–166; vertices stay around 510–512K and particle populations remain present.
These are variable combat scenes, not deterministic command replays.

A separate 20-second mode-2 CPU profile records 10,658 samples with zero lost.
Mali driver leaf share is 25.58% of main userspace CPU
(previous profile 26.68%); driver work called from MDX draws
still accounts for about 9.52% of main userspace CPU, and uniform uploads about
2.41%. The main client/server CPU partition remains 66.83%/32.66%. Inclusive
profiles overlap and these are not GPU durations. Removing state changes leaves
substantial draw submission work; the 40-FPS goal remains unmet.

All three modes stay available, with default **0 on every platform** while the
small gain and first-use/memory costs need broader validation. Use
`set r_mdx_submit_cache 1` or `2` for a later comparison. Existing optimized
animation, fog and effect batching defaults are unchanged. The preceding
accepted APK is restored to Honor after this experiment, without making a
normal/distribution build. Settings and saves are hash-verified.

Full suite: **6,931 executions, zero failures/errors/skips**. Normal/shadow
renderer suites each pass **239 cases** in debug and release. Only the renderer
native library changes; network/save formats do not. Source/patches, all six
windows, screenshots, raw profile, tests and recipes are retained under
`build/android/optimization/mdx-submit-20261018/` and the
durable submission archive (`$OPENREALM_EXPERIMENTS_DIR/warcraft-3/optimization-archive/mdx-submit-20261018/README.md`, external archive).


The GL-state fixture exercises actual model upload/release with two geosets,
multiple UV channels, equal geometry bindings and independent divisors. It
asserts VAOs are deleted before their buffers and repeats after reusing driver
IDs. Packet tests alternate A/B/A models, then ordinary and batched rendering,
in all three modes and both solid/alpha and texture/uniform palette transports.
The existing exact-depth/jitter shader fixes remain intact; static screenshots
cannot establish absence of temporal jitter on every unit model.

#### What can be moved to workers

The current engine frame calls `SV_Frame` and `CL_Frame` sequentially in
`common/main.c`. Moving just the graphics API calls into arbitrary jobs would
not separate their state. The useful boundaries and required ownership are:

| Work | Current state | Safe boundary to establish |
|---|---|---|
| MDX poses | Already use private contexts and a joined SDL worker pool | Keep immutable tracks, frozen clocks and disjoint output ranges |
| Packet palette/material preparation | `model_batch`, `instance_bones` and `mdlx.shader->state` are shared mutable scratch | Ordered packet records with private output buffers; owner submits completed packets |
| Renderer backend/frame overlap | `R_RenderFrameNative` shallow-copies `viewDef_t`; entity/decal/splat arrays remain borrowed | Frame-owned copies/arenas, pinned resources and a bounded queue, with one context owner |
| Whole client/server overlap | Loopback buffers grow/free storage with ordinary read/write counters; cvar setters free returned string storage | Synchronized queue ownership and frozen/copied settings, plus map/load barriers |

`SCR_DrawScreenField` interleaves BeginFrame, world rendering, layout/UI, console
and EndFrame; registration and fog updates also enter the renderer through
`re.*`. A backend thread therefore needs the complete command/lifetime boundary,
not only asynchronous `RenderFrame`. Synchronous trace/text/attachment queries
need CPU-side answers or explicit synchronization. Model release, map replacement,
Android surface loss/backgrounding and shutdown must drain the queue first.

Server imports include direct client calls (`MenuAction`, `CL_QueueMovie`,
`CL_LoadingFrame`) and shared FS/cvar services. These must be isolated before
server/client concurrency; the universal client/game boundary still applies.
Both gameplay damage and renderer particle emission currently call libc `rand()`
(`m_unit.c`, `r_mdx_particles.c`), so gameplay and presentation RNG ownership also
needs separation before overlapping them. A coarse lock around an entire frame
would preserve serialization and provide no frame-overlap gain.

These findings support two larger experiments: private CPU packet preparation,
and a full renderer command boundary. Larger palette packets through uniform
blocks could reduce the remaining draw count, but need queried hardware limits
and Mali measurements; the earlier texture-palette result is not evidence that
uniform blocks will be faster. These larger changes were outside the binding
trial. The subsequent CPU packet trial below implements private preparation;
the backend thread remains separate work. The later uniform-block trial below addresses palette packet capacity.

### Parallel draw-packet preparation (October 18)

`r_mdx_packet_prepare` preserves three complete paths, initially defaulting to 0:

| Mode | Material/palette preparation | Pose evaluation |
|---|---|---|
| 0 | Original on-demand owner path | Existing pose workers |
| 1 | Private packets filled by the owner after pose workers join | Existing pose workers |
| 2 | Private packets filled in the same jobs as their poses | Existing pose workers |

Mode 1 compares preparing/reusing values on the owner with preparing them in
the pose jobs. Its post-join pass also restores each cached pose into owner
scratch; mode 2 consumes the job's already resident matrices, so the difference
includes that copy/locality effect. There is no second thread pool, per-draw dispatch or
additional synchronization barrier. Each job receives an owner-reserved packet
and writes only that packet and its existing pose range. Workers sample geoset
alpha/RGB, layer/material alpha, flipbook texture slots and UV transforms, expand
geoset-local uniform palettes and transpose compact texture-palette rows.
The owner consumes packets only after `MDLX_EndPosePreparation` joins the pool.

`MDLX_PrepareDrawPacket` calls raw samplers. It never reads/writes `mdlx.shader`,
the active owner draw pointer, `model_batch`, `instance_bones`, GL state,
emitter accumulators or RNG. Authored tracks and the render clock are immutable
through the joined job scope; material global-sequence samples therefore see
the same nonzero `tr.viewDef.time` as pose evaluation. Packet compatibility
compares prepared values and retains original source order and flush reasons.
Uniform draws copy ready per-geoset matrices directly instead of transposing
them to rows and reconstructing them on the owner. Texture draws copy the
already transposed rows into the existing ordered upload. Unprepared actors
retain original preparation; singletons/ordinary draws retain their original
shader/VAO path and may copy a ready palette.

The optional arena has a 4 MiB ceiling and stable pointers for one pose epoch.
Reservations and model/material/array metadata belong to the owner. No arena
allocation or GL resource creation occurs on workers. Overflow, fewer than 16
jobs, disabled workers and deliberately incomplete poses keep on-demand work;
they do not remove geometry. Epoch invalidation cancels queued work and resets
the arena cursor before reuse. Model-batch shutdown clears pose references
before freeing the arena. `MDLX_PreparedDraw` checks the epoch and exact frames,
using the actor basis for billboard poses. Fitted terrain models retain the
existing on-demand policy. See [memory costs](memory.md#optional-cpu-draw-packets-october-18-2026).

`MDX_POSE` adds `packets`, `worker_packets`, `packet_hits` and `packet_misses`
per view. Positive packet counts prove that this path ran; `worker_packets`
must be zero in mode 1 and positive in mode 2. A cvar setting alone is not
evidence of parallel work. Keep `r_mdx_submit_cache 0` when comparing this
trial so the previous binding experiment is independent.

The entry-point regression fails before implementation because material
samples do not run on workers and drawing repeats them. Four new tests cover
worker sampling with no shader/GL/effect mutations, byte-identical uniform and
texture draw outputs, sparse/missing palette indices, billboards, animated
alpha/RGB/UV/flipbook values, both phases, mode switching, partial-pose expansion,
small/overflow batches, stale clocks/view/lerp/release and particle/RNG ownership.
Full suite: 6,939 executions, zero failures/errors/skips; renderer normal/shadow
debug/release suites each pass 243 cases.

ThreadSanitizer and AddressSanitizer binaries compile locally, but the former
segfaults before the test banner and the latter hangs there, including outside
the sandbox. These are recorded verification gaps, not sanitizer passes.
Ordinary/release tests and the ownership/output checks pass. Local macOS test
commands need `CPATH="$PWD/build/android/sdl-host/include"` and
`LIBRARY_PATH="$PWD/build/android/sdl-host/lib"`; unrestricted UDP tests also
need execution outside the filesystem/network sandbox. A passing isolated
baseline was captured before production edits. An earlier overlapping baseline
attempt is historical diagnostic evidence, not the accepted baseline.

The same test APK `c8391b23763ac5a3f572846dfe9cfc42b857440e0a3142e4f20d13558720e91c`
was measured on Honor AGM3-AL09HN/Mali-G52 MC2 at 67% render, full particles,
fixed mixed-200-starting-unit camera, fresh process/first combat and 25-second
windows. Order 0/2/1/1/2/0 balances the endpoints; no host build or profiler
overlaps the FPS windows:

| Mode | Separate windows, FPS | Weighted FPS | Change from mode 0 |
|---|---|---|---|
| 0 | 18.576 / 18.727 | 18.653 | control |
| 1 | 19.039 / 19.191 | 19.115 | +2.47% |
| 2 | 19.076 / 18.858 | 18.967 | +1.68% |

Mode 2 is about 0.77% below mode 1: this series does not establish an FPS
advantage from parallel packet preparation. The combined preparation stage
drops from 6.471 ms in mode 1 to 5.531 ms in mode 2, but mode 0 costs only
4.824 ms there. Reuse removes work later during drawing; the extra preparation
and lookup also have costs. Mode 2 prepares about 293 packets/view, 203 on
workers, with 724 prepared draw hits/view. These counters prove work moved;
they do not establish a large whole-frame gain. End counts are 161–167 units
at 29–30 combat ticks, so small FPS differences remain workload-dependent.
All three modes were kept at default 0 for this comparison. Subsequently, the
owner requested enabling the optimization in the installed Android build:
Android now defaults to **2**, other platforms to **0**. This changes the chosen
default, not the measurements above; `set r_mdx_packet_prepare 0` restores the
original preparation. The 40-FPS goal is unmet, and no first-TFT-mission result
is inferred from this map.

The separate mode-2 profile has 10,802 samples, zero lost, task-clock:u at
500 Hz for 20 seconds. `MDLX_PrepareDrawPacket` accounts for 0.60% of main
userspace CPU and 9.03% of worker userspace CPU (different denominators).
Main `MDLX_BindPose` is 2.79%; substantial work still remains in the Mali driver
(26.64% leaf share), MDX draw caller sites (9.78%), uniform uploads (2.44%)
and server execution (33.85% inclusive main CPU). Inclusive shares overlap;
these are not GPU durations or a same-APK on/off CPU-profile comparison.
Packet preparation is now movable, but its remaining owner cost is small
relative to those stages. The previous accepted APK and original Honor
settings/map/save hashes were restored after the test/profile runs.

The later requested Android build enables mode 2 without a startup test-map or
credits override. The independent submission experiment remains at its existing
default; enabling CPU packet preparation does not select its VAO/stream modes.

Source, tests and recipes are in
`build/android/optimization/mdx-packets-20261018/`; final retention is indexed by
the draw-packet archive (`$OPENREALM_EXPERIMENTS_DIR/warcraft-3/optimization-archive/mdx-packets-20261018/README.md`, external archive).
Only the WC3 renderer native library changes in its test APK. There are no
`re.*`, game/client, network-message or save-layout changes, and no ordinary
distribution build is required.

### Larger uniform-block palette packets (October 18)

`r_mdx_palette_ubo` is a WC3 renderer-only option. It initially defaulted to 0
during the comparison; following the owner's instruction to enable beneficial
optimizations, Android now defaults to **2**, other platforms to **0**. It keeps
all previous instancing and CPU packet switches independent:

| Mode | Bone transport | Ordered packet palette limit |
|---|---|---|
| 0 | Existing uniform/texture paths | Existing 128-matrix uniform limit |
| 1 | Uniform block | 128 matrices, transport control |
| 2 | Same uniform block | Queried block capacity, at most 1,024 matrices |

The renderer queries `GL_MAX_UNIFORM_BLOCK_SIZE`, vertex block support and binding
slots once per context. Both block modes use the same compiled block and full
allocation size; only packet capacity differs. The limit controls the number of
complete actors in a packet, never the bones or indices of an individual geoset.
The ordinary MDX/M2/M3 128-matrix contract and shared shader library stay intact.
GLSL 120 or insufficient capabilities explicitly report why they retain the
original path. Shader/link/layout failures remain fatal.

The private shader removes the default `u_bones` descriptor and prepends a
`std140, column_major` block to the unchanged shared vertex body. Independent
actor palettes are copied into that block, using ready CPU packets when present;
on-demand poses retain the existing exact matrix reconstruction. Placement,
projection/depth math, animation precision and adjacent source order are
unchanged. Model/material/team/tint/flags and unsafe-alpha boundaries still
split packets. The instance stream retains its 256-actor/16-KiB ceiling.
Each geoset orphans a full declared block and uploads only its active prefix;
this follows the [Khronos block binding/layout contract](https://registry.khronos.org/OpenGL/extensions/ARB/ARB_uniform_buffer_object.txt), which requires the bound range to cover the linked block. The owner
unbinds the block after a packet and frees its CPU/GPU/program resources before
context release. See [memory costs](memory.md#optional-larger-palette-blocks-october-18-2026).

`MDX_BATCH` adds `block_packets`, `block_uploads`, active `block_bytes` and
`bone_limit`. `bone_limit` is a maximum, not a sum to divide by scope count.
Capacity flushes and actual draw counts establish whether larger packets ran;
a cvar value alone does not. Four regressions cover queried 16/64-KiB capacities,
independent sparse/missing matrices past index 128, team/source order,
singletons, mode reversal, worker-prepared billboard palettes, resource release
and explicit capability refusal. Full `make test` passes 6,947 executions in 35
reports, with zero failures/errors/skips. Normal/shadow renderer suites pass
247 cases each in debug/release and GLSL 120, whose requested UBO modes exercise
the explicit original-path contract. The new positive tests fail against the
pre-trial source; the insufficient-capability control passes both by design.

A standalone EGL executable includes the production private shader and palette
upload, uses the real descriptor generator and compares complete RGBA buffers
against three ordinary model draws. Honor Mali-G52 MC2 reports a 65,536-byte
block: 1,024 matrices. Both frames, including a changed independent high-index
bone, match byte for byte at indices 64/129/194, with nonempty triangle output
and no GL error. This synthetic readback verifies transport and positioning;
it does not establish every retail model's visual correctness.

The same APK `aa0c9bfbf52ad9ba69537dd26b977c4dd4ef0cc45ed66e68ae09c9f63ed86e89`
was measured at 67% render/full particles, fixed camera `(4600,-4600,3000)`,
fresh processes and first combat only, six 25-second windows ordered 0/2/1/1/2/0.
CPU packet preparation remains mode 2 in every window; submission cache remains
0. No host builds or profilers overlap the measurements.

| Mode | Separate windows, FPS | Weighted FPS | Change from original |
|---|---|---|---|
| 0, original | 18.440 / 18.723 | 18.585 | control |
| 1, 128-matrix UBO | 19.013 / 18.712 | 18.865 | +1.51% |
| 2, 1,024-matrix UBO | 19.489 / 19.182 | 19.336 | +4.04% |

The larger cap adds about 2.49% over the UBO transport control. Mean per-window
median draw calls fall from 563.2 to 486.3 (-13.65%); mean solid packets/view
fall from 108.6 to 81.9. Capacity-reason flushes fall from 41.3/view to zero.
Flush reasons classify the first applicable condition, so those 41.3 do not all
represent avoidable draws: an adjacent model/material boundary may also apply.
Remaining solid model/flags/material splits are about 38.8/18.2/14.7 per view.
The mean renderer-wall median is 38.727 versus 37.388 ms. Geometry medians stay
around 510,000 vertices; all windows start with 200 units and end with 162–167
at 29–30 fight ticks. Combat RNG is not fixed, so this is a modest measured gain,
not a statistical guarantee. The 40-FPS target is unmet. No first-mission or
100%-render result is inferred from this benchmark.

All modes remain available; `set r_mdx_palette_ubo 0` restores the original
transport on Android. Increasing palette capacity alone
has removed this limit in the benchmark, and the remaining state/order splits
need separate evidence. After the comparison, the previously requested working
APK `4d600aaa5c94aa6f9ad4d1f641edb3c77022f58671b9653efb6afc8207391937`
and original config/autoexec/log/campaign/map/save hashes were restored and
verified; the ordinary launcher was open without a benchmark process.

The subsequent default activation uses the existing +4.04% Honor comparison
and exact readback, not a new FPS claim. It changes only the Android default;
mode 1 remains the narrow UBO control and other platforms retain mode 0.
At this source-default activation step, device gameplay was left running; no
APK was rebuilt or installed. The next requested/test-required Android package
would use mode 2 unless user config/autoexec explicitly selected another value.
Baseline and final full tests each pass 6,947 executions; normal/shadow renderer
suites each pass 247 cases in debug/release. The Android renderer native target
builds successfully without packaging an APK. Compiler preprocessing confirms
the actual cvar call defaults to 2 on Android and 0 on the host. Existing mode
regressions are reused; this activation changes no algorithm or quality setting.
Activation source and checks are retained in the
activation archive (`$OPENREALM_EXPERIMENTS_DIR/warcraft-3/optimization-archive/mdx-wide-batches-enabled-20261018/README.md`, external archive).

Performance comparison and final restoration are recorded with the experiment
under `build/android/optimization/mdx-wide-batches-20261018/` and its durable
archive (`$OPENREALM_EXPERIMENTS_DIR/warcraft-3/optimization-archive/mdx-wide-batches-20261018/README.md`, external archive).
There are no game/client API, network-message, simulation or save-layout changes.
Only the WC3 renderer native library differs in the test APK; no ordinary
packaging pass is required.

### Ability, visibility and splat CPU work (October 18)

Following the larger-packet profile, three independent switches preserve the
original paths: `wc3_ability_cache`, `wc3_visibility_fast` and `r_splat_cache`.
All initially defaulted to 0 for controlled same-APK measurement. Following the
Honor comparison, Android defaults to ability cache 1 and visibility fast 1;
other platforms retain 0, and the splat cache stays 0 everywhere. These changes
do not alter MDX packet compatibility, GL submission, shaders, draw ordering,
animation quality, visibility rules, simulation rate, network or save layouts.

`wc3_ability_cache 1` enables game-owned decoded native ability lists, rawcode
alias results and a four-character registry index. Runtime added/removed arrays
are still checked on every membership query, and runtime rank entries remain
authoritative for Heroes and ordinary units. `G_HeroSkillLevel` resolves an
alias only after finding a nonzero rank. Long engine command names retain their
string lookup; the index preserves the first registered concrete rawcode before
following an AbilityData alias.

Decoded lists use 512 bounded pointer-indexed slots with separately allocated
rawcodes and a source-text snapshot. The content comparison detects in-place
changes; pointer/text rebinding naturally misses. All valid tokens are retained,
including quoted tokens and long lists; there is no fixed ability-count cutoff.
Replacing a slot frees it; `InitAbilities` and `ShutdownUnitData` clear all
slots. `S_UnitAbilityCodes` returns a view valid until another list lookup
replaces that slot or the cache is cleared. Current consumers never retain it
across a lookup of another list. This lazy cache belongs to the game owner
thread; it is not an API for worker-side mutation. The 2,048-entry rawcode alias
cache uses `G_AbilityDataGeneration`, so map overrides, table replacement and
archive reloads invalidate results. Options refresh at initialization and the
real `G_RunFrame` boundary, rather than adding cvar lookups to every query.

A cold-cache enabled-suite run exposed a nested-parser hazard: filling a list
through the old `parse_segment` scratch overwrote an outer consumer's current
token, breaking the Wisp `autoharvestgold` order. The regression is
`wc3_ability_dispatch.query_cache_keeps_outer_parser_token_on_cold_decode`.
Cache construction now uses `parse_segment_into` with caller-owned storage;
the original parser delegates to the same bounded implementation while keeping
its existing scratch-based API. The list lexer tests cover nested buffers,
quoted/unquoted truncation, sentinel preservation and complete source advance.
The gameplay order test also passes with the cache enabled. Other original
scratch-based parser callers remain owner-thread-only.

`wc3_visibility_fast 1` adds a zero-source test to
`G_UnitIsForcedVisibleToPlayer`: fixed-size `memcpy` word loads OR the existing
16 saved reference counts, allowing the compiler to combine loads without
alignment/aliasing assumptions. An all-zero array returns immediately; nonzero
counts retain the original owner/shared-vision walk. There is no derived mask
to invalidate, no entity field, and no new save state. Add/remove, saturation,
live alliance edits and restored counts keep their original behavior. The option
refreshes in `G_FowUpdate`, including its grid-not-ready entry path.

`r_splat_cache 1` retains exact clipped geometry per terrain tile and rectangle
in 512 fixed slots. Cached vertices use normalized UVs; current tint and atlas
coordinates are applied while emitting into the existing contiguous-texture
stream. Texture/shader/source ordering and upload boundaries are preserved.
Keys include the world, tile, exact minimum and width/height. Changed rectangles
miss; UV/color changes reuse geometry. `R_UpdateGroundSegment` invalidates the
generation for live deformation, and `R_ResetGroundTextures` invalidates map
release/replacement. Generation wrap clears the slots. Regressions compare
complete uploaded vertex records against mode 0, including changing atlas/color,
clipped slopes, live ground height changes and rectangle movement. Test height
changes must retain tile ownership: changing the W3E cliff level can hide the
ground rather than deform it. The cache reserves 680 KiB with the current
44-byte `vertex_t`, with no new GPU resource. Moving footprints can miss often;
their performance needs measurement independently of stationary decals.

The test APK `338c5e3d6cc27e4cbbb24409ed8fd2d6381b37d923863a2afee54a027585f919`
reuses the previous APK's Java/assets and all other libraries; only game and
renderer libraries are replaced. Its three new options compile with default 0;
explicit settings select every measured mode. Android's subsequent source
default activation needs no second package: the same tested APK uses saved
settings. Existing pose workers/CPU packets/effects/exact sight cache and UBO
mode 2 remain enabled; MDX submission cache remains 0. No driver/submission
optimization accompanies this trial.

Honor AGM3-AL09HN / Mali-G52 MC2 uses the same fixed camera, 67% render, full
particles/animation and mixed 200-starting-unit first combat. All windows are
25 seconds in fresh processes with a 6,000-frame limit. Ten component windows
run B/W/A/S/V/V/S/A/W/B; no builds or profilers overlap FPS measurements.

| Ability / splat / visibility | Individual windows, FPS | Weighted FPS | Versus B |
|---|---|---:|---:|
| 0 / 0 / 0, B | 19.322 / 19.982 | 19.652 | control |
| 1 / 0 / 0, A | 20.305 / 20.370 | 20.337 | +3.49% |
| 0 / 1 / 0, S | 19.042 / 19.357 | 19.200 | -2.30% |
| 0 / 0 / 1, V | 19.675 / 19.536 | 19.606 | -0.23% |
| 1 / 1 / 1, W | 20.107 / 20.149 | 20.127 | +2.42% |

Splat caching has no demonstrated FPS benefit here and stays disabled on every
platform. Its complete toggle, code, correctness tests and negative measurements
are retained for future workloads. Visibility alone is within scene/run noise;
the component series does not establish an independent FPS gain from it.

A separate same-APK B/F/F/B series excludes splat caching and measures the
accepted ability+visibility combination:

| Mode | Individual windows, FPS | Weighted FPS | Mean frame |
|---|---|---:|---:|
| B, 0 / 0 / 0 | 19.089 / 19.454 | 19.272 | 51.888 ms |
| F, 1 / 0 / 1 | 20.109 / 20.495 | 20.302 | 49.257 ms |

This series gives **+5.34%**, with server/client wall-stage means of
13.943/37.955 ms versus 11.750/37.491 ms. Wall stages include waits, so they
are not active CPU budgets. The two series have different controls; do not
subtract their percentages to claim visibility synergy. Combat/RNG and CPU
frequency remain variable, not a deterministic replay or statistical guarantee.
The 40-FPS goal is unmet; no first-mission or 100%-render FPS result is inferred.

Two separate 20-second, 500-Hz DWARF userspace profiles of B and F record
10,611/10,615 samples, both without loss. Main-thread userspace CPU is
15.642/15.420 seconds; pose workers total 3.366/3.576 seconds.

| Weighted main CPU | B | F |
|---|---:|---:|
| Server partition | 34.77% | 31.83% |
| Ability/list family, internally de-duplicated | 6.46% | 3.68% |
| `G_RunEntities`, inclusive | 20.51% | 18.08% |
| Fog/visibility family, internally de-duplicated | 7.13% | 7.00% |
| `G_FowPlayerCanSeeEntity`, inclusive | 1.85% | 1.62% |
| Terrain splats, inclusive | 6.83% | 7.02% |

Inclusive families overlap, and profiled FPS is excluded from the comparison.
The list family includes the new `parse_segment_into` name as well as the old
reader/helper names. `S_UnitAbilityCodes` appears in F (1.40% inclusive), proving
the prepared list path ran. A larger relative renderer/driver share after
reducing server work is not evidence of a driver regression or GPU duration.

A separate bounded ARM64 microbenchmark includes the production forced-visibility
query, uses a volatile call pointer, 200 synthetic entities and thread CPU time,
and runs two million calls per sample in repeated 0/1/1/0 order. Mean query cost
is 46.92 → 7.25 ns for all-zero counts and 33.80 → 8.26 ns when 5% of units have
an own-player forced reveal. The first zero-control run is slower than later
controls; all raw samples are retained. With every unit forced visible to its
own player, the added zero test costs 7.79 → 11.03 ns. These are query timings,
not frame FPS. The common-case CPU result and paired F comparison support the
Android ability+visibility defaults; the original query remains selectable for
other mixes. Other platforms keep both defaults at 0.

Baseline full tests pass 6,947 executions; final full tests pass **6,964** in 35
reports, zero failures/errors/skips. Release RoC/TFT engine tests with queries
enabled pass 2,654 cases each; normal/shadow release renderer tests pass 249 each.
Native Android compilation and preprocessing confirm actual defaults 1/1 on
Android and 0/0 without `__ANDROID__`. No save format or entity layout changes
are needed. The existing tested APK stays installed with ability/visibility
set to 1 in the original config; all other backed-up files and five save/scope/
checkpoint hashes are verified. Test startup bindings/map/profiling settings are
removed, with the ordinary launcher open and no native game process.

Source before/after, the exact default-0 measured source, reconstruction context,
all fourteen FPS windows, CPU samples/symbols, query microbenchmark, tests and
deployment checks are retained in the
CPU-work archive (`$OPENREALM_EXPERIMENTS_DIR/warcraft-3/optimization-archive/cpu-work-families-20261018/README.md`, external archive).

### Honor profile after larger packets (October 18)

This investigation reuses the measured `aa0c9bfb…` APK above, with explicit
`r_mdx_palette_ubo 2`, CPU packet mode 2, three pose workers, effect mode 2,
exact sight cache 1 and submission cache 0. No new APK or engine changes are
needed. Honor AGM3-AL09HN / Android 12 / Mali-G52 MC2 uses 67% render, full
particles/animation and camera `(4600,-4600,3000)`. Each capture starts a fresh
native process and the first combat with 200 mixed units; reusing a second fight
would change the particle workload. The owner authorized interrupting gameplay.

One 25-second unprofiled window gives **20.397 FPS / 49.027 ms**. The 522 recorded
loop intervals average 13.300 ms server and 35.678 ms client, including waits.
This is a new baseline, not evidence of improvement over the earlier 19.336-FPS
comparison: combat, scheduling and CPU frequencies vary. Separate 20-second
`task-clock:u`, 500-Hz DWARF captures record 10,318 and 10,602 samples, both with
zero loss. Percentages below use weighted **main-thread userspace CPU**, not
wall time, all-thread CPU or GPU time. Inclusive rows/families overlap.

| Main-thread work | CPU profile 1 | CPU profile 2 |
|---|---:|---:|
| Client / server partition | 64.50% / 35.10% | 64.91% / 34.54% |
| Mali driver, exclusive leaf | 23.02% | 24.07% |
| `G_RunEntities`, inclusive | 20.45% | 20.37% |
| Ability membership/rank/alias/list family, internally de-duplicated | 6.38% | 6.72% |
| Fog update + visibility family, internally de-duplicated | 7.33% | 6.84% |
| `R_RenderRectSplatUV`, inclusive | 6.63% | 6.32% |
| `SV_SendClientMessages`, inclusive | 5.58% | 5.69% |
| `MDLX_EndPosePreparation`, inclusive | 5.27% | 5.74% |
| `MDLX_PrepareDrawPacket`, inclusive | 0.72% | 0.65% |

The main thread consumes 15.334 / 15.632 sampled CPU seconds; pose workers
consume another 3.200 / 3.410 seconds across all workers. Packet preparation is
8.62–10.06% of **worker** CPU, a different denominator. More parallel palette
preparation targets little remaining main-thread work. Some pose evaluation
still runs on the owner; its 5.00–5.68% inclusive share needs eligibility/job
coverage evidence before changing scheduling.

A separate 20-second scheduler/EGL trace retains all 712,097 events. The main
thread is running 78.25%, runnable but waiting for CPU 3.71%, and blocked 18.02%.
Across 378 complete frame intervals it averages 41.21 ms running, 1.96 ms runnable
and 9.53 ms blocked. There are 379 EGL swaps; interruptible blocking **inside**
`eglSwapBuffers` averages 8.99 ms/swap. These waits are material, but do not
identify GPU execution, compositor/vsync waits or a particular fence. CPU remains
a substantial limit; removing the observed waits alone still leaves about
41 ms scheduled work in this trace, beyond the 25-ms budget for 40 FPS.
Scheduler slices include kernel/interrupt work. Trace mean/p95 frame intervals
are 52.70/81.70 ms and must not replace the unprofiled FPS baseline.

The focused next trials are:

1. **Predecode ability metadata and aliases in `game.dll`.** `actor_has_skill`
   in `skills/s_harvest_lumber.c` reparses `UnitAbilities->abilList` for every
   query, after checking runtime added/removed arrays. Its consumers include
   cargo, minimap, projectiles and Shadow Meld. `parse_segment` accounts for
   2.48–3.10% inclusive main CPU; much comes through this membership helper.
   `G_HeroSkillLevel` also resolves the requested alias before finding whether
   any runtime rank exists. Cache immutable row/registry results and avoid empty
   rank work while preserving runtime add/remove, hero ranks, morphs, map/object
   overrides and load invalidation. The game-owned `parse_segment` uses a static
   1,024-byte buffer: moving these calls to workers directly is unsafe.
2. **Reduce compatible MDX packet splits and driver submissions.** Ordinary and
   instanced MDX draw call sites together account for 7.60–7.97% main CPU inside
   Mali. The baseline has 85.14 solid packets/view; first-reason model/flags/
   material flushes average 38.82/19.59/16.14, with **zero capacity flushes**.
   Increasing the palette cap again has no demonstrated opening here.
   `MDLX_QueueModel` compares the entire entity flags word; audit a geometry
   compatibility key before attempting per-instance material parameters.
   Preserve lighting, fog, carry-frame remapping, emitter ownership and source
   order. A flags flush can also conceal a later material/team mismatch, so
   19.59 is not a promised number of removable draws. Do not infer private Mali
   function meanings from nearest exported symbols in a stripped driver.
3. **Reuse terrain-decal topology when its footprint is unchanged.**
   `R_RenderRectSplatUV` includes tile generation, polygon clipping, contiguous
   texture batches and driver calls. Separate stable geometry from changing
   color/UV/lifetime values; retain cliff/terrain height interpolation, depth
   math and transparent order. Its 6.32–6.63% inclusive share is an upper work
   family share, not a predicted saving.
4. **Reduce repeated visibility queries.**
   `G_UnitIsForcedVisibleToPlayer` walks every player owner even when every
   forced-visibility count is zero. A derived active-owner mask/zero-source fast
   path needs exact add/remove, shared-vision and load handling. Snapshot CPU
   overlaps visibility; do not add the percentages. Existing shadowcast/rim
   updates have ordered grid dependencies and cannot simply become parallel
   per-unit writes.

Raw samples, compressed scheduler trace, matching APK symbols, call-site/source
summaries, HTML report and bounded capture recipes are retained outside the Git repository
in the profile archive (`$OPENREALM_EXPERIMENTS_DIR/warcraft-3/optimization-archive/post-wide-profile-20261018/README.md`, external archive).
The matching renderer symbol hash is `e142b2cb…`; use the earlier trial's frozen
source for DWARF lines, since the later source-default macro shifts line numbers.
Workload images show 200 initial units and 162/171/168/171 remaining in baseline/
CPU1/CPU2/scheduler captures respectively; the after-images include collection
overhead and are not equally timed combat endpoints.

After this profile, the existing tested `aa0c9bfb…` APK stays installed with
`seta r_mdx_palette_ubo "2"` appended to the original config, following the
standing instruction to enable good optimizations. Original autoexec presence,
log, campaign progress, map and all five save/checkpoint files were hash-verified;
test bindings, forced startup map and tracing were removed. The ordinary launcher
is open with no native game process. No new algorithm, quality, protocol or save
format change is introduced by profiling.

### Exact sight footprint reuse on Honor (October 18)

After effect-geometry batching, a separate 20-second all-thread CPU profile of
APK `674d5c9d613922e48f7243b4a68ccedeccd0274531a288f701a6fc1b7dac33a1`
recorded 10,729 samples with no loss. SDLThread's userspace CPU partition was
64.77% client / 34.67% server; Mali driver leaf samples were 25.16% of that
thread, `G_FowUpdate` inclusive 7.15% and `G_FowCastLight` 4.33%. These are CPU
execution shares, not GPU durations; inclusive stacks overlap.

`wc3_fow_cache` now retains exact shadowcast footprints keyed by source cell
and integer radius, validated against a full blocker-bitmap snapshot. It keeps
the live, ordered tree-rim expansion and scripted reveals outside the cache.
See [fog cache contract and tests](fog-and-cinematics.md#exact-sight-footprint-cache).
Android defaults to `1`; `0` preserves the complete original cast path. Other
platforms default to `0`. No quality, grid resolution, animation or simulation
rate changes are involved. `wc3_fow_stats 1` includes replacement counts so
future cache-size/associativity trials can distinguish occupied-slot misses.

The Honor AGM3-AL09HN / Mali-G52 comparison uses the existing mixed 200-starting-unit
first combat, 67% world rendering, full particles, camera `(4600,-4600,3000)`,
25-second windows, fresh native processes and no overlapping host builds or
profilers. The prototype gives **17.970 → 18.567 FPS (+3.32%)**. A separate
B/D/D/B comparison on the final test APK, with D using its default and no cache
override, gives:

| Path | Separate windows, FPS | Weighted FPS |
|---|---|---|
| B: `wc3_fow_cache 0` | 17.844 / 18.120 | 17.985 |
| D: Android default | 18.537 / 19.285 | 18.911 |

The final observed gain is **5.15%**. Mean server time per loop
moves from **16.12 to 14.50 ms**. Keep
prototype and final cohorts separate. Combat evolves: units die and spell/RNG
work varies, so these are paired scene measurements, not identical command
replays or a guarantee for the first TFT mission. The 40-FPS target is unmet.

The red regression fails on repeated recursive work before the optimization.
Five added tests cover scheduler consumption, exact planes/occupied/dirty rows
for three viewers, changing blockers, movement/radius/death/modifier states,
large-radius bypass, the full 63-cell row, off/on and map lifecycle, and scratch
allocation failure. Full-suite final JUnit: **6,925 executions, zero
failures/errors/skips**. RoC/TFT fog suites pass **20 tests / 566 assertions**
each, also in release. Network and save formats are unchanged.

A separate final 20-second CPU profile records 10,878 samples with zero loss.
`G_FowUpdate` inclusive falls from 7.15% to 4.89% of SDLThread userspace CPU,
and `G_FowCastLight` from 4.33% to 1.40%. The final Mali driver leaf share is
26.68%; about 10.39% of main userspace CPU samples are in driver work called
from the ordinary/instanced MDX draws. This supports targeting command/vertex
binding preparation next; it does not identify GPU duration or promise a gain
from changing those bindings. The profile is excluded from FPS acceptance.
Screenshot status OCR confirms all eight windows start at 200 live units;
the four final windows end at 164/165/165/166, around 30 fight ticks. Thus the
final default cohort did not simply finish with substantially fewer actors.
Combat/spell randomness still prevents identical-workload claims.

Test APK SHA-256:
`b1783789664bba59f2e32dc03698c20006635d85e03eda895df35b5c80c90055`.
Only `librealm_game.so` changes relative to the preceding accepted APK; renderer
and other native libraries match. All eight windows, configs, screenshots,
profiles, tests and source snapshots are under
`build/android/optimization/cycle-after-effects-20261018/`.
Durable source/evidence archive (`$OPENREALM_EXPERIMENTS_DIR/warcraft-3/optimization-archive/fow-shadow-cache-20261018/README.md`, external archive).
The owner's new rule is recorded in `AGENTS.md`: ordinary/distribution builds
are prepared only on explicit request. Subsequent captures reused the APK that
had already finished building before that instruction; no further normal
build/package pass was launched.

### Parallel MDX preparation on Android (October 18)

Visible world actors can prepare their exact MDX poses in a bounded SDL worker
pool. The context owner reserves/deduplicates slots in the existing 4 MiB pose
arena; workers write disjoint compact matrix ranges with private local/global
node scratch and sampling scope. They never write cache metadata, call GL,
emit particles, dispatch events or mutate model data. The owner also executes
jobs and joins the batch before geometry/socket/event consumers continue.
Pose time, fractional interpolation, required-node ancestry and billboard
placement are the same inputs used by ordinary drawing, without an extra frame
of presentation latency.

`R_DrawWorld` submits terrain first, then collects entity poses. The collector
reuses the draw path's visibility/material/instancing rules and unions solid
and alpha bone masks. Emitter/ribbon models retain the full pose their existing
immediate path requests. Hidden/empty work is skipped; terrain-fitted models
remain on demand because fitting can release cached model variants. A slot's
job index is reused only after verifying that the current job still owns that
slot. Capacity excess is evaluated normally when drawn.

The pool has at most three background workers, claims four poses at a time,
and dispatches only batches of at least 16 unique incomplete poses. There are
512 reservations; the first 256-reservation prototype missed some expensive
actors in this mixed scene. Workers are created lazily and stopped at map and
renderer teardown. Model release invalidates queued reservations. Changed
render time, interpolation or view projection cancels unexecuted work. All
active jobs have completed before the owner can resume rendering or release
models.

| Cvar | Android default | Other platforms | Meaning |
|---|---|---|---|
| `r_mdx_pose_workers` | `min(3, max(0, SDL_GetCPUCount()-1))` | `0` | Background worker count; `0` disables preparation |
| `r_mdx_pose_flush` | `1` | `0` | Submit pending terrain with `glFlush` before a sufficiently large pose batch |
| `r_mdx_pose_stats` | `0` | `0` | Once-per-second `[MDX_POSE]` diagnostics |

`glFlush` supplies earlier GPU work while CPU pose preparation runs; no GPU
completion wait is introduced. Diagnostics average requests, unique jobs,
evaluated nodes, worker-owned jobs, actual prepared-cache hits, preparation
wall time and final worker-wait wall time per view. `flushes` counts actual
submissions in the report interval. The world CPU stage includes preparation;
a smaller entity stage alone does not establish a frame-time improvement.
Use the overall frame count/time and compare whole renderer wall time. GPU
execution duration remains unmeasured on this Honor.

The new bounded job/worker scratch and slot metadata occupy approximately
0.5 MiB at the three-worker limit, plus SDL thread stacks. Each worker has
128 KiB of node matrix scratch; the 4 MiB cache is shared rather than replicated
per thread. No per-frame allocation is needed after the pool has started.

Early Honor experiments at 67%, full animation/particles and fresh first fights:

| Preparation/submission variant | FPS observations |
|---|---|
| Original APK | 16.478, 16.583 |
| Updated scalar control, workers/flush disabled | 16.526, 16.433, 16.411 |
| Three workers, no explicit submission | 16.694, 16.842, 16.766 |
| One worker restricted to CPU 6/7, no explicit submission | 16.642, 16.526 |
| Three workers, submission before pose evaluation | 17.389, 17.264 |
| Submission immediately after terrain, no workers | 16.779, 16.773 |
| Same immediate submission, three workers | 17.402, 17.409 |

The last two rows use the **same prototype APK** in H/I/I/H order. This control
separates earlier submission from the additional parallel preparation benefit;
do not attribute the whole gain to threading alone. Battles begin with 200
living units and retain real damage/spells; later random spell/casualty histories
are not bit-identical. All valid windows, including slow variants, remain in
`build/android/optimization/mdx-pose-workers/`. FPS captures do not overlap host
builds or CPU/GPU profilers. These exploratory APK versions are separate from
the final default-build acceptance below.

Final acceptance uses one ordinary APK in **Z/B/B/Z** order. Z has no worker or
submission overrides, only diagnostics enabled; B sets workers to zero, which
also disables the associated submission. Each approximately 25-second window
uses a fresh native process, its first fight, render scale 67%, full particles
and the same camera `(4600, -4600, 3000)`:

| Final APK mode | Run 1 FPS | Run 2 FPS | Total frames / total time FPS |
|---|---:|---:|---:|
| Workers disabled | 16.352 | 16.447 | 16.399 |
| Android defaults, three workers | 17.204 | 17.373 | 17.287 |

This is **+5.41% FPS** for preparation plus earlier terrain submission; it is
still well below the 40 FPS target. Default runs confirm all three `mdx-pose`
threads. Median diagnostics per view are 286–295 unique jobs, 12.8–13.5 thousand
evaluated nodes, 185–188 worker-owned jobs, 537–541 prepared-cache hits,
4.80–4.88 ms total preparation and 0.37–0.43 ms final worker wait. Preparation
includes collection, dispatch, owner work and the join; do not add its wait
again. Particle counts remain populated in all accepted windows.

APK SHA-256: `e4bb52f33fd48bb9bfcac2f4854ca7538bbf0036a0103e5bf3aa406c85bc752e`.
The packaged renderer matches the local Android build byte for byte. The
retired affinity/frame-palette controls are absent from both final source and
renderer binary. Raw runs and the aggregation are in
`build/android/optimization/mdx-pose-workers/final-comparison.json`.

The self-affinity experiment successfully limited the worker to CPU 6/7 while
leaving SDLThread on CPU 0–7, but did not establish an advantage over three
unrestricted workers. Its cvar, capacity selector, sysfs reads, affinity fields
and Android `_GNU_SOURCE` build change were removed. The independent early
submission mode used by H/I was also retired; the retained submission occurs
only when pose jobs will actually execute. External `run-as ... taskset -p`
is denied on this Honor; a native calling-thread affinity operation is a
different permission path. The failed external attempts occurred before any
FPS window and are retained as aborted launches.

Tests use real pose evaluation and the world-render entry point: independent
actors/global sequences/billboards with exact float comparisons, duplicate-mask
union, partial-to-full expansion, current-batch slot ownership, capacity excess,
small/disabled batches, stale view inputs, actual model-release cancellation,
solid/alpha mask reuse, no emitter advancement during preparation, and positive/
inverse GPU-submission cases. Shared shader/transport math is unchanged.
Network and save formats are unchanged.

A separate final all-thread `simpleperf` capture uses `task-clock:u`, 500 Hz,
DWARF stacks and a bounded 20-second first fight. It records 11,350 samples
with zero reported lost samples. The three workers account for 2.582 of 22.700
sampled user CPU seconds (11.37%); SDLThread accounts for 17.734 seconds
(78.12%). This confirms active background pose evaluation, rather than merely
created threads. It is not a second FPS acceptance window.

Within **SDLThread user CPU**, client/server/other partition as
67.46% / 32.12% / 0.42%. Mali driver leaf samples account for 32.82%; inclusive
`G_RunEntities` is 17.29%, `G_FowUpdate` 6.80% and `MDLX_EvaluatePose` 4.85%.
Driver callsite attribution still finds 11.48% of main user CPU under ordinary
MDX `glDrawElements`, versus 2.15% under instanced MDX draw submission and
1.76% under the batched bone-texture upload. Reducing ordinary draw/driver work
and server simulation remain separate follow-up candidates. These percentages
are CPU execution samples, not GPU or frame durations; inclusive parents and
children overlap and must not be summed.

The profile lives under `mdx-pose-workers/final-cpu/`: `perf.data`, matching APK
libraries, a build-ID binary cache, `summary.json`, `driver-callsites.json`,
`report.txt` and `report.html`. The HTML viewer requires its CDN assets.
Recompute summaries with `python3 ../summarize_cpu.py` and
`python3 ../driver_callsites.py` from that directory. The earlier isolated
probe below retains an archived source copy and is reproducible independently
of the new context implementation.

Verification: baseline and final `make test` pass; the final full run contains
35 JUnit reports and 6,911 test executions, without failures, errors or skips.
Both normal and shadow renderer suites pass 234 cases each in debug and release
builds; the ordinary Android APK builds, signs and aligns successfully. The
emitter fixture proves that preparation does not advance a live emitter, then
checks emission through the real particle path. Engine/menu boundary audits
are clean. Host sanitizer runtimes failed before even a minimal program could
run, so no sanitizer pass is claimed.

The final APK remains installed. Original render configuration (100%), campaign
progress, test-map bytes and save-file hashes were restored and checked;
the originally absent `autoexec.cfg` is absent. During final verification an
ordinary game process had been opened again; verification then became read-only
and did not interrupt it. Its live `engine.log` naturally differs from the
archived pre-test log. See `restoration.json`; the earlier restoration record
is explicitly superseded. ADB's original `exec-out cat` returned success with
ENOENT text for the missing autoexec, so presence metadata was corrected from
that retained observation. Always check existence separately before backing up
optional files.

#### Measured follow-up targets

Prioritize the remaining work using the final profile above, rather than the
number of available cores. Percentages below refer to main-thread user CPU,
not frame duration, and do not predict FPS gains directly.

- **Geometry batching for actors with effects:** in the pre-extension baseline
  (now effect mode 0), `MDLX_InstanceBlocker` rejects a whole model immediately
  if `model->emitters` or `model->ribbons` is present.
  Two final Z2 solid reports have emitter rejects of 4,051 / 6,569 and
  3,612 / 5,834 **attempts** (61.7–61.9%). This is the first rejection reason,
  not a count of unique units, draw calls, or guaranteed compatible packets.
  Ordinary MDX draw submission still attributes 11.48% main CPU to the driver.
  A useful next experiment separates eligible geometry from per-actor emitter,
  ribbon and event work while preserving advancement/RNG order and alpha
  compositing. Existing batching groups only adjacent compatible actors;
  collecting eligible opaque draws by actual material/model state is another
  candidate. Transparent order and depth/shader equivalence remain constraints.
- **Frozen CPU draw preparation:** culling, material/visibility samples and
  packet preparation can potentially share immutable per-view inputs across
  workers. Existing `mdlx`/shader state mutation (notably UV evaluation in
  `MDLX_SameMaterials`) must first become owner-independent outputs. GL calls
  stay on the context owner. The current profile does not quantify the gain
  from this candidate separately.
- **Fog visibility:** `G_FowUpdate` is 6.80% inclusive. It reads sight sources
  after simulation/collisions and writes per-viewer visible/explored grids,
  row-dirty flags and a shared `level.fow.rim_cells` scratch array. Jobs would
  need frozen blockers/sources plus private masks/scratch, followed by an exact
  owner merge before Far Sight and authored fog modifiers. Splitting by player
  alone has little concurrency in a single-connected-player scene. Preserve
  blocker-rim semantics and modifier order; parallel source jobs are a
  feasibility candidate, not yet a proven equivalent implementation.
- **Server/render overlap:** `common/main.c` currently invokes `SV_Frame` then
  `CL_Frame`; the final CPU partition is 32.12% / 67.46%. Separating their
  execution has a larger candidate region than the remaining pose work, but
  requires explicit snapshot/command publication and audits of loopback,
  shared services and teardown. `G_RunEntities` is not independently parallel:
  callbacks mutate entity state and publish ordered events, and later trigger
  crossing checks consume their results. Investigate pure queries or a stable
  server/client ownership boundary before parallelizing entity callbacks.

On October 18 the user reported stable **20–25 FPS at 100% native render scale
in TFT's first mission** with the new build. This is a useful gameplay
observation, not a paired benchmark of the mixed-army scene or proof of a GPU
time reduction. Reaching 40 FPS from that range requires frame time to fall
from 40–50 ms to 25 ms (37.5–50%); the next substantial improvement likely
needs several of the remaining paths, not only more pose workers.

### Parallel pose feasibility on Honor (October 17)

An isolated CPU experiment exercises the October 17 `MDLX_BindBoneMatrices` /
`MDLX_BindBoneSubset` implementation and shared matrix/quaternion/vector math.
It **does not add workers to the game or establish an FPS improvement**. Local
sources, build commands, raw runs, negative control and device state checks are
in `build/android/optimization/mdx-parallel-20261017/`.

Honor's sysfs reports CPU 0–5 in one frequency policy, maximum 1.8 GHz and
`cpu_capacity=445`; CPU 6–7 share another, maximum 2.0 GHz and capacity 1024.
These are kernel scheduling capacities, not measured throughput ratios. A
worker count equal to the number of online CPUs is not a scheduling policy.

The probe compiles an archived pre-worker copy of `r_mdx_anim.c`, changing **only eight
mutable scratch/cache declarations** to `_Thread_local`. It uses a frozen
render input while jobs run. That experiment left production source and the installed APK
unchanged. A persistent pthread pool dynamically claims four actors at a time;
the calling thread also executes jobs, then joins the frame barrier. Thread
creation, initial TLS allocation and correctness comparisons are outside timing;
job dispatch, all pose work, result copies and the completion barrier are inside.

Each batch contains 200 actors with 32 sparse-ID bones, T/R/S tracks, distinct
old/new frames, interpolation 0.37, global sequences, parent chains and distinct
placements. Half use a billboard ancestor. Subset-to-full expansion and a
repeated full-pose cache hit are exercised. Eight validation frames and the last
timed frame compare every finite matrix component **bit for bit** against a
serial evaluation. All 30 device runs pass, totaling 27,648,000 compared float
components. A shared-scratch negative control fails this comparison on the host.

Each run times 300 batches. The unrestricted order is
`1,2,4,8,8,4,2,1`, repeated three times, retaining all results. A separate
`1,2,4,4,2,1` diagnostic restricts this child process to CPU 6/7 with `taskset c0`.
There are six measurements per unrestricted row and two per restricted row:

| Execution lanes, including caller | Scheduler chooses CPUs, median ms/batch | Restricted to CPU 6/7, median ms/batch |
|---|---:|---:|
| 1 | 4.724 | 4.759 |
| 2 | 6.224 | 2.439 |
| 4 | 3.462 | 2.493 |
| 8 | 2.544 | — |

Two lanes on the larger-capacity pair provide **1.95x pose throughput**. Without
the restriction, two lanes range from 3.249 to 6.699 ms, often slower than one.
Recorded dispatch CPU masks show the helper confined to CPU 0–5 in several slow
runs. Eight unrestricted lanes achieve 1.86x, rather than anything close to 8x.
This diagnostic uses an ADB-shell process (`cpuset=/`, initial affinity `ff`),
static-linked math and a synthetic workload, with no competing renderer, GPU
submission or simulation. It does not validate a production affinity policy,
sustained thermals, application scheduling, or an optimal worker count.

The unchanged game's whole-cycle profile below assigns 13.50% of main-thread
userspace execution to `MDLX_BindPose`, including its track lookups. Applying
the synthetic 1.95x speedup to that fraction would remove about 6.6% of that
CPU budget before integration costs. This is an illustrative Amdahl estimate,
**not a predicted FPS**. Pose parallelism alone does not explain a path from
the measured 16.9 FPS to 40 FPS. Mali submission remains a separate large cost.

Ownership constraints identified before the later implementation:

- The pre-worker `r_mdx_anim.c` had shared `local_matrices`, `node_matrices`, `sample_scope`,
  pose slots, arena and render-clock cache key. It could not run concurrent
  jobs as-is. The experiment replicates the existing 4 MiB matrix
  cache per lane, plus node/slot scratch; that is an experimental convenience,
  not an adopted memory design. Keep immutable pose inputs explicit and give
  each job an exclusive result range and each worker private scratch. Reserve
  cache slots/arena ranges before dispatch rather than racing on cache inserts.
- Snapshot the model pointer/lifetime, old/new frames, lerp fraction, render
  clock, required-node mask and billboard placement. Preserve ancestor
  expansion, exact pose keys and subset reuse. Do not eagerly compute every
  node for every world object: the existing demand-driven paths avoid unused
  poses. Results must be ready before geometry, sockets or events consume them.
- Keep GL calls and ordered submission on the context owner. Particle creation
  uses the shared particle pool, `rand()` and model emitter accumulators;
  ribbons also have shared instance/strip storage. Parallel pose computation
  does not make these consumers safe to execute in workers. Preserve their
  order and animation precision. Drain workers before model release, cache
  invalidation, map replacement and renderer shutdown.
- `G_FowUpdate` is game-owned. Multiple connected viewer grids could be separate
  jobs, but single-player currently updates one connected viewer. Parallel unit
  reveals into that grid race even when both write `1`, including dirty-row
  writes. They need independent visibility results and a deterministic merge.
  Build the lazily allocated blocker-prefix scratch before jobs; apply scripted
  fog modifiers after the reveal barrier in the existing order. No client-owned
  copy of authoritative visibility is appropriate.
- `G_RunEntities` is not a collection of independent actor jobs: combat,
  collision, triggers and allocation affect other entities. Preserve simulation
  ordering unless a separately tested command/result contract replaces it.
- Moving all of `SV_Frame` into another thread requires more than a function
  call change. `common/net.c` loopback queues can reallocate/free storage while
  receiving; command parsing/buffers and the cvar registry are shared mutable
  state. Client lifecycle commands currently call server map/save/pause/shutdown
  functions synchronously. These need explicit ownership/queues before overlap.

Reproduce the isolated experiment from the local artifact directory:

```sh
python3 build/android/optimization/mdx-parallel-20261017/build_probe.py host
build/android/optimization/mdx-parallel-20261017/probe-host 4 40
python3 build/android/optimization/mdx-parallel-20261017/build_probe.py android
caffeinate -i python3 build/android/optimization/mdx-parallel-20261017/capture.py
```

The capture requires the ordinary launcher, leaves the native game inactive,
checks identical launcher/PID/APK state afterward and removes its temporary
device executable. No configuration, map, progress or save file is written.
The normal full test suite also passes. Host sanitizer runtimes are unusable:
a minimal ThreadSanitizer hello-world exits with SIGSEGV before printing,
and a minimal AddressSanitizer hello-world exceeds the 10-second startup budget,
including outside the sandbox. No sanitizer result is claimed. This isolated
probe did not cover real-render-path lifetimes or game FPS. See the later
implementation section above for its render tests and fresh first-fight acceptance.

### Whole game-loop CPU and scheduler profile (October 17)

The user requested a whole-cycle profile and selected `CombatBench200.w3x`.
Three separate fresh native processes measured the first battle at 67% scale,
particles=2, full animation and camera `(4600,-4600,3000)`: an unprofiled
25-second FPS window, a 30-second Simpleperf sample and a 20-second scheduler/EGL
trace. Repeated-fight particle loss was avoided. No builds or profilers overlap
that primary FPS window. The installed APK stayed unchanged at
`0905ada59c79e764365b07c24cf99aaa7e9a25f866cd401d399374ee112f0da1`.
CMake confirms `RelWithDebInfo`, `-O2 -g -DNDEBUG`; matching APK libraries contain
`.debug_info`, `.debug_line` and `.symtab`. An unoptimized debug build is unnecessary
and would distort the workload being investigated.

The control is **16.889 FPS / 59.211 ms per rendered frame**, median 646 live
particles. `COM_FRAME` has the same 417 loop/render counts in the window:
mean server wall time 16.646 ms/loop, client 42.551 ms, online 0.0001 ms.
These stage measurements include waits and are not active CPU times.

Simpleperf recorded `task-clock:u`, 500 Hz, DWARF callchains, 15,237 samples
and **zero lost samples** (record stderr retained). All application threads
were sampled; 30.474 sampled userspace CPU-seconds include 27.020 on the native
`SDLThread` (88.67%). The native PID/TID is 31913/31953; the launcher also
contributes 0.020 seconds. Count actual thread membership rather than only
samples whose unwind reaches `SDL_main` (some chains stop earlier).

| Partition of active userspace CPU on SDLThread | Share |
|---|---:|
| `CL_Frame` subtree | 68.53% |
| `SV_Frame` subtree | 31.05% |
| Other/short callchains | 0.42% |

Inclusive hotspots on that **same thread denominator** are `MDX_RenderModel`
44.65%, `MDLX_RenderGeoset` 23.77%, `MDLX_BindPose` 13.50%,
`MDLX_GetModelKeytrackValue` 6.45%, `G_RunEntities` 16.74% and `G_FowUpdate`
6.42%. These overlap and must not be added. The FOW update and entity loop are
separate `G_RunFrame` stages. Exclusive leaf-library shares provide a different,
disjoint partition: Mali driver 31.54%, game 24.00%, renderer 22.52%, libc 9.64%,
shared math/helpers 4.89%, and remaining libraries 7.41%.

Matching debug info maps driver samples to engine caller sites: ordinary
`MDLX_RenderGeoset` draw at `r_mdx_geoset.c:724` accounts for 11.23% of all
SDLThread userspace CPU; instanced draw at line 718, 2.28%; the palette texture
upload, 1.81%; matrix uniform application, 1.76%. These are **CPU samples inside
Mali called from those sites**, not GPU execution times. Other caller lines can
be approximate in optimized code; raw addresses and inline chains are retained.
The private hot address range `0x8c5ecc..0x8c5f24` traverses linked records and
branches on fields. Do not label it an OpenCL operation from a stripped binary's
nearest exported `clSetContextDestructorCallback` name, or infer that all driver
CPU work is a GPU wait. Private function semantics remain unrecovered.

Atrace (`gfx sched freq idle sync`) independently captured native TID 32244:
786,928 entries retained out of 786,928 written, 20.0007 seconds, no overwritten
buffer. Thread state accounting is:

| State | Seconds | Wall share |
|---|---:|---:|
| Scheduled on CPU | 18.348 | 91.73% |
| Runnable, waiting for a CPU | 0.446 | 2.23% |
| Interruptible blocked | 1.207 | 6.03% |
| Uninterruptible blocked | 0.0009 | 0.005% |

CPU slices include kernel/interrupt activity. Almost all execution is on CPU
6/7; 14.742 scheduled seconds have a reported 2,000-MHz frequency. The trace
contains 334 complete EGL swaps and 333 swap-start frame intervals: mean
59.879 ms, median 66.929 ms, p95 80.636 ms. Mean per interval is 54.933 ms
scheduled on CPU, 1.336 ms runnable and 3.610 ms blocked. The server's 10-Hz
work helps explain uneven frame lengths; the trace alone does not assign each
long frame to a server tick. EGL swap spans average 3.332 ms wall, with 1.113
seconds total over the trace; only 0.732 seconds of that is blocked time.
Nested queue/dequeue spans are not added again.

This workload is dominated by main-thread CPU execution, including graphics
driver work, with substantial simulation cost. It is stronger evidence than a
flat resolution sweep. It does not measure concurrent GPU execution, prove
GPU headroom, or establish the device's maximum possible FPS. Practical next
owners are ordinary MDX draw submission/driver validation, pose/track work,
and server entity/FOW work. 40 FPS requires a 25-ms frame budget; eliminating
only the observed waits cannot bridge the current ~60-ms frame.

Artifacts: `build/android/optimization/whole-cycle-20261017/`. `cpu/report.html`
is the standard interactive NDK report with source annotations (its UI loads
external JS libraries); `summary.txt`, `cpu/summary.json`,
`cpu/driver-callsites.json` and `sched/summary.json` are standalone evidence.
`capture.py` performs bounded launches; `sample_summary.py` weights event
periods and de-duplicates recursive inclusive frames; `trace_summary.py`
reconstructs switch/wakeup states and has synthetic accounting checks. The
traced/instrumented log windows extend through recording postprocessing and
trace pull, so they are not equal-duration profiler-overhead comparisons.
The CPU and scheduler recordings also have different battle/spell histories.

Original config, autoexec absence, engine log, benchmark map and campaign
progress were restored; save file hashes were checked unchanged after the cold
ordinary launcher. APK checksum is unchanged, no native game process remains,
and `debug.atrace.tags.enableflags` is 0. See `restoration.json`.

## September 21 CPU profile: Graveyard update scan

Inspection of `build/perf-full.txt` found about 7K `cpu/cycles/P` samples, no lost samples,
and 89.30% inclusive cost under `SV_Frame`. These are sampled CPU-cycle shares, not wall-time or GPU measurements;
inclusive parent/child percentages overlap. The report does not identify the capture command, map, or build revision.

`graveyard_find_thinker` accounts for 74.76% self / 75.28% inclusive in the historical capture. The confirmed code path is
`monster_think -> S_RunAbilityUpdates -> CAbilityGraveyard -> graveyard_ensure -> graveyard_find_thinker`.
`S_RunAbilityUpdates` dispatches every registered update procedure for each unit. The current
`skills/s_undead_abilities.c` implementation checks `G_UnitAbilityLevel(..., Agyd)` before scanning
`globals.num_edicts`, so ordinary living units do not perform that fruitless full scan. `git blame`
attributes the original ordering to `b8deb304` (`wc3: implement corpse mechanics`); the correction is
covered by the current branch's Graveyard tests.

The first two recommendations below are already implemented on the current branch:

1. `graveyard_ensure` checks `G_UnitAbilityLevel(..., Agyd)` before scanning for its thinker,
   so ordinary units do not pay the full edict scan. The focused Graveyard and corpse tests cover
   duplicate updates, ability removal/re-add, death and save/load.
2. `InitAbilities` builds a procedure-to-first-registry-index cache for `GetAbilityIndex`, preserving
   duplicate-procedure first-match behavior and the 255 miss sentinel.

Remaining implementation sequence:

1. Re-profile the corrected Graveyard path through real `S_RunAbilityUpdates` dispatch with many
   non-Agyd units and increasing edict counts, including freed slots. Keep behavior in `CAbilityGraveyard`;
   retain corpse production timing, cap, radii and
   saveable thinker ownership. Extend `tests/t_exhume.c` coverage for duplicate updates, ability removal/re-add,
   death and save/load. Measure real Graveyard counts before introducing a persistent thinker index.
2. Re-profile the same fixed scene with matching build mode, camera, entity population and simulation interval.
   Record absolute server/frame timings and repeated-run medians. The existing `wc3_perf.run_entities_1900`
   benchmark is a starting point; verify that its fixture actually enters `monster_think` and ability updates.
   Removing all current lookup self cost has an idealized fixed-work CPU speedup ceiling of about 4x,
   not a predicted FPS gain.
3. The full-list `R_ConformGroundSurfaces` candidate scan described below has now
   been replaced; see [the Honor work](#honor-200-actor-performance).
   An earlier capture measured 1.76%
   self / 2.19% inclusive; `screenshots/2026-09-25/fixed4/perf-full-Orc04.txt` measures 11.01% self /
   11.12% inclusive with about 3K CPU-cycle samples and no lost samples. Orc03 and Orc04 are different
   campaign scenes, so this is not an A/B regression measurement. The former nested loops scanned the full
   view entity list for each ground-conforming entity. A possible fix is to gather eligible
   `RF_GROUND_SURFACE` entity indices once into bounded renderer-owned scratch, then test only those
   candidates for each eligible `RF_GROUND_CONFORM` entity. Preserve original candidate order and leave
   `MDLX_TraceWalkableSurface`, highest-hit selection, and `ground_offset` behavior unchanged. This reduces
   the list work from O(C*N) to O(N+C*S), where N is the view entity count, C the conforming entity count,
   and S the walkable surface count; add spatial filtering only if a follow-up profile shows the exact MDX
   traces still dominate.

   Test first in the headless renderer suite using the production `R_ConformGroundSurfaces` path: cover no
   surfaces/no hit, hidden or model-less entities, overlapping surfaces (highest hit wins), altitude offsets,
   and `RDF_NOWORLDMODEL`. In the test build, count entity classification visits and exact trace calls; a scene
   with many unrelated entities, several conformers, and few surfaces should fail the old repeated full-list
   scan bound and pass with one collection pass plus C-by-S traces. Then repeat the same bounded Orc04 scene
   and record N/C/S counts and repeated profile or frame-time measurements.
4. This older `MDLX_BindBoneMatrices` profile was 2.01% inclusive, including geometry and attachment collection.
   The later bounded pose cache above implements reuse with explicit model/view/clock lifetime and billboard
   dependencies. No interpolation rate or pose precision was reduced.
5. `G_BlightPackRows` is 0.86% inclusive. `G_BlightWriteDatagram` retries encoding with one fewer row on
   overflow. Measure retry counts and patterns before selecting bounded chunk sizing or incremental packing;
   preserve dirty-row delivery, periodic sweeps and the existing wire encoding. The profile alone does not
   prove retries dominate this cost.

Run the baseline `make test` before implementation, reproduce each regression before its fix, and use focused
`wc3_spell.graveyard*`, corpse/save tests and `wc3_perf.*` comparisons for the first change. Subsequent optimizations
should be selected from the new profile, since the dominant scan currently suppresses their relative shares.
See [corpse mechanics](corpse-mechanics.md) for the authored Graveyard contract.

## Hero aura presentation cache

An earlier full profile assigned 41.62% of sampled CPU cycles to
`S_UpdateHeroAuraEffects`, with `hero_aura_presentation` scanning the edict list
and reparsing source ability lists for each recipient. The per-frame provider cache
now resolves Devotion and Unholy providers alongside the regeneration families;
presentation still checks live range, alliance, and target rules per recipient, but
iterates cached providers. Cache validity uses the `UINT_MAX` reset sentinel, so
frame zero does not rebuild the provider list for every entity.

The in-engine 1,900-unit benchmark measured `G_RunEntities` at 2,394.48 ms/call
while frame-zero cache invalidation rebuilt the list per entity. After the generation
invalidation and provider reuse it measured 2.27 ms/call in the same debug test
binary. This benchmark is a regression signal for cache invalidation and scaling;
it is not an end-to-end frame-rate claim. Aura presentation tests drive
`G_RunEntities` at frame zero and after a timed range change so the scheduler path
and cache reset contract remain covered.

The September 23 `build/perf-full.txt` capture exposed a separate mechanical path:
`S_UnholyHealthRegen -> hero_aura_bonus -> actor_aura_ability` accounts for 72.99%
inclusive sampled cycles. Each recipient refresh scanned all edicts and resolved all
combat aura aliases for every active friendly unit. `regen_aura_cache_update` now
caches those alias/rank references with the other aura providers, so a shared source
pass replaces the repeated recipient-by-edict alias resolution. Each recipient still
checks live source activity, alliance, range, and authored target masks; its numeric
cache keeps the existing two-second refresh and invalidates on AbilityData generation
changes. `wc3_spell.combat_aura_alias_resolution_scales_with_edicts` reproduces the
old resolver-call growth with 32 aura sources and 96 recipients, then checks the
linear edict-bound pass and runtime ability removal/re-add. The stored profile has
only about 4K CPU samples and no scene/build metadata; take a new weighted profile to
measure the post-fix cycle share.

Profile-driven optimizations across the renderer, client, and server. The five sampled hot spots and the fixes applied to each are listed below so a future reader understands *why* each path is shaped the way it is.

## Client frame-rate limiter

OpenWarcraft3 registers the archived `com_maxfps` cvar with a default of `64`, matching the intended classic-Warcraft presentation target. `0` disables the limiter. The same engine main loop is shared with the other game binaries, but their built-in default remains `0` unless their own configuration overrides it.

The limiter lives in `common/main.c` and applies only to non-dedicated clients. It measures the complete main-loop iteration with SDL's performance counter, including simulation, client work, rendering, and any VSync blocking, then sleeps/yields only for the remaining part of the target frame interval. It therefore does **not** change `FRAMETIME`, server tick accounting, animation/game timers, or `com_fast_forward`; if a frame already takes longer than the cap interval, no extra delay is added. `r_vsync` can still impose a lower effective frame rate than `com_maxfps`.

Keep `com_maxfps` separate from `com_frame_limit`: the latter is a diagnostic auto-quit counter for main-loop iterations. Useful commands are:

```bash
# Default Warcraft III cap: 64 FPS
build/bin/openwarcraft3 -data 'data/Warcraft III' +map 'Maps/Campaign/Human02.w3m'

# Uncapped rendering/performance measurement
build/bin/openwarcraft3 -data 'data/Warcraft III' +set com_maxfps 0 +set r_vsync 0 +map 'Maps/Campaign/Human02.w3m'

# Explicit alternate cap
build/bin/openwarcraft3 -data 'data/Warcraft III' +set com_maxfps 120 +map 'Maps/Campaign/Human02.w3m'
```

## MDX keytrack and ground splat hot paths

An Instruments capture after the 64 FPS cap was added sampled `R_KeyFrameBound`
for 321 ms and `R_ClipSplatPoly` for 273 ms over 9.23 seconds. These are useful
CPU targets, but changing them cannot raise FPS while `com_maxfps` remains 64.

The original `MDLX_GetModelKeytrackValue` searched three bounds in a packed keytrack. The
keyframe stride depends only on the track's data and interpolation types, so
compute it once per evaluation and pass it to each search and key access.
`R_MakeSplatTile` builds an axis-aligned terrain tile. Only rectangle edges
inside that tile can cut its two triangles; pass those edges to
`R_ClipSplatPoly` in the original left/right/bottom/top order. Keep Z
interpolation on the original terrain triangles.

A local `BUILD=debug` headless microbenchmark with a 32-key linear track and
one tile measured 140 to 93 ns per keytrack evaluation, 443 to 276 ns for a
single-edge splat, and 509 to 504 ns for a four-edge splat. These measurements
cover the CPU entry points only; they do not predict full-frame FPS. Use
`make test-renderer-model` for keytrack sequence and partial-tile geometry
coverage, then profile an uncapped run before pursuing another change.
The later [neighbour-key sampler](#mdx-neighbour-key-sampling-october-17)
removes the two interval searches from ordinary interior samples.

## MDX bone setup (was ~16%)

`MDLX_BindBoneMatrices` (`games/warcraft-3/renderer/mdx/r_mdx_anim.c`) serves geometry passes and attachment/event consumers. The old path:

1. `memset(node_matrices, 0, sizeof(node_matrices))` — 64 KiB (1024 nodes × 64-byte matrix) cleared per model.
2. Two loops over all `MDX_MAX_NODES` (1024) slots, each null-checking the sparse `model->nodes[]` array.
3. A `bone_matrices[MDX_MATRIX_PALETTE]` build loop plus a `glUniformMatrix4fv(uBones, ...)` upload — dead work, because `MDLX_BindGeosetMatrixPalette` re-uploads the real per-geoset palette immediately afterward.

Fix: `R_LoadModelMDLX` now builds a compact `model->node_list[]`/`num_nodes` (the nodes the model actually has, typically tens), and `MDLX_BindBoneMatrices` iterates only that list, zeroing each used matrix individually to reset the `v[15]==0` "computed" flag that `R_GetNodeGlobalMatrix` relies on. The dead `bone_matrices` build + upload were removed.

An early persistent pose-cache proposal was rejected because validity depends on both the interpolated `frame0/frame1` pair (with `lerpfrac`) and the global-sequence render clock. The later bounded render-clock cache described above includes those inputs, isolates billboard bases, and reuses exact poses across geometry passes and socket consumers; it does not reuse animated poses across render clocks.

## MDX geometry / material drawing (was ~19%)

`MDLX_BindGeosetMatrixPalette` (`r_mdx_geoset.c`) uploaded the full `BZ_BONE_PALETTE_MAX` (128) matrices per geoset. Skin indices are geoset-local (`0..num_matrixPalette-1`), so only `min(num_matrixPalette, BZ_BONE_PALETTE_MAX)` entries ever need to reach the shader. Uploading 128 when a geoset references, say, 12 was wasted uniform traffic on every draw.

## Entity shadows (was ~8%)

Unit shadows are ground decals: one shader (`SHADER_SHADOWSPLAT`), differing only by texture and rect. `R_RenderShadow` previously called `R_RenderRectSplat` per unit, which bound the splat shader/VAO/VBO, uploaded view uniforms, re-set blend/depth-mask, re-set texture wrap, then uploaded the vertex buffer (`glBufferData`) and issued a draw per shadow — hundreds of tiny draws/upload per scene.

Fix: `r_war3map_ground.c` now exposes `R_BeginSplatBatch` / `R_AddRectSplat` / `R_EndSplatBatch`. `R_RenderRectSplat` was refactored to share `R_SetupSplatState` + `R_GenerateSplatTiles` with the batch path. `R_DrawEntityShadows` (in `renderer/r_ents.c`) runs a dedicated pre-pass that accumulates every visible unit shadow and flushes only when the texture changes or the buffer fills, collapsing N shadows into one upload + draw per contiguous texture run. WoW and SC2 provide immediate fallback implementations (WoW shadows already go through `R_GameRenderShadow` returning true; SC2 splats are flat quads).

## Client entity collection + snapshot copying (was ~11%)

Both `CL_ParseFrame` (snapshot copy) and `CL_AddEntities` (collection) scanned all `MAX_CLIENT_ENTITIES` (16384) slots every frame, even though a scene has far fewer live entities.

Fix: `client_state` gains `active_entities[]` / `num_active`, a compact list of entity numbers whose current state carries a live model. It is maintained incrementally in `CL_ReadPacketEntities` (U_REMOVE drops the index; a `!old.model → model` transition adds it) and `CL_ParseBaseline`, reset in `CL_BeginLoadingMap` on map change and in `CL_ClearState` on disconnect. `CL_ParseFrame` and `CL_AddEntities` iterate the compact list instead of 16384 slots.

## Server entity simulation (was ~10%)

`G_RunEntities` ran three full passes over `globals.num_edicts`, and `G_RunEntity` ran `spell_run_frame` + `unit_updatestatuses` + status compression for every edict — including freed edicts, since `G_FreeEdict` memsets in place and `num_edicts` is a high-water mark that never shrinks.

Fix: all three `G_RunEntities` loops skip `!ent->inuse`, and `G_RunEntity` guards with `if (!ent->inuse) return;`. Freed edicts carry no simulation state and are never re-sent, so this is a pure skip.

## Lumber order routing hitch (August 2026)

A weighted ARM `perf report` captured while ordering a Peasant to harvest a tree resolved the server hot path through `ai_walktree -> unit_changeangle_for_radius -> M_RefreshHeatmap -> CM_BuildHeatmapForRadius`. Roughly half of that route-build cost was `build_heatmap()` repeatedly expanding the mover footprint, and the other half was `bake_flow_field()` computing vectors for every reachable cell even though the Peasant samples only its current location.

The route now keeps three performance boundaries:

1. Harvest first searches only the small `HARVEST_RANGE` interaction disc for a collision-safe point with a direct static line from the worker. Reachable trees therefore steer to the interaction area without building a whole-map field.
2. Cached routes store integration prices only. `get_flow_direction()` computes the four local samples needed for interpolation on demand; there is no whole-map flow-vector bake on the order path.
3. Radius-expanded static walkability uses a summed-area table over baked `nowalk` cells, making each square footprint query O(1) during a genuine heatmap flood.

The unreachable-interior-tree behavior remains unchanged: when no direct harvest-range point is visible, the collision-sized integration field still drives the worker to the forest edge and exposes route completion/unreachability to Harvest, which owns retargeting.

## Verification

```bash
make test                    # full umbrella, incl. in-engine WC3 suites
make test-wc3-engine         # 344 tests / 918 assertions, incl. wc3_perf.run_entities_1900
make -j4 openwarcraft3 openwow opensc2   # all three renderers still build
build/bin/openwarcraft3 -data 'data/Warcraft III' +menu_main +screenshot 10 +com_frame_limit 20       # ROC
build/bin/openwarcraft3 -data 'data/Warcraft III' -tft +menu_main +com_frame_limit 20                 # TFT
```

The `wc3_perf.run_entities_1900` benchmark (`games/warcraft-3/game/tests/t_game.c`) measures `G_RunEntities` over 1900 active units.

## Static-scenery snapshot saturation (RG40xx report)

Commit `215d9630` classified doodads/destructibles correctly but made `G_FowPlayerCanSeeEntity` return true for every
`SVF_STATIC_SCENERY` entity, including unexplored cells. A bounded ROC Human02 run measured 2,430 edicts, of which 2,299
were static scenery; all 2,299 passed visibility and filled the 1,024-entity snapshot cap on every sampled server frame.
This explained the report's simultaneous tree-pop improvement, unexplored waterfall rendering, and CPU-bound FPS drop.
The accompanying [post-regression ARM address map](https://gist.github.com/sookyboo/0fc1e06966e3b677e22e8ff7c1f0edc0)
repeatedly resolves through `SV_AddVisibleEntityCandidate` / `SV_BuildClientFrame`, corroborating the measured path. It is
an `addr2line` mapping without sample counts, so do not derive percentages from repetitions in that text.
The reporter's [22 FPS baseline mapping](https://gist.github.com/sookyboo/b9ac1cc2657ad46209644878fab777b0)
contains the expected MDX geoset, entity-shadow, simulation, and fog-update stacks but no server snapshot/candidate stack.
The later capture adding that path is qualitatively consistent with the scenery regression; neither mapping contains the
underlying sample weights, so this comparison identifies a new path but cannot quantify its frame-time share.

Static scenery now follows the explored plane, like buildings: it does not pop when current sight leaves, but unexplored
scenery neither consumes snapshot candidates nor renders through black fog. Diagnose future regressions by temporarily
counting total and visible `SVF_STATIC_SCENERY` entities in `SV_BuildClientFrame` during a bounded Human02 run; remove the
counter after capturing the result.

### PortMaster building-cache claim audit

[Commit `f98c70c9`](https://github.com/corepunch/open-realm/commit/f98c70c94a3d01a784f5d6c9492ad50338e97199)
reports a Human02 increase from about 20 to 25 FPS after adding `G_FowUnitIsBuilding`. The submitted helper is never
called: `G_FowPlayerCanSeeEntity` still expands `UNIT_IS_BUILDING(ent->class_id)` directly, and the only executed change
is `g_fow_building_cache_count = 0` once during `G_FowInit`. Release `-O2` may remove the unused static helper entirely;
debug builds may retain it and shift code addresses, but neither case supplies a credible 20% frame-time reduction.

The intended optimization became unnecessary after typed-row binding. `G_BindEntityData` caches each immutable SLK row
on the edict, and `UnitMetaBoolean`/`UnitMetaString` resolve reflected fields through a compile-time FOURCC descriptor and
two offsets. Hot visibility code reads `ent->runtime.flags` directly. Test further changes separately from scene
progression, thermal/DVFS state, controller-helper changes, and build-mode changes; Human02 draw counts change as the
cinematic and entity population advance, so two instantaneous `r_stats` readings are not an A/B benchmark.

### PortMaster post-fix address-map audit

The [post-fix Human02 gist](https://gist.github.com/sookyboo/301df705f27a9ead75dcca9204ab7893) is three
`addr2line` outputs, not the sampled profile that produced the addresses. It contains symbols from FOW, snapshots, AI,
MDX animation/geosets, splats, particles, UI layout, minimap, text, and network parsing, but no sample counts,
percentages, timestamps, shared-library/GL-driver frames, or call tree. Repetition in the text is not a weight: use it
only to identify code that was reachable. Obtain `perf report --stdio` plus `perf script` or folded stacks from a fixed
Human02 interval before choosing work from this capture.

The best candidates based on scaling and current code, pending weighted data, are:

1. Make FOW work dirty-driven. `G_FowUpdate` currently hashes every blocker, clears every visible cell, and scans every
   edict every 100 ms. Track blocker/revealer cell changes and sight/alliance/modifier changes so a stationary world can
   skip blocker rebuilding and shadowcasts; clear only cells touched by the previous visible generation.
2. Stop scanning all static scenery for every client snapshot. Human02 has about 2,299 static-scenery edicts. Bucket
   them by FOW cell and add newly explored buckets to a per-client visible set, while retaining the ordinary dynamic
   entity path and snapshot delta contract.
3. Cache immutable unit metadata at spawn. `G_AcquisitionRange` and building visibility still reach linear SLK lookup
   paths. Extend the existing per-unit balance data for hot class values instead of querying `UNIT_*` macros during AI
   acquisition or snapshot construction.
4. Cache decoded client layout geometry until its payload or viewport changes, and preserve precomputed draw-order
   lists. `SCR_DrawLayout` calls `SCR_Clear` for every visible layer every frame; that function clears the full frame
   arrays, decodes the wire payload, and resolves layout again before three frame scans.
5. Batch adjacent UI quads by texture/shader/blend/clip. `R_DrawImageEx` currently reaches `glBufferData` and
   `glDrawArrays` for each image. This is a plausible GL4ES/handheld driver cost even when desktop CPU profiles make it
   look small. Text already has an atlas batch path that demonstrates the appropriate boundary.

MDX pose sharing and splat topology caching are secondary measurement candidates. Harvest routing is separately covered by the weighted tree-order profile above. The
local Human02 pose-key audit found little identical-pose reuse; disabling shadows barely changed desktop FPS; and
`get_flow_direction` itself only scans four generations and performs a bilinear interpolation. Do not trade animation,
terrain-conforming shadows, or movement quality for these without weighted evidence. First isolate the handheld run
with `r_norefresh`, then A/B `r_entities`, `r_unit_shadows`, `r_particles`, and `r_fogofwar` over the same scripted time
window; a large `r_norefresh` gain implicates rendering/driver cost, while a low no-refresh rate implicates game/FOW/
snapshot work.

### Cutscene snapshot and MDX report audit (August 2026)

The reported `SV_SendClientDatagram` subtree includes both `SV_BuildClientFrame` and `SV_WriteFrameToClient`; it does not
isolate wire serialization. The builder's overflow policy was an actual scaling defect: after filling the 1,024-entity
budget, every additional visible entity scanned all retained candidates to find the farthest one. The candidate set is
now a bounded max-heap, reducing selection from O(E*K) to O(E*log K), followed by the same entity-number sort required
by delta encoding. A server test feeds farther entities first, forces nearer replacements after the heap is full, and
checks the final wire order.

Unchanged entity deltas now compare the contiguous `entityState_t` first. Exact matches skip the descriptor-table walk;
changed or forced states retain field-granular encoding and the existing wire format. This benefits local and remote
clients equally. ioquake3's `SV_SendClientSnapshot` does not bypass serialization for loopback clients: it exempts
loopback from rate limiting, while only bots consume snapshots without transmission. Keep Open Realm's one snapshot
contract too; a direct-pointer loopback path would hide remote-client cost and create a second state-delivery path.

WC3 MDX key tracks were already contiguous file-shaped allocations, so repacking was not the prerequisite claimed by
the animation report. The measured defect was lookup: each sample scanned the complete packed track once for sequence
bounds and again for its interpolation pair. `MDLX_GetModelKeytrackValue` now uses binary lower/upper bounds over the
existing variable stride. Tests preserve exact-key sampling, pre-first-key clamping, interpolation, interval-tail wrap,
and exclusion of adjacent-sequence keys.

Do not apply the remaining proposals without a weighted same-scene A/B capture:

1. `nlerp` changes authored rotation timing, while Hermite/Bezier quaternion tracks require spherical quadrangle
   interpolation. Add a visual corpus and angular-error threshold before changing either the key-track interpolation or
   the separate old-frame/current-frame render blend.
2. MDX, M2, and M3 have separate loaders, animation clocks, and render paths. A shared SoA runtime is a cross-game
   architecture change, not an MDX load-time cleanup; measure each path before replacing its file-shaped representation.
3. NEON is useful only after a scalar batch with four independent bones exists. Hierarchy concatenation is parent
   dependent, and the current global matrix cache is indexed by sparse authored node IDs. Benchmark a scalar packed-pose
   prototype before adding an architecture-specific kernel.
4. Spawn/damage staggering changes JASS-visible timing and ordering. Shadow splats are already batched, and the local
   Human02 A/B above found negligible FPS change from disabling unit shadows. Neither change is justified by an address
   map that only proves reachability.

Re-profile a fixed Human02 interval with weighted stacks after these changes. Keep `SV_BuildClientFrame`,
`MSG_WriteDeltaEntity`, `MDLX_GetModelKeytrackValue`, and `Quaternion_slerp` separate in the report; only then choose
between spatial snapshot indexing, animation cursor state, pose packing, or interpolation approximation.

### Implemented follow-up: sparse FOW clears and spawn-cached unit fields

A temporary bounded Human02 diagnostic at the old `G_FowClearVisible` loop confirmed that each connected player
scanned all 65,536 cells of the 256x256 FOW grid although only 2,569 cells were visible in the sampled updates. The
player grid now records which rows contain visible cells. The next update scans 256 row flags and only `memset`s rows
that were actually populated; it still marks those rows dirty so the existing network delta contract is unchanged.
The row-occupied state is allocated, reset, and freed with the other per-player FOW planes.

Immutable unit acquisition range and building classification are now resolved by `SP_SpawnUnit`. AI acquisition and
FOW/snapshot visibility read the cached values instead of repeatedly walking unit metadata/SLK tables. Acquisition
range retains the old default-to-half-day-sight and cap-to-day-sight behavior. Building classification retains the
explored-plane rule, so buildings stay shrouded after current vision leaves while ordinary units disappear.

Clean detached worktrees at `d1a60ff4`, both including the waterfall particle-FOW shader change, were built with
`BUILD=release`. Five independent runs produced these medians on Apple M1 arm64:

| In-engine benchmark | Before | After | Change |
|---|---:|---:|---:|
| `G_FowUpdate`, 256x256 grid, 160 revealers, 2 players | 0.20 ms/update | 0.14 ms/update | 30% less time |
| `G_AcquisitionRange`, 1,900 units x 10 passes | 12.02 ms | 0.02 ms | 99.8% less time |

Run the comparison with:

```sh
make -j4 BUILD=release build/bin/openwarcraft3-tests
for run in 1 2 3 4 5; do
    build/bin/openwarcraft3-tests -data build/tests/wc3-engine-data -tft +dedicated 1 +test 'wc3_perf.*'
done
```

These are subsystem timings, not a claimed handheld FPS increase. A 1,200-frame release Human02 render A/B changed
draw counts throughout the cinematic and showed run-order/thermal noise larger than the expected server saving, so it
was not treated as an end-to-end result. Repeat the fixed-scene weighted profile on the reported ARM/gl4es device to
measure the actual frame-rate effect; renderer submission still dominates the available weighted profile.

### Contributor FOW follow-up audit

The non-PortMaster FOW commits from `sookyboo/portmaster_rebase_28_08_2026_2` were audited individually against current
main. `ffcb57ca` (replace the visible-cell loop with `memchr`/`memset`) is obsolete: the `visible_rows` implementation
above avoids scanning empty rows altogether. The dirty-blocker idea from `722469c8` and rim-cell list from `e279778d`
remain useful, but were reapplied rather than cherry-picked because current destructable lifecycle ownership has changed.

A temporary bounded ROC Human02 diagnostic over 50 FOW updates recorded 121,500 blocker-hash edict visits and 540,700
second-pass rim-cell visits for 41,909 committed rim cells. Steady updates now skip the blocker hash until a blocker is
spawned, freed, moved, scaled, hidden, killed, restored, or revived. Dirty updates retain the hash comparison as a
correctness check, so redundant invalidations do not rebuild the grid. The rim pass records its temporary blocker cells
while discovering them and commits only that list; initialization treats allocation failure as fatal instead of silently
falling back to the square scan. `wc3_game.fow_blocker_cache_skips_clean_and_unchanged_dirty_updates` covers the clean
cache hit, dirty/hash hit, and dirty/hash miss paths.

### Retail Game.dll fog audit

The ROC demo `data/Warcraft3demo/Game.dll` (build 4486, SHA-256
`286823c37a1083e91f07d040e46a9df7af4c4952e01fcbba460589bd4e297654`) retains `CFogOfWarMap.cpp`,
`CFogMaskTable.h`, `CFogModifier`, and a fog-checksum diagnostic. With image base `0x6f000000`, the routines at
`Game.dll+0x38e730`, `+0x38e7f0`, and `+0x38e8b0` index rows through a stored shift, clip spans, expand a 16-bit mask
to both halves of a 32-bit word, and update paired planes with `or`/`and`. The dispatcher at `+0x38f4b0` clamps a
map-space window, reads table entries, and calls those span helpers. Retail therefore uses packed, table-driven fog
planes rather than a byte setter for every cell.

The packed-mask series ending at `af82327b` is closer to retail in storage and update shape: it adds 16-bit current
and explored planes and writes word spans. It is not retail-exact. OpenRealm uses `(width + 15) / 16` rows rather than
retail's power-of-two dimensions, generates circular spans with `sqrtf` instead of the unrecovered mask table, and
enables that path only with `wc3_fow_fast`, which skips blocker occlusion. The normal shadowcast remains the closer
visibility behavior around blockers; packed storage alone is not evidence that fast fog matches retail silhouettes,
modifier selection, plane semantics, or scheduling.

`WC3_FOW_PACKED_MASK` remains a removable build guard: delete its blocks in `g_fow.c`, its fields in `g_local.h`, and
the define in `game.mk` to remove the experiment. Do not enable `wc3_fow_fast` for correctness or parity validation
until the retail mask tables and modifier semantics have been recovered.

### Local release A/B and remaining cost

A same-machine release-build comparison used ROC Human02, 150 console `wait` commands, `cmd cancel`, then 1,200 waits
with `r_stats 1` at the default 2048x1536 Retina drawable. Detached worktrees are required because changing
`BUILD=debug/release` does not invalidate existing make outputs. Results are directional because the campaign continues
to change the scene during the one-second statistic windows:

| Revision/configuration | Settled FPS | Draws/frame |
|---|---:|---:|
| `6c274d96` suspected-good | 345, then 435 as entities left the view/snapshot | 115, then 34 |
| `e471c472` | 311-316 | 146-148 |
| current + explored-scenery fix | 338-351 | 146-148 |
| current, `r_unit_shadows 0` | 342-350 | 129-130 |
| current, `r_entities 0` | up to 472 | 41 |

The current release therefore reaches the expected ~350 FPS locally even at the doubled Retina drawable. Disabling
shadows does not materially change FPS; entity model submission is the remaining scalable owner. A temporary strict
batch key `(model, skin, frame, oldframe, team, flags)` found 49 visible entities at the settled camera, 40 unique keys,
and only 14 entities across five repeated keys (largest group six). Naive MDX instancing has limited coverage there.
Do not treat the suspected-good 435 FPS window as equivalent content: its draw count fell to 34 as the older visibility
lifecycle removed scenery. Obtain a post-fix handheld profile with sample weights before redesigning MDX pose/geoset
submission; the address-only gists cannot choose between CPU submission and driver/GPU stalls.

### Review regression cases (PR #164)

- The active-list invariant is membership iff `cl.ents[index].current.model != 0`. Test baselines, duplicate adds,
  model-to-zero deltas, removal, slot reuse, frame copying, and map reset through the real client parser.
  `SV_BuildClientFrame` also transmits model-less entities carrying sound/events, so `U_REMOVE` is not the only way
  to lose a model. At `34a556f2`, a baseline `{number=7, model=1}`, followed by a packet delta to
  `{number=7, model=0, sound=1}`, leaves `num_active == 1`; a subsequent `U_REMOVE` still leaves that stale entry
  because removal is guarded by `old.model`. A focused wire-parser test reproduced both failures; the existing
  umbrella suite passes but does not exercise this lifecycle. Extend `tests/test_net.c` for regression coverage.
- `CL_BeginLoadingMap` in `games/warcraft-3/tests/test_client_stubs.c` does not mirror the new list reset;
  standalone parser tests using that stub do not validate production map-reset behavior.
- The WC3 splat implementation batches **contiguous texture runs**, not all occurrences of each distinct texture.
  `R_AddRectSplat` flushes at every texture change; `R_GenerateSplatTiles` also flushes at buffer capacity.
  Below capacity, A/A/B/B produces two draws, but A/B/A/B produces four. Entity order is not sorted by shadow
  texture and swap-removal changes it. Use both patterns when checking draw/upload counters; the distinct-texture
  claims above describe the optimization goal, not a guaranteed bound in this revision.
- The follow-up `10904293` restores the MDX particle size factor removed from `R_DrawParticles` in `97a52d18`.
  `ReadParticleEmitter` doubles all three `ParticleScaling` lifecycle values once; both MDX head and tail spawns
  consume those values. The shared renderer and M2 particle scaling are unchanged.

## Adaptive harvest interaction lanes (August 31, 2026)

An RG40xx-H weighted `perf report` after the blocked-footprint worker approach fix showed the new interaction helper dominating the server frame. The resolved game stack was `G_RunFrame -> G_RunEntities -> G_RunEntity -> monster_think`, with about 34% under `ai_goldmine_walkback -> gold_find_direct_footprint_approach` and another 11% under `ai_walkmine -> gold_find_direct_footprint_approach`. Both paths entered `CM_FindApproachPointToFootprintForRadius`, whose old candidate search called `CM_DistanceToPathingFootprint` for every candidate cell. Because that distance helper scans every authored footprint pixel, one edge selection became candidate-count times footprint-area work and accounted for roughly 45% of sampled cycles in that capture.

Caching one selected edge point for an entire resource leg was rejected after runtime testing. Packed Peasants are displaced by live-unit collision as they converge on a Town Hall; a point that was the nearest legal edge lane earlier in the leg can later lie behind another worker or across the blocked building footprint. Keeping that stale point made several returners repeatedly steer back toward the old lane and visibly dance instead of completing the deposit. Gold and lumber therefore re-select the nearest direct footprint edge from the worker's **current** position each think. The later 30-Peasant Human02 crowd simulation moved live-unit separation out of same-tree lane allocation entirely: tree approach keeps the closest direct legal chop point, while resource-worker local avoidance queues same-stream workers and uses deterministic bounded right-first passing only for crossing or persistently blocked traffic. This removes the previous full-edict same-tree slot/occupancy scans as well as their forced angular detours. See [worker-crowd-routing.md](worker-crowd-routing.md).

`CM_FindApproachPointToFootprintForRadius` now makes that adaptive selection cheap without changing its authored-footprint semantics. The pathmap owns a reusable one-byte-per-cell scratch mask. For each blocked footprint pixel, the helper marks only nearby pathmap cell centres whose exact grid-rectangle distance is within the requested range; it then scans those marked candidates once. This replaces the nested candidate-by-footprint distance scan with footprint-area times a small local range plus one candidate pass. Once a direct candidate has been found, farther candidates also skip redundant line-walkability traces. The mask is scratch state only and is cleared over the small search rectangle on each call.

Keep the single `CM_DistanceToPathingFootprint` call in the per-think interaction-range check: that authored-footprint distance decides exact mine/deposit completion. The optimized cost is the optional staging search, not the behavior-owned interaction boundary. `wc3_pathfinding.footprint_approach_respects_sparse_path_texture` protects irregular footprint accuracy, and `wc3_movement.gold_return_reselects_footprint_edge_after_displacement` protects the adaptive lane behavior that prevents the Town Hall dance.
