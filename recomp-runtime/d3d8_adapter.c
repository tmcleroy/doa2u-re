/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "d3d8_adapter.h"
#include "d3d8_state_adapter.h"
#include "d3d8_resource_adapter.h"
#include "d3d_device_model.h"
#include "d3d_vertex_shader_model.h"
#include "d3d_frame_model.h"
#include "d3d_tile_model.h"
#include "d3d_vblank.h"
#include "stop_report.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* Verified against the selected D3D8 5849 library. No sister-title bindings. */
enum {
    DEVICE_GLOBAL = 0x003554f8u,
    DEVICE = 0x00355500u,
    DEVICE_SIZE = 0x24a0u,
    TARGET = 0x1a04u,
    DEPTH = 0x1a08u,
    BUFFER_COUNT = 0x1a10u,
    BUFFER_ARRAY = 0x1a14u,
    DEFAULT_DEPTH = 0x1a20u,
    COLOR_HEADERS = 0x1a24u,
    DEPTH_HEADER = 0x1a6cu,
    VIEWPORT = 0x0ee0u,
    FRAME_COUNTER = 0x2478u,
};

static RecompD3dDeviceState device_state;
static RecompD3dFrameState frame_state;
static RecompD3dPresenter *presenter;
static uint32_t submitted_draws;

static uint32_t argument(uint32_t entry, uint32_t index)
{
    return *recomp_memory_u32(entry + 4u + index * 4u);
}

static void load(void *destination, uint32_t address, size_t size)
{
    memcpy(destination, recomp_memory(address, size), size);
}

static void store(uint32_t address, const void *source, size_t size)
{
    memcpy(recomp_memory(address, size), source, size);
}

uint32_t recomp_d3d8_require_device(void)
{
    if (!device_state.created || *recomp_memory_u32(DEVICE_GLOBAL) != DEVICE) {
        recomp_stop(2, "d3d8:device-not-created");
    }
    return DEVICE;
}

void recomp_d3d8_submit(const RecompD3dPresenterCommand *command)
{
    const RecompD3dPresenterError error = recomp_d3d_presenter_submit(presenter, command);
    if (error == RECOMP_D3D_PRESENTER_CLOSED) recomp_stop(0, "host-window-closed");
    if (error != RECOMP_D3D_PRESENTER_OK) {
        recomp_stop(2, "d3d8:presenter:%u", (unsigned)error);
    }
    if (command->type == RECOMP_D3D_PRESENTER_COMMAND_DRAW && ++submitted_draws <= 4u) {
        fprintf(stderr, "recomp d3d8: Draw count=%u fvf=%08x vertices=%u texture=%u format=%02x\n",
            submitted_draws, command->data.draw.fvf, command->data.draw.vertex_count,
            command->data.draw.has_texture, command->data.draw.texture.format_byte);
    }
}

void recomp_d3d8_release_memory(uint32_t base, uint32_t size)
{
    if (presenter == NULL) return;
    const RecompD3dPresenterError error = recomp_d3d_presenter_release_memory(presenter, base, size);
    if (error != RECOMP_D3D_PRESENTER_OK) recomp_stop(2, "d3d8:release-memory:%u", (unsigned)error);
}

void recomp_d3d8_shutdown(void)
{
    if (presenter != NULL) {
        recomp_d3d_presenter_report_draw_textures();
        const RecompD3dPresenterError error = recomp_d3d_presenter_destroy(&presenter);
        fprintf(stderr, "recomp d3d8: presenter shutdown=%u\n", (unsigned)error);
    }
    recomp_d3d8_resource_reset();
    recomp_d3d_device_reset(&device_state);
    recomp_d3d_frame_reset(&frame_state);
    recomp_d3d8_state_reset();
    submitted_draws = 0u;
}

static uint32_t color_header(uint32_t index)
{
    return DEVICE + COLOR_HEADERS + index * sizeof(RecompD3dSurfaceHeader);
}

