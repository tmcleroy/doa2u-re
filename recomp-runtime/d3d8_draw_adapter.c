/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "d3d8_draw_adapter.h"
#include "d3d8_adapter.h"
#include "d3d8_resource_adapter.h"
#include "d3d8_state_adapter.h"
#include "d3d_device_model.h"
#include "d3d_draw_model.h"
#include "d3d_light_model.h"
#include "kernel_abi.h"
#include "stop_report.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* 5849 API layout verified from instructions in the selected DOA2 image. */
enum {
    DIRTY = 0x355058u, STAGES = 0x355060u, PIXEL_SHADOW = 0x355260u,
    STREAMS = 0x3579a8u, PIXEL_HANDLE = 0x784u,
    PIXEL_DEFINITION_SIZE = 0xf0u,
};

static uint32_t definition(uint32_t handle)
{
    if (handle == 0u || *recomp_memory_u32(handle) == 0u)
        recomp_stop(2, "d3d8:invalid-pixel-shader:%08x", handle);
    const uint32_t data = *recomp_memory_u32(handle + 8u);
    if (data == 0u) recomp_stop(2, "d3d8:invalid-pixel-definition");
    recomp_memory(data, PIXEL_DEFINITION_SIZE);
    return data;
}

static void pixel_create(void)
{
    const uint32_t source = kernel_arg(1u), output = kernel_arg(2u);
    if (source == 0u || output == 0u) recomp_stop(2, "d3d8:pixel-create-pointer");
    recomp_memory(source, PIXEL_DEFINITION_SIZE);
    recomp_memory(output, 4u);
    const uint32_t handle = recomp_kernel_allocate_pool(0xfcu);
    if (handle == 0u) { kernel_return(2u, RECOMP_D3D_OUT_OF_MEMORY); return; }
    *recomp_memory_u32(handle) = 1u;
    *recomp_memory_u32(handle + 4u) = 1u;
    *recomp_memory_u32(handle + 8u) = handle + 12u;
    recomp_guest_memcpy(handle + 12u, source, PIXEL_DEFINITION_SIZE);
    *recomp_memory_u32(output) = handle;
    kernel_return(2u, 0u);
}

static void pixel_delete(void)
{
    const uint32_t handle = kernel_arg(1u);
    definition(handle);
    if (--*recomp_memory_u32(handle) == 0u && *recomp_memory_u32(handle + 4u) != 0u)
        recomp_kernel_free_pool(handle);
    recomp_runtime.registers.esp += 8u;
}

static void pixel_get(void)
{
    const uint32_t device = recomp_d3d8_require_device(), output = kernel_arg(1u);
    if (output == 0u) recomp_stop(2, "d3d8:pixel-get-pointer");
    *recomp_memory_u32(output) = *recomp_memory_u32(device + PIXEL_HANDLE);
    recomp_runtime.registers.esp += 8u;
}

static void pixel_method(uint32_t method, uint32_t value)
{
    if (!recomp_d3d8_state_set_simple(method, value))
        recomp_stop(2, "d3d8:pixel-method:%08x:%08x", method, value);
}

static void pixel_set(void)
{
    const uint32_t device = recomp_d3d8_require_device(), handle = kernel_arg(1u);
    if (handle == 0u) {
        *recomp_memory_u32(DIRTY) |= 0x4800u;
        if (*recomp_memory_u32(device + 0x788u) != 0u) *recomp_memory_u32(DIRTY) |= 0x2000u;
    } else {
        const uint32_t data = definition(handle);
        const uint32_t inputs = *recomp_memory_u32(data + 0x20u) | *recomp_memory_u32(data + 0x24u);
        if (inputs == 0u && *recomp_memory_u32(device + 0x788u) != 0u)
            *recomp_memory_u32(DIRTY) |= 0x2000u;
        *recomp_memory_u32(device + 0x788u) = inputs;
        *recomp_memory_u32(device + 0x78cu) = *recomp_memory_u32(data + 0xecu) & 0x100u;
        *recomp_memory_u32(device + 0x790u) = *recomp_memory_u32(data + 0xd8u);
        *recomp_memory_u32(DIRTY) |= 0x4000u;
        if ((*recomp_memory_u32(device + 8u) & 0x10u) == 0u) {
            recomp_guest_memcpy(PIXEL_SHADOW, data, 0xe4u);
            *recomp_memory_u32(0x355480u) = *recomp_memory_u32(data + 0xd8u);
        }
        for (uint32_t i = 0u; i < 8u; ++i) {
            pixel_method(0x40260u + i * 4u, *recomp_memory_u32(data + i * 4u));
            pixel_method(0x40a60u + i * 4u, *recomp_memory_u32(data + 0x28u + i * 4u));
            pixel_method(0x40a80u + i * 4u, *recomp_memory_u32(data + 0x48u + i * 4u));
            pixel_method(0x40aa0u + i * 4u, *recomp_memory_u32(data + 0x68u + i * 4u));
            pixel_method(0x40ac0u + i * 4u, *recomp_memory_u32(data + 0x88u + i * 4u));
            pixel_method(0x41e40u + i * 4u, *recomp_memory_u32(data + 0xb4u + i * 4u));
        }
        pixel_method(0x417f8u, *recomp_memory_u32(data + 0xa8u));
        pixel_method(0x41e20u, *recomp_memory_u32(data + 0xacu));
        pixel_method(0x41e24u, *recomp_memory_u32(data + 0xb0u));
        pixel_method(0x41e60u, *recomp_memory_u32(data + 0xd4u));
        pixel_method(0x41e74u, *recomp_memory_u32(data + 0xdcu));
        pixel_method(0x41e78u, *recomp_memory_u32(data + 0xe0u));
        if (inputs != 0u) {
            pixel_method(0x40288u, *recomp_memory_u32(data + 0x20u));
            pixel_method(0x4028cu, *recomp_memory_u32(data + 0x24u));
        }
    }
    *recomp_memory_u32(device + PIXEL_HANDLE) = handle;
    recomp_runtime.registers.esp += 8u;
}

