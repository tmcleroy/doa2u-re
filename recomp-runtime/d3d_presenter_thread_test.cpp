#include "d3d_presenter.h"
#include "d3d_presenter_d3d11_backend.h"
#include "d3d_draw_model.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

// d3d_vblank.cpp's guest pacing reaches the kernel; these tests use only its host sleep.
extern "C" void recomp_kernel_wait_for_vblank(uint64_t) {}

static const RecompD3dPresenterConfig config = {
    320u, 240u, RECOMP_D3D_PRESENTER_COLOR_FORMAT_BGRA8_UNORM,
    RECOMP_D3D_PRESENTER_DEPTH_FORMAT_D24S8};

static bool expect(const char *label, RecompD3dPresenterError actual,
    RecompD3dPresenterError expected = RECOMP_D3D_PRESENTER_OK)
{
    if (actual == expected) return true;
    std::fprintf(stderr, "FAIL %s actual=%u expected=%u\n", label,
        static_cast<unsigned>(actual), static_cast<unsigned>(expected));
    return false;
}

static RecompD3dPresenterCommand clearCommand()
{
    RecompD3dPresenterCommand command{};
    command.type = RECOMP_D3D_PRESENTER_COMMAND_CLEAR;
    command.data.clear = {true, true, false, 0xff204060u, 1.0f, 0u};
    return command;
}

static RecompD3dPresenterError tick(RecompD3dPresenter *presenter, uint32_t swap)
{
    auto command = clearCommand();
    const auto error = recomp_d3d_presenter_submit(presenter, &command);
    if (error != RECOMP_D3D_PRESENTER_OK) return error;
    command = {};
    command.type = RECOMP_D3D_PRESENTER_COMMAND_PRESENT;
    command.data.present = {5u, swap};
    return recomp_d3d_presenter_submit(presenter, &command);
}

static bool destroy(RecompD3dPresenter *&presenter)
{
    const bool passed = expect("destroy", recomp_d3d_presenter_destroy(&presenter));
    if (presenter != nullptr) std::fprintf(stderr, "FAIL destroy retained handle\n");
    return passed && presenter == nullptr;
}

static HWND presenterWindow()
{
    HWND window = nullptr;
    while ((window = FindWindowExW(nullptr, window,
            L"DOA2URecompPresenterWindow", nullptr)) != nullptr) {
        DWORD process = 0;
        GetWindowThreadProcessId(window, &process);
        if (process == GetCurrentProcessId()) return window;
    }
    return nullptr;
}

static bool testTicks()
{
    RecompD3dPresenter *presenter = nullptr;
    bool passed = expect("null config", recomp_d3d_presenter_create(nullptr, &presenter),
        RECOMP_D3D_PRESENTER_INVALID_ARGUMENT);
    if (!expect("create ticks", recomp_d3d_presenter_create(&config, &presenter))) return false;
    RecompD3dPresenter *second = nullptr;
    passed &= expect("singleton", recomp_d3d_presenter_create(&config, &second),
        RECOMP_D3D_PRESENTER_ALREADY_INITIALIZED);
    std::thread other([&] {
        passed &= expect("owner thread", tick(presenter, 1), RECOMP_D3D_PRESENTER_WRONG_THREAD);
        passed &= expect("flush owner", recomp_d3d_presenter_flush(presenter, true),
            RECOMP_D3D_PRESENTER_WRONG_THREAD);
    });
    other.join();
    for (uint32_t i = 1; i <= 5 && passed; ++i) passed &= expect("tick", tick(presenter, i));
    auto clear = clearCommand();
    passed &= expect("partial clear", recomp_d3d_presenter_submit(presenter, &clear));
    passed &= expect("partial release", recomp_d3d_presenter_release_memory(
        presenter, 0x00200000u, 4096u));
    passed &= expect("publish without present", recomp_d3d_presenter_flush(presenter, false));
    passed &= expect("wait idle", recomp_d3d_presenter_flush(presenter, true));
    passed &= expect("empty wait idle", recomp_d3d_presenter_flush(presenter, true));
    recomp_d3d_presenter_report_draw_textures();
    passed &= destroy(presenter);
    if (passed) std::puts("PASS five CLEAR/PRESENT ticks and partial shutdown");
    return passed;
}

static bool testCapturedDraw()
{
    RecompD3dPresenter *presenter = nullptr;
    if (!expect("create draw", recomp_d3d_presenter_create(&config, &presenter))) return false;
    struct Vertex { float x, y, z, rhw; uint32_t diffuse; float u, v; };
    Vertex vertices[] = {
        {20, 20, 0.25f, 1, 0xffff8040u, 0, 0},
        {200, 20, 0.25f, 1, 0xffff8040u, 1, 0},
        {20, 200, 0.25f, 1, 0xffff8040u, 0, 1},
        {200, 200, 0.25f, 1, 0xffff8040u, 1, 1},
    };
    uint16_t indices[] = {0, 1, 2, 3};
    auto clear = clearCommand();
    bool passed = expect("draw clear", recomp_d3d_presenter_submit(presenter, &clear));
    RecompD3dPresenterCommand command{};
    command.type = RECOMP_D3D_PRESENTER_COMMAND_DRAW;
    auto &draw = command.data.draw;
    draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
    draw.index_count = draw.vertex_count = 4;
    draw.triangle_count = 2;
    draw.vertex_stride = sizeof(Vertex);
    draw.fvf = 0x144u;
    draw.vertex_bytes = vertices;
    draw.index_bytes = indices;
    draw.blend.color_write_mask = 15;
    passed &= expect("capture draw", recomp_d3d_presenter_submit(presenter, &command));
    // No PRESENT has published this packet, so these writes precede execution.
    std::memset(vertices, 0xff, sizeof vertices);
    std::memset(indices, 0xff, sizeof indices);
    command = {};
    command.type = RECOMP_D3D_PRESENTER_COMMAND_PRESENT;
    command.data.present = {5, 1};
    passed &= expect("draw present", recomp_d3d_presenter_submit(presenter, &command));
    recomp_d3d_presenter_report_draw_textures();
    passed &= destroy(presenter);
    if (passed) std::puts("PASS captured draw survives source overwrite");
    return passed;
}

static bool testMipSampler()
{
    char folder[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, folder) || !GetTempFileNameA(folder, "mip", 0, path)) return false;
    _putenv_s("RECOMP_D3D_FRAME_DUMP", path);
    RecompD3dPresenter *presenter = nullptr;
    bool passed = expect("create mip sampler", d3d11_backend_create(&config, &presenter));
    if (passed) {
        struct Vertex { float x, y, z, rhw; uint32_t diffuse; float u, v; };
        Vertex vertices[] = {
            {20, 20, 0.25f, 1, 0xffffffffu, 0, 0},
            {84, 20, 0.25f, 1, 0xffffffffu, 16, 0},
            {20, 84, 0.25f, 1, 0xffffffffu, 0, 16},
            {84, 84, 0.25f, 1, 0xffffffffu, 16, 16},
        };
        uint16_t indices[] = {0, 1, 2, 3};
        // One solid BC1 block per mip: red 4x4, green 2x2, blue 1x1.
        const uint32_t mips[] = {0xf800f800u, 0u, 0x07e007e0u, 0u, 0x001f001fu, 0u};
        auto clear = clearCommand();
        clear.data.clear.color = 0xff000000u;
        passed &= expect("mip clear", d3d11_backend_submit(presenter, &clear));
        RecompD3dPresenterCommand command{};
        command.type = RECOMP_D3D_PRESENTER_COMMAND_DRAW;
        auto &draw = command.data.draw;
        draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
        draw.index_count = draw.vertex_count = 4;
        draw.triangle_count = 2;
        draw.vertex_stride = sizeof(Vertex);
        draw.fvf = 0x144u;
        draw.vertex_bytes = vertices;
        draw.index_bytes = indices;
        draw.blend.color_write_mask = 15;
        draw.has_texture = true;
        draw.texture.format_byte = RECOMP_D3D_TEXTURE_FORMAT_DXT1;
        draw.texture.bits_per_pixel = 4;
        draw.texture.width = draw.texture.height = 4;
        draw.texture.mip_levels = 3;
        draw.texture.data = 0x1000u;
        draw.texture_bytes = mips;
        draw.texture_byte_count = sizeof mips;
        // Texture alpha must survive a zero material alpha and pass the test.
        draw.material_alpha_mode = RECOMP_D3D_MATERIAL_ALPHA_SELECT_TEXTURE;
        draw.depth.alpha_test_enable = true;
        draw.depth.alpha_func = RECOMP_D3D_COMPARE_GREATER;
        draw.depth.alpha_ref = 128;
        // Exceed the cache once, then revisit the four samplers below.
        for (unsigned i = 0; i < 65 && passed; ++i) {
            draw.mip_lod_bias = -8.0f + i * 0.25f;
            passed &= expect("mip cache eviction", d3d11_backend_submit(presenter, &command));
        }
        const float biases[] = {0.0f, 1.25f, 1.25f, -2.0f};
        for (unsigned i = 0; i < 4 && passed; ++i) {
            draw.point_mip_filter = i == 1 || i == 3;
            draw.mip_lod_bias = biases[i];
            passed &= expect("mip draw", d3d11_backend_submit(presenter, &command));
            for (auto &vertex : vertices) vertex.x += 64.0f;
        }
        for (auto &vertex : vertices) {
            vertex.x -= 256.0f;
            vertex.y += 64.0f;
            vertex.diffuse = 0x00ffffffu;
        }
        draw.material_alpha_mode = RECOMP_D3D_MATERIAL_ALPHA_SELECT_VERTEX_DIFFUSE;
        draw.material_alpha = 1.0f;
        draw.depth.alpha_func = RECOMP_D3D_COMPARE_GREATER_EQUAL;
        draw.mip_lod_bias = 0.0f;
        passed &= expect("vertex alpha discard", d3d11_backend_submit(presenter, &command));
        for (float bias : {-16.01f, 16.0f, INFINITY, NAN}) {
            draw.mip_lod_bias = bias;
            passed &= expect("invalid mip bias", d3d11_backend_submit(presenter, &command),
                RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND);
        }
        command = {};
        command.type = RECOMP_D3D_PRESENTER_COMMAND_PRESENT;
        command.data.present = {5, 1};
        passed &= expect("mip present", d3d11_backend_submit(presenter, &command));
        passed &= expect("destroy mip sampler", d3d11_backend_destroy(&presenter));
        if (FILE *file = std::fopen(path, "rb")) {
            for (unsigned i = 0; i < 5; ++i) {
                unsigned char bgr[3]{};
                const unsigned x = i == 4 ? 52 : 52 + 64 * i;
                const unsigned y = i == 4 ? 116 : 52;
                std::fseek(file, 54 + (240 - 1 - y) * 320 * 3 + x * 3, SEEK_SET);
                passed &= std::fread(bgr, 1, 3, file) == 3;
                const bool red = bgr[2] > 245 && bgr[1] < 10 && bgr[0] < 10;
                const bool green = bgr[1] > 245 && bgr[0] < 10 && bgr[2] < 10;
                const bool blended = bgr[1] > 170 && bgr[1] < 210 &&
                    bgr[0] > 45 && bgr[0] < 85 && bgr[2] < 10;
                const bool black = bgr[0] == 0 && bgr[1] == 0 && bgr[2] == 0;
                const bool pixel_ok = i == 4 ? black : i == 1 ? green : i == 2 ? blended : red;
                if (!pixel_ok) std::fprintf(stderr, "FAIL mip pixel %u BGR=%u,%u,%u\n",
                    i, bgr[0], bgr[1], bgr[2]);
                passed &= pixel_ok;
            }
            std::fclose(file);
        } else passed = false;
    }
    _putenv_s("RECOMP_D3D_FRAME_DUMP", "");
    DeleteFileA(path);
    if (passed) std::puts("PASS mip selection, interpolation, bias, sampler cache eviction, texture alpha and vertex alpha");
    return passed;
}