static void write_initial_device(void)
{
    recomp_d3d8_state_reset();
    memset(recomp_memory(DEVICE, DEVICE_SIZE), 0, DEVICE_SIZE);
    for (uint32_t i = 0; i < 2u; ++i) {
        store(color_header(i), &device_state.color[i], sizeof device_state.color[i]);
        *recomp_memory_u32(DEVICE + BUFFER_ARRAY + i * 4u) = color_header(i);
    }
    store(DEVICE + DEPTH_HEADER, &device_state.depth, sizeof device_state.depth);
    *recomp_memory_u32(DEVICE + TARGET) = color_header(0u);
    *recomp_memory_u32(DEVICE + DEPTH) = DEVICE + DEPTH_HEADER;
    *recomp_memory_u32(DEVICE + DEFAULT_DEPTH) = DEVICE + DEPTH_HEADER;
    *recomp_memory_u32(DEVICE + BUFFER_COUNT) = 2u;
    store(DEVICE + VIEWPORT, &device_state.viewport, sizeof device_state.viewport);
    *recomp_memory_u32(DEVICE + 0x954u) = device_state.width;
    *recomp_memory_u32(DEVICE + 0x958u) = device_state.height;
    for (uint32_t i = 0; i < 2u; ++i) {
        *recomp_memory_u32(DEVICE + 0x95cu + i * 4u) = 0x3f800000u;
        *recomp_memory_u32(DEVICE + 0x96cu + i * 4u) = 0x3f800000u;
    }
    for (uint32_t slot = 0u; slot < 10u; ++slot) {
        for (uint32_t diagonal = 0u; diagonal < 4u; ++diagonal) {
            *recomp_memory_u32(DEVICE + 0xc60u + slot * 64u + diagonal * 20u) = 0x3f800000u;
        }
    }
    *recomp_memory_u32(DEVICE + 0x938u) = 1u;
    *recomp_memory_u32(DEVICE + 0x196cu) = 0x11u;
    *recomp_memory_u32(DEVICE + 0x1970u) = 1u;
    *recomp_memory_u32(DEVICE + 0x940u) = 0x3f800000u;
    *recomp_memory_u32(DEVICE + 0x944u) = 0x3f800000u;
    *recomp_memory_u32(DEVICE + 0x8u) = 2u;
    for (uint32_t bank = 0u; bank < 2u; ++bank) {
        uint8_t *ramp = recomp_memory(DEVICE + 0x1e04u + bank * 0x300u, 0x300u);
        for (uint32_t plane = 0u; plane < 3u; ++plane) {
            for (uint32_t i = 0u; i < 256u; ++i) ramp[plane * 256u + i] = (uint8_t)i;
        }
    }
    *recomp_memory_u32(DEVICE + 0x2404u) = 1u;
    /* SDK creation selects FVF 4 then FVF 2 through the public setter; replay
       only the resulting shadows so later binds never read page zero. */
    uint32_t *declaration = (uint32_t *)(void *)recomp_memory(
        RECOMP_D3D_DEFAULT_DECLARATION, RECOMP_D3D_VERTEX_DECLARATION_SIZE);
    if (!recomp_d3d_build_fixed_function_declaration(2u, declaration)) {
        recomp_stop(2, "d3d8:default-declaration");
    }
    *recomp_memory_u32(DEVICE + 0x794u) = RECOMP_D3D_DEFAULT_DECLARATION;
    *recomp_memory_u32(DEVICE + 0x798u) = 2u;
    *recomp_memory_u32(DEVICE + 0x79cu) = 0u;
    *recomp_memory_u32(0x00355058u) |= 0x70u;
}

static void validate_storage(const RecompD3dSurfaceHeader *surface)
{
    const uint32_t pitch = ((surface->size >> 24u) + 1u) * 64u;
    const uint32_t height = ((surface->size >> 12u) & 0xfffu) + 1u;
    (void)recomp_memory(surface->data, (size_t)pitch * height);
}

