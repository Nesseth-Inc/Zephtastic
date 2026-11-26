// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "../stock/modules/StoreForwardModule.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace mt::modules {
// Bounded volatile archive policy for stock StoreAndForward text history.
// The owner supplies both arrays so constrained targets choose their own RAM
// budget. This class performs no allocation, radio/settings access, or locking.
// The caller must admit only decoded TEXT_MESSAGE_APP records from the normal
// packet path; this type intentionally has no port or cryptographic policy.
class StoreForwardArchive {
public:
  static constexpr size_t MaxPayload=233;
  static constexpr uint32_t BroadcastNode=UINT32_MAX;

  struct MessageView {
    uint32_t time=0,to=0,from=0,id=0,replyId=0;
    uint8_t channel=0,hopStart=0,hopLimit=0,transportMechanism=0;
    bool hasRxTime=false,emoji=false,hasRxRssi=false,viaMqtt=false;
    int32_t rxRssi=0;
    float rxSnr=0;
    const uint8_t *payload=nullptr;
    size_t payloadSize=0;
  };

  // This mirrors the stock PacketHistoryStruct's replay-relevant content while
  // using explicit fixed-width payload size. It is not a wire or flash ABI;
  // compiler padding is allowed and callers must budget sizeof(Record).
  struct Record {
    // Match the existing PkiRuntime fixed-storage pattern: Zephyr's minimal
    // <new> has no global placement overload. This never allocates a heap.
    static void *operator new(size_t,void *storage)noexcept{return storage;}
    uint32_t time=0,to=0,from=0,id=0,replyId=0;
    uint8_t channel=0,hopStart=0,hopLimit=0,transportMechanism=0;
    bool hasRxTime=false,emoji=false,hasRxRssi=false,viaMqtt=false;
    uint16_t payloadSize=0;
    int32_t rxRssi=0;
    float rxSnr=0;
    uint8_t payload[MaxPayload]{};
  };

  struct Cursor {
    uint32_t requester=0;
    size_t nextIndex=0;
    bool used=false;
  };

  enum class AddResult:uint8_t {Added,OverwroteOldest,Unavailable,InvalidMessage};

private:
  Record *records_=nullptr;
  size_t capacity_=0,count_=0;
  Cursor *cursors_=nullptr;
  size_t cursorCapacity_=0,nextVictim_=0;
  uint32_t epoch_=0;
  bool ready_=false;

  // Archive objects can be destroyed/re-created in the same fixed storage.
  // A per-object increment would restart at the same value and let a local
  // replay cursor skip newly appended records after runtime reconfiguration.
  // Callers serialize archive mutation; reserve zero as an invalid/sentinel
  // generation and wrap UINT32_MAX directly to 1.
  static uint32_t nextEpoch(){
    static uint32_t generation=0;
    if(++generation==0)++generation;
    return generation;
  }

  Cursor *cursorFor(uint32_t requester) {
    for(size_t i=0;i<cursorCapacity_;++i)
      if(cursors_[i].used&&cursors_[i].requester==requester)return &cursors_[i];
    for(size_t i=0;i<cursorCapacity_;++i)if(!cursors_[i].used){
      cursors_[i]=Cursor{requester,0,true};return &cursors_[i];
    }
    // The stock unordered_map is unbounded. On a small target, evict request
    // cursor metadata round-robin; the consequence is a safe replay from the
    // beginning for that requester, never cross-requester message visibility.
    Cursor &slot=cursors_[nextVictim_];
    nextVictim_=(nextVictim_+1)%cursorCapacity_;
    slot=Cursor{requester,0,true};return &slot;
  }
  static void copy(Record &out,const MessageView &in){
    out=Record{};out.time=in.time;out.to=in.to;out.from=in.from;out.id=in.id;
    out.replyId=in.replyId;out.channel=in.channel;out.hopStart=in.hopStart;
    out.hopLimit=in.hopLimit;out.transportMechanism=in.transportMechanism;
    out.hasRxTime=in.hasRxTime;out.emoji=in.emoji;out.hasRxRssi=in.hasRxRssi;
    out.viaMqtt=in.viaMqtt;out.rxRssi=in.rxRssi;out.rxSnr=in.rxSnr;
    out.payloadSize=uint16_t(in.payloadSize);
    if(in.payloadSize)memcpy(out.payload,in.payload,in.payloadSize);
  }

public:
  static void *operator new(size_t,void *storage)noexcept{return storage;}
  StoreForwardArchive(Record *records,size_t capacity,Cursor *cursors,size_t cursorCapacity)
    :records_(records),capacity_(capacity),cursors_(cursors),cursorCapacity_(cursorCapacity),
     ready_(records&&capacity&&cursors&&cursorCapacity&&capacity<=UINT32_MAX&&
            capacity<=SIZE_MAX/sizeof(Record)&&cursorCapacity<=SIZE_MAX/sizeof(Cursor)){
    if(ready_)clear();
  }
  StoreForwardArchive(const StoreForwardArchive &)=delete;
  StoreForwardArchive &operator=(const StoreForwardArchive &)=delete;

