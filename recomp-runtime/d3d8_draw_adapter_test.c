/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "d3d8_draw_adapter.h"
#include "d3d8_adapter.h"
#include "d3d8_resource_adapter.h"
#include "d3d8_state_adapter.h"
#include "d3d_light_model.h"

#include <math.h>
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
static float captured_vertices[64];
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
    const size_t vertex_bytes = (size_t)command->data.draw.vertex_stride * command->data.draw.vertex_count;
    memcpy(captured_vertices, command->data.draw.vertex_bytes,
        vertex_bytes < sizeof captured_vertices ? vertex_bytes : sizeof captured_vertices);
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
    *recomp_memory_u32(DEVICE + 0xef8u) = *recomp_memory_u32(DEVICE + 0xefcu) = 0x3f080000u;
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
    CHECK(submitted.data.draw.screen_space_offset[0] == 0.0f && submitted.data.draw.screen_space_offset[1] == 0.0f);
    CHECK(submitted.data.draw.has_scissor && submitted.data.draw.scissor[2] == 720 &&
        submitted.data.draw.scissor[3] == 480);
    const int32_t scissor[4] = {8, 12, 80, 120};
    recomp_guest_store(DEVICE + 0x1b80u, scissor, sizeof scissor);
    *recomp_memory_u32(DEVICE + 0x1c00u) = 1u;
    *recomp_memory_u32(DEVICE + 0xef8u) = 0x3d000000u;
    *recomp_memory_u32(DEVICE + 0xefcu) = 0x3f480000u;
    target.offscreen = true;
    target.no_depth = true;
    target.color = texture;
    invoke(0x348240u, 5u, 3u, VERTICES, 28u);
    CHECK(submitted.data.draw.target.offscreen && submitted.data.draw.target.no_depth &&
        submitted.data.draw.target.color.data == texture.data);
    CHECK(submitted.data.draw.screen_space_offset[0] == -0.5f && submitted.data.draw.screen_space_offset[1] == 0.25f);
    CHECK(submitted.data.draw.has_scissor &&
        memcmp(submitted.data.draw.scissor, scissor, sizeof scissor) == 0);
    *recomp_memory_u32(DEVICE + 0x1c00u) = 0u;
    *recomp_memory_u32(DEVICE + 0xee0u) = 20u;
    *recomp_memory_u32(DEVICE + 0xee8u) = 700u;
    *recomp_memory_u32(DEVICE + 0xef8u) = *recomp_memory_u32(DEVICE + 0xefcu) = 0x3f080000u;
    target = (RecompD3dPresenterTarget){0};
    invoke(0x3483b0u, 6u, 2u, 4u, 0u);
    CHECK(recomp_runtime.registers.esp == STACK + 16u && submits == 3u);
    CHECK(submitted.data.draw.has_scissor && submitted.data.draw.scissor[0] == 20 &&
        submitted.data.draw.scissor[2] == 720);
    *recomp_memory_u32(DEVICE + 0xee0u) = 0u;
    *recomp_memory_u32(DEVICE + 0xee8u) = 720u;
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
    /* -2.0 * 256 + 0.5 truncates toward zero to -511. */
    CHECK(submitted.data.draw.point_mip_filter && submitted.data.draw.mip_lod_bias == -511.0f / 256.0f);
    CHECK(!submitted.data.draw.alpha_kill);
    *recomp_memory_u32(STAGES + 44u) = 4u;
    invoke(0x348240u, 5u, 3u, VERTICES, 28u);
    CHECK(submitted.data.draw.alpha_kill);
    *recomp_memory_u32(STAGES + 44u) = 1u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 28u); return 1; }
    CHECK(strncmp(stop_reason, "d3d8:draw-texture-sampler:", 26) == 0);
    *recomp_memory_u32(STAGES + 44u) = 0u;
    *recomp_memory_u32(STAGES + 64u) = 2u;
    invoke(0x348240u, 5u, 3u, VERTICES, 28u);
    CHECK(submitted.data.draw.material_alpha_mode == RECOMP_D3D_MATERIAL_ALPHA_SELECT_TEXTURE);
    *recomp_memory_u32(STAGES + 72u) = 0u;
    invoke(0x348240u, 5u, 3u, VERTICES, 28u);
    CHECK(submitted.data.draw.material_alpha_mode == RECOMP_D3D_MATERIAL_ALPHA_SELECT_VERTEX_DIFFUSE);
    *recomp_memory_u32(STAGES + 72u) = 2u;
    *recomp_memory_u32(STAGES + 64u) = 4u;
    *recomp_memory_u32(STAGES + 24u) = 0x7f800000u;
    /* 0x34f400 clamps the 13-bit fixed-point bias; infinity converts to INT_MIN. */
    invoke(0x348240u, 5u, 3u, VERTICES, 28u);
    CHECK(submitted.data.draw.mip_lod_bias == -16.0f);
    *recomp_memory_u32(STAGES + 24u) = 0x42c80000u; /* 100.0 */
    invoke(0x348240u, 5u, 3u, VERTICES, 28u);
    CHECK(submitted.data.draw.mip_lod_bias == 15.99f);
    *recomp_memory_u32(STAGES + 24u) = 0xbb800000u; /* -1/256 truncates to 0 after +0.5 */
    invoke(0x348240u, 5u, 3u, VERTICES, 28u);
    CHECK(submitted.data.draw.mip_lod_bias == 0.0f);
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
    const uint32_t quads = submits;
    invoke(0x348240u, 9u, 5u, VERTICES, 28u);
    CHECK(submits == quads + 1u && submitted.data.draw.primitive_type == 6u &&
        submitted.data.draw.index_count == 4u && submitted.data.draw.triangle_count == 2u);
    invoke(0x348240u, 2u, 5u, VERTICES, 28u);
    CHECK(submits == quads + 2u && submitted.data.draw.primitive_type == 2u &&
        submitted.data.draw.index_count == 4u && submitted.data.draw.triangle_count == 2u);
    if (setjmp(stopped) == 0) { invoke(0x348240u, 2u, 2u, VERTICES, 20u); return 1; }
    CHECK(strncmp(stop_reason, "d3d8:draw-vertex-layout:00000144:20", 35u) == 0);
    *recomp_memory_u32(DEVICE + 0xf88u) = 0u;
    invoke(0x348240u, 2u, 2u, VERTICES, 20u);
    CHECK(submits == quads + 3u && submitted.data.draw.fvf == 0x44u && !submitted.data.draw.has_texture);
    /* Width 2 widens an x-major line by one pixel above and below. */
    const float line[10] = {10.0f, 20.0f, 0.0f, 1.0f, 0.0f, 50.0f, 24.0f, 0.0f, 1.0f, 0.0f};
    const float wide = 2.0f;
    recomp_guest_store(0x4800u, line, sizeof line);
    recomp_guest_store(0x3554d4u, &wide, sizeof wide);
    invoke(0x348240u, 2u, 2u, 0x4800u, 20u);
    CHECK(submitted.data.draw.primitive_type == 5u && submitted.data.draw.index_count == 6u &&
        submitted.data.draw.vertex_count == 4u && submitted.data.draw.triangle_count == 2u &&
        submitted.data.draw.cull_mode == RECOMP_D3D_CULL_NONE);
    CHECK(captured_indices[2] == 3u && captured_indices[5] == 2u);
    CHECK(captured_vertices[1] == 19.0f && captured_vertices[6] == 21.0f &&
        captured_vertices[10] == 50.0f && captured_vertices[11] == 23.0f && captured_vertices[16] == 25.0f);
    *recomp_memory_u32(DEVICE + 0x798u) = 0x142u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 2u, 2u, VERTICES, 24u); return 1; }
    CHECK(strcmp(stop_reason, "d3d8:draw-wide-lines:1") == 0);
    *recomp_memory_u32(DEVICE + 0x798u) = 0x144u;
    recomp_guest_store(0x3554d4u, &(float){1.0f}, sizeof(float));
    *recomp_memory_u32(DEVICE + 0xf88u) = 0x7000u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 65537u, VERTICES, 28u); return 1; }
    CHECK(strcmp(stop_reason, "d3d8:draw-span") == 0);
    *recomp_memory_u32(DEVICE + 0x798u) = 0x1002u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 28u); return 1; }
    CHECK(strncmp(stop_reason, "d3d8:draw-vertex-layout", 23u) == 0);
    *recomp_memory_u32(DEVICE + 0x798u) = 0x144u;
    *recomp_memory_u32(DEVICE + 0x784u) = 1u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 28u); return 1; }
    CHECK(strcmp(stop_reason, "d3d8:draw-pixel-shader-unsupported") == 0);
    return 0;
}

