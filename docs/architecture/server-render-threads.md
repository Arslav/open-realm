# Server Simulation and Rendering Threads

## Contract

Native listen servers overlap one authoritative game tick with the current
client draw. `server-sim` owns `SV_RunGameFrame` / `ge->RunFrame` while the SDL
owner draws the client mirror. Entity callbacks remain ordered on one worker;
this does not parallelize individual units or change the 100 ms simulation step.
There is no runtime toggle. The experiment lives on `perf/server-render-threads`,
based on `9c37777c`; that commit retains the preceding synchronous implementation.

This is bounded frame overlap, with a join after each draw. The server cannot
continue across arbitrary client update/lifecycle calls. A slow tick can still
stall the frame barrier; this is not an independently paced server loop and does
not establish a 40/60 FPS result.

## Frame Ownership

`common/main.c:OpenRealm_Frame` runs these phases:

1. `Online_Frame`, then server transport on the owner. In an active world,
   `SV_PrepareFrame` processes incoming packets/external control and reserves a
   due tick without starting it. Loading/menu frames use synchronous `SV_Frame`.
2. `CL_UpdateFrame` on the owner: session actions, input, completed incoming
   snapshots, outgoing commands and resource registration. A lifecycle change
   may cancel the reservation; pause is rechecked before starting it.
3. `SV_StartPreparedFrame` posts the surviving job to `server-sim`, then
   `SCR_UpdateScreen` runs concurrently on the owner. GL/EGL,
   model workers, UI, sound playback and the SDL window retain their owner.
4. `SV_EndFrame` joins the tick while servicing its host requests, then sends
   complete snapshots through the existing per-recipient transport.

The next client update decodes those snapshots. Relative to the old
server-before-client loop, a new snapshot can wait one additional client
iteration before drawing. Input/transport ordering stays the same. No network
or save layouts change.

The first prototype updated the client before server transport. After long
resource registration, `CL_CheckTimeout` could then disconnect the client before
it received fresh server traffic; Honor reached a loading acknowledgement but
never `G_ClientBegin`. The regression test fails that phase order. Transport now
precedes the client update, and preparation after a gap of at least one keepalive
interval sends complete preceding state before the timeout check. A reserved
tick cancelled by a map change/shutdown never advances time or publishes a new
snapshot. The failed source/APK hash and capture remain retained as prototype 1.

Menus/loading and movies use the synchronous server entry; dedicated servers,
externally stepped batches and the non-pthread browser build also remain
synchronous. `CL_Frame` is still a complete synchronous client frame for tools
and existing acceptance callers. There is at most one posted job, so neither
simulation ticks nor render frames accumulate in an unbounded queue.

## Shared Services and Lifetimes

`SV_BindHostImports` captures the original typed imports and wraps client calls,
filesystem services, pause changes, immediate unicast and sound transport.
`SV_CallOnMain` uses a single mutex/condition-protected request slot. The worker
blocks with its arguments alive; `SV_JoinGameFrame` executes the request on the
owner and returns the result before releasing that stack. Callbacks such as
`ReadFileAll` therefore also execute while the worker is blocked. A bare join
without pumping requests would deadlock this contract.

Server-owned entity allocation, spatial queries, message authoring and asset
indices stay with the tick. Renderer inputs remain client-owned snapshots, never
live edicts. The client draw phase must not execute commands or mutate the cvar
registry: both renderer and game may borrow strings from it during overlap.
The game's pause setter is marshalled so its cvar update happens after drawing.
New imports must be classified before being exposed to the worker.

The shared sheet library can read files outside the game import table. Its SLK
scratch pools are protected for the parse/decode/copy interval. File I/O precedes
that lock. `FS_OpenFile`/`FS_CloseFile` and file enumeration use an atomic archive
transaction lock; layered reads release it before invoking a callback that may
open another file. Immutable INI caches keep their existing independent storage.

`SV_Map`, manual saving and `SV_Shutdown` drain pending work before touching the
world. Shutdown joins and destroys the persistent worker before game resources
are freed. `Com_Quit` shuts the server down before client shutdown destroys SDL.
Paused frames keep their existing packet processing/keepalive behavior. All
players still receive their own visibility, state and delta-compressed snapshots;
there is no local-player shortcut or LAN protocol change.

## Randomness

MDX and M2 particle emission uses renderer-owned `R_Random`, a xorshift32 stream
with a nonzero seed and a 24-bit `[0, 1)` float result. It never consumes libc
`rand`, which the game still uses. Presentation samples consequently differ from
the previous implementation; rendering more particles no longer advances the
game's random sequence. Particle preparation and submission retain their
existing owner/worker ordering. `R_SeedRandom` supports repeatable render tests.

## Diagnostics and Verification

With `com_profile 1`, `[SV_THREAD]` reports job count, job wall time,
worker wait for host calls, owner barrier time and host-call count. Job wall
time includes host waits; these overlapping columns must not be summed or
reported as CPU utilization. The diagnostic interval starts at the first
completed job and includes both endpoint jobs; use per-job costs rather than
inferring the exact simulation frequency from `jobs / elapsed_ms`. `[COM_FRAME] server_ms` now measures owner-side
server preparation plus the barrier/snapshot phase, excluding concurrent game
work. Its values are not directly comparable with older serial server totals.

Automated coverage includes:

- `renderer_frame.engine_scheduler_polls_transport_before_client_then_overlaps_tick_with_draw`:
  actual outer-loop phase order, including the loading path.
- `server_net.threaded_tick_overlaps_owner_and_marshals_host_calls_before_shutdown`:
  different thread IDs, typed imports with borrowed stack arguments, repeated
  jobs, pause/resume, teardown during a job and worker restart.
- `server_net.threaded_tick_publishes_completed_snapshots_to_local_and_remote_players`:
  no premature snapshots, completed entity/player state for loopback and IP
  recipients, and no duplicate publication on a second join.
- `server_net.prepared_tick_services_long_loading_gap_and_cancels_on_pause_or_shutdown`:
  a 60-second gap receives a keepalive without advancing the reserved tick;
  pause/lifecycle cancellation and synchronous completion preserve the clock.
- `commands.concurrent_archive_reads_layer_callbacks_and_slk_decodes_keep_private_results`:
  two concurrent resource readers, reentrant layered callbacks and separate
  typed SLK results.
- MDX packet regressions compare particle positions, velocities and renderer
  stream progression across modes, and verify that emission leaves the gameplay
  RNG unchanged.

Run `make test` with the local SDL dependency prefix when required. The initial
full suite passes 7,244 executions / 36 JUnit reports plus the ANGLE Python tests.
Native Android compilation passes. ThreadSanitizer cannot start in this local
environment: even an empty `main` exits with SIGSEGV before test output. This is
a sanitizer coverage gap, not evidence that the implementation is race-free.
ASan/UBSan combined also fails before test output; an empty `main` times out
after 10 seconds. Neither sanitizer run establishes a passing result.

On-device behavior and timing belong in
[WC3 performance](../games/warcraft-3/performance.md); the SDL/ANGLE render path
cannot be validated by the headless scheduling tests alone.