static void create_device(void)
{
    const uint32_t entry = recomp_runtime.registers.esp;
    const uint32_t parameters = argument(entry, 4u);
    const uint32_t output = argument(entry, 5u);
    RecompD3dCreateRequest request = {
        .adapter = argument(entry, 0u), .device_type = argument(entry, 1u),
        .focus_window = argument(entry, 2u), .behavior_flags = argument(entry, 3u),
    };
    RecompD3dDeviceState candidate = {0};
    RecompD3dSurfaceHeader colors[2], depth;
    uint32_t result = RECOMP_D3D_INVALID_CALL;

    if (output != 0u) *recomp_memory_u32(output) = 0u;
    if (parameters == 0u || device_state.created) goto returned;
    load(&request.presentation, parameters, sizeof request.presentation);
    if (!recomp_d3d_create_request_supported(&request)) {
        const uint32_t *words = (const uint32_t *)(const void *)&request.presentation;
        for (uint32_t i = 0; i < sizeof request.presentation / 4u; ++i) {
            fprintf(stderr, "recomp d3d8: presentation[%u]=%08" PRIx32 "\n", i, words[i]);
        }
        recomp_stop(2, "d3d8:create-device-request");
    }
    for (uint32_t i = 0; i < 2u; ++i) {
        load(&colors[i], request.presentation.buffer_surfaces[i], sizeof colors[i]);
    }
    load(&depth, request.presentation.depth_stencil_surface, sizeof depth);
    const RecompD3dDeviceError error = recomp_d3d_device_create(&candidate, &request, colors, &depth);
    if (error != RECOMP_D3D_DEVICE_OK) recomp_stop(2, "d3d8:create-device-model:%u", (unsigned)error);
    for (uint32_t i = 0; i < 2u; ++i) validate_storage(&colors[i]);
    validate_storage(&depth);
    const RecompD3dPresenterError host_error = recomp_d3d_presenter_create(&candidate.presenter_config, &presenter);
    if (host_error != RECOMP_D3D_PRESENTER_OK) {
        result = host_error == RECOMP_D3D_PRESENTER_OUT_OF_MEMORY ? RECOMP_D3D_OUT_OF_MEMORY : RECOMP_D3D_FAIL;
        goto returned;
    }
    device_state = candidate;
    write_initial_device();
    if (recomp_d3d_frame_initialize(&frame_state, candidate.width, candidate.height) != RECOMP_D3D_FRAME_OK) {
        recomp_stop(2, "d3d8:frame-initialize");
    }
    recomp_d3d_vblank_reset();
    *recomp_memory_u32(DEVICE_GLOBAL) = DEVICE;
    if (output != 0u) *recomp_memory_u32(output) = DEVICE;
    result = RECOMP_D3D_OK;
    /* The API owns initialization of the native depth/stencil target too. */
    RecompD3dFrameResult clear = recomp_d3d_frame_clear(&frame_state, 0u, 0u,
        RECOMP_D3D_CLEAR_DEPTH | RECOMP_D3D_CLEAR_STENCIL, 0u, 0x3f800000u, 0u);
    recomp_d3d8_submit(&clear.command);
returned:
    fprintf(stderr, "recomp d3d8: CreateDevice result=%08" PRIx32 " device=%08" PRIx32 "\n",
        result, result == RECOMP_D3D_OK ? DEVICE : 0u);
    recomp_runtime.registers.eax = result;
    recomp_runtime.registers.esp = entry + 28u;
}

static uint32_t reference_surface(uint32_t address, bool release)
{
    RecompD3dSurfaceHeader surface;
    uint32_t count = 0u;
    bool destroy = false;
    RecompD3dDeviceError error;
    if (address == 0u) recomp_stop(2, "d3d8:resource-null");
    load(&surface, address, sizeof surface);
    error = release ? recomp_d3d_surface_release(&surface, &count, &destroy)
                    : recomp_d3d_surface_add_ref(&surface, &count);
    if (error != RECOMP_D3D_DEVICE_OK || destroy) {
        recomp_stop(2, "d3d8:resource-reference:%u:%u", (unsigned)error, (unsigned)destroy);
    }
    store(address, &surface, sizeof surface);
    return count;
}

static void add_ref(void)
{
    const uint32_t entry = recomp_runtime.registers.esp;
    recomp_runtime.registers.eax = reference_surface(argument(entry, 0u), false);
    recomp_runtime.registers.esp = entry + 8u;
}

static void release(void)
{
    const uint32_t entry = recomp_runtime.registers.esp;
    recomp_runtime.registers.eax = reference_surface(argument(entry, 0u), true);
    recomp_runtime.registers.esp = entry + 8u;
}

static void get_back_buffer(void)
{
    const uint32_t entry = recomp_runtime.registers.esp;
    const uint32_t index = argument(entry, 0u);
    recomp_d3d8_require_device();
    if (index != 0u && index != UINT32_MAX) recomp_stop(2, "d3d8:back-buffer-index:%u", index);
    const uint32_t address = color_header(index == UINT32_MAX ? 1u : 0u);
    reference_surface(address, false);
    recomp_runtime.registers.eax = address;
    recomp_runtime.registers.esp = entry + 8u;
}