static void pixel_constant(void)
{
    const uint32_t device = recomp_d3d8_require_device();
    const uint32_t data = definition(*recomp_memory_u32(device + PIXEL_HANDLE));
    const uint32_t first = kernel_arg(1u), source = kernel_arg(2u), count = kernel_arg(3u);
    if (first > 8u || count > 8u - first) recomp_stop(2, "d3d8:pixel-constant-range");
    const float *values = (const float *)(const void *)recomp_memory(source, count * 16u);
    for (uint32_t i = 0u; i < count; ++i) {
        uint32_t packed = 0u;
        static const uint32_t shifts[4] = {16u, 8u, 0u, 24u};
        for (uint32_t channel = 0u; channel < 4u; ++channel) {
            const float value = fmaxf(0.0f, fminf(1.0f, values[i * 4u + channel]));
            packed |= (uint32_t)nearbyintf(value * 255.0f) << shifts[channel];
        }
        const uint32_t index = first + i;
        *recomp_memory_u32(device + 0x8e4u + index * 4u) = packed;
        for (uint32_t bank = 0u; bank < 3u; ++bank) {
            const uint32_t mapping = *recomp_memory_u32(data + 0xe4u + bank * 4u);
            const uint32_t slots = bank == 2u ? 2u : 8u;
            for (uint32_t slot = 0u; slot < slots; ++slot) {
                if (((mapping >> (slot * 4u)) & 15u) != index) continue;
                const uint32_t base = bank == 2u ? 0x41e20u : 0x40a60u + bank * 0x20u;
                const uint32_t shadow = bank == 2u ? PIXEL_SHADOW + 0xacu : PIXEL_SHADOW + 0x28u + bank * 0x20u;
                *recomp_memory_u32(shadow + slot * 4u) = packed;
                pixel_method(base + slot * 4u, packed);
            }
        }
    }
    recomp_runtime.registers.esp += 16u;
}

static uint32_t stream_vertices(uint32_t first, uint32_t *stride)
{
    const uint32_t buffer = *recomp_memory_u32(STREAMS + 8u);
    *stride = *recomp_memory_u32(STREAMS);
    if (buffer == 0u || (*recomp_memory_u32(buffer) & 0x70000u) != 0u)
        recomp_stop(2, "d3d8:draw-vertex-buffer");
    uint32_t address;
    if (!recomp_d3d_texture_resolve_cpu_address(*recomp_memory_u32(buffer + 4u), &address))
        recomp_stop(2, "d3d8:draw-vertex-address");
    const uint64_t offset = (uint64_t)first * *stride + address;
    if (offset > UINT32_MAX) recomp_stop(2, "d3d8:draw-vertex-overflow");
    return (uint32_t)offset;
}

static void multiply(const float left[16], const float right[16], float result[16])
{
    for (uint32_t row = 0u; row < 4u; ++row) {
        for (uint32_t column = 0u; column < 4u; ++column) {
            float value = 0.0f;
            for (uint32_t k = 0u; k < 4u; ++k) value += left[row * 4u + k] * right[k * 4u + column];
            if (!isfinite(value)) recomp_stop(2, "d3d8:nonfinite-transform");
            result[row * 4u + column] = value;
        }
    }
}

