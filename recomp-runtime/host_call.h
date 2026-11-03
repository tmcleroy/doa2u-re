/* recomp_host_call(fn, data): fn(data), on a host thread that can reach the system's own code.
   Android: SDL reaches Java for windows, gamepads and events, and Java refuses calls made on a guest
   fiber's stack (it takes them for a stack overflow), so there fn runs on a thread of its own while the
   caller waits. Everywhere else it is a plain call. */
#ifndef RECOMP_HOST_CALL_H
#define RECOMP_HOST_CALL_H

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __ANDROID__
void recomp_host_call(void (*fn)(void *), void *data);
/* Android: SDL's events and gamepads pumped on a thread of their own, every millisecond, so the game reads
   the event queue and the pads' state without waiting (start once SDL is up) */
void recomp_host_pump_start(void);
/* Android: returns at once, or once the app is back in front if it is in the background (the game waits
   there, as a program does while the system has it stopped) */
void recomp_host_wait_foreground(void);
#else
static inline void recomp_host_call(void (*fn)(void *), void *data) { fn(data); }
#endif

#ifdef __cplusplus
}
#endif

#endif
