# Missing Campaign AI, Alarm and Model-Cinematic Natives

## Scope and source contracts

This change implements the Warcraft III `common.j` natives `UnitIgnoreAlarm`,
`UnitIgnoreAlarmToggled` and `PlayModelCinematic`, plus the `common.ai` entry
points `SetAmphibious`, `DisablePathing`, `TeleportCaptain`, and the adjacent
attack-captain movement/query functions. All are registered in the shared
`api_module.c` table; each computer player still has its own AI VM.

The initial goal is to let campaign scripts, including UndeadX08, progress
past missing natives and exhibit sensible behaviour. **`BZ_COMPAT_GUESS`**
comments identify the places where Blizzard's internal engine behaviour is
not documented or where original-game experiments remain necessary. These are
working compatibility approximations, not proven retail algorithms.

## Unit alarms

- `UnitIgnoreAlarm(unit, true)` suppresses the victim unit's automatic
  under-attack notification path (`G_WC3_AttackAlert`), including owner and
  help-request ally messages/pings. It does not change combat or retaliation.
- `UnitIgnoreAlarmToggled` reports the flag, defaulting to false.
- **BZ_COMPAT_GUESS:** the native returns `true` for a valid unit and `false`
  for an invalid unit; retail return-value and exact per-notification scope
  require direct comparison. Normal C edict values survive game saves; no
  persistent pointer or network entity field was added.

## Campaign AI navigation and captains

- `SetAmphibious()` marks the calling AI player's policy (never a global
  switch). Existing movement/pathing in `naval-movement.md` continues to
  route each unit using its authored `movetp`: amphibious Naga can cross water
  and ground-only units cannot. On explicit captain point orders, enabling
  this AI policy performs a **per-member closest-reachable waypoint preflight**
  using that unit's actual movement mask, rather than forcing all members to
  share one land-oriented route. Each order stores the actual waypoint sent
  to each living member; `CaptainAtGoal` checks these reachable waypoints
  instead of waiting for a land unit to cross inaccessible water. The captain's
  authored common goal is preserved. **BZ_COMPAT_GUESS:** partial arrival means
  a unit reached its reachable bank, NOT that it crossed water; if no route can
  be found the member's current coordinate is used. This prevents hung scripts
  but is not Blizzard's undocumented group-level routing algorithm.
- `DisablePathing()` switches off the optional AI preflight when it is active;
  it never disables collision or static pathing enforced by the movement
  system. This separation is intentional; exact retail policy is unknown.
- `TeleportCaptain(x,y)` updates the calling player's attack captain's
  logical position; no soldier is teleported, and home and goal are preserved.
- `CaptainAttack`, `CaptainGoHome`, `CaptainVsUnits`, and `CaptainVsPlayer`
  use normal attack-move/move/target orders for existing assault members.
  Candidates require current fog visibility (`G_FowPlayerCanHoverEntity`),
  including for buildings which otherwise persist in explored fog. The `VsUnits`
  variant excludes structures; `VsPlayer` includes units and buildings. A
  single shared visible target is selected per captain order; member selection
  order cannot silently overwrite the group objective. If nothing eligible is visible, neither invents a target by
  falling back to an unrelated stage waypoint. **BZ_COMPAT_GUESS:** the
  retail targeting preference between these two natives is not fully known.
- `CaptainAtGoal` and `CaptainIsHome` use the captain's explicitly teleported
  logical coordinate when set, even if living members still occupy their old
  locations. A later attack/go-home order clears the override and queries
  compare living members with the requested goal/home. **BZ_COMPAT_GUESS:** their tolerance uses the existing group-flee
  home radius; retail captain arrival and in-flight teleport effects are
  unverified. `ResetCaptainLocs` and `ClearCaptainTargets` are separate
  operations and retain the group roster. `ClearCaptainTargets` clears logical
  targets/routes but preserves soldiers' already-issued orders and does not
  falsely mark an actively fighting captain idle (BZ_COMPAT_GUESS).

## Full-screen model cinematics

