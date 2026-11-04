// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "d3d_draw_shader.h"
#include "d3d_shader_compiler.h"
#include <cassert>
#include <cstdio>
#include <cstring>

int main(int argc, char **argv) {
    const bool compile = argc == 2 && std::strcmp(argv[1], "--compile") == 0;
    RecompD3dPixelProgram p{};
    p.kind=RECOMP_D3D_PIXEL_FIXED_FUNCTION; p.stage_count=2;
    for (unsigned i=0;i<2;++i) {
        auto &s=p.stages[i]; s.sampled=true; s.transform_uv=true; s.texcoord=0;
        s.color_op=s.alpha_op=RECOMP_D3DTOP_SELECTARG1;
        s.color_arg[1]=s.alpha_arg[1]=RECOMP_D3DTA_TEXTURE;
        s.result_arg=RECOMP_D3DTA_CURRENT;
    }
    p.constants[RECOMP_D3D_PIXEL_CONSTANT_UV][0]=.25f;
    p.constants[RECOMP_D3D_PIXEL_CONSTANT_UV][2]=.75f;
    const auto hash=recomp_d3d_pixel_program_hash(&p);
    p.constants[RECOMP_D3D_PIXEL_CONSTANT_UV][2]=0;
    assert(hash==recomp_d3d_pixel_program_hash(&p)); // animation cannot grow the shader cache
    p.stages[0].transform_uv=false;
    assert(hash!=recomp_d3d_pixel_program_hash(&p));
    p.stages[0].transform_uv=true;
    RecompD3dDrawShaderKey key{}; key.fvf=0x152; key.pixel=&p;
    for (unsigned kind : {RECOMP_D3D_PIXEL_FIXED_FUNCTION, RECOMP_D3D_PIXEL_COMBINERS}) {
        p.kind=kind; p.combiners.control=1; p.combiners.texture_modes=1|(1<<5);
        std::string source,error; RecompD3dVertexLayout layout{};
        assert(recomp_d3d_draw_shader_source(key,layout,source));
        assert(source.find("dot(float4(coords[0].xy, 1, 0), pixel_constants[24])")!=std::string::npos);
        assert(source.find("dot(float4(coords[0].xy, 1, 0), pixel_constants[26])")!=std::string::npos);
        if (!compile) continue;
        source=recomp_shader_draw_bindings(source);
        for (bool vertex : {true,false}) {
            RecompCompiledShader compiled;
            if (!recomp_shader_compile(source,vertex?"vs_main":"ps_main",vertex,RecompShaderTarget::Spirv,{},compiled,error)) {
                std::fprintf(stderr,"%s\n",error.c_str()); return 1;
            }
            assert(!compiled.code.empty());
        }
    }
    std::puts(compile ? "PASS: UV transforms and vertex/fragment compilation" :
        "PASS: independent per-stage UV transforms and stable shader identity");
}
