// SPDX-License-Identifier: GPL-3.0-only
#include "stock/admin/ZephyrResetFloor.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>

namespace {
constexpr unsigned end=0xf4000,floorBase=0xf3000;
constexpr unsigned lfsBase=FIXED_PARTITION_OFFSET(lfs_partition);
std::array<uint8_t,0x100000> flashImage;
unsigned next=0,limit=0,calls=0,writes=0,errorAddress=UINT32_MAX;
bool ready=true;
void expectReads(unsigned endAddress){
    next=lfsBase;limit=endAddress;calls=0;
}
}
const device *flashDevice(){static device dev;return &dev;}
bool device_is_ready(const device *){return ready;}
int flash_read(const device *,unsigned address,void *out,size_t size){
    assert(address==next&&size==64&&address>=lfsBase&&address+size<=limit);
    next+=size;++calls;
    if(address==errorAddress)return -EIO;
    memcpy(out,flashImage.data()+address,size);return 0;
}
size_t flash_get_write_block_size(const device *){return 4;}
int flash_write(const device *,unsigned,const void *,size_t){++writes;return -EIO;}

int main(){
    using mt::stock::admin::device_reset::ZephyrFloor;
    static_assert(MT_HAS_DEVICE_RESET);
    ZephyrFloor floor;
    // Everything outside the intended scan is occupied, including NVS, the
    // running application, and UF2. A whole-chip read would fail this test.
    flashImage.fill(0x36);
    std::fill(flashImage.begin()+lfsBase,flashImage.begin()+end,0xff);
    expectReads(end);assert(!floor.firstEnrollment()&&next==end&&calls==(end-lfsBase)/64);
    unsigned rejected=0;
    for(unsigned page=lfsBase;page<end;page+=4096){
        for(unsigned at:{page,page+4095}){
            flashImage[at]=0xfe;expectReads(end);
            assert(floor.firstEnrollment()==-EEXIST&&next>=at+1&&next<=at+64);++rejected;
            flashImage[at]=0xff;
        }
    }
    errorAddress=lfsBase+4096;expectReads(end);assert(floor.firstEnrollment()==-EIO);
    errorAddress=UINT32_MAX;ready=false;expectReads(end);
    assert(floor.firstEnrollment()==-ENODEV&&!calls);ready=true;
    // Existing partial first-seal bytes are validated separately by Floor's
    // recovery owner. The retry still requires every LFS byte to be erased.
    flashImage[floorBase+7]=0x32;expectReads(floorBase);
    assert(!floor.firstEnrollment(true)&&next==floorBase);
    flashImage[floorBase-1]=0x2a;expectReads(floorBase);
    assert(floor.firstEnrollment(true)==-EEXIST&&next==floorBase&&!writes);
    printf("PASS reset-floor owner reads [%#x,%#x), rejects %u occupied page edges, propagates read errors, and limits partial recovery to [%#x,%#x)\n",
           lfsBase,end,rejected,lfsBase,floorBase);
}
