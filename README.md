# Dead or Alive 2 Ultimate Native PC Port

A starter project for a native Windows port of the Xbox release of Dead or
Alive 2 Ultimate, followed by replacement of generated game logic with readable,
hand-written source. Sister projects: [DOA3](https://github.com/NoRain211/doa3-re)
and [DOAXBV](https://github.com/NoRain211/doaxbv-re).

This repository contains no game files. Original project source and
documentation are GPL-3.0-or-later; see [LICENSE](LICENSE) and [NOTICE](NOTICE).

## Status

Initialized for the USA-labelled Ultimate Disc 2 image. The private import has
been checked for consistency; its retail revision has not been authenticated
against an independent disc catalog. The disc's small launcher is separate
from its main game executable. Executable paths, hashes and inventory belong
in `private/`; the selected target is recorded in `private/target.json`.

The main executable records XDK, D3D8 and DirectSound build 5849. DOA3 uses
3911/3925/3936 and DOAXBV uses 4928, so their library address bindings cannot
be reused unchanged.

The whole-program lift and native runner are in place. The runtime loads the
selected executable, binds kernel imports, initializes TLS and schedules
guest threads. XAPI fibers run on native Windows fibers, so the game's scene
tasks execute. The clock and vblank bindings let background archive workers
progress; the first boot copies the disc archives into the cache partition
and shows a black window for about 12 minutes.

The D3D8 5849 adapter drives the sister projects' semantic D3D11 presenter.
It covers device creation and Reset for the 720x480 mode and the supersampled
character-select mode, frame and render state, lights and materials,
textures, vertex buffers and render targets, programmable vertex shaders,
the fight's pixel shaders and post-process passes, lines, point sprites and
visibility tests. A draw whose state the presenter cannot yet express stops
with a report of that state.

The DirectSound 5849 adapter uses XAudio2 for PCM/Xbox ADPCM buffers,
including data ownership, reference counts, headroom, looping and cursors.
The effects-image adapter decodes programs, owns validated maps/state and
binds native I3DL2 reverb with stereo dry/wet routing. Positioned 3D voices
render through the game's HRTF tables. Recording sends and DSP crosstalk
cancellation remain guarded. The XAPI 5849 input APIs read SDL gamepads and
the keyboard.

In bounded local probes the game reaches the first Story fight (Stage 01)
and plays it for 180 seconds with audio. Other modes, stages and characters
can still reach unbound draw paths and stop. Passing the synthetic tests
alone does not establish compatibility.

Read [reuse](docs/reuse.md) for the source provenance and what can be brought
over next, and [bring-up](docs/bring-up.md) for the next implementation steps.

## Build the public tests

No game files or lifter checkout are needed:

```powershell
python -m unittest discover -s tools -p "test_*.py"
python tools/public_export.py verify --require-public-tree
cmake -S recomp-runtime -B build/recomp-runtime
cmake --build build/recomp-runtime --config Debug
ctest --test-dir build/recomp-runtime -C Debug --output-on-failure
```

To also test real PCM consumption on this PC's audio endpoint, configure with
`-DRECOMP_NATIVE_AUDIO_TEST=ON`. That test silences its captured output and
requires a working endpoint; the default synthetic tests need no sound device.

## Import another local disc

Use XboxDev [extract-xiso](https://github.com/XboxDev/extract-xiso) on PATH or
pass its executable with `--extractor`. The importer retains logs and a hash
receipt under `private/`, refuses existing output directories, and verifies
that the original ISO did not change.

```powershell
python tools/extract_iso.py <your-disc.iso> --output private/new-import
```

The source ISO remains in its original location. Extract one disc once and
reuse it; future play folders should use hardlinks instead of copying it.

## Lift and probe the selected game

The ignored `private/target.json` supplies the game executable path and hash.
The runner rejects any executable that differs from its compiled identity.
Initialize the lifter submodule and use the locally selected Capstone version:

```powershell
git submodule update --init tools/xboxrecomp
python -m pip install capstone==5.0.7
python tools/build_game.py
python tools/run_game.py --seconds 30
```

After a runtime change, `python tools/build_game.py --build-only` reuses the
authenticated lift. `--lift-only` generates without building. Generated C,
analysis and build receipts stay in `private/program/`; boot logs stay in
`private/boot-logs/`. The probe creates `private/play-disc` with hardlinks,
preserves its saves, and reports a timeout as termination of the probe.

`play.cmd` opens a settings window with 480p, 720p, 1080p, 1440p and 2160p
(4K) render resolutions; MSAA Off/2x/4x/8x/16x/32x; SMAA Ultra; and native
16:9 widescreen. Choices are remembered in `private/launcher.json`. The first
launch selects the nearest preset at or below the primary display's height,
with widescreen, a 32x MSAA request and SMAA enabled. The game
selects its own widescreen camera and menu layouts through the dashboard video
setting; the presenter displays that anamorphic image at 16:9. MSAA resolves
before SMAA, and gamma applies last. Unsupported MSAA counts fall back to the
highest supported count below the request. The Windows build fetches a pinned,
hash-verified copy of [upstream SMAA](https://github.com/iryoku/smaa).

The Python launcher also accepts these switches for play or bounded probes:

```powershell
python tools/run_game.py --play --resolution 2160 --widescreen --msaa 32 --smaa
python tools/run_game.py --play --no-widescreen --msaa 1 --no-smaa
```

Omitted switches preserve the existing `RECOMP_D3D_WIDESCREEN`,
`RECOMP_D3D_MSAA`, `RECOMP_D3D_SMAA` and `RECOMP_D3D_SCALE` environment settings. Pass
`--no-widescreen --msaa 1 --no-smaa` to `play.cmd` to disable the enhancements.
Arguments to `play.cmd` bypass the settings window. Higher render resolutions
fill the display when they reach its height, and downsample when larger;
480p stays windowed. Width follows 4:3 or 16:9. A 32x request uses actual
32-sample MSAA only when the GPU supports both color and depth at that count;
otherwise the log reports the supported fallback. These options add GPU work
and memory use while preserving the game's logical 720x480 coordinates.

Input uses SDL gamepads or the focused game window's keyboard. Enter sends
Start; arrows move the D-pad and Z/X send A/B. A bounded intro-skip probe can
use repeated `--input-start-pulse-at <poll>` arguments (up to four).

When `private/dedication.bmp` exists (an uncompressed 24- or 32-bit BMP),
the launcher shows it for 10 seconds, fading in and out, each time the staff
roll starts. The game is held meanwhile and resumes unchanged.
If it is missing and `private/dedication-photo.jpg` exists, `run_game.py`
first builds the 4K card from that photo with Pillow (`pip install pillow`),
drawing the name in the game's fight-screen capitals read from your disc.

`python tools/run_game.py --play --story-finale` is a debug shortcut to the
ending: every Story CPU has 1 life, and the first win skips to the final
fight. It uses the separate probe saves so the clear does not touch yours.

## Layout

| Path | Purpose |
| --- | --- |
| `recomp-runtime/` | Shared runtime, D3D11 presenter, verified adapters and synthetic tests |
| `tools/` | Disc import, XBE inspection and public custody checks |
| `tools/xboxrecomp` | Pinned lifter submodule; no generated game output |
| `docs/` | Reuse assessment and bring-up plan |
| `private/` | Ignored imports, target receipt, generated output and evidence |
