// SPDX-License-Identifier: GPL-3.0-or-later
#include "d3d_pixel_program.h"

#include <cstddef>
#include <cstdio>
#include <cstring>

namespace {

// Stage textures 0..2 share the draw shader's existing resources.
const char *const kTextures[] = {"guest_texture", "alpha_mask", "bump_map"};
const char *const kSamplers[] = {"guest_sampler", "mask_sampler", "bump_sampler"};

bool opSupported(uint32_t op)
{
    return op >= RECOMP_D3DTOP_DISABLE && op <= RECOMP_D3DTOP_LERP && op != RECOMP_D3DTOP_PREMODULATE;
}

bool argSupported(uint32_t arg)
{
    return (arg & ~(RECOMP_D3DTA_SELECTMASK | RECOMP_D3DTA_COMPLEMENT | RECOMP_D3DTA_ALPHAREPLICATE)) == 0u &&
        (arg & RECOMP_D3DTA_SELECTMASK) <= RECOMP_D3DTA_TEMP;
}

// Which arguments an op reads: bit 0 ARG0, bit 1 ARG1, bit 2 ARG2.
unsigned opArgs(uint32_t op)
{
    switch (op) {
    case RECOMP_D3DTOP_DISABLE: return 0u;
    case RECOMP_D3DTOP_SELECTARG1: return 2u;
    case RECOMP_D3DTOP_SELECTARG2: return 4u;
    case RECOMP_D3DTOP_MULTIPLYADD:
    case RECOMP_D3DTOP_LERP: return 7u;
    default: return 6u;
    }
}

bool readsTexture(const RecompD3dPixelStage &stage)
{
    const auto reads = [](uint32_t op, const uint32_t (&args)[3]) {
        const unsigned used = opArgs(op);
        for (unsigned i = 0u; i < 3u; ++i)
            if ((used & 1u << i) && (args[i] & RECOMP_D3DTA_SELECTMASK) == RECOMP_D3DTA_TEXTURE) return true;
        return op == RECOMP_D3DTOP_BLENDTEXTUREALPHA || op == RECOMP_D3DTOP_BLENDTEXTUREALPHAPM;
    };
    return reads(stage.color_op, stage.color_arg) || reads(stage.alpha_op, stage.alpha_arg);
}

std::string argument(uint32_t arg)
{
    static const char *const sources[] = {"diffuse", "current", "tex", "tfactor", "specular", "temp"};
    std::string value = sources[arg & RECOMP_D3DTA_SELECTMASK];
    if (arg & RECOMP_D3DTA_ALPHAREPLICATE) value += ".aaaa";
    if (arg & RECOMP_D3DTA_COMPLEMENT) value = "(1.0f - " + value + ")";
    return value;
}

/* One D3DTOP over the components named by `part` (".rgb" or ".a"). */
std::string operation(uint32_t op, const uint32_t (&args)[3], const char *part)
{
    const std::string a0 = "(" + argument(args[0]) + ")" + part;
    const std::string a1 = "(" + argument(args[1]) + ")" + part;
    const std::string a2 = "(" + argument(args[2]) + ")" + part;
    const std::string alpha_of_1 = "(" + argument(args[1]) + ").a";
    switch (op) {
    case RECOMP_D3DTOP_SELECTARG1: return a1;
    case RECOMP_D3DTOP_SELECTARG2: return a2;
    case RECOMP_D3DTOP_MODULATE: return a1 + " * " + a2;
    case RECOMP_D3DTOP_MODULATE2X: return "2.0f * " + a1 + " * " + a2;
    case RECOMP_D3DTOP_MODULATE4X: return "4.0f * " + a1 + " * " + a2;
    case RECOMP_D3DTOP_ADD: return a1 + " + " + a2;
    case RECOMP_D3DTOP_ADDSIGNED: return a1 + " + " + a2 + " - 0.5f";
    case RECOMP_D3DTOP_ADDSIGNED2X: return "2.0f * (" + a1 + " + " + a2 + " - 0.5f)";
    case RECOMP_D3DTOP_SUBTRACT: return a1 + " - " + a2;
    case RECOMP_D3DTOP_ADDSMOOTH: return a1 + " + " + a2 + " - " + a1 + " * " + a2;
    case RECOMP_D3DTOP_BLENDDIFFUSEALPHA: return "lerp(" + a2 + ", " + a1 + ", diffuse.a)";
    case RECOMP_D3DTOP_BLENDCURRENTALPHA: return "lerp(" + a2 + ", " + a1 + ", current.a)";
    case RECOMP_D3DTOP_BLENDTEXTUREALPHA: return "lerp(" + a2 + ", " + a1 + ", tex.a)";
    case RECOMP_D3DTOP_BLENDFACTORALPHA: return "lerp(" + a2 + ", " + a1 + ", tfactor.a)";
    case RECOMP_D3DTOP_BLENDTEXTUREALPHAPM: return a1 + " + " + a2 + " * (1.0f - tex.a)";
    case RECOMP_D3DTOP_MODULATEALPHA_ADDCOLOR: return a1 + " + " + alpha_of_1 + " * " + a2;
    case RECOMP_D3DTOP_MODULATECOLOR_ADDALPHA: return a1 + " * " + a2 + " + " + alpha_of_1;
    case RECOMP_D3DTOP_MODULATEINVALPHA_ADDCOLOR: return "(1.0f - " + alpha_of_1 + ") * " + a2 + " + " + a1;
    case RECOMP_D3DTOP_MODULATEINVCOLOR_ADDALPHA: return "(1.0f - " + a1 + ") * " + a2 + " + " + alpha_of_1;
    case RECOMP_D3DTOP_DOTPRODUCT3:
        return "4.0f * dot((" + argument(args[1]) + ").rgb - 0.5f, (" + argument(args[2]) + ").rgb - 0.5f)";
    case RECOMP_D3DTOP_MULTIPLYADD: return a0 + " + " + a1 + " * " + a2;
    case RECOMP_D3DTOP_LERP: return a0 + " * " + a1 + " + (1.0f - " + a0 + ") * " + a2;
    default: return "";
    }
}

} // namespace