static int point_tests(void)
{
    if (setjmp(stopped) != 0) { fprintf(stderr, "Unexpected point stop: %s\n", stop_reason); return 1; }
    initialize();
    /* 0x2345c0's particles: XYZ|DIFFUSE points with the sprite texture on stage 3. */
    *recomp_memory_u32(DEVICE + 0x798u) = 0x42u;
    *recomp_memory_u32(DEVICE + 0xf94u) = 0x7000u;
    *recomp_memory_u32(STAGES + 3u * 128u + 48u) = 2u;
    const float projection[16] = {2,0,0,0, 0,2,0,0, 0,0,1,1, 0,0,-1,0};
    recomp_guest_store(DEVICE + 0xca0u, projection, sizeof projection);
    const float size[2] = {0.1f, 1.0f}, scale[4] = {1.0f, 0.0f, 0.0f, 32.0f};
    recomp_guest_store(0x355430u, size, sizeof size);
    recomp_guest_store(0x355440u, scale, sizeof scale);
    *recomp_memory_u32(0x355438u) = *recomp_memory_u32(0x35543cu) = 1u;
    const struct { float x, y, z; uint32_t diffuse; } vertex = {0.0f, 0.0f, 10.0f, 0x80123456u};
    recomp_guest_store(VERTICES, &vertex, sizeof vertex);
    invoke(0x348240u, 1u, 1u, VERTICES, 16u);
    const RecompD3dPresenterDrawCommand *draw = &submitted.data.draw;
    CHECK(draw->primitive_type == 5u && draw->index_count == 6u && draw->vertex_count == 4u &&
        draw->fvf == 0x142u && draw->vertex_stride == 24u && draw->has_texture &&
        draw->cull_mode == RECOMP_D3D_CULL_NONE && draw->transform[11] == 1.0f);
    CHECK(captured_indices[0] == 0u && captured_indices[4] == 1u && captured_indices[5] == 3u);
    /* 480 * 0.1 clamps to MAX 32 pixels: +-32*10/(2*720) eye units across, +-32*10/(2*480) up. */
    uint32_t diffuse;
    memcpy(&diffuse, &captured_vertices[3], sizeof diffuse);
    CHECK(fabsf(captured_vertices[0] + 2.0f / 9.0f) < 1e-6f && fabsf(captured_vertices[1] - 1.0f / 3.0f) < 1e-6f &&
        captured_vertices[2] == 10.0f && captured_vertices[4] == 0.0f && captured_vertices[5] == 0.0f);
    CHECK(fabsf(captured_vertices[18] - 2.0f / 9.0f) < 1e-6f && fabsf(captured_vertices[19] + 1.0f / 3.0f) < 1e-6f &&
        captured_vertices[22] == 1.0f && captured_vertices[23] == 1.0f);
    CHECK(diffuse == 0x80ffffffu);
    *recomp_memory_u32(STAGES + 3u * 128u + 56u) = 0u;
    invoke(0x348240u, 1u, 1u, VERTICES, 16u);
    memcpy(&diffuse, &captured_vertices[3], sizeof diffuse);
    CHECK(draw->texture_alpha_only && diffuse == 0x80123456u);
    *recomp_memory_u32(STAGES + 3u * 128u + 56u) = 2u;
    *recomp_memory_u32(0x35543cu) = 0u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 1u, 1u, VERTICES, 16u); return 1; }
    CHECK(strcmp(stop_reason, "d3d8:draw-points-unbound") == 0);
    *recomp_memory_u32(0x35543cu) = 1u;
    *recomp_memory_u32(STAGES + 3u * 128u + 48u) = 7u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 1u, 1u, VERTICES, 16u); return 1; }
    CHECK(strncmp(stop_reason, "d3d8:point-sprite-stage:", 24u) == 0);
    return 0;
}

