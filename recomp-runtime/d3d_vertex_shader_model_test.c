/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "d3d_vertex_shader_model.h"

#include <stdio.h>
#include <string.h>

static int expect_u32(const char *field, uint32_t actual, uint32_t expected)
{
    if (actual == expected) return 1;
    fprintf(stderr, "vertex declaration: %s was 0x%08x, expected 0x%08x\n",
        field, actual, expected);
    return 0;
}

int main(void)
{
    uint32_t declaration[RECOMP_D3D_VERTEX_DECLARATION_WORDS];
    int passed = 1;

    memset(declaration, 0xa5, sizeof declaration);
    passed &= expect_u32("FVF 0x112 accepted",
        recomp_d3d_build_fixed_function_declaration(0x112u, declaration), 1u);
    passed &= expect_u32("unwritten word preserved", declaration[0], 0xa5a5a5a5u);
    passed &= expect_u32("flags", declaration[0x04u / 4u], 0u);
    passed &= expect_u32("texture mask", declaration[0x10u / 4u], 2u);
    passed &= expect_u32("position type", declaration[0x1cu / 4u], 0x32u);
    passed &= expect_u32("unused slot default", declaration[0x2cu / 4u], 2u);
    passed &= expect_u32("normal offset", declaration[0x38u / 4u], 0x0cu);
    passed &= expect_u32("normal type", declaration[0x3cu / 4u], 0x32u);
    passed &= expect_u32("uv offset", declaration[0xa8u / 4u], 0x18u);
    passed &= expect_u32("uv type", declaration[0xacu / 4u], 0x22u);

    memset(declaration, 0, sizeof declaration);
    passed &= expect_u32("creation default FVF 2 accepted",
        recomp_d3d_build_fixed_function_declaration(2u, declaration), 1u);
    passed &= expect_u32("FVF 2 flags", declaration[0x04u / 4u], 0u);
    passed &= expect_u32("FVF 2 mask", declaration[0x10u / 4u], 0u);
    passed &= expect_u32("FVF 2 position type", declaration[0x1cu / 4u], 0x32u);
    passed &= expect_u32("FVF 2 slot default", declaration[0x2cu / 4u], 2u);

    memset(declaration, 0, sizeof declaration);
    passed &= expect_u32("XYZRHW diffuse accepted",
        recomp_d3d_build_fixed_function_declaration(0x144u, declaration), 1u);
    passed &= expect_u32("XYZRHW diffuse flags", declaration[0x04u / 4u], 0x402u);
    passed &= expect_u32("XYZRHW position type", declaration[0x1cu / 4u], 0x42u);
    passed &= expect_u32("diffuse offset", declaration[0x48u / 4u], 0x10u);
    passed &= expect_u32("diffuse type", declaration[0x4cu / 4u], 0x40u);

    memset(declaration, 0, sizeof declaration);
    passed &= expect_u32("3-component UV accepted",
        recomp_d3d_build_fixed_function_declaration(0x00010102u, declaration), 1u);
    passed &= expect_u32("3-component UV type", declaration[0xacu / 4u], 0x32u);
    passed &= expect_u32("3-component UV mask", declaration[0x10u / 4u], 3u);

    passed &= expect_u32("eight texture sets accepted",
        recomp_d3d_build_fixed_function_declaration(0x802u, declaration), 1u);
    passed &= expect_u32("x86 texture-mask shift wraps",
        declaration[0x10u / 4u], 0x02020202u);
    passed &= expect_u32("nine texture sets rejected",
        recomp_d3d_build_fixed_function_declaration(0x902u, declaration), 0u);
    passed &= expect_u32("null declaration rejected",
        recomp_d3d_build_fixed_function_declaration(0x112u, NULL), 0u);
    if (passed) puts("PASS fixed-function vertex declaration builder");
    return passed ? 0 : 1;
}