static void attach_texture(uint32_t device, uint32_t stage, RecompD3dPresenterDrawCommand *draw)
{
    const uint32_t resource = *recomp_memory_u32(device + 0xf88u + stage * 4u);
    if (resource == 0u) return;
    if (!recomp_d3d8_resource_describe(resource, &draw->texture) || draw->texture.depth)
        recomp_stop(2, "d3d8:draw-texture-description");
    const RecompD3dTextureDesc *desc = &draw->texture;
    uint64_t bytes = recomp_d3d_texture_compressed_mip_span(desc);
    if (bytes == 0u) bytes = desc->linear ? (uint64_t)desc->pitch * desc->height :
        (uint64_t)desc->width * desc->height * desc->bits_per_pixel / 8u;
    if (bytes == 0u || bytes > RECOMP_XBOX_RAM_SIZE)
        recomp_stop(2, "d3d8:draw-texture-span");
    uint32_t address;
    if (!recomp_d3d_texture_resolve_cpu_address(desc->data, &address))
        recomp_stop(2, "d3d8:draw-texture-address");
    draw->texture_bytes = recomp_memory(address, (size_t)bytes);
    draw->texture_byte_count = (uint32_t)bytes;
    draw->has_texture = true;
    draw->address_u = *recomp_memory_u32(STAGES + stage * 128u);
    draw->address_v = *recomp_memory_u32(STAGES + stage * 128u + 4u);
    if (desc->format_byte == RECOMP_D3D_TEXTURE_FORMAT_P8) {
        const uint32_t palette = *recomp_memory_u32(device + 0xf98u + stage * 4u);
        if (palette == 0u) recomp_stop(2, "d3d8:draw-missing-palette");
        if (!recomp_d3d_texture_resolve_cpu_address(*recomp_memory_u32(palette + 4u), &address))
            recomp_stop(2, "d3d8:draw-palette-address");
        draw->palette_bytes = recomp_memory(address, 1024u);
        draw->palette_byte_count = 1024u;
    }
    for (uint32_t slot = 0u; slot < 2u; ++slot) {
        const uint32_t header = device + 0x1a24u + slot * 24u;
        if ((*recomp_memory_u32(header + 4u) & 0x7fffffffu) == (desc->data & 0x7fffffffu)) {
            draw->texture_is_backbuffer = slot == 0u;
            draw->texture_is_front_buffer = slot == 1u;
        }
    }
}

static bool four_tap(uint32_t device, RecompD3dPresenterDrawCommand *draw)
{
    /* Weighted four-source combiner: r0 = c0*t0 + c1*t1 and r1 = c0*t2 + c1*t3 with
       unique per-stage factors, then r0 + r1. The stage sums are nonnegative, so the
       signed and unsigned stage-2 inputs of the two measured variants agree. */
    static const uint32_t expected[][2] = {
        {0x20u, 0xcu}, {0x24u, 0x1c80u}, {0xd4u, 0x11103u}, {0xd8u, 0x8421u},
    };
    static const uint32_t offsets[8] = {0x08u, 0x68u, 0x6cu, 0x70u, 0x90u, 0xb4u, 0xb8u, 0xbcu};
    /* Doubled stages with a halved sum (the DOA3 contract), or identity scales. */
    static const struct { uint32_t words[8]; float scale, sum_scale; } variants[] = {
        {{0xdc30dd30u, 0x10c00u, 0x10d00u, 0x30c00u, 0xcc20cd20u, 0x10c00u, 0x10d00u, 0x30c00u},
            2.0f, 0.5f},
        {{0x1c301d30u, 0xc00u, 0xd00u, 0xc00u, 0x0c200d20u, 0xc00u, 0xd00u, 0xc00u}, 1.0f, 1.0f},
    };
    static const uint32_t simple[][2] = {
        {0x40260u, 0xd1d8d2d9u}, {0x40264u, 0xd1dad2dbu},
        {0x40ac0u, 0xc1c8c2c9u}, {0x40ac4u, 0xc1cac2cbu},
    };
    /* C0/C1 of stage 0 weight t0/t1; C0/C1 of stage 1 weight t2/t3. */
    static const uint32_t factors[4] = {0x40a60u, 0x40a80u, 0x40a64u, 0x40a84u};
    /* Single-level sources make point and disabled mip filtering equivalent. */
    static const uint32_t sampler[][2] = {
        {0u, 3u}, {1u, 3u}, {3u, 2u}, {4u, 2u},
        {7u, 0u}, {9u, 0u}, {10u, 0u}, {11u, 0u}, {21u, 0u},
    };
    if (!draw->has_texture || ((draw->fvf >> 8u) & 15u) != 4u ||
        draw->texture.bits_per_pixel != 32u || draw->texture.mip_levels != 1u ||
        !((draw->texture.format_byte == 0x12u && draw->texture.linear) ||
          (draw->texture.format_byte == 6u && !draw->texture.linear))) return false;
    for (uint32_t i = 0u; i < sizeof expected / sizeof expected[0]; ++i) {
        if (*recomp_memory_u32(PIXEL_SHADOW + expected[i][0]) != expected[i][1]) return false;
    }
    uint32_t variant = 0u;
    for (; variant < 2u; ++variant) {
        uint32_t i = 0u;
        while (i < 8u && *recomp_memory_u32(PIXEL_SHADOW + offsets[i]) == variants[variant].words[i]) ++i;
        if (i == 8u) break;
    }
    if (variant == 2u) return false;
    const RecompD3dRenderStateModel *state = recomp_d3d8_state_model();
    for (uint32_t i = 0u; i < sizeof simple / sizeof simple[0]; ++i) {
        uint32_t value;
        if (!recomp_d3d_get_simple_render_state(state, simple[i][0], &value) || value != simple[i][1])
            return false;
    }
    for (uint32_t i = 0u; i < 4u; ++i) {
        if (!recomp_d3d_get_simple_render_state(state, factors[i], &draw->four_tap_factors[i]))
            return false;
    }
    for (uint32_t stage = 0u; stage < 4u; ++stage) {
        RecompD3dTextureDesc desc;
        if (!recomp_d3d8_resource_describe(*recomp_memory_u32(device + 0xf88u + stage * 4u), &desc) ||
            desc.data != draw->texture.data || desc.format_byte != draw->texture.format_byte ||
            desc.width != draw->texture.width || desc.height != draw->texture.height ||
            desc.pitch != draw->texture.pitch || desc.mip_levels != 1u) return false;
        for (uint32_t i = 0u; i < sizeof sampler / sizeof sampler[0]; ++i) {
            if (*recomp_memory_u32(STAGES + stage * 128u + sampler[i][0] * 4u) != sampler[i][1])
                return false;
        }
        if (*recomp_memory_u32(STAGES + stage * 128u + 20u) > 1u) return false;
        float bias;
        recomp_guest_load(&bias, STAGES + stage * 128u + 24u, sizeof bias);
        if (!isfinite(bias) || *recomp_memory_u32(STAGES + stage * 128u + 112u) != stage) return false;
    }
    draw->four_tap_filter = true;
    draw->four_tap_scale = variants[variant].scale;
    draw->four_tap_sum_scale = variants[variant].sum_scale;
    return true;
}

