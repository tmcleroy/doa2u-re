/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "input_keyboard_win32.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>

/* The presenter receives events on its worker; the guest samples elsewhere. */
static SRWLOCK lock = SRWLOCK_INIT;
static uint8_t held[RECOMP_INPUT_KEY_COUNT], pending[RECOMP_INPUT_KEY_COUNT];

void recomp_input_keyboard_event(uint32_t key, bool down)
{
    if (key >= RECOMP_INPUT_KEY_COUNT) return;
    AcquireSRWLockExclusive(&lock);
    if (down && !held[key]) pending[key] = 1u;
    held[key] = (uint8_t)down;
    ReleaseSRWLockExclusive(&lock);
}

void recomp_input_keyboard_reset(void)
{
    AcquireSRWLockExclusive(&lock);
    memset(held, 0, sizeof held);
    memset(pending, 0, sizeof pending);
    ReleaseSRWLockExclusive(&lock);
}

void recomp_input_keyboard_sample(uint8_t keys[RECOMP_INPUT_KEY_COUNT])
{
    AcquireSRWLockExclusive(&lock);
    for (uint32_t i = 0; i < RECOMP_INPUT_KEY_COUNT; ++i) {
        keys[i] = held[i] | pending[i];
        pending[i] = 0u;
    }
    ReleaseSRWLockExclusive(&lock);
}
