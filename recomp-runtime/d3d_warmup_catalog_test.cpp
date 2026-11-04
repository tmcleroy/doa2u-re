// SPDX-License-Identifier: GPL-3.0-or-later
#include "d3d_warmup_catalog.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
int main() {
 const std::string path="catalog-test.bin";
 std::vector<uint64_t> values{1,42,999};
 assert(recomp_warmup::rewrite(path,values));
 assert(recomp_warmup::read<uint64_t>(path)==values);
 FILE *f=std::fopen(path.c_str(),"ab"); const char bad=7; std::fwrite(&bad,1,1,f); std::fclose(f);
 assert(recomp_warmup::read<uint64_t>(path)==values);
 assert(recomp_warmup::rewrite(path,recomp_warmup::read<uint64_t>(path)));
 f=std::fopen(path.c_str(),"ab"); recomp_warmup::append<uint64_t>(f,1000); std::fclose(f);
 assert(recomp_warmup::read<uint64_t>(path).size()==4);
 f=std::fopen(path.c_str(),"r+b"); std::fseek(f,sizeof(recomp_warmup::Header),SEEK_SET); std::fwrite(&bad,1,1,f); std::fclose(f);
 assert(recomp_warmup::read<uint64_t>(path).empty());
 assert(recomp_warmup::rewrite(path,values)); assert(recomp_warmup::read<uint32_t>(path).empty());
 std::remove(path.c_str()); std::puts("Catalog integrity tests passed");
}
