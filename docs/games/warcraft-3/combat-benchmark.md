# Mixed-army Android performance scene

Current measurement policy: the owner requested **100% native render resolution**
for subsequent Honor captures on October 18. Use `r_render_scale 100`, verify
the actual drawable dimensions (1920x1200 on this Honor) and confirm that no
scaled render target is active. After source merges, verify the measured APK's
native modules against a build of the current tree and resolve CPU samples
using that APK's symbols. Historical 67% captures below keep their original
settings and are not controls for a different revision at 100%.

The ANGLE/Vulkan trial (`$OPENREALM_EXPERIMENTS_DIR/warcraft-3/optimization-archive/vulkan-backend-20261018/README.md`, external archive)
retains the current 100% GLES/Vulkan comparison, artifact checks and exact package
identity. Backend availability alone is not evidence of a performance gain.

The two-model allied benchmark does not represent TFT campaign performance.
On the same Honor and ordinary APK, the opening playable view of
`Maps/FrozenThrone/Campaign/NightElfX01.w3x` averaged **19.40 FPS**, and the
mixed-army battle below averaged **14.19 FPS**. The earlier 43.16 FPS result
applies only to the fixed 100-Footman / 100-Grunt movement scene.

## Build the diagnostic maps

`tools/android/make_combat_bench.py` extends a supplied, unmodified
Battle Simulator v0.19d. Its JASS source is `tools/android/combat_bench.j`.
The downloaded map and retail assets remain local artifacts; neither is
distributed with the tool. See [benchmark map candidates](performance.md#gameplay-benchmark-map-candidates)
for the source-map investigation.

```sh
python3 tools/android/make_combat_bench.py \
  --base build/android/optimization/maps/BattleSimulatorV0.19d.w3x \
  --output build/android/optimization/honor-combat/CombatBench200.w3x
python3 tools/android/make_combat_bench.py \
  --base build/android/optimization/maps/BattleSimulatorV0.19d.w3x \
  --output build/android/optimization/honor-combat/CombatBenchDemo.w3x --demo
```

The builder retains all 17 original members and byte-verifies every unchanged
member after packing. Only `war3map.j` and `war3map.w3i` change; the packer may
also add its generated `(listfile)`. The original terrain, pathing, object
overrides, doodads, actors and triggers remain. This is an extension of that
map, not a replacement with an empty world. Metadata beside each output
records the source/output SHA-256, roster and changed members. The known source
SHA-256 is `ce375f3525fcf635c980172f42beb5cc4c926a4387c015272b151b01d21fee99`.

The default output is manually controlled. `--demo` automatically repeats
10 seconds idle, 20 seconds movement and 60 seconds combat, resetting the army
each cycle. It moves the observer's authored start to the arena. Zoom out to
3000 for the full scene; measurement captures explicitly position the camera.
The demo's five-second status updates are separate from the manually controlled
measurement map, which displays status only when requested.

## Workload and controls

Each side starts with 100 units, including four level-six Heroes:

| Per side | Left / player 2 | Right / player 4 |
| --- | --- | --- |
| 24 | Footman `hfoo` | Grunt `ogru` |
| 16 | Rifleman `hrif` | Headhunter `ohun` |
| 12 | Knight `hkni` | Raider `orai` |
| 12 | Archer `earc` | Ghoul `ugho` |
| 12 | Huntress `esen` | Crypt Fiend `ucry` |
| 8 | Dryad `edry` | Banshee `uban` |
| 6 | Priest `hmpr` | Shaman `oshm` |
| 6 | Sorceress `hsor` | Necromancer `unec` |
| 2 | Archmage `Hamg` | Blademaster `Obla` |
| 2 | Warden `Ewar` | Lich `Ulic` |

The arena also adds 64 destructable trees and eight distinct buildings:
`hhou`, `hbar`, `hgtw`, `otrb`, `obar`, `unpl`, `uzig`, `etol`. Building
animation and authored behavior, including Blight, remain active. Props are
created once; resetting the army does not rewind the whole map or its Blight.
Starting positions and scripted orders are fixed; combat damage, targeting and
particle populations must not be assumed bit-identical across captures.

The 72-unit deployment spacing clears the large troops' 32-unit collision
radii. Allied movement alternates offsets of 250 units every six seconds,
remaining inside the tree border. Combat removes the alliance and issues real
Attack orders. Every nine seconds, surviving casters receive eligible spell
orders against living targets. Heroes use Blizzard, Fan of Knives, Frost Nova
and Bladestorm; ordinary casters use Heal, Slow, Bloodlust, Cripple and Curse.
Mana and health are filled only during setup. The running battle does not
refill mana, heal the army artificially, resurrect or replace its casualties.

For the manual map, these console commands require `sv_cheats 1`:

```text
camera edge 0
camera move 4600 -4600
zoomkey -20
jass Combat_Setup
jass Combat_Move
jass Combat_Fight
jass Combat_Status
```

`Combat_Setup` is the allied idle phase. Move and Fight each reset the roster
before starting. Noon is held with `SuspendTimeOfDay(true)` solely to stabilize
lighting; simulation, movement, AI, attacks and timers continue.

Status reports live/dead/wounded units, current total HP and starting HP, the
last accepted Move/Attack order, accepted spell orders and sampled mana costs.
An Attack order count is not a count of impacts. Mana drops larger than ten
are sampled once per second; this roster contains no mana-drain attackers.
Several casts can be combined into one observation, so `mana_costs` and the
four Hero counters are **not exact cast counts**. Health and deaths provide
independent evidence that this is a damaging fight.

## Authoring and diagnostic pitfalls

The original W3I declares only the observer. Both army owners must be declared
as computer players in the diagnostic copy: `S_SpellIsEnemy` intentionally
rejects unused map slots. Merely spawning units for players 2 and 4 allowed
melee combat but rejected hostile spells such as Frost Nova. The builder adds
the two W3I records and preserves the remaining version-25 data.

A second, separate issue remains in the engine: the installed old
`Scripts/common.j` declares player `SPELL_EFFECT=274` and unit
`SPELL_EFFECT=291`, whereas `g_local.h` uses the modern values 277 and 294.
The fixture common.j also uses modern values, so existing spell-event tests do
not cover this installed-data mismatch. A generic version-aware event contract
needs separate regression coverage; this benchmark does not modify event
ordinals or claim to fix campaign spell triggers. Its mana observations avoid
relying on those callbacks. The failed event-counter audit is retained locally
as `honor-combat/event-code-mismatch.log`.

`UI_ShowText` retains one active ordinary message. Two consecutive display
calls replace one another, and large Y offsets can move the status outside its
frame. The diagnostic uses one message with `|n`, at `(0.01, 0)`.

For optional Android text files, use `adb shell run-as ... cat` and inspect
the remote exit status. On this device, `adb exec-out` returned the missing-file
error in stdout with exit zero; treating that as an existing `autoexec.cfg`
backup would restore an error message as a console command. The diagnostic
error file was removed, restoring the original absence of `autoexec.cfg`.

## Bounded functional verification

`--auto-phase Fight` starts the real map scheduler, checks every initial rawcode
and nonzero authored max health, then checks elapsed simulation, actual deaths
and all four Hero mana-cost counters after 40 seconds. Use a data directory
containing the locally supplied retail MPQs and the generated `Maps` directory:

```sh
python3 tools/android/make_combat_bench.py \
  --base build/android/optimization/maps/BattleSimulatorV0.19d.w3x \
  --output build/android/optimization/honor-combat/Maps/CombatBenchAudit.w3x \
  --auto-phase Fight
XDG_DATA_HOME="$PWD/build/android/optimization/honor-combat/audit-home" \
  build/bin/openwarcraft3 -data build/android/optimization/honor-combat -tft \
  +dedicated 1 +set game_port 27994 +set com_fast_forward 1 +set vid_hidden 1 \
  +map Maps/CombatBenchAudit.w3x +com_frame_limit 450 \
  > build/android/optimization/honor-combat/final-headless.log 2>&1
```

Map timer errors do not set a failing process exit status. Therefore require
exactly one JASS runtime line, `JASS runtime error: COMBAT_VERIFY_PASS`. This
intentional `BJassError` completion sentinel occurs **after** all assertions;
an assertion error, missing sentinel or additional runtime error fails the
audit. Ordinary manual/demo maps contain neither assertions nor this sentinel.
This retail integration audit supplements the asset-independent `make test`;
it is not part of the portable unit suite. Framebuffer output and Honor GLES
performance still require the bounded device capture.

## Honor measurements, October 16, 2026

Device: AGM3-AL09HN, Mali-G52 MC2, 1920x1200 display, 67% 3D scale
(1286x804). Ordinary APK SHA-256:
`5dd1cb2f4f3396201cd44a70f824dcb3b5dbf2d9916b2f4fbdf1316b8d1f9cc0`.
No engine or APK changes were made for this scene.

| Scene | Weighted FPS | One-second range | Server wall ms/displayed frame |
| --- | --- | --- | --- |
| NightElfX01, starting party after normal opening | 19.40 | 18.4–22.0 | 19.22 |
| Mixed army idle | 19.19 | 18.0–19.9 | 11.69 |
| Mixed army moving | 15.92 | 14.0–16.7 | 18.09 |
| Mixed army fighting | 14.19 | 12.5–15.1 | 21.56 |

Each FPS window is nominally 30 seconds, computed as
`1000 * sum(R_STATS.frames) / sum(R_STATS.elapsed_ms)`, with CPU profiling
performed separately. Mixed-scene camera: `(4600, -4600)`, distance 3000;
10-second idle warmup and three-second movement warmup; Fight is measured
from the opening engagement. At the final status sample, 31 fight ticks had
passed: 160 alive, 40 dead, 30 wounded, HP 73493/98290, and positive mana-cost
counters for all four Hero types. No fixed-200 claim applies to the later fight.

Mixed-scene particles were explicitly enabled with `r_particles 2`. Other
quality values came from the user's existing configuration. Several persisted
quality strings are literally `%u 0` and parse as zero; these measurements
must not be described as a maximum-quality preset. NightElfX01 retained those
settings, including particles off. The scene table is not an A/B optimization
gain or a strict isolation of particle/combat cost.

The final battle's median renderer wall time was 48.33 ms: entities 30.73 ms,
alpha surfaces 9.64 ms, particles 1.77 ms, swap 1.30 ms. The separate Simpleperf
capture has `MDX_RenderModel` at 41.34%, `MDLX_RenderGeoset` at 20.55% and
`MDLX_FlushModelBatch` at 14.36% of all-app sampled user CPU. These inclusive
shares overlap and cannot be added. They point to heterogeneous MDX submission
and setup as the next battle-rendering investigation. The previous rejected
keytrack-cache prototype has no demonstrated benefit on this scene.

NightElfX01 has a different profile: `G_FowPlayerCanSeeEntity` 16.89%, including
`S_UnitIsDetectedByPlayer` 7.07%, and `G_ClientVertexColor` 12.12%. Visibility
and detection work during snapshot/presentation construction are a separate
campaign target. No minimum 40 FPS or stable 60 FPS result is established for
either workload.

The subsequent [offline MDX pass](performance.md#offline-mdx-visibility-work-reduction-october-16)
removes repeated visibility/colour sampling and has local CPU A/B measurements.
Its APK has not been measured on Honor; retain the scene table above as the
device baseline rather than applying the desktop percentages to its FPS.
The later [emulator A/B](performance.md#emulator-ab-of-the-mdx-visibility-change-october-16)
observed about 3% higher moving FPS and 5% higher fighting FPS in two opposite-order
pairs, with inconsistent idle results. Its SwiftShader graphics and changing battle
populations prevent treating those percentages as Honor gains.

Raw captures, maps, screenshots, checksums, Simpleperf reports and test logs
are under `build/android/optimization/honor-combat/`. Final manual-map capture
SHA-256: `f0e3b4523f36844843582f95a1ef64f8203048a7d64b0b4412330ce711bf81c5`.
An earlier capture used tighter deployment and an off-screen status message;
do not mix its `honor/` windows with `final-capture/`. The mission's diagnostic
save is `perf-tft-start-20261016`, also copied to `restored-demo/campaign-start.sav`.

The October 17 [automatic MDX transport comparison](performance.md#automatic-mdx-uniform-transport-on-android-october-17)
uses a fresh native process for each arm. Repeating `Combat_Fight` in one
process can leave the particle workload empty even after returning to the
original renderer settings; the cause remains unresolved. Inspect
`R_PARTICLES.alive` and do not count such a window as a full-effects comparison.
Keep diagnostic key bindings shorter than 80 bytes (`UINAME[80]`) and verify
the actual transport with `r_mdx_stats 1`.

See also [Android diagnostics](../../android.md),
[performance](performance.md#honor-200-actor-performance) and
[trigger events](trigger-events.md).
