// SPDX-License-Identifier: GPL-3.0-only
#include "stock/files/RangeFile.h"
#include "stock/files/XModem.h"
#include <cassert>
#include <cerrno>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
using namespace mt::stock::files;
static std::string root;
static bool failRename=false,failSync=false;
static int capacityError=0;
static bool mockCapacity=false;
static struct fs_statvfs capacity{};
static unsigned statCalls=0;
static int writeBudget=-1;
static std::string local(const char *p){
#if defined(CONFIG_MT_PHONE_PUBLIC_TREE)
    const size_t n=strlen(PublicTree::Mount);
    assert(!strncmp(p,PublicTree::Mount,n)&&(p[n]==0||p[n]=='/')&&!strstr(p,"/../"));
    return root+(p+n);
#else
    assert(!strcmp(p,"/mounted")||!strcmp(p,"/mounted/rangetest.csv")||!strcmp(p,"/mounted/rangetest.csv.tmp"));
    return root+(p+strlen("/mounted"));
#endif
}
void fs_file_t_init(fs_file_t *f){f->file=nullptr;}
int fs_stat(const char *p,fs_dirent *out){struct stat s{};if(stat(local(p).c_str(),&s))return -errno;out->size=s.st_size;out->type=S_ISDIR(s.st_mode)?FS_DIR_ENTRY_DIR:FS_DIR_ENTRY_FILE;return 0;}
int fs_statvfs(const char *name,struct fs_statvfs *out){
    ++statCalls;if(capacityError)return capacityError;
    if(mockCapacity){*out=capacity;return 0;}
    struct statvfs st{};if(statvfs(local(name).c_str(),&st))return -errno;
    *out={st.f_bsize,st.f_frsize,st.f_blocks,st.f_bfree};return 0;
}
int fs_unlink(const char *p){return unlink(local(p).c_str())?-errno:0;}
int fs_open(fs_file_t *f,const char *p,int flags){f->file=fopen(local(p).c_str(),flags&FS_O_TRUNC?"wb":flags&FS_O_APPEND?"ab":"rb");return f->file?0:-errno;}
ssize_t fs_read(fs_file_t *f,void *p,size_t n){return fread(p,1,n,f->file);}
ssize_t fs_write(fs_file_t *f,const void *p,size_t n){
    if(writeBudget==0)return -EIO;
    if(writeBudget>0){if(size_t(writeBudget)<n)n=size_t(writeBudget);writeBudget-=int(n);}
    return fwrite(p,1,n,f->file);}