- Numbered campaign entries are not necessarily maps. Russian TFT 1.26a `UI/CampaignStrings_exp.txt`
  authors Undead `Mission13` as `"Финал","Время настало...","Doodads\Cinematic\ArthasIllidanFight\ArthasIllidanFight.mdl"`.
  The old menu sent every numbered entry to `map`, which rejected this model as a map archive.
  `single_player.c` now classifies `.mdl`/`.mdx` paths case-insensitively and calls the typed
  `menuImport.QueueModelCinematic` service. These rows use camera-button art, keep their authored
  mission index/progress policy, and remain available without FFmpeg, including browser builds.
  Ordinary map entries retain the map launch path. In decoder-enabled builds, the last numbered
  model entry queues the same campaign's `EndCinematic`/`EdCinematic` movie after the model.
  Earlier model entries and campaigns without an ending movie remain standalone. EOF/skip of
  the model advances into that movie. Ending EOF/skip then loads the edition's authored
  credits map; it does not expose the campaign menu between the clips or before loading.
- `PlayModelCinematic(modelName)` passes the **authored asset name** from JASS
  to the universal client. The normal game module does not decode or render
  the model; the renderer already resolves `.mdl` to `.mdx`.
- The client queues the model cinematic even if there is no following session
  action, pauses the outgoing simulation, and runs a full-screen isolated MDX
  view. The optional FFmpeg AVI/movie decoder is not required.
- A model sequence timer advances through the MDX sequences and selects the
  matching embedded camera. Playback ends after the last sequence or Escape.
  No sequence loops forever. MDX event-sound/particle handling uses the
  existing renderer; model instance presentation state is released on stop.
  A model scene suspends gameplay music only if it was not already suspended,
  and restores that suspension on natural completion, Escape and disconnect.
  Existing effect sounds stop before the first sequence starts. The isolated
  scene owns effect audio until EOF, skip or cancellation, which stop its sounds
  before resuming the prior music session.
- Sequence audio is authored separately from MDX events: `UI/MiscData.txt`
  `[<MODL name>Sounds] SceneN` names an `UI/SoundInfo/AnimSounds.slk` row.
  `refExport.StartModelCinematicSequence` lets the game renderer resolve this
  binding and use the ordinary non-positional sound path at the row's volume.
  The generic client only announces actual sequence changes; an unbound next
  sequence does not restart or cut an earlier track. Both quoted and bare
  labels are accepted. Missing authored rows/files produce diagnostics.
  Russian TFT 1.26a binds `[ArthasIllidanFightSounds] Scene1="FinalCinematic"`
  to `Sound\Ambient\DoodadEffects\FinalCinematic.mp3`, volume 127. The MDX
  itself has MRF/MRD events but **no SND events**, explaining the previously
  silent playback. Audio starts with sequence 1 after sequence 0's five-second
  black opening; neither the soundtrack path nor the model name is hardcoded
  into the client. Scene audio uses sound-effect preferences, not music preferences.
- Model scenes may start without a pending map/menu transition. `PlayCinematic`
  movie files wait for a session transition so a call made during `Next_Level_Prep`
  cannot interrupt the rest of the map's scripted ending. When both types are
  queued, model scenes play before unlock movies, preserving FIFO order within
  each type. This handles UndeadX08's `OutroX` unlock followed by its
  `ArthasIllidanFight.mdl` ending scene. A bounded queue holds up to eight
  requests; overflow reports a diagnostic and rejects the new request while
  preserving prior items. The session transition runs after all eligible scenes
  finish or are skipped.
- Frontend finale replay uses `menuImport.PlayMovieBeforeMap`: the client frame scheduler
  retains the credits-map transition across both model and movie playback. Script
  `QueueMovie` requests still wait for a map/menu transition; standalone model playback
  does not release an unrelated movie. Cancellation clears the whole playlist and its
  deferred transition. In the original Russian TFT 1.26a data, Undead's
  final model is followed by `EndCinematic` `OutroX` ("Восхождение"). Completing
  `UndeadX08.w3x` separately queues `OutroX` through `SetCinematicAvailableBJ`, then
  `PlayModelCinematic`, and continues toward `War3XBonusCredits.w3x` through the
  victory/next-level path. `Trig_Next_Level_Prep_Actions` authors that next-level path;
  `Trig_Next_Level_Run_Actions` requests the model, then calls `CustomVictoryBJ`.