static bool bright_pass(uint32_t device, RecompD3dPresenterDrawCommand *draw)
{
    /* Verified combiner: r1 = t0 - c0; r0 = 2 * max(0, r1) - 1; out = max(0, r0).
       Two stages, unique C0/C1, MSB mux; only stage 0 samples (PROJECT2D). */
    static const uint32_t simple[][2] = {
        {0x40260u, 0xd830d150u}, {0x40264u, 0x5d301010u},
        {0x40ac0u, 0xc820c140u}, {0x40ac4u, 0x4d200000u},
        {0x40aa0u, 0xd00u}, {0x40aa4u, 0xc0u}, {0x41e40u, 0xd00u}, {0x41e44u, 0xc0u},
        {0x41e60u, 0x11102u}, {0x40288u, 0xcu}, {0x4028cu, 0x1c80u},
    };
    /* Gaussian-cubic min/mag, no mips, UV0, no color key, sign, kill or transform. */
    static const uint32_t sampler[][2] = {
        {3u, 5u}, {4u, 5u}, {5u, 0u}, {9u, 0u}, {10u, 0u}, {11u, 0u}, {21u, 0u}, {28u, 0u},
    };
    if (!draw->has_texture || ((draw->fvf >> 8u) & 15u) != 1u ||
        *recomp_memory_u32(device + 0x790u) != 1u) return false;
    const RecompD3dRenderStateModel *state = recomp_d3d8_state_model();
    uint32_t value;
    for (uint32_t i = 0u; i < sizeof simple / sizeof simple[0]; ++i) {
        if (!recomp_d3d_get_simple_render_state(state, simple[i][0], &value) || value != simple[i][1])
            return false;
    }
    for (uint32_t i = 0u; i < sizeof sampler / sizeof sampler[0]; ++i) {
        if (*recomp_memory_u32(STAGES + sampler[i][0] * 4u) != sampler[i][1]) return false;
    }
    if (!recomp_d3d_get_simple_render_state(state, 0x40a60u, &value)) return false;
    draw->bright_pass = true;
    draw->bright_pass_threshold = value;
    return true;
}

