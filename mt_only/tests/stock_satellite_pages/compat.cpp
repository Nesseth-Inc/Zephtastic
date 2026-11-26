// SPDX-License-Identifier: GPL-3.0-only
#include "companion/PeerDirectoryOwner.h"
#include <cassert>
#include <cstdio>
#include <map>
#include <vector>
#include "../stock_warm_peers/Map.h"
struct Disk:Map {int discardManifest(){files.erase(-1);return 0;}};
using namespace mt::companion;

template<unsigned Count,unsigned Rows> void fixture(const char *path){
    using Owner=PeerDirectoryOwner<Count,1,Rows>;
    Owner owner;Disk fs;typename Owner::Domain d{};d.node=55;d.epoch[0]=1;d.key[0]=7;
#if defined(CONFIG_MT_PAGED_SATELLITES)
    auto *in=fopen(path,"rb");assert(in);
    int c;while((c=fgetc(in))!=EOF)fs.files[-1].push_back(c);assert(!fclose(in));
    fs.files[7]={1,7,11};const auto previous=fs.files;
    assert(owner.open(fs,d,7)==-EXDEV&&!owner.ready());
    assert(owner.fenceJournal(fs,7,8)==-EXDEV&&fs.files==previous);
#else
    assert(!owner.open(fs,d,7));
    mt::NodeCache::Entry e;e.identity(689,1,0);owner.observe(e);assert(!owner.poll(fs,7));
    assert(!owner.fenceJournal(fs,7,8)); // old next generation remains primed
    const auto bytes=fs.files.at(-1);auto *out=fopen(path,"wb");assert(out);
    assert(fwrite(bytes.data(),1,bytes.size(),out)==bytes.size());assert(!fclose(out));
#endif
}
int main(int argc,char **argv){
    assert(argc==4);
#if defined(CONFIG_MT_PAGED_SATELLITES)
    fixture<119,40>(argv[1]);fixture<119,40>(argv[2]);fixture<199,16>(argv[3]);
    puts("PASS recognize prior T096/16, T096/32 and XIAO/16 stores, preserve exact bytes, refuse implicit ownership of pending old fences");
#else
    fixture<119,16>(argv[1]);fixture<119,32>(argv[2]);fixture<199,16>(argv[3]);
#endif
}
