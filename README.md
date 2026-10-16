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
guest threads. The initial D3D8 5849 adapter creates a native D3D11 device and
handles framebuffer headers, viewport, tiles, clear, swap, gamma, transforms,
point/directional lights and fog state. It reuses the sister projects'
semantic presenter.

The graphics adapter supports the verified 720x480 startup mode and the
fixed-function vertex format, frame render states, material and empty texture
stages that the game sets each frame. Texture resources, programmable shaders
and draw submission remain work, as does input.
XAPI fibers run on native Windows fibers, so the game's scene tasks execute.
They stop at the first D3D API that is still unbound; the window stays black
until the draw path exists.
The initial DirectSound 5849 adapter uses XAudio2 for PCM/Xbox ADPCM buffers,
including data ownership, reference counts, headroom, looping and cursors.
The effects-image adapter decodes programs, owns validated maps/state and
binds native I3DL2 reverb with stereo dry/wet routing. Recording sends and
DSP crosstalk cancellation need further native services; positioned 3D
voices render through the game's HRTF tables. The clock and vblank bindings
allow background archive workers to progress during startup.
The graphics adapter also binds textures, vertex buffers, pixel-shader state
and three draw APIs to the shared D3D11 presenter. Unsupported shader and
resource paths stop explicitly. Passing synthetic tests does not establish
title-screen or gameplay compatibility.

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

Input uses SDL gamepads or the focused game window's keyboard. Enter sends
Start; arrows move the D-pad and Z/X send A/B. A bounded intro-skip probe can
use repeated `--input-start-pulse-at <poll>` arguments (up to four).

## Layout

| Path | Purpose |
| --- | --- |
| `recomp-runtime/` | Shared runtime, D3D11 presenter, verified adapters and synthetic tests |
| `tools/` | Disc import, XBE inspection and public custody checks |
| `tools/xboxrecomp` | Pinned lifter submodule; no generated game output |
| `docs/` | Reuse assessment and bring-up plan |
| `private/` | Ignored imports, target receipt, generated output and evidence |