static void attach_lighting(uint32_t device, const RecompD3dVertexLayout *layout,
    RecompD3dPresenterDrawCommand *draw)
{
    if (layout->pretransformed || *recomp_memory_u32(0x3553f8u) == 0u) return;
    /* Missing FVF colors resolve to material even when COLORVERTEX is enabled. */
    /* shortcut: vertex-color sources and two-sided lighting need additional presenter inputs. */
    if (layout->normal_offset == RECOMP_D3D_FVF_ABSENT ||
        layout->diffuse_offset != RECOMP_D3D_FVF_ABSENT ||
        layout->specular_offset != RECOMP_D3D_FVF_ABSENT ||
        *recomp_memory_u32(0x355494u) != 0u)
        recomp_stop(2, "d3d8:lighting-layout-or-source");
    /* Inline Simple setters bypass the guest shadow; absent state retains the SDK smooth default. */
    uint32_t shade = 0x1d01u;
    (void)recomp_d3d_get_simple_render_state(recomp_d3d8_state_model(), 0x4037cu, &shade);
    if (shade != 0x1d01u)
        recomp_stop(2, "d3d8:lighting-shade-mode");
    RecompD3dDirectionalLighting result = {0};
    const uint32_t *stage = recomp_memory_u32(STAGES);
    for (uint32_t channel = 0u; draw->has_texture && channel < 2u; ++channel) {
        const uint32_t op = stage[12u + channel * 4u];
        const uint32_t arg1 = stage[14u + channel * 4u], arg2 = stage[15u + channel * 4u];
        const bool first = op == 2u || op == 4u, second = op == 3u || op == 4u;
        if ((!first && !second) || (first && arg1 != 0u && arg1 != 2u) ||
            (second && arg2 != 0u && arg2 != 2u)) recomp_stop(2, "d3d8:lighting-stage");
        result.stage[channel * 2u] = (first && arg1 == 2u) || (second && arg2 == 2u);
        result.stage[channel * 2u + 1u] = (first && arg1 == 0u) || (second && arg2 == 0u);
    }
    float material[17];
    recomp_guest_load(material, device + 0xf00u, sizeof material);
    for (uint32_t i = 0u; i < 17u; ++i)
        if (!isfinite(material[i])) recomp_stop(2, "d3d8:lighting-material");
    const uint32_t ambient = *recomp_memory_u32(0x35542cu);
    for (uint32_t c = 0u; c < 3u; ++c)
        result.ambient_emissive[c] = material[12u + c] + material[4u + c] *
            ((ambient >> (16u - 8u * c)) & 255u) / 255.0f;
    memcpy(result.material_diffuse, material, sizeof result.material_diffuse);
    const uint32_t array = *recomp_memory_u32(device + 0x7a4u);
    const uint32_t capacity = *recomp_memory_u32(device + 0x7a8u);
    if (capacity > RECOMP_XBOX_RAM_SIZE / sizeof(RecompD3dLightRecord) ||
        (capacity != 0u && array == 0u)) recomp_stop(2, "d3d8:lighting-array");
    if (capacity != 0u) recomp_memory(array, capacity * sizeof(RecompD3dLightRecord));
    for (uint32_t slot = 0u; slot < 8u; ++slot) {
        const uint32_t address = *recomp_memory_u32(device + 0x7acu + slot * 4u);
        if (address == 0u) continue;
        if (address < array || (address - array) / sizeof(RecompD3dLightRecord) >= capacity ||
            (address - array) % sizeof(RecompD3dLightRecord) != 0u)
            recomp_stop(2, "d3d8:lighting-slot");
        RecompD3dLightRecord record;
        recomp_guest_load(&record, address, sizeof record);
        const RecompD3dLightDesc *light = &record.desc;
        if (record.slot != slot || (light->type != 1u && light->type != 3u))
            recomp_stop(2, "d3d8:lighting-type-or-slot");
        const float *values = (const float *)(const void *)((const uint8_t *)light + 4u);
        for (uint32_t i = 0u; i < 25u; ++i)
            if (!isfinite(values[i])) recomp_stop(2, "d3d8:lighting-nonfinite");
        const uint32_t i = result.count++;
        for (uint32_t c = 0u; c < 3u; ++c) {
            result.colors[i][c] = light->diffuse[c];
            result.specular[i][c] = light->specular[c] * material[8u + c];
            result.ambient[i][c] = light->ambient[c] * material[4u + c];
            result.positions[i][c] = light->position[c];
            result.directions[i][c] = record.normalized_direction[c];
            result.attenuation[i][c] = light->attenuation[c];
            if (!isfinite(result.directions[i][c])) recomp_stop(2, "d3d8:lighting-direction");
        }
        result.positions[i][3] = light->type == 1u;
        result.attenuation[i][3] = light->range;
    }
    for (uint32_t i = 0u; i <= layout->blend_weight_count; ++i) {
        recomp_guest_load(result.world_transforms[i], device + 0xde0u + i * 64u, 64u);
        if (!recomp_d3d_normal_transform(result.world_transforms[i], result.normal_transforms[i]))
            recomp_stop(2, "d3d8:lighting-normal-transform");
    }
    if (*recomp_memory_u32(0x3553fcu) != 0u || (*recomp_memory_u32(device + 8u) & 0x40u) != 0u) {
        if (*recomp_memory_u32(0x355400u) != 0u) recomp_stop(2, "d3d8:lighting-local-viewer");
        float view[16];
        recomp_guest_load(view, device + 0xc60u, sizeof view);
        const float length = sqrtf(view[2] * view[2] + view[6] * view[6] + view[10] * view[10]);
        if (!isfinite(length) || length <= 0.0f) recomp_stop(2, "d3d8:lighting-view");
        for (uint32_t c = 0u; c < 3u; ++c) result.view[c] = -view[2u + 4u * c] / length;
        /* shortcut: zero power uses epsilon until the presenter has a separate specular-enable bit. */
        result.view[3] = material[16] > 0.0f ? material[16] : 1e-6f;
    }
    result.normalize = *recomp_memory_u32(0x355498u) != 0u;
    result.stage_mode = result.enabled = true;
    draw->directional = result;
}

