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
    if (!testTicks() || !testCapturedDraw() || !testMipSampler() || !testClose(false) || !testClose(true)) return 1;
    std::puts("PASS presenter render thread");
    return 0;
}