- Finale replay from the menu (the last numbered model plus its ending, or an edition's
  final ending row selected directly) continues to the **post-campaign bonus credits**:
  `Maps\Campaign\BonusCredits.w3m` (RoC) or
  `Maps\FrozenThrone\Campaign\War3XBonusCredits.w3x` (TFT). These are the exact
  `SetNextLevelBJ` destinations in NightElf07 / UndeadX08, recorded as stock edition
  routing constants in the WC3 menu because CampaignStrings has no continuation field.
  Replay does not execute a finished mission's victory/progress/cache writes merely
  to discover its next map. `PlayMovieBeforeMap` preserves the map across both clips;
  the client executes it after EOF/skip. To support RoC's finale without a numbered model, the final ending
  is the last campaign in the authored `CampaignList` order with a non-empty ending
  movie. Later bonus campaigns without an ending do not consume that position.
  This identifies RoC NightElf and TFT Undead from metadata; movie/faction names
  are not matched. Earlier campaign movie rows remain independent.
  **BZ_COMPAT_GUESS:** the order-based finale classification is a compatibility policy:
  CampaignStrings authors selector order, not an explicit story-finale flag.
- **Do not use `UI/MiscUI.txt` `[Misc] CreditsFile[_V0/_V1]` for campaign endings.**
  Those keys select the separate regular credits: `maps\campaign\warcraftiiicredits.w3m`
  (RoC) / `maps\frozenthrone\campaign\war3xregularcreditsice.w3x` (TFT).
  Using them for finale replay was a confirmed routing mistake in both editions.
  All four credits assets are scripted maps, not movie files. Bonus maps finish through
  `CustomVictoryBJ` without another next level. RoC's bonus map also reads the stored
  `Credits/VictoryHard` flag for its extra StarCraft scene; replay does not fabricate it.
  The project's current `menu_credits` dialog separately lists OpenWarcraft contributors;
  it is not either of the original game's credits maps. To inspect the source:
  `build/bin/mpqtool -data <game-dir> grep CreditsFile UI` and extract `war3map.j`
  from the campaign maps with `mpqtool -mpq <extracted-map> cat war3map.j`.
- RoC 1.26a's `NightElf07.w3m` `Trig_Next_Level_Run_Actions` calls
  `SetCinematicAvailableBJ(true, bj_CINEMATICINDEX_NED)` (`NightElfEd`, "Конец вечности"),
  `SetNextLevelBJ("Maps\\Campaign\\BonusCredits.w3m")`, then `CustomVictoryBJ`.
  It has no `PlayModelCinematic` step. The original mission route already retains
  that next-level action through the movie. Frontend replay selects the same bonus map
  without executing those victory/cache writes; previously its model-only classification
  returned directly to the menu.
- Disconnect and shutdown cancel active playback, clear deferred transitions
  and restore the session's prior pause state. A missing/unloadable asset is
  skipped so it cannot permanently suppress a pending victory or map change.
- **BZ_COMPAT_GUESS:** generic playback starts at sequence 0; Warsmash's
  Arthas/Illidan demo starts at 1. Sequence-to-camera index mapping, order,
  total playback duration and JASS's blocking semantics require retail
  validation. This implementation defers session transitions rather than
  suspending the triggering JASS coroutine.
- **Known visual gap:** Warcraft's MRF/MRD animated cape mesh events are not
  implemented by the current MDX renderer. The cinematic model still renders
  and advances its scenes, but the cape morph animation is incomplete.
  Exact retail sequence timing and visual parity still require comparison;
  authored sequence sound bindings are now consumed without per-model exceptions.

