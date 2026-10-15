/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "d3d8_draw_adapter.h"
#include "d3d8_adapter.h"
#include "d3d8_resource_adapter.h"
#include "d3d8_state_adapter.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL draw line %d: %s\n", __LINE__, #condition); return 1; \
} } while (0)

enum { DEVICE = 0x355500u, STACK = 0x1000u, INPUT = 0x2000u,
    OUTPUT = 0x3000u, VERTICES = 0x4000u, INDICES = 0x5000u, BUFFER = 0x6000u,
    STAGES = 0x355060u };
RecompRuntime recomp_runtime;
uint8_t *recomp_fast_ram;
static uint8_t ram[RECOMP_XBOX_RAM_SIZE];
static RecompD3dRenderStateModel state;
static RecompD3dPresenterCommand submitted;
static uint16_t captured_indices[65536];
static uint32_t allocated = 0x360000u, freed, submits;
static bool fail_allocation;
static jmp_buf stopped;
static char stop_reason[128];
static RecompD3dTextureDesc texture;
static RecompD3dPresenterTarget target;
void recomp_d3d8_target(RecompD3dPresenterTarget *out) { *out = target; }

void recomp_stop(int code, const char *format, ...)
{
    va_list arguments;
    (void)code;
    va_start(arguments, format);
    vsnprintf(stop_reason, sizeof stop_reason, format, arguments);
    va_end(arguments);
    longjmp(stopped, 1);
}
uint8_t *recomp_memory(uint32_t address, size_t size)
{
    address &= 0x7fffffffu;
    if (address >= sizeof ram || size > sizeof ram - address)
        recomp_stop(2, "test:guest-span");
    return ram + address;
}
uint32_t *recomp_memory_u32_checked(uint32_t address)
{
    return (uint32_t *)(void *)recomp_memory(address, 4u);
}
void recomp_guest_load(void *to, uint32_t from, size_t size) { memcpy(to, recomp_memory(from, size), size); }
void recomp_guest_store(uint32_t to, const void *from, size_t size) { memcpy(recomp_memory(to, size), from, size); }
void recomp_guest_memcpy(uint32_t to, uint32_t from, size_t size)
{
    memcpy(recomp_memory(to, size), recomp_memory(from, size), size);
}
uint32_t recomp_kernel_allocate_pool(uint32_t size)
{
    if (fail_allocation) return 0u;
    const uint32_t result = allocated;
    allocated += size;
    return result;
}
void recomp_kernel_free_pool(uint32_t address) { freed = address; }
uint32_t recomp_d3d8_require_device(void) { return DEVICE; }
const RecompD3dRenderStateModel *recomp_d3d8_state_model(void) { return &state; }
bool recomp_d3d8_state_set_simple(uint32_t method, uint32_t value)
{
    return recomp_d3d_set_simple_render_state(&state, method, value);
}
bool recomp_d3d8_resource_describe(uint32_t resource, RecompD3dTextureDesc *out)
{
    if (resource != 0x7000u) return false;
    *out = texture;
    return true;
}
void recomp_d3d8_submit(const RecompD3dPresenterCommand *command)
{
    submitted = *command;
    memcpy(captured_indices, command->data.draw.index_bytes, command->data.draw.index_count * 2u);
    ++submits;
}

static uint32_t invoke(uint32_t address, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    RecompFunction function = recomp_d3d8_draw_lookup_manual(address);
    if (function == NULL) abort();
    recomp_runtime.registers.esp = STACK;
    const uint32_t args[5] = {0x12345678u, a, b, c, d};
    recomp_guest_store(STACK, args, sizeof args);
    recomp_runtime.registers.eax = 0x12345678u;
    function();
    return recomp_runtime.registers.eax;
}