  bool ready()const{return ready_;}
  size_t size()const{return count_;}
  size_t capacity()const{return ready_?capacity_:0;}
  uint32_t epoch()const{return epoch_;}
  const Record *recordAt(size_t index)const{return ready_&&index<count_?&records_[index]:nullptr;}

  // Volatile reset (including startup) deliberately discards all archived text
  // and requester progress. This store never writes a persistence backend.
  void clear(){
    if(!ready_)return;
    memset(records_,0,sizeof(Record)*capacity_);
    memset(cursors_,0,sizeof(Cursor)*cursorCapacity_);
    count_=0;nextVictim_=0;epoch_=nextEpoch();
  }

  AddResult add(const MessageView &message){
    if(!ready_)return AddResult::Unavailable;
    if(message.payloadSize>MaxPayload||(message.payloadSize&&!message.payload))
      return AddResult::InvalidMessage;
    const bool overwrite=count_==capacity_;
    if(overwrite){
      // Stock historyAdd restarts its linear index at zero and clears every
      // requester cursor when the configured history fills.
      count_=0;
      for(size_t i=0;i<cursorCapacity_;++i)if(cursors_[i].used)cursors_[i].nextIndex=0;
      epoch_=nextEpoch();
    }
    copy(records_[count_],message);++count_;
    return overwrite?AddResult::OverwroteOldest:AddResult::Added;
  }

  // Stock count query creates a per-requester cursor but does not advance it.
  // afterTime is the strict timestamp cutoff (records with time <= cutoff and
  // records whose captured time is zero are excluded, as in stock).
  bool countAvailable(uint32_t requester,uint32_t afterTime,size_t &count,
                      size_t *lastRequest=nullptr){
    count=0;if(lastRequest)*lastRequest=0;
    if(!ready_)return false;
    Cursor *cursor=cursorFor(requester);if(!cursor)return false;
    if(lastRequest)*lastRequest=cursor->nextIndex;
    for(size_t i=cursor->nextIndex;i<count_;++i)
      if(stock::StoreForwardModule::available(records_[i],requester,afterTime))++count;
    return true;
  }

  // Prepares one eligible replay record and advances the stock-style cursor
  // before delivery. As stock replay is unacknowledged, callers must not treat
  // this selection as a delivered-message receipt.
  bool next(uint32_t requester,uint32_t afterTime,Record &out,size_t *selectedIndex=nullptr){
    if(!ready_)return false;
    Cursor *cursor=cursorFor(requester);if(!cursor)return false;
    for(size_t i=cursor->nextIndex;i<count_;++i){
      if(!stock::StoreForwardModule::available(records_[i],requester,afterTime))continue;
      out=records_[i];cursor->nextIndex=i+1;
      if(selectedIndex)*selectedIndex=i;
      return true;
    }
    return false;
  }

  bool cursor(uint32_t requester,size_t &nextIndex)const{
    nextIndex=0;if(!ready_)return false;
    for(size_t i=0;i<cursorCapacity_;++i)if(cursors_[i].used&&cursors_[i].requester==requester){
      nextIndex=cursors_[i].nextIndex;return true;
    }
    return false;
  }
};

static_assert(StoreForwardArchive::MaxPayload==233,"stock Data payload bound");
static_assert(sizeof(StoreForwardArchive::Record)<=288,"bounded text archive record RAM cost");
}
