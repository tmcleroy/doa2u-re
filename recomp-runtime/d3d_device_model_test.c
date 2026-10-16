#include "d3d_device_model.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

_Static_assert(sizeof(RecompD3dPresentationParameters) == 17u * 4u,
    "presentation parameters must contain 17 DWORDs");
_Static_assert(sizeof(RecompD3dCreateRequest) == 21u * 4u,
    "creation request must contain 21 DWORDs");
_Static_assert(sizeof(RecompD3dSurfaceHeader) == 6u * 4u,
    "surface header must contain six DWORDs");
_Static_assert(sizeof(RecompD3dViewport) == 6u * 4u,
    "viewport must contain six DWORDs");

static int expect(const char *name, bool condition)
{
    if (!condition) {
        fprintf(stderr, "D3D device: %s failed\n", name);
        return 0;
    }
    return 1;
}

static const RecompD3dCreateRequest request = {
    .adapter = 0u,
    .device_type = 1u,
    .focus_window = 0u,
    .behavior_flags = 0x40u,
    .presentation = {
        .back_buffer_width = 720u,
        .back_buffer_height = 480u,
        .back_buffer_format = 0x12u,
        .back_buffer_count = 1u,
        .multi_sample_type = 0x11u,
        .swap_effect = 1u,
        .enable_auto_depth_stencil = 1u,
        .auto_depth_stencil_format = 0x2eu,
        .full_screen_presentation_interval = 0x80000001u,
        .buffer_surfaces = {0x1000u, 0x2000u, 0u},
        .depth_stencil_surface = 0x3000u,
    },
};

static const RecompD3dSurfaceHeader color[2] = {
    {0x00050001u, 0x00100000u, 0u, 0x00011229u, 0x2f1df2cfu, 0u},
    {0x00050001u, 0x00300000u, 0u, 0x00011229u, 0x2f1df2cfu, 0u},
};
static const RecompD3dSurfaceHeader depth = {
    0x00050001u, 0x00500000u, 0u, 0x00012e29u, 0x2f1df2cfu, 0u,
};

