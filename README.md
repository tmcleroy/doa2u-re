<div align="center">

# DOA2U Native PC Port

**A native Windows port of _Dead or Alive 2 Ultimate_ (Xbox), built by static recompilation, with the goal of rewriting it into readable source.**

[![Latest release](https://img.shields.io/github/v/release/NoRain211/doa2u-re?label=alpha)](https://github.com/NoRain211/doa2u-re/releases/latest)
[![CI](https://github.com/NoRain211/doa2u-re/actions/workflows/public-ci.yml/badge.svg)](https://github.com/NoRain211/doa2u-re/actions/workflows/public-ci.yml)
[![License: GPL-3.0-or-later](https://img.shields.io/badge/license-GPL--3.0--or--later-blue)](LICENSE)
![Platform: Windows](https://img.shields.io/badge/platform-Windows-0078D6)

[Download](https://github.com/NoRain211/doa2u-re/releases/latest) ·
[Bring-up notes](docs/bring-up.md) ·
[Report a bug](https://github.com/NoRain211/doa2u-re/issues)

Sister projects: [DOA3](https://github.com/NoRain211/doa3-re) ·
[DOAXBV](https://github.com/NoRain211/doaxbv-re)

</div>

> [!IMPORTANT]
> This project ships **no game files**. You need your own legally obtained copy
> of _Dead or Alive Ultimate_ (Xbox, USA), Disc 2. It is not affiliated with or
> endorsed by the game's rights holders.

## About

The DOA2 game executable on Ultimate's second disc is lifted to C with a
static recompiler, then linked against a hand-written runtime that replaces
the Xbox kernel imports and the game's Direct3D 8, DirectSound and input
libraries (XDK build 5849) with native Windows equivalents: Direct3D 11,
XAudio2 and SDL3. The rest of the game still runs as generated code. The plan
is to replace game logic with readable, hand-written source while the game
keeps running.

The port carries a short card in memory of Tomonobu Itagaki, shown before the
Story staff roll.

## Status

> [!WARNING]
> **0.1 Alpha is bring-up software.** Story mode and the attract demo play in
> development runs, but many modes, stages and characters have not been tried.
> A draw the port cannot render yet stops the game with a message in the log.

| Area | State |
| --- | --- |
| Boot, menus, attract demo, character select | Working |
| Story, Kasumi | Played through the final fight, ending, credits and CG Gallery unlock (with the `--story-finale` shortcut) |
| Story, Ayane and Gen Fu | Stages 1-6 cleared without a stop in scripted runs; the Tengu fight was lost each time |
| Other characters, Versus, Tag, Survival, Time Attack, Sparring | Not yet tried |
| Every character and costume | Unlocked; the game saves the unlocks to your profile |
| Native 16:9 widescreen, up to 4K, MSAA up to 32x, SMAA Ultra | Working, chosen in the launcher |
| Audio, including 3D voices and reverb | Working |
| Xbox and other SDL3 controllers, keyboard | Working; keyboard drives player 1 |
| Hand-written game logic | Not started; the game runs as generated code |
| macOS and Linux | Planned ([ADR 0001](docs/adr/0001-three-platform-targets.md)) |

## Play the alpha

**Requirements:** Windows 10/11 x64,
[Python 3.12 or newer](https://www.python.org/downloads/windows/) on PATH,
[CMake 3.20 or newer](https://cmake.org/download/) on PATH,
[Visual Studio 2022 or its Build Tools](https://visualstudio.microsoft.com/downloads/)
with **Desktop development with C++**, internet access for the first build,
and your Disc 2 ISO with about 5 GB free.

1. Download the ZIP from the
   [latest release](https://github.com/NoRain211/doa2u-re/releases/latest) and
   extract it into a new folder with a short path, such as `C:\DOA2U`.
2. Drag your Disc 2 ISO onto `BuildGame.cmd` and wait for **Build complete**
   (about 45 minutes). The build stops unless it reproduces the exact program
   the developers tested.
3. Run `play.cmd`, pick resolution, MSAA, SMAA, widescreen and whether to
   skip the intro movies, then press **Play**. Your choices are remembered.

The first boot copies the game's archives into its cache and shows a black
window for about 12 minutes; later boots start normally. Saves and the cache
live in `private\play-disc`. The package contains tools and source, never a
prebuilt runner or game data. GitHub's automatic source archives lack the
pinned lifter and extractor; use the release ZIP.

**Controls:** the first connected controller, or the keyboard for player 1:
arrows are the D-pad, Enter is Start, Backspace is Back, Z/X are A/B, A/S are
X/Y, Q/W are White/Black and E/R are the triggers.

## Build from source

The public tests need no game files or lifter checkout:

```powershell
git clone https://github.com/NoRain211/doa2u-re.git
cd doa2u-re
python -m unittest discover -s tools -p "test_*.py"
python tools/public_export.py verify --require-public-tree
cmake -S recomp-runtime -B build/recomp-runtime
cmake --build build/recomp-runtime --config Debug
ctest --test-dir build/recomp-runtime -C Debug --output-on-failure
```

To also test real PCM consumption on this PC's audio endpoint, configure with
`-DRECOMP_NATIVE_AUDIO_TEST=ON`. Passing tests does not establish game
compatibility.

To build the game from a clone, initialize the lifter and pass your ISO:

```powershell
git submodule update --init tools/xboxrecomp
python tools/build_game.py <your-disc-2.iso>
python tools/run_game.py --play
```

The importer extracts the disc once into `private/imported-disc` with XboxDev
[extract-xiso](https://github.com/XboxDev/extract-xiso) (on PATH, or bundled
in the release ZIP), selects the game executable by its SHA-256 and leaves
the ISO unchanged. Only that hash is published, in `tools/build_game.py`.
After a runtime change, `python tools/build_game.py --build-only` reuses the
authenticated lift; `--lift-only` generates without building. Generated C,
receipts and logs stay in `private/`.

`run_game.py` takes the launcher's settings as switches, and
`--seconds <n>` runs a bounded probe instead of `--play`:

```powershell
python tools/run_game.py --play --resolution 2160 --widescreen --msaa 32 --smaa
python tools/run_game.py --play --no-widescreen --msaa 1 --no-smaa
```

The game picks its own 16:9 camera and menu layouts from the dashboard video
setting, and the presenter shows that image at 16:9. MSAA resolves before
SMAA, and gamma applies last; unsupported MSAA counts fall back to the
highest count the GPU supports. Arguments to `play.cmd` bypass the settings
window. The build fetches pinned, hash-verified copies of SDL3 and
[upstream SMAA](https://github.com/iryoku/smaa).

The dedication card is built at first launch from
`private/dedication-photo.jpg` (shipped in the release ZIP) with Pillow,
drawing the name in the game's fight-screen capitals read from your disc.
`python tools/run_game.py --play --story-finale` is a debug shortcut to the
ending that uses separate probe saves.

To package a release ZIP from a clean checkout:

```powershell
python tools/package_test.py 0.1-Alpha
```

It zips committed HEAD, the pinned lifter, extract-xiso and its license, and
the dedication photo from `private/release-assets/`, with
[test-release.md](docs/test-release.md) as `README.txt`.

## Repository layout

| Path | Contents |
| --- | --- |
| [`recomp-runtime/`](recomp-runtime) | Runtime, kernel and input adapters, audio, D3D8 replacements, D3D11 presenter, tests |
| [`xbe/`](xbe) | XBE parsing and hashing |
| [`tools/`](tools) | Disc import, build, launcher, packaging and export checks; pinned lifter submodule |
| [`docs/`](docs) | Bring-up notes, reuse provenance, decisions and the release README |
| `private/` | Ignored local game inputs, generated output and run evidence |

## Contributing

Translation defects are fixed in the
[lifter](https://github.com/NoRain211/xboxrecomp) and regenerated; generated
game C is never patched by hand. Read [bring-up](docs/bring-up.md) and
[reuse](docs/reuse.md) before changing an adapter.

When [reporting a bug](https://github.com/NoRain211/doa2u-re/issues), say
what you did and what happened, and copy the `recomp stop` or crash lines
from the end of the newest log in `private\play-logs` (or the error from the
newest `private\build-logs` folder). Remove private paths, and never upload
game binaries, generated game C, assets, extracted filenames, saves or whole
logs.

## AI assistance

This project uses large language models and AI coding agents extensively for
code, reverse-engineering analysis, debugging, tests and documentation. Their
output can contain mistakes; changes are reviewed and tested, and unverified
behavior is tracked as such.

## License

Original code and documentation are licensed under
[GPL-3.0-or-later](LICENSE); see also [NOTICE](NOTICE). Third-party components
keep their own licenses. The license grants no rights to the game or its assets.

## Support

If you like this work, consider [buying me a coffee on Ko-fi](https://ko-fi.com/norainsrecomps).
