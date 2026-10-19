# Warcraft III Fog And Cinematic Visibility

> This document covers gameplay fog of war (exploration/current sight). Atmospheric distance mist from `SetTerrainFogEx` is a separate renderer/environment system; see [Environmental Terrain Fog](environmental-fog.md).

## Contract

Camera position, fog state, and entity visibility are independent systems. Camera natives move the gameplay camera without changing
fog, while fog natives can reveal or mask remote terrain without moving the camera.

OpenRealm represents Warcraft's three fog states with the existing per-player `explored` and `visible` planes:

| JASS state | `explored` | `visible` | Meaning |
| --- | ---: | ---: | --- |
| `FOG_OF_WAR_MASKED` | 0 | 0 | unexplored |
| `FOG_OF_WAR_FOGGED` | 1 | 0 | explored without current sight |
| `FOG_OF_WAR_VISIBLE` | 1 | 1 | explored with current sight |

`G_FowClearVisible()` clears current visibility without clearing exploration. A temporary visible reveal therefore naturally becomes
fogged after the modifier stops when no normal sight source still covers the area.

## Direct Trigger Reveals

`SetFogStateRect`, `SetFogStateRadius`, and `SetFogStateRadiusLoc` write directly into the authoritative player fog grid through
`G_FowSetStateRect()` and `G_FowSetStateRadius()`.

- `MASKED` clears `visible` and `explored`.
- `FOGGED` clears `visible` and sets `explored`.
- `VISIBLE` sets both planes.

The operations reuse the existing server fog-grid resolution and dirty-row network path. They do not maintain a second cinematic
exploration map. With `useSharedVision`, the same state is also applied to viewers receiving the source player's shared vision.

## Per-Unit Shared Vision

`UnitShareVision(unit, player, true)` adds that player to the unit's persistent `shared_vision` recipient mask. During
`G_FowUpdate()`, the unit's ordinary day/night sight is rasterized for the union of its normal owner/alliance viewers and
its explicit per-unit recipients. Passing `false` removes only that explicit relationship. This is deliberately separate
from `ALLIANCE_SHARED_VISION`: sharing one unit does not expose the owner's other units or change alliance state.

The relationship is saved with the unit, follows the unit as it moves, and is idempotent because it is represented as a
player bit rather than a reference count. Removing the unit naturally removes the sight source. `UnitShareVision` does not
create a fog modifier, force camera movement, or grant true sight/detection; entity invisibility continues through the
existing per-viewer visibility policy. The NightElf06 `UnitShareVisionBJ(true, gg_unit_Utic_0055, udg_Player)` campaign
reveal therefore works through the generic sight pipeline rather than a Tichondrius-specific exception.

## Fog Modifiers

`CreateFogModifierRect`, `CreateFogModifierRadius`, and `CreateFogModifierRadiusLoc` create disabled modifier handles.
`FogModifierStart` applies the modifier once immediately and then makes it participate in `G_FowUpdate()`; `FogModifierStop` removes it. The synchronous first application is required for map scripts that start and stop/destroy a `VISIBLE` modifier in the same trigger turn to permanently explore an area without holding current sight open.

Started modifiers apply the same three-state cell writer after normal unit sight:

- `VISIBLE` keeps terrain explored and currently visible;
- `FOGGED` keeps terrain explored while suppressing current sight in the modifier area;
- `MASKED` keeps the modifier area unexplored while active.

Stopping a `VISIBLE` modifier stops forcing current vision but does not erase the exploration it already created, so the normal
`VISIBLE -> FOGGED` transition is preserved.

## Camera And Consumers

Camera movement does not call the fog API, and fog state changes do not call camera APIs. Maps that want a cinematic pan and reveal must
request both actions explicitly.

The server's existing `G_FowPlayerCanSeeEntity()` remains the foreign-entity visibility gate. Known gameplay invisibility is evaluated there per viewer: owners/shared-vision allies keep access to their invisible units, while hostile viewers need a detector covering the target. Permanent Invisibility (`Apiv`) and Shadow Meld (`Ashm`/`Ahid`) have dedicated saved state; Sorceress Invisibility/Wind Walk and hidden wards retain `RF_HIDDEN`, but only those recognized invisibility states participate in this visibility policy. Snapshot customization clears `RF_HIDDEN` only for a viewer who is allowed to see that invisible entity, leaving the authoritative entity flag untouched. This deliberately avoids treating cargo, mine workers, revival/training placeholders, and other non-invisibility uses of `RF_HIDDEN` as revealable. The same viewer-specific predicate gates selection, automatic hostile acquisition, attack continuation, and unit-target spell validation. Far Sight's saved timed thinker and passive detector abilities contribute true sight through the shared query.

The client receives current and explored fog as separate planes, so world fog and minimap fog consume the same authoritative state while the minimap camera box continues to derive from camera state independently.

