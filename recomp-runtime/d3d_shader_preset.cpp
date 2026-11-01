// SPDX-License-Identifier: GPL-3.0-or-later
#include "d3d_shader_preset.h"

#include <atomic>
#include <cstdlib>
#include <mutex>

namespace {

std::mutex lock;
std::string asked;
unsigned generation = 1u;
std::atomic<int> state{0};

} // namespace

extern "C" void recomp_d3d_set_shader_preset(const char *path)
{
    std::lock_guard<std::mutex> hold(lock);
    const std::string next = path != nullptr ? path : "";
    if (next == asked) return;
    asked = next;
    ++generation;
}

extern "C" int recomp_d3d_shader_state(void)
{
    return state.load();
}

std::string recomp_d3d_shader_preset(unsigned &asked_generation)
{
    static const char *fixed = std::getenv("RECOMP_SHADER_PRESET");
    std::lock_guard<std::mutex> hold(lock);
    asked_generation = generation;
    return fixed != nullptr && fixed[0] != '\0' ? std::string(fixed) : asked;
}

void recomp_d3d_shader_report(int now)
{
    state.store(now);
}
