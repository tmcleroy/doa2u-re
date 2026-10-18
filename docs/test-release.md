Dead or Alive 2 Ultimate native PC port - test build
====================================================

This is an early test build. It contains no game files. You build the
game yourself from your own copy of Dead or Alive Ultimate (Xbox, USA),
Disc 2. Other regions, versions and Disc 1 are refused.

What you need (Windows 10 or 11, 64-bit):

- Python 3.12 or newer: https://www.python.org/downloads/windows/
  (tick "Add python.exe to PATH").
- CMake 3.20 or newer: https://cmake.org/download/ (add it to PATH).
- Visual Studio 2022 or newer, or its Build Tools, with the
  "Desktop development with C++" workload:
  https://visualstudio.microsoft.com/downloads/
- An internet connection for the first build (it downloads SDL3 for
  controller support and the Capstone Python package).
- Your Disc 2 as an Xbox ISO, and about 5 GB of free space on the drive
  holding this folder.

Build (once, about 10-20 minutes):

1. Extract this ZIP to a folder with a short path, for example C:\DOA2U.
2. Drag your Disc 2 ISO onto BuildGame.cmd. The ISO is only read.
3. Wait for "Build complete". The build is checked against the exact
   program the developers tested; any difference stops the build.

Play: double-click play.cmd. The first boot copies the game's archives
into its cache and shows a black window for about 12 minutes; later boots
start normally. Saves and the cache live in private\play-disc.

Controls: the first connected controller (Xbox, PlayStation and other
controllers SDL3 supports). The keyboard drives player 1:

    Arrow keys = D-pad      Enter = START     Backspace = BACK
    A = X    S = Y    Z = A    X = B
    Q = White    W = Black    E = Left trigger    R = Right trigger

Known limits: this is bring-up software. Many modes, stages and
characters reach graphics the port does not draw yet; the game then
stops and the log says why. The picture is 4:3, as on the Xbox.

Reporting problems: say what you did and what happened. Copy the
"recomp stop" or crash lines from the end of the newest log in
private\play-logs, or the error from the newest private\build-logs
folder if the build failed. Remove your user name and other private
paths, and do not send whole logs or any game files.