static int lighting_tests(void)
{
    if (setjmp(stopped) != 0) { fprintf(stderr, "Unexpected lighting stop: %s\n", stop_reason); return 1; }
    initialize();
    *recomp_memory_u32(DEVICE + 0x798u) = 0x112u;
    *recomp_memory_u32(DEVICE + 0xf88u) = 0x7000u;
    *recomp_memory_u32(0x355368u) = 0xdeadbeefu; /* Inline Simple calls do not maintain this shadow. */
    *recomp_memory_u32(0x3553f8u) = 1u;
    *recomp_memory_u32(0x3553fcu) = 1u;
    *recomp_memory_u32(0x355404u) = 1u;
    *recomp_memory_u32(0x35541cu) = 1u; /* No FVF color: source falls back to material. */
    *recomp_memory_u32(0x35542cu) = 0xff804020u;
    *recomp_memory_u32(0x355498u) = 1u;
    const float material[17] = {0.5f, 0.75f, 1.0f, 0.25f, 0.2f, 0.4f, 0.6f, 1.0f,
        0.1f, 0.2f, 0.3f, 1.0f, 0.05f, 0.1f, 0.15f, 0.0f, 16.0f};
    recomp_guest_store(DEVICE + 0xf00u, material, sizeof material);
    const uint32_t lights = 0x9000u;
    *recomp_memory_u32(DEVICE + 0x7a4u) = lights;
    *recomp_memory_u32(DEVICE + 0x7a8u) = 4u;
    *recomp_memory_u32(DEVICE + 0x7acu) = lights;
    *recomp_memory_u32(DEVICE + 0x7acu + 12u) = lights + 3u * sizeof(RecompD3dLightRecord);
    RecompD3dLightRecord directional = {0}, point = {0};
    directional.desc.type = 3u;
    directional.desc.diffuse[0] = 1.0f;
    directional.desc.specular[1] = 0.5f;
    directional.desc.ambient[2] = 0.25f;
    directional.normalized_direction[2] = 1.0f;
    directional.slot = 0u;
    point.desc.type = 1u;
    point.desc.diffuse[1] = 0.5f;
    point.desc.position[0] = 2.0f;
    point.desc.position[1] = 3.0f;
    point.desc.position[2] = 4.0f;
    point.desc.range = 20.0f;
    point.desc.attenuation[0] = 1.0f;
    point.desc.attenuation[1] = 0.5f;
    point.slot = 3u;
    recomp_guest_store(lights, &directional, sizeof directional);
    recomp_guest_store(lights + 3u * sizeof point, &point, sizeof point);
    const float world[16] = {2,0,0,0, 0,3,0,0, 0,0,4,0, 5,6,7,1};
    recomp_guest_store(DEVICE + 0xde0u, world, sizeof world);
    invoke(0x348240u, 5u, 3u, VERTICES, 32u);
    const RecompD3dDirectionalLighting *lit = &submitted.data.draw.directional;
    CHECK(lit->enabled && lit->normalize && lit->stage_mode && lit->count == 2u);
    CHECK(lit->directions[0][2] == 1.0f && lit->positions[0][3] == 0.0f);
    CHECK(lit->positions[1][0] == 2.0f && lit->positions[1][3] == 1.0f);
    CHECK(lit->attenuation[1][1] == 0.5f && lit->attenuation[1][3] == 20.0f);
    CHECK(lit->material_diffuse[3] == 0.25f && lit->specular[0][1] == 0.1f);
    CHECK(lit->ambient[0][2] == 0.15f && lit->ambient_emissive[0] > 0.15f &&
        lit->ambient_emissive[0] < 0.151f);
    CHECK(lit->normal_transforms[0][0] == 0.5f && lit->normal_transforms[0][10] == 0.25f);
    CHECK(lit->world_transforms[0][12] == 5.0f && lit->view[2] == -1.0f && lit->view[3] == 16.0f);
    CHECK(lit->stage[0] == 1.0f && lit->stage[1] == 1.0f && lit->stage[2] == 1.0f && lit->stage[3] == 1.0f);
    /* Stage-0 CURRENT reads diffuse (MODULATE(TEXTURE, CURRENT) in a fight). */
    *recomp_memory_u32(STAGES + 15u * 4u) = 1u;
    invoke(0x348240u, 5u, 3u, VERTICES, 32u);
    CHECK(lit->enabled && lit->stage[0] == 1.0f && lit->stage[1] == 1.0f);
    *recomp_memory_u32(STAGES + 15u * 4u) = 0u;
    CHECK(recomp_d3d8_state_set_simple(0x4037cu, 0x1d00u));
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 32u); return 1; }
    CHECK(strcmp(stop_reason, "d3d8:lighting-shade-mode") == 0);
    CHECK(recomp_d3d8_state_set_simple(0x4037cu, 0x1d01u));
    const float flat[16] = {0,0,0,0, 0,-2,0,0, 0,0,0,0, 5,6,7,1};
    recomp_guest_store(DEVICE + 0xde0u, flat, sizeof flat);
    invoke(0x348240u, 5u, 3u, VERTICES, 32u);
    CHECK(lit->enabled && lit->world_transforms[0][5] == -2.0f && lit->normal_transforms[0][5] == 0.0f);
    recomp_guest_store(DEVICE + 0xde0u, world, sizeof world);
    uint32_t saved_stages[4][32];
    recomp_guest_load(saved_stages, STAGES, sizeof saved_stages);
    const uint32_t reflection[3][6] = {{2,2,1,4,2,0}, {14,2,1,14,2,1}, {4,0,1,4,0,1}};
    const uint32_t offsets[6] = {12,14,15,16,18,19};
    for (uint32_t stage = 0u; stage < 3u; ++stage)
        for (uint32_t i = 0u; i < 6u; ++i)
            *recomp_memory_u32(STAGES + stage * 128u + offsets[i] * 4u) = reflection[stage][i];
    *recomp_memory_u32(STAGES + 128u + 21u * 4u) = 2u;
    *recomp_memory_u32(STAGES + 128u + 28u * 4u) = 0x30000u;
    *recomp_memory_u32(DEVICE + 0xf8cu) = 0x7000u;
    invoke(0x348240u, 5u, 3u, VERTICES, 32u);
    CHECK(submitted.data.draw.has_reflection && !submitted.data.draw.reflection_mesh_uv &&
        !submitted.data.draw.reflection_premultiplied);
    CHECK(submitted.data.draw.reflection_bytes == ram + texture.data &&
        submitted.data.draw.reflection_byte_count == 64u);
    CHECK(submitted.data.draw.reflection_world_view[12] == 5.0f &&
        submitted.data.draw.reflection_normal[0] == 0.5f &&
        submitted.data.draw.reflection_transform[15] == 1.0f &&
        submitted.data.draw.reflection_diffuse[3] == 0.25f);
    /* The fight's environment stage clamps (U/V 3); its sampler follows stage 1. */
    *recomp_memory_u32(STAGES + 128u) = *recomp_memory_u32(STAGES + 132u) = 3u;
    invoke(0x348240u, 5u, 3u, VERTICES, 32u);
    CHECK(submitted.data.draw.has_reflection && submitted.data.draw.reflection_address_u == 3u &&
        submitted.data.draw.reflection_address_v == 3u);
    *recomp_memory_u32(STAGES + 128u) = 2u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 32u); return 1; }
    CHECK(strncmp(stop_reason, "d3d8:draw-texture-stages:", 25) == 0);
    *recomp_memory_u32(STAGES + 128u) = 3u;
    *recomp_memory_u32(STAGES + 128u + 48u) = *recomp_memory_u32(STAGES + 128u + 64u) = 16u;
    if (setjmp(stopped) != 0) { fprintf(stderr, "Unexpected premultiplied stop: %s\n", stop_reason); return 1; }
    invoke(0x348240u, 5u, 3u, VERTICES, 32u);
    CHECK(submitted.data.draw.has_reflection && submitted.data.draw.reflection_premultiplied);
    *recomp_memory_u32(STAGES + 128u + 64u) = 14u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 32u); return 1; }
    CHECK(strncmp(stop_reason, "d3d8:draw-texture-stages:", 25) == 0);
    *recomp_memory_u32(STAGES + 128u + 48u) = *recomp_memory_u32(STAGES + 128u + 64u) = 14u;
    *recomp_memory_u32(STAGES + 128u + 6u * 4u) = 0x3f800000u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 32u); return 1; }
    CHECK(strcmp(stop_reason, "d3d8:reflection-sampler") == 0);
    *recomp_memory_u32(STAGES + 128u + 6u * 4u) = 0u;
    *recomp_memory_u32(STAGES + 128u + 21u * 4u) = 0u;
    *recomp_memory_u32(STAGES + 128u + 28u * 4u) = 0u;
    *recomp_memory_u32(DEVICE + 0x798u) = 0x142u;
    *recomp_memory_u32(0x3553f8u) = 0u;
    invoke(0x348240u, 5u, 3u, VERTICES, 24u);
    CHECK(submitted.data.draw.has_reflection && submitted.data.draw.reflection_mesh_uv && !lit->enabled);
    *recomp_memory_u32(DEVICE + 0x798u) = 0x112u;
    *recomp_memory_u32(0x3553f8u) = 1u;
    /* Fight chrome: stage 0 generates the environment coordinates (TEXTURE0
       matrix) and stage 1 blends the mesh-UV base by its own alpha. */
    const float chrome_matrix[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 7,0,0,1};
    recomp_guest_store(DEVICE + 0xce0u, chrome_matrix, sizeof chrome_matrix);
    *recomp_memory_u32(STAGES + 21u * 4u) = 2u;
    *recomp_memory_u32(STAGES + 28u * 4u) = 0x30000u;
    invoke(0x348240u, 5u, 3u, VERTICES, 32u);
    CHECK(submitted.data.draw.has_reflection && submitted.data.draw.reflection_mesh_uv &&
        submitted.data.draw.texgen_reflection && submitted.data.draw.reflection_transform[12] == 7.0f);
    /* Generating on both stages would need two texture matrices. */
    *recomp_memory_u32(STAGES + 128u + 21u * 4u) = 2u;
    *recomp_memory_u32(STAGES + 128u + 28u * 4u) = 0x30000u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 32u); return 1; }
    CHECK(strncmp(stop_reason, "d3d8:draw-texture-stages:", 25) == 0);
    recomp_guest_store(STAGES, saved_stages, sizeof saved_stages);
    /* Stage-0 camera-space reflection texgen (COUNT2) reads TEXTURE0. */
    const float texture0[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 9,0,0,1};
    recomp_guest_store(DEVICE + 0xce0u, texture0, sizeof texture0);
    *recomp_memory_u32(STAGES + 21u * 4u) = 2u;
    *recomp_memory_u32(STAGES + 28u * 4u) = 0x30000u;
    *recomp_memory_u32(DEVICE + 0x798u) = 0x142u;
    *recomp_memory_u32(0x3553f8u) = 0u;
    invoke(0x348240u, 5u, 3u, VERTICES, 24u);
    CHECK(submitted.data.draw.texgen_reflection && !submitted.data.draw.has_reflection &&
        submitted.data.draw.reflection_transform[12] == 9.0f &&
        submitted.data.draw.reflection_world_view[12] == 5.0f);
    *recomp_memory_u32(STAGES + 21u * 4u) = 3u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 24u); return 1; }
    CHECK(strcmp(stop_reason, "d3d8:draw-texture-coordinates") == 0);
    *recomp_memory_u32(STAGES + 21u * 4u) = 0u;
    *recomp_memory_u32(STAGES + 28u * 4u) = 0u;
    /* COUNT2 over mesh UV0 animates coordinates through TEXTURE0 (0x34f7d0). */
    *recomp_memory_u32(STAGES + 21u * 4u) = 2u;
    invoke(0x348240u, 5u, 3u, VERTICES, 24u);
    CHECK(submitted.data.draw.texture_transform_uv && !submitted.data.draw.texgen_reflection &&
        submitted.data.draw.reflection_transform[12] == 9.0f);
    *recomp_memory_u32(STAGES + 21u * 4u) = 0x102u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 24u); return 1; }
    CHECK(strcmp(stop_reason, "d3d8:draw-texture-coordinates") == 0);
    *recomp_memory_u32(STAGES + 21u * 4u) = 0u;
    *recomp_memory_u32(DEVICE + 0x798u) = 0x112u;
    *recomp_memory_u32(0x3553f8u) = 1u;
    *recomp_memory_u32(DEVICE + 0xf8cu) = 0u;
    *recomp_memory_u32(DEVICE + 0xf88u) = 0u;
    *recomp_memory_u32(STAGES + 48u) = *recomp_memory_u32(STAGES + 64u) = 1u;
    invoke(0x348240u, 5u, 3u, VERTICES, 32u);
    CHECK(lit->enabled && lit->stage_mode && !submitted.data.draw.has_texture && lit->count == 2u);
    recomp_guest_store(STAGES, saved_stages, sizeof saved_stages);
    *recomp_memory_u32(DEVICE + 0xf88u) = 0x7000u;
    *recomp_memory_u32(DEVICE + 0x798u) = 0x116u;
    *recomp_memory_u32(0x355484u) = 1u;
    invoke(0x348240u, 5u, 3u, VERTICES, 36u);
    CHECK(submitted.data.draw.blend_weight_count == 1u &&
        lit->normal_transforms[1][0] == 1.0f && lit->world_transforms[1][15] == 1.0f);
    *recomp_memory_u32(0x3553fcu) = 0u;
    invoke(0x348240u, 5u, 3u, VERTICES, 36u);
    CHECK(lit->view[3] == 0.0f);
    /* Disabled specular ignores a NaN specular color; enabled, it stops. */
    const uint32_t specular_green = *recomp_memory_u32(DEVICE + 0xf24u);
    *recomp_memory_u32(DEVICE + 0xf24u) = 0x7fc00000u;
    invoke(0x348240u, 5u, 3u, VERTICES, 36u);
    CHECK(lit->enabled && lit->specular[0][1] == 0.0f);
    *recomp_memory_u32(0x3553fcu) = 1u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 36u); return 1; }
    CHECK(strcmp(stop_reason, "d3d8:lighting-material:9:7fc00000") == 0);
    *recomp_memory_u32(DEVICE + 0xf24u) = specular_green;
    *recomp_memory_u32(0x3553fcu) = 1u;
    *recomp_memory_u32(0x355400u) = 1u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 36u); return 1; }
    CHECK(strcmp(stop_reason, "d3d8:lighting-local-viewer") == 0);
    *recomp_memory_u32(0x355400u) = 0u;
    *recomp_memory_u32(DEVICE + 0x7acu) = lights + 1u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 36u); return 1; }
    CHECK(strcmp(stop_reason, "d3d8:lighting-slot") == 0);
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
    CHECK(submitted.data.draw.four_tap_factors[0] == 0x40404040u &&
        submitted.data.draw.four_tap_factors[3] == 0x40404040u &&
        submitted.data.draw.four_tap_scale == 2.0f && submitted.data.draw.four_tap_sum_scale == 0.5f);
    /* Identity-scale variant with unsigned stage-2 inputs, disabled mips and live factors. */
    static const uint32_t identity[][2] = {
        {0x08u, 0x1c301d30u}, {0x68u, 0xc00u}, {0x6cu, 0xd00u}, {0x70u, 0xc00u},
        {0x90u, 0x0c200d20u}, {0xb4u, 0xc00u}, {0xb8u, 0xd00u}, {0xbcu, 0xc00u},
    };
    for (uint32_t i = 0u; i < sizeof identity / sizeof identity[0]; ++i)
        *recomp_memory_u32(0x355260u + identity[i][0]) = identity[i][1];
    CHECK(recomp_d3d8_state_set_simple(0x40a60u, 0x55555555u));
    CHECK(recomp_d3d8_state_set_simple(0x40a84u, 0x2b2b2b2bu));
    for (uint32_t stage = 0u; stage < 4u; ++stage) *recomp_memory_u32(STAGES + stage * 128u + 20u) = 0u;
    invoke(0x348240u, 5u, 3u, VERTICES, 48u);
    CHECK(submits == 2u && submitted.data.draw.four_tap_filter &&
        submitted.data.draw.four_tap_factors[0] == 0x55555555u &&
        submitted.data.draw.four_tap_factors[3] == 0x2b2b2b2bu &&
        submitted.data.draw.four_tap_scale == 1.0f && submitted.data.draw.four_tap_sum_scale == 1.0f);
    *recomp_memory_u32(STAGES + 20u) = 2u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 48u); return 1; }
    CHECK(submits == 2u && strcmp(stop_reason, "d3d8:draw-pixel-shader-unsupported") == 0);
    *recomp_memory_u32(STAGES + 20u) = 0u;
    *recomp_memory_u32(0x355268u) ^= 1u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 48u); return 1; }
    CHECK(submits == 2u && strcmp(stop_reason, "d3d8:draw-pixel-shader-unsupported") == 0);
    return 0;
}

