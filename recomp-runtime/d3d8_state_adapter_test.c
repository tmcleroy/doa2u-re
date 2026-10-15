/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "d3d8_state_adapter.h"
#include "d3d8_adapter.h"
#include "d3d_light_model.h"

#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL D3D state line %d: %s\n", __LINE__, #condition); return 1; \
} } while (0)

enum { DEVICE = 0x00355500u, STACK = 0x1000u, INPUT = 0x2000u, DIRTY = 0x00355058u };
RecompRuntime recomp_runtime;
uint8_t *recomp_fast_ram;
static uint8_t ram[0x400000u];
static uint32_t allocated = 0x00360000u, freed, submits;
static bool fail_allocation;
static RecompD3dPresenterCommand submitted;
static jmp_buf stopped;
static char stop_reason[128];

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
    if (address > sizeof ram || size > sizeof ram - address) abort();
    return ram + address;
}
uint32_t *recomp_memory_u32_checked(uint32_t address)
{
    return (uint32_t *)(void *)recomp_memory(address, 4u);
}
void recomp_guest_load(void *to, uint32_t from, size_t size)
{
    memcpy(to, recomp_memory(from, size), size);
}
void recomp_guest_store(uint32_t to, const void *from, size_t size)
{
    memcpy(recomp_memory(to, size), from, size);
}
void recomp_guest_memcpy(uint32_t to, uint32_t from, size_t size)
{
    memcpy(recomp_memory(to, size), recomp_memory(from, size), size);
}
uint32_t recomp_kernel_allocate_pool(uint32_t size)
{
    uint32_t result = allocated;
    if (fail_allocation || size > sizeof ram - allocated) return 0u;
    allocated += size;
    return result;
}
void recomp_kernel_free_pool(uint32_t address) { freed = address; }
uint32_t recomp_d3d8_require_device(void) { return DEVICE; }
void recomp_d3d8_submit(const RecompD3dPresenterCommand *command)
{
    submitted = *command;
    ++submits;
}

static uint32_t invoke(uint32_t address, uint32_t a, uint32_t b)
{
    RecompFunction function = recomp_d3d8_state_lookup_manual(address);
    if (function == NULL) abort();
    recomp_runtime.registers.esp = STACK;
    *recomp_memory_u32(STACK) = 0x12345678u;
    *recomp_memory_u32(STACK + 4u) = a;
    *recomp_memory_u32(STACK + 8u) = b;
    recomp_runtime.registers.eax = 0x11223344u;
    function();
    return recomp_runtime.registers.eax;
}

static RecompD3dLightRecord record(uint32_t index)
{
    RecompD3dLightRecord result;
    recomp_guest_load(&result, *recomp_memory_u32(DEVICE + 0x7a4u) +
        index * sizeof result, sizeof result);
    return result;
}

static int check_frame_state(void);

