# Warcraft III Pre-Rendered Movies

## Contract

Classic Warcraft III campaign movies are client presentation, not simulation state. JASS chooses a logical movie with
`PlayCinematic("HumanEd")`; `games/warcraft-3/game/api/api_misc.h` converts that to the Warcraft asset convention
`Movies\\HumanEd.mpq` and calls the generic `game_import.QueueMovie` callback. The game module must not link FFmpeg or
decode video itself.

`QueueMovie` is a deferred-session interposer. If the same simulation frame later requests `ChangeLevel`, `EndGame`, or
another `gi.MenuAction`, `client/cl_main.c` freezes the outgoing server simulation, plays the queued movie, then performs
the preserved map/menu action after EOF or Escape. This avoids replacing the world while JASS is on the C stack and also
avoids advancing the outgoing map behind a full-screen movie.

`SetCinematicScene` / triggered dialogue are unrelated in-engine presentation. They do not use this movie path.

## Optional FFmpeg Backend

The default desktop build has no FFmpeg dependency. Enable the movie decoder with:

```bash
make FFMPEG=1
```

Android game APKs build and include the decoder by default through `tools/android/build_ffmpeg.py`.
The script pins [FFmpeg 8.0.3](https://ffmpeg.org/releases/ffmpeg-8.0.3.tar.xz), verifies its SHA-256,
and builds only AVI/MP3 demux, MPEG-4/MP3/PCM/raw-video decode, file I/O and conversion. The five shared
libraries use 16 KiB ELF alignment; the APK includes their source/build manifest and LGPL notice.
Game media stays outside the APK. A matching host build needs no system pkg-config:

```sh
python3 tools/android/build_ffmpeg.py
make FFMPEG=1 FFMPEG_ROOT="$PWD/build/ffmpeg/host" openwarcraft3 openwarcraft3-tests test-menu
```

`FFMPEG_ROOT` points to a prepared include/lib prefix; without it, the desktop path uses pkg-config.
The media build-config stamp invalidates menu/app/test binaries when either option changes.
`test-music-mp3` explicitly selects the minimp3 backend even in an FFmpeg-enabled test run;
its backend-specific state assertions must not compile against the FFmpeg state layout.
Run focused `client_movie.*` and `client_session.*` invocations sequentially across editions:
the movie extractor uses the same per-user `openrealm-movie.tmp` filename.

The Warcraft III build then requires only these pkg-config packages:

```text
libavformat
libavcodec
libavutil
libswscale
libswresample
```

`client/cl_movie.c` owns demux/decode, A/V scheduling, Escape-to-skip, archive extraction, and full-screen presentation.
The renderer exposes a Quake-II-style `DrawCinematicFrame` operation. The client supplies decoded RGBA pixels and the destination rectangle; the renderer owns the persistent upload texture and draws it. Passing `NULL` releases that renderer-owned texture. The sound mixer exposes a generic 44.1-kHz stereo S16 stream slot, following the same ownership split as Quake-style cinematic audio. Movie and music streams are independent; see [music.md](music.md).

The decoder first asks `FS_ResolveLoosePath()` for a real disk path. This lets libavformat stream a normal loose
`Movies\\*.mpq` file without loading the whole movie into memory. If the movie is found only inside an archive,
`FS_ExtractFile()` copies it to the per-user `openrealm-movie.tmp` file for the duration of playback and removes that file
when playback ends. Do not hold `FS_OpenFile()` open for a movie: the archive API's global file lock is intentionally
short-lived and would block unrelated asset reads.

The backend is format-driven. Do not special-case the `.mpq` filename extension in the decoder; libavformat probes the
actual media container. Warcraft's movie files use the historical movie naming convention even though the payload is
media rather than a normal game-data MPQ archive.
The filesystem's recursive archive scanner recognizes RIFF/AVI files and leaves them in the loose
asset root instead of attempting to mount them as MPQ archives.

For direct diagnosis from the console:

```text
playmovie HumanEd
playmovie "Movies\\HumanEd.mpq"
```

The first form expands to `Movies\\HumanEd.mpq`. A build without `FFMPEG=1` keeps the command but reports that movie
playback is disabled.

## Renderer And Audio Flow

```text
PlayCinematic("HumanEd")
    -> gi.QueueMovie("Movies\\HumanEd.mpq")
    -> client defers pending MenuAction
    -> CL_PlayMovie
        -> libavformat
        -> libavcodec
        -> swscale -> RGBA -> renderer dynamic texture
        -> swresample -> stereo S16/44100 -> S_StreamSamples(S_STREAM_MOVIE)
    -> EOF or Escape
    -> resume preserved MenuAction
```

Movie rendering uses the renderer's actual UI scene rectangle and letterboxes/pillarboxes to preserve the source aspect
ratio. While active, movie input consumes keyboard/mouse interaction so hidden gameplay/menu controls cannot be
activated. The first left-button/touch release reveals a top-right skip button and continues playback.
Skipping requires a subsequent press and release inside that button; background taps, dragging out,
unmatched releases and focus loss cannot activate it. Controls remain visible until playback ends and
start hidden for each new movie. Escape still skips directly. The system cursor is hidden until controls
are revealed. If background music is active, movie startup suspends it before the movie PCM stream
starts; EOF/Escape restores the prior music pause state. The separate `S_STREAM_MOVIE` and
`S_STREAM_MUSIC` buffers prevent either decoder from resetting the other.

The same media control covers isolated model cinematics. `refExport.DrawMediaControl` draws a native
control using the game's renderer, without reactivating suspended glue or the underlying gameplay HUD.
WC3 resolves its normal/pressed Esc-menu textures and font through `UI\\war3skins.txt`; the native
label is shared with the in-engine cinematic button. Other renderers expose the generic media control.
The client uses one rectangle for drawing and window-pixel hit testing, including stretched/wide canvases.

## Startup And Campaign Preludes

The Warcraft menu starts `Movies\TutorialIn.mpq` on the first RoC frontend launch and `Movies\IntroX.mpq`
on the first TFT frontend launch. Archived `wc3_intro_seen` is a bit mask (RoC `1`, TFT `2`); a bit is set
only when playback starts successfully. Missing files/decoder support therefore remain repairable on
the next application launch. One attempted start per menu lifetime prevents repeated errors when navigating
back to the main menu. Direct map/network launches do not show the intro. Reset the preference to `0`
to replay first-launch intros.

Launching mission zero from a campaign with an authored opening plays its `OpenCinematic`/`OpCinematic`
first. `menuImport.PlayMovieBeforeMap` queues that asset plus a typed deferred map action; EOF, skipping,
or playback failure then loads the chosen mission. The ordinary console `map` command is immediate and
must not be used to sequence a movie followed by a map. Later missions keep their ordinary launch path;
camera-button rows allow explicit replay. The supplied 1.26a data uses `TutorialOp` for Thrall's Vision
and `HumanOp` for the Alliance opening. Both are AVI videos, rather than map/JASS scenes.

## Android Movie Data

The original installation's `Movies/` directory is separate from the main game archives. All nine supplied
1.26a movies are AVI containers (MPEG-4/`BLZ0`, stereo 44.1-kHz MP3) with `.mpq` filenames. Importing only
`war3.mpq`/expansion archives cannot supply them. `ArchiveStore` now accepts those video files, verifies
their RIFF/AVI identity and declared length, and atomically installs them under `files/data/Movies/`.
It retains native MPQ validation for the six main archives. Preserve original movie filenames/case.
Interrupted, truncated or invalid imports retain the previous file; movie data is never packaged in the APK.

## Campaign Selection Metadata

Warcraft campaign data has three movie-like fields in each campaign section:

```text
IntroCinematic
OpenCinematic
EndCinematic
```

The Warsmash reference parser (`CampaignMenuData`) reads each field with exactly the same three-column shape as a mission:

```text
header, display name, filename
```

and stores them separately from the numbered `Mission0`, `Mission1`, ... list. Current Warsmash parses these three fields
but its current mission-list construction iterates only `getMissions()`, so the reference code does not itself provide a
finished movie-selection implementation.

`games/warcraft-3/menu/screens/single_player.c` parses these records into separate cinematic slots. It
also accepts the classic single-name `InCinematic`, `OpCinematic`, `EdCinematic` records, with headers
from `[Label]` and filenames derived from the campaign key plus `In`/`Op`/`Ed`. Decoder-enabled builds
show these entries with the stock `CampaignListBox` camera-button art: intro and opening precede
missions, ending follows them. The existing list model retains a diagnostic `Cinematic:` display name.

The menu normalizes logical movie names such as `HumanEd` to `Movies\HumanEd.mpq` and invokes the existing typed
`menuImport.PlayMovie(path)` callback when that row is selected. If a cinematic filename already contains a directory it
is preserved; a bare filename with an extension is placed under `Movies\`.

These rows are compiled out of the mission list when `BZ_FFMPEG` is absent, so a normal build made without `FFMPEG=1`
continues to show the numbered mission list. Parsing the metadata is still harmless in that build and keeps campaign data
handling independent from the decoder capability.

Numbered `MissionN` entries can separately point to `.mdl`/`.mdx` scenes, such as TFT Undead's
"Время настало..." finale. Those entries use `menuImport.QueueModelCinematic`, preserve the authored
model path, and show a camera button in every build. They do not use the AVI decoder or a `map` command.
When the final numbered entry is a model, decoder-enabled builds queue the campaign's
authored ending movie after it. Model EOF/skip advances to that movie; ending EOF/skip
loads the edition's post-campaign bonus-credits map: `Maps\Campaign\BonusCredits.w3m`
(RoC) / `Maps\FrozenThrone\Campaign\War3XBonusCredits.w3x` (TFT), matching the
original NightElf07 / UndeadX08 `SetNextLevelBJ` destinations. Selecting that finale's
ending row directly also continues to credits. RoC has no numbered model finale;
its final ending row is also recognized as the last campaign with an ending movie
in `CampaignList` order (ignoring trailing campaigns with no ending movie).
The menu uses `PlayMovieBeforeMap` so the map waits until all clips are done.
Earlier campaign movie rows stay independent. JASS-queued movies retain their transition
gate. This replay policy does not execute mission victory scripts or fabricate completion
flags. `UI/MiscUI.txt` `CreditsFile[_V0/_V1]` selects the **separate regular credits**
(`WarcraftIIICredits.w3m` / `War3XRegularCreditsIce.w3x`); using it after the campaign
was incorrect. It is not consulted by finale replay. The order-based classification is a documented
compatibility policy, not an explicit continuation field in CampaignStrings.
See [model cinematics](campaign-ai-model-cinematic.md#full-screen-model-cinematics).

This first-pass list intentionally does **not** claim retail availability semantics: all three authored cinematic records
are shown when FFmpeg is enabled, regardless of mission-play filtering. That is useful for validating data parsing and
playback before progression persistence exists. The retail-compatible follow-up remains:

1. Gate Open/End rows with the state written by `SetOpCinematicAvailable` / `SetEdCinematicAvailable`; those natives are
   still stubs, so unlock persistence must be implemented before claiming retail-compatible availability.
2. Determine whether Intro availability is always data-authored/default-open or has its own progression rule in the
   original client before hiding it behind a guessed state bit.
3. Keep campaign/mission availability (`wc3_campaign_visibility=unlocked`) separate from cinematic availability.
   Campaign progress records authored `SetCampaignAvailable` / `SetMissionAvailable` state; an unlocked cinematic is a
   different progression fact.

## Known Pitfalls

- `PlayCinematic` is not `SetCinematicScene`; one is pre-rendered movie playback, the other is in-engine dialogue/cinematic
  UI.
- Do not synchronously play or change maps from the JASS native. Both operations cross client/session boundaries only
  after `SV_Frame` returns.
- Do not load an entire movie with `FS_ReadFile()` on memory-constrained targets. Prefer a loose disk path or temporary
  extraction so FFmpeg can seek/stream normally.
- Do not expose FFmpeg types through `server/game.h`, `client/menu.h`, or renderer public structs. FFmpeg remains a client
  implementation detail under `BZ_FFMPEG`.
- The camera-selection rows are not safe to expose as "unlocked" until `SetOpCinematicAvailable` and
  `SetEdCinematicAvailable` have persistent state.

## Verification

Automated tests use original generated AVI fixtures (`tools/wc3moviegen.py`), rather than retail movies.
With the decoder enabled, `client_movie.*` covers real archive extraction, video pixels, PCM stream
delivery, EOF/Escape cleanup, missing files and aspect ratio. `client_touch.*` checks reveal/press-to-skip,
background taps, drag-out and focus cancellation, new-movie reset and suppression of hidden UI activation;
`client_session.*` checks the prelude/map handoff. `test-menu` covers first-start
edition flags, unavailable assets, direct-map exclusion and modern RoC/TFT campaign preludes.
Android `ImportTests` also exercises movie import and preservation with the actual transaction code.
Headless renderer tests resolve non-stock skin fixtures through the production movie-control renderer
and check its normal/pressed textures, localized label, font and centered placement without a GL context.

During initial decoder bring-up on the Honor tablet, the APK was verified with all nine supplied 1.26a movies copied unchanged
(SHA-256 matched). First-start RoC/TFT intros produced visible frames; selecting chapter one through
the campaign menu played `HumanOp`/`TutorialOp`, and tapping each prelude opened the corresponding
Human/prologue chapter-one loading screen (before the two-step media control was added). These checks verify image presentation and transition
ordering; subjective lip-sync still needs listening on the device. The source `TutorialOp` contains
extended black transitions between its opening title cards, so a black screenshot alone does not
establish a stalled decoder. Device diagnostics restored the pre-test user configuration.

Useful bounded manual playback for framebuffer/audio-device verification:

```bash
./build/bin/openwarcraft3 -data "data/Warcraft III" +playmovie HumanOp +screenshot 90 +com_frame_limit 120
```

Android's diagnostic launch accepts the same logical basename, suppresses the frontend intro,
and can exit after a bounded number of frames:

```sh
adb shell am start -W -n org.openrealm.wc3/.LauncherActivity --ez play true --es movie HumanOp --ei frames 1200
```

Then verify:

- aspect ratio is preserved with black bars where necessary;
- audio is synchronized closely enough to dialogue/video and does not underrun;
- Escape skips immediately and the hidden menu/game does not receive the key;
- a first tap reveals the skip button without stopping the movie, a background tap leaves it playing,
  and only a subsequent button press skips;
- a campaign `PlayCinematic` followed by `ChangeLevel` plays the movie before loading the next map;
- a campaign `PlayCinematic` followed by `EndGame` plays the movie before rebuilding the frontend;
- a build made without `FFMPEG=1` has no FFmpeg link dependency and continues the deferred map/menu transition if movie
  playback is unavailable.
