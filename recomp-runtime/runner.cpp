/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "host_diagnostics.h"
#include "d3d8_adapter.h"
#include "dsound_api_adapter.h"
#include "input_adapter.h"
#include "input_host_win32.h"
#include "input_pulse_source.h"
#include "runner_load.h"
#include "save_transaction.h"
#include "xbox_memory_layout.h"
extern "C" {
#include "kernel_abi.h"
#include "stop_report.h"
RecompFunction recomp_program_lookup(std::uint32_t address);
}

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>
#include <charconv>

static RecompInputPulseSource inputPulses;

static bool sampleInput(std::uint32_t port, RecompInputGamepad *gamepad)
{
    if (port != 0u) return recomp_input_host_sample(port, gamepad);
    const bool sampled = recomp_input_pulse_source_sample(&inputPulses, gamepad);
    static bool startHeld;
    const bool start = sampled && (gamepad->buttons & RECOMP_INPUT_BUTTON_START) != 0u;
    if (start != startHeld) {
        std::fprintf(stderr, "recomp input: Start %s poll=%llu\n", start ? "down" : "up",
            static_cast<unsigned long long>(inputPulses.sample_count));
        startHeld = start;
    }
    return sampled;
}

int main(int argc, char **argv)
{
    recomp_install_host_diagnostics();
    std::string xbePath, discRoot, expectStop, milestoneLog;
    recomp_input_pulse_source_init(&inputPulses, recomp_input_host_sample, 0u);
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--input-start-pulse-at" && i + 1 < argc) {
            const std::string value = argv[++i];
            std::uint64_t poll = 0u;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), poll);
            if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size() ||
                !recomp_input_pulse_source_add_poll(&inputPulses, poll)) {
                std::cerr << "recomp runner: invalid Start poll or too many pulses\n";
                return 64;
            }
            continue;
        }
        if (argument == "--input-a-pulse-at" && i + 1 < argc) {
            const std::string value = argv[++i];
            std::uint64_t poll = 0u;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), poll);
            if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size() ||
                !recomp_input_pulse_source_add_a_poll(&inputPulses, poll)) {
                std::cerr << "recomp runner: invalid A poll or too many pulses\n";
                return 64;
            }
            continue;
        }
        if ((argument != "--xbe" && argument != "--disc-root" &&
             argument != "--expect-stop" && argument != "--milestone-log") || i + 1 >= argc) {
            std::cerr << "usage: recomp_program_runner --xbe <path> [--disc-root <path>]"
                " [--expect-stop <id>] [--milestone-log <path>]"
                " [--input-start-pulse-at <poll>] [--input-a-pulse-at <poll>]\n";
            return 64;
        }
        const std::string value = argv[++i];
        if (argument == "--xbe") xbePath = value;
        else if (argument == "--disc-root") discRoot = value;
        else if (argument == "--expect-stop") expectStop = value;
        else milestoneLog = value;
    }
    if (xbePath.empty()) {
        std::cerr << "recomp runner: --xbe is required\n";
        return 64;
    }
    recomp_stop_configure(expectStop.empty() ? nullptr : expectStop.c_str(),
        milestoneLog.empty() ? nullptr : milestoneLog.c_str());
    const auto parsed = doaxbv::XbeParser().parse(std::filesystem::u8path(xbePath));
    if (!parsed.ok()) {
        std::cerr << "recomp runner: " << parsed.errors.front() << '\n';
        return 1;
    }
    if (parsed.metadata.fileSha256 != RECOMP_XBE_SHA256) {
        std::cerr << "recomp runner: XBE SHA-256 differs from the authenticated program target\n";
        return 1;
    }
    std::vector<std::uint8_t> memory(RECOMP_XBOX_RAM_SIZE, 0u);
    std::string error;
    if (!recomp_load_xbe(parsed, memory, error)) {
        std::cerr << "recomp runner: " << error << '\n';
        return 1;
    }
    const auto imageEnd = static_cast<std::uint64_t>(parsed.metadata.header.baseAddress) +
        parsed.metadata.header.sizeImage;
    if (!xbox_HeapSetImageEnd(static_cast<std::uint32_t>(imageEnd))) {
        std::cerr << "recomp runner: invalid heap image boundary\n";
        return 1;
    }
    if (discRoot.empty()) discRoot = std::filesystem::absolute(std::filesystem::u8path(xbePath)).parent_path().string();
    const auto discPath = std::filesystem::u8path(discRoot);
    std::error_code filesystemError;
    if (!std::filesystem::is_directory(discPath, filesystemError)) {
        std::cerr << "recomp runner: disc root is not a readable directory\n";
        return 1;
    }
    recomp_disc_root_path = discRoot.c_str();
    if (!recomp_save_initialize(recomp_disc_root_path)) {
        std::cerr << "recomp runner: save recovery failed; guest not started\n";
        return 2;
    }
    const RecompMemoryRegion region = {0u, memory.size(), memory.data()};
    recomp_runtime_init(&region, 1u, nullptr, 0u, nullptr, 0u);
    recomp_runtime_set_lookup(recomp_program_lookup);
    recomp_input_adapter_reset();
    recomp_input_adapter_set_source(sampleInput);
    recomp_input_adapter_set_feedback_sink(recomp_input_host_set_vibration);
    std::atexit(recomp_input_host_stop_vibration);
    std::atexit(recomp_d3d8_shutdown);
    std::atexit(recomp_dsound_api_shutdown);
    recomp_runtime.registers.esp = XBOX_STARTUP_STACK_TOP;
    std::fprintf(stderr, "recomp runner: entry=0x%08x image_end=0x%08x native-d3d8-dsound\n",
        parsed.metadata.selectedEntryPoint, static_cast<unsigned>(imageEnd));
    recomp_watchdog_start();
    recomp_runtime.registers.esp -= 4u;
    *recomp_memory_u32(recomp_runtime.registers.esp) = 0u;
    recomp_dispatch_indirect_site(parsed.metadata.selectedEntryPoint,
        XBOX_STARTUP_STACK_TOP, __FILE__, __LINE__);
    recomp_kernel_run_threads();
    recomp_stop(0, "program-returned");
}