int fs_sync(fs_file_t *f){return failSync?-EIO:fflush(f->file)?-errno:0;}
int fs_close(fs_file_t *f){return fclose(f->file)?-errno:0;}
int fs_seek(fs_file_t *f,off_t offset,int from){return fseeko(f->file,offset,from)?-errno:0;}
int fs_rename(const char *a,const char *b){if(failRename)return -EIO;return rename(local(a).c_str(),local(b).c_str())?-errno:0;}
#if defined(CONFIG_MT_PHONE_PUBLIC_TREE)
#include <fcntl.h>
#include "public_tree.inc"
#endif
static bool stockSpace(unsigned long bytes){
    struct {unsigned long bytes; unsigned long totalBytes(){return bytes;} unsigned long usedBytes(){return 0;}} FSCom{bytes};
#include "stock_range_space.inc"
}
static void recoveryCases(){
    using Log=mt::modules::RangeTestLog;
    Log log; RangeFile file(log); Log::Record row{};const Log::Config saving{true,true,false};
    const auto enqueue=[&](){assert(log.enqueue(row)==Log::EnqueueResult::Queued);};
    const auto read=[&](){assert(file.open(false)==0);uint8_t raw[2048]{};auto n=file.read(0,raw,sizeof(raw));
        assert(n>=0);assert(file.close(false)==0);return std::string((char*)raw,size_t(n));};
    // Start with an empty file on the real host filesystem. Stat errors or
    // simulated capacity never replace the real open/write/sync/delete path.
    assert(!log.removeFile(PublicName)&&log.start(saving)==Log::StartResult::Ready);
    enqueue();assert(log.poll(saving)==Log::PollResult::Written);auto text=read();
    assert(text.find("sender name")!=std::string::npos);
    mockCapacity=true;capacity={16,256,1000,200};
#if defined(CONFIG_BOARD_XIAO_ESP32S3)
    assert(statCalls);assert(stockSpace(51199)==false&&stockSpace(51200)&&stockSpace(51201));
    for(unsigned free:{199u,200u,201u}){
        capacity.f_bfree=free;const auto dropped=log.dropped();const auto before=read();enqueue();
        const bool accepted=stockSpace(free*capacity.f_frsize);
        assert(log.poll(saving)==(accepted?Log::PollResult::Written:Log::PollResult::LowSpace));
        assert(!log.queued()&&(log.dropped()==dropped+unsigned(!accepted)));
        assert(accepted?read().size()>before.size():read()==before);
    }
    for(unsigned fault=0;fault<3;++fault){
        capacity={16,256,1000,200};capacityError=fault==0?-EIO:0;
        if(fault==1)capacity.f_frsize=0;
        if(fault==2)capacity.f_bfree=capacity.f_blocks+1;
        const auto before=read();enqueue();assert(log.poll(saving)==Log::PollResult::IoError&&log.queued()==1);
        assert(read()==before);
        // Uncertain/free-space-unreadable rows remain eligible for a retry;
        // disabling capture releases all of them and restores file access.
        const auto drops=log.dropped();assert(log.poll({true,false,false})==Log::PollResult::Disabled);
        assert(!log.queued()&&log.dropped()==drops+1);
    }
    capacityError=-EIO;capacity={16,256,1000,200};enqueue();
    assert(log.poll(saving)==Log::PollResult::IoError);
    constexpr char other[]="/static/independent.csv";
    assert(!file.open(true,other,sizeof(other)-1));
    const uint8_t extra[]="unrelated";assert(!file.write(extra,sizeof(extra)-1)&&!file.close(true)&&log.queued()==1);
    assert(!file.open(false,other,sizeof(other)-1));uint8_t copy[16]{};
    assert(file.read(0,copy,sizeof(copy))==sizeof(extra)-1&&!memcmp(extra,copy,sizeof(extra)-1)&&!file.close(false));
    assert(!log.removeFile(other)&&log.queued()==1);
    assert(file.open(true)==0);enqueue();assert(!file.close(false)&&log.queued()==2);
    assert(file.open(true)==0);const uint8_t replacement[]="replacement";assert(!file.write(replacement,sizeof(replacement)-1));
    enqueue();assert(!file.close(true)&&log.queued()==1&&read()=="replacement");
    assert(!log.removeFile(PublicName)&&!log.queued());capacityError=0;
    enqueue();assert(log.poll(saving)==Log::PollResult::Written);
#else
    capacity.f_bfree=0;assert(!stockSpace(0));enqueue();
    assert(log.poll(saving)==Log::PollResult::Written&&statCalls==0);
#endif
    // An actual append followed by uncertain sync cannot duplicate the same
    // row on the next poll. Owner download and delete must still succeed.
    failSync=true;enqueue();assert(log.poll(saving)==Log::PollResult::IoError&&!log.queued());
    failSync=false;const auto after=read();assert(log.poll(saving)==Log::PollResult::Empty&&read()==after);
    enqueue();assert(log.poll({false,false,false})==Log::PollResult::Disabled&&!log.queued());
    const auto intact=read();writeBudget=0;enqueue();
    assert(log.poll(saving)==Log::PollResult::IoError&&log.queued()==1&&read()==intact);
    writeBudget=3;assert(log.poll(saving)==Log::PollResult::IoError&&!log.queued());
    const auto partial=read();assert(partial.size()==intact.size()+3);writeBudget=-1;
    enqueue();assert(log.poll(saving)==Log::PollResult::IoError&&log.queued()==1&&read()==partial);
    assert(!log.removeFile(PublicName)&&!log.queued());enqueue();assert(log.poll(saving)==Log::PollResult::Written);
    assert(read().find("time,from,")==0);assert(!log.removeFile(PublicName));mockCapacity=false;
    puts("PASS stock low-space boundary, safe file access after failed/disabled logging and committed replacement order");
}
int main(int argc,char **argv){
#if defined(CONFIG_MT_PHONE_PUBLIC_TREE)
    assert(argc==2);root=argv[1];recoveryCases();publicTreeCases();return 0;
#else
    assert(argc==2);root=argv[1];recoveryCases();mt::modules::RangeTestLog log;RangeFile file(log);uint32_t size=0;
    assert(file.info(size)==-ENOENT);
    auto write=[&](const char *body,bool commit){assert(file.open(true)==0);assert(file.write((const uint8_t*)body,strlen(body))==0);
        assert(log.removeFile(PublicName)==-EBUSY);return file.close(commit);};
    auto read=[&](){assert(file.open(false)==0);uint8_t b[128];int n=file.read(0,b,sizeof(b));
        assert(n>=0&&log.removeFile(PublicName)==-EBUSY);assert(file.close(false)==0);return std::string((char*)b,n);};
    assert(write("original",true)==0&&read()=="original");assert(file.info(size)==0&&size==8);
    assert(write("incomplete",false)==0&&read()=="original");
    failRename=true;assert(write("not committed",true)==-EIO);failRename=false;assert(read()=="original");
    assert(write("new",true)==0&&read()=="new");
    assert(log.start({true,true,false})==mt::modules::RangeTestLog::StartResult::Ready);
    mt::modules::RangeTestLog::Record row{};assert(log.enqueue(row)==mt::modules::RangeTestLog::EnqueueResult::Queued);
    assert(file.open(true)==-EBUSY);assert(log.poll({true,true,false})==mt::modules::RangeTestLog::PollResult::Written);
    assert(read().size()>3);fs_file_t abandoned{};assert(fs_open(&abandoned,"/mounted/rangetest.csv.tmp",FS_O_WRITE|FS_O_TRUNC)==0);fs_close(&abandoned);
    assert(log.removeFile(PublicName)==0&&file.info(size)==-ENOENT);assert(access((root+"/rangetest.csv.tmp").c_str(),F_OK)!=0);
    puts("PASS real temporary files, Zephyr owner exclusion, atomic replace/abort/failure, stale upload cleanup and existing CSV logger");
#endif
}
