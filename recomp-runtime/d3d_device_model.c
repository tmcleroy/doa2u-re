#include "d3d_device_model.h"

#include <math.h>
#include <stddef.h>

void recomp_d3d_device_reset(RecompD3dDeviceState *state)
{
    if (state != NULL) {
        *state = (RecompD3dDeviceState){0};
    }
}

bool recomp_d3d_create_request_supported(
    const RecompD3dCreateRequest *request)
{
    const RecompD3dPresentationParameters *presentation;

    if (request == NULL) {
        return false;
    }
    presentation = &request->presentation;
    /* shortcut: only the verified startup mode and its 2x horizontal
       supersample Reset (0x2021), expand after API validation. */
    return request->adapter == 0u && request->device_type == 1u &&
        request->focus_window == 0u && request->behavior_flags == 0x40u &&
        presentation->back_buffer_width == 720u &&
        presentation->back_buffer_height == 480u &&
        presentation->back_buffer_format == 0x12u &&
        presentation->back_buffer_count == 1u &&
        (presentation->multi_sample_type == 0x11u ||
         presentation->multi_sample_type == 0x2021u) &&
        presentation->swap_effect == 1u &&
        presentation->device_window == 0u && presentation->windowed == 0u &&
        presentation->enable_auto_depth_stencil == 1u &&
        presentation->auto_depth_stencil_format == 0x2eu &&
        presentation->flags == 0u &&
        presentation->full_screen_refresh_rate == 0u &&
        (presentation->full_screen_presentation_interval == 0x80000001u ||
         presentation->full_screen_presentation_interval == 1u) &&
        presentation->buffer_surfaces[0] != 0u &&
        (presentation->buffer_surfaces[0] & 3u) == 0u &&
        presentation->buffer_surfaces[0] <=
            UINT32_MAX - (sizeof(RecompD3dSurfaceHeader) - 1u) &&
        presentation->buffer_surfaces[1] != 0u &&
        (presentation->buffer_surfaces[1] & 3u) == 0u &&
        presentation->buffer_surfaces[1] <=
            UINT32_MAX - (sizeof(RecompD3dSurfaceHeader) - 1u) &&
        presentation->buffer_surfaces[2] == 0u &&
        presentation->depth_stencil_surface != 0u &&
        (presentation->depth_stencil_surface & 3u) == 0u &&
        presentation->depth_stencil_surface <=
            UINT32_MAX - (sizeof(RecompD3dSurfaceHeader) - 1u);
}

static RecompD3dDeviceError validate_resource(
    const RecompD3dSurfaceHeader *surface)
{
    const uint32_t allowed = RECOMP_D3D_SURFACE_REFERENCE_MASK |
        0x00070000u | RECOMP_D3D_SURFACE_RETAIN_MASK;

    if (surface == NULL) {
        return RECOMP_D3D_DEVICE_INVALID_ARGUMENT;
    }
    if (surface->parent != 0u) {
        return RECOMP_D3D_DEVICE_UNSUPPORTED_PARENT;
    }
    if ((surface->common & 0x00070000u) != RECOMP_D3D_SURFACE_TYPE ||
        (surface->common & ~allowed) != 0u) {
        return RECOMP_D3D_DEVICE_INVALID_SURFACE;
    }
    return RECOMP_D3D_DEVICE_OK;
}

RecompD3dDeviceError recomp_d3d_surface_add_ref(
    RecompD3dSurfaceHeader *surface,
    uint32_t *count)
{
    RecompD3dDeviceError error;

    if (count == NULL) {
        return RECOMP_D3D_DEVICE_INVALID_ARGUMENT;
    }
    error = validate_resource(surface);
    if (error != RECOMP_D3D_DEVICE_OK) {
        return error;
    }
    if ((surface->common & RECOMP_D3D_SURFACE_REFERENCE_MASK) ==
        RECOMP_D3D_SURFACE_REFERENCE_MASK) {
        return RECOMP_D3D_DEVICE_REFERENCE_OVERFLOW;
    }
    ++surface->common;
    *count = surface->common & RECOMP_D3D_SURFACE_REFERENCE_MASK;
    return RECOMP_D3D_DEVICE_OK;
}

RecompD3dDeviceError recomp_d3d_surface_release(
    RecompD3dSurfaceHeader *surface,
    uint32_t *count,
    bool *destroy_needed)
{
    RecompD3dDeviceError error;

    if (count == NULL || destroy_needed == NULL) {
        return RECOMP_D3D_DEVICE_INVALID_ARGUMENT;
    }
    error = validate_resource(surface);
    if (error != RECOMP_D3D_DEVICE_OK) {
        return error;
    }
    if ((surface->common & RECOMP_D3D_SURFACE_REFERENCE_MASK) == 0u) {
        return RECOMP_D3D_DEVICE_REFERENCE_UNDERFLOW;
    }
    --surface->common;
    *count = surface->common & RECOMP_D3D_SURFACE_REFERENCE_MASK;
    *destroy_needed = *count == 0u &&
        (surface->common & RECOMP_D3D_SURFACE_RETAIN_MASK) == 0u;
    return RECOMP_D3D_DEVICE_OK;
}

