// SPDX-License-Identifier: GPL-3.0-only
#include "RangeTestLog.h"
#include "../stock/modules/RangeTestDistance.h"

#if defined(CONFIG_MT_PHONE_FILES)
#include "../stock/files/RangeFile.h"
#endif
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#if defined(CONFIG_MT_RANGE_TEST_LOG) && defined(CONFIG_FILE_SYSTEM) && defined(CONFIG_MT_RANGE_TEST_LOG_DIR)
#include <zephyr/fs/fs.h>
#define MT_RANGE_LOG_FS 1
#else
#define MT_RANGE_LOG_FS 0
#endif

namespace mt::modules {
namespace {
constexpr char CsvHeader[]="time,from,sender name,sender lat,sender long,rx lat,rx long,rx elevation,rx snr,distance,hop limit,payload,rx rssi\n";

#if MT_RANGE_LOG_FS
constexpr char ConfiguredDirectory[]=CONFIG_MT_RANGE_TEST_LOG_DIR;
constexpr size_t PathCapacity=192;
bool makePath(char (&path)[PathCapacity]) {
  const size_t length=strlen(ConfiguredDirectory);
  if(!length || length>=PathCapacity || ConfiguredDirectory[0]!='/') return false;
  // The configured directory is an explicit mounted-FS path; reject parent
  // traversal and duplicate separators, and never create it ourselves.
  if(length>1)
    for(size_t i=1;i<length;++i)
      if(ConfiguredDirectory[i]=='/' && ConfiguredDirectory[i-1]=='/') return false;
  for(size_t i=0;i+1<length;++i)
    if(ConfiguredDirectory[i]=='.' && ConfiguredDirectory[i+1]=='.' &&
       (i==0 || ConfiguredDirectory[i-1]=='/') &&
       (i+2==length || ConfiguredDirectory[i+2]=='/')) return false;
  if(length==1 && ConfiguredDirectory[0]=='/')
    return snprintf(path,sizeof(path),"/rangetest.csv")>0;
  const char *separator=ConfiguredDirectory[length-1]=='/'?"":"/";
  const int used=snprintf(path,sizeof(path),"%s%srangetest.csv",ConfiguredDirectory,separator);
  return used>=0 && static_cast<size_t>(used)<sizeof(path);
}
#endif

bool validCoordinate(int32_t latitudeI,int32_t longitudeI) {
  return latitudeI>=-900000000 && latitudeI<=900000000 &&
         longitudeI>=-1800000000 && longitudeI<=1800000000;
}

bool appendText(char *out,size_t capacity,size_t &used,const char *text,size_t size) {
  if(used>capacity || size>capacity-used) return false;
  memcpy(out+used,text,size); used+=size; return true;
}
bool appendFormat(char *out,size_t capacity,size_t &used,const char *format,...) {
  if(used>=capacity) return false;
  va_list args; va_start(args,format);
  const int length=vsnprintf(out+used,capacity-used,format,args);
  va_end(args);
  if(length<0 || static_cast<size_t>(length)>=capacity-used) return false;
  used+=static_cast<size_t>(length); return true;
}
bool appendQuoted(char *out,size_t capacity,size_t &used,const uint8_t *text,size_t size) {
  if(!appendText(out,capacity,used,"\"",1)) return false;
  static constexpr char hex[]="0123456789ABCDEF";
  for(size_t i=0;i<size;++i) {
    const uint8_t c=text[i];
    if(c=='"') { if(!appendText(out,capacity,used,"\"\"",2)) return false; }
    else if(c>=0x20 && c<=0x7e) {
      const char ch=static_cast<char>(c);
      if(!appendText(out,capacity,used,&ch,1)) return false;
    } else {
      const char escaped[]={'\\','x',hex[c>>4],hex[c&0x0f]};
      if(!appendText(out,capacity,used,escaped,sizeof(escaped))) return false;
    }
  }
  return appendText(out,capacity,used,"\"",1);
}
bool appendCoordinate(char *out,size_t capacity,size_t &used,int32_t value) {
  const int64_t signedValue=value;
  const uint64_t absolute=signedValue<0 ? uint64_t(-signedValue) : uint64_t(signedValue);
  return appendFormat(out,capacity,used,"%s%llu.%07llu",
      signedValue<0?"-":"",static_cast<unsigned long long>(absolute/10000000u),
      static_cast<unsigned long long>(absolute%10000000u));
}
bool appendFloat6(char *out,size_t capacity,size_t &used,float value) {
  if(!isfinite(value)) return false;
  const double scaledDouble=double(value)*1000000.0;
  if(scaledDouble>9.0e18 || scaledDouble< -9.0e18) return false;
  const int64_t scaled=static_cast<int64_t>(scaledDouble>=0?scaledDouble+0.5:scaledDouble-0.5);
  const uint64_t absolute=scaled<0?uint64_t(-scaled):uint64_t(scaled);
  return appendFormat(out,capacity,used,"%s%llu.%06llu",
      scaled<0?"-":"",static_cast<unsigned long long>(absolute/1000000u),
      static_cast<unsigned long long>(absolute%1000000u));
}
bool appendComma(char *out,size_t capacity,size_t &used) {
  return appendText(out,capacity,used,",",1);
}
}

bool RangeTestLog::validRecord(const Record &record) {
  if(record.payloadSize>PayloadCapacity ||
     !memchr(record.senderName,'\0',SenderNameCapacity) ||
     !isfinite(record.rxSnr) ||
     (record.senderPositionValid && !validCoordinate(record.senderLatitudeI,record.senderLongitudeI)) ||
     (record.receiverPositionValid && !validCoordinate(record.receiverLatitudeI,record.receiverLongitudeI)) ||
     (record.distanceValid && (!isfinite(record.distanceMeters) || record.distanceMeters<0))) return false;
  if(record.timeValid && (record.utcHour>23 || record.utcMinute>59 || record.utcSecond>60)) return false;
  return true;
}

RangeTestLog::EnqueueResult RangeTestLog::enqueue(const Record &record) {
  if(!validRecord(record)) return EnqueueResult::Invalid;
  const k_spinlock_key_t key=k_spin_lock(&lock_);
  if(count_==QueueCapacity) {
    if(dropped_!=UINT32_MAX) ++dropped_;
    k_spin_unlock(&lock_,key);
    return EnqueueResult::Full;
  }
  Entry &entry=queue_[write_];
  entry.record=record;
  if(++lastSequence_==0) ++lastSequence_;
  entry.sequence=lastSequence_;
  write_=(write_+1)%QueueCapacity;
  ++count_;
  k_spin_unlock(&lock_,key);
  return EnqueueResult::Queued;
}

bool RangeTestLog::peek(Entry &entry) {
  const k_spinlock_key_t key=k_spin_lock(&lock_);
  const bool available=count_!=0;
  if(available) entry=queue_[read_];
  k_spin_unlock(&lock_,key);
  return available;
}
void RangeTestLog::consume(uint32_t sequence) {
  const k_spinlock_key_t key=k_spin_lock(&lock_);
  if(count_ && queue_[read_].sequence==sequence) {
    queue_[read_]=Entry{};
    read_=(read_+1)%QueueCapacity;
    --count_;
  }
  k_spin_unlock(&lock_,key);
}
// Only the filesystem lease holder drains. An enqueue during a file operation
// stays behind the captured prefix and survives an explicit CSV replacement.
void RangeTestLog::discard(size_t count) {
  const auto key=k_spin_lock(&lock_);
  if(count>count_)count=count_;
  read_=(read_+count)%QueueCapacity;
  count_-=count;
  dropped_+=uint32_t(count>UINT32_MAX-dropped_?UINT32_MAX-dropped_:count);
  k_spin_unlock(&lock_,key);
}
uint32_t RangeTestLog::dropped() const {
  const k_spinlock_key_t key=k_spin_lock(&lock_);
  const uint32_t result=dropped_;
  k_spin_unlock(&lock_,key);
  return result;
}
size_t RangeTestLog::queued() const {
  const k_spinlock_key_t key=k_spin_lock(&lock_);
  const size_t result=count_;
  k_spin_unlock(&lock_,key);
  return result;
}

bool RangeTestLog::available() {
#if MT_RANGE_LOG_FS
  char path[PathCapacity];
  return makePath(path);
#else
  return false;
#endif
}

int RangeTestLog::removeFile(const char *name) {
#if defined(CONFIG_MT_PHONE_PUBLIC_TREE)
  if(!name)return -EINVAL;
  char path[stock::files::PublicTree::PhysicalSize];
  const int mapped=stock::files::PublicTree::path(name,strlen(name),path);if(mapped)return mapped;
#else
  if(!name || strcmp(name,"/static/rangetest.csv"))return -EINVAL;
#if MT_RANGE_LOG_FS
  char path[PathCapacity];if(!makePath(path))return -ENOTSUP;
#endif
#endif
#if MT_RANGE_LOG_FS
  FileLease lease(*this);
  if(!lease.held())return -EBUSY;
  const size_t pending=queued();
  if(pending&&!appendBlocked_)return -EBUSY;
  const auto mounted=[](){
    struct fs_dirent directory{};
    const int rc=fs_stat(ConfiguredDirectory,&directory);
    if(rc==-ENOENT||rc==-ENODEV||rc==-ENXIO)return -EAGAIN;
    return rc?rc:directory.type==FS_DIR_ENTRY_DIR?0:-ENOTDIR;
  };
  const int ready=mounted();if(ready)return ready;
#if defined(CONFIG_MT_PHONE_PUBLIC_TREE)
  fs_dirent target{};const int stat=fs_stat(path,&target);
  if(!stat&&target.type!=FS_DIR_ENTRY_FILE)return -EISDIR;
  if(stat&&stat!=-ENOENT)return stat;
  const int cleanup=fs_unlink(stock::files::PublicTree::Stage);
  if(cleanup&&cleanup!=-ENOENT)return cleanup;
#endif
#if defined(CONFIG_MT_PHONE_FILES)
  // A power cut during an upload can leave only this private staging file.
  char stage[sizeof(path)+4];snprintf(stage,sizeof(stage),"%s.tmp",path);
  const int staged=!strcmp(name,"/static/rangetest.csv")?fs_unlink(stage):0;if(staged && staged!=-ENOENT)return staged;
#endif
  const int result=fs_unlink(path);
  // Unmounted paths can report ENOENT just like a missing file. Confirm
  // that the configured directory is still available after the unlink.
  const int confirmed=result==-ENOENT?mounted():result;
  if(!confirmed&&!strcmp(name,"/static/rangetest.csv")){discard(pending);csvRepairNeeded_=false;}
  return confirmed;
#else
  return -ENOTSUP;
#endif
}

RangeTestLog::StartResult RangeTestLog::start(const Config &config) {
  FileLease lease(*this);if(!lease.held())return StartResult::Unavailable;
  if(startCalled_) return startResult_;
  if(!config.enabled) {
    startCalled_=true; startResult_=StartResult::Disabled; return startResult_;
  }
  if(!config.save && !config.clearOnReboot) {
    startCalled_=true; startResult_=StartResult::Disabled; return startResult_;
  }
  if(!available()) return StartResult::Unavailable;
#if MT_RANGE_LOG_FS
  if(config.clearOnReboot) {
    char path[PathCapacity];
    if(!makePath(path)) return StartResult::Unavailable;
    struct fs_dirent directory{};
    const int dirRc=fs_stat(ConfiguredDirectory,&directory);
    if(dirRc!=0 || directory.type!=FS_DIR_ENTRY_DIR) return StartResult::Unavailable;
    const int rc=fs_unlink(path);
    if(rc!=0 && rc!=-ENOENT) return StartResult::ClearFailed;
  } else if(config.save) {
    struct fs_dirent directory{};
    if(fs_stat(ConfiguredDirectory,&directory)!=0 || directory.type!=FS_DIR_ENTRY_DIR)
      return StartResult::Unavailable;
  }
  startCalled_=true;
  startResult_=StartResult::Ready;
  return startResult_;
#else
  return StartResult::Unavailable;
#endif
}

size_t RangeTestLog::formatCsv(const Record &r,char *out,size_t capacity) {
  if(!out || !capacity || !validRecord(r)) return 0;
  size_t used=0;
  if(r.timeValid) {
    if(!appendFormat(out,capacity,used,"%02u:%02u:%02u",unsigned(r.utcHour),
                     unsigned(r.utcMinute),unsigned(r.utcSecond))) return 0;
  } else if(!appendText(out,capacity,used,"??:??:??",8)) return 0;
  if(!appendComma(out,capacity,used) || !appendFormat(out,capacity,used,"%u",unsigned(r.from)) ||
     !appendComma(out,capacity,used) ||
     !appendQuoted(out,capacity,used,reinterpret_cast<const uint8_t *>(r.senderName),
                   strnlen(r.senderName,SenderNameCapacity)) || !appendComma(out,capacity,used)) return 0;
  if(r.senderPositionValid && !appendCoordinate(out,capacity,used,r.senderLatitudeI)) return 0;
  if(!appendComma(out,capacity,used)) return 0;
  if(r.senderPositionValid && !appendCoordinate(out,capacity,used,r.senderLongitudeI)) return 0;
  if(!appendComma(out,capacity,used)) return 0;
  if(r.receiverPositionValid && !appendCoordinate(out,capacity,used,r.receiverLatitudeI)) return 0;
  if(!appendComma(out,capacity,used)) return 0;
  if(r.receiverPositionValid && !appendCoordinate(out,capacity,used,r.receiverLongitudeI)) return 0;
  if(!appendComma(out,capacity,used)) return 0;
  if(r.receiverPositionValid && !appendFormat(out,capacity,used,"%ld",long(r.receiverAltitudeM))) return 0;
  if(!appendComma(out,capacity,used) || !appendFloat6(out,capacity,used,r.rxSnr) ||
     !appendComma(out,capacity,used)) return 0;
  if(r.distanceValid && !appendFloat6(out,capacity,used,r.distanceMeters)) return 0;
  if(!appendComma(out,capacity,used) || !appendFormat(out,capacity,used,"%u",unsigned(r.hopLimit)) ||
     !appendComma(out,capacity,used) || !appendQuoted(out,capacity,used,r.payload,r.payloadSize) ||
     !appendComma(out,capacity,used) || !appendFormat(out,capacity,used,"%ld\n",long(r.rxRssi))) return 0;
  return used;
}

RangeTestLog::PollResult RangeTestLog::poll(const Config &config) {
  FileLease lease(*this);if(!lease.held())return PollResult::Unavailable;
  if(!config.enabled || !config.save){
    discard(QueueCapacity);appendBlocked_=false;return PollResult::Disabled;
  }
  if(!available() || !startCalled_) return blocked(PollResult::Unavailable);
  if(csvRepairNeeded_)return blocked(PollResult::IoError);
  Entry entry{};
  if(!peek(entry)){appendBlocked_=false;return PollResult::Empty;}
#if MT_RANGE_LOG_FS
  auto &record=entry.record;
  if(record.senderPositionValid && record.receiverPositionValid && record.senderLatitudeI &&
     record.senderLongitudeI && record.receiverLatitudeI && record.receiverLongitudeI) {
    record.distanceMeters=stock::range::latLongToMeter(record.senderLatitudeI*1e-7,
        record.senderLongitudeI*1e-7,record.receiverLatitudeI*1e-7,record.receiverLongitudeI*1e-7);
    record.distanceValid=true;
  }
  char path[PathCapacity];
  if(!makePath(path)) return blocked(PollResult::Unavailable);
#if defined(CONFIG_BOARD_XIAO_ESP32S3) && defined(CONFIG_FILE_SYSTEM_LITTLEFS)
  // ESP32 stock aborts this sample below 51,200 free bytes. The pinned
  // LittleFS backend reports available erase blocks in units of f_frsize.
  struct fs_statvfs space{};
  if(fs_statvfs(ConfiguredDirectory,&space)||!space.f_frsize||space.f_bfree>space.f_blocks)
    return blocked(PollResult::IoError);
  constexpr unsigned reserve=51200;
  if(space.f_bfree < reserve/space.f_frsize + (reserve%space.f_frsize!=0)){
    discard(1);return blocked(PollResult::LowSpace);
  }
#endif
  struct fs_dirent stat{};
  int rc=fs_stat(path,&stat);
  const bool needHeader=(rc==-ENOENT) || (rc==0 && stat.size==0);
  if(rc!=0 && rc!=-ENOENT) return blocked(PollResult::IoError);
  char row[CsvCapacity];
  const size_t rowSize=formatCsv(entry.record,row,sizeof(row));
  if(!rowSize){discard(1);return blocked(PollResult::IoError);}
  struct fs_file_t file;
  fs_file_t_init(&file);
  rc=fs_open(&file,path,FS_O_CREATE|FS_O_WRITE|FS_O_APPEND);
  if(rc!=0) return blocked(PollResult::IoError);
  bool changed=false;
  auto writeAll=[&](const char *bytes,size_t size) {
    size_t offset=0;
    while(offset<size) {
      const ssize_t written=fs_write(&file,bytes+offset,size-offset);
      if(written<=0) return false;
      changed=true;offset+=static_cast<size_t>(written);
    }
    return true;
  };
  bool ok=true;
  if(needHeader) ok=writeAll(CsvHeader,sizeof(CsvHeader)-1);
  if(ok) ok=writeAll(row,rowSize);
  // Keep an incomplete row from being joined to a later sample. The
  // public file remains available for a download, replacement or deletion.
  if(!ok&&changed)csvRepairNeeded_=true;
  if(ok) ok=fs_sync(&file)==0;
  const int closeRc=fs_close(&file);
  if(closeRc!=0) ok=false;
  // The file may already contain a partial or fully committed row after a
  // write, sync, or close error. Re-appending it could duplicate the sample.
  if(!ok){if(changed)discard(1);return blocked(PollResult::IoError);}
  consume(entry.sequence);appendBlocked_=false;
  return PollResult::Written;
#else
  (void)entry;
  return PollResult::Unavailable;
#endif
}
} // namespace mt::modules