static bool testLines()
{
    char folder[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, folder) || !GetTempFileNameA(folder, "lin", 0, path)) return false;
    _putenv_s("RECOMP_D3D_FRAME_DUMP", path);
    RecompD3dPresenter *presenter = nullptr;
    bool passed = expect("create lines", d3d11_backend_create(&config, &presenter));
    if (passed) {
        auto clear = clearCommand();
        clear.data.clear.color = 0xff000000u;
        passed &= expect("lines clear", d3d11_backend_submit(presenter, &clear));
        struct Vertex { float x, y, z; uint32_t color; };
        Vertex vertices[2] = {{20,20.25f,0.5f,~0u}, {100,20.25f,0.5f,~0u}};
        const uint16_t indices[2] = {0,1};
        RecompD3dPresenterCommand command{RECOMP_D3D_PRESENTER_COMMAND_DRAW};
        auto &draw = command.data.draw;
        draw.primitive_type = 2;
        draw.index_count = draw.vertex_count = 2;
        draw.vertex_stride = sizeof(Vertex);
        draw.fvf = 0x42;
        draw.vertex_bytes = vertices;
        draw.index_bytes = indices;
        draw.has_transform = true;
        draw.transform[0] = 2.0f / 320;
        draw.transform[5] = -2.0f / 240;
        draw.transform[10] = draw.transform[15] = 1;
        draw.transform[12] = -1;
        draw.transform[13] = 1;
        draw.blend.color_write_mask = 15;
        draw.blend.blend_enable = true;
        draw.blend.src_factor = RECOMP_D3D_BLEND_SRC_ALPHA;
        draw.blend.dst_factor = RECOMP_D3D_BLEND_INV_SRC_ALPHA;
        draw.line_antialias = true;
        draw.line_width = 1.25f;
        draw.cull_mode = RECOMP_D3D_CULL_CLOCKWISE;
        passed &= expect("fractional line", d3d11_backend_submit(presenter, &command));
        for (auto &v : vertices) v.y = 40.5f;
        std::swap(vertices[0], vertices[1]);
        draw.line_width = 0.25f;
        passed &= expect("subpixel line", d3d11_backend_submit(presenter, &command));
        std::swap(vertices[0], vertices[1]);
        // Near-plane clipping keeps the second half; the fully hidden line emits nothing.
        for (auto &v : vertices) v.y = 60.5f;
        vertices[0].z = -0.5f;
        draw.line_width = 1;
        passed &= expect("near crossing line", d3d11_backend_submit(presenter, &command));
        for (auto &v : vertices) { v.y = 80.5f; v.z = -1; }
        passed &= expect("hidden line", d3d11_backend_submit(presenter, &command));
        // A later aliased line must use the ordinary pipeline again.
        for (auto &v : vertices) { v.y = 100.5f; v.z = 0.5f; }
        draw.line_antialias = false;
        passed &= expect("ordinary line", d3d11_backend_submit(presenter, &command));
        RecompD3dPresenterCommand present{RECOMP_D3D_PRESENTER_COMMAND_PRESENT};
        present.data.present = {5,1};
        passed &= expect("lines present", d3d11_backend_submit(presenter, &present));
        passed &= expect("destroy lines", d3d11_backend_destroy(&presenter));
        if (FILE *file = std::fopen(path, "rb")) {
            const unsigned pixels[][3] = {{50,20,223}, {50,19,96}, {50,18,0}, {50,40,64},
                {40,60,0}, {80,60,255}, {50,80,0}, {50,100,255}};
            for (const auto &pixel : pixels) {
                unsigned char bgr[3]{};
                std::fseek(file, 54 + (239-pixel[1])*960 + pixel[0]*3, SEEK_SET);
                passed &= std::fread(bgr, 1, 3, file) == 3;
                for (unsigned c=0; c<3; ++c) {
                    const bool ok = std::abs(int(bgr[c])-int(pixel[2])) <= 2;
                    if (!ok) std::fprintf(stderr, "FAIL line pixel %u,%u channel=%u actual=%u expected=%u\n",
                        pixel[0], pixel[1], c, bgr[c], pixel[2]);
                    passed &= ok;
                }
            }
            std::fclose(file);
        } else passed = false;
    }
    _putenv_s("RECOMP_D3D_FRAME_DUMP", "");
    DeleteFileA(path);
    if (passed) std::puts("PASS transformed line widths, fractional alpha coverage, clipping and pipeline restore");
    return passed;
}

static bool testLighting()
{
    char folder[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, folder) || !GetTempFileNameA(folder, "lit", 0, path)) return false;
    _putenv_s("RECOMP_D3D_FRAME_DUMP", path);
    RecompD3dPresenter *presenter = nullptr;
    bool passed = expect("create lighting", d3d11_backend_create(&config, &presenter));
    if (passed) {
        auto clear = clearCommand();
        clear.data.clear.color = 0xff000000u;
        passed &= expect("lighting clear", d3d11_backend_submit(presenter, &clear));
        for (unsigned i = 0; i < 16 && passed; ++i) {
            struct Vertex { float x, y, z, nx, ny, nz, u, v; };
            Vertex vertices[] = {
                {-0.1f, 0.1f, 0.5f, 0, 0, 1, 0, 0}, {0.1f, 0.1f, 0.5f, 0, 0, 1, 1, 0},
                {-0.1f, -0.1f, 0.5f, 0, 0, 1, 0, 1}, {0.1f, -0.1f, 0.5f, 0, 0, 1, 1, 1}};
            const uint16_t indices[] = {0, 1, 2, 3};
            RecompD3dPresenterCommand command{RECOMP_D3D_PRESENTER_COMMAND_DRAW};
            auto &draw = command.data.draw;
            draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
            draw.index_count = draw.vertex_count = 4;
            draw.triangle_count = 2;
            draw.vertex_stride = sizeof(Vertex);
            draw.fvf = 0x112u;
            draw.vertex_bytes = vertices;
            draw.index_bytes = indices;
            draw.blend.color_write_mask = 15;
            draw.has_transform = true;
            draw.transform[0] = draw.transform[5] = 1.5f;
            draw.transform[10] = draw.transform[15] = 1;
            draw.transform[12] = -0.75f + 0.5f * (i % 4);
            draw.transform[13] = i < 4 ? 0.5f : -0.5f;
            draw.depth.alpha_test_enable = true;
            draw.depth.alpha_func = RECOMP_D3D_COMPARE_GREATER;
            draw.depth.alpha_ref = 128;
            auto &light = draw.directional;
            light.enabled = i != 5;
            light.stage_mode = light.normalize = true;
            light.count = 1;
            for (unsigned j = 0; j < 16; j += 5)
                light.normal_transforms[0][j] = light.world_transforms[0][j] = 1;
            const float base[] = {0.1f, 0.05f, 0.025f, 0};
            const float material[] = {0.5f, 0.25f, 0.75f, 0.75f};
            const float color[] = {0.6f, 0.8f, 0.4f, 0};
            const float ambient[] = {0.025f, 0.05f, 0.075f, 0};
            std::memcpy(light.ambient_emissive, base, sizeof base);
            std::memcpy(light.material_diffuse, material, sizeof material);
            std::memcpy(light.colors[0], color, sizeof color);
            std::memcpy(light.ambient[0], ambient, sizeof ambient);
            light.directions[0][2] = 1;
            if (i == 1) for (auto &vertex : vertices) vertex.nz = -1;
            if (i == 2 || i == 3) {
                light.world_transforms[0][14] = 1;
                light.positions[0][2] = 3.5f;
                light.positions[0][3] = 1;
                light.attenuation[0][0] = 1;
                light.attenuation[0][1] = 0.5f;
                light.attenuation[0][3] = i == 2 ? 3.0f : 1.0f;
            }
            if (i == 4 || i == 8 || i == 11) {
                if (i == 4) {
                    std::memset(light.colors, 0, sizeof light.colors);
                    std::memset(light.ambient, 0, sizeof light.ambient);
                }
                for (unsigned j = 0; j < 3; ++j)
                    light.specular[0][j] = i == 4 ? 0.2f + j * 0.1f : 0.05f + j * 0.05f;
                light.view[2] = 1;
                light.view[3] = 8;
            }
            if (i == 6) light.material_diffuse[3] = 0.25f;
            if (i == 7) {
                for (auto &vertex : vertices) { vertex.nx = 2; vertex.nz = 0; }
                light.normal_transforms[0][0] = light.normal_transforms[0][10] = 0;
                light.normal_transforms[0][2] = 1;
            }
            const bool vertex_diffuse = i >= 9 && i < 14 && i != 11;
            const uint32_t base_pixel = i >= 14 ? 0x80c08040u : vertex_diffuse ? 0xc080e0ffu : 0x800000ffu;
            const uint32_t environment_pixel = i == 12 ? 0u : i == 13 ? 0xff804020u
                : vertex_diffuse ? 0x80ff8000u : 0x40ff0000u;
            if (i >= 8 && i < 14) {
                draw.transform[0] = 0.75f;
                draw.transform[12] = -5.0f / 6.0f + (i - 8) / 3.0f;
                draw.transform[13] = 0;
                draw.depth.alpha_test_enable = false;
                draw.blend.blend_enable = true;
                draw.blend.src_factor = RECOMP_D3D_BLEND_SRC_ALPHA;
                draw.blend.dst_factor = RECOMP_D3D_BLEND_INV_SRC_ALPHA;
                draw.has_texture = draw.has_reflection = draw.reflection_mesh_uv = true;
                draw.reflection_premultiplied = i >= 10;
                draw.texture.data = 0x1000u;
                draw.texture.format_byte = RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8;
                draw.texture.bits_per_pixel = 32;
                draw.texture.width = draw.texture.height = 1;
                draw.texture_bytes = &base_pixel;
                draw.texture_byte_count = sizeof base_pixel;
                draw.reflection_texture = draw.texture;
                draw.reflection_texture.data = 0x2000u;
                draw.reflection_bytes = &environment_pixel;
                draw.reflection_byte_count = sizeof environment_pixel;
                std::memcpy(draw.reflection_diffuse, material, sizeof material);
            }
            if (i >= 14) {
                draw.transform[12] = i == 14 ? -0.5f : 0.5f;
                draw.transform[13] = 0.85f;
                draw.depth.alpha_test_enable = false;
                draw.blend.blend_enable = true;
                draw.blend.src_factor = RECOMP_D3D_BLEND_SRC_ALPHA;
                draw.blend.dst_factor = RECOMP_D3D_BLEND_INV_SRC_ALPHA;
                draw.has_texture = true;
                draw.texture.data = 0x3000u;
                draw.texture.format_byte = RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8;
                draw.texture.bits_per_pixel = 32;
                draw.texture.width = draw.texture.height = 1;
                draw.texture_bytes = &base_pixel;
                draw.texture_byte_count = sizeof base_pixel;
                draw.texture_factor = 0x804080c0u;
                light.stage[0] = light.stage[2] = 1;
                light.stage[1] = light.stage[3] = 2;
                if (i == 15) {
                    light.specular[0][0] = 0.2f;
                    light.specular[0][1] = 0.1f;
                    light.specular[0][2] = 0.05f;
                    light.view[2] = 1;
                    light.view[3] = 8;
                }
            }
            struct DiffuseVertex { float x, y, z; uint32_t diffuse; float u, v; };
            DiffuseVertex diffuse_vertices[4]{};
            if (vertex_diffuse) {
                for (unsigned j = 0; j < 4; ++j)
                    diffuse_vertices[j] = {vertices[j].x, vertices[j].y, vertices[j].z,
                        0x8080ffc0u, vertices[j].u, vertices[j].v};
                draw.fvf = 0x142u;
                draw.vertex_stride = sizeof(DiffuseVertex);
                draw.vertex_bytes = diffuse_vertices;
                light.enabled = false;
            }
            if (i == 8) {
                draw.program_alpha_mask = true;
                passed &= expect("reflection mask overlap", d3d11_backend_submit(presenter, &command),
                    RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND);
                draw.program_alpha_mask = false;
            }
            passed &= expect("lighting draw", d3d11_backend_submit(presenter, &command));
        }
        RecompD3dPresenterCommand present{RECOMP_D3D_PRESENTER_COMMAND_PRESENT};
        present.data.present = {5, 1};
        passed &= expect("lighting present", d3d11_backend_submit(presenter, &present));
        passed &= expect("destroy lighting", d3d11_backend_destroy(&presenter));
        if (FILE *file = std::fopen(path, "rb")) {
            const int expected[][3] = {{102, 77, 108}, {26, 26, 32}, {54, 45, 67}, {6, 13, 26},
                {108, 89, 77}, {255, 0, 0}, {0, 0, 0}, {102, 77, 108}, {30, 7, 10}, {21, 39, 21},
                {33, 83, 44}, {46, 10, 48}, {36, 42, 12}, {12, 32, 32}, {12, 16, 12}, {15, 23, 25}};
            // SRC_ALPHA over black checks reflection alpha as well as RGB. PM clamps before diffuse.
            for (unsigned i = 0; i < 16; ++i) {
                unsigned char bgr[3]{};
                const unsigned x = i >= 14 ? 80 + 160 * (i - 14)
                    : i >= 8 ? (160 * (i - 8) + 80) / 3 : 40 + 80 * (i % 4);
                const unsigned y = i >= 14 ? 18 : i >= 8 ? 120 : 60 + 120 * (i / 4);
                std::fseek(file, 54 + (239 - y) * 960 + x * 3, SEEK_SET);
                passed &= std::fread(bgr, 1, 3, file) == 3;
                for (unsigned c = 0; c < 3; ++c) {
                    const bool pixel_ok = std::abs(int(bgr[c]) - expected[i][c]) <= 1;
                    if (!pixel_ok) std::fprintf(stderr, "FAIL lighting pixel %u channel=%u actual=%u expected=%d\n",
                        i, c, bgr[c], expected[i][c]);
                    passed &= pixel_ok;
                }
            }
            std::fclose(file);
        } else passed = false;
    }
    _putenv_s("RECOMP_D3D_FRAME_DUMP", "");
    DeleteFileA(path);
    if (passed) std::puts("PASS native lighting, normal and premultiplied reflections, stage saturation and alpha endpoints");
    return passed;
}