static void attach_reflection(uint32_t device, const RecompD3dVertexLayout *layout,
    RecompD3dPresenterDrawCommand *draw)
{
    uint32_t stages[4][32];
    recomp_guest_load(stages, STAGES, sizeof stages);
    if (!recomp_d3d_reflection_material(stages)) return;
    /* shortcut: generated reflection coordinates need blended world/view inputs before admitting skinning. */
    if (layout->pretransformed || layout->blend_weight_count != 0u || layout->texcoord_count == 0u ||
        (stages[1][28] != 0u && layout->normal_offset == RECOMP_D3D_FVF_ABSENT) || !draw->has_texture ||
        draw->texture.linear || draw->texture.format_byte == RECOMP_D3D_TEXTURE_FORMAT_A8 ||
        draw->palette_bytes != NULL) recomp_stop(2, "d3d8:reflection-layout");
    /* The shared reflection path samples both textures with the stage-0 sampler. */
    if (stages[0][5] != stages[1][5] || stages[0][6] != stages[1][6] ||
        stages[0][7] != 0u || stages[1][7] != 0u)
        recomp_stop(2, "d3d8:reflection-sampler");
    RecompD3dPresenterDrawCommand texture = {0};
    attach_texture(device, 1u, &texture);
    if (!texture.has_texture || texture.texture.linear || texture.palette_bytes != NULL ||
        texture.texture.format_byte == RECOMP_D3D_TEXTURE_FORMAT_A8)
        recomp_stop(2, "d3d8:reflection-texture");
    draw->reflection_texture = texture.texture;
    draw->reflection_bytes = texture.texture_bytes;
    draw->reflection_byte_count = texture.texture_byte_count;
    float world[16], view[16];
    recomp_guest_load(world, device + 0xde0u, sizeof world);
    recomp_guest_load(view, device + 0xc60u, sizeof view);
    multiply(world, view, draw->reflection_world_view);
    if (!recomp_d3d_normal_transform(draw->reflection_world_view, draw->reflection_normal))
        recomp_stop(2, "d3d8:reflection-normal");
    recomp_guest_load(draw->reflection_transform, device + 0xd20u, sizeof draw->reflection_transform);
    for (uint32_t i = 0u; i < 16u; ++i)
        if (!isfinite(draw->reflection_transform[i])) recomp_stop(2, "d3d8:reflection-transform");
    recomp_guest_load(draw->reflection_diffuse, device + 0xf00u, sizeof draw->reflection_diffuse);
    for (uint32_t i = 0u; i < 4u; ++i)
        if (!isfinite(draw->reflection_diffuse[i])) recomp_stop(2, "d3d8:reflection-material");
    draw->reflection_mesh_uv = stages[1][28] == 0u;
    draw->reflection_premultiplied = stages[1][12] == 16u;
    draw->reflection_normalize = *recomp_memory_u32(0x355498u) != 0u;
    draw->has_reflection = true;
}

static void fixed_stages(const RecompD3dVertexLayout *layout, RecompD3dPresenterDrawCommand *draw)
{
    uint32_t stage[4][32];
    recomp_guest_load(stage, STAGES, sizeof stage);
    draw->use_texture_factor = recomp_d3d_texture_factor_selected(stage[0][12], stage[0][14],
        stage[0][16], stage[0][18]);
    if (!draw->use_texture_factor && draw->has_texture) {
        if (layout->texcoord_count == 0u || stage[0][0] < 1u || stage[0][0] > 3u ||
            stage[0][1] < 1u || stage[0][1] > 3u || stage[0][3] != 2u || stage[0][4] != 2u ||
            stage[0][9] != 0u || stage[0][10] != 0u || stage[0][11] != 0u ||
            (draw->texture.mip_levels > 1u && stage[0][5] != 1u && stage[0][5] != 2u))
            recomp_stop(2, "d3d8:draw-texture-sampler");
        float bias;
        memcpy(&bias, &stage[0][6], sizeof bias);
        if (!isfinite(bias) || bias < -16.0f || bias > 15.99f)
            recomp_stop(2, "d3d8:draw-texture-lod-bias");
        draw->point_mip_filter = stage[0][5] == 1u;
        draw->mip_lod_bias = bias;
        const bool color_modulate = stage[0][12] == 4u && stage[0][14] == 2u &&
            (stage[0][15] == 0u || stage[0][15] == 1u);
        const bool alpha_modulate = stage[0][16] == 4u && stage[0][18] == 2u &&
            (stage[0][19] == 0u || stage[0][19] == 1u);
        const bool texture_alpha = stage[0][16] == 2u && stage[0][18] == 2u;
        if (texture_alpha) draw->material_alpha_mode = RECOMP_D3D_MATERIAL_ALPHA_SELECT_TEXTURE;
        const bool white = layout->diffuse_offset == RECOMP_D3D_FVF_ABSENT;
        const bool vertex_alpha = !white &&
            ((stage[0][16] == 2u && stage[0][18] <= 1u) ||
             (stage[0][16] == 3u && stage[0][19] <= 1u));
        if (vertex_alpha) draw->material_alpha_mode = RECOMP_D3D_MATERIAL_ALPHA_SELECT_VERTEX_DIFFUSE;
        /* shortcut: stage-0 ALPHAOP DISABLE passes diffuse alpha, an unverified SDK rule;
           its observed ONE/ONE composite only adds into ignored back-buffer alpha. */
        const bool diffuse_alpha = stage[0][16] == 1u;
        if (diffuse_alpha) {
            draw->material_alpha_mode = white ? RECOMP_D3D_MATERIAL_ALPHA_SELECT_DIFFUSE
                : RECOMP_D3D_MATERIAL_ALPHA_SELECT_VERTEX_DIFFUSE;
            draw->material_alpha = 1.0f;
        }
        /* MODULATE2X(TEXTURE, TFACTOR) reads no diffuse, so admit it only for white vertices. */
        draw->texture_factor_modulate2x = white && stage[0][12] == 5u &&
            stage[0][14] == 2u && stage[0][15] == 3u;
        const bool white_alpha = white && layout->pretransformed &&
            recomp_d3d_texture_material_alpha_mode(stage[0][12], stage[0][14], stage[0][15],
                stage[0][16], stage[0][18], stage[0][19]) == RECOMP_D3D_MATERIAL_ALPHA_SELECT_DIFFUSE;
        if (white_alpha) {
            draw->material_alpha_mode = RECOMP_D3D_MATERIAL_ALPHA_SELECT_DIFFUSE;
            draw->material_alpha = 1.0f;
        }
        if (!draw->directional.enabled && !draw->has_reflection &&
            (!(color_modulate || draw->texture_factor_modulate2x ||
               (white && stage[0][12] == 2u && stage[0][14] == 2u)) ||
             !(alpha_modulate || white_alpha || texture_alpha || vertex_alpha || diffuse_alpha)))
            recomp_stop(2, "d3d8:draw-texture-combine");
        if (stage[0][20] != 0u || stage[0][21] != 0u || stage[0][28] != 0u)
            recomp_stop(2, "d3d8:draw-texture-coordinates");
        const uint32_t final[6] = {stage[1][12], stage[1][14], stage[1][15],
            stage[1][16], stage[1][18], stage[1][19]};
        draw->modulate_texture_factor = recomp_d3d_texture_factor_modulate_selected(final, stage[2][12]);
        if (stage[1][12] != 1u && !draw->modulate_texture_factor && !draw->has_reflection)
            recomp_stop(2, "d3d8:draw-texture-stages");
    }
}

