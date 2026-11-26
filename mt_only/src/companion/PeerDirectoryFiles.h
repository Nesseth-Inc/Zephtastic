// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdio.h>
#include <errno.h>
namespace mt::companion {
struct PeerDirectoryFiles {
    static constexpr const char *stage="/zt-history/.zt-dir-tmp";
    static void path(char (&out)[40],int page,unsigned bank){
        if(page<0)snprintf(out,sizeof(out),"/zt-history/.zt-directory");
        else snprintf(out,sizeof(out),"/zt-history/.zt-dir-%02x-%u",unsigned(page),bank);
    }
};
// This namespace is outside the only phone-visible root, /zt-history/public.
struct WarmPeerNames {
    static constexpr const char *stage="/zt-history/.zt-warm-tmp";
    static void path(char (&out)[40],int page,unsigned){
        if(page<0)snprintf(out,sizeof(out),"/zt-history/.zt-warm-index");
        else snprintf(out,sizeof(out),"/zt-history/.zt-warm-%02x",unsigned(page));
    }
};
template<class Names,class Unlink> int erasePeerFiles(unsigned pages,unsigned banks,Unlink unlink){
    for(unsigned page=0;page<pages;++page){
        for(unsigned bank=0;bank<banks;++bank){
            char name[40];Names::path(name,page,bank);
            int rc=unlink(name);if(rc&&rc!=-ENOENT)return rc;
        }
    }
    char name[40];Names::path(name,-1,0);
    int rc=unlink(name);if(rc&&rc!=-ENOENT)return rc;
    rc=unlink(Names::stage);return rc==-ENOENT?0:rc;
}
}