extern "C" bool recomp_d3d_pixel_program_supported(const RecompD3dPixelProgram *program, const char **reason)
{
    const char *why = nullptr;
    if (program == nullptr || program->kind != RECOMP_D3D_PIXEL_FIXED_FUNCTION) why = "kind";
    else if (program->stage_count > 3u) why = "stage 3 needs a texture slot";
    for (uint32_t i = 0u; why == nullptr && i < program->stage_count; ++i) {
        const RecompD3dPixelStage &stage = program->stages[i];
        if (!opSupported(stage.color_op) || stage.color_op == RECOMP_D3DTOP_DISABLE) why = "color op";
        else if (!opSupported(stage.alpha_op)) why = "alpha op";
        else if (stage.result_arg != RECOMP_D3DTA_CURRENT && stage.result_arg != RECOMP_D3DTA_TEMP) why = "result";
        else if (stage.texcoord > 3u) why = "texcoord";
        for (unsigned a = 0u; why == nullptr && a < 3u; ++a)
            if (!argSupported(stage.color_arg[a]) || !argSupported(stage.alpha_arg[a])) why = "argument";
        if (why == nullptr && readsTexture(stage) && !stage.sampled) why = "texture argument without a texture";
    }
    if (reason != nullptr) *reason = why;
    return why == nullptr;
}

extern "C" uint64_t recomp_d3d_pixel_program_hash(const RecompD3dPixelProgram *program)
{
    // FNV-1a over everything before the constants.
    const auto *bytes = reinterpret_cast<const unsigned char *>(program);
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0u; i < offsetof(RecompD3dPixelProgram, constants); ++i) {
        hash = (hash ^ bytes[i]) * 1099511628211ull;
    }
    return hash;
}

bool recomp_d3d_pixel_program_source(const RecompD3dPixelProgram &program, std::string &source)
{
    if (!recomp_d3d_pixel_program_supported(&program, nullptr)) return false;
    char line[256];
    source =
        "    float4 diffuse = input.color, specular = float4(0, 0, 0, 0);\n"
        "    float4 tfactor = pixel_constants[4];\n"
        "    float4 current = diffuse, temp = float4(0, 0, 0, 0), tex = float4(1, 1, 1, 1);\n"
        "    float4 coords[4] = {input.t0, input.t1, input.t2, input.t3};\n";
    for (uint32_t i = 0u; i < program.stage_count; ++i) {
        const RecompD3dPixelStage &stage = program.stages[i];
        if (stage.sampled) {
            std::snprintf(line, sizeof line,
                "    tex = %s.Sample(%s, coords[%u].xy * pixel_constants[%u].xy);\n",
                kTextures[i], kSamplers[i], stage.texcoord, RECOMP_D3D_PIXEL_CONSTANT_STAGE_SCALE + i);
            source += line;
            /* NV2A A8 reads white RGB; DXGI A8 has alpha only. */
            std::snprintf(line, sizeof line, "    if (pixel_constants[%u].z > 0.5f) tex.rgb = 1.0f;\n",
                RECOMP_D3D_PIXEL_CONSTANT_STAGE_SCALE + i);
            source += line;
            if (stage.alpha_kill) source += "    if (tex.a == 0.0f) discard;\n";
        }
        const char *result = stage.result_arg == RECOMP_D3DTA_TEMP ? "temp" : "current";
        const std::string color = operation(stage.color_op, stage.color_arg, ".rgb");
        /* ALPHAOP DISABLE keeps CURRENT's alpha, which at stage 0 is diffuse. */
        const std::string alpha = stage.alpha_op == RECOMP_D3DTOP_DISABLE ? "current.a"
            : operation(stage.alpha_op, stage.alpha_arg, ".a");
        source += "    {\n        float3 rgb = " + color + ";\n";
        source += "        float a = " + alpha + ";\n";
        source += std::string("        ") + result + " = saturate(float4(rgb, a));\n    }\n";
    }
    source += "    float4 shaded = current;\n";
    return true;
}