int main(void)
{
    int passed = 1;
    CHECK(recomp_d3d8_state_lookup_manual(0u) == NULL);
    for (uint32_t i = 0u; i < 768u; ++i) ram[INPUT + i] = (uint8_t)i;
    *recomp_memory_u32(DEVICE + 0x2478u) = 1u;
    CHECK(invoke(0x344180u, 1u, INPUT) == 0x11223344u);
    CHECK(recomp_runtime.registers.esp == STACK + 12u && submits == 1u);
    CHECK(submitted.type == RECOMP_D3D_PRESENTER_COMMAND_GAMMA);
    CHECK(memcmp(submitted.data.gamma, ram + INPUT, 768u) == 0);
    CHECK(*recomp_memory_u32(DEVICE + 0x240cu) == 1u);
    CHECK(*recomp_memory_u32(DEVICE + 0x2408u) == 1u);
    CHECK(memcmp(ram + DEVICE + 0x2104u, ram + INPUT, 768u) == 0);
    invoke(0x344180u, 2u, INPUT);
    CHECK(submits == 1u);
    ram[INPUT] = 123u;
    *recomp_memory_u32(DEVICE + 0x2478u) = 2u;
    invoke(0x344180u, 2u, INPUT);
    CHECK(submits == 2u && *recomp_memory_u32(DEVICE + 0x2404u) == 0u);
    CHECK(*recomp_memory_u32(DEVICE + 0x240cu) == 0u);

    float matrix[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    recomp_guest_store(INPUT, matrix, sizeof matrix);
    CHECK(invoke(0x344210u, 9u, INPUT) == 0x11223344u);
    CHECK(recomp_runtime.registers.esp == STACK + 12u);
    CHECK(memcmp(ram + DEVICE + 0xea0u, matrix, sizeof matrix) == 0);
    memset(ram + INPUT, 0, sizeof matrix);
    CHECK(invoke(0x344330u, 9u, INPUT) == 0x11223344u);
    CHECK(memcmp(ram + INPUT, matrix, sizeof matrix) == 0);
    CHECK(*recomp_memory_u32(DIRTY) == 0x200u);
    invoke(0x344210u, 1u, INPUT);
    float derived[3];
    recomp_guest_load(derived, DEVICE + 0x93cu, sizeof derived);
    CHECK(derived[0] == 1.0f && derived[1] == 1.0f && derived[2] == 1.0f);
    CHECK((*recomp_memory_u32(DEVICE + 8u) & 2u) != 0u);
    matrix[10] = 2.0f; matrix[11] = 1.0f; matrix[14] = -4.0f; matrix[15] = 0.0f;
    recomp_guest_store(INPUT, matrix, sizeof matrix);
    invoke(0x344210u, 1u, INPUT);
    recomp_guest_load(derived, DEVICE + 0x93cu, sizeof derived);
    CHECK(derived[0] == 2.0f && derived[1] == 4.0f && derived[2] == 0.25f);
    CHECK((*recomp_memory_u32(DEVICE + 8u) & 2u) == 0u);
    matrix[10] = matrix[11];
    recomp_guest_store(INPUT, matrix, sizeof matrix);
    invoke(0x344210u, 1u, INPUT);
    recomp_guest_load(derived, DEVICE + 0x93cu, sizeof derived);
    CHECK(derived[0] == 0.0f && derived[1] == 1.0f && derived[2] == 1.0f);
    if (setjmp(stopped) == 0) { invoke(0x344210u, 10u, INPUT); return 1; }
    CHECK(strcmp(stop_reason, "d3d8:unsupported-transform:10") == 0);
    CHECK(invoke(0x3465c0u, 0xaabbccddu, 0u) == 0x11223344u);
    CHECK(*recomp_memory_u32(0x355488u) == 0xaabbccddu);
    CHECK(recomp_runtime.registers.esp == STACK + 8u);

    CHECK(invoke(0x345300u, 3u, 0u) == 0u);
    CHECK(recomp_runtime.registers.esp == STACK + 12u);
    RecompD3dLightRecord light = record(3u);
    CHECK(*recomp_memory_u32(DEVICE + 0x7a8u) == 16u);
    CHECK(light.desc.type == 3u && light.flags == 7u && light.slot == UINT32_MAX);
    CHECK(light.desc.diffuse[3] == 0.0f && light.normalized_direction[2] == -1.0f);
    invoke(0x345300u, 0u, 1u);
    invoke(0x345300u, 1u, 1u);
    CHECK(record(0u).slot == 0u && record(1u).slot == 1u);
    CHECK(*recomp_memory_u32(DEVICE + 0x7ccu) == 5u);
    uint32_t old_array = *recomp_memory_u32(DEVICE + 0x7a4u);
    RecompD3dLightDesc desc;
    recomp_d3d_light_default(&desc);
    desc.type = 1u;
    recomp_guest_store(INPUT, &desc, sizeof desc);
    CHECK(invoke(0x345040u, 16u, INPUT) == 0u);
    uint32_t new_array = *recomp_memory_u32(DEVICE + 0x7a4u);
    CHECK(freed == old_array && new_array != old_array);
    CHECK(*recomp_memory_u32(DEVICE + 0x7a8u) == 32u);
    CHECK(*recomp_memory_u32(DEVICE + 0x7acu) == new_array);
    CHECK(*recomp_memory_u32(DEVICE + 0x7b0u) == new_array + sizeof light);
    CHECK(record(0u).slot == 0u && record(16u).desc.type == 1u);
    CHECK(invoke(0x345040u, 0u, INPUT) == 0u);
    CHECK(*recomp_memory_u32(DEVICE + 0x7ccu) == 6u);
    *recomp_memory_u32(DIRTY) = 0u;
    invoke(0x345300u, 0u, 1u);
    CHECK(*recomp_memory_u32(DIRTY) == 0u);
    invoke(0x345300u, 0u, 0u);
    CHECK(record(0u).slot == UINT32_MAX && *recomp_memory_u32(DEVICE + 0x7acu) == 0u);
    CHECK(*recomp_memory_u32(DEVICE + 0x7ccu) == 4u);
    CHECK(*recomp_memory_u32(DIRTY) == 0x10000u);
    fail_allocation = true;
    CHECK(invoke(0x345040u, 32u, INPUT) == 0x8007000eu);
    CHECK(*recomp_memory_u32(DEVICE + 0x7a4u) == new_array);
    CHECK(*recomp_memory_u32(DEVICE + 0x7a8u) == 32u);
    CHECK(invoke(0x345300u, UINT32_MAX, 0u) == 0x8007000eu);
    fail_allocation = false;
    desc.type = 2u;
    recomp_guest_store(INPUT, &desc, sizeof desc);
    if (setjmp(stopped) == 0) { invoke(0x345040u, 0u, INPUT); return 1; }
    CHECK(strcmp(stop_reason, "d3d8:unsupported-spot-light") == 0);
    for (uint32_t i = 0u; i < 8u; ++i) invoke(0x345300u, i, 1u);
    if (setjmp(stopped) == 0) { invoke(0x345300u, 8u, 1u); return 1; }
    CHECK(strcmp(stop_reason, "d3d8:unsupported-active-light-count") == 0);
    passed &= check_frame_state() == 0;
    if (passed) puts("PASS D3D8 gamma, transform, fog, light, shader and frame-state ABI/shadow state");
    return passed ? 0 : 1;
}

static int check_frame_state(void)
{
    /* SetRenderState_Simple: fastcall, plain ret, caller owns its shadows. */
    recomp_runtime.registers.ecx = 0x4035cu;
    recomp_runtime.registers.edx = 1u;
    CHECK(invoke(0x346240u, 0u, 0u) == 0x11223344u);
    CHECK(recomp_runtime.registers.esp == STACK + 4u);

    CHECK(invoke(0x347490u, 1u, 0u) == 0u);
    CHECK(*recomp_memory_u32(0x35549cu) == 1u && recomp_runtime.registers.esp == STACK + 8u);
    CHECK(invoke(0x347520u, 1u, 0u) == 0u);
    CHECK(*recomp_memory_u32(0x3554a0u) == 1u);
    CHECK(invoke(0x346930u, 0x1b02u, 0u) == 0u);
    CHECK(*recomp_memory_u32(0x35548cu) == 0x1b02u);
    if (setjmp(stopped) == 0) { invoke(0x346930u, 0x1b03u, 0u); return 1; }
    CHECK(strncmp(stop_reason, "d3d8:unsupported-fill-mode", 26) == 0);
    CHECK(invoke(0x346540u, 0u, 0u) == 0u);
    CHECK(*recomp_memory_u32(0x3554bcu) == 0u);
    CHECK(invoke(0x346610u, 0x901u, 0u) == 0u);
    CHECK(*recomp_memory_u32(0x3554acu) == 0x901u);
    CHECK(invoke(0x347800u, 0u, 0u) == 0u);
    CHECK(*recomp_memory_u32(0x3554c0u) == 0u);
    if (setjmp(stopped) == 0) { invoke(0x347800u, 0x11u, 0u); return 1; }
    CHECK(strncmp(stop_reason, "d3d8:unsupported-state-value", 28) == 0);
    *recomp_memory_u32(DIRTY) = 0u;
    CHECK(invoke(0x3466c0u, 1u, 0u) == 0u);
    CHECK(*recomp_memory_u32(0x355498u) == 1u && *recomp_memory_u32(DIRTY) == 0x200u);

    /* SetMaterial copies 17 DWORDs into the device shadow and dirties it. */
    for (uint32_t i = 0u; i < 68u; ++i) ram[INPUT + i] = (uint8_t)(0xc0u + i);
    *recomp_memory_u32(DIRTY) = 0u;
    CHECK(invoke(0x3443a0u, INPUT, 0u) == 0u);
    CHECK(recomp_runtime.registers.esp == STACK + 8u);
    CHECK(memcmp(ram + DEVICE + 0xf00u, ram + INPUT, 68u) == 0);
    CHECK(*recomp_memory_u32(DIRTY) == 0x9000u);
    if (setjmp(stopped) == 0) { invoke(0x3443a0u, 0u, 0u); return 1; }
    CHECK(strcmp(stop_reason, "d3d8:material-pointer") == 0);

    CHECK(invoke(0x346760u, 0x3f800000u, 0u) == 0u);
    CHECK(*recomp_memory_u32(0x3554d4u) == 0x3f800000u);
    CHECK(invoke(0x346700u, 0xff123456u, 0u) == 0u);
    CHECK(recomp_d3d8_state_model()->texture_factor == 0xff123456u);
    CHECK(recomp_d3d8_state_set_simple(0x40304u, 1u));
    CHECK(recomp_d3d8_state_set_simple(0x40344u, 0x302u));
    CHECK(recomp_d3d8_state_set_simple(0x40348u, 0x303u));
    CHECK(!recomp_d3d8_state_set_simple(0x40345u, 0u));
    RecompD3dBlendState blend;
    recomp_d3d_blend_state(recomp_d3d8_state_model(), &blend);
    CHECK(blend.blend_enable && blend.src_factor == RECOMP_D3D_BLEND_SRC_ALPHA &&
        blend.dst_factor == RECOMP_D3D_BLEND_INV_SRC_ALPHA);
    RecompD3dDepthState depth;
    recomp_d3d_depth_state(recomp_d3d8_state_model(), &depth);
    CHECK(depth.depth_write_enable && depth.depth_test_enable && depth.stencil_enable);
    CHECK(invoke(0x346a60u, 2u, 0u) == 0u);
    CHECK(*recomp_memory_u32(0x355484u) == 2u);
    /* SetVertexShader binds the FVF declaration and latches device shadows. */
    *recomp_memory_u32(DEVICE + 0x794u) = 0x357b80u;
    *recomp_memory_u32(0x357b80u + 4u) = 0x402u;
    *recomp_memory_u32(0x357b80u + 0x10u) = 0x44u;
    *recomp_memory_u32(DIRTY) = 0u;
    CHECK(invoke(0x349320u, 0x112u, 0u) == 0u);
    CHECK(recomp_runtime.registers.esp == STACK + 8u);
    CHECK(*recomp_memory_u32(DEVICE + 0x794u) == 0x357b80u);
    CHECK(*recomp_memory_u32(DEVICE + 0x798u) == 0x112u);
    CHECK(*recomp_memory_u32(DEVICE + 0x79cu) == 0u);
    CHECK(*recomp_memory_u32(DIRTY) == 0x1670u);
    CHECK(*recomp_memory_u32(0x357b80u + 0x1cu) == 0x32u);
    CHECK(*recomp_memory_u32(0x357b80u + 0x38u) == 0x0cu);
    CHECK(*recomp_memory_u32(0x357b80u + 0xa8u) == 0x18u);
    CHECK(*recomp_memory_u32(0x357b80u + 0x10u) == 2u);
    *recomp_memory_u32(DIRTY) = 0u;
    CHECK(invoke(0x349320u, 0x112u, 0u) == 0u);
    CHECK(*recomp_memory_u32(DIRTY) == 0x70u);
    if (setjmp(stopped) == 0) { invoke(0x349320u, 0x357b81u, 0u); return 1; }
    CHECK(strncmp(stop_reason, "d3d8:programmable-shader-unbound", 32) == 0);
    if (setjmp(stopped) == 0) { invoke(0x349320u, 0x902u, 0u); return 1; }
    CHECK(strncmp(stop_reason, "d3d8:invalid-fvf", 16) == 0);
    *recomp_memory_u32(DEVICE + 0x794u) = 0u;
    if (setjmp(stopped) == 0) { invoke(0x349320u, 0x112u, 0u); return 1; }
    CHECK(strcmp(stop_reason, "d3d8:missing-declaration") == 0);
    return 0;
}
