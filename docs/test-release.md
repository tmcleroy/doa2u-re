Dead or Alive 2 Ultimate native PC port - 0.1 Alpha (experimental)
=================================================================

This is an early, experimental build. It contains no game files. You
build the game yourself from your own copy of Dead or Alive Ultimate
(Xbox, USA), Disc 2. Other regions, versions and Disc 1 are refused.

What you need (Windows 10 or 11, 64-bit):

- Python 3.12 or newer: https://www.python.org/downloads/windows/
  (tick "Add python.exe to PATH").
- CMake 3.20 or newer: https://cmake.org/download/ (add it to PATH).
- Visual Studio 2022 or newer, or its Build Tools, with the
  "Desktop development with C++" workload:
  https://visualstudio.microsoft.com/downloads/
- An internet connection for the first build (it downloads SDL3, SMAA
  and the Capstone and Pillow Python packages).
- Your Disc 2 as an Xbox ISO, and about 5 GB of free space on the drive
  holding this folder.

Build (once, about 45 minutes; most of it is disassembly):

1. Extract this ZIP to a folder with a short path, for example C:\DOA2U.
2. Drag your Disc 2 ISO onto BuildGame.cmd. The ISO is only read.
3. Wait for "Build complete". The build is checked against the exact
   program the developers tested; any difference stops the build.

Play: double-click play.cmd, pick resolution, MSAA, SMAA, widescreen and
whether to skip the intro movies, and press Play; it remembers your
choices. Widescreen uses the game's own 16:9 camera and menus. The first
boot copies the game's archives into its cache and shows a black window
for about 12 minutes; later boots start normally. Saves and the cache live
in private\play-disc. Start + Back + both triggers resets the game.

Every character and costume is unlocked, and the game saves the unlocks
to your profile. Before the Story staff roll, a 10-second card honors the
memory of Tomonobu Itagaki.

Controls: the first connected controller (Xbox, PlayStation and other
controllers SDL3 supports). The keyboard drives player 1:

    Arrow keys = D-pad      Enter = START     Backspace = BACK
    A = X    S = Y    Z = A    X = B
    Q = White    W = Black    E = Left trigger    R = Right trigger

Known limits: this is bring-up software. Story, Watch and the attract demo
play in development runs on every stage, but many modes and characters
have not been tried. A draw the port cannot render yet stops the game, and the log
says why. The Xbox Live menus stop the game, as there is no online service.
A demanding moment on The Burai Zenin can still drop a single frame, and
the safari stage's heat haze does not ripple yet.

Reporting problems: open an issue at
https://github.com/NoRain211/doa2u-re/issues and say what you did and what
happened. Copy the "recomp stop" or crash lines from the end of the newest
log in private\play-logs, or the error from the newest private\build-logs
folder if the build failed. Remove your user name and other private paths,
and do not upload whole logs or any game files.