#if defined(CONFIG_MT_PHONE_FILES)
namespace mt::stock::files {
int RangeFile::info(uint32_t &bytes){
    char path[modules::PathCapacity];
    if(!modules::makePath(path))return -ENODEV;
    fs_dirent entry{};
    int result=fs_stat(path,&entry);
    if(result)return result;
    if(entry.type!=FS_DIR_ENTRY_FILE||entry.size>UINT32_MAX)return -EFBIG;
    bytes=uint32_t(entry.size);return 0;
}
int RangeFile::open(bool upload){
#if defined(CONFIG_MT_PHONE_PUBLIC_TREE)
    return open(upload,"/static/rangetest.csv",sizeof("/static/rangetest.csv")-1);
}
int RangeFile::open(bool upload,const char *name,size_t length){
    char path[PublicTree::PhysicalSize];if(PublicTree::path(name,length,path))return -EINVAL;
#else
    char path[modules::PathCapacity];if(!modules::makePath(path))return -ENODEV;
#endif
    const auto key=k_spin_lock(&log_.lock_);
    const bool busy=log_.fileBusy_||(log_.count_&&!log_.appendBlocked_);
    if(!busy){
        log_.fileBusy_=true;
        // Successful replacement supersedes queued samples captured before
        // this open. Aborts and unrelated public files preserve the queue.
        replacingCsv_=upload;
#if defined(CONFIG_MT_PHONE_PUBLIC_TREE)
        if(length!=sizeof("/static/rangetest.csv")-1||memcmp(name,"/static/rangetest.csv",length))replacingCsv_=false;
#endif
        pendingReplacement_=replacingCsv_?log_.count_:0;
    }
    k_spin_unlock(&log_.lock_,key);
    if(busy)return -EBUSY;
    upload_=upload;
#if defined(CONFIG_MT_PHONE_PUBLIC_TREE)
    strcpy(destination_,path);const char *stage=PublicTree::Stage;
#else
    char stage[sizeof(path)+4];snprintf(stage,sizeof(stage),"%s.tmp",path);
#endif
    fs_file_t_init(&handle_);
    const int rc=fs_open(&handle_,upload?stage:path,upload?FS_O_CREATE|FS_O_WRITE|FS_O_TRUNC:FS_O_READ);
    if(rc){
        const auto release=k_spin_lock(&log_.lock_);log_.fileBusy_=false;k_spin_unlock(&log_.lock_,release);
    }
    return rc;
}
int RangeFile::read(uint32_t offset,uint8_t *out,size_t n){
    const int rc=fs_seek(&handle_,offset,FS_SEEK_SET);
    return rc?rc:int(fs_read(&handle_,out,n));
}
int RangeFile::write(const uint8_t *data,size_t n){
    const auto written=fs_write(&handle_,data,n);
    return written<0?int(written):size_t(written)==n?0:-EIO;
}
int RangeFile::close(bool commit){
    int rc=upload_&&commit?fs_sync(&handle_):0;
    const int closed=fs_close(&handle_);
    if(!rc)rc=closed;
    if(upload_){
#if defined(CONFIG_MT_PHONE_PUBLIC_TREE)
        const char *path=destination_,*stage=PublicTree::Stage;
#else
        char path[modules::PathCapacity],stage[sizeof(path)+4];
        (void)modules::makePath(path);snprintf(stage,sizeof(stage),"%s.tmp",path);
#endif
        if(!rc&&commit)rc=fs_rename(stage,path);
        if(rc||!commit)(void)fs_unlink(stage);
        else if(replacingCsv_){log_.discard(pendingReplacement_);log_.csvRepairNeeded_=false;}
    }
    const auto key=k_spin_lock(&log_.lock_);log_.fileBusy_=false;k_spin_unlock(&log_.lock_,key);
    return rc;
}
}
#endif