int recomp_d3d_device_model_test(void)
{
    static const struct {
        const char *name;
        size_t offset;
        uint32_t value;
        RecompD3dDeviceError error;
    } invalid_surfaces[] = {
        {"zero references", offsetof(RecompD3dSurfaceHeader, common),
            0x50000u, RECOMP_D3D_DEVICE_INVALID_SURFACE},
        {"wrong resource type", offsetof(RecompD3dSurfaceHeader, common),
            0x40001u, RECOMP_D3D_DEVICE_INVALID_SURFACE},
        {"unknown resource flags", offsetof(RecompD3dSurfaceHeader, common),
            0x80050001u, RECOMP_D3D_DEVICE_INVALID_SURFACE},
        {"already bound source", offsetof(RecompD3dSurfaceHeader, common),
            0xd0001u, RECOMP_D3D_DEVICE_INVALID_SURFACE},
        {"owner reference overflow", offsetof(RecompD3dSurfaceHeader, common),
            0x5ffffu, RECOMP_D3D_DEVICE_REFERENCE_OVERFLOW},
        {"null storage", offsetof(RecompD3dSurfaceHeader, data),
            0u, RECOMP_D3D_DEVICE_INVALID_SURFACE},
        {"unaligned storage", offsetof(RecompD3dSurfaceHeader, data),
            0x100001u, RECOMP_D3D_DEVICE_INVALID_SURFACE},
        {"storage overflow", offsetof(RecompD3dSurfaceHeader, data),
            0xfffffffcu, RECOMP_D3D_DEVICE_INVALID_SURFACE},
        {"locked source", offsetof(RecompD3dSurfaceHeader, lock),
            1u, RECOMP_D3D_DEVICE_INVALID_SURFACE},
        {"depth format as color", offsetof(RecompD3dSurfaceHeader, format),
            0x12e29u, RECOMP_D3D_DEVICE_INVALID_SURFACE},
        {"unsupported mip count", offsetof(RecompD3dSurfaceHeader, format),
            0x21229u, RECOMP_D3D_DEVICE_INVALID_SURFACE},
        {"swizzled source", offsetof(RecompD3dSurfaceHeader, size),
            0u, RECOMP_D3D_DEVICE_INVALID_SURFACE},
        {"wrong width", offsetof(RecompD3dSurfaceHeader, size),
            0x2f1df2ceu, RECOMP_D3D_DEVICE_INVALID_SURFACE},
        {"wrong height", offsetof(RecompD3dSurfaceHeader, size),
            0x2f1de2cfu, RECOMP_D3D_DEVICE_INVALID_SURFACE},
        {"short pitch", offsetof(RecompD3dSurfaceHeader, size),
            0x001df2cfu, RECOMP_D3D_DEVICE_INVALID_SURFACE},
        {"parent resource", offsetof(RecompD3dSurfaceHeader, parent),
            0x4000u, RECOMP_D3D_DEVICE_UNSUPPORTED_PARENT},
    };
    RecompD3dDeviceState state;
    RecompD3dDeviceState saved;
    RecompD3dCreateRequest changed_request;
    RecompD3dSurfaceHeader changed_color[2];
    RecompD3dSurfaceHeader surface;
    RecompD3dViewport viewport;
    uint32_t count = 0u;
    bool destroy_needed = false;
    int passed = 1;

    recomp_d3d_device_reset(&state);
    saved = state;
    passed &= expect("null request rejected",
        !recomp_d3d_create_request_supported(NULL));
    passed &= expect("startup request supported",
        recomp_d3d_create_request_supported(&request));
    for (size_t i = 0u; i < sizeof request / 4u; ++i) {
        uint32_t word;

        changed_request = request;
        memcpy(&word, (const unsigned char *)&changed_request + i * 4u, 4u);
        word ^= 1u;
        memcpy((unsigned char *)&changed_request + i * 4u, &word, 4u);
        passed &= expect("unsupported request field",
            !recomp_d3d_create_request_supported(&changed_request) &&
            recomp_d3d_device_create(
                &state, &changed_request, color, &depth) ==
                RECOMP_D3D_DEVICE_UNSUPPORTED_REQUEST &&
            memcmp(&state, &saved, sizeof state) == 0);
    }
    changed_request = request;
    changed_request.presentation.buffer_surfaces[0] = 0u;
    passed &= expect("allocation path unsupported",
        !recomp_d3d_create_request_supported(&changed_request));
    changed_request = request;
    changed_request.presentation.depth_stencil_surface = 0xfffffffcu;
    passed &= expect("header address overflow",
        !recomp_d3d_create_request_supported(&changed_request));
    passed &= expect("null state rejected",
        recomp_d3d_device_create(NULL, &request, color, &depth) ==
            RECOMP_D3D_DEVICE_INVALID_ARGUMENT);
    passed &= expect("null color rejected",
        recomp_d3d_device_create(&state, &request, NULL, &depth) ==
            RECOMP_D3D_DEVICE_INVALID_ARGUMENT);
    passed &= expect("null depth rejected",
        recomp_d3d_device_create(&state, &request, color, NULL) ==
            RECOMP_D3D_DEVICE_INVALID_ARGUMENT);
    passed &= expect("viewport before creation",
        recomp_d3d_device_set_viewport(&state, NULL) ==
            RECOMP_D3D_DEVICE_NOT_CREATED);
    passed &= expect("swap before creation",
        recomp_d3d_device_swap_buffers(&state) == RECOMP_D3D_DEVICE_NOT_CREATED);
    for (size_t i = 0u;
         i < sizeof invalid_surfaces / sizeof invalid_surfaces[0]; ++i) {
        memcpy(changed_color, color, sizeof color);
        memcpy((unsigned char *)&changed_color[1] + invalid_surfaces[i].offset,
            &invalid_surfaces[i].value, 4u);
        passed &= expect(invalid_surfaces[i].name,
            recomp_d3d_device_create(&state, &request, changed_color, &depth) ==
                invalid_surfaces[i].error &&
            memcmp(&state, &saved, sizeof state) == 0);
    }
    surface = depth;
    surface.format = color[0].format;
    passed &= expect("wrong depth format",
        recomp_d3d_device_create(&state, &request, color, &surface) ==
            RECOMP_D3D_DEVICE_INVALID_SURFACE);
    memcpy(changed_color, color, sizeof color);
    changed_color[0].data = UINT32_MAX - (479u * 3072u + 720u * 4u - 1u);
    passed &= expect("highest fitting aligned storage",
        recomp_d3d_device_create(&state, &request, changed_color, &depth) ==
            RECOMP_D3D_DEVICE_OK);
    recomp_d3d_device_reset(&state);
    passed &= expect("startup creation",
        recomp_d3d_device_create(&state, &request, color, &depth) ==
            RECOMP_D3D_DEVICE_OK && state.created &&
        state.width == 720u && state.height == 480u &&
        state.presenter_config.width == 720u &&
        state.presenter_config.height == 480u &&
        state.presenter_config.color_format ==
            RECOMP_D3D_PRESENTER_COLOR_FORMAT_BGRA8_UNORM &&
        state.presenter_config.depth_format ==
            RECOMP_D3D_PRESENTER_DEPTH_FORMAT_D24S8);
    passed &= expect("embedded owner references and binding",
        state.color[0].common == 0xd0002u &&
        state.color[1].common == 0x50002u && state.depth.common == 0xd0002u &&
        color[0].common == 0x50001u && color[1].common == 0x50001u &&
        depth.common == 0x50001u);
    saved = state;
    passed &= expect("duplicate creation preserves state",
        recomp_d3d_device_create(&state, &request, color, &depth) ==
            RECOMP_D3D_DEVICE_ALREADY_CREATED &&
        memcmp(&state, &saved, sizeof state) == 0);
    passed &= expect("initial viewport",
        state.viewport.x == 0u && state.viewport.y == 0u &&
        state.viewport.width == 720u && state.viewport.height == 480u &&
        state.viewport.min_z == 0.0f && state.viewport.max_z == 1.0f);
    passed &= expect("null viewport preserves state",
        recomp_d3d_device_set_viewport(&state, NULL) == RECOMP_D3D_DEVICE_OK &&
        memcmp(&state, &saved, sizeof state) == 0);
    viewport = (RecompD3dViewport){700u, 460u, UINT32_MAX, UINT32_MAX, 0.2f, 0.8f};
    passed &= expect("viewport clips without integer wrap",
        recomp_d3d_device_set_viewport(&state, &viewport) ==
            RECOMP_D3D_DEVICE_OK &&
        state.viewport.x == 700u && state.viewport.y == 460u &&
        state.viewport.width == 20u && state.viewport.height == 20u &&
        state.viewport.min_z == 0.2f && state.viewport.max_z == 0.8f);
    viewport.x = UINT32_MAX;
    viewport.y = UINT32_MAX;
    saved = state;
    passed &= expect("outside viewport origin rejected without mutation",
        recomp_d3d_device_set_viewport(&state, &viewport) ==
            RECOMP_D3D_DEVICE_INVALID_VIEWPORT &&
        memcmp(&state, &saved, sizeof state) == 0);
    viewport.x = 0u;
    viewport.y = 0u;
    viewport.min_z = NAN;
    passed &= expect("nonfinite viewport depth rejected",
        recomp_d3d_device_set_viewport(&state, &viewport) ==
            RECOMP_D3D_DEVICE_INVALID_VIEWPORT &&
        memcmp(&state, &saved, sizeof state) == 0);
    viewport.min_z = 0.0f;
    viewport.max_z = INFINITY;
    passed &= expect("infinite viewport depth rejected",
        recomp_d3d_device_set_viewport(&state, &viewport) ==
            RECOMP_D3D_DEVICE_INVALID_VIEWPORT);
    viewport.max_z = 1.0f;
    viewport.min_z = -0.1f;
    passed &= expect("negative viewport depth rejected",
        recomp_d3d_device_set_viewport(&state, &viewport) ==
            RECOMP_D3D_DEVICE_INVALID_VIEWPORT);
    viewport.min_z = 0.8f;
    viewport.max_z = 0.2f;
    passed &= expect("reversed viewport depth rejected",
        recomp_d3d_device_set_viewport(&state, &viewport) ==
            RECOMP_D3D_DEVICE_INVALID_VIEWPORT);
    surface = state.color[0];
    passed &= expect("surface getter adds reference",
        recomp_d3d_surface_add_ref(&state.color[0], &count) ==
            RECOMP_D3D_DEVICE_OK && count == 3u);
    passed &= expect("getter release preserves bound surface",
        recomp_d3d_surface_release(&state.color[0], &count, &destroy_needed) ==
            RECOMP_D3D_DEVICE_OK && count == 2u && !destroy_needed &&
        memcmp(&state.color[0], &surface, sizeof surface) == 0);
    for (uint32_t retained = 0x80000u;
         retained <= 0x400000u; retained <<= 1u) {
        surface = color[0];
        surface.common |= retained;
        passed &= expect("retain flag prevents destruction",
            recomp_d3d_surface_release(&surface, &count, &destroy_needed) ==
                RECOMP_D3D_DEVICE_OK && count == 0u && !destroy_needed &&
            surface.common == (0x50000u | retained));
    }
    surface = color[0];
    passed &= expect("unretained final release reports destruction",
        recomp_d3d_surface_release(&surface, &count, &destroy_needed) ==
            RECOMP_D3D_DEVICE_OK && count == 0u && destroy_needed);
    passed &= expect("zero reference release rejected",
        recomp_d3d_surface_release(&surface, &count, &destroy_needed) ==
            RECOMP_D3D_DEVICE_REFERENCE_UNDERFLOW && surface.common == 0x50000u);
    surface.common = 0x5ffffu;
    count = 123u;
    passed &= expect("reference overflow preserves inputs",
        recomp_d3d_surface_add_ref(&surface, &count) ==
            RECOMP_D3D_DEVICE_REFERENCE_OVERFLOW &&
        surface.common == 0x5ffffu && count == 123u);
    surface = color[0];
    surface.parent = 0x4000u;
    passed &= expect("parent reference unsupported before mutation",
        recomp_d3d_surface_add_ref(&surface, &count) ==
            RECOMP_D3D_DEVICE_UNSUPPORTED_PARENT &&
        recomp_d3d_surface_release(&surface, &count, &destroy_needed) ==
            RECOMP_D3D_DEVICE_UNSUPPORTED_PARENT && surface.common == 0x50001u);
    surface.parent = 0u;
    surface.common |= 0x80000000u;
    passed &= expect("unknown reference flags rejected",
        recomp_d3d_surface_add_ref(&surface, &count) ==
            RECOMP_D3D_DEVICE_INVALID_SURFACE);
    state.color[0].lock = 0x11111111u;
    state.color[1].lock = 0x22222222u;
    saved = state;
    passed &= expect("swap storage preserves header identities",
        recomp_d3d_device_swap_buffers(&state) == RECOMP_D3D_DEVICE_OK &&
        state.color[0].data == saved.color[1].data &&
        state.color[1].data == saved.color[0].data &&
        state.color[0].lock == saved.color[1].lock &&
        state.color[1].lock == saved.color[0].lock &&
        state.color[0].common == saved.color[0].common &&
        state.color[1].common == saved.color[1].common &&
        memcmp(&state.depth, &saved.depth, sizeof state.depth) == 0 &&
        memcmp(&state.viewport, &saved.viewport, sizeof state.viewport) == 0);
    passed &= expect("second swap restores storage",
        recomp_d3d_device_swap_buffers(&state) == RECOMP_D3D_DEVICE_OK &&
        memcmp(&state, &saved, sizeof state) == 0);
    /* 0x2021: 2x horizontal supersample back and depth buffers, 720 front. */
    changed_request = request;
    changed_request.presentation.multi_sample_type = 0x2021u;
    memcpy(changed_color, color, sizeof color);
    surface = depth;
    recomp_d3d_device_reset(&state);
    passed &= expect("supersample rejects logical-size back buffer",
        recomp_d3d_device_create(&state, &changed_request, changed_color, &surface) ==
            RECOMP_D3D_DEVICE_INVALID_SURFACE && !state.created);
    changed_color[0].size = 0x5f1df59fu;
    passed &= expect("supersample rejects logical-size depth",
        recomp_d3d_device_create(&state, &changed_request, changed_color, &surface) ==
            RECOMP_D3D_DEVICE_INVALID_SURFACE && !state.created);
    surface.size = 0x5f1df59fu;
    passed &= expect("supersample keeps logical size and decodes scale",
        recomp_d3d_device_create(&state, &changed_request, changed_color, &surface) ==
            RECOMP_D3D_DEVICE_OK && state.width == 720u && state.height == 480u &&
        state.scale[0] == 2u && state.scale[1] == 1u &&
        state.presenter_config.width == 720u && state.viewport.width == 720u);
    saved = state;
    passed &= expect("supersample swap keeps the filtered front buffer",
        recomp_d3d_device_swap_buffers(&state) == RECOMP_D3D_DEVICE_OK &&
        memcmp(&state, &saved, sizeof state) == 0);
    changed_request.presentation.multi_sample_type = 0x1021u;
    passed &= expect("other multisample types stay unsupported",
        !recomp_d3d_create_request_supported(&changed_request));
    recomp_d3d_device_reset(&state);
    passed &= expect("reset clears creation", !state.created);
    return passed;
}