static void get_depth_surface(void)
{
    const uint32_t entry = recomp_runtime.registers.esp;
    recomp_d3d8_require_device();
    const uint32_t address = *recomp_memory_u32(DEVICE + DEPTH);
    if (address != 0u) reference_surface(address, false);
    recomp_runtime.registers.eax = address;
    recomp_runtime.registers.esp = entry + 4u;
}

static void get_render_target(void)
{
    recomp_d3d8_require_device();
    const uint32_t address = *recomp_memory_u32(DEVICE + TARGET);
    if (address != 0u) reference_surface(address, false);
    recomp_runtime.registers.eax = address;
    recomp_runtime.registers.esp += 4u;
}

static void get_viewport(void)
{
    const uint32_t entry = recomp_runtime.registers.esp;
    const uint32_t output = argument(entry, 0u);
    recomp_d3d8_require_device();
    if (output == 0u) recomp_stop(2, "d3d8:viewport-null");
    load(&device_state.viewport, DEVICE + VIEWPORT, sizeof device_state.viewport);
    store(output, &device_state.viewport, sizeof device_state.viewport);
    recomp_runtime.registers.esp = entry + 8u;
}

static void set_viewport(void)
{
    const uint32_t entry = recomp_runtime.registers.esp;
    const uint32_t source = argument(entry, 0u);
    RecompD3dViewport viewport;
    recomp_d3d8_require_device();
    if (source != 0u) {
        load(&viewport, source, sizeof viewport);
        const RecompD3dDeviceError error = recomp_d3d_device_set_viewport(&device_state, &viewport);
        if (error != RECOMP_D3D_DEVICE_OK) recomp_stop(2, "d3d8:viewport:%u", (unsigned)error);
        store(DEVICE + VIEWPORT, &device_state.viewport, sizeof device_state.viewport);
    }
    recomp_runtime.registers.esp = entry + 8u;
}

static void set_tile(void)
{
    const uint32_t entry = recomp_runtime.registers.esp;
    const uint32_t index = argument(entry, 0u), source = argument(entry, 1u);
    uint32_t destination;
    RecompD3dTile tile, normalized;
    recomp_d3d8_require_device();
    if (!recomp_d3d_tile_entry_address(DEVICE, index, &destination)) recomp_stop(2, "d3d8:tile-index:%u", index);
    if (source != 0u) load(&tile, source, sizeof tile);
    recomp_d3d_set_tile(&normalized, source == 0u ? NULL : &tile);
    store(destination, &normalized, sizeof normalized);
    recomp_runtime.registers.esp = entry + 12u;
}

static void set_render_target(void)
{
    const uint32_t entry = recomp_runtime.registers.esp;
    uint32_t color = argument(entry, 0u), depth = argument(entry, 1u);
    recomp_d3d8_require_device();
    if (color == 0u) color = *recomp_memory_u32(DEVICE + TARGET);
    /* Offscreen/front-buffer target mapping requires a separate verified seam. */
    if (color != color_header(0u) || (depth != 0u && depth != DEVICE + DEPTH_HEADER)) {
        recomp_stop(2, "d3d8:set-render-target-unbound");
    }
    const uint32_t old_depth = *recomp_memory_u32(DEVICE + DEPTH);
    if (depth != old_depth) {
        if (old_depth != 0u) *recomp_memory_u32(old_depth) &= ~RECOMP_D3D_SURFACE_BOUND;
        if (depth != 0u) *recomp_memory_u32(depth) |= RECOMP_D3D_SURFACE_BOUND;
    }
    *recomp_memory_u32(DEVICE + TARGET) = color;
    *recomp_memory_u32(DEVICE + DEPTH) = depth;
    device_state.viewport = (RecompD3dViewport){0u, 0u, device_state.width, device_state.height, 0.0f, 1.0f};
    store(DEVICE + VIEWPORT, &device_state.viewport, sizeof device_state.viewport);
    recomp_runtime.registers.esp = entry + 12u;
}

