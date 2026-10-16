/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_D3D_VERTEX_SHADER_MODEL_H
#define DOA2U_RECOMP_D3D_VERTEX_SHADER_MODEL_H

#include <stdbool.h>
#include <stdint.h>

enum {
    RECOMP_D3D_VERTEX_DECLARATION_SIZE = 0x128u,
    RECOMP_D3D_VERTEX_DECLARATION_WORDS =
        RECOMP_D3D_VERTEX_DECLARATION_SIZE / 4u,
};

/* Mutates an existing declaration image exactly as the fixed-function
   declaration builder does. Unwritten words remain unchanged. */
bool recomp_d3d_build_fixed_function_declaration(
    uint32_t fvf,
    uint32_t declaration[RECOMP_D3D_VERTEX_DECLARATION_WORDS]);

/* Unpack bounded vertex-program upload blocks; reject other command types. */
bool recomp_d3d_unpack_vertex_program(const uint32_t *packed, uint32_t words,
    uint32_t instructions, uint32_t program[136][4]);

#endif
