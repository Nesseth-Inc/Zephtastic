// SPDX-License-Identifier: GPL-3.0-only
#include "stock/admin/ZephyrResetFloor.h"
#include <cassert>
#include <cstring>
#include <cstdio>
static unsigned reads=0;
static uint8_t marker[8];
const device *flashDevice(){static device instance;return &instance;}
bool device_is_ready(const device *){return true;}
int flash_read(const device *,unsigned offset,void *out,size_t n){
    assert(offset==0xf3000&&n==sizeof(marker));++reads;memcpy(out,marker,n);return 0;
}
int main(){
    using namespace mt::stock::admin::device_reset;
    static_assert(MT_CHECK_PRIOR_DEVICE_RESET&&!MT_HAS_DEVICE_RESET);
    memset(marker,255,sizeof(marker));assert(!priorEnrollment());
    memcpy(marker,"ZTRF",4);marker[4]=1;marker[5]='R';marker[6]=marker[7]=0;
    assert(priorEnrollment()==-EXDEV&&reads==2);
    puts("PASS T096 non-reset firmware at both private/legacy FS extents guards the identical 0xf3000 retirement marker");
}
