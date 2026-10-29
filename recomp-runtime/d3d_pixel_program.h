/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RECOMP_D3D_PIXEL_PROGRAM_H
#define RECOMP_D3D_PIXEL_PROGRAM_H

/* A general Xbox pixel pipeline, described by its guest state and turned
   into a pixel shader, instead of a recognised special case per effect.

   Fixed function: up to four texture stages in Xbox D3DTSS terms (D3DTOP,
   D3DTA with COMPLEMENT and ALPHAREPLICATE, RESULTARG CURRENT or TEMP), as
   D3D8 defines them. The Xbox SDK compiles these to register combiners; the
   shader evaluates the same operations directly.

   Register combiners: the NV2A pixel registers a D3DPIXELSHADERDEF holds
   (and the SDK writes as Kelvin methods): per stage the RGB and alpha input
   and output control words, the combiner count and flags, the final
   combiner's two words, and the texture stage modes.

   The structure (ops, arguments, which stages sample) selects a shader;
   the constants do not and travel in the draw's constant buffer. */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    RECOMP_D3D_PIXEL_NONE = 0u,
    RECOMP_D3D_PIXEL_FIXED_FUNCTION = 1u,
    RECOMP_D3D_PIXEL_COMBINERS = 2u,
    RECOMP_D3D_PIXEL_STAGES = 4u,
    /* Float4 constants the shader reads (pixel_constants in the draw buffer). */
    RECOMP_D3D_PIXEL_CONSTANTS = 24u,
    /* pixel_constants slots. */
    /* 4, one per stage: xy the texel scale of a linear texture (1 when
       swizzled), z 1 for an A8 texture (white RGB on NV2A), w 1 for an X8
       format (alpha reads one). */
    RECOMP_D3D_PIXEL_CONSTANT_STAGE_SCALE = 0u,
    RECOMP_D3D_PIXEL_CONSTANT_TFACTOR = 4u,      /* D3DRS_TEXTUREFACTOR as RGBA */
    RECOMP_D3D_PIXEL_CONSTANT_FACTOR0 = 5u,      /* 8: combiner C0 per stage, RGBA */
    RECOMP_D3D_PIXEL_CONSTANT_FACTOR1 = 13u,     /* 8: combiner C1 per stage */
    RECOMP_D3D_PIXEL_CONSTANT_FINAL0 = 21u,      /* final combiner C0, C1 */
    RECOMP_D3D_PIXEL_CONSTANT_FINAL1 = 22u,
    /* The combiners' fog register: fog color, and in alpha the fog factor
       (one with fog off, so the final combiner's FOG.a keeps R0). */
    RECOMP_D3D_PIXEL_CONSTANT_FOG = 23u,
};

/* D3DTOP (Xbox numbering, which follows D3D8). */
enum {
    RECOMP_D3DTOP_DISABLE = 1u, RECOMP_D3DTOP_SELECTARG1, RECOMP_D3DTOP_SELECTARG2,
    RECOMP_D3DTOP_MODULATE, RECOMP_D3DTOP_MODULATE2X, RECOMP_D3DTOP_MODULATE4X,
    RECOMP_D3DTOP_ADD, RECOMP_D3DTOP_ADDSIGNED, RECOMP_D3DTOP_ADDSIGNED2X,
    RECOMP_D3DTOP_SUBTRACT, RECOMP_D3DTOP_ADDSMOOTH, RECOMP_D3DTOP_BLENDDIFFUSEALPHA,
    RECOMP_D3DTOP_BLENDCURRENTALPHA, RECOMP_D3DTOP_BLENDTEXTUREALPHA,
    RECOMP_D3DTOP_BLENDFACTORALPHA, RECOMP_D3DTOP_BLENDTEXTUREALPHAPM,
    RECOMP_D3DTOP_PREMODULATE, RECOMP_D3DTOP_MODULATEALPHA_ADDCOLOR,
    RECOMP_D3DTOP_MODULATECOLOR_ADDALPHA, RECOMP_D3DTOP_MODULATEINVALPHA_ADDCOLOR,
    RECOMP_D3DTOP_MODULATEINVCOLOR_ADDALPHA, RECOMP_D3DTOP_DOTPRODUCT3,
    RECOMP_D3DTOP_MULTIPLYADD, RECOMP_D3DTOP_LERP,
};

/* D3DTA. */
enum {
    RECOMP_D3DTA_DIFFUSE = 0u, RECOMP_D3DTA_CURRENT = 1u, RECOMP_D3DTA_TEXTURE = 2u,
    RECOMP_D3DTA_TFACTOR = 3u, RECOMP_D3DTA_SPECULAR = 4u, RECOMP_D3DTA_TEMP = 5u,
    RECOMP_D3DTA_SELECTMASK = 0x0fu, RECOMP_D3DTA_COMPLEMENT = 0x10u,
    RECOMP_D3DTA_ALPHAREPLICATE = 0x20u,
};

typedef struct RecompD3dPixelStage {
    uint32_t color_op, color_arg[3];   /* arg[0] is ARG0, used by MULTIPLYADD and LERP */
    uint32_t alpha_op, alpha_arg[3];
    uint32_t result_arg;               /* CURRENT or TEMP */
    uint32_t texcoord;                 /* vertex texture coordinate set this stage reads */
    bool sampled;                      /* a texture is bound and some argument reads it */
    bool alpha_kill;                   /* discard where the texel's alpha is zero */
} RecompD3dPixelStage;

/* NV2A register combiner state, as the Kelvin methods carry it. */
typedef struct RecompD3dCombiners {
    uint32_t control;                  /* 0x1e60: count, mux select, unique C0/C1 */
    uint32_t color_input[8], color_output[8];
    uint32_t alpha_input[8], alpha_output[8];
    uint32_t final0, final1;           /* 0x288, 0x28c */
    uint32_t texture_modes;            /* 5 bits per stage */
    uint32_t input_texture, dot_mapping, compare_mode;
} RecompD3dCombiners;

typedef struct RecompD3dPixelProgram {
    uint32_t kind;
    uint32_t stage_count;              /* stages before the first COLOROP DISABLE */
    RecompD3dPixelStage stages[RECOMP_D3D_PIXEL_STAGES];
    RecompD3dCombiners combiners;      /* kind COMBINERS; stages[].sampled and
                                          .texcoord say which textures it reads */
    /* Not part of the shader's identity: */
    float constants[RECOMP_D3D_PIXEL_CONSTANTS][4];
} RecompD3dPixelProgram;

/* Checks a program the generator can express; false names the first
   unsupported part in `reason` (static text). */
bool recomp_d3d_pixel_program_supported(const RecompD3dPixelProgram *program, const char **reason);

/* A hash of the program's structure, for shader caches; constants excluded. */
uint64_t recomp_d3d_pixel_program_hash(const RecompD3dPixelProgram *program);

#ifdef __cplusplus
}

#include <string>

/* HLSL statements that compute `float4 shaded` from `input.color` (diffuse;
   specular reads zero for now), `input.t0` to `input.t3` (float4 texture coordinates),
   the stage textures (guest_texture, alpha_mask and bump_map with their
   samplers) and pixel_constants. */
bool recomp_d3d_pixel_program_source(const RecompD3dPixelProgram &program, std::string &source);
#endif

#endif
