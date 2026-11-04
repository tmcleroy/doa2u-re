// SPDX-License-Identifier: GPL-3.0-or-later
#include "d3d_visual_timing.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <cstdio>
int main() {
    using recomp_visual::interpolationAlpha;
    constexpr int64_t tick = recomp_visual::history_ns;
    constexpr int64_t previous = 1000000000, current = previous + tick;
    const auto near = [](double a, double b) { return std::abs(a-b) < .000001; };
    assert(interpolationAlpha(current, previous, current) == 0);
    assert(near(interpolationAlpha(current+tick/2, previous, current), .5));
    assert(interpolationAlpha(current+tick, previous, current) == 1);
    assert(interpolationAlpha(current+10*tick, previous, current) == 1); // never predicts
    assert(interpolationAlpha(previous, previous, current) == 0);
    assert(interpolationAlpha(current, 0, current) == 1); // first snapshot
    assert(interpolationAlpha(current, current, current) == 1);
    assert(interpolationAlpha(current, current+1, current) == 1);
    // Preparation/queue time consumes history; it cannot restart the clock.
    assert(near(interpolationAlpha(current+5000000, previous, current), 5000000.0/tick));
    // Adjacent irregular producer intervals represent the same continuous time
    // at their shared endpoint. Delayed frames clamp rather than extrapolate.
    constexpr int64_t next = current + 23000000;
    assert(interpolationAlpha(current+tick, current, next) == 0);
    assert(near(interpolationAlpha(current+tick+11500000, current, next), .5));
    assert(interpolationAlpha(next+tick, current, next) == 1);
    double last = 0;
    for (int64_t now=previous; now<next+3*tick; now+=1000000) {
        const bool has_next = now >= next;
        const int64_t a=has_next?current:previous, b=has_next?next:current;
        const double represented = a + double(interpolationAlpha(now,a,b))*(b-a);
        assert(represented >= last); last=represented;
    }
    std::puts("PASS: production-clock interpolation, irregular intervals, missing history and no extrapolation");
}
