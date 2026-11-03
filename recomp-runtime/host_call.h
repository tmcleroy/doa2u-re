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
#else
static inline void recomp_host_call(void (*fn)(void *), void *data) { fn(data); }
#endif

#ifdef __cplusplus
}
#endif

#endif
