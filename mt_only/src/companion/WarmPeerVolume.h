// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "PeerDirectoryVolume.h"
namespace mt::companion {
struct WarmPeerVolume:PrivatePeerVolume<WarmPeerNames>{
    int get(int page,void *p,size_t n){return PrivatePeerVolume::get(page,0,p,n);}
    int put(int page,const void *p,size_t n){return PrivatePeerVolume::put(page,0,p,n);}
    int erase(unsigned pages){return PrivatePeerVolume::erase(pages,1);}
};
}