static RecompD3dDeviceError validate_surface(
    const RecompD3dSurfaceHeader *surface,
    uint32_t width,
    uint32_t height,
    uint32_t format)
{
    RecompD3dDeviceError error = validate_resource(surface);
    uint32_t pitch;
    uint32_t span;

    if (error != RECOMP_D3D_DEVICE_OK) {
        return error;
    }
    if ((surface->common & RECOMP_D3D_SURFACE_RETAIN_MASK) != 0u ||
        (surface->common & RECOMP_D3D_SURFACE_REFERENCE_MASK) == 0u ||
        surface->data == 0u || (surface->data & 3u) != 0u ||
        surface->lock != 0u ||
        surface->format != (0x00010029u | (format << 8u)) ||
        (surface->size & 0xfffu) + 1u != width ||
        ((surface->size >> 12u) & 0xfffu) + 1u != height) {
        return RECOMP_D3D_DEVICE_INVALID_SURFACE;
    }
    pitch = ((surface->size >> 24u) + 1u) * 64u;
    span = (height - 1u) * pitch + width * 4u;
    if (pitch < width * 4u || surface->data > UINT32_MAX - (span - 1u)) {
        return RECOMP_D3D_DEVICE_INVALID_SURFACE;
    }
    if ((surface->common & RECOMP_D3D_SURFACE_REFERENCE_MASK) ==
        RECOMP_D3D_SURFACE_REFERENCE_MASK) {
        return RECOMP_D3D_DEVICE_REFERENCE_OVERFLOW;
    }
    return RECOMP_D3D_DEVICE_OK;
}

RecompD3dDeviceError recomp_d3d_device_create(
    RecompD3dDeviceState *state,
    const RecompD3dCreateRequest *request,
    const RecompD3dSurfaceHeader color[2],
    const RecompD3dSurfaceHeader *depth)
{
    RecompD3dDeviceState created = {0};
    RecompD3dDeviceError error;

    if (state == NULL || request == NULL || color == NULL || depth == NULL) {
        return RECOMP_D3D_DEVICE_INVALID_ARGUMENT;
    }
    if (state->created) {
        return RECOMP_D3D_DEVICE_ALREADY_CREATED;
    }
    if (!recomp_d3d_create_request_supported(request)) {
        return RECOMP_D3D_DEVICE_UNSUPPORTED_REQUEST;
    }
    created.width = request->presentation.back_buffer_width;
    created.height = request->presentation.back_buffer_height;
    /* CDevice_InitializeFrameBuffers (0x34be80) decodes the factors this way. */
    created.scale[0] = (request->presentation.multi_sample_type >> 4u) & 15u;
    created.scale[1] = request->presentation.multi_sample_type & 15u;
    for (uint32_t i = 0u; i < 2u; ++i) {
        /* Buffer 0 is the (supersampled) back buffer, buffer 1 the front. */
        const uint32_t scale_x = i == 0u ? created.scale[0] : 1u;
        const uint32_t scale_y = i == 0u ? created.scale[1] : 1u;
        error = validate_surface(&color[i], created.width * scale_x,
            created.height * scale_y, 0x12u);
        if (error != RECOMP_D3D_DEVICE_OK) {
            return error;
        }
        created.color[i] = color[i];
        ++created.color[i].common;
    }
    error = validate_surface(depth, created.width * created.scale[0],
        created.height * created.scale[1], 0x2eu);
    if (error != RECOMP_D3D_DEVICE_OK) {
        return error;
    }
    created.depth = *depth;
    ++created.depth.common;
    created.color[0].common |= RECOMP_D3D_SURFACE_BOUND;
    created.depth.common |= RECOMP_D3D_SURFACE_BOUND;
    created.viewport = (RecompD3dViewport){
        0u, 0u, created.width, created.height, 0.0f, 1.0f,
    };
    created.presenter_config = (RecompD3dPresenterConfig){
        created.width, created.height,
        RECOMP_D3D_PRESENTER_COLOR_FORMAT_BGRA8_UNORM,
        RECOMP_D3D_PRESENTER_DEPTH_FORMAT_D24S8,
    };
    created.created = true;
    *state = created;
    return RECOMP_D3D_DEVICE_OK;
}

RecompD3dDeviceError recomp_d3d_device_set_viewport(
    RecompD3dDeviceState *state,
    const RecompD3dViewport *viewport)
{
    RecompD3dViewport clamped;

    if (state == NULL) {
        return RECOMP_D3D_DEVICE_INVALID_ARGUMENT;
    }
    if (!state->created) {
        return RECOMP_D3D_DEVICE_NOT_CREATED;
    }
    if (viewport == NULL) {
        return RECOMP_D3D_DEVICE_OK;
    }
    if (!isfinite(viewport->min_z) || !isfinite(viewport->max_z) ||
        viewport->min_z < 0.0f || viewport->max_z > 1.0f ||
        viewport->min_z > viewport->max_z ||
        viewport->x > state->width || viewport->y > state->height) {
        return RECOMP_D3D_DEVICE_INVALID_VIEWPORT;
    }
    clamped = *viewport;
    /* shortcut: reject outside origins, extend after SDK underflow validation. */
    if (clamped.width > state->width - clamped.x) {
        clamped.width = state->width - clamped.x;
    }
    if (clamped.height > state->height - clamped.y) {
        clamped.height = state->height - clamped.y;
    }
    state->viewport = clamped;
    return RECOMP_D3D_DEVICE_OK;
}

RecompD3dDeviceError recomp_d3d_device_swap_buffers(
    RecompD3dDeviceState *state)
{
    uint32_t data;
    uint32_t lock;

    if (state == NULL) {
        return RECOMP_D3D_DEVICE_INVALID_ARGUMENT;
    }
    if (!state->created) {
        return RECOMP_D3D_DEVICE_NOT_CREATED;
    }
    /* Swap (0x34ac40) filters a supersampled back buffer into the single
       front buffer; with two buffers no storage rotates. */
    if (state->scale[0] != 1u || state->scale[1] != 1u) {
        return RECOMP_D3D_DEVICE_OK;
    }
    data = state->color[0].data;
    lock = state->color[0].lock;
    state->color[0].data = state->color[1].data;
    state->color[0].lock = state->color[1].lock;
    state->color[1].data = data;
    state->color[1].lock = lock;
    return RECOMP_D3D_DEVICE_OK;
}