static void initialize(void)
{
    memset(ram, 0, sizeof ram);
    recomp_d3d_render_state_reset(&state);
    submits = 0u;
    *recomp_memory_u32(DEVICE + 0x798u) = 0x144u;
    *recomp_memory_u32(DEVICE + 0x1a04u) = DEVICE + 0x1a24u;
    *recomp_memory_u32(DEVICE + 0x1a08u) = DEVICE + 0x1a6cu;
    *recomp_memory_u32(DEVICE + 0xee8u) = 720u;
    *recomp_memory_u32(DEVICE + 0xeecu) = 480u;
    *recomp_memory_u32(DEVICE + 0xef4u) = 0x3f800000u;
    for (uint32_t slot = 0u; slot < 10u; ++slot) {
        for (uint32_t i = 0u; i < 4u; ++i)
            *recomp_memory_u32(DEVICE + 0xc60u + slot * 64u + i * 20u) = 0x3f800000u;
    }
    *recomp_memory_u32(0x3579a8u) = 28u;
    *recomp_memory_u32(0x3579b0u) = BUFFER;
    *recomp_memory_u32(BUFFER + 4u) = VERTICES;
    for (uint32_t i = 0u; i < 4u; ++i) {
        const uint32_t base = STAGES + i * 128u;
        *recomp_memory_u32(base) = 1u;
        *recomp_memory_u32(base + 4u) = 1u;
        *recomp_memory_u32(base + 12u) = 2u;
        *recomp_memory_u32(base + 16u) = 2u;
        *recomp_memory_u32(base + 48u) = i == 0u ? 4u : 1u;
        *recomp_memory_u32(base + 56u) = 2u;
        *recomp_memory_u32(base + 60u) = 0u;
        *recomp_memory_u32(base + 64u) = 4u;
        *recomp_memory_u32(base + 72u) = 2u;
        *recomp_memory_u32(base + 76u) = 0u;
    }
    texture = (RecompD3dTextureDesc){0};
    target = (RecompD3dPresenterTarget){0};
    texture.format_byte = 6u;
    texture.bits_per_pixel = 32u;
    texture.width = texture.height = 4u;
    texture.data = 0x8000u;
    texture.mip_levels = 1u;
}

static int pixel_tests(void)
{
    if (setjmp(stopped) != 0) { fprintf(stderr, "Unexpected stop: %s\n", stop_reason); return 1; }
    CHECK(recomp_d3d8_draw_lookup_manual(0u) == NULL);
    memset(ram + INPUT, 0, 0xf0u);
    *recomp_memory_u32(INPUT + 0xe4u) = UINT32_MAX;
    *recomp_memory_u32(INPUT + 0xe8u) = UINT32_MAX;
    *recomp_memory_u32(INPUT + 0xecu) = UINT32_MAX;
    *recomp_memory_u32(INPUT + 0xd8u) = 0x1234u;
    fail_allocation = true;
    *recomp_memory_u32(OUTPUT) = 0xdeadbeefu;
    CHECK(invoke(0x349490u, INPUT, OUTPUT, 0u, 0u) == 0x8007000eu);
    CHECK(*recomp_memory_u32(OUTPUT) == 0xdeadbeefu);
    fail_allocation = false;
    CHECK(invoke(0x349490u, INPUT, OUTPUT, 0u, 0u) == 0u);
    CHECK(recomp_runtime.registers.esp == STACK + 12u);
    const uint32_t handle = *recomp_memory_u32(OUTPUT);
    CHECK(*recomp_memory_u32(handle) == 1u && *recomp_memory_u32(handle + 4u) == 1u);
    CHECK(*recomp_memory_u32(handle + 8u) == handle + 12u);
    CHECK(memcmp(ram + handle + 12u, ram + INPUT, 0xf0u) == 0);
    invoke(0x349520u, handle, 0u, 0u, 0u);
    CHECK(recomp_runtime.registers.esp == STACK + 8u);
    CHECK(*recomp_memory_u32(DEVICE + 0x784u) == handle);
    CHECK(*recomp_memory_u32(DEVICE + 0x790u) == 0x1234u);
    CHECK(*recomp_memory_u32(0x355480u) == 0x1234u);
    CHECK(memcmp(ram + 0x355260u, ram + INPUT, 0xe4u) == 0);
    invoke(0x349500u, OUTPUT, 0u, 0u, 0u);
    CHECK(*recomp_memory_u32(OUTPUT) == handle && recomp_runtime.registers.esp == STACK + 8u);
    *recomp_memory_u32(handle + 12u + 0xe4u) = 0xfffffff2u;
    *recomp_memory_u32(handle + 12u + 0xe8u) = 0xffffff2fu;
    *recomp_memory_u32(handle + 12u + 0xecu) = 0x12fu;
    const float color[4] = {-1.0f, 0.5f, 2.0f, 1.0f};
    recomp_guest_store(INPUT, color, sizeof color);
    invoke(0x349720u, 2u, INPUT, 1u, 0u);
    CHECK(recomp_runtime.registers.esp == STACK + 16u);
    CHECK(*recomp_memory_u32(DEVICE + 0x8ecu) == 0xff0080ffu);
    CHECK(*recomp_memory_u32(0x355288u) == 0xff0080ffu);
    CHECK(*recomp_memory_u32(0x3552acu) == 0xff0080ffu);
    CHECK(*recomp_memory_u32(0x355310u) == 0xff0080ffu);
    uint32_t value;
    CHECK(recomp_d3d_get_simple_render_state(&state, 0x40a60u, &value) && value == 0xff0080ffu);
    if (setjmp(stopped) == 0) { invoke(0x349720u, 7u, INPUT, 2u, 0u); return 1; }
    CHECK(strcmp(stop_reason, "d3d8:pixel-constant-range") == 0);
    invoke(0x349520u, 0u, 0u, 0u, 0u);
    CHECK(*recomp_memory_u32(DEVICE + 0x784u) == 0u);
    invoke(0x3494e0u, handle, 0u, 0u, 0u);
    CHECK(freed == handle && recomp_runtime.registers.esp == STACK + 8u);
    return 0;
}