static void clear(void)
{
    const uint32_t entry = recomp_runtime.registers.esp;
    recomp_d3d8_require_device();
    const uint32_t depth = *recomp_memory_u32(DEVICE + DEPTH);
    if (*recomp_memory_u32(DEVICE + TARGET) != color_header(0u) ||
        (depth != 0u && depth != DEVICE + DEPTH_HEADER)) {
        recomp_stop(2, "d3d8:clear-target-unbound");
    }
    load(&device_state.viewport, DEVICE + VIEWPORT, sizeof device_state.viewport);
    if (device_state.viewport.x != 0u || device_state.viewport.y != 0u ||
        device_state.viewport.width != device_state.width || device_state.viewport.height != device_state.height) {
        recomp_stop(2, "d3d8:clear-viewport-unbound");
    }
    RecompD3dFrameResult result = recomp_d3d_frame_clear(&frame_state,
        argument(entry, 0u), argument(entry, 1u), argument(entry, 2u),
        argument(entry, 3u), argument(entry, 4u), argument(entry, 5u));
    if (result.error != RECOMP_D3D_FRAME_OK) recomp_stop(2, "d3d8:clear:%u", (unsigned)result.error);
    if (depth == 0u) {
        result.command.data.clear.clear_depth = false;
        result.command.data.clear.clear_stencil = false;
        result.command.data.clear.target.no_depth = true;
    }
    recomp_d3d8_submit(&result.command);
    fprintf(stderr, "recomp d3d8: Clear flags=%08" PRIx32 "\n", argument(entry, 2u));
    recomp_runtime.registers.esp = entry + 28u;
}

static void swap(void)
{
    const uint32_t entry = recomp_runtime.registers.esp;
    recomp_d3d8_require_device();
    const uint32_t completed_bank = frame_state.swap_counter & 1u;
    const RecompD3dFrameResult result = recomp_d3d_frame_swap(&frame_state, argument(entry, 0u));
    if (result.error != RECOMP_D3D_FRAME_OK) recomp_stop(2, "d3d8:swap:%u", (unsigned)result.error);
    for (uint32_t i = 0; i < 2u; ++i) load(&device_state.color[i], color_header(i), sizeof device_state.color[i]);
    if (recomp_d3d_device_swap_buffers(&device_state) != RECOMP_D3D_DEVICE_OK) recomp_stop(2, "d3d8:swap-headers");
    recomp_d3d_wait_present();
    recomp_d3d8_submit(&result.command);
    *recomp_memory_u32(DEVICE + 0x2404u + completed_bank * 4u) = 0u;
    *recomp_memory_u32(DEVICE + 0x1de4u) = result.command.data.present.swap_counter;
    for (uint32_t i = 0; i < 2u; ++i) store(color_header(i), &device_state.color[i], sizeof device_state.color[i]);
    *recomp_memory_u32(DEVICE + FRAME_COUNTER) = result.command.data.present.swap_counter;
    recomp_runtime.registers.eax = result.command.data.present.swap_counter;
    recomp_runtime.registers.esp = entry + 8u;
    fprintf(stderr, "recomp d3d8: Swap frame=%u\n", result.command.data.present.swap_counter);
}

static void block_until_vertical_blank(void)
{
    recomp_d3d8_require_device();
    recomp_d3d_wait_vblank();
    recomp_runtime.registers.esp += 4u;
}

static void flush(bool wait)
{
    recomp_d3d8_require_device();
    const RecompD3dPresenterError error = recomp_d3d_presenter_flush(presenter, wait);
    if (error == RECOMP_D3D_PRESENTER_CLOSED) recomp_stop(0, "host-window-closed");
    if (error != RECOMP_D3D_PRESENTER_OK) recomp_stop(2, "d3d8:flush:%u", (unsigned)error);
}

static void kick(void) { flush(false); recomp_runtime.registers.esp += 4u; }
static void wait_idle(void) { flush(true); recomp_runtime.registers.esp += 4u; }

static void persist_display(void)
{
    /* The native window retains its displayed front buffer across reset. */
    flush(true);
    recomp_runtime.registers.eax = RECOMP_D3D_OK;
    recomp_runtime.registers.esp += 4u;
}

