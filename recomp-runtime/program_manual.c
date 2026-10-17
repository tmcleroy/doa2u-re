/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "program_manual.h"
#include "d3d8_adapter.h"
#include "d3d8_state_adapter.h"
#include "d3d8_resource_adapter.h"
#include "d3d8_draw_adapter.h"
#include "dsound_api_adapter.h"
#include "fiber_adapter.h"
#include "input_adapter.h"
#include "kernel_abi.h"
#include "xapi_time_adapter.h"
#include "xbox_memory_layout.h"
#include "stop_report.h"
#include <stdio.h>
#include <math.h>

static void host_kernel_init_segment(void)
{
    /* XAPI 5849's void/ret-0 kernel compatibility entry patches old kernels
       and tail-calls INIT/LDT setup. The native host owns kernel lifetime. */
    fprintf(stderr, "recomp xapi: kernel INIT segment owned by host\n");
    recomp_runtime.registers.esp += 4u;
}

static void get_tick_count(void)
{
    /* 5849 reads KeTickCount. Clock reads also offer a bounded scheduler slice
       because the native host has no preemptive console clock interrupt. */
    recomp_kernel_poll();
    const uint32_t tick = (uint32_t)(recomp_xapi_performance_counter() / 1000000u);
    *recomp_memory_u32(XBOX_KERNEL_DATA_BASE + 0x40u) = tick;
    kernel_return(0u, tick);
}

static void xnet_startup(void)
{
    /* shortcut: offline host has no Xbox network stack; replace this policy
       when native online/System Link enters scope. 5849 returns INT/ret 4. */
    fprintf(stderr, "recomp xnet: startup unavailable policy=offline error=10091\n");
    kernel_return(1u, 10091u); /* Native policy: WSASYSNOTREADY. */
}

static void crt_fmod_intrinsic(void)
{
    /* CRT 0x336dfe: ST(1) is dividend, ST(0) divisor; ret 0 leaves one result.
       Native libm replaces the entire intrinsic, including its table dispatch. */
    const unsigned divisor = recomp_runtime.fpu_top;
    const unsigned result = (divisor + 1u) & 7u;
    recomp_runtime.fpu_stack[result] = fmod(
        recomp_runtime.fpu_stack[result], recomp_runtime.fpu_stack[divisor]);
    recomp_runtime.fpu_top = result;
    recomp_runtime.registers.esp += 4u;
}

static void skip_startup_legal(void)
{
    /* 0xd92c0 owns the conditional startup legal presentation. Skipping its
       menu/asset setup also avoids the corresponding cleanup; the original
       disabled branch returns directly. No arguments, void/ret 0. */
    recomp_runtime.registers.esp += 4u;
}

RecompFunction recomp_lookup_manual(uint32_t guest_address)
{
    if (guest_address == 0x000d92c0u) return skip_startup_legal;
    if (guest_address == 0x00336dfeu) return crt_fmod_intrinsic;
    if (guest_address == 0x002bd531u) return host_kernel_init_segment;
    if (guest_address == 0x002b8a24u) return get_tick_count;
    if (guest_address == 0x00381331u) return xnet_startup;
    RecompFunction function = recomp_d3d8_resource_lookup_manual(guest_address);
    if (function == NULL) function = recomp_d3d8_lookup_manual(guest_address);
    if (function == NULL) function = recomp_d3d8_state_lookup_manual(guest_address);
    if (function == NULL) function = recomp_d3d8_draw_lookup_manual(guest_address);
    if (function == NULL) function = recomp_dsound_api_lookup_manual(guest_address);
    /* XAPI fibers: native Windows fibers replace the SDK stack switch. */
    if (function == NULL) function = recomp_fiber_lookup_manual(guest_address);
    if (function == NULL) function = recomp_input_lookup_manual(guest_address);
    return function;
}
