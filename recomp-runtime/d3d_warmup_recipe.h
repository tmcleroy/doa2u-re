// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "d3d_pixel_program.h"
#include <cstddef>
#include <cstdint>
struct RecompWarmShader {
    uint32_t fvf, count, samples;
    uint8_t coverage, scaled_bias, sample_bias, count_pass, reserved;
    uint32_t program[136][4];
    RecompD3dPixelProgram pixel;
};
static_assert(sizeof(RecompWarmShader)==3052 && offsetof(RecompWarmShader,pixel)==2196, "inventory schema changed");
