# Tablet Touch Controls

## Contract

The first tablet profile keeps the authored Warcraft III HUD and changes direct-touch gameplay input.
The shared client recognizes gestures and sends pick results; `CMD_Touch` in `game/g_commands.c` owns
selection, Smart orders, and active-command precedence. Mouse bindings remain independent.

| Input | Result |
|---|---|
| Tap a controlled unit/building | Select it |
| Tap a foreign unit without a controllable selection | Select it for inspection |
| Tap a foreign object with a controllable selection | Existing Smart target order (attack/harvest/etc.) |
| Tap ground with a controllable selection | Existing formation-aware Smart point order |
| Tap while a command is targeting | Supply that command's entity or ground target |
| Drag one finger | Existing rectangle selection |
| Drag one finger into a world-view edge and hold | Continuous camera scroll; that contact no longer selects or issues orders |
| Drag two fingers | Terrain-anchored camera pan |
| Pinch two fingers | Proportional camera zoom, clamped by the existing game/map camera policy |
| Hold an object/button | Existing world hover or HUD tooltip; release does not activate it |
| Tap/drag the minimap | Existing camera navigation |

Controlled friendly targets have selection priority. Repair, boarding and other orders targeting those
objects use their explicit command buttons. A point-only command retains the traced ground position even
when an entity was also hit; building-on-mine placement retains its entity-specific selection path.
The minimap signal overlay also takes precedence over ordinary context orders.

## Input And Command Flow

SDL direct-touch contacts continue to synthesize the ordinary left-pointer input for HUD and rectangle
selection. A short world release replaces the ordinary `select`/`point` message with:

```text
clc_stringcmd: touch <entity-or-0> <ground-hit-0-or-1> <world-x> <world-y>
```

The client does not predict a selection change for this tap: the game may accept it as a command target
or Smart order. The existing `svc_set_selection` acknowledgement reconciles actual selection changes.
The game validates the entity, hit flag and finite coordinates, then reuses `CMD_Select`, `CMD_Point`,
`CMD_Smart` or `CMD_SmartPoint`. No player/entity snapshot fields or message framing change.

`cl_touch_command` is empty by default in the shared client; Warcraft III's shipped config sets it to
`touch`. Set it to an empty string to restore ordinary synthesized left-click selection. Other games
can opt in with their own command, using the same pick-result argument shape.
`cl_touch_hold_ms` defaults to 500 milliseconds. Movement uses `cl_click_threshold` (10 logical window
pixels by default); crossing it commits the contact to a drag even if it later returns to its start.

The second finger cancels the pending world selection and HUD press. Two-finger gestures start panning
only when both contacts are outside gameplay UI. Motion is coalesced once per input pump, including pinch
distance changes, so two SDL finger-motion events cannot apply one camera offset twice. Idle touch-driven
pointers never edge-scroll. Indirect trackpad contacts keep their existing mouse/wheel behavior.
Motion queued immediately before both fingers lift is flushed before ending the gesture.

### Two-finger camera intent

Pan and pinch are exclusive for each pair of contacts. `touch_pan.mode` begins as `TOUCH_CAMERA_PENDING`:
small initial jitter changes neither camera position nor zoom. Midpoint travel reaching
`cl_click_threshold` claims `TOUCH_CAMERA_PAN`. Pinch claims `TOUCH_CAMERA_ZOOM` only when the span change
reaches both the click threshold and 8% of the initial span, and is at least 1.5 times midpoint travel.
This also permits pinching with one anchored finger. While pending, compare against the initial contacts
so gradual motion can accumulate across input pumps.

Once claimed, pan ignores all spacing changes and pinch ignores all midpoint translation. Release the
pair and begin another gesture to change intent. A new second contact resets recognition. The existing
coalescing and final-motion flush apply to either mode. A game without `cl_touch_command` keeps its
original two-finger pan path without pinch recognition.

Previously `CL_FlushTouchMotion` applied every span change as zoom and always flushed the pending pan.
That coupled natural finger-spacing noise to zoom during dragging and coupled pinch drift to map motion.
The shared client now makes only this generic input decision; the game's camera limits remain authoritative.

### Pan sampling across input and render frames