// texgen: stage 0 samples the base at generated coordinates (TEXTURE0 matrix)
// while the half-alpha environment keeps mesh UV, as the fight's chrome does.
// uv_transform: no reflection; UV0 is (u, v, 1, 0) times TEXTURE0.
// stage1_texgen: with texgen, stage 1 also generates through its own TEXTURE1 matrix.
static bool testReflectionAddress(bool texgen, bool uv_transform = false, bool skip_diffuse = false,
    bool stage1_texgen = false, float skin_weight = -1.0f)
{
    char folder[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, folder) || !GetTempFileNameA(folder, "env", 0, path)) return false;
    _putenv_s("RECOMP_D3D_FRAME_DUMP", path);
    RecompD3dPresenter *presenter = nullptr;
    bool passed = expect("create reflection", d3d11_backend_create(&config, &presenter));
    if (passed) {
        struct Vertex { float x, y, z, nx, ny, nz, u, v; };
        const Vertex vertices[] = {
            {-1, 1, 0.5f, 0, 0, 1, 0, 0}, {1, 1, 0.5f, 0, 0, 1, 2, 0},
            {-1, -1, 0.5f, 0, 0, 1, 0, 1}, {1, -1, 0.5f, 0, 0, 1, 2, 1}};
        struct SkinVertex { float x, y, z, weight, nx, ny, nz, u, v; };
        SkinVertex skinned[4];
        for (unsigned i = 0; i < 4; ++i) {
            const auto &v = vertices[i];
            skinned[i] = {v.x, v.y, v.z, skin_weight, v.nx, v.ny, v.nz, v.u, v.v};
        }
        const uint16_t indices[] = {0, 1, 2, 3};
        const uint32_t base_pixels[2] = {0xff0000ffu, 0xffffff00u};
        const uint32_t environment[2] = {texgen ? 0x80ff0000u : 0xffff0000u,
            texgen ? 0x8000ff00u : 0xff00ff00u};
        const float white[] = {1, 1, 1, 1};
        RecompD3dPresenterCommand command{RECOMP_D3D_PRESENTER_COMMAND_DRAW};
        auto &draw = command.data.draw;
        draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
        draw.index_count = draw.vertex_count = 4;
        draw.triangle_count = 2;
        draw.vertex_stride = sizeof(Vertex);
        draw.fvf = 0x112u;
        draw.vertex_bytes = vertices;
        draw.index_bytes = indices;
        draw.blend.color_write_mask = 15;
        draw.has_transform = true;
        draw.transform[0] = draw.transform[5] = draw.transform[10] = draw.transform[15] = 1;
        draw.has_texture = draw.has_reflection = draw.reflection_mesh_uv = true;
        draw.texture.data = 0x1000u;
        draw.texture.format_byte = RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8;
        draw.texture.bits_per_pixel = 32;
        draw.texture.width = draw.texture.height = 1;
        draw.texture_bytes = base_pixels;
        draw.texture_byte_count = sizeof base_pixels[0];
        draw.reflection_texture = draw.texture;
        draw.reflection_texture.data = 0x2000u;
        draw.reflection_texture.width = 2;
        draw.reflection_bytes = environment;
        draw.reflection_byte_count = sizeof environment;
        draw.reflection_address_u = draw.reflection_address_v = 3u;
        std::memcpy(draw.reflection_diffuse, white, sizeof white);
        if (skip_diffuse) {
            // A stage-2 pass-through leaves the blend unmodulated: grey diffuse must not dim it.
            const float grey[] = {0.5f, 0.5f, 0.5f, 1};
            std::memcpy(draw.reflection_diffuse, grey, sizeof grey);
            draw.reflection_skip_diffuse = true;
        }
        if (texgen) {
            draw.texture.width = 2;
            draw.texture_byte_count = sizeof base_pixels;
            draw.texgen_reflection = true;
            draw.reflection_world_view[0] = draw.reflection_world_view[5] = 1;
            draw.reflection_world_view[10] = draw.reflection_world_view[15] = 1;
            std::memcpy(draw.reflection_normal, draw.reflection_world_view, sizeof draw.reflection_normal);
            // Every generated coordinate lands on the yellow base texel.
            draw.reflection_transform[12] = 0.75f;
            draw.reflection_transform[13] = 0.25f;
            draw.reflection_transform[15] = 1;
        }
        if (stage1_texgen) {
            // Base U -0.25 wraps to yellow; environment U 0.75 picks green, and TEXTURE0
            // or an ungenerated (0, 0) coordinate would clamp to red.
            draw.reflection_mesh_uv = false;
            draw.reflection_transform[12] = -0.25f;
            draw.reflection_stage1_transform[12] = 0.75f;
            draw.reflection_stage1_transform[13] = 0.25f;
            draw.reflection_stage1_transform[15] = 1;
        }
        if (uv_transform) {
            draw.texgen_reflection = draw.has_reflection = draw.reflection_mesh_uv = false;
            draw.texture_transform_uv = true;
            std::memset(draw.reflection_transform, 0, sizeof draw.reflection_transform);
            draw.reflection_transform[0] = draw.reflection_transform[5] = 1;
            // The third row translates 2D coordinates; the fourth must not.
            draw.reflection_transform[8] = 0.5f;
            draw.reflection_transform[12] = 0.125f;
        }
        if (skin_weight >= 0.0f) {
            draw.fvf = 0x116u;
            draw.vertex_bytes = skinned;
            draw.vertex_stride = sizeof(SkinVertex);
            draw.blend_weight_count = 1u;
            std::memcpy(draw.blend_transforms[0], draw.transform, sizeof draw.transform);
            draw.reflection_mesh_uv = false;
            draw.reflection_normalize = true;
            // Endpoint eyes (1,0,1), (-1,0,1) and normals Z, X reflect right.
            // Their equal-weight blend reflects left; either missing blend fails.
            draw.reflection_world_view[12] = draw.reflection_world_view[14] = 1;
            draw.reflection_blend_world_view[0][12] = -1;
            draw.reflection_blend_world_view[0][14] = 1;
            draw.reflection_normal[10] = 1;
            draw.reflection_blend_normal[0][8] = 1;
            draw.reflection_transform[0] = draw.reflection_transform[12] = 0.5f;
            draw.reflection_transform[13] = 0.25f;
        }
        passed &= expect("clamped reflection draw", d3d11_backend_submit(presenter, &command));
        RecompD3dPresenterCommand present{RECOMP_D3D_PRESENTER_COMMAND_PRESENT};
        present.data.present = {5, 1};
        passed &= expect("reflection present", d3d11_backend_submit(presenter, &present));
        passed &= expect("destroy reflection", d3d11_backend_destroy(&presenter));
        if (FILE *file = std::fopen(path, "rb")) {
            // U 0.25 samples the red texel; U 1.25 clamps to green (WRAP would give red).
            // With texgen, yellow base halves with them (mesh UV would give blue base).
            const unsigned xs[] = {40, 200};
            for (unsigned i = 0; i < 2; ++i) {
                unsigned char bgr[3]{};
                std::fseek(file, 54 + (239 - 120) * 960 + xs[i] * 3, SEEK_SET);
                passed &= std::fread(bgr, 1, 3, file) == 3;
                // U 0.25 and 1.25 move to the yellow texel centre (0.75, wrapping).
                const bool pixel_ok = skin_weight >= 0.0f
                    ? bgr[0] < 10 && (skin_weight == 0.5f
                        ? bgr[2] > 245 && bgr[1] < 10 : bgr[1] > 245 && bgr[2] < 10)
                    : uv_transform ? bgr[2] > 245 && bgr[1] > 245 && bgr[0] < 10
                    : stage1_texgen
                    ? bgr[0] < 10 && bgr[1] > 250 && std::abs(int(bgr[2]) - 128) <= 3
                    : texgen
                    ? bgr[0] < 10 && std::abs(int(bgr[2 - i]) - 255) <= 3 && std::abs(int(bgr[1 + i]) - 128) <= 3
                    : i == 0 ? bgr[2] > 245 && bgr[1] < 10 : bgr[1] > 245 && bgr[2] < 10;
                if (!pixel_ok) std::fprintf(stderr, "FAIL reflection address pixel %u BGR=%u,%u,%u\n",
                    i, bgr[0], bgr[1], bgr[2]);
                passed &= pixel_ok;
            }
            std::fclose(file);
        } else passed = false;
    }
    _putenv_s("RECOMP_D3D_FRAME_DUMP", "");
    DeleteFileA(path);
    if (passed) std::puts(skin_weight >= 0.0f ? "PASS skinned reflection eye, normal and remainder weights"
        : stage1_texgen ? "PASS both stages generate through their own texture matrices"
        : skip_diffuse ? "PASS reflection stage-2 pass-through skips the final diffuse"
        : uv_transform ? "PASS stage-0 texture matrix translates 2D UV by its third row"
        : texgen ? "PASS stage-0 texgen pairs with a mesh-UV reflection blend"
        : "PASS reflection texture samples with its own stage address mode");
    return passed;
}

