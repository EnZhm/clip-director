# Clip Director

Clip Director is an OBS Studio plugin (Windows x64) for VJ sets. It is an OBS source that
plays folders of video clips in time with the music: it follows the
[Palette Director](https://github.com/EnZhm/palette-director) beat clock, cuts on the
beat, shuffles deeply, evens out brightness, and sends the picture to Synesthesia over
Spout. Version 1.1 adds Synesthesia scene control over OSC on the same beat, and a MIDI
learn dock. Part of the [Director](https://github.com/EnZhm/director) suite.

| | |
|---|---|
| Version | 1.1.0 |
| Platform | Windows 10/11 x64 |
| OBS Studio | 32.2.x (built against 32.2.2) |
| Needs | Palette Director 1.1 or later for the beat clock |
| Licence | GPL-2.0 |

## What it adds

| Added | Name | Where it goes |
|---|---|---|
| Source | Clip Director | Any scene; plays clip folders, output over Spout to Synesthesia |
| Dock | Clip Director | Now playing / next clip, Skip / Hold / Ban, clip folders, the Synesthesia tab, the log |
| Dock | MIDI Director | Maps MIDI notes and CCs to OBS hotkeys across the suite |

With Palette Director 1.1 or newer, Clip Director cuts on Palette Director's beat grid.
With an older Palette Director (or none) it still plays, free-running at a fixed BPM.

## Requirements

| Item | Notes |
|---|---|
| Windows 10 or 11, x64 | Windows only. |
| OBS Studio 32.2.x | 32.2.2 at release. |
| Palette Director 1.1+ | The beat clock: `palette_director_clock`. Installed by the suite zip. |
| Synesthesia, Pro licence | For the Spout input, and for OSC scene control in the Synesthesia tab. |
| Python 3 and ffmpeg | Optional, for `tools/cd-ingest.py`; local disc only, OBS is not involved. |
| MIDI controller | Optional, for the MIDI Director dock. |

## Install

1. Install the suite first: download `director-1.0.0-windows-x64.zip` from
   [EnZhm/director releases](https://github.com/EnZhm/director/releases) and copy both
   folders into `C:\ProgramData\obs-studio\plugins\`. That gets you Clip Director and
   Palette Director in one step, which is the combination that ships.
2. If you only want Clip Director, take `clip-director-1.1.0-windows-x64.zip` from
   [this repo's releases](https://github.com/EnZhm/clip-director/releases) and copy the
   folder `clip-director` into `C:\ProgramData\obs-studio\plugins\`. Palette Director
   1.1+ must already be installed for the clock.
3. Start OBS and check Help > Log Files > View Current Log for:

   ```
   [clip-director] plugin loaded successfully (version 1.1.0)
   [palette-director] plugin loaded successfully (version 1.2.0)
   ```

Step-by-step install, clip preparation, cut rules, troubleshooting and uninstall:
[release/INSTALL.txt](release/INSTALL.txt).

## Prepare the clip folders

Raw downloads usually have a keyframe only every ~8 seconds, so a random start point can
take longer than a beat, and there are no brightness statistics. The ingest tool writes a
cleaned copy next to the originals:

```
python tools\cd-ingest.py "D:\Clips\groove" --dry-run
python tools\cd-ingest.py "D:\Clips\groove"
```

- Needs Python 3 and ffmpeg on PATH; uses the NVIDIA encoder when there is one.
- Writes into a `_cd` folder inside the clip folder and never changes the originals
  (hashes are checked before and after).
- Resumable: run it again after adding clips and only the new ones are encoded.
- Add the **original** folder to Clip Director; it finds the `_cd` copy itself and says
  so in the log ("from ingest index").

## The Clip Director dock

Open it from Docks > "Clip Director". The dock has two tabs: **Clips** (the source, what is
playing and next, Skip / Hold / Ban, the clip folders, the log and the footer) and
**Synesthesia** (OSC scene control). The top strip and the footer belong to the Clips tab.

| Area | Contents |
|---|---|
| Top | The source, the clock (`PD` = following Palette Director, `free` = not), the section (GROOVE / BREAKDOWN / DROP), four beat lights, and when the next cut comes |
| Now playing | Thumbnail, clip, folder, start point, speed, inverted or not; Invert (auto / always / never, saved per clip) and Ban clip (never play it again, saved) |
| Next | The clip prepared for the next cut (during a breakdown: the drop clip waiting for the drop) |
| Controls | Skip (cut on the next beat), Hold (no cuts and no stutter until released) |
| Folders | Per-folder on/off, section tag (Any / Breakdown / Drop / Groove) and weight 0-10 |
| Synesthesia | OSC scene control, see below |
| Log | Every cut, drop, stutter, skip, hold and ban |
| Footer | Clip count, how many have stats, how many are banned, cuts so far, average cut timing error |

Folder rules: `Any` folders play in grooves and anywhere nothing is tagged. Once any
folder carries `Breakdown` or `Drop`, that section plays only folders with that tag. A
folder whose name contains "breakdown", "drop" or "groove" starts with that tag. All of
this is saved with the scene collection.

## Synesthesia scene control (OSC)

Synesthesia must have a **Pro** licence, and OSC must be enabled. The host and the send and
feedback ports in the Synesthesia tab must match the ports set in Synesthesia; the defaults
are host `127.0.0.1`, send port `7000` and feedback port `7001`.

| Control | Behaviour |
|---|---|
| Auto ON/OFF, link chip | Auto OFF by default. The chip reads `OSC off` (control disabled), `OSC sending` (amber: messages going out, no echo yet), `OSC linked` (green: Synesthesia echoed a launch in the last 10 s) or `OSC error` (the socket did not open) |
| Host / send port / feedback port | Must match Synesthesia Settings > OSC |
| Now / next scene | From the pools below |
| Next scene, Hold scene, Fire bang 1 | Manual. Hold suspends automatic launches (as Hold does for clips) |
| Breakdown starts | Launch from the Breakdown pool, or do nothing |
| Drop downbeat | Launch from the Drop pool, or do nothing; optionally also fire scene bang 1 |
| During groove | Never, every 2 phrases or every 4 phrases, on the phrase downbeat |
| Scene lead (0-500 ms) | Sends the launch early to hide Synesthesia's scene-load time (default 0; groove launches and "Drop now" only) <!-- scene-lead-default --> |
| Scene pools (Groove / Breakdown / Drop) | Scene plus an optional preset; add, remove, reorder, double-click to launch; shuffle bag per pool, same rule as clips |
| Import from Synesthesia | Fills the scene names from Synesthesia's OSC output |
| Log | Every OSC message with its beat position and reason |

Messages on the wire:

| Direction | Message |
|---|---|
| Out | `/scenes/{name} "{preset}"` - name lowercase, no spaces, `_` or `-`; the preset is optional, and without one Synesthesia uses its gold slot |
| Out | `/presets "..."`, `/controls/global/{type}/{n}`, `/controls/meta/{name}`, `/playlist/next` |
| In | `/scenes/{name}` on every launch, used as the link and confirmation signal |

Timing: scene launches are emitted by the Clip Director scheduler as a slot event on the
**same beat** as the clip transition they pair with, so a drop lands as one hit: clip cut,
Palette Director's drop colour cycle and the Synesthesia scene together. Groove launches
wait until a drop cycle ends, as clip cuts do. Breakdowns and drops are detected rather
than predicted, so the scene lead applies only to phrase-aligned groove launches and to
"Drop now".

The settings are stored with the Clip Director source (`syn_*` keys) in the scene
collection.

The **Log** lists every OSC message with its beat position and reason, one per line:

```
 123.45  -> /scenes/tunnel "acid"  [drop downbeat]
 123.45  <- /scenes/tunnel
```

`->` is a message going out, `<-` an echo from Synesthesia; a launch sent early carries
`(+N ms early)`, and a message the socket refused is tagged `[NOT SENT]`.

## The MIDI Director dock

Open it from Docks > "MIDI Director". Learn mode maps a MIDI note or CC from a controller
to an OBS hotkey in the suite.

| Area | Contents |
|---|---|
| Device | Device picker, a Rescan button and an activity light that flashes on incoming MIDI |
| Learn | Pick the hotkey in the searchable list, choose the mode (press / release or toggle), press Learn to arm it, then move a control; the first press binds it |
| Mapping table | Control, target and mode, with a Remove button per row; saved with the scene collection |
| Monitor | The last MIDI message received, with a counter and a short history |

The list is every hotkey OBS knows about (frontend, source, output, encoder and service),
searchable by description or name. The suite's own hotkeys are in it: Palette Director
(Force off / on, Pause, Rotate colours, PANIC, Next / Previous palette, Palette 1-8 go
live) and its Music clock commands (Phrase is now, Drop now, Force breakdown, Tap tempo,
Lock BPM, Nudge back / forward); Clip Director (Skip, Hold, Ban) and Synesthesia (next
scene, hold scene, fire bang 1). Bind any of them to a MIDI note or CC.

Windows MIDI is read through WinMM, so there is no extra dependency to install. Mappings
are saved with the scene collection.

## Build from source

The project is an OBS plugin template layout: `buildspec.json` names the plugin, its
version and the dependency versions, and `CMakePresets.json` holds the presets.

| Requirement | Version |
|---|---|
| CMake | 3.28 or newer |
| Windows | Visual Studio 17 2022 (desktop C++), Windows 10 SDK 10.0.20348 or newer |
| Linux (unit tests) | a C++17 compiler, Ninja, pkg-config |

Windows x64:

```
cmake --preset windows-x64
cmake --build build_x64 --config RelWithDebInfo
```

The configure step downloads the dependencies pinned in `buildspec.json` (OBS Studio
32.2.2 sources, obs-deps, Qt6) into `.deps/` and checks their SHA256 hashes.

Unit tests (libobs-free, run anywhere with CMake):

```
cmake -S tests -B build-tests
cmake --build build-tests
ctest --test-dir build-tests --output-on-failure
```

Only the Windows x64 packages are released; the macOS and Ubuntu presets are kept from
the template for development.

## Licence

GPL-2.0. See [LICENSE](LICENSE).

- Suite, install and changelog: <https://github.com/EnZhm/director>
- Palette Director (the beat clock this plugin follows):
  <https://github.com/EnZhm/palette-director>
- Changelog: [CHANGELOG.md](CHANGELOG.md) | Install: [release/INSTALL.txt](release/INSTALL.txt)
