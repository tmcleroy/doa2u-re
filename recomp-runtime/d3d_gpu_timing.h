// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <SDL3/SDL.h>
#include "d3d_latency_trace.h"
#include <vector>

// Renderer-thread-only, opt-in diagnostics. Fences are polled, never waited on.
// Completion is bracketed between the last negative query and first positive
// query. This includes queueing and is NOT GPU execution time or display time.
struct D3dGpuTiming {
    struct Pending { SDL_GPUFence *fence; uint64_t frame; uint32_t scene; int64_t submitted, lower; };
    struct Row { const char *event; uint64_t frame; uint32_t scene; int64_t begin, end; bool ok; };
    const char *path = std::getenv("RECOMP_GPU_TIMING");
    std::vector<Pending> pending;
    std::vector<Row> rows;
    uint64_t dropped = 0;
    int64_t next_poll = 0;
    bool enabled() const { return path && *path; }
    void add(const char *event, uint64_t frame, uint32_t scene, int64_t begin, int64_t end, bool ok) {
        if (!enabled()) return;
        if (rows.size() < 1000000) rows.push_back({event,frame,scene,begin,end,ok}); else ++dropped;
    }
    void poll(SDL_GPUDevice *device, bool force=false) {
        if (!enabled() || pending.empty()) return;
        const auto tick = recomp_latency::now();
        if (!force && tick < next_poll) return;
        next_poll = tick + 100000; // at most 10,000 checks/s, only on renderer opportunities
        for (auto it=pending.begin(); it!=pending.end();) {
            const auto before = recomp_latency::now();
            if (SDL_QueryGPUFence(device,it->fence)) {
                const auto after = recomp_latency::now();
                add("completion_bounds",it->frame,it->scene,it->lower,after,true);
                SDL_ReleaseGPUFence(device,it->fence);
                it=pending.erase(it);
            } else { it->lower=before; ++it; }
        }
    }
    void submit(SDL_GPUDevice *device, SDL_GPUCommandBuffer *commands, uint64_t frame, uint32_t scene) {
        poll(device);
        const auto begin=recomp_latency::now();
        if (pending.size() >= 256) {
            add("fence_overflow",frame,scene,begin,begin,false);
            SDL_SubmitGPUCommandBuffer(commands); return;
        }
        auto *fence=SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
        const auto end=recomp_latency::now();
        add("submit",frame,scene,begin,end,fence!=nullptr);
        if (fence) pending.push_back({fence,frame,scene,begin,begin});
    }
    void finish(SDL_GPUDevice *device) {
        if (!enabled()) return;
        poll(device,true);
        for (auto &p:pending) {
            add("incomplete",p.frame,p.scene,p.lower,recomp_latency::now(),false);
            SDL_ReleaseGPUFence(device,p.fence);
        }
        pending.clear();
        FILE *file=std::fopen(path,"wb");
        if (!file) { std::fputs("GPU timing: output open failed\n",stderr); return; }
        std::fputs("event,frame,scene,begin_ns,end_ns,ok\n",file);
        for (const auto &r:rows) std::fprintf(file,"%s,%llu,%u,%lld,%lld,%u\n",r.event,
            (unsigned long long)r.frame,r.scene,(long long)r.begin,(long long)r.end,unsigned(r.ok));
        std::fprintf(file,"dropped,0,0,%llu,0,0\n",(unsigned long long)dropped);
        std::fclose(file);
    }
};
