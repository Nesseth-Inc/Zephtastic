// SPDX-License-Identifier: GPL-3.0-only
#include <map>
#include <cstdint>
#include <cassert>
#include <cstdio>
using NodeNum=uint32_t;
struct NodeDB{std::map<uint32_t,uint32_t> heard;uint32_t getNodeNum(){return 1;}uint32_t hotNodeLastHeard(uint32_t n){return heard[n];}};
#include "oracle.inc"
int main(){
    using M=std::map<uint32_t,uint32_t>;NodeDB db;M maps[4];
    for(auto &map:maps)for(unsigned i=1;i<=MAX_SATELLITE_NODES;i++){map[i]=9000+i;db.heard[i]=1000+i;}
    // Update a member at capacity without sacrificing unrelated rows.
    for(auto &map:maps){evictSatelliteOverCap(db,map,7);assert(map.size()==MAX_SATELLITE_NODES);}
    db.heard[1]=0;db.heard[9]=0;db.heard[15]=0;const auto before=maps[1];
    evictSatelliteOverCap(db,maps[0],900);maps[0][900]=42;
    assert(maps[0].count(1)&&!maps[0].count(9)&&maps[0].count(15)&&maps[0].size()==MAX_SATELLITE_NODES);
    assert(maps[1]==before&&maps[2]==before&&maps[3]==before);
    // Refresh age in the owner directory, without altering the satellite map.
    db.heard[15]=7000;db.heard[17]=10;db.heard[900]=200;
    evictSatelliteOverCap(db,maps[0],901);assert(!maps[0].count(17)&&maps[0].count(15));
    printf("PASS donor limit %d independent kind, in-place update, self protected, oldest current-owner last-heard, orphan and sorted tie\n",MAX_SATELLITE_NODES);
}
