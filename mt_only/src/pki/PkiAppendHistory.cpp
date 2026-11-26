// SPDX-License-Identifier: GPL-3.0-only
#include "PkiAppendHistory.h"
#include "MtSignature.h"
#include <string.h>

namespace mt::pki {
namespace {
constexpr uint32_t Commit=0x54494d43u;
constexpr uint32_t CounterCommit=Commit&~1u;
constexpr size_t Snapshot=128,SnapshotBytes=1944,SnapshotSeal=1940;
constexpr size_t BasePeerOffset=628,BasePeerBytes=328;
static_assert(PkiLedger::RecordSize==SnapshotBytes&&PkiLedger::SendLimit==144&&PkiLedger::ReceiveLimit==72,"Immutable M69 baseline format");
enum:uint8_t {Tx=1,Rx=2,Add=3,Revoke=4};
struct Busy {bool &v;explicit Busy(bool &b):v(b){v=true;}~Busy(){v=false;}};
uint32_t get(const uint8_t *p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
void put(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=uint8_t(n>>(8*i));}
uint32_t update(uint32_t c,const uint8_t *p,size_t n){for(size_t i=0;i<n;++i){c^=p[i];for(unsigned b=0;b<8;++b)c=(c>>1)^((0u-(c&1u))&0xedb88320u);}return c;}
uint32_t crc(const uint8_t *p,size_t n){return ~update(~0u,p,n);}
bool zero(const uint8_t *p,size_t n){for(size_t i=0;i<n;++i)if(p[i])return false;return true;}
bool erased(const uint8_t *p,size_t n){for(size_t i=0;i<n;++i)if(p[i]!=255)return false;return true;}
bool node(uint32_t n){return n>1&&n<0xfffffffeu;}
bool anchorEqual(const AppendAnchor &a,const AppendAnchor &b){return a.generation==b.generation&&a.snapshotCrc==b.snapshotCrc&&a.rootCrc==b.rootCrc&&a.capacity==b.capacity;}
bool validNode(uint32_t n){return n>1&&n<0xfffffffeu;}
uint32_t crcUpdate(uint32_t c,const uint8_t *p,size_t n){return update(c,p,n);}
void word(uint8_t *out,size_t start,size_t n,size_t at,uint32_t value){for(unsigned i=0;i<4;++i)if(at+i>=start&&at+i-start<n)out[at+i-start]=uint8_t(value>>(8*i));}
}
int PkiAppendHistory::fail(int rc)const{state_=State::Frozen;error_=rc<0?rc:-EIO;return error_;}
int PkiAppendHistory::read(size_t offset,uint8_t *out,size_t size)const{
  if(!out||offset>StoreBytes||size>StoreBytes-offset)return fail(-EINVAL);
  const int rc=store_.read(offset,out,size);return rc?fail(rc):0;
}
int PkiAppendHistory::blank(size_t offset,size_t length)const{
  uint8_t bytes[64];
  while(length){const size_t n=length>sizeof(bytes)?sizeof(bytes):length;const int rc=read(offset,bytes,n);if(rc)return rc;if(!erased(bytes,n))return fail(-EILSEQ);offset+=n;length-=n;}
  return 0;
}
int PkiAppendHistory::program(size_t offset,const uint8_t *data,size_t size){
  if(!data||!size||offset>StoreBytes||size>StoreBytes-offset||(offset&3)||(size&3))return fail(-EINVAL);
  const int rc=store_.program(offset,data,size);if(rc)return fail(rc);
  uint8_t verify[64];size_t done=0;
  while(done<size){const size_t n=size-done>sizeof(verify)?sizeof(verify):size-done;const int r=read(offset+done,verify,n);if(r)return r;if(memcmp(verify,data+done,n))return fail(-EIO);done+=n;}
  return 0;
}
int PkiAppendHistory::baseInfo(const PkiLedger &base,Base &out)const{
  if(!base.ready())return -EACCES;
  uint8_t bytes[64];Base value;
  if(!base.snapshotRead(0,bytes,52)||memcmp(bytes,"ZTPL",4)||bytes[4]!=2||bytes[5]!=144||bytes[6]!=4||bytes[7]!=72)return -EILSEQ;
  value.generation=get(bytes+8);value.node=get(bytes+12);value.sent=bytes[48];value.stored=bytes[49];
  if(!value.generation||value.generation>UINT32_MAX-EventLimit||!node(value.node)||value.stored>4||value.sent>144)return -EILSEQ;
  uint32_t c=~0u;
  for(size_t off=0;off<SnapshotSeal;){const size_t n=SnapshotSeal-off>sizeof(bytes)?sizeof(bytes):SnapshotSeal-off;if(!base.snapshotRead(off,bytes,n))return -EIO;c=update(c,bytes,n);off+=n;}
  if(!base.snapshotRead(SnapshotSeal,bytes,4)||get(bytes)!=~c)return -EILSEQ;
  value.snapshotCrc=~c;
  for(unsigned i=0;i<value.stored;++i){if(!base.snapshotRead(BasePeerOffset+i*BasePeerBytes,bytes,40))return -EIO;if(bytes[36]>72||bytes[37]>1||bench::MtSignature::nodeId(bytes+4)!=get(bytes))return -EILSEQ;value.received=uint16_t(value.received+bytes[36]);if(!bytes[37])++value.active;}
  out=value;return 0;
}
bool PkiAppendHistory::rootChunk(const PkiLedger &base,const Base &info,size_t offset,uint8_t *out,size_t n)const{
  if(!out||offset>4088||n>4088-offset)return false;
  memset(out,0,n);
  const uint8_t prefix[8]={'Z','T','A','H',1,128,64,0};
  for(unsigned i=0;i<8;++i)if(i>=offset&&i-offset<n)out[i-offset]=prefix[i];
  word(out,offset,n,8,uint32_t(StoreBytes));word(out,offset,n,12,uint32_t(RootBytes));word(out,offset,n,16,uint32_t(EventLimit));word(out,offset,n,20,info.generation);
  word(out,offset,n,24,uint32_t(SnapshotBytes));word(out,offset,n,28,info.snapshotCrc);word(out,offset,n,32,info.node);
  const size_t end=offset+n;
  if(offset<68&&end>36){const size_t start=offset>36?offset:36,stop=end<68?end:68;if(!base.snapshotRead(16+start-36,out+start-offset,stop-start))return false;}
  if(offset<Snapshot+SnapshotBytes&&end>Snapshot){const size_t start=offset>Snapshot?offset:Snapshot,stop=end<Snapshot+SnapshotBytes?end:Snapshot+SnapshotBytes;if(!base.snapshotRead(start-Snapshot,out+start-offset,stop-start))return false;}
  return true;
}
int PkiAppendHistory::validateRoot(const PkiLedger &base,const Base &info,const AppendAnchor &a){
  if(a.capacity!=StoreBytes||a.generation!=info.generation||a.snapshotCrc!=info.snapshotCrc)return fail(-EILSEQ);
  uint8_t expected[64],actual[64];uint32_t c=~0u;
  for(size_t off=0;off<4088;){const size_t n=4088-off>sizeof(actual)?sizeof(actual):4088-off;if(!rootChunk(base,info,off,expected,n))return fail(-EIO);const int rc=read(off,actual,n);if(rc)return rc;if(memcmp(expected,actual,n))return fail(-EILSEQ);c=update(c,actual,n);off+=n;}
  const int rc=read(4088,actual,8);if(rc)return rc;
  const uint32_t commit=get(actual+4);
  if(get(actual)!=~c||get(actual)!=a.rootCrc||(commit!=Commit&&commit!=CounterCommit))return fail(-EILSEQ);
  counterMode_=commit==CounterCommit;
  return 0;
}
int PkiAppendHistory::initialize(const PkiLedger &base,bool q,AppendAnchor &out){
  if(busy_)return -EBUSY;
  if(state_!=State::New)return -EACCES;
  if(!q)return -EAGAIN;
  if(store_.capacity()!=StoreBytes)return fail(-EXDEV);
  AppendAnchor installed;
  if(base.appendAnchor(installed))return -EACCES; // Never recreate a missing selected journal.
  Busy guard(busy_);Base info;int rc=baseInfo(base,info);if(rc)return fail(rc);
  // This unique refusal is proven to precede every program call. Program
  // errno values (including EEXIST) must never imply a no-write outcome.
  {uint8_t probe[64];
    for(size_t off=0;off<StoreBytes;off+=sizeof(probe)){
      if((rc=read(off,probe,sizeof(probe))))return rc;
      if(!erased(probe,sizeof(probe)))return -ENOTEMPTY;
    }
  }
  uint8_t bytes[64];uint32_t c=~0u;
  for(size_t off=0;off<4088;){const size_t n=4088-off>sizeof(bytes)?sizeof(bytes):4088-off;if(!rootChunk(base,info,off,bytes,n))return fail(-EIO);c=update(c,bytes,n);if((rc=program(off,bytes,n)))return rc;off+=n;}
  AppendAnchor candidate;candidate.generation=info.generation;candidate.snapshotCrc=info.snapshotCrc;candidate.rootCrc=~c;candidate.capacity=uint32_t(StoreBytes);
  put(bytes,candidate.rootCrc);if((rc=program(4088,bytes,4)))return rc;
  put(bytes,Commit);if((rc=program(4092,bytes,4)))return rc;
  if((rc=validateRoot(base,info,candidate))||(rc=blank(RootBytes,StoreBytes-RootBytes)))return rc;
  base_=&base;initial_=info;anchor_=candidate;state_=State::Initialized;error_=0;out=candidate;return 0;
}
int PkiAppendHistory::baselinePin(unsigned index,Pin &out,bool &tombstone)const{
  if(!base_||index>=initial_.stored)return -ENOENT;
  uint8_t bytes[40];if(!base_->snapshotRead(BasePeerOffset+index*BasePeerBytes,bytes,sizeof(bytes)))return fail(-EIO);
  Pin p;p.node=get(bytes);p.domain=index+1;memcpy(p.publicKey,bytes+4,32);out=p;tombstone=bytes[37]!=0;return 0;
}
int PkiAppendHistory::findActive(uint32_t id)const{for(unsigned i=0;i<activeCount_;++i)if(pins_[i].node==id)return int(i);return -1;}
int PkiAppendHistory::cell(unsigned index,uint8_t out[CellBytes])const{
  if(index>=EventLimit)return fail(-EINVAL);
  const int rc=read(RootBytes+size_t(index)*CellBytes,out,CellBytes);if(rc)return rc;
  if(out[0]<Tx||out[0]>Revoke||out[1]!=1||out[2]||out[3]||get(out+4)!=index+1||get(out+56)!=crc(out,56)||get(out+60)!=Commit)return fail(-EILSEQ);
  return 0;
}
int PkiAppendHistory::historical(uint32_t id,const uint8_t *pub,Pin *out,size_t before)const{
  for(unsigned i=0;i<initial_.stored;++i){Pin p;bool tombstone=false;const int rc=baselinePin(i,p,tombstone);if(rc)return rc;if((id&&p.node==id)||(pub&&!memcmp(pub,p.publicKey,32))){if(out)*out=p;return 1;}}
  uint8_t bytes[64];const size_t count=before<events_?before:events_;
  for(unsigned i=0;i<count;++i){const int rc=cell(i,bytes);if(rc)return rc;if(bytes[0]==Add&&((id&&get(bytes+12)==id)||(pub&&!memcmp(pub,bytes+20,32)))){if(out){Pin p;p.node=get(bytes+12);p.domain=get(bytes+8);memcpy(p.publicKey,bytes+20,32);*out=p;}return 1;}}
  return 0;
}
int PkiAppendHistory::hasId(uint32_t domain,uint32_t id,size_t before)const{
  uint8_t bytes[64];
  if(!domain){
    for(unsigned i=0;i<initial_.sent;++i){if(!base_->snapshotRead(52+size_t(i)*4,bytes,4))return fail(-EIO);if(get(bytes)==id)return 1;}
  }else if(domain<=initial_.stored){
    const size_t off=BasePeerOffset+size_t(domain-1)*BasePeerBytes;
    if(!base_->snapshotRead(off+36,bytes,1))return fail(-EIO);
    const unsigned count=bytes[0];
    for(unsigned i=0;i<count;++i){if(!base_->snapshotRead(off+40+size_t(i)*4,bytes,4))return fail(-EIO);if(get(bytes)==id)return 1;}
  }
  const size_t count=before<events_?before:events_;
  for(unsigned i=0;i<count;++i){const int rc=cell(i,bytes);if(rc)return rc;if(bytes[0]==(domain?Rx:Tx)&&get(bytes+8)==domain&&get(bytes+16)==id)return 1;}
  return 0;
}
int PkiAppendHistory::admissible(uint32_t id,const uint8_t *pub)const{
  if(!pub||!node(id)||id==initial_.node)return -EINVAL;
  uint8_t converted[32];if(!bench::MtSignature::admissiblePublicKey(pub,converted)||bench::MtSignature::nodeId(pub)!=id)return -EINVAL;
  if(!base_->snapshotRead(16,converted,32))return fail(-EIO);
  return memcmp(converted,pub,32)?0:-EINVAL;
}
int PkiAppendHistory::fold(const uint8_t bytes[CellBytes],size_t before){
  const uint8_t kind=bytes[0];const uint32_t domain=get(bytes+8),id=get(bytes+12),packet=get(bytes+16);int rc=0;
  if(kind==Tx){
    if(domain||id!=initial_.node||!packet||!zero(bytes+20,32))return fail(-EILSEQ);
    rc=hasId(0,packet,before);if(rc)return rc<0?rc:fail(-EILSEQ);++status_.sent;
  }else if(kind==Rx){
    const int at=findActive(id);if(at<0||pins_[at].domain!=domain||!packet||!zero(bytes+20,32))return fail(-EILSEQ);
    rc=hasId(domain,packet,before);if(rc)return rc<0?rc:fail(-EILSEQ);++status_.received;
  }else if(kind==Add){
    if(packet||domain!=4+get(bytes+4)||activeCount_>=ActivePeerLimit)return fail(-EILSEQ);
    rc=admissible(id,bytes+20);
    if(rc)return state_==State::Frozen?rc:fail(-EILSEQ);
    rc=historical(id,bytes+20,nullptr,before);if(rc)return rc<0?rc:fail(-EILSEQ);
    Pin &p=pins_[activeCount_++];p.node=id;p.domain=domain;memcpy(p.publicKey,bytes+20,32);++status_.stored;
  }else if(kind==Revoke){
    const int at=findActive(id);if(packet||at<0||pins_[at].domain!=domain||memcmp(pins_[at].publicKey,bytes+20,32))return fail(-EILSEQ);
    for(unsigned i=unsigned(at);i+1<activeCount_;++i)pins_[i]=pins_[i+1];
    pins_[--activeCount_]={};
  }else return fail(-EILSEQ);
  status_.active=activeCount_;return 0;
}
int PkiAppendHistory::open(const PkiLedger &base,const AppendAnchor &anchor){
  if(busy_)return -EBUSY;
  if(state_!=State::New&&state_!=State::Initialized)return -EACCES;
  if(store_.capacity()!=StoreBytes)return fail(-EXDEV);
  Busy guard(busy_);AppendAnchor selected;
  if(!base.appendAnchor(selected)||!anchorEqual(selected,anchor))return fail(-EACCES);
  Base info;int rc=baseInfo(base,info);if(rc)return fail(rc);
  if((rc=validateRoot(base,info,anchor)))return rc;
  base_=&base;initial_=info;anchor_=anchor;events_=0;activeCount_=0;lastCrc_=anchor.rootCrc;
  for(auto &p:pins_)p=Pin{};
  status_={};status_.generation=info.generation;status_.sent=info.sent;status_.received=info.received;status_.stored=info.stored;status_.format=3;
  for(unsigned i=0;i<info.stored;++i){Pin p;bool tombstone=false;if((rc=baselinePin(i,p,tombstone)))return rc;if(!tombstone)pins_[activeCount_++]=p;}
  status_.active=activeCount_;uint8_t bytes[64];bool suffix=false;
  for(unsigned i=0;i<EventLimit;++i){
    if((rc=read(RootBytes+size_t(i)*CellBytes,bytes,sizeof(bytes))))return rc;
    if(erased(bytes,sizeof(bytes))){suffix=true;continue;}
    if(suffix)return fail(-EILSEQ);
    if((rc=cell(i,bytes)))return rc;
    if(get(bytes+52)!=lastCrc_||(rc=fold(bytes,i)))return state_==State::Frozen?error_:fail(-EILSEQ);
    lastCrc_=get(bytes+56);++events_;status_.generation=initial_.generation+events_;
  }
  if(counterMode_&&events_!=EventLimit)return fail(-EILSEQ);
  state_=State::Ready;error_=0;return 0;
}
bool PkiAppendHistory::ready()const{return state_==State::Ready&&!busy_&&base_&&base_->ready();}
bool PkiAppendHistory::close(){if(busy_)return false;for(auto &p:pins_)p=Pin{};base_=nullptr;events_=activeCount_=0;counterMode_=false;status_={};state_=State::Closed;error_=0;return true;}
uint16_t PkiAppendHistory::remaining()const{return ready()?uint16_t(EventLimit-events_):0;}
bool PkiAppendHistory::status(PkiLedger::Status &out)const{if(!ready())return false;out=status_;return true;}
bool PkiAppendHistory::lookup(uint32_t id,uint8_t out[32])const{if(!ready()||!out)return false;const int at=findActive(id);if(at<0)return false;memcpy(out,pins_[at].publicKey,32);return true;}
bool PkiAppendHistory::active(uint32_t id,const uint8_t pub[32])const{if(!ready()||!pub)return false;const int at=findActive(id);return at>=0&&!memcmp(pub,pins_[at].publicKey,32);}
bool PkiAppendHistory::revoked(uint32_t id)const{if(!ready()||findActive(id)>=0)return false;Busy guard(busy_);return historical(id,nullptr)>0;}
bool PkiAppendHistory::activePeers(uint32_t *out,size_t capacity,size_t &count)const{if(!ready()||!out||capacity<activeCount_)return false;for(unsigned i=0;i<activeCount_;++i)out[i]=pins_[i].node;count=activeCount_;return true;}
int PkiAppendHistory::readBaselineChunk(size_t offset,uint8_t *out,size_t size)const{
  if(!ready())return -EACCES;
  if(!out||!size||size>64||offset>SnapshotBytes||size>SnapshotBytes-offset)return -EINVAL;
  uint8_t candidate[64],expected[64];Busy guard(busy_);
  int rc=read(Snapshot+offset,candidate,size);if(rc)return rc;
  if(!base_->snapshotRead(offset,expected,size))return fail(-EIO);
  if(memcmp(candidate,expected,size))return fail(-EILSEQ);
  memcpy(out,candidate,size);return 0;
}
int PkiAppendHistory::reconstructBaseline(AppendStore &store,uint32_t localNode,
                                          const uint8_t localPublic[32],
                                          uint8_t snapshot[PkiLedger::RecordSize],
                                          AppendAnchor &anchor){
  if(!localPublic||!snapshot||!validNode(localNode)||bench::MtSignature::nodeId(localPublic)!=localNode)return -EINVAL;
  if(store.capacity()!=StoreBytes)return -EXDEV;
  uint8_t header[Snapshot],candidate[SnapshotBytes],chunk[64],trailer[8];
  uint32_t rootCrc=~0u;
  for(size_t offset=0;offset<4088;){
    const size_t n=4088-offset>sizeof(chunk)?sizeof(chunk):4088-offset;
    const int rc=store.read(offset,chunk,n);if(rc)return rc<0?rc:-EIO;
    rootCrc=crcUpdate(rootCrc,chunk,n);
    if(offset<sizeof(header)){const size_t copy=n<sizeof(header)-offset?n:sizeof(header)-offset;memcpy(header+offset,chunk,copy);}
    const size_t start=offset>Snapshot?offset:Snapshot;
    const size_t end=offset+n<Snapshot+SnapshotBytes?offset+n:Snapshot+SnapshotBytes;
    if(start<end)memcpy(candidate+(start-Snapshot),chunk+(start-offset),end-start);
    if(offset+n>Snapshot+SnapshotBytes){
      const size_t tail=offset>Snapshot+SnapshotBytes?offset:Snapshot+SnapshotBytes;
      for(size_t i=tail;i<offset+n;++i)if(chunk[i-offset])return -EILSEQ;
    }
    offset+=n;
  }
  int rc=store.read(4088,trailer,sizeof(trailer));if(rc)return rc<0?rc:-EIO;
  const uint8_t prefix[8]={'Z','T','A','H',1,128,64,0};
  if(memcmp(header,prefix,sizeof(prefix))||get(header+8)!=StoreBytes||
     get(header+12)!=RootBytes||get(header+16)!=EventLimit||
     get(header+24)!=SnapshotBytes||get(header+32)!=localNode||
     memcmp(header+36,localPublic,32))return -EILSEQ;
  if(!get(header+20)||get(header+20)>UINT32_MAX-EventLimit)return -EILSEQ;
  for(size_t i=68;i<sizeof(header);++i)if(header[i])return -EILSEQ;
  if(!PkiLedger::validateSnapshot(candidate,sizeof(candidate))||
     get(header+20)!=get(candidate+8)||get(candidate+12)!=localNode||
     memcmp(candidate+16,localPublic,32)||get(header+28)!=get(candidate+SnapshotBytes-4)||
     get(trailer)!=~rootCrc||(get(trailer+4)!=Commit&&get(trailer+4)!=CounterCommit))return -EILSEQ;
  AppendAnchor result{get(header+20),get(header+28),get(trailer),uint32_t(StoreBytes)};
  memcpy(snapshot,candidate,sizeof(candidate));anchor=result;return 0;
}
int PkiAppendHistory::reconstructLedgerBaseline(PkiLedger &ledger,
                                                const IdentityBinding &identity,
                                                AppendAnchor &anchor)const{
  uint8_t snapshot[PkiLedger::RecordSize]{};AppendAnchor reconstructed{};
  int rc=reconstructBaseline(store_,identity.node,identity.publicKey,snapshot,reconstructed);
  if(!rc)rc=ledger.loadValidatedAppendBaseline(snapshot,sizeof(snapshot),reconstructed,
                                                identity.node,identity.publicKey);
  eraseIdentityBytes(snapshot,sizeof(snapshot));
  if(!rc)anchor=reconstructed;
  return rc;
}
bool PkiAppendHistory::sent(uint32_t id)const{if(!ready()||!id)return true;Busy guard(busy_);return hasId(0,id,events_)!=0;}
ReplayCheck PkiAppendHistory::recorded(uint32_t id,const uint8_t pub[32],uint32_t packet)const{
  if(!ready())return ReplayCheck::NotReady;
  if(!packet||!pub||!node(id))return ReplayCheck::Invalid;
  const int at=findActive(id);if(at<0)return ReplayCheck::UnknownPeer;if(memcmp(pub,pins_[at].publicKey,32))return ReplayCheck::WrongKey;
  Busy guard(busy_);const int rc=hasId(pins_[at].domain,packet,events_);return rc<0?ReplayCheck::NotReady:rc?ReplayCheck::Seen:ReplayCheck::Fresh;
}
ReplayCheck PkiAppendHistory::seen(uint32_t id,const uint8_t pub[32],uint32_t packet)const{
  const ReplayCheck result=recorded(id,pub,packet);
  return result==ReplayCheck::Fresh&&remaining()==0?ReplayCheck::Full:result;
}
ReplayCheck PkiAppendHistory::historicalRecorded(uint32_t id,const uint8_t pub[32],uint32_t packet)const{
  if(!ready())return ReplayCheck::NotReady;
  if(!packet||!pub||!node(id))return ReplayCheck::Invalid;
  Busy guard(busy_);Pin pin;const int at=findActive(id);
  if(at>=0)pin=pins_[at];
  else {const int rc=historical(id,pub,&pin);if(rc<0)return ReplayCheck::NotReady;if(!rc)return ReplayCheck::UnknownPeer;}
  if(pin.node!=id||memcmp(pin.publicKey,pub,32))return ReplayCheck::WrongKey;
  const int rc=hasId(pin.domain,packet,events_);
  return rc<0?ReplayCheck::NotReady:rc?ReplayCheck::Seen:ReplayCheck::Fresh;
}
int PkiAppendHistory::activateCounterMode(bool q){
  if(busy_)return -EBUSY;
  if(!ready())return -EACCES;
  if(counterMode_)return -EALREADY;
  if(!q)return -EAGAIN;
  if(events_!=EventLimit)return -EINVAL;
  Busy guard(busy_);uint8_t word[4];
  int rc=store_.selectCounterMode();if(rc)return fail(rc);
  rc=read(RootBytes-4,word,sizeof(word));if(rc)return rc;
  if(get(word)!=CounterCommit)return fail(-EIO);
  counterMode_=true;return 0;
}
int PkiAppendHistory::append(uint8_t kind,uint32_t domain,uint32_t id,uint32_t packet,const uint8_t *pub){
  if(events_==EventLimit)return -ENOSPC;
  const size_t offset=RootBytes+size_t(events_)*CellBytes;int rc=blank(offset,StoreBytes-offset);if(rc)return rc;
  uint8_t bytes[64]{};bytes[0]=kind;bytes[1]=1;put(bytes+4,uint32_t(events_)+1);put(bytes+8,domain);put(bytes+12,id);put(bytes+16,packet);if(pub)memcpy(bytes+20,pub,32);put(bytes+52,lastCrc_);put(bytes+56,crc(bytes,56));put(bytes+60,Commit);
  if((rc=program(offset,bytes,60))||(rc=program(offset+60,bytes+60,4)))return rc;
  uint8_t verify[64];if((rc=read(offset,verify,sizeof(verify))))return rc;if(memcmp(bytes,verify,sizeof(bytes)))return fail(-EIO);
  if((rc=fold(bytes,events_)))return rc;
  lastCrc_=get(bytes+56);++events_;status_.generation=initial_.generation+events_;error_=0;return 0;
}
int PkiAppendHistory::reserve(uint32_t id,bool q){
  if(busy_)return -EBUSY;
  if(!ready())return -EACCES;
  if(!id)return -EINVAL;
  if(!q)return -EAGAIN;
  Busy guard(busy_);
  const int rc=hasId(0,id,events_);
  if(rc)return rc<0?rc:-EALREADY;
  return append(Tx,0,initial_.node,id,nullptr);
}
int PkiAppendHistory::addTrustedPeer(uint32_t id,const uint8_t pub[32],bool q){
  if(busy_)return -EBUSY;
  if(!ready())return -EACCES;
  if(!q)return -EAGAIN;
  Busy guard(busy_);
  int rc=admissible(id,pub);
  if(rc)return rc;
  rc=historical(id,pub);
  if(rc)return rc<0?rc:-EEXIST;
  if(activeCount_>=ActivePeerLimit)return -ENOSPC;
  return append(Add,4+uint32_t(events_)+1,id,0,pub);
}
int PkiAppendHistory::revokeTrustedPeer(uint32_t id,const uint8_t pub[32],bool q){
  if(busy_)return -EBUSY;
  if(!ready()||!pub)return -EACCES;
  Busy guard(busy_);Pin p;
  const int found=historical(id,nullptr,&p);
  if(found<0)return found;
  if(!found)return -ENOENT;
  if(memcmp(pub,p.publicKey,32))return -EACCES;
  const int at=findActive(id);
  if(at<0)return 0;
  if(!q)return -EAGAIN;
  return append(Revoke,pins_[at].domain,id,0,pub);
}
int PkiAppendHistory::commitAuthenticated(uint32_t id,const uint8_t pub[32],uint32_t packet,bool q){
  if(busy_)return -EBUSY;
  if(!ready())return -EACCES;
  if(!packet||!pub||!node(id))return -EINVAL;
  const int at=findActive(id);
  if(at<0||memcmp(pub,pins_[at].publicKey,32))return -EACCES;
  if(!q)return -EAGAIN;
  Busy guard(busy_);
  const int rc=hasId(pins_[at].domain,packet,events_);
  if(rc)return rc<0?rc:-EALREADY;
  return append(Rx,pins_[at].domain,id,packet,nullptr);
}
}
