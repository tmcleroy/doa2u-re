#ifndef DOAXBV_RECOMP_INPUT_HOST_WIN32_H
#define DOAXBV_RECOMP_INPUT_HOST_WIN32_H

#include "input_model.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool recomp_input_host_sample(uint32_t port, RecompInputGamepad *gamepad);
/* Sees every finished sample (pad and keyboard) and may change it: the free
   camera reads its controls here and hands the game a still pad. While it is
   set the right stick's click never reaches the game; stick_clicks holds it
   (and the left one) apart from the sample. */
typedef void (*RecompInputCaptureHook)(uint32_t port, RecompInputGamepad *gamepad, uint16_t stick_clicks);
void recomp_input_host_set_capture_hook(RecompInputCaptureHook hook);

/* Keys the runtime reads for itself. Some (A S Z X Q W E R, the arrows,
   Return, Backspace) also press pad buttons for the game; the free camera
   leaves those buttons out while it flies. */
typedef enum RecompInputHostKey {
    RECOMP_INPUT_HOST_KEY_F1,
    RECOMP_INPUT_HOST_KEY_GRAVE,
    RECOMP_INPUT_HOST_KEY_F12,
    RECOMP_INPUT_HOST_KEY_W,
    RECOMP_INPUT_HOST_KEY_A,
    RECOMP_INPUT_HOST_KEY_S,
    RECOMP_INPUT_HOST_KEY_D,
    RECOMP_INPUT_HOST_KEY_Q,
    RECOMP_INPUT_HOST_KEY_E,
    RECOMP_INPUT_HOST_KEY_R,
    RECOMP_INPUT_HOST_KEY_Z,
    RECOMP_INPUT_HOST_KEY_X,
    RECOMP_INPUT_HOST_KEY_C,
    RECOMP_INPUT_HOST_KEY_P,
    RECOMP_INPUT_HOST_KEY_H,
    RECOMP_INPUT_HOST_KEY_PERIOD,
    RECOMP_INPUT_HOST_KEY_BACKSLASH,
    RECOMP_INPUT_HOST_KEY_UP,
    RECOMP_INPUT_HOST_KEY_DOWN,
    RECOMP_INPUT_HOST_KEY_LEFT,
    RECOMP_INPUT_HOST_KEY_RIGHT,
    RECOMP_INPUT_HOST_KEY_RETURN,
    RECOMP_INPUT_HOST_KEY_BACKSPACE,
    RECOMP_INPUT_HOST_KEY_SHIFT,
    RECOMP_INPUT_HOST_KEY_CONTROL,
    RECOMP_INPUT_HOST_KEY_COUNT
} RecompInputHostKey;

/* As of port 0's last sample; false while the game window does not have the
   keyboard. */
bool recomp_input_host_key(RecompInputHostKey key);
/* True while a right-button drag that started over the game is held (it goes
   on outside the window), with the pointer's motion in pixels since the last
   call; zero on the call that starts a drag. */
bool recomp_input_host_mouse_drag(float *dx, float *dy);

/* A port's own layout (remapping): for each port and each Xbox control, the
   key (SDL scancode, or -1) and the gamepad button (SDL_GamepadButton, or -1)
   or trigger (SDL_GamepadAxis, or -1) that press it. With a layout set the
   built-in one is not used; each port's gamepad still drives its sticks.
   NULL returns to the built-in layout. Keys reach it from the SDL_GPU
   presenter. */
typedef enum RecompInputControl {
    RECOMP_INPUT_CONTROL_UP, RECOMP_INPUT_CONTROL_DOWN, RECOMP_INPUT_CONTROL_LEFT,
    RECOMP_INPUT_CONTROL_RIGHT, RECOMP_INPUT_CONTROL_START, RECOMP_INPUT_CONTROL_BACK,
    RECOMP_INPUT_CONTROL_LEFT_THUMB, RECOMP_INPUT_CONTROL_RIGHT_THUMB,
    /* Analog buttons, in the guest's order */
    RECOMP_INPUT_CONTROL_A, RECOMP_INPUT_CONTROL_B, RECOMP_INPUT_CONTROL_X, RECOMP_INPUT_CONTROL_Y,
    RECOMP_INPUT_CONTROL_BLACK, RECOMP_INPUT_CONTROL_WHITE,
    RECOMP_INPUT_CONTROL_LEFT_TRIGGER, RECOMP_INPUT_CONTROL_RIGHT_TRIGGER,
    RECOMP_INPUT_CONTROL_COUNT
} RecompInputControl;

typedef struct RecompInputHostBinding {
    int16_t scancode;
    int16_t pad_button;
    int16_t pad_axis;
} RecompInputHostBinding;

typedef struct RecompInputHostLayout {
    RecompInputHostBinding controls[RECOMP_INPUT_PORT_COUNT][RECOMP_INPUT_CONTROL_COUNT];
} RecompInputHostLayout;

/* Copies the layout. */
void recomp_input_host_set_layout(const RecompInputHostLayout *layout);

void recomp_input_host_set_vibration(
    uint32_t port,
    uint16_t left_motor,
    uint16_t right_motor);
void recomp_input_host_stop_vibration(void);

#ifdef __cplusplus
}
#endif

#endif