`camera_drag.point` retains the previous screen-space midpoint (also used by mouse `+pan`). Each pan update
traces both that point and the new midpoint against the same current rendered projection, then adds only
their world-space difference to the latest predicted camera position. Unchanged contacts do not send a move.
The pointer baseline advances even if ground tracing misses or the game clamps the camera at a map edge;
resuming valid input or reversing direction therefore does not replay skipped travel.

The old fixed world anchor mixed the last rendered projection with an already updated predicted origin.
Coalescing finger events within one input pump did not cover separate pumps sharing that projection, or
changes to the rendered camera/terrain base between samples. Split contact events could reapply prior motion
and produce visible camera jitter. Tests retain a fixed rendered projection across several real SDL input
pumps, then advance it independently of prediction; each new pixel must contribute exactly once.

### One-finger edge scroll

`cl_touch_edge_margin` is a fraction of the shorter world-viewport dimension (Warcraft III: `0.06`,
shared default: `0`, disabled; capped at `0.25` to prevent opposing edge bands overlapping).
The existing `cl_camera_edge_scroll` enables scrolling and `cl_camera_scroll_speed` controls travel speed.
The viewport's normalized, bottom-origin rectangle is converted to logical screen pixels, so the lower
edge is above the command console rather than at the physical bottom of the screen.

Only a contact that started world selection and crossed `cl_click_threshold` can enter `TOUCH_SCROLL`.
Taps, holds, minimap navigation and contacts originating on HUD controls do not enter it. On entering
the edge band the client cancels the rectangle and pending HUD activation, then retains camera ownership
until release. The held finger can continue over the console without clicking it; returning to the center
stops movement but cannot turn that same contact back into selection. Corners scroll diagonally.
Focus/modal ownership changes cancel the contact; a second finger takes over through the existing pan path.

`CL_MouseScroll` supplies the direction for the camera and authored scroll cursor. `CL_ScrollFrame` uses
the input pump's existing elapsed interval, capped at 100 ms, rather than a second hidden wall clock.
This keeps held-scroll tests deterministic without sleeps and preserves yaw-relative arrow/mouse movement.

Selection priority requires a selectable controlled unit, not ownership alone: environmental objects
such as trees may carry player zero. Treating that as control would swallow the worker's harvest tap.

`SCR_LayoutCancelPress` clears pending HUD activation while preserving hover and releases any held
`+command`. Cancellation is explicit: fabricating a release outside the window is unsafe before canvas
initialization, when all screen positions can map to UI origin. Focus loss and modal/gameplay ownership
changes cancel the contact, so its later release cannot issue an order.

## Verification

```sh
make -j4 openwarcraft3-tests test-assets
build/bin/openwarcraft3-tests -data build/tests +dedicated 1 +test 'client_touch.*'
build/bin/openwarcraft3-tests -data build/tests +dedicated 1 +test 'wc3_touch.*'
build/bin/openwarcraft3-tests -data build/tests -tft +dedicated 1 +test 'wc3_touch.*'
make test
make android-warcraft3
```

On the local macOS setup with SDL built under `build/android`, use
`make HOMEBREW_PREFIX="$PWD/build/android/sdl-host" ...` and run native binaries with
`DYLD_LIBRARY_PATH="$PWD/build/android/sdl-host/lib"`.

Tests exercise SDL event ordering, release-only dispatch, rectangle selection, long holds, HUD activation,
focus loss, exclusive pan/pinch intent, jitter rejection, gradual and anchored pinch, zoom limits,
final-motion release and gesture rearming, split-event pan sampling across render updates, missed ground
and clamped-pan reversal, held edge scrolling and release, viewport corners,
HUD exclusion, disabled edge bands, game-side selection/Smart/target-mode dispatch,
worker harvesting, and Holy Light healing with the hero remaining selected. Physical finger
precision, tooltip occlusion, and comfortable HUD target sizes still need a real tablet session.
Touch-specific HUD geometry and visible control-group slots are follow-up design work.

See also [shared input](../../architecture/shared-input.md), [selection and control](selection-and-control.md),
[camera zoom](camera-zoom.md), and [Android packaging](../../android.md).
