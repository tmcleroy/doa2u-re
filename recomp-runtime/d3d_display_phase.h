#ifndef DOAXBV_RECOMP_D3D_DISPLAY_PHASE_H
#define DOAXBV_RECOMP_D3D_DISPLAY_PHASE_H

#include <SDL3/SDL.h>

// Keeps a 60 Hz game steady on a display whose refresh is a whole multiple of 60 (60, 120, 180, 240 Hz).
// The game paces itself on its own clock; a display refreshing at 120 Hz shows each frame for two refreshes only
// while the frame's last GPU work ends well inside a refresh. Near the edge one frame gets one refresh and the next
// three, a hitch. This watches when each presented frame's work ends against the display's refreshes and moves the
// game's frame clock (recomp_d3d_vblank_nudge) a little at a time so frames end half a refresh from either edge.
// macOS only (CoreVideo's display link); elsewhere every call does nothing. RECOMP_DISPLAY_PHASE=0 turns it off.
// RECOMP_PERF_COUNTER prints, every 600 frames, how many refreshes each frame was on the screen for.

// The window whose display to follow (again when the window moves to another display).
void recomp_display_phase_follow(SDL_Window *window);
// Submits a frame's last command buffer; true when it was submitted here (else submit it as usual).
bool recomp_display_phase_submit(SDL_GPUDevice *device, SDL_GPUCommandBuffer *commands);
// Before the device goes: waits for the frames handed over and stops.
void recomp_display_phase_stop(void);

#endif