static bool testScreenSpaceOffsetAndScissor()
{
    char folder[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, folder) || !GetTempFileNameA(folder, "off", 0, path)) return false;
    _putenv_s("RECOMP_D3D_FRAME_DUMP", path);
    RecompD3dPresenter *presenter = nullptr;
    bool passed = expect("create screen offset", d3d11_backend_create(&config, &presenter));
    if (passed) {
        auto clear = clearCommand();
        clear.data.clear.color = 0xff000000u;
        passed &= expect("offset clear", d3d11_backend_submit(presenter, &clear));
        const float offsets[][2] = {{12, 10}, {-12, -10}, {20, -14}, {-20, 14}};
        const uint16_t indices[] = {0, 1, 2, 3};
        for (unsigned i = 0; i < 4 && passed; ++i) {
            RecompD3dPresenterCommand command{RECOMP_D3D_PRESENTER_COMMAND_DRAW};
            auto &draw = command.data.draw;
            draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
            draw.index_count = draw.vertex_count = 4;
            draw.triangle_count = 2;
            draw.index_bytes = indices;
            draw.blend.color_write_mask = 15;
            std::memcpy(draw.screen_space_offset, offsets[i], sizeof offsets[i]);
            struct ScreenVertex { float x, y, z, rhw; uint32_t diffuse; };
            ScreenVertex screen[] = {{20, 20, 0.25f, 0.5f, 0xffffffffu}, {28, 20, 0.25f, 0.25f, 0xffffffffu},
                {20, 28, 0.25f, 0.5f, 0xffffffffu}, {28, 28, 0.25f, 0.25f, 0xffffffffu}};
            struct BlendVertex { float x, y, z, weights[3]; uint32_t diffuse; };
            const BlendVertex blended[] = {{-0.5f, 1, 0.5f, {0.1f, 0.2f, 0.3f}, 0xffffffffu},
                {0.5f, 1, 0.5f, {0.1f, 0.2f, 0.3f}, 0xffffffffu},
                {-0.5f, -1, 0.5f, {0.1f, 0.2f, 0.3f}, 0xffffffffu},
                {0.5f, -1, 0.5f, {0.1f, 0.2f, 0.3f}, 0xffffffffu}};
            struct Vertex { float x, y, z; uint32_t diffuse; };
            const Vertex vertices[] = {{-0.05f, 0.1f, 0.5f, 0xffffffffu}, {0.05f, 0.1f, 0.5f, 0xffffffffu},
                {-0.05f, -0.1f, 0.5f, 0xffffffffu}, {0.05f, -0.1f, 0.5f, 0xffffffffu}};
            if (i < 2) {
                if (i == 1) for (auto &vertex : screen) vertex.x += 60;
                draw.fvf = 0x44u;
                draw.vertex_stride = sizeof(ScreenVertex);
                draw.vertex_bytes = screen;
            } else {
                draw.has_transform = true;
                if (i == 2) {
                    draw.fvf = 0x4au;
                    draw.vertex_stride = sizeof(BlendVertex);
                    draw.vertex_bytes = blended;
                    draw.blend_weight_count = 3;
                    draw.transform[0] = -0.7f;
                    draw.transform[3] = 1.5f;
                    draw.transform[5] = 0.1f;
                    draw.transform[10] = 1;
                    draw.transform[12] = -1;
                    draw.transform[15] = 2;
                    for (auto &transform : draw.blend_transforms)
                        std::memcpy(transform, draw.transform, sizeof draw.transform);
                } else {
                    draw.fvf = 0x42u;
                    draw.vertex_stride = sizeof(Vertex);
                    draw.vertex_bytes = vertices;
                    for (unsigned j = 0; j < 16; j += 5) draw.transform[j] = 1;
                    const float viewport[] = {160, 80, 160, 120, 0, 1};
                    std::memcpy(draw.viewport, viewport, sizeof viewport);
                }
            }
            passed &= expect("screen offset draw", d3d11_backend_submit(presenter, &command));
            if (i == 0) {
                for (unsigned axis = 0; axis < 2; ++axis) {
                    for (float invalid : {INFINITY, NAN}) {
                        draw.screen_space_offset[axis] = invalid;
                        passed &= expect("nonfinite screen offset", d3d11_backend_submit(presenter, &command),
                            RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND);
                    }
                    draw.screen_space_offset[axis] = offsets[i][axis];
                }
                const float program_vertices[4][8]{};
                draw.fvf = 0x112u;
                draw.vertex_stride = sizeof program_vertices[0];
                draw.vertex_bytes = program_vertices;
                draw.program_count = 1;
                draw.program[0][3] = 1; // One synthetic terminating NOP.
                draw.program_constants[58][0] = draw.program_constants[58][1] = draw.program_constants[58][2] = 1;
                draw.screen_space_offset[0] = draw.screen_space_offset[1] = 0;
                passed &= expect("zero program screen offset", d3d11_backend_submit(presenter, &command));
                draw.screen_space_offset[0] = 0.5f;
                passed &= expect("unsupported program screen offset", d3d11_backend_submit(presenter, &command),
                    RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND);
            }
        }
        struct ScreenVertex { float x, y, z, rhw; uint32_t diffuse; };
        ScreenVertex vertices[] = {{20, 180, 0.25f, 1, 0xffffffffu}, {148, 180, 0.25f, 1, 0xffffffffu},
            {20, 236, 0.25f, 1, 0xffffffffu}, {148, 236, 0.25f, 1, 0xffffffffu}};
        RecompD3dPresenterCommand clipped{RECOMP_D3D_PRESENTER_COMMAND_DRAW};
        auto &draw = clipped.data.draw;
        draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
        draw.index_count = draw.vertex_count = 4;
        draw.triangle_count = 2;
        draw.fvf = 0x44u;
        draw.vertex_stride = sizeof(ScreenVertex);
        draw.vertex_bytes = vertices;
        draw.index_bytes = indices;
        draw.blend.color_write_mask = 15;
        draw.screen_space_offset[0] = 8;
        draw.screen_space_offset[1] = 4;
        draw.has_scissor = true;
        const int32_t scissor[] = {40, 192, 100, 216};
        std::memcpy(draw.scissor, scissor, sizeof scissor);
        passed &= expect("scissor draw", d3d11_backend_submit(presenter, &clipped));
        draw.scissor[2] = 39;
        passed &= expect("inverted scissor", d3d11_backend_submit(presenter, &clipped),
            RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND);
        draw.scissor[2] = 100;
        draw.has_scissor = false;
        for (auto &vertex : vertices) vertex.x += 152;
        passed &= expect("scissor disabled draw", d3d11_backend_submit(presenter, &clipped));
        RecompD3dPresenterCommand present{RECOMP_D3D_PRESENTER_COMMAND_PRESENT};
        present.data.present = {5, 1};
        passed &= expect("offset present", d3d11_backend_submit(presenter, &present));
        passed &= expect("destroy offset", d3d11_backend_destroy(&presenter));
        if (FILE *file = std::fopen(path, "rb")) {
            // Offset positions move; scissor edges stay absolute and exclude right/bottom.
            const unsigned pixels[][3] = {{34, 32, 255}, {22, 22, 0}, {70, 12, 255}, {82, 22, 0},
                {98, 106, 255}, {80, 120, 0}, {106, 106, 0}, {220, 154, 255}, {240, 140, 0},
                {40, 192, 255}, {99, 215, 255}, {39, 200, 0}, {100, 200, 0}, {60, 191, 0}, {60, 216, 0},
                {200, 200, 255}, {280, 224, 255}};
            for (const auto &pixel : pixels) {
                unsigned char bgr[3]{};
                std::fseek(file, 54 + (239 - pixel[1]) * 960 + pixel[0] * 3, SEEK_SET);
                passed &= std::fread(bgr, 1, 3, file) == 3;
                const bool pixel_ok = bgr[0] == pixel[2] && bgr[1] == pixel[2] && bgr[2] == pixel[2];
                if (!pixel_ok) std::fprintf(stderr, "FAIL offset/scissor pixel (%u,%u) BGR=%u,%u,%u expected=%u\n",
                    pixel[0], pixel[1], bgr[0], bgr[1], bgr[2], pixel[2]);
                passed &= pixel_ok;
            }
            std::fclose(file);
        } else passed = false;
    }
    _putenv_s("RECOMP_D3D_FRAME_DUMP", "");
    DeleteFileA(path);
    if (passed) std::puts("PASS screen-space offsets, XYZRHW, blended transforms, clip W, subviewport and scissor reset");
    return passed;
}

