// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <type_traits>
#include <filesystem>
#ifdef _WIN32
#include <windows.h>
#endif

namespace recomp_warmup {
inline uint64_t checksum(const void *data, size_t size) {
    uint64_t h=1469598103934665603ull;
    for (size_t i=0;i<size;++i) h=(h^static_cast<const uint8_t*>(data)[i])*1099511628211ull;
    return h;
}
struct Header { uint32_t magic, size; uint64_t hash; };
template<class T> std::vector<T> read(const std::string &path, size_t limit=16384) {
    static_assert(std::is_trivially_copyable<T>::value);
    std::vector<T> result;
    FILE *f=std::fopen(path.c_str(),"rb"); if (!f) return result;
    Header h{}; T value{};
    while (result.size()<limit && std::fread(&h,sizeof h,1,f)==1) {
        if (h.magic!=0x32574353u || h.size!=sizeof(T) || std::fread(&value,sizeof value,1,f)!=1 ||
            h.hash!=checksum(&value,sizeof value)) break;
        result.push_back(value);
    }
    std::fclose(f); return result;
}
template<class T> bool append(FILE *f,const T &value) {
    if (!f) return false;
    const Header h{0x32574353u,sizeof(T),checksum(&value,sizeof value)};
    return std::fwrite(&h,sizeof h,1,f)==1 && std::fwrite(&value,sizeof value,1,f)==1;
}
// Rewrite the validated prefix atomically; a truncated prior append cannot
// hide all future additions. Failure leaves the original catalog available.
template<class T> bool rewrite(const std::string &path,const std::vector<T> &records) {
    const std::string temp=path+".tmp";
    FILE *f=std::fopen(temp.c_str(),"wb"); if (!f) return false;
    bool ok=true; for (const auto &r:records) if (!append(f,r)) { ok=false; break; }
    if (std::fclose(f)!=0) ok=false;
    if (!ok) return false;
#ifdef _WIN32
    return MoveFileExW(std::filesystem::u8path(temp).c_str(),std::filesystem::u8path(path).c_str(),
        MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
#else
    return std::rename(temp.c_str(),path.c_str())==0;
#endif
}
}
