// Compile donor's actual admission decision on the stock XIAO 200-node geometry.
#include <cassert>
#include <cstdint>
#include <array>
constexpr int MAX_NUM_NODES=200;
struct meshtastic_NodeInfoLite {uint32_t bitfield=0;};
#include "helpers.inc"
struct NodeDB {
    std::array<meshtastic_NodeInfoLite,200> nodes{};decltype(nodes)*meshNodes=&nodes;
    int numMeshNodes=200;int numProtectedNodes()const;
    bool setProtectedFlag(meshtastic_NodeInfoLite*,uint32_t,bool);
};
#include "oracle.inc"
int main(){NodeDB db;const uint32_t masks[]={NODEINFO_BITFIELD_IS_FAVORITE_MASK,NODEINFO_BITFIELD_IS_IGNORED_MASK,NODEINFO_BITFIELD_IS_KEY_MANUALLY_VERIFIED_MASK};
    for(unsigned i=1;i<=198;++i)assert(db.setProtectedFlag(&db.nodes[i],masks[i%3],true));
    assert(!db.setProtectedFlag(&db.nodes[199],masks[1],true));assert(db.setProtectedFlag(&db.nodes[198],masks[2],true));
    assert(db.numProtectedNodes()==198);}