static int draw_tests(void)
{
    if (setjmp(stopped) != 0) { fprintf(stderr, "Unexpected stop: %s\n", stop_reason); return 1; }
    initialize();
    CHECK(invoke(0x348240u, 5u, 3u, VERTICES, 28u) == 0x12345678u);
    CHECK(recomp_runtime.registers.esp == STACK + 20u && submits == 1u);
    CHECK(submitted.type == RECOMP_D3D_PRESENTER_COMMAND_DRAW);
    CHECK(submitted.data.draw.fvf == 0x144u && submitted.data.draw.vertex_count == 3u);
    CHECK(submitted.data.draw.vertex_bytes == ram + VERTICES);
    CHECK(captured_indices[0] == 0u && captured_indices[2] == 2u);
    CHECK(!submitted.data.draw.has_transform && submitted.data.draw.viewport[2] == 720.0f);
    target.offscreen = true;
    target.no_depth = true;
    target.color = texture;
    invoke(0x348240u, 5u, 3u, VERTICES, 28u);
    CHECK(submitted.data.draw.target.offscreen && submitted.data.draw.target.no_depth &&
        submitted.data.draw.target.color.data == texture.data);
    target = (RecompD3dPresenterTarget){0};
    invoke(0x3483b0u, 6u, 2u, 4u, 0u);
    CHECK(recomp_runtime.registers.esp == STACK + 16u && submits == 3u);
    CHECK(submitted.data.draw.vertex_bytes == ram + VERTICES + 56u);
    CHECK(submitted.data.draw.triangle_count == 2u);
    const uint16_t indices[3] = {4u, 0u, 2u};
    recomp_guest_store(INDICES, indices, sizeof indices);
    *recomp_memory_u32(DEVICE + 0x1cu) = 2u;
    invoke(0x348450u, 5u, 3u, INDICES, 0u);
    CHECK(recomp_runtime.registers.esp == STACK + 16u && submits == 4u);
    CHECK(submitted.data.draw.vertex_count == 5u && captured_indices[0] == 4u);
    CHECK(submitted.data.draw.vertex_bytes == ram + VERTICES + 56u);
    *recomp_memory_u32(DEVICE + 0x798u) = 2u;
    const float translate = 3.5f;
    recomp_guest_store(DEVICE + 0xde0u + 48u, &translate, sizeof translate);
    invoke(0x348240u, 5u, 3u, VERTICES, 12u);
    CHECK(submitted.data.draw.has_transform && submitted.data.draw.transform[12] == 3.5f);
    *recomp_memory_u32(DEVICE + 0xf88u) = 0x7000u;
    *recomp_memory_u32(DEVICE + 0x798u) = 0x144u;
    invoke(0x348240u, 5u, 3u, VERTICES, 28u);
    CHECK(submitted.data.draw.has_texture && submitted.data.draw.texture_bytes == ram + 0x8000u);
    CHECK(submitted.data.draw.texture_byte_count == 64u);
    CHECK(submitted.data.draw.address_u == 1u && submitted.data.draw.address_v == 1u);
    texture.mip_levels = 3u;
    *recomp_memory_u32(STAGES + 20u) = 1u;
    *recomp_memory_u32(STAGES + 24u) = 0xc0000000u;
    invoke(0x348240u, 5u, 3u, VERTICES, 28u);
    CHECK(submitted.data.draw.point_mip_filter && submitted.data.draw.mip_lod_bias == -2.0f);
    *recomp_memory_u32(STAGES + 64u) = 2u;
    invoke(0x348240u, 5u, 3u, VERTICES, 28u);
    CHECK(submitted.data.draw.material_alpha_mode == RECOMP_D3D_MATERIAL_ALPHA_SELECT_TEXTURE);
    *recomp_memory_u32(STAGES + 72u) = 0u;
    invoke(0x348240u, 5u, 3u, VERTICES, 28u);
    CHECK(submitted.data.draw.material_alpha_mode == RECOMP_D3D_MATERIAL_ALPHA_SELECT_VERTEX_DIFFUSE);
    *recomp_memory_u32(STAGES + 72u) = 2u;
    *recomp_memory_u32(STAGES + 64u) = 4u;
    *recomp_memory_u32(STAGES + 24u) = 0x7f800000u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 28u); return 1; }
    CHECK(strcmp(stop_reason, "d3d8:draw-texture-lod-bias") == 0);
    texture.mip_levels = 1u;
    *recomp_memory_u32(STAGES + 24u) = 0u;
    *recomp_memory_u32(DEVICE + 0x798u) = 0x104u;
    *recomp_memory_u32(STAGES + 64u) = 3u;
    invoke(0x348240u, 6u, 4u, VERTICES, 24u);
    CHECK(submitted.data.draw.material_alpha_mode == RECOMP_D3D_MATERIAL_ALPHA_SELECT_DIFFUSE &&
        submitted.data.draw.material_alpha == 1.0f);
    *recomp_memory_u32(STAGES + 64u) = 4u;
    *recomp_memory_u32(DEVICE + 0x798u) = 0x144u;
    const uint32_t before = submits;
    invoke(0x348240u, 5u, 0u, VERTICES, 28u);
    CHECK(submits == before);
    if (setjmp(stopped) == 0) { invoke(0x348240u, 4u, 3u, VERTICES, 28u); return 1; }
    CHECK(strncmp(stop_reason, "d3d8:draw-primitive", 19u) == 0);
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 65537u, VERTICES, 28u); return 1; }
    CHECK(strcmp(stop_reason, "d3d8:draw-span") == 0);
    *recomp_memory_u32(DEVICE + 0x798u) = 3u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 28u); return 1; }
    CHECK(strncmp(stop_reason, "d3d8:draw-vertex-layout", 23u) == 0);
    *recomp_memory_u32(DEVICE + 0x798u) = 0x144u;
    *recomp_memory_u32(DEVICE + 0x784u) = 1u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 28u); return 1; }
    CHECK(strcmp(stop_reason, "d3d8:draw-pixel-shader-unsupported") == 0);
    return 0;
}

