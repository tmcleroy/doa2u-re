#ifndef DOA2U_RECOMP_D3D_DEVICE_MODEL_H
#define DOA2U_RECOMP_D3D_DEVICE_MODEL_H

#include "d3d_presenter.h"

#include <stdbool.h>
#include <stdint.h>

enum {
    RECOMP_D3D_OK = 0x00000000u,
    RECOMP_D3D_FAIL = 0x80004005u,
    RECOMP_D3D_OUT_OF_MEMORY = 0x8007000eu,
    RECOMP_D3D_INVALID_CALL = 0x8876086cu,
    RECOMP_D3D_SURFACE_REFERENCE_MASK = 0x0000ffffu,
    RECOMP_D3D_SURFACE_TYPE = 0x00050000u,
    RECOMP_D3D_SURFACE_BOUND = 0x00080000u,
    RECOMP_D3D_SURFACE_RETAIN_MASK = 0x00780000u,
};

typedef struct RecompD3dPresentationParameters {
    uint32_t back_buffer_width;
    uint32_t back_buffer_height;
    uint32_t back_buffer_format;
    uint32_t back_buffer_count;
    uint32_t multi_sample_type;
    uint32_t swap_effect;
    uint32_t device_window;
    uint32_t windowed;
    uint32_t enable_auto_depth_stencil;
    uint32_t auto_depth_stencil_format;
    uint32_t flags;
    uint32_t full_screen_refresh_rate;
    uint32_t full_screen_presentation_interval;
    uint32_t buffer_surfaces[3];
    uint32_t depth_stencil_surface;
} RecompD3dPresentationParameters;

typedef struct RecompD3dCreateRequest {
    uint32_t adapter;
    uint32_t device_type;
    uint32_t focus_window;
    uint32_t behavior_flags;
    RecompD3dPresentationParameters presentation;
} RecompD3dCreateRequest;

typedef struct RecompD3dSurfaceHeader {
    uint32_t common;
    uint32_t data;
    uint32_t lock;
    uint32_t format;
    uint32_t size;
    uint32_t parent;
} RecompD3dSurfaceHeader;

typedef struct RecompD3dViewport {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    float min_z;
    float max_z;
} RecompD3dViewport;

typedef enum RecompD3dDeviceError {
    RECOMP_D3D_DEVICE_OK,
    RECOMP_D3D_DEVICE_INVALID_ARGUMENT,
    RECOMP_D3D_DEVICE_UNSUPPORTED_REQUEST,
    RECOMP_D3D_DEVICE_ALREADY_CREATED,
    RECOMP_D3D_DEVICE_NOT_CREATED,
    RECOMP_D3D_DEVICE_INVALID_SURFACE,
    RECOMP_D3D_DEVICE_UNSUPPORTED_PARENT,
    RECOMP_D3D_DEVICE_REFERENCE_OVERFLOW,
    RECOMP_D3D_DEVICE_REFERENCE_UNDERFLOW,
    RECOMP_D3D_DEVICE_INVALID_VIEWPORT,
} RecompD3dDeviceError;

typedef struct RecompD3dDeviceState {
    bool created;
    uint32_t width;
    uint32_t height;
    /* Supersample factors from the multisample type (SDK +0x96c/+0x970):
       the back and depth buffers are width*scale[0] by height*scale[1]. */
    uint32_t scale[2];
    RecompD3dPresenterConfig presenter_config;
    RecompD3dSurfaceHeader color[2];
    RecompD3dSurfaceHeader depth;
    RecompD3dViewport viewport;
} RecompD3dDeviceState;

void recomp_d3d_device_reset(RecompD3dDeviceState *state);
bool recomp_d3d_create_request_supported(
    const RecompD3dCreateRequest *request);
RecompD3dDeviceError recomp_d3d_device_create(
    RecompD3dDeviceState *state,
    const RecompD3dCreateRequest *request,
    const RecompD3dSurfaceHeader color[2],
    const RecompD3dSurfaceHeader *depth);
RecompD3dDeviceError recomp_d3d_device_set_viewport(
    RecompD3dDeviceState *state,
    const RecompD3dViewport *viewport);
RecompD3dDeviceError recomp_d3d_device_swap_buffers(
    RecompD3dDeviceState *state);
RecompD3dDeviceError recomp_d3d_surface_add_ref(
    RecompD3dSurfaceHeader *surface,
    uint32_t *count);
RecompD3dDeviceError recomp_d3d_surface_release(
    RecompD3dSurfaceHeader *surface,
    uint32_t *count,
    bool *destroy_needed);

#endif