static bool testBrightPass()
{
    char folder[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, folder) || !GetTempFileNameA(folder, "brt", 0, path)) return false;
    _putenv_s("RECOMP_D3D_FRAME_DUMP", path);
    RecompD3dPresenter *presenter = nullptr;
    bool passed = expect("create bright pass", d3d11_backend_create(&config, &presenter));
    if (passed) {
        // 3x3 tiles: red 0.75 with a 1.0 centre, green 0.9, blue 0.5 (below threshold).
        uint32_t texels[9 * 9];
        for (unsigned y = 0; y < 9; ++y)
            for (unsigned x = 0; x < 9; ++x)
                texels[y * 9 + x] = (x % 3 == 1 && y % 3 == 1 ? 0xffff0000u : 0xffbf0000u) | 0xe580u;
        struct Vertex { float x, y, z, rhw, u, v; };
        const Vertex vertices[] = {{40, 40, 0, 1, 0, 0}, {43, 40, 0, 1, 9, 0},
            {40, 43, 0, 1, 0, 9}, {43, 43, 0, 1, 9, 9}};
        const uint16_t indices[] = {0, 1, 2, 3};
        auto clear = clearCommand();
        clear.data.clear.color = 0xff000000u;
        passed &= expect("bright clear", d3d11_backend_submit(presenter, &clear));
        RecompD3dPresenterCommand command{RECOMP_D3D_PRESENTER_COMMAND_DRAW};
        auto &draw = command.data.draw;
        draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
        draw.index_count = draw.vertex_count = 4;
        draw.triangle_count = 2;
        draw.vertex_stride = sizeof(Vertex);
        draw.fvf = 0x104u;
        draw.vertex_bytes = vertices;
        draw.index_bytes = indices;
        draw.blend.color_write_mask = 15;
        // The game's -0.5 offset puts each output centre on its tile's middle texel.
        draw.screen_space_offset[0] = draw.screen_space_offset[1] = -0.5f;
        draw.address_u = draw.address_v = 3;
        draw.has_texture = true;
        draw.texture.format_byte = 0x12u;
        draw.texture.linear = true;
        draw.texture.bits_per_pixel = 32;
        draw.texture.width = draw.texture.height = 9;
        draw.texture.pitch = 36;
        draw.texture.mip_levels = 1;
        draw.texture.data = 0x2000u;
        draw.texture_bytes = texels;
        draw.texture_byte_count = sizeof texels;
        draw.bright_pass = true;
        draw.bright_pass_threshold = 0x1a1a1a1au;
        passed &= expect("bright pass draw", d3d11_backend_submit(presenter, &command));
        draw.four_tap_filter = true;
        passed &= expect("bright pass excludes four tap", d3d11_backend_submit(presenter, &command),
            RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND);
        // Four texel-centred taps of a 2x2 texture: red 0.6, 0.3, 0.9, 0; green 1.
        const uint32_t taps[] = {0xff99ff00u, 0xff4dff00u, 0xffe6ff00u, 0xff00ff00u};
        struct TapVertex { float x, y, z, rhw, uv[8]; };
        TapVertex quad[4]{};
        for (unsigned i = 0; i < 4; ++i) {
            quad[i] = {100.0f + (i & 1) * 8, 40.0f + (i >> 1) * 8, 0, 1,
                {0.5f, 0.5f, 1.5f, 0.5f, 0.5f, 1.5f, 1.5f, 1.5f}};
        }
        RecompD3dPresenterCommand tap{RECOMP_D3D_PRESENTER_COMMAND_DRAW};
        auto &filter = tap.data.draw;
        filter.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
        filter.index_count = filter.vertex_count = 4;
        filter.triangle_count = 2;
        filter.vertex_stride = sizeof(TapVertex);
        filter.fvf = 0x404u;
        filter.vertex_bytes = quad;
        filter.index_bytes = indices;
        filter.blend.color_write_mask = 15;
        filter.has_texture = true;
        filter.texture = draw.texture;
        filter.texture.width = filter.texture.height = 2;
        filter.texture.pitch = 8;
        filter.texture.data = 0x3000u;
        filter.texture_bytes = taps;
        filter.texture_byte_count = sizeof taps;
        filter.four_tap_filter = true;
        // Identity variant: 1/3 and 1/6 weights; doubled variant: 0x40 weights, x2 then /2.
        const uint32_t weights[2][2] = {{0x55555555u, 0x2b2b2b2bu}, {0x40404040u, 0x40404040u}};
        for (unsigned v = 0; v < 2 && passed; ++v) {
            for (unsigned i = 0; i < 4; ++i) filter.four_tap_factors[i] = weights[v][i & 1];
            filter.four_tap_scale = v ? 2.0f : 1.0f;
            filter.four_tap_sum_scale = v ? 0.5f : 1.0f;
            passed &= expect("four tap draw", d3d11_backend_submit(presenter, &tap));
            for (auto &vertex : quad) vertex.x += 20;
        }
        // Composite: 2 * texture * 0.8, saturating at red 0.75 and keeping green 0.5 -> 0.8.
        const uint32_t composite_texel = 0xffbf8000u;
        RecompD3dPresenterCommand composite = command;
        auto &mod = composite.data.draw;
        mod.bright_pass = mod.four_tap_filter = false;
        mod.texture.width = mod.texture.height = 1;
        mod.texture.pitch = 4;
        mod.texture.data = 0x4000u;
        mod.texture_bytes = &composite_texel;
        mod.texture_byte_count = sizeof composite_texel;
        mod.texture_factor = 0xccccccccu;
        mod.texture_factor_modulate2x = true;
        mod.material_alpha_mode = RECOMP_D3D_MATERIAL_ALPHA_SELECT_DIFFUSE;
        mod.material_alpha = 1.0f;
        Vertex composite_quad[4];
        std::memcpy(composite_quad, vertices, sizeof vertices);
        for (auto &vertex : composite_quad) { vertex.x += 100; vertex.u = vertex.v = 0.5f; }
        mod.vertex_bytes = composite_quad;
        passed &= expect("modulate2x draw", d3d11_backend_submit(presenter, &composite));
        // Trail strips: 1x texture * factor, red 0.6 and green 0.4.
        mod.texture_factor_modulate2x = false;
        mod.texture_factor_modulate = true;
        Vertex trail_quad[4];
        std::memcpy(trail_quad, composite_quad, sizeof composite_quad);
        for (auto &vertex : trail_quad) vertex.x += 40;
        mod.vertex_bytes = trail_quad;
        passed &= expect("modulate draw", d3d11_backend_submit(presenter, &composite));
        RecompD3dPresenterCommand present{RECOMP_D3D_PRESENTER_COMMAND_PRESENT};
        present.data.present = {5, 1};
        passed &= expect("bright present", d3d11_backend_submit(presenter, &present));
        passed &= expect("destroy bright pass", d3d11_backend_destroy(&presenter));
        if (FILE *file = std::fopen(path, "rb")) {
            // Gaussian red 0.8125 -> 107; linear 1.0 would give 203, a box filter 90.
            const unsigned pixels[][5] = {{40, 40, 107, 152, 0}, {42, 41, 107, 152, 0}, {43, 43, 0, 0, 0},
                {104, 44, 141, 255, 0}, {124, 44, 115, 255, 0}, {141, 41, 255, 204, 0}, {181, 41, 153, 102, 0}};
            for (const auto &pixel : pixels) {
                unsigned char bgr[3]{};
                std::fseek(file, 54 + (239 - pixel[1]) * 960 + pixel[0] * 3, SEEK_SET);
                passed &= std::fread(bgr, 1, 3, file) == 3;
                const bool pixel_ok = std::abs(bgr[2] - static_cast<int>(pixel[2])) <= 2 &&
                    std::abs(bgr[1] - static_cast<int>(pixel[3])) <= 2 && bgr[0] == pixel[4];
                if (!pixel_ok) std::fprintf(stderr, "FAIL bright pixel (%u,%u) BGR=%u,%u,%u\n",
                    pixel[0], pixel[1], bgr[0], bgr[1], bgr[2]);
                passed &= pixel_ok;
            }
            std::fclose(file);
        } else passed = false;
    }
    _putenv_s("RECOMP_D3D_FRAME_DUMP", "");
    DeleteFileA(path);
    if (passed) std::puts("PASS Gaussian-cubic bright pass, weighted four-tap filters and MODULATE2X composite");
    return passed;
}

// DOT_ST distortion: t2 is read at (dot(uv1, uv2.x; n), dot(uv2.y, uv3; n)) with n the signed t0 texel.
static bool testDistortion()
{
    char folder[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, folder) || !GetTempFileNameA(folder, "dst", 0, path)) return false;
    _putenv_s("RECOMP_D3D_FRAME_DUMP", path);
    RecompD3dPresenter *presenter = nullptr;
    bool passed = expect("create distortion", d3d11_backend_create(&config, &presenter));
    if (passed) {
        auto clear = clearCommand();
        clear.data.clear.color = 0xff000000u;
        passed &= expect("distortion clear", d3d11_backend_submit(presenter, &clear));
        // Normal texel (255, 128, 128) is n = (1, 0, 0); ZERO_TO_ONE would give (1, 0.5, 0.5).
        const uint32_t normal = 0xffff8080u;
        const uint32_t scene[2] = {0xffff0000u, 0xff00ff00u};
        struct Vertex { float x, y, z, rhw, uv[8]; };
        const uint16_t indices[] = {0, 1, 2, 3};
        // Quad A reads s = 0.5 (red; unsigned n would add 2 and clamp to green), quad B s = 1.5 (green).
        const float s[2] = {0.5f, 1.5f};
        for (unsigned q = 0; q < 2 && passed; ++q) {
            Vertex quad[4]{};
            for (unsigned i = 0; i < 4; ++i)
                quad[i] = {40.0f + q * 160 + (i & 1) * 20, 100.0f + (i >> 1) * 40, 0, 1,
                    {0.5f, 0.5f, s[q], 4, 0, 0.5f, 4, 4}};
            RecompD3dPresenterCommand command{RECOMP_D3D_PRESENTER_COMMAND_DRAW};
            auto &draw = command.data.draw;
            draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
            draw.index_count = draw.vertex_count = 4;
            draw.triangle_count = 2;
            draw.vertex_stride = sizeof(Vertex);
            draw.fvf = 0x404u;
            draw.vertex_bytes = quad;
            draw.index_bytes = indices;
            draw.blend.color_write_mask = 15;
            draw.address_u = draw.address_v = 3;
            draw.has_texture = true;
            draw.texture.format_byte = 0x12u;
            draw.texture.linear = true;
            draw.texture.bits_per_pixel = 32;
            draw.texture.width = 2;
            draw.texture.height = 1;
            draw.texture.pitch = 8;
            draw.texture.mip_levels = 1;
            draw.texture.data = 0x2000u;
            draw.texture_bytes = scene;
            draw.texture_byte_count = sizeof scene;
            draw.reflection_texture.format_byte = RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8;
            draw.reflection_texture.bits_per_pixel = 32;
            draw.reflection_texture.width = draw.reflection_texture.height = 1;
            draw.reflection_texture.mip_levels = 1;
            draw.reflection_texture.data = 0x3000u;
            draw.reflection_bytes = &normal;
            draw.reflection_byte_count = sizeof normal;
            draw.reflection_address_u = draw.reflection_address_v = 3;
            draw.distortion = true;
            passed &= expect("distortion draw", d3d11_backend_submit(presenter, &command));
        }
        RecompD3dPresenterCommand present{RECOMP_D3D_PRESENTER_COMMAND_PRESENT};
        present.data.present = {5, 1};
        passed &= expect("distortion present", d3d11_backend_submit(presenter, &present));
        passed &= expect("destroy distortion", d3d11_backend_destroy(&presenter));
        if (FILE *file = std::fopen(path, "rb")) {
            const unsigned xs[] = {50, 210};
            for (unsigned i = 0; i < 2; ++i) {
                unsigned char bgr[3]{};
                std::fseek(file, 54 + (239 - 120) * 960 + xs[i] * 3, SEEK_SET);
                passed &= std::fread(bgr, 1, 3, file) == 3;
                const bool pixel_ok = bgr[0] < 10 && (i == 0 ? bgr[2] > 245 && bgr[1] < 10 : bgr[1] > 245 && bgr[2] < 10);
                if (!pixel_ok) std::fprintf(stderr, "FAIL distortion pixel %u BGR=%u,%u,%u\n",
                    i, bgr[0], bgr[1], bgr[2]);
                passed &= pixel_ok;
            }
            std::fclose(file);
        } else passed = false;
    }
    _putenv_s("RECOMP_D3D_FRAME_DUMP", "");
    DeleteFileA(path);
    if (passed) std::puts("PASS DOT_ST distortion reads the scene at signed normal dot products");
    return passed;
}

