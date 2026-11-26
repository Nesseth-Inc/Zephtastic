// SPDX-License-Identifier: GPL-3.0-only
#include "modules/RangeTestLog.h"
#include "stock/admin/FactoryConfigReset.h"
#include <map>
#include <string>
#include <vector>
#include <zephyr/fs/fs.h>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
using Log=mt::modules::RangeTestLog;
static Log logger;
static bool exists=false,writing=false,mountedVolume=true,unmountDuringUnlink=false;
static int unlinks=0;
static constexpr char mounted[]="/mounted/rangetest.csv",app[]="/static/rangetest.csv";
void fs_file_t_init(fs_file_t *){}
int fs_stat(const char *p,fs_dirent *out){
  if(!mountedVolume)return -ENOENT;
  if(!strcmp(p,"/mounted")){out->type=FS_DIR_ENTRY_DIR;return 0;}
  assert(!strcmp(p,mounted));out->type=FS_DIR_ENTRY_FILE;out->size=0;return exists?0:-ENOENT;
}
int fs_unlink(const char *p){assert(!writing&&!strcmp(p,mounted));++unlinks;if(unmountDuringUnlink){mountedVolume=false;return -ENOENT;}if(!exists)return -ENOENT;exists=false;return 0;}
int fs_open(fs_file_t *,const char *p,int){assert(!strcmp(p,mounted));writing=exists=true;return 0;}
ssize_t fs_write(fs_file_t *,const void *,size_t n){assert(writing&&logger.removeFile(app)==-EBUSY);return ssize_t(n);}
int fs_sync(fs_file_t *){return 0;}
int fs_close(fs_file_t *){writing=false;return 0;}
int main(){
  assert(logger.start({true,true,false})==Log::StartResult::Ready);
  assert(logger.removeFile("../prefs")==-EINVAL&&unlinks==0);
  assert(logger.removeFile(app)==0&&unlinks==1);
  Log::Record row{};row.from=42;row.payload[0]='x';row.payloadSize=1;
  assert(logger.enqueue(row)==Log::EnqueueResult::Queued);
  assert(logger.removeFile(app)==-EBUSY&&unlinks==1);
  assert(logger.poll({true,true,false})==Log::PollResult::Written&&exists);
  assert(logger.removeFile(app)==0&&!exists&&unlinks==2);
  mountedVolume=false;
  assert(logger.removeFile(app)==-EAGAIN&&unlinks==2);
  mountedVolume=true;unmountDuringUnlink=true;
  assert(logger.removeFile(app)==-EAGAIN&&!mountedVolume);
  unmountDuringUnlink=false;
  namespace reset=mt::stock::admin::factory;
  struct Store{
    std::map<std::string,std::vector<uint8_t>> rows;
    int read(const char *k,uint8_t *out,size_t cap){auto i=rows.find(k);if(i==rows.end())return -ENOENT;
      if(i->second.size()>cap)return -E2BIG;memcpy(out,i->second.data(),i->second.size());return int(i->second.size());}
    int write(const char *k,const uint8_t *b,size_t n){rows[k]={b,b+n};return 0;}
    int eraseVerified(const char *k){rows.erase(k);return 0;}
  } store;
  const uint8_t key[32]={1};store.rows["mtbase/state0"]={10};
  assert(!reset::mark(store,42,key,reset::Phase::ErasePreferences));
  unsigned calls=0;auto finish=[&](){++calls;return reset::finish(store,42,key,[&](){return logger.removeFile(app);});};
  {reset::EraseRetry early;early.begin();assert(early.poll(0,finish)==-EAGAIN&&calls==1);
   assert(early.poll(999,finish)==-EAGAIN&&calls==1&&store.rows.count("mtbase/state0"));}
  reset::Phase phase;assert(!reset::inspect(store,42,key,phase)&&phase==reset::Phase::ErasePreferences);
  reset::EraseRetry coldBoot;coldBoot.begin();assert(coldBoot.poll(5,finish)==-EAGAIN&&calls==2);
  exists=mountedVolume=true;
  assert(coldBoot.poll(1004,finish)==-EAGAIN&&exists&&calls==2);
  assert(!coldBoot.poll(1005,finish)&&!coldBoot.waiting()&&!exists&&calls==3&&store.rows.empty());
  puts("PASS two cold boot recovery owners retain reset intent on an unmounted volume and complete once the mount appears");
  puts("PASS stock CSV maps to mounted path; queued and active writes exclude deletion; missing/deleted states are idempotent");
}