static int filter_tests(void)
{
    if (setjmp(stopped) != 0) { fprintf(stderr, "Unexpected stop: %s\n", stop_reason); return 1; }
    initialize();
    static const uint32_t shader[][2] = {
        {0x08u, 0xdc30dd30u}, {0x20u, 0xcu}, {0x24u, 0x1c80u},
        {0x68u, 0x10c00u}, {0x6cu, 0x10d00u}, {0x70u, 0x30c00u},
        {0x90u, 0xcc20cd20u}, {0xb4u, 0x10c00u}, {0xb8u, 0x10d00u},
        {0xbcu, 0x30c00u}, {0xd4u, 0x11103u}, {0xd8u, 0x8421u},
    };
    static const uint32_t overrides[][2] = {
        {0x40260u, 0xd1d8d2d9u}, {0x40264u, 0xd1dad2dbu},
        {0x40a60u, 0x40404040u}, {0x40a64u, 0x40404040u},
        {0x40a80u, 0x40404040u}, {0x40a84u, 0x40404040u},
        {0x40ac0u, 0xc1c8c2c9u}, {0x40ac4u, 0xc1cac2cbu},
    };
    for (uint32_t i = 0u; i < sizeof shader / sizeof shader[0]; ++i)
        *recomp_memory_u32(INPUT + shader[i][0]) = shader[i][1];
    invoke(0x349490u, INPUT, OUTPUT, 0u, 0u);
    invoke(0x349520u, *recomp_memory_u32(OUTPUT), 0u, 0u, 0u);
    for (uint32_t i = 0u; i < sizeof overrides / sizeof overrides[0]; ++i)
        CHECK(recomp_d3d8_state_set_simple(overrides[i][0], overrides[i][1]));
    for (uint32_t stage = 0u; stage < 4u; ++stage) {
        *recomp_memory_u32(DEVICE + 0xf88u + stage * 4u) = 0x7000u;
        *recomp_memory_u32(STAGES + stage * 128u) = 3u;
        *recomp_memory_u32(STAGES + stage * 128u + 4u) = 3u;
        *recomp_memory_u32(STAGES + stage * 128u + 20u) = 1u;
        *recomp_memory_u32(STAGES + stage * 128u + 112u) = stage;
    }
    *recomp_memory_u32(DEVICE + 0x798u) = 0x404u;
    invoke(0x348240u, 5u, 3u, VERTICES, 48u);
    CHECK(submits == 1u && submitted.data.draw.four_tap_filter);
    *recomp_memory_u32(0x355268u) ^= 1u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 48u); return 1; }
    CHECK(submits == 1u && strcmp(stop_reason, "d3d8:draw-pixel-shader-unsupported") == 0);
    return 0;
}

int main(void)
{
    initialize();
    if (pixel_tests() || draw_tests() || filter_tests()) return 1;
    puts("PASS D3D8 pixel lifecycle/constants and semantic draw ABI/snapshots");
    return 0;
}