// Stage-effect overlays: unlit SELECTARG1(TEXTURE) over diffuse, texture-factor
// alpha, a stage-1 UV1 alpha mask with ALPHAKILL, and projected camera-space texgen.
static bool testStageEffect()
{
    char folder[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, folder) || !GetTempFileNameA(folder, "eff", 0, path)) return false;
    _putenv_s("RECOMP_D3D_FRAME_DUMP", path);
    RecompD3dPresenter *presenter = nullptr;
    bool passed = expect("create stage effect", d3d11_backend_create(&config, &presenter));
    if (passed) {
        auto clear = clearCommand();
        clear.data.clear.color = 0xff000000u;
        passed &= expect("effect clear", d3d11_backend_submit(presenter, &clear));
        const uint16_t indices[] = {0, 1, 2, 3};
        const uint32_t green = 0xff00ff00u;
        // Pass 2 is FUN_0027be10's kill-only mask over UV0 (FVF 0x144).
        const uint32_t masks[3][2] = {{0xff000000u, 0x80000000u}, {0xff000000u, 0x00000000u},
            {0x80000000u, 0x00000000u}};
        struct Screen { float x, y, z, rhw; uint32_t diffuse; float u0, v0, u1, v1; };
        for (unsigned pass = 0; pass < 3 && passed; ++pass) {
            Screen quad[4];
            for (unsigned i = 0; i < 4; ++i)
                quad[i] = {20.0f + pass * 60 + (i & 1) * 40, 20.0f + (i >> 1) * 40, 0, 1, 0xffff0000u,
                    pass == 2 ? float(i & 1) : 0.5f, 0.5f, pass == 2 ? 0.5f : float(i & 1), 0.5f};
            RecompD3dPresenterCommand command{RECOMP_D3D_PRESENTER_COMMAND_DRAW};
            auto &draw = command.data.draw;
            draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
            draw.index_count = draw.vertex_count = 4;
            draw.triangle_count = 2;
            draw.vertex_stride = sizeof(Screen);
            draw.fvf = pass == 2 ? 0x144u : 0x244u;
            draw.vertex_bytes = quad;
            draw.index_bytes = indices;
            draw.blend.color_write_mask = 15;
            draw.blend.blend_enable = pass != 1;
            draw.blend.src_factor = RECOMP_D3D_BLEND_SRC_ALPHA;
            draw.blend.dst_factor = RECOMP_D3D_BLEND_INV_SRC_ALPHA;
            draw.blend.op = RECOMP_D3D_BLEND_OP_ADD;
            draw.address_u = draw.address_v = 3;
            draw.has_texture = true;
            draw.texture.format_byte = RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8;
            draw.texture.bits_per_pixel = 32;
            draw.texture.width = draw.texture.height = 1;
            draw.texture.mip_levels = 1;
            draw.texture.data = 0x1000u;
            draw.texture_bytes = &green;
            draw.texture_byte_count = sizeof green;
            draw.reflection_texture = draw.texture;
            draw.reflection_texture.width = 2;
            draw.reflection_texture.data = 0x2000u + pass * 0x100u;
            draw.reflection_bytes = masks[pass];
            draw.reflection_byte_count = sizeof masks[pass];
            draw.reflection_address_u = draw.reflection_address_v = 3;
            draw.directional.stage_mode = true;
            draw.directional.stage[0] = draw.directional.stage[3] = 1;
            draw.texture_factor = 0x80ffffffu;
            draw.modulate_texture_factor_alpha = true;
            draw.stage1_alpha_mask = draw.stage1_alpha_kill = true;
            draw.stage1_kill_only = pass == 2;
            passed &= expect("masked overlay draw", d3d11_backend_submit(presenter, &command));
        }
        // Eight texels, red then green; s / q = (x + 1) / 2 across the screen.
        uint32_t strip[8];
        for (unsigned i = 0; i < 8; ++i) strip[i] = i < 4 ? 0xffff0000u : 0xff00ff00u;
        struct World { float x, y, z; uint32_t diffuse; float u, v; };
        // The lower half of the screen; the overlays stay above it.
        const World quad[] = {{-1, 0, 0.5f, 0xff0000ffu, 0, 0}, {1, 0, 0.5f, 0xff0000ffu, 0, 0},
            {-1, -1, 0.5f, 0xff0000ffu, 0, 0}, {1, -1, 0.5f, 0xff0000ffu, 0, 0}};
        RecompD3dPresenterCommand command{RECOMP_D3D_PRESENTER_COMMAND_DRAW};
        auto &draw = command.data.draw;
        draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
        draw.index_count = draw.vertex_count = 4;
        draw.triangle_count = 2;
        draw.vertex_stride = sizeof(World);
        draw.fvf = 0x142u;
        draw.vertex_bytes = quad;
        draw.index_bytes = indices;
        draw.blend.color_write_mask = 15;
        draw.has_transform = true;
        draw.transform[0] = draw.transform[5] = draw.transform[10] = draw.transform[15] = 1;
        draw.address_u = draw.address_v = 3;
        draw.has_texture = true;
        draw.texture.format_byte = RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8;
        draw.texture.bits_per_pixel = 32;
        draw.texture.width = 8;
        draw.texture.height = 1;
        draw.texture.mip_levels = 1;
        draw.texture.data = 0x3000u;
        draw.texture_bytes = strip;
        draw.texture_byte_count = sizeof strip;
        draw.texgen_projected = true;
        draw.reflection_world_view[0] = draw.reflection_world_view[5] = 1;
        draw.reflection_world_view[10] = draw.reflection_world_view[15] = 1;
        // s = x + 2z, t = 2z, q = 4z: with z = 0.5, q = 2 and only the divide maps s into [0, 1].
        draw.reflection_transform[0] = 1;
        draw.reflection_transform[8] = 2;
        draw.reflection_transform[9] = 2;
        draw.reflection_transform[10] = 4;
        draw.reflection_transform[15] = 1;
        draw.directional.stage_mode = true;
        draw.directional.stage[0] = draw.directional.stage[2] = draw.directional.stage[3] = 1;
        draw.texture_factor = 0xffffffffu;
        draw.modulate_texture_factor_alpha = true;
        passed &= expect("projected draw", d3d11_backend_submit(presenter, &command));
        RecompD3dPresenterCommand present{RECOMP_D3D_PRESENTER_COMMAND_PRESENT};
        present.data.present = {5, 1};
        passed &= expect("effect present", d3d11_backend_submit(presenter, &present));
        passed &= expect("destroy stage effect", d3d11_backend_destroy(&presenter));
        if (FILE *file = std::fopen(path, "rb")) {
            // {x, y, R, G}: masked green at alpha 0.5 and 0.25, then 1.0 and killed, then
            // kill-only at factor alpha 0.5 and killed;
            // the projected quad reads red at s/q 0.31 (s 0.63 would be green), green at 0.69.
            const int pixels[][4] = {{25, 40, 0, 128}, {55, 40, 0, 64}, {85, 40, 0, 255}, {115, 40, 0, 0},
                {145, 40, 0, 128}, {175, 40, 0, 0}, {100, 200, 255, 0}, {220, 200, 0, 255}};
            for (const auto &pixel : pixels) {
                unsigned char bgr[3]{};
                std::fseek(file, 54 + (239 - pixel[1]) * 960 + pixel[0] * 3, SEEK_SET);
                passed &= std::fread(bgr, 1, 3, file) == 3;
                const bool pixel_ok = std::abs(bgr[2] - pixel[2]) <= 3 && std::abs(bgr[1] - pixel[3]) <= 3 &&
                    bgr[0] < 4;
                if (!pixel_ok) std::fprintf(stderr, "FAIL stage effect pixel (%d,%d) BGR=%u,%u,%u\n",
                    pixel[0], pixel[1], bgr[0], bgr[1], bgr[2]);
                passed &= pixel_ok;
            }
            std::fclose(file);
        } else passed = false;
    }
    _putenv_s("RECOMP_D3D_FRAME_DUMP", "");
    DeleteFileA(path);
    if (passed) std::puts("PASS stage-effect overlays: texture select, factor alpha, UV1 mask/kill, UV0 kill, projected texgen");
    return passed;
}

// Vertex-program MAX and SLT over a position/normal stream (FVF 0x012), as in
// the projected shadow program: max(v0, c1) clamps x >= 0 and slt(v2, c2) = 1.
static bool testProgramMaxSlt()
{
    char folder[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, folder) || !GetTempFileNameA(folder, "vps", 0, path)) return false;
    _putenv_s("RECOMP_D3D_FRAME_DUMP", path);
    RecompD3dPresenter *presenter = nullptr;
    bool passed = expect("create program", d3d11_backend_create(&config, &presenter));
    if (passed) {
        auto clear = clearCommand();
        clear.data.clear.color = 0xff000000u;
        passed &= expect("program clear", d3d11_backend_submit(presenter, &clear));
        const float vertices[4][6] = {{-1, 1, 0.5f, 0, 0, 0}, {1, 1, 0.5f, 0, 0, 0},
            {-1, -1, 0.5f, 0, 0, 0}, {1, -1, 0.5f, 0, 0, 0}};
        const uint16_t indices[] = {0, 1, 2, 3};
        const uint32_t texels[2] = {0xffff0000u, 0xff00ff00u};
        RecompD3dPresenterCommand command{RECOMP_D3D_PRESENTER_COMMAND_DRAW};
        auto &draw = command.data.draw;
        draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
        draw.index_count = draw.vertex_count = 4;
        draw.triangle_count = 2;
        draw.vertex_stride = sizeof vertices[0];
        draw.fvf = 0x012u;
        draw.vertex_bytes = vertices;
        draw.index_bytes = indices;
        draw.blend.color_write_mask = 15;
        draw.address_u = draw.address_v = 3;
        draw.has_texture = true;
        draw.texture.format_byte = RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8;
        draw.texture.bits_per_pixel = 32;
        draw.texture.width = 2;
        draw.texture.height = 1;
        draw.texture.mip_levels = 1;
        draw.texture.data = 0x1000u;
        draw.texture_bytes = texels;
        draw.texture_byte_count = sizeof texels;
        // a: mac, attribute, constant, A swizzle; b: A and B mux/register/swizzle; c: output.
        const uint32_t xyzw = 0x1bu;
        draw.program_count = 2;
        draw.program[0][1] = (10u << 21) | (1u << 13) | xyzw;
        draw.program[0][2] = (2u << 26) | (3u << 11) | (xyzw << 17);
        draw.program[0][3] = (15u << 12) | 2048u;
        draw.program[1][1] = (11u << 21) | (2u << 9) | (2u << 13) | xyzw;
        draw.program[1][2] = (2u << 26) | (3u << 11) | (xyzw << 17);
        draw.program[1][3] = (15u << 12) | (9u << 3) | 2048u | 1u;
        const float c1[4] = {0, -2, 0.5f, 1}, c2[4] = {1, 1, 1, 2};
        std::memcpy(draw.program_constants[1], c1, sizeof c1);
        std::memcpy(draw.program_constants[2], c2, sizeof c2);
        draw.program_constants[58][0] = draw.program_constants[58][1] = draw.program_constants[58][2] = 1;
        passed &= expect("max/slt program draw", d3d11_backend_submit(presenter, &command));
        // Position/color/UV (FVF 0x142): oD0 = v3 feeds MODULATE(TEXTURE, oD0) color;
        // alpha is texture times the factor's 0.5, ignoring oD0's 0.5 alpha.
        struct Colored { float x, y, z; uint32_t color; float u, v; };
        const Colored colored[] = {{-1, 1, 0.5f, 0x80ff0000u, 0, 0}, {0, 1, 0.5f, 0x80ff0000u, 0, 0},
            {-1, -1, 0.5f, 0x80ff0000u, 0, 0}, {0, -1, 0.5f, 0x80ff0000u, 0, 0}};
        const uint32_t white = 0xffffffffu;
        RecompD3dPresenterCommand second = command;
        auto &color = second.data.draw;
        color.fvf = 0x142u;
        color.vertex_stride = sizeof(Colored);
        color.vertex_bytes = colored;
        color.texture.width = 1;
        color.texture.data = 0x2000u;
        color.texture_bytes = &white;
        color.texture_byte_count = sizeof white;
        color.blend.blend_enable = true;
        color.blend.src_factor = RECOMP_D3D_BLEND_SRC_ALPHA;
        color.blend.dst_factor = RECOMP_D3D_BLEND_INV_SRC_ALPHA;
        color.blend.op = RECOMP_D3D_BLEND_OP_ADD;
        color.directional.stage_mode = true;
        color.directional.stage[0] = color.directional.stage[1] = color.directional.stage[2] = 1;
        color.texture_factor = 0x80ffffffu;
        color.modulate_texture_factor_alpha = true;
        const uint32_t movs[3][2] = {{0u, 0u}, {3u, 3u}, {9u, 9u}};
        color.program_count = 3;
        for (unsigned i = 0; i < 3; ++i) {
            color.program[i][1] = (1u << 21) | (movs[i][0] << 9) | xyzw;
            color.program[i][2] = 2u << 26;
            color.program[i][3] = (15u << 12) | (movs[i][1] << 3) | 2048u | (i == 2 ? 1u : 0u);
        }
        passed &= expect("color program draw", d3d11_backend_submit(presenter, &second));
        RecompD3dPresenterCommand present{RECOMP_D3D_PRESENTER_COMMAND_PRESENT};
        present.data.present = {5, 1};
        passed &= expect("program present", d3d11_backend_submit(presenter, &present));
        passed &= expect("destroy program", d3d11_backend_destroy(&presenter));
        if (FILE *file = std::fopen(path, "rb")) {
            // MAX keeps the left half for the color program (red at half alpha);
            // SLT's (1, 1, 1, 1) samples the green texel.
            const unsigned xs[] = {80, 240};
            for (unsigned i = 0; i < 2; ++i) {
                unsigned char bgr[3]{};
                std::fseek(file, 54 + (239 - 120) * 960 + xs[i] * 3, SEEK_SET);
                passed &= std::fread(bgr, 1, 3, file) == 3;
                const bool pixel_ok = bgr[0] < 4 && (i == 0 ? bgr[1] < 4 && std::abs(bgr[2] - 128) <= 3
                    : bgr[2] < 4 && bgr[1] > 250);
                if (!pixel_ok) std::fprintf(stderr, "FAIL program max/slt pixel %u BGR=%u,%u,%u\n",
                    i, bgr[0], bgr[1], bgr[2]);
                passed &= pixel_ok;
            }
            std::fclose(file);
        } else passed = false;
    }
    _putenv_s("RECOMP_D3D_FRAME_DUMP", "");
    DeleteFileA(path);
    if (passed) std::puts("PASS vertex-program MAX/SLT over a position/normal stream");
    return passed;
}

