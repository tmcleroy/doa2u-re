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


/* ---- Register combiners ---------------------------------------------------

   Per stage, each of the RGB and alpha halves reads four inputs A, B, C, D
   from its input word (A in bits 31-24 down to D in 7-0): a register (bits
   3-0), a component select (bit 4: RGB takes alpha replicated; alpha takes
   blue when clear) and a mapping (bits 7-5). Its output word names three
   destinations (CD bits 3-0, AB 7-4, sum 11-8), RGB dot products (bit 12 CD,
   bit 13 AB), a mux instead of the sum (bit 14), the scale/bias (17-15) and,
   for RGB, the blue-to-alpha copies of the dot products (18 CD, 19 AB). */
namespace {

const char *const kRegisters[16] = {
    "zero4", "c0", "c1", "fog", "v0", "v1", "zero4", "zero4",
    "t0", "t1", "t2", "t3", "r0", "r1", "v1r0", "ef",
};

std::string mapped(uint32_t input, bool alpha_half)
{
    const uint32_t reg = input & 15u, mapping = (input >> 5) & 7u;
    const bool alpha = (input & 0x10u) != 0u;
    std::string value = kRegisters[reg];
    value += alpha_half ? (alpha ? ".aaaa" : ".bbbb") : (alpha ? ".aaaa" : ".rgba");
    switch (mapping) {
    case 0: return "max(" + value + ", 0)";                       /* UNSIGNED_IDENTITY */
    case 1: return "(1 - saturate(" + value + "))";               /* UNSIGNED_INVERT */
    case 2: return "(2 * max(" + value + ", 0) - 1)";             /* EXPAND_NORMAL */
    case 3: return "(1 - 2 * max(" + value + ", 0))";             /* EXPAND_NEGATE */
    case 4: return "(max(" + value + ", 0) - 0.5)";               /* HALFBIAS_NORMAL */
    case 5: return "(0.5 - max(" + value + ", 0))";               /* HALFBIAS_NEGATE */
    case 6: return value;                                         /* SIGNED_IDENTITY */
    default: return "(-" + value + ")";                           /* SIGNED_NEGATE */
    }
}

bool writable(uint32_t reg)
{
    return reg == 0u || reg == 4u || reg == 5u || (reg >= 8u && reg <= 13u);
}

/* One half (RGB or alpha) of one general combiner stage. */
bool combinerHalf(std::string &out, uint32_t input, uint32_t output, bool alpha_half, uint32_t stage)
{
    const uint32_t cd_dest = output & 15u, ab_dest = (output >> 4) & 15u, sum_dest = (output >> 8) & 15u;
    if (!writable(cd_dest) || !writable(ab_dest) || !writable(sum_dest)) return false;
    const uint32_t op = (output >> 15) & 7u;
    if (op == 5u || op == 7u) return false;
    const char *swizzle = alpha_half ? ".a" : ".rgb";
    char line[512];
    std::snprintf(line, sizeof line, "    {\n        float4 a = %s, b = %s, c = %s, d = %s;\n",
        mapped(input >> 24, alpha_half).c_str(), mapped(input >> 16, alpha_half).c_str(),
        mapped(input >> 8, alpha_half).c_str(), mapped(input, alpha_half).c_str());
    out += line;
    const bool ab_dot = !alpha_half && (output & 0x2000u), cd_dot = !alpha_half && (output & 0x1000u);
    out += ab_dot ? "        float4 ab = dot(a.rgb, b.rgb).xxxx;\n" : "        float4 ab = a * b;\n";
    out += cd_dot ? "        float4 cd = dot(c.rgb, d.rgb).xxxx;\n" : "        float4 cd = c * d;\n";
    if (output & 0x4000u) {
        /* The mux reads R0's alpha: its MSB, or with MUX_LSB its least bit. */
        out += "        float4 sum = mux_msb ? (r0.a >= 0.5 ? cd : ab) : ((uint(round(saturate(r0.a) * 255)) & 1u) ? cd : ab);\n";
    } else {
        out += "        float4 sum = ab + cd;\n";
    }
    static const char *const scale[] = {"x", "(x - 0.5)", "(2 * x)", "(2 * (x - 0.5))", "(4 * x)", "", "(0.5 * x)", ""};
    const auto scaled = [&](const char *name) {
        std::string expr = scale[op];
        for (size_t at; (at = expr.find('x')) != std::string::npos;) expr.replace(at, 1u, name);
        return "clamp(" + expr + ", -1, 1)";
    };
    out += "        ab = " + scaled("ab") + "; cd = " + scaled("cd") + "; sum = " + scaled("sum") + ";\n";
    const auto write = [&](uint32_t dest, const char *value) {
        if (dest == 0u) return;
        out += std::string("        ") + kRegisters[dest] + swizzle + " = " + value + swizzle + ";\n";
    };
    write(cd_dest, "cd");
    write(ab_dest, "ab");
    write(sum_dest, "sum");
    /* Blue-to-alpha: an RGB dot product also lands in its destination's alpha. */
    if (!alpha_half && (output & 0x80000u) && ab_dot && ab_dest) out += std::string("        ") + kRegisters[ab_dest] + ".a = ab.b;\n";
    if (!alpha_half && (output & 0x40000u) && cd_dot && cd_dest) out += std::string("        ") + kRegisters[cd_dest] + ".a = cd.b;\n";
    out += "    }\n";
    (void)stage;
    return true;
}

std::string finalInput(uint32_t input, bool alpha_half)
{
    /* The final combiner maps with UNSIGNED_IDENTITY or UNSIGNED_INVERT. */
    const uint32_t reg = input & 15u;
    const bool alpha = (input & 0x10u) != 0u, invert = ((input >> 5) & 7u) == 1u;
    std::string value = std::string(kRegisters[reg]) + (alpha_half ? (alpha ? ".a" : ".b") : (alpha ? ".aaa" : ".rgb"));
    return invert ? "(1 - saturate(" + value + "))" : "saturate(" + value + ")";
}

bool combinersSupported(const RecompD3dCombiners &c, const char **why)
{
    const uint32_t count = c.control & 0xffu;
    if (count == 0u || count > 8u) { *why = "combiner count"; return false; }
    for (uint32_t i = 0u; i < 4u; ++i) {
        const uint32_t mode = (c.texture_modes >> (i * 5u)) & 31u;
        if (mode > 1u && mode != 4u) { *why = "texture mode"; return false; }   /* NONE, PROJECT2D, PASSTHRU */
    }
    if (c.compare_mode != 0u) { *why = "compare mode"; return false; }
    for (uint32_t i = 0u; i < count; ++i) {
        for (uint32_t half = 0u; half < 2u; ++half) {
            const uint32_t output = half ? c.alpha_output[i] : c.color_output[i];
            const uint32_t dests[3] = {output & 15u, (output >> 4) & 15u, (output >> 8) & 15u};
            for (uint32_t dest : dests) if (!writable(dest)) { *why = "combiner destination"; return false; }
            const uint32_t op = (output >> 15) & 7u;
            if (op == 5u || op == 7u) { *why = "combiner op"; return false; }
        }
    }
    return true;
}

bool combinerSource(const RecompD3dPixelProgram &program, std::string &out)
{
    const RecompD3dCombiners &c = program.combiners;
    const uint32_t count = c.control & 0xffu;
    char line[512];
    out =
        "    float4 zero4 = float4(0, 0, 0, 0), fog = float4(pixel_constants[23].rgb, fogFactor(input.fog_coord));\n"
        "    float4 v0 = input.color, v1 = input.specular;\n"
        "    float4 coords[4] = {input.t0, input.t1, input.t2, input.t3};\n"
        "    float4 t0 = zero4, t1 = zero4, t2 = zero4, t3 = zero4;\n";
    std::snprintf(line, sizeof line, "    const bool mux_msb = %s;\n", (c.control & 0x100u) ? "true" : "false");
    out += line;
    const char *const names[] = {"t0", "t1", "t2", "t3"};
    for (uint32_t i = 0u; i < 3u; ++i) {
        const uint32_t mode = (c.texture_modes >> (i * 5u)) & 31u;
        if (mode == 1u && program.stages[i].sampled) {
            std::snprintf(line, sizeof line,
                "    %s = %s.Sample(%s, coords[%u].xy * pixel_constants[%u].xy);\n"
                "    if (pixel_constants[%u].z > 0.5f) %s.rgb = 1.0f;\n"
                "    if (pixel_constants[%u].w > 0.5f) %s.a = 1.0f;\n",
                names[i], kTextures[i], kSamplers[i], program.stages[i].texcoord,
                RECOMP_D3D_PIXEL_CONSTANT_STAGE_SCALE + i, RECOMP_D3D_PIXEL_CONSTANT_STAGE_SCALE + i, names[i],
                RECOMP_D3D_PIXEL_CONSTANT_STAGE_SCALE + i, names[i]);
            out += line;
            if (program.stages[i].alpha_kill) out += std::string("    if (") + names[i] + ".a == 0.0f) discard;\n";
        } else if (mode == 4u) {
            std::snprintf(line, sizeof line, "    %s = saturate(coords[%u]);\n", names[i], program.stages[i].texcoord);
            out += line;
        }
    }
    if (((c.texture_modes >> 15u) & 31u) != 0u) return false;     /* stage 3 has no texture slot yet */
    /* NV2A starts R0's alpha with T0's. */
    out += "    float4 r0 = float4(0, 0, 0, t0.a), r1 = zero4;\n";
    for (uint32_t i = 0u; i < count; ++i) {
        const uint32_t constant = (c.control & 0x1000u) ? i : 0u, constant1 = (c.control & 0x10000u) ? i : 0u;
        std::snprintf(line, sizeof line, "    {\n    float4 c0 = pixel_constants[%u], c1 = pixel_constants[%u];\n",
            RECOMP_D3D_PIXEL_CONSTANT_FACTOR0 + constant, RECOMP_D3D_PIXEL_CONSTANT_FACTOR1 + constant1);
        out += line;
        /* Both halves read the stage's inputs before either writes. */
        out += "    float4 rgb_r0 = r0, rgb_r1 = r1, rgb_t0 = t0, rgb_t1 = t1, rgb_t2 = t2, rgb_t3 = t3, rgb_v0 = v0, rgb_v1 = v1;\n";
        std::string rgb, alpha;
        if (!combinerHalf(rgb, c.color_input[i], c.color_output[i], false, i) ||
            !combinerHalf(alpha, c.alpha_input[i], c.alpha_output[i], true, i)) return false;
        /* The alpha half reads the registers as they were before the RGB half wrote. */
        std::string read_before = alpha;
        const char *const regs[] = {"r0", "r1", "t0", "t1", "t2", "t3", "v0", "v1"};
        std::string guarded = "    {\n        float4 nr0 = r0, nr1 = r1, nt0 = t0, nt1 = t1, nt2 = t2, nt3 = t3, nv0 = v0, nv1 = v1;\n"
            "        r0 = rgb_r0; r1 = rgb_r1; t0 = rgb_t0; t1 = rgb_t1; t2 = rgb_t2; t3 = rgb_t3; v0 = rgb_v0; v1 = rgb_v1;\n";
        /* Evaluate alpha against the old values, then merge: RGB from the
           RGB half, alpha from the alpha half. */
        guarded += read_before;
        guarded += "        r0.rgb = nr0.rgb; r1.rgb = nr1.rgb; t0.rgb = nt0.rgb; t1.rgb = nt1.rgb;\n"
                   "        t2.rgb = nt2.rgb; t3.rgb = nt3.rgb; v0.rgb = nv0.rgb; v1.rgb = nv1.rgb;\n";
        /* An RGB blue-to-alpha write wins over an untouched alpha. */
        guarded += "    }\n";
        (void)regs;
        out += rgb + guarded + "    }\n";
    }
    /* Final combiner: rgb = A*B + (1-A)*C + D, alpha = G; E*F and V1+R0 are
       inputs too (registers 15 and 14). */
    std::snprintf(line, sizeof line, "    float4 c0 = pixel_constants[%u], c1 = pixel_constants[%u];\n",
        RECOMP_D3D_PIXEL_CONSTANT_FINAL0, RECOMP_D3D_PIXEL_CONSTANT_FINAL1);
    out += line;
    /* No final combiner in the definition: the SDK's default passes R0. */
    const bool default_final = c.final0 == 0u && c.final1 == 0u;
    const uint32_t final0 = default_final ? 0x0000000cu : c.final0;      /* D = R0 */
    const uint32_t final1 = default_final ? 0x00001c00u : c.final1;      /* G = R0.a */
    const uint32_t flags = final1 & 0xffu;
    out += std::string("    float4 v1r0 = float4(") +
        ((flags & 0x40u) ? "(1 - saturate(v1.rgb))" : "v1.rgb") + " + " +
        ((flags & 0x20u) ? "(1 - saturate(r0.rgb))" : "r0.rgb") + ", 0);\n";
    if (flags & 0x80u) out += "    v1r0.rgb = saturate(v1r0.rgb);\n";
    out += "    float4 ef = float4(" + finalInput(final1 >> 24, false) + " * " + finalInput(final1 >> 16, false) + ", 0);\n";
    out += "    float3 fa = " + finalInput(final0 >> 24, false) + ";\n";
    out += "    float3 rgb = fa * " + finalInput(final0 >> 16, false) + " + (1 - fa) * " +
        finalInput(final0 >> 8, false) + " + " + finalInput(final0, false) + ";\n";
    out += "    float4 shaded = float4(saturate(rgb), " + finalInput(final1 >> 8, true) + ");\n";
    return true;
}

} // namespace