static int bright_tests(void)
{
    if (setjmp(stopped) != 0) { fprintf(stderr, "Unexpected stop: %s\n", stop_reason); return 1; }
    initialize();
    static const uint32_t shader[][2] = {
        {0x00u, 0xd830d150u}, {0x04u, 0x5d301010u}, {0x20u, 0xcu}, {0x24u, 0x1c80u},
        {0x28u, 0x1a1a1a1au}, {0x68u, 0xd00u}, {0x6cu, 0xc0u}, {0x88u, 0xc820c140u},
        {0x8cu, 0x4d200000u}, {0xb4u, 0xd00u}, {0xb8u, 0xc0u}, {0xd4u, 0x11102u}, {0xd8u, 1u},
    };
    for (uint32_t i = 0u; i < sizeof shader / sizeof shader[0]; ++i)
        *recomp_memory_u32(INPUT + shader[i][0]) = shader[i][1];
    invoke(0x349490u, INPUT, OUTPUT, 0u, 0u);
    invoke(0x349520u, *recomp_memory_u32(OUTPUT), 0u, 0u, 0u);
    *recomp_memory_u32(DEVICE + 0xf88u) = 0x7000u;
    *recomp_memory_u32(STAGES + 12u) = *recomp_memory_u32(STAGES + 16u) = 5u;
    *recomp_memory_u32(DEVICE + 0x798u) = 0x104u;
    invoke(0x348240u, 5u, 3u, VERTICES, 24u);
    CHECK(submits == 1u && submitted.data.draw.bright_pass && !submitted.data.draw.four_tap_filter);
    CHECK(submitted.data.draw.bright_pass_threshold == 0x1a1a1a1au);
    /* SetPixelShaderConstant(0) reaches stage-0 C0 through the zero C0 mapping. */
    const float threshold[4] = {0.2f, 0.2f, 0.2f, 0.2f};
    recomp_guest_store(INPUT + 0x100u, threshold, sizeof threshold);
    invoke(0x349720u, 0u, INPUT + 0x100u, 1u, 0u);
    invoke(0x348240u, 5u, 3u, VERTICES, 24u);
    CHECK(submits == 2u && submitted.data.draw.bright_pass_threshold == 0x33333333u);
    *recomp_memory_u32(STAGES + 16u) = 2u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 24u); return 1; }
    CHECK(submits == 2u && strcmp(stop_reason, "d3d8:draw-pixel-shader-unsupported") == 0);
    *recomp_memory_u32(STAGES + 16u) = 5u;
    CHECK(recomp_d3d8_state_set_simple(0x40ac4u, 0x4d200001u));
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 24u); return 1; }
    CHECK(submits == 2u && strcmp(stop_reason, "d3d8:draw-pixel-shader-unsupported") == 0);
    /* Bloom composite: MODULATE2X(TEXTURE, TFACTOR) with stage-0 alpha disabled. */
    initialize();
    *recomp_memory_u32(DEVICE + 0xf88u) = 0x7000u;
    *recomp_memory_u32(DEVICE + 0x798u) = 0x104u;
    *recomp_memory_u32(STAGES + 48u) = 5u;
    *recomp_memory_u32(STAGES + 60u) = 3u;
    *recomp_memory_u32(STAGES + 64u) = 1u;
    invoke(0x348240u, 5u, 3u, VERTICES, 24u);
    CHECK(submits == 1u && submitted.data.draw.texture_factor_modulate2x &&
        submitted.data.draw.material_alpha_mode == RECOMP_D3D_MATERIAL_ALPHA_SELECT_DIFFUSE &&
        submitted.data.draw.material_alpha == 1.0f);
    *recomp_memory_u32(DEVICE + 0x798u) = 0x144u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 28u); return 1; }
    CHECK(submits == 1u && strncmp(stop_reason, "d3d8:draw-texture-combine:", 26) == 0);
    return 0;
}

