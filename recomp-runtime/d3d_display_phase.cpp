#include "d3d_display_phase.h"
#include "d3d_vblank.h"

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && TARGET_OS_OSX

#pragma clang diagnostic push
// CVDisplayLink is deprecated from macOS 15 for NSView/NSScreen display links, which need Objective-C; it still works.
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

#include <ApplicationServices/ApplicationServices.h>
#include <CoreVideo/CoreVideo.h>
#include <mach/mach_time.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

namespace {

constexpr double kGameFrameNs = 1e9 / 60.0;
constexpr double kTarget = 0.3;        // where in a refresh half the frames should end
constexpr long long kStepNs = 20000;   // the game's clock moved this much a frame

bool enabled()
{
    static const bool on = [] {
        const char *value = std::getenv("RECOMP_DISPLAY_PHASE");
        return value == nullptr || std::strcmp(value, "0") != 0;
    }();
    return on;
}

bool reporting()
{
    static const bool on = std::getenv("RECOMP_PERF_COUNTER") != nullptr;
    return on;
}

int64_t hostNs(uint64_t host)
{
    static const mach_timebase_info_data_t base = [] {
        mach_timebase_info_data_t info{};
        mach_timebase_info(&info);
        return info;
    }();
    return static_cast<int64_t>(static_cast<__int128>(host) * base.numer / base.denom);
}

CVDisplayLinkRef display_link;
CGDirectDisplayID followed;
unsigned follow_calls;

// The display's latest refresh (host ns), how many there have been and the time between two.
std::atomic<int64_t> refresh_ns{0};
std::atomic<uint64_t> refresh_count{0};
std::atomic<int64_t> period_ns{0};

std::mutex queue_lock;
std::condition_variable queue_changed;
std::deque<std::pair<SDL_GPUDevice *, SDL_GPUFence *>> fences;
std::thread waiter;
bool stopping;

CVReturn onRefresh(CVDisplayLinkRef, const CVTimeStamp *, const CVTimeStamp *out, CVOptionFlags, CVOptionFlags *, void *)
{
    if (out->videoTimeScale != 0 && out->videoRefreshPeriod != 0) {
        period_ns.store(static_cast<int64_t>(
            static_cast<double>(out->videoRefreshPeriod) * 1e9 / out->videoTimeScale), std::memory_order_relaxed);
    }
    refresh_ns.store(hostNs(out->hostTime), std::memory_order_relaxed);
    refresh_count.fetch_add(1u, std::memory_order_release);
    return kCVReturnSuccess;
}

// Refreshes to a game frame when the display's rate is a whole multiple of 60 (59.94 too), else 0.
int refreshesPerFrame(int64_t period)
{
    if (period <= 0) return 0;
    const int whole = static_cast<int>(std::lround(kGameFrameNs / period));
    if (whole < 1 || whole > 4) return 0;
    return std::fabs(whole * static_cast<double>(period) - kGameFrameNs) < 100000.0 ? whole : 0;
}

struct Steering {
    double late = 0.5;          // filtered: the share of frames ending past kTarget
    uint64_t previous = 0u;     // the refresh the last frame went up at
    unsigned frames = 0u, shown[6] = {}, phases[10] = {};
};
Steering steering;

// A frame's work ended at end_ns: the next refresh shows it. Moves the game's clock towards the middle.
void frameEnded(int64_t end_ns)
{
    const int64_t period = period_ns.load(std::memory_order_relaxed);
    const uint64_t count = refresh_count.load(std::memory_order_acquire);
    const int64_t refresh = refresh_ns.load(std::memory_order_relaxed);
    const int whole = refreshesPerFrame(period);
    if (whole == 0 || count == 0u) return;
    const double since = static_cast<double>(end_ns - refresh) / period;
    const double floor_since = std::floor(since);
    const uint64_t shown_at = count + static_cast<int64_t>(floor_since) + 1;
    const double phase = since - floor_since;   // 0 just after a refresh, 1 just before the next
    const uint64_t refreshes = steering.previous == 0u ? 0u : shown_at - steering.previous;
    steering.previous = shown_at;
    ++steering.phases[std::min(9, static_cast<int>(phase * 10.0))];
    {
        ++steering.frames;
        ++steering.shown[std::min<uint64_t>(refreshes, 5u)];
    }
    // A frame long missing (a load, a pause) says nothing of the phase.
    if (refreshes != 0u && (refreshes + 1u < static_cast<uint64_t>(whole) || refreshes > static_cast<uint64_t>(whole) + 1u))
        return;
    // Frames mostly end at about the same point with a long tail of late ones, so the median is steered to 30%
    // into a refresh (a frame may then end 70% of a refresh late and still be shown on time). Just past a
    // refresh counts as late too: a frame that ended a little after the refresh it was meant for.
    const bool late = phase > kTarget || phase < kTarget / 3.0;
    steering.late += ((late ? 1.0 : 0.0) - steering.late) * 0.02;
    recomp_d3d_vblank_nudge(late ? -kStepNs : kStepNs);
    if (reporting() && steering.frames >= 600u) {
        std::fprintf(stderr, "recomp display phase: %.2f Hz, %d refreshes a frame, %.0f%% of frames ending past %.0f%% of a refresh;"
            " frames shown for", 1e9 / period, whole, steering.late * 100.0, kTarget * 100.0);
        for (unsigned i = 0u; i < 6u; ++i)
            if (steering.shown[i] != 0u) std::fprintf(stderr, " %u%s=%u", i, i == 5u ? "+" : "", steering.shown[i]);
        std::fputs(" refreshes; phase tenths", stderr);
        for (unsigned i = 0u; i < 10u; ++i) std::fprintf(stderr, " %u", steering.phases[i]);
        std::fputc('\n', stderr);
        std::memset(steering.phases, 0, sizeof steering.phases);
        steering.frames = 0u;
        std::memset(steering.shown, 0, sizeof steering.shown);
    }
}

void waitForFrames()
{
    for (;;) {
        std::pair<SDL_GPUDevice *, SDL_GPUFence *> next;
        {
            std::unique_lock<std::mutex> lock(queue_lock);
            queue_changed.wait(lock, [] { return stopping || !fences.empty(); });
            if (fences.empty()) return;
            next = fences.front();
            fences.pop_front();
        }
        SDL_WaitForGPUFences(next.first, true, &next.second, 1u);
        const int64_t ended = hostNs(mach_absolute_time());
        SDL_ReleaseGPUFence(next.first, next.second);
        frameEnded(ended);
    }
}

} // namespace