static void submit_draw(uint32_t primitive, uint32_t count, uint32_t vertices,
    uint32_t stride, uint32_t index_address)
{
    const uint32_t device = recomp_d3d8_require_device();
    /* An even-count quad strip covers the triangles of a strip, in the same winding. */
    if (primitive == 9u) { primitive = 6u; count &= ~1u; }
    if (count == 0u) return;
    if (count > 65536u || vertices == 0u || stride == 0u)
        recomp_stop(2, "d3d8:draw-span");
    const uint32_t triangles = recomp_d3d_draw_triangle_count(primitive, count);
    if (primitive < 5u || primitive > 7u) recomp_stop(2, "d3d8:draw-primitive:%u", primitive);
    if (triangles == 0u) return;
    RecompD3dPresenterCommand command = {0};
    RecompD3dPresenterDrawCommand *draw = &command.data.draw;
    draw->fvf = *recomp_memory_u32(device + 0x798u);
    RecompD3dVertexLayout layout;
    if ((draw->fvf & 1u) != 0u || !recomp_d3d_fvf_layout(draw->fvf, &layout) ||
        layout.stride > stride || (draw->fvf & ~0xfdfu) != 0u)
        recomp_stop(2, "d3d8:draw-vertex-layout:%08x:%u", draw->fvf, stride);
    for (uint32_t i = 1u; i < 16u; ++i) {
        if (*recomp_memory_u32(STREAMS + i * 12u + 8u) != 0u)
            recomp_stop(2, "d3d8:draw-multiple-streams");
    }
    const uint16_t *indices;
    uint16_t *sequential = NULL;
    uint32_t largest = count - 1u;
    if (index_address != 0u) {
        indices = (const uint16_t *)(const void *)recomp_memory(index_address, count * 2u);
        largest = 0u;
        for (uint32_t i = 0u; i < count; ++i) if (indices[i] > largest) largest = indices[i];
    } else {
        sequential = malloc(count * sizeof *sequential);
        if (sequential == NULL) recomp_stop(2, "d3d8:draw-index-allocation");
        for (uint32_t i = 0u; i < count; ++i) sequential[i] = (uint16_t)i;
        indices = sequential;
    }
    draw->vertex_count = largest + 1u;
    const uint32_t bytes = recomp_d3d_draw_vertex_bytes(stride, largest);
    if (bytes == 0u) recomp_stop(2, "d3d8:draw-vertex-span");
    draw->vertex_bytes = recomp_memory(vertices, bytes);
    draw->index_bytes = indices;
    draw->index_count = count;
    draw->vertex_stride = stride;
    draw->primitive_type = primitive;
    draw->triangle_count = triangles;
    if (!layout.pretransformed) {
        float view[16], projection[16], world[16], vp[16];
        recomp_guest_load(view, device + 0xc60u, sizeof view);
        recomp_guest_load(projection, device + 0xca0u, sizeof projection);
        multiply(view, projection, vp);
        recomp_guest_load(world, device + 0xde0u, sizeof world);
        multiply(world, vp, draw->transform);
        draw->has_transform = true;
        draw->blend_weight_count = layout.blend_weight_count;
        if (layout.blend_weight_count != 0u && *recomp_memory_u32(0x355484u) != layout.blend_weight_count)
            recomp_stop(2, "d3d8:draw-vertex-blend");
        for (uint32_t i = 0u; i < layout.blend_weight_count; ++i) {
            recomp_guest_load(world, device + 0xe20u + i * 64u, sizeof world);
            multiply(world, vp, draw->blend_transforms[i]);
        }
    }
    RecompD3dViewport viewport;
    recomp_guest_load(&viewport, device + 0xee0u, sizeof viewport);
    const float rect[6] = {(float)viewport.x, (float)viewport.y, (float)viewport.width,
        (float)viewport.height, viewport.min_z, viewport.max_z};
    memcpy(draw->viewport, rect, sizeof rect);
    recomp_guest_load(draw->screen_space_offset, device + 0xef8u, sizeof draw->screen_space_offset);
    for (uint32_t i = 0u; i < 2u; ++i) draw->screen_space_offset[i] -= 0.53125f;
    const uint32_t scissors = *recomp_memory_u32(device + 0x1c00u);
    if (scissors > 1u || (scissors != 0u && *recomp_memory_u32(device + 0x1c04u) != 0u))
        recomp_stop(2, "d3d8:draw-scissors");
    draw->has_scissor = true;
    if (scissors != 0u) {
        recomp_guest_load(draw->scissor, device + 0x1b80u, sizeof draw->scissor);
    } else {
        /* SDK count zero clips to the viewport, including pretransformed draws. */
        const int32_t clip[4] = {(int32_t)viewport.x, (int32_t)viewport.y,
            (int32_t)(viewport.x + viewport.width), (int32_t)(viewport.y + viewport.height)};
        memcpy(draw->scissor, clip, sizeof clip);
    }
    const RecompD3dRenderStateModel *state = recomp_d3d8_state_model();
    if ((state->fill_mode != 0u && state->fill_mode != 0x1b02u) || state->edge_antialias != 0u ||
        state->z_enable == 2u)
        recomp_stop(2, "d3d8:draw-raster-state");
    recomp_d3d_depth_state(state, &draw->depth);
    recomp_d3d_blend_state(state, &draw->blend);
    draw->cull_mode = state->cull_mode == 0x900u ? RECOMP_D3D_CULL_CLOCKWISE :
        state->cull_mode == 0x901u ? RECOMP_D3D_CULL_COUNTER_CLOCKWISE : RECOMP_D3D_CULL_NONE;
    draw->texture_factor = state->texture_factor;
    recomp_d3d8_target(&draw->target);
    attach_texture(device, 0u, draw);
    attach_lighting(device, &layout, draw);
    if (*recomp_memory_u32(device + PIXEL_HANDLE) != 0u) {
        if (!four_tap(device, draw) && !bright_pass(device, draw))
            recomp_stop(2, "d3d8:draw-pixel-shader-unsupported");
    } else {
        attach_reflection(device, &layout, draw);
        fixed_stages(&layout, draw);
    }
    command.type = RECOMP_D3D_PRESENTER_COMMAND_DRAW;
    recomp_d3d8_submit(&command);
    free(sequential);
}