The WC3 model renderer applies gameplay fog after the shaded/unshaded material branch. This is required for unshaded doodad and model layers: their lighting bypass must not bypass fog of war. A zero fog texture sample discards the fragment, hiding it in unexplored terrain; nonzero samples attenuate its RGB according to the fog texture. Entities authored with `RF_NO_FOGOFWAR` use the renderer's white fog texture and remain unaffected by this mask. The fog sampler is declared third in the model shader, so the shader loader assigns it texture unit 2, where the world frame binds the fog texture; no explicit sampler assignment is needed.

Day/night sight-radius selection consumes the server-owned clock documented in [time-of-day.md](time-of-day.md); fog must not derive a
second clock from `level.time`.

## Exact CPU rasterization and blocker queries

Normal circular reveals and script radius writes operate on clipped horizontal
spans. Row bookkeeping happens once per span, and already filled visible/explored
spans can return immediately. The three fog states, dirty-row transitions,
inclusive circle edge, exploration retention and temporary nonzero visibility
markers keep their previous semantics. The optional packed-mask experiment
retains its own cell writer; this optimization does not enable that experiment.

The first blocker-box query in each `G_FowUpdate` builds an integer summed-area
table from the current blocker byte grid. Subsequent queries use four entries
to answer the same inclusive/clipped rectangle question, then keep the existing
disk or exact shadowcast/rim path. The table is invalidated on every update,
including updates without a blocker rebuild, so a changed bitmap cannot reuse
a stale answer. It is transient scratch, allocated lazily, grown to demand and
released by `G_FowShutdown`; no network or save field changes. Allocation failure
retains the exact byte scan and retries on the next update. Storage is
`4 * (width+1) * (height+1)` bytes (about 258 KiB for a 256×256 fog grid).

`wc3_game.fow_disk_spans_preserve_each_state_dirty_rows_and_idempotence` checks
all cells/dirty rows for all three states, repeated writes and nonzero markers.
Blocker query tests cover clipping, empty rectangles, bitmap changes, blocker
removal, map reinitialization and allocation failure/recovery. The original
work-bound regressions failed before the optimization. Both ROC/TFT focused
suites pass; Honor FPS validation is still required after reconnecting the device.

## Exact sight footprint cache

`wc3_fow_cache 1` (Android default; other platforms default to `0`) reuses the exact shadowcast footprint for an identical source
cell and integer sight radius. It caches the cells emitted by the existing
recursive caster, then reapplies their horizontal spans through the ordinary
visible/explored writer. It does not approximate occlusion or change fog-grid
resolution, simulation cadence, day/night radii or visibility policy.

The tree-rim pass deliberately remains outside the cache and runs for every
source in the original order against the live viewer grid. Rim expansion can
read visibility contributed by earlier sources, so caching a complete final
per-unit reveal or combining independently expanded masks would change behavior.
Far Sight and started script modifiers still run after ordinary sight.

Storage is 512 direct-mapped entries with at most 63 uint64 rows (about 260 KiB),
plus one byte per map fog cell for an exact blocker snapshot. Radii above 31 fog
cells use the original caster. The full blocker bitmap is compared once per
update; any byte change invalidates every footprint, including direct edits
without an entity-blocker rebuild. Entries are also cleared at map shutdown and
initialization. Allocation failure emits a diagnostic and keeps ordinary casts
until the next map initialization. None of this scratch is serialized.

`wc3_fow_stats 1` reports `[FOW_CACHE]` update/hit/miss/replacement/bypass totals once per
simulation second. The recursive-work test exercises cache consumption through
`globals.RunFrame`. Differential tests compare both byte planes, occupied rows
and dirty rows for three viewers over changing obstacles, movement, sight
radii, source death and all three script fog states. Separate tests cover
switching off/on, map reset, large-radius bypass and allocation failure.

## Known Gaps

This change intentionally does not alter unrelated compatibility areas that need broader evidence:

- `FogEnable` / `FogMaskEnable` retain their existing global behavior.
- Ghost/Ghost Visible are not yet normalized into the player-aware invisibility path. Shadow Meld now uses dedicated unit state with the same owner/shared-vision/detector visibility query as Permanent Invisibility, Sorceress Invisibility, Wind Walk, and explicitly hidden ward mechanics.
- `FOW_CELL_SIZE` is unchanged.
- Camera Z-offset native parity is separate from fog-state handling.

## Verification

The WC3 API tests cover:

- all three direct fog states through rect/radius/radius-location natives;
- `useSharedVision` propagation;
- per-unit `UnitShareVision` recipient isolation, revocation, idempotence, and save persistence;
- a same-turn `VISIBLE` start/stop still recording exploration;
- a temporary `VISIBLE` modifier falling back to `FOGGED`;
- active `FOGGED` and `MASKED` modifiers;
- Far Sight true sight and player-local Permanent Invisibility visibility are covered by the spell/ward regression suite.

Runtime campaign validation should additionally check that a camera-only pan into unexplored terrain remains masked and that a
cinematic reveal can show a remote area without coupling camera movement to fog mutation.

The headless renderer model suite also checks that the generated fragment shader applies fog after the unshaded lighting branch,
discards zero fog samples, and that the fog sampler resolves to texture unit 2 for both regular and instanced model programs.
