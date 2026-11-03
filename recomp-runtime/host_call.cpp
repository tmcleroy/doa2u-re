/* Android: recomp_host_call's thread (host_call.h). One thread, started at the first call, runs each call
   while its caller waits, so the calls stay in order and never overlap. */
#include "host_call.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <chrono>

#include <condition_variable>
#include <mutex>
#include <thread>

namespace {

std::mutex lock;
std::condition_variable changed;
void (*pending)(void *) = nullptr;
void *pending_data = nullptr;
bool done = false;
std::mutex callers; /* (one call at a time) */

void serve()
{
    std::unique_lock<std::mutex> hold(lock);
    for (;;) {
        changed.wait(hold, [] { return pending != nullptr; });
        void (*fn)(void *) = pending;
        void *data = pending_data;
        hold.unlock();
        fn(data);
        hold.lock();
        pending = nullptr;
        done = true;
        changed.notify_all();
    }
}

} // namespace

extern "C" void recomp_host_call(void (*fn)(void *), void *data)
{
    static std::once_flag started;
    std::call_once(started, [] { std::thread(serve).detach(); });
    std::lock_guard<std::mutex> one(callers);
    std::unique_lock<std::mutex> hold(lock);
    pending = fn;
    pending_data = data;
    done = false;
    changed.notify_all();
    changed.wait(hold, [] { return done; });
}

namespace {

std::mutex front_lock;
std::condition_variable front_changed;
bool background = false;

bool SDLCALL watchFront(void *, SDL_Event *event)
{
    if (event->type == SDL_EVENT_WILL_ENTER_BACKGROUND || event->type == SDL_EVENT_DID_ENTER_FOREGROUND) {
        std::lock_guard<std::mutex> hold(front_lock);
        background = event->type == SDL_EVENT_WILL_ENTER_BACKGROUND;
        front_changed.notify_all();
    }
    return true;
}

} // namespace

extern "C" void recomp_host_pump_start(void)
{
    static std::once_flag started;
    std::call_once(started, [] {
        SDL_AddEventWatch(watchFront, nullptr);
        std::thread([] {
            for (;;) {
                SDL_PumpEvents();     // (in the background SDL holds it here until the app is back)
                SDL_UpdateGamepads();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }).detach();
    });
}

extern "C" void recomp_host_wait_foreground(void)
{
    std::unique_lock<std::mutex> hold(front_lock);
    front_changed.wait(hold, [] { return !background; });
}