static int program_tests(void)
{
    if (setjmp(stopped) != 0) { fprintf(stderr, "Unexpected stop: %s\n", stop_reason); return 1; }
    initialize();
    enum { PROGRAM = 0x9000u };
    for (uint32_t i = 0u; i < 16u; ++i) *recomp_memory_u32(PROGRAM + 0x1cu + i * 16u) = 2u;
    *recomp_memory_u32(PROGRAM + 4u) = 0x10u;
    *recomp_memory_u32(PROGRAM + 8u) = 1u;
    *recomp_memory_u32(PROGRAM + 0xcu) = 5u;
    *recomp_memory_u32(PROGRAM + 0x1cu) = 0x32u;
    *recomp_memory_u32(PROGRAM + 0x38u) = 12u;
    *recomp_memory_u32(PROGRAM + 0x3cu) = 0x32u;
    *recomp_memory_u32(PROGRAM + 0xa8u) = 24u;
    *recomp_memory_u32(PROGRAM + 0xacu) = 0x22u;
    const uint32_t upload[5] = {(4u << 18u) | 0xb00u, 1u, 2u, 3u, 4u};
    recomp_guest_store(PROGRAM + 0x114u, upload, sizeof upload);
    const float one = 1.0f, two = 2.0f, constant = 7.0f;
    recomp_guest_store(DEVICE + 0x95cu, &one, 4u);
    recomp_guest_store(DEVICE + 0x960u, &one, 4u);
    recomp_guest_store(DEVICE + 0x96cu, &one, 4u);
    recomp_guest_store(DEVICE + 0x970u, &one, 4u);
    recomp_guest_store(DEVICE + 0x948u, &two, 4u);
    recomp_guest_store(0x354440u + 96u * 16u, &constant, 4u);
    *recomp_memory_u32(DEVICE + 0x798u) = PROGRAM + 1u;
    *recomp_memory_u32(DEVICE + 0xf88u) = 0x7000u;
    *recomp_memory_u32(STAGES + 20u) = 1u;
    *recomp_memory_u32(STAGES + 48u) = *recomp_memory_u32(STAGES + 64u) = 2u;
    invoke(0x348240u, 5u, 3u, VERTICES, 32u);
    const RecompD3dPresenterDrawCommand *draw = &submitted.data.draw;
    CHECK(submits == 1u && draw->fvf == 0x112u && draw->program_count == 1u);
    CHECK(draw->program[0][3] == 4u && draw->program_constants[96][0] == 7.0f);
    CHECK(draw->program_constants[58][0] == 360.0f && draw->program_constants[58][1] == -240.0f);
    CHECK(draw->program_constants[58][2] == 2.0f && draw->program_constants[59][0] == 360.53125f);
    CHECK(draw->program_constants[59][1] == 240.53125f && draw->program_constants[59][2] == 0.0f);
    CHECK(!draw->has_transform && draw->screen_space_offset[0] == 0.0f && !draw->program_alpha_mask);
    /* A 2x supersampled back buffer keeps the host at its logical 720 pixels. */
    recomp_guest_store(DEVICE + 0x95cu, &two, 4u);
    recomp_guest_store(DEVICE + 0x96cu, &two, 4u);
    invoke(0x348240u, 5u, 3u, VERTICES, 32u);
    CHECK(draw->program_constants[58][0] == 360.0f && draw->program_constants[59][0] == 360.53125f);
    recomp_guest_store(DEVICE + 0x95cu, &one, 4u);
    recomp_guest_store(DEVICE + 0x96cu, &one, 4u);
    /* Multisampled back-buffer rendering shifts the sample grid by half a pixel. */
    *recomp_memory_u32(DEVICE + 8u) = 0x8000u;
    *recomp_memory_u32(0x3554c0u) = 1u;
    invoke(0x348240u, 5u, 3u, VERTICES, 32u);
    CHECK(draw->program_constants[59][0] == 360.03125f);
    /* Stage 1 masks alpha with its own texture. */
    const uint32_t mask_stage[6][2] = {{48u, 2u}, {56u, 1u}, {60u, 0u}, {64u, 4u}, {72u, 1u}, {76u, 2u}};
    for (uint32_t i = 0u; i < 6u; ++i) *recomp_memory_u32(STAGES + 128u + mask_stage[i][0]) = mask_stage[i][1];
    *recomp_memory_u32(STAGES + 128u + 20u) = 1u;
    *recomp_memory_u32(STAGES + 128u + 24u) = 0xc0000000u;
    invoke(0x348240u, 5u, 3u, VERTICES, 32u);
    CHECK(submits == 4u && !draw->program_alpha_mask);
    *recomp_memory_u32(DEVICE + 0xf8cu) = 0x7000u;
    invoke(0x348240u, 5u, 3u, VERTICES, 32u);
    CHECK(submits == 5u && draw->program_alpha_mask && draw->reflection_bytes == ram + 0x8000u);
    CHECK(draw->program_mask_lod_bias == -2.0f);
    *recomp_memory_u32(STAGES + 48u) = 4u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 32u); return 1; }
    CHECK(submits == 5u && strncmp(stop_reason, "d3d8:vertex-program-stages", 26) == 0);
    *recomp_memory_u32(STAGES + 48u) = 2u;
    *recomp_memory_u32(PROGRAM + 0x38u) = 16u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 32u); return 1; }
    CHECK(strncmp(stop_reason, "d3d8:vertex-program-declaration:00009001:2:", 43) == 0);
    return 0;
}

int main(void)
{
    initialize();
    if (pixel_tests() || draw_tests() || point_tests() || lighting_tests() || filter_tests() || bright_tests() ||
        program_tests()) return 1;
    puts("PASS D3D8 pixel lifecycle/constants and semantic draw ABI/snapshots");
    return 0;
}
