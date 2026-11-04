// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Opt-in host timing/provenance. These timestamps are not display scanout or
// proof that a sampled input caused a visible response. Never waits for the GPU.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>
namespace recomp_latency {
inline int64_t now() { return std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count(); }
struct Stamp { uint64_t packet=0, input=0; int64_t sampled=0, queued=0; uint32_t scene=0; };
struct Row { const char *event; int64_t time; Stamp current, previous; double alpha; bool window; };
class Trace {
public:
    const char *path=std::getenv("RECOMP_LATENCY_TRACE");
    std::mutex mutex;
    Stamp latest;
    uint64_t packets=0, dropped=0;
    std::vector<Row> rows;
    bool enabled() const { return path && *path; }
    void append(Row row) { if (rows.size()<1000000) rows.push_back(row); else ++dropped; }
    ~Trace() {
        if (!enabled()) return;
        FILE *file=std::fopen(path,"wb");
        if (!file) { std::fprintf(stderr,"Latency trace: cannot write output\n"); return; }
        std::fprintf(file,"event,time_ns,packet,input,sampled_ns,queued_ns,scene,previous_packet,previous_input,previous_sampled_ns,previous_queued_ns,alpha,window_submitted\n");
        for (const auto &r:rows) std::fprintf(file,"%s,%lld,%llu,%llu,%lld,%lld,%u,%llu,%llu,%lld,%lld,%.9f,%u\n",
            r.event,(long long)r.time,(unsigned long long)r.current.packet,(unsigned long long)r.current.input,
            (long long)r.current.sampled,(long long)r.current.queued,r.current.scene,
            (unsigned long long)r.previous.packet,(unsigned long long)r.previous.input,
            (long long)r.previous.sampled,(long long)r.previous.queued,r.alpha,unsigned(r.window));
        std::fprintf(file,"dropped,%llu,0,0,0,0,0,0,0,0,0,1,0\n",(unsigned long long)dropped);
        std::fclose(file);
    }
};
inline Trace &trace() { static Trace value; return value; }
inline thread_local Stamp executing;
inline void sample(uint32_t scene) {
    auto &t=trace(); if (!t.enabled()) return;
    std::lock_guard<std::mutex> lock(t.mutex);
    ++t.latest.input;t.latest.sampled=now();t.latest.scene=scene;
    t.append({"input",t.latest.sampled,t.latest,{},1,false});
}
inline Stamp publish() {
    auto &t=trace(); if (!t.enabled()) return {};
    std::lock_guard<std::mutex> lock(t.mutex);
    Stamp stamp=t.latest;stamp.packet=++t.packets;stamp.queued=now();
    t.append({"publish",stamp.queued,stamp,{},1,false});return stamp;
}
inline void record(const char *event,Stamp current,Stamp previous={},double alpha=1,bool window=false) {
    auto &t=trace(); if (!t.enabled()) return;
    std::lock_guard<std::mutex> lock(t.mutex);
    t.append({event,now(),current,previous,alpha,window});
}
}