static void draw_up(void)
{
    submit_draw(kernel_arg(1u), kernel_arg(2u), kernel_arg(3u), kernel_arg(4u), 0u);
    recomp_runtime.registers.esp += 20u;
}

static void draw_vertices(void)
{
    uint32_t stride;
    const uint32_t vertices = stream_vertices(kernel_arg(2u), &stride);
    submit_draw(kernel_arg(1u), kernel_arg(3u), vertices, stride, 0u);
    recomp_runtime.registers.esp += 16u;
}

static void draw_indexed(void)
{
    uint32_t stride;
    const uint32_t device = recomp_d3d8_require_device();
    const uint32_t vertices = stream_vertices(*recomp_memory_u32(device + 0x1cu), &stride);
    uint32_t indices = kernel_arg(3u);
    if (indices == 0u) recomp_stop(2, "d3d8:draw-index-pointer");
    submit_draw(kernel_arg(1u), kernel_arg(2u), vertices, stride, indices);
    recomp_runtime.registers.esp += 16u;
}

RecompFunction recomp_d3d8_draw_lookup_manual(uint32_t address)
{
    /* Whole 5849 SDK APIs; the renderer consumes semantic commands. */
    switch (address) {
    case 0x348240u: return draw_up;
    case 0x3483b0u: return draw_vertices;
    case 0x348450u: return draw_indexed;
    case 0x349490u: return pixel_create;
    case 0x3494e0u: return pixel_delete;
    case 0x349500u: return pixel_get;
    case 0x349520u: return pixel_set;
    case 0x349720u: return pixel_constant;
    default: return NULL;
    }
}