void recomp_display_phase_follow(SDL_Window *window)
{
    if (!enabled() || window == nullptr) return;
    if (display_link != nullptr && follow_calls++ % 120u != 0u) return;
    int x = 0, y = 0, width = 0, height = 0;
    SDL_GetWindowPosition(window, &x, &y);
    SDL_GetWindowSize(window, &width, &height);
    CGDirectDisplayID display = CGMainDisplayID();
    uint32_t found = 0u;
    CGGetDisplaysWithPoint(CGPointMake(x + width / 2.0, y + height / 2.0), 1u, &display, &found);
    if (found == 0u) display = CGMainDisplayID();
    if (display_link != nullptr && display == followed) return;
    if (display_link == nullptr) {
        if (CVDisplayLinkCreateWithCGDisplay(display, &display_link) != kCVReturnSuccess) {
            display_link = nullptr;
            return;
        }
        CVDisplayLinkSetOutputCallback(display_link, onRefresh, nullptr);
    } else {
        CVDisplayLinkStop(display_link);
        CVDisplayLinkSetCurrentCGDisplay(display_link, display);
        refresh_count.store(0u, std::memory_order_release);
        steering = Steering{};
    }
    followed = display;
    CVDisplayLinkStart(display_link);
    if (!waiter.joinable()) {
        stopping = false;
        waiter = std::thread(waitForFrames);
    }
}

bool recomp_display_phase_submit(SDL_GPUDevice *device, SDL_GPUCommandBuffer *commands)
{
    if (display_link == nullptr || !waiter.joinable() ||
        refreshesPerFrame(period_ns.load(std::memory_order_relaxed)) == 0) return false;
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
    if (fence != nullptr) {
        std::lock_guard<std::mutex> lock(queue_lock);
        fences.emplace_back(device, fence);
        queue_changed.notify_one();
    }
    return true;
}

void recomp_display_phase_stop(void)
{
    if (waiter.joinable()) {
        {
            std::lock_guard<std::mutex> lock(queue_lock);
            stopping = true;
            queue_changed.notify_one();
        }
        waiter.join();
    }
    if (display_link != nullptr) {
        CVDisplayLinkStop(display_link);
        CVDisplayLinkRelease(display_link);
        display_link = nullptr;
    }
    steering = Steering{};
}

#pragma clang diagnostic pop

#else

void recomp_display_phase_follow(SDL_Window *) {}
bool recomp_display_phase_submit(SDL_GPUDevice *, SDL_GPUCommandBuffer *) { return false; }
void recomp_display_phase_stop(void) {}

#endif