## Save/load and limitations

The current save format is version 76. It persists `edict_t.ignore_alarm` and
texttag presentation identity; the round-trip suites cover both contracts, and
older version 75 files are rejected according to `CONTRIBUTING.md`.

**Existing limitation (not solved by this change):** the WC3 save serializer
explicitly does **not** snapshot AI JASS VM roots, pending AI coroutines,
rosters or bot runtime (`save-load.md`). That means captain/planning settings
cannot be accurately resumed from a saved mid-battle AI script yet. This
change deliberately does not pretend that copying bot flags into `level_fields`
would provide AI script restoration. A complete bot save feature must serialize
or reconstruct each bot's VM and outstanding work alongside its captain data.

## Verification

The game module tests cover alert suppression, alarm save round-trips,
captain logical teleport with empty and populated rosters, authored movement
flags under AI policy changes, and JASS registration/argument forwarding.
Client tests cover FIFO order, bounded overflow, and standalone playback through
`CL_UpdateFrame`, including Escape and a missing asset without losing the frontend.
With FFmpeg enabled, that regression also advances from a model into a real
fixture AVI through the frame scheduler, checks decoded PCM, and verifies cancellation
does not leave the ending queued. The requested map remains deferred through the model
and AVI; cancellation at the final boundary avoids world/GL initialization in this test.
Menu tests drive the real camera-button commands with non-stock model paths in
both the legacy RoC `MissionN`/`FileN` and TFT three-column `MissionN` schemas;
they also check that map entries still launch maps. Non-stock movie/model paths cover
both editions' exact bonus-credit destinations, direct ending replay and unrelated
campaign endings. Conflicting, empty and absent `UI/MiscUI.txt` fixtures must not be read
or change this routing. A RoC-shaped map-plus-ending
fixture (without a model) checks final credits; the same campaign's earlier position
in the TFT list checks independent replay with a different movie name. Full retail scene rendering
remains an interactive verification gap. The
cinematic JASS test verifies the original MDL asset path crosses the game/
client callback without translating it into a movie path.

`renderer_game.cinematic_scene_sound_uses_misc_model_name_and_animsounds` reads
the fixture MPQ's real INI/SLK parser path, covering the stock binding plus a
different model/label/path/volume, absent bindings and unresolved rows.
`client_movie.model_sequence_audio_survives_scene_changes_and_stops_on_skip_eof_cancel`
checks actual mixer PCM, first-sequence ordering, continuity and all three teardown
paths. The client-frame regression also checks delivery of the sequence hook.
The menu import and renderer export tables changed; rebuild client, menu and
renderer modules together. Network and save layouts are unchanged.

Run `make test` for both editions and the menu suite; `make FFMPEG=1 test-menu`
also covers model rows beside decoder-enabled movie rows. The focused scheduler
regression is `client_session.standalone_model_plays_on_client_frame_and_skip_keeps_frontend`.

Runtime acceptance with retail assets:

1. Start UndeadX08 with each named AI player; verify that the missing-native
   failures are gone and waves/defence continue. Inspect later errors too.
2. Attempt Naga water crossings and mixed ground/water attacks; ground-only
   units must not gain water access even after `SetAmphibious`.
3. Teleport the attack captain during formation and while attacking; verify
   membership and real positions are preserved, then observe arrival queries.
4. Finish UndeadX08 and check full-screen Arthas–Illidan playback, camera
   changes, sounds, Escape, and subsequent ending transition; repeat with
   `FFMPEG=0` and `FFMPEG=1` builds.
5. Compare the exact starting sequence, total length, soundtrack and blocking
   behaviour with an actual retail Warcraft III executable. MRF cape morphs
   are deliberately listed as a known outstanding renderer feature.

Follow [AGENTS.md](https://github.com/corepunch/open-realm/blob/main/AGENTS.md) and [CONTRIBUTING.md](https://github.com/corepunch/open-realm/blob/main/CONTRIBUTING.md)
for the normal build and test process.
