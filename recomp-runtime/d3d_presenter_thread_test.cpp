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
        for (unsigned i = 0; i < 14 && passed; ++i) {
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
            const bool vertex_diffuse = i >= 9 && i != 11;
            const uint32_t base_pixel = vertex_diffuse ? 0xc080e0ffu : 0x800000ffu;
            const uint32_t environment_pixel = i == 12 ? 0u : i == 13 ? 0xff804020u
                : vertex_diffuse ? 0x80ff8000u : 0x40ff0000u;
            if (i >= 8) {
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
                {33, 83, 44}, {46, 10, 48}, {36, 42, 12}, {12, 32, 32}};
            // SRC_ALPHA over black checks reflection alpha as well as RGB. PM clamps before diffuse.
            for (unsigned i = 0; i < 14; ++i) {
                unsigned char bgr[3]{};
                const unsigned x = i >= 8 ? (160 * (i - 8) + 80) / 3 : 40 + 80 * (i % 4);
                const unsigned y = i >= 8 ? 120 : 60 + 120 * (i / 4);
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

int main()
{
    recomp_d3d_presenter_set_immediate_present(false);
    if (!testTicks() || !testCapturedDraw() || !testMipSampler() || !testLighting() ||
        !testScreenSpaceOffsetAndScissor() || !testClose(false) || !testClose(true)) return 1;
    std::puts("PASS presenter render thread");
    return 0;
}