// Vertex-program LIT and SGE over the stage effect's four-UV stream (FVF 0x442):
// oT0 = lit(v10).zwxx * sge(v11, c3).yyyy, so x = v10.y only when v10.x > 0.
static bool testProgramLitSge()
{
    char folder[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, folder) || !GetTempFileNameA(folder, "vpl", 0, path)) return false;
    _putenv_s("RECOMP_D3D_FRAME_DUMP", path);
    RecompD3dPresenter *presenter = nullptr;
    bool passed = expect("create lit", d3d11_backend_create(&config, &presenter));
    if (passed) {
        auto clear = clearCommand();
        clear.data.clear.color = 0xff000000u;
        passed &= expect("lit clear", d3d11_backend_submit(presenter, &clear));
        struct Vertex { float x, y, z; uint32_t color; float uv[8]; };
        const uint16_t indices[] = {0, 1, 2, 3};
        const uint32_t texels[2] = {0xffff0000u, 0xff00ff00u};
        const uint32_t xyzw = 0x1bu;
        // Left quad: v10.x > 0, so lit.z = 0.75 picks green. Right: lit.z = 0 keeps red.
        const float lit_x[2] = {0.5f, -0.5f};
        for (unsigned q = 0; q < 2 && passed; ++q) {
            Vertex quad[4]{};
            for (unsigned i = 0; i < 4; ++i)
                quad[i] = {-1.0f + q + (i & 1), 1.0f - 2.0f * (i >> 1), 0.5f, 0xffffffffu,
                    {0, 0, lit_x[q], 0.75f, 0.25f, 0.75f, 0, 0}};
            RecompD3dPresenterCommand command{RECOMP_D3D_PRESENTER_COMMAND_DRAW};
            auto &draw = command.data.draw;
            draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
            draw.index_count = draw.vertex_count = 4;
            draw.triangle_count = 2;
            draw.vertex_stride = sizeof(Vertex);
            draw.fvf = 0x442u;
            draw.vertex_bytes = quad;
            draw.index_bytes = indices;
            draw.blend.color_write_mask = 15;
            draw.address_u = draw.address_v = 3;
            draw.has_texture = true;
            draw.texture.format_byte = RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8;
            draw.texture.bits_per_pixel = 32;
            draw.texture.width = 2;
            draw.texture.height = 1;
            draw.texture.mip_levels = 1;
            draw.texture.data = 0x1000u;
            draw.texture_bytes = texels;
            draw.texture_byte_count = sizeof texels;
            draw.program_count = 5;
            // oPos = v0; oD0 = v3; r0 = lit(v10); r2 = sge(v11, c3); oT0 = r0.zwxx * r2.yyyy.
            draw.program[0][1] = (1u << 21) | xyzw;
            draw.program[0][2] = 2u << 26;
            draw.program[0][3] = (15u << 12) | 2048u;
            draw.program[1][1] = (1u << 21) | (3u << 9) | xyzw;
            draw.program[1][2] = 2u << 26;
            draw.program[1][3] = (15u << 12) | (3u << 3) | 2048u;
            draw.program[2][1] = (7u << 25) | (10u << 9);
            draw.program[2][2] = xyzw << 2;
            draw.program[2][3] = (2u << 28) | (15u << 16);
            draw.program[3][1] = (12u << 21) | (11u << 9) | (3u << 13) | xyzw;
            draw.program[3][2] = (2u << 26) | (3u << 11) | (xyzw << 17);
            draw.program[3][3] = (2u << 20) | (15u << 24);
            draw.program[4][1] = (2u << 21) | 0xb0u;
            draw.program[4][2] = (1u << 26) | (1u << 11) | (2u << 13) | (0x55u << 17);
            draw.program[4][3] = (15u << 12) | (9u << 3) | 2048u | 1u;
            const float c3[4] = {0.5f, 0.5f, 0.5f, 0.5f};
            std::memcpy(draw.program_constants[3], c3, sizeof c3);
            draw.program_constants[58][0] = draw.program_constants[58][1] = draw.program_constants[58][2] = 1;
            passed &= expect("lit program draw", d3d11_backend_submit(presenter, &command));
        }
        RecompD3dPresenterCommand present{RECOMP_D3D_PRESENTER_COMMAND_PRESENT};
        present.data.present = {5, 1};
        passed &= expect("lit present", d3d11_backend_submit(presenter, &present));
        passed &= expect("destroy lit", d3d11_backend_destroy(&presenter));
        if (FILE *file = std::fopen(path, "rb")) {
            const unsigned xs[] = {80, 240};
            for (unsigned i = 0; i < 2; ++i) {
                unsigned char bgr[3]{};
                std::fseek(file, 54 + (239 - 120) * 960 + xs[i] * 3, SEEK_SET);
                passed &= std::fread(bgr, 1, 3, file) == 3;
                const bool pixel_ok = bgr[0] < 4 && (i == 0 ? bgr[1] > 250 && bgr[2] < 4
                    : bgr[2] > 250 && bgr[1] < 4);
                if (!pixel_ok) std::fprintf(stderr, "FAIL program lit/sge pixel %u BGR=%u,%u,%u\n",
                    i, bgr[0], bgr[1], bgr[2]);
                passed &= pixel_ok;
            }
            std::fclose(file);
        } else passed = false;
    }
    _putenv_s("RECOMP_D3D_FRAME_DUMP", "");
    DeleteFileA(path);
    if (passed) std::puts("PASS vertex-program LIT/SGE over a four-UV stream");
    return passed;
}

// Stage-effect bump pass: t2 is read at oT2 + M2 n with n the signed V8U8 map
// texel, masked by 1 - t3.a^4 and blended toward the fog color by oFog.
static bool testStageBump()
{
    char folder[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, folder) || !GetTempFileNameA(folder, "bmp", 0, path)) return false;
    _putenv_s("RECOMP_D3D_FRAME_DUMP", path);
    RecompD3dPresenter *presenter = nullptr;
    bool passed = expect("create bump", d3d11_backend_create(&config, &presenter));
    if (passed) {
        auto clear = clearCommand();
        clear.data.clear.color = 0xff000000u;
        passed &= expect("bump clear", d3d11_backend_submit(presenter, &clear));
        struct Vertex { float x, y, z; uint32_t color; float uv[8]; };
        const uint16_t indices[] = {0, 1, 2, 3};
        const uint32_t scene[2] = {0xffff0000u, 0xff00ff00u}, mask = 0x80ffffffu;
        // U = +64 and V = -64: u moves from 0.25 to 0.75 (green) only if U is signed and first.
        const uint8_t map[2] = {0x40u, 0xc0u};
        const uint32_t xyzw = 0x1bu;
        for (unsigned q = 0; q < 2 && passed; ++q) {
            Vertex quad[4]{};
            for (unsigned i = 0; i < 4; ++i)
                quad[i] = {-1.0f + q + (i & 1), 1.0f - 2.0f * (i >> 1), 0.5f, 0xffffffffu,
                    {0, 0, 0, 0, 0.25f, 0.5f, 0.5f, 0.5f}};
            RecompD3dPresenterCommand command{RECOMP_D3D_PRESENTER_COMMAND_DRAW};
            auto &draw = command.data.draw;
            draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
            draw.index_count = draw.vertex_count = 4;
            draw.triangle_count = 2;
            draw.vertex_stride = sizeof(Vertex);
            draw.fvf = 0x442u;
            draw.vertex_bytes = quad;
            draw.index_bytes = indices;
            draw.blend.color_write_mask = 15;
            draw.address_u = draw.address_v = 3;
            draw.has_texture = true;
            draw.texture.format_byte = RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8;
            draw.texture.bits_per_pixel = 32;
            draw.texture.width = 2;
            draw.texture.height = 1;
            draw.texture.mip_levels = 1;
            draw.texture.data = 0x1000u;
            draw.texture_bytes = scene;
            draw.texture_byte_count = sizeof scene;
            draw.reflection_texture = draw.texture;
            draw.reflection_texture.width = 1;
            draw.reflection_texture.data = 0x2000u;
            draw.reflection_bytes = &mask;
            draw.reflection_byte_count = sizeof mask;
            draw.reflection_address_u = draw.reflection_address_v = 3;
            draw.alpha_mask.format_byte = 0x28u;
            draw.alpha_mask.bits_per_pixel = 16;
            draw.alpha_mask.width = draw.alpha_mask.height = draw.alpha_mask.mip_levels = 1;
            draw.alpha_mask.data = 0x3000u;
            draw.alpha_mask_bytes = map;
            draw.alpha_mask_byte_count = sizeof map;
            draw.bump_address_u = draw.bump_address_v = 1;
            draw.bump_env = true;
            draw.bump_matrices[0][0] = 1.0f;
            // Right quad: f = 0 + 1 * oFog = 0.25 toward blue fog.
            draw.bump_fog[1] = 1.0f;
            draw.bump_fog[2] = q ? 1.0f : 0.0f;
            draw.fog_color = 0xff0000ffu;
            // oPos = v0, oD0 = v3, oT0 = v9, oT2 = v11, oT3 = v12, oFog = c5.
            const uint32_t moves[6][2] = {{0u, 0u}, {3u, 3u}, {9u, 9u}, {11u, 11u}, {12u, 12u}, {5u, 5u}};
            draw.program_count = 6;
            for (unsigned i = 0; i < 6; ++i) {
                const bool constant = i == 5;
                draw.program[i][1] = (1u << 21) | (constant ? moves[i][0] << 13 : moves[i][0] << 9) | xyzw;
                draw.program[i][2] = (constant ? 3u : 2u) << 26;
                draw.program[i][3] = (15u << 12) | (moves[i][1] << 3) | 2048u | (constant ? 1u : 0u);
            }
            draw.program_constants[5][0] = 0.25f;
            draw.program_constants[58][0] = draw.program_constants[58][1] = draw.program_constants[58][2] = 1;
            passed &= expect("bump draw", d3d11_backend_submit(presenter, &command));
        }
        RecompD3dPresenterCommand present{RECOMP_D3D_PRESENTER_COMMAND_PRESENT};
        present.data.present = {5, 1};
        passed &= expect("bump present", d3d11_backend_submit(presenter, &present));
        passed &= expect("destroy bump", d3d11_backend_destroy(&presenter));
        if (FILE *file = std::fopen(path, "rb")) {
            // 1 - (128/255)^4 keeps 239 of 255; fog 0.25 leaves 60 green and 191 blue.
            const unsigned xs[] = {80, 240}, blue[] = {0, 191}, green[] = {239, 60};
            for (unsigned i = 0; i < 2; ++i) {
                unsigned char bgr[3]{};
                std::fseek(file, 54 + (239 - 120) * 960 + xs[i] * 3, SEEK_SET);
                passed &= std::fread(bgr, 1, 3, file) == 3;
                const bool pixel_ok = std::abs(bgr[0] - int(blue[i])) <= 3 &&
                    std::abs(bgr[1] - int(green[i])) <= 3 && bgr[2] < 4;
                if (!pixel_ok) std::fprintf(stderr, "FAIL stage bump pixel %u BGR=%u,%u,%u\n",
                    i, bgr[0], bgr[1], bgr[2]);
                passed &= pixel_ok;
            }
            std::fclose(file);
        } else passed = false;
    }
    _putenv_s("RECOMP_D3D_FRAME_DUMP", "");
    DeleteFileA(path);
    if (passed) std::puts("PASS stage-effect bump: signed V8U8 offsets, t3.a^4 mask and linear fog");
    return passed;
}