static void reset_device(void)
{
    recomp_d3d8_require_device();
    const uint32_t parameters = argument(recomp_runtime.registers.esp, 0u);
    RecompD3dCreateRequest request = {.device_type = 1u, .behavior_flags = 0x40u};
    RecompD3dDeviceState candidate = {0};
    RecompD3dSurfaceHeader colors[2], depth;
    if (parameters == 0u) recomp_stop(2, "d3d8:reset-pointer");
    load(&request.presentation, parameters, sizeof request.presentation);
    if (!recomp_d3d_create_request_supported(&request)) recomp_stop(2, "d3d8:reset-mode");
    for (uint32_t i = 0u; i < 2u; ++i)
        load(&colors[i], request.presentation.buffer_surfaces[i], sizeof colors[i]);
    load(&depth, request.presentation.depth_stencil_surface, sizeof depth);
    const RecompD3dDeviceError error = recomp_d3d_device_create(&candidate, &request, colors, &depth);
    if (error != RECOMP_D3D_DEVICE_OK) recomp_stop(2, "d3d8:reset-surfaces:%u", (unsigned)error);
    if (candidate.width != device_state.width || candidate.height != device_state.height)
        recomp_stop(2, "d3d8:reset-resize-unbound");
    for (uint32_t i = 0u; i < 2u; ++i) validate_storage(&colors[i]);
    validate_storage(&depth);
    flush(true);
    /* Same native attachments: retain displayed pixels and all non-RT state. */
    device_state = candidate;
    for (uint32_t i = 0u; i < 2u; ++i)
        store(color_header(i), &candidate.color[i], sizeof candidate.color[i]);
    store(DEVICE + DEPTH_HEADER, &candidate.depth, sizeof candidate.depth);
    *recomp_memory_u32(DEVICE + TARGET) = color_header(0u);
    *recomp_memory_u32(DEVICE + DEPTH) = DEVICE + DEPTH_HEADER;
    *recomp_memory_u32(DEVICE + DEFAULT_DEPTH) = DEVICE + DEPTH_HEADER;
    store(DEVICE + VIEWPORT, &candidate.viewport, sizeof candidate.viewport);
    *recomp_memory_u32(0x35545cu) = request.presentation.full_screen_presentation_interval;
    *recomp_memory_u32(DEVICE + 0x196cu) = request.presentation.multi_sample_type;
    *recomp_memory_u32(DEVICE + 0x1970u) = request.presentation.swap_effect;
    RecompD3dFrameResult clear = recomp_d3d_frame_clear(&frame_state, 0u, 0u,
        RECOMP_D3D_CLEAR_DEPTH | RECOMP_D3D_CLEAR_STENCIL, 0u, 0x3f800000u, 0u);
    if (clear.error != RECOMP_D3D_FRAME_OK) recomp_stop(2, "d3d8:reset-clear");
    recomp_d3d8_submit(&clear.command);
    recomp_runtime.registers.eax = RECOMP_D3D_OK;
    recomp_runtime.registers.esp += 8u;
    fprintf(stderr, "recomp d3d8: Reset %ux%u interval=%08x frame=%u\n",
        candidate.width, candidate.height, request.presentation.full_screen_presentation_interval,
        frame_state.swap_counter);
}

/* Every SDK setter that is still unreplaced reserves console push-buffer
   space here, then waits forever for GPU hardware this host lacks. Stop with
   the caller's return address so the missing API is named immediately. */
static void push_buffer_unbound(void)
{
    recomp_stop(2, "d3d8:push-buffer-unbound:ret=%08" PRIx32,
        *recomp_memory_u32(recomp_runtime.registers.esp));
}

RecompFunction recomp_d3d8_lookup_manual(uint32_t address)
{
    switch (address) {
    case 0x0034a470u: return create_device;
    case 0x00347e80u: return add_ref;
    case 0x00347ec0u: return release;
    case 0x00344a50u: return get_back_buffer;
    case 0x00344df0u: return get_depth_surface;
    case 0x00344dd0u: return get_render_target;
    case 0x00344360u: return get_viewport;
    case 0x00344e10u: return set_viewport;
    case 0x003447d0u: return set_render_target;
    case 0x003459e0u: return set_tile;
    case 0x00347920u: return clear;
    case 0x0034af80u: return swap;
    case 0x0034a310u: return push_buffer_unbound;
    case 0x00344440u: return kick;
    case 0x00344420u: case 0x0034a370u: return wait_idle;
    case 0x00345d70u: return persist_display;
    case 0x00344720u: return reset_device;
    /* CRI's no-argument tail aliases bypass generated callee lookup. */
    case 0x002fc300u: case 0x002fc5a0u: case 0x003444c0u: return block_until_vertical_blank;
    default: return NULL;
    }
}