extern "C" bool recomp_d3d_pixel_program_supported(const RecompD3dPixelProgram *program, const char **reason)
{
    const char *why = nullptr;
    if (program != nullptr && program->kind == RECOMP_D3D_PIXEL_COMBINERS) {
        const bool ok = combinersSupported(program->combiners, &why);
        if (reason != nullptr) *reason = ok ? nullptr : why;
        return ok;
    }
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
    if (program.kind == RECOMP_D3D_PIXEL_COMBINERS) return combinerSource(program, source);
    char line[256];
    source =
        "    float4 diffuse = input.color, specular = input.specular;\n"
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
            std::snprintf(line, sizeof line, "    if (pixel_constants[%u].z > 0.5f) tex.rgb = 1.0f;\n"
                "    if (pixel_constants[%u].w > 0.5f) tex.a = 1.0f;\n",
                RECOMP_D3D_PIXEL_CONSTANT_STAGE_SCALE + i, RECOMP_D3D_PIXEL_CONSTANT_STAGE_SCALE + i);
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
    /* With SPECULARENABLE the SDK's final combiner adds V1. */
    source += program.specular_add ? "    float4 shaded = float4(saturate(current.rgb + specular.rgb), current.a);\n"
                                   : "    float4 shaded = current;\n";
    /* With FOGENABLE the SDK's final combiner blends toward the fog color. */
    source += "    if (general_light[4].x > 0.5f)\n"
              "        shaded.rgb = lerp(pixel_constants[23].rgb, shaded.rgb, fogFactor(input.fog_coord));\n";
    return true;
}