// Water bump pass: t1 at oT1 + M1 n, s = dot(expand oD0, expand t3)^8, then
// r0 = s + oD1 t1.a - 1 + t3.a t1.
static bool testStageWater()
{
    char folder[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, folder) || !GetTempFileNameA(folder, "wtr", 0, path)) return false;
    _putenv_s("RECOMP_D3D_FRAME_DUMP", path);
    RecompD3dPresenter *presenter = nullptr;
    bool passed = expect("create water", d3d11_backend_create(&config, &presenter));
    if (passed) {
        auto clear = clearCommand();
        clear.data.clear.color = 0xff000000u;
        passed &= expect("water clear", d3d11_backend_submit(presenter, &clear));
        struct Vertex { float x, y, z; uint32_t color; float uv[8]; };
        const uint16_t indices[] = {0, 1, 2, 3};
        // Opaque red at u = 0.25; half-alpha green where the +64 U offset lands.
        const uint32_t reflection[2] = {0xffff0000u, 0x8000ff00u}, environment = 0x808080ffu;
        const uint8_t map[2] = {0x40u, 0x00u};
        const uint32_t xyzw = 0x1bu;
        Vertex quad[4]{};
        for (unsigned i = 0; i < 4; ++i)
            quad[i] = {-1.0f + 2.0f * (i & 1), 1.0f - 2.0f * (i >> 1), 0.5f, 0xff8080ffu,
                {0, 0, 0.25f, 0.5f, 0, 0, 0.5f, 0.5f}};
        RecompD3dPresenterCommand command{RECOMP_D3D_PRESENTER_COMMAND_DRAW};
        auto &draw = command.data.draw;
        draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
        draw.index_count = draw.vertex_count = 4;
        draw.triangle_count = 2;
        draw.vertex_stride = sizeof(Vertex);
        draw.fvf = 0x442u;
        draw.vertex_bytes = quad;
        draw.index_bytes = indices;
        draw.blend.color_write_mask = 15;
        draw.address_u = draw.address_v = 3;
        draw.has_texture = true;
        draw.texture.format_byte = RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8;
        draw.texture.bits_per_pixel = 32;
        draw.texture.width = 2;
        draw.texture.height = 1;
        draw.texture.mip_levels = 1;
        draw.texture.data = 0x1000u;
        draw.texture_bytes = reflection;
        draw.texture_byte_count = sizeof reflection;
        draw.reflection_texture = draw.texture;
        draw.reflection_texture.width = 1;
        draw.reflection_texture.data = 0x2000u;
        draw.reflection_bytes = &environment;
        draw.reflection_byte_count = sizeof environment;
        draw.reflection_address_u = draw.reflection_address_v = 3;
        draw.alpha_mask.format_byte = 0x28u;
        draw.alpha_mask.bits_per_pixel = 16;
        draw.alpha_mask.width = draw.alpha_mask.height = draw.alpha_mask.mip_levels = 1;
        draw.alpha_mask.data = 0x3000u;
        draw.alpha_mask_bytes = map;
        draw.alpha_mask_byte_count = sizeof map;
        draw.bump_address_u = draw.bump_address_v = 1;
        draw.bump_env = draw.bump_water = true;
        draw.bump_matrices[0][0] = draw.bump_matrices[0][3] = 1.0f;
        // oPos = v0, oD0 = v3, oT0 = v9, oT1 = v10, oT3 = v12, oD1 = c6.
        const uint32_t moves[6][2] = {{0u, 0u}, {3u, 3u}, {9u, 9u}, {10u, 10u}, {12u, 12u}, {6u, 4u}};
        draw.program_count = 6;
        for (unsigned i = 0; i < 6; ++i) {
            const bool constant = i == 5;
            draw.program[i][1] = (1u << 21) | (constant ? moves[i][0] << 13 : moves[i][0] << 9) | xyzw;
            draw.program[i][2] = (constant ? 3u : 2u) << 26;
            draw.program[i][3] = (15u << 12) | (moves[i][1] << 3) | 2048u | (constant ? 1u : 0u);
        }
        draw.program_constants[6][0] = 0.5f;
        draw.program_constants[58][0] = draw.program_constants[58][1] = draw.program_constants[58][2] = 1;
        passed &= expect("water draw", d3d11_backend_submit(presenter, &command));
        RecompD3dPresenterCommand present{RECOMP_D3D_PRESENTER_COMMAND_PRESENT};
        present.data.present = {5, 1};
        passed &= expect("water present", d3d11_backend_submit(presenter, &present));
        passed &= expect("destroy water", d3d11_backend_destroy(&presenter));
        if (FILE *file = std::fopen(path, "rb")) {
            // s = 1, t1 = green at 0.502 alpha, t3.a = 0.502: r0 = (0.25, 0.5, 0).
            unsigned char bgr[3]{};
            std::fseek(file, 54 + (239 - 120) * 960 + 160 * 3, SEEK_SET);
            passed &= std::fread(bgr, 1, 3, file) == 3;
            const bool pixel_ok = bgr[0] < 4 && std::abs(bgr[1] - 128) <= 4 && std::abs(bgr[2] - 64) <= 4;
            if (!pixel_ok) std::fprintf(stderr, "FAIL stage water pixel BGR=%u,%u,%u\n", bgr[0], bgr[1], bgr[2]);
            passed &= pixel_ok;
            std::fclose(file);
        } else passed = false;
    }
    _putenv_s("RECOMP_D3D_FRAME_DUMP", "");
    DeleteFileA(path);
    if (passed) std::puts("PASS stage water bump: offset reflection, oD1 and dot^8 highlight");
    return passed;
}

static bool testClose(bool idle)
{
    RecompD3dPresenter *presenter = nullptr;
    if (!expect("create close", recomp_d3d_presenter_create(&config, &presenter))) return false;
    const HWND window = presenterWindow();
    bool passed = window != nullptr;
    DWORD owner = GetWindowThreadProcessId(window, nullptr);
    passed &= owner != 0 && owner != GetCurrentThreadId();
    if (passed) passed = PostMessageW(window, WM_CLOSE, 0, 0) != 0;
    if (passed && idle) {
        const ULONGLONG deadline = GetTickCount64() + 2000;
        while (IsWindow(window) && GetTickCount64() < deadline) Sleep(1);
        passed = !IsWindow(window);
    }
    RecompD3dPresenterError error = RECOMP_D3D_PRESENTER_OK;
    uint32_t ticks = 0;
    while (passed && error == RECOMP_D3D_PRESENTER_OK && ticks < 600) {
        error = tick(presenter, ++ticks);
    }
    passed &= expect("close status", error, RECOMP_D3D_PRESENTER_CLOSED);
    passed &= expect("sticky close", recomp_d3d_presenter_submit(presenter, nullptr),
        RECOMP_D3D_PRESENTER_CLOSED);
    passed &= destroy(presenter);
    if (passed) std::printf("PASS %s close ticks=%u\n", idle ? "idle" : "queued", ticks);
    else std::fprintf(stderr, "FAIL close idle=%d window=%p owner=%lu\n", idle,
        static_cast<void *>(window), owner);
    return passed;
}

// A far quad behind a near quad covering the left half counts only the right half.
static bool testVisibility()
{
    RecompD3dPresenter *presenter = nullptr;
    if (!expect("create visibility", recomp_d3d_presenter_create(&config, &presenter))) return false;
    struct Vertex { float x, y, z, rhw; uint32_t diffuse; };
    const uint16_t indices[] = {0, 1, 2, 3};
    // The occluder's right edge avoids pixel centers with or without a half-pixel shift.
    const Vertex near_quad[] = {{-10, -10, 0.25f, 1, ~0u}, {159.9f, -10, 0.25f, 1, ~0u},
        {-10, 250, 0.25f, 1, ~0u}, {159.9f, 250, 0.25f, 1, ~0u}};
    const Vertex far_quad[] = {{-10, -10, 0.5f, 1, ~0u}, {330, -10, 0.5f, 1, ~0u},
        {-10, 250, 0.5f, 1, ~0u}, {330, 250, 0.5f, 1, ~0u}};
    RecompD3dPresenterCommand draw_command{RECOMP_D3D_PRESENTER_COMMAND_DRAW};
    auto &draw = draw_command.data.draw;
    draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
    draw.index_count = draw.vertex_count = 4;
    draw.triangle_count = 2;
    draw.vertex_stride = sizeof(Vertex);
    draw.fvf = 0x44u;
    draw.index_bytes = indices;
    draw.blend.color_write_mask = 15;
    draw.depth.depth_test_enable = true;
    draw.depth.depth_write_enable = true;
    draw.depth.depth_func = RECOMP_D3D_COMPARE_LESS_EQUAL;
    draw.vertex_bytes = near_quad;
    RecompD3dPresenterCommand begin{RECOMP_D3D_PRESENTER_COMMAND_VISIBILITY_BEGIN};
    RecompD3dPresenterCommand end{RECOMP_D3D_PRESENTER_COMMAND_VISIBILITY_END};
    auto clear = clearCommand();
    bool passed = expect("visibility clear", recomp_d3d_presenter_submit(presenter, &clear));
    passed &= expect("occluder", recomp_d3d_presenter_submit(presenter, &draw_command));
    draw.vertex_bytes = far_quad;
    draw.depth.depth_write_enable = false;
    for (uint32_t index : {7u, 8u}) {
        draw.depth.depth_test_enable = index == 7u;
        end.data.visibility_index = index;
        passed &= expect("visibility begin", recomp_d3d_presenter_submit(presenter, &begin));
        passed &= expect("counted draw", recomp_d3d_presenter_submit(presenter, &draw_command));
        passed &= expect("visibility end", recomp_d3d_presenter_submit(presenter, &end));
    }
    bool complete = true;
    uint32_t pixels[2]{};
    passed &= expect("queued result", recomp_d3d_presenter_visibility_result(presenter, 7u, &complete, &pixels[0]));
    passed &= !complete;
    passed &= expect("unknown index", recomp_d3d_presenter_visibility_result(presenter, 9u, &complete, &pixels[0]),
        RECOMP_D3D_PRESENTER_INVALID_ARGUMENT);
    passed &= expect("publish visibility", recomp_d3d_presenter_flush(presenter, false));
    const ULONGLONG deadline = GetTickCount64() + 5000u;
    for (uint32_t i = 0; i < 2u && passed; ++i) {
        complete = false;
        while (passed && !complete && GetTickCount64() < deadline) {
            passed &= expect("poll visibility", recomp_d3d_presenter_visibility_result(
                presenter, 7u + i, &complete, &pixels[i]));
        }
        passed &= complete;
    }
    passed &= pixels[0] == 160u * 240u && pixels[1] == 320u * 240u;
    // An END the worker cannot match to a BEGIN fails the presenter.
    passed &= expect("unbegun end", recomp_d3d_presenter_submit(presenter, &end));
    passed &= expect("unbegun end fails", recomp_d3d_presenter_flush(presenter, true),
        RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND);
    recomp_d3d_presenter_destroy(&presenter);
    if (passed) std::puts("PASS occlusion counts partially hidden and unoccluded quads");
    else std::fprintf(stderr, "FAIL visibility complete=%d pixels=%u,%u\n", complete, pixels[0], pixels[1]);
    return passed;
}

int main()
{
    recomp_d3d_presenter_set_immediate_present(false);
    if (!testTicks() || !testCapturedDraw() || !testMipSampler() || !testLines() || !testLighting() ||
        !testReflectionAddress(false) || !testReflectionAddress(true) || !testReflectionAddress(true, true) ||
        !testReflectionAddress(false, false, true) || !testReflectionAddress(true, false, false, true) ||
        !testReflectionAddress(false, false, false, false, 0.0f) ||
        !testReflectionAddress(false, false, false, false, 0.5f) ||
        !testReflectionAddress(false, false, false, false, 1.0f) ||
        !testScreenSpaceOffsetAndScissor() || !testBrightPass() || !testDistortion() || !testStageEffect() ||
        !testProgramMaxSlt() ||
        !testProgramLitSge() ||
        !testStageBump() ||
        !testStageWater() ||
        !testVisibility() ||
        !testClose(false) || !testClose(true)) return 1;
    std::puts("PASS presenter render thread");
    return 0;
}
