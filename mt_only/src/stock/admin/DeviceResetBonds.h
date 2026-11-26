// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <errno.h>
#include <string.h>
#include <zephyr/settings/settings.h>

namespace mt::stock::admin::device_reset {
// Execute delete outside the Settings enumeration lock. Verify disappearance
// of the exact returned name; never select a key outside this subtree.
template<class Store> int eraseSubtree(Store &settings,const char *subtree){
    struct First {char name[SETTINGS_FULL_NAME_LEN+1]{};size_t prefix=0;bool found=false;int error=0;};
    for(unsigned count=0;;++count){
      First first;first.prefix=strlen(subtree);
      if(first.prefix+2>sizeof(first.name))return -EINVAL;
      memcpy(first.name,subtree,first.prefix);first.name[first.prefix++]='/';
      const int scan=settings_load_subtree_direct(subtree,[](const char *name,size_t len,settings_read_cb,
                                                             void *,void *ctx){
        auto &f=*static_cast<First *>(ctx);if(!len)return 0;
        if(f.found||f.error)return 1;
        if(!name||!*name||strlen(name)>=sizeof(f.name)-f.prefix){f.error=-E2BIG;return 1;}
        strcpy(f.name+f.prefix,name);f.found=true;return 1;
      },&first);
      if(scan<0)return scan;
      if(first.error)return first.error;
      if(!first.found)return 0;
      if(count==32)return -EAGAIN;
      const int rc=settings.eraseVerified(first.name);if(rc)return rc;
    }
  }
}
