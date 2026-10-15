/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "program_manual.h"
#include "xbox_memory_layout.h"
#include <stdlib.h>
RecompRuntime recomp_runtime;
uint8_t *recomp_fast_ram;
static uint32_t tick, polls;
uint32_t *recomp_memory_u32_checked(uint32_t address)
{
    if (address != XBOX_KERNEL_DATA_BASE + 0x40u) abort();
    return &tick;
}
void recomp_kernel_poll(void) { ++polls; }
uint64_t recomp_xapi_performance_counter(void) { return UINT64_C(123456789000); }

RecompFunction recomp_d3d8_lookup_manual(uint32_t address)
{
    (void)address;
    return NULL;
}

RecompFunction recomp_d3d8_resource_lookup_manual(uint32_t address)
{
    (void)address;
    return NULL;
}

RecompFunction recomp_d3d8_draw_lookup_manual(uint32_t address)
{
    (void)address;
    return NULL;
}

RecompFunction recomp_d3d8_state_lookup_manual(uint32_t address)
{
    (void)address;
    return NULL;
}

RecompFunction recomp_dsound_api_lookup_manual(uint32_t address)
{
    (void)address;
    return NULL;
}

RecompFunction recomp_fiber_lookup_manual(uint32_t address)
{
    (void)address;
    return NULL;
}

RecompFunction recomp_input_lookup_manual(uint32_t address)
{
    (void)address;
    return NULL;
}

int main(void)
{
    RecompFunction init_segment = recomp_lookup_manual(0x002bd531u);
    if (init_segment == NULL) return 1;
    recomp_runtime.registers.esp = 0x1000u;
    recomp_runtime.registers.eax = 0x12345678u;
    init_segment();
    if (recomp_runtime.registers.esp != 0x1004u || recomp_runtime.registers.eax != 0x12345678u) return 1;
    RecompFunction get_tick = recomp_lookup_manual(0x002b8a24u);
    if (!get_tick) return 1;
    get_tick();
    if (recomp_runtime.registers.esp != 0x1008u ||
        recomp_runtime.registers.eax != 123456u || tick != 123456u || polls != 1u) return 1;
    RecompFunction xnet = recomp_lookup_manual(0x00381331u);
    if (!xnet) return 1;
    xnet();
    return recomp_runtime.registers.esp != 0x1010u ||
        recomp_runtime.registers.eax != 10091u || tick != 123456u || polls != 1u ||
        recomp_lookup_manual(0u) != NULL ||
        recomp_lookup_manual(1u) != NULL ||
        recomp_lookup_manual(UINT32_MAX) != NULL;
}
