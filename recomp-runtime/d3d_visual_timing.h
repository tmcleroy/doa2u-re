// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>

namespace recomp_visual {
constexpr int64_t history_ns = 16666667;
inline int64_t now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
// Packet production time is presentation metadata, independent of diagnostic
// tracing. The renderer thread carries it through execution into owned snapshots.
inline thread_local int64_t executing_produced_ns = 0;
inline float interpolationAlpha(int64_t render_ns, int64_t previous_ns, int64_t current_ns) {
    if (previous_ns <= 0 || current_ns <= previous_ns) return 1.0f;
    // Sample one tick behind the host timeline. Preparation and queue delays
    // advance this timeline; they must not restart a new interpolation period.
    const double target = double(render_ns - previous_ns) - double(history_ns);
    return float(std::clamp(target / double(current_ns - previous_ns), 0.0, 1.0));
}
}
