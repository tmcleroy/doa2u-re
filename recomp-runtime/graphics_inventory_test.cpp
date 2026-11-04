// SPDX-License-Identifier: GPL-3.0-or-later
#include "d3d_presenter_backend.h"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <thread>
#include <chrono>
#include <SDL3/SDL.h>
extern "C" void recomp_kernel_wait_for_vblank(uint64_t) {}
int main(int argc,char **argv) {
    if (argc!=2 && !(argc==3 && std::strcmp(argv[2],"--cancel")==0)) { std::fprintf(stderr,"usage: recomp_graphics_inventory_test CACHE_DIRECTORY [--cancel]\n");return 2; }
#ifdef _WIN32
    _putenv_s("RECOMP_GRAPHICS_INVENTORY_STRICT","1");
    _putenv_s("RECOMP_GRAPHICS_CATALOG",argv[1]);
    _putenv_s("RECOMP_SHADER_CACHE",argv[1]);
#else
    setenv("RECOMP_GRAPHICS_INVENTORY_STRICT","1",1);
    setenv("RECOMP_GRAPHICS_CATALOG",argv[1],1);
    setenv("RECOMP_SHADER_CACHE",argv[1],1);
#endif
    const RecompD3dPresenterConfig config{640,480,RECOMP_D3D_PRESENTER_COLOR_FORMAT_BGRA8_UNORM,RECOMP_D3D_PRESENTER_DEPTH_FORMAT_D24S8};
    RecompD3dPresenter *presenter=nullptr;
    const bool cancel=argc==3;
    std::thread closer;
    if (cancel) closer=std::thread([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        SDL_Event event{};event.type=SDL_EVENT_QUIT;SDL_PushEvent(&event);
    });
    const auto result=d3d_backend_create(&config,&presenter);
    if (closer.joinable()) closer.join();
    if (cancel) {
        if (presenter) d3d_backend_destroy(&presenter);
        return result!=RECOMP_D3D_PRESENTER_OK?0:1;
    }
    if (result!=RECOMP_D3D_PRESENTER_OK) return 1;
    const auto destroyed=d3d_backend_destroy(&presenter);
    std::puts("Offline preparation completed without loading or executing the game.");
    return destroyed==RECOMP_D3D_PRESENTER_OK?0:1;
}
// This preparation-only executable has no input overlay or guest kernel.
extern "C" bool sc2_touch_shown(void) { return false; }
extern "C" bool sc2_touch_menu(void) { return false; }
extern "C" void sc2_touch_draw(float,float) {}
