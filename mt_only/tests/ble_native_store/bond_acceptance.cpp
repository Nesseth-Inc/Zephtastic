#include "BleBondAcceptance.h"
#include <cassert>
int main(){using mt::ble::BleBondAcceptance;BleBondAcceptance g;
 g.begin(1,false,false);assert(g.allowed());
 g.begin(2,true,true);assert(g.allowed()&&!g.storeBegin());
 g.begin(3,true,false);assert(!g.allowed()&&g.storeBegin()==3);g.paired(true);assert(!g.allowed());g.storeEnd(3,0);assert(g.allowed());
 g.begin(4,true,false);g.storeEnd(3,0);g.paired(true);assert(!g.allowed());g.storeEnd(4,0);assert(g.allowed());
 g.begin(5,true,false);g.storeEnd(5,-16);g.paired(true);assert(!g.allowed());g.storeEnd(5,0);assert(!g.allowed());
 g.begin(6,true,false);g.storeEnd(6,0);assert(!g.allowed());g.paired(false);assert(!g.allowed());
 g.begin(7,true,false);g.storeEnd(7,0);g.paired(true);assert(g.allowed());g.begin(0,false,false);assert(!g.allowed());
}
