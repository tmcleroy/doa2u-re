#ifndef DOAXBV_RECOMP_D3D_VBLANK_H
#define DOAXBV_RECOMP_D3D_VBLANK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void recomp_d3d_vblank_reset(void);
void recomp_d3d_wait_vblank(void);
void recomp_d3d_wait_present(void);
// RECOMP_PERF_COUNTER: tallies a guest BlockUntilVerticalBlank by return address and
// reports, once a second, those callers and the vblank waits Present added.
void recomp_d3d_note_block(uint32_t return_address);
// Host milliseconds the guest thread has slept in vblank waits; the pacing report resets it.
extern double recomp_d3d_vblank_wait_ms;
// RECOMP_PERF_COUNTER: per-frame breakdown appended to "recomp vblank miss" lines.
// The runner points this at the kernel's per-thread run times; submit ms is the draw-queue copy.
extern void (*recomp_d3d_frame_breakdown)(char *out, size_t size);
extern unsigned recomp_d3d_submit_count;
extern double recomp_d3d_submit_mb;
// The share of the picture's height left off at the top and at the bottom as it goes to the window (and to a
// screenshot), and of its width at each side, so what is left fills the window as the whole did: a game's own
// letterbox taken away (0: the whole picture). Set by the game's side between frames.
extern float recomp_d3d_present_crop;
// The share of the picture's height left off at the top and at the bottom alone, the rest stretched to the whole
// height as the window shows it (its width kept): a game's letterbox of its own taken away the way another console
// showed the same picture (0: none). Set by the game's side between frames, as recomp_d3d_present_crop.
extern float recomp_d3d_present_fill;
// A widescreen presenter shows this frame 4:3, black at the sides: the game's 4:3 picture, drawn across the 16:9 one,
// squeezed back to its shape (a game's menus kept 4:3 while its fights are 16:9). Set by the game's side between
// frames, as recomp_d3d_present_crop.
extern bool recomp_d3d_present_narrow;
// Moves the next refreshes the game waits for by ns (any thread; taken at the next wait), to keep frames in step
// with a display that refreshes at a multiple of 60 Hz (d3d_display_phase).
void recomp_d3d_vblank_nudge(long long ns);
// Sleeps the calling thread until a steady_clock time, in nanoseconds since its epoch.
void recomp_d3d_sleep_until(long long steady_ns);

#ifdef __cplusplus
}
#endif

#endif
