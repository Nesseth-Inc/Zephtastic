// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "MqttAtomic.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace mt::mqtt {
struct DownlinkMessage {
  static constexpr size_t TopicCapacity=160,PayloadCapacity=1024;
  size_t topicLength=0,payloadLength=0;
  char topic[TopicCapacity]{};
  uint8_t payload[PayloadCapacity]{};
};

// Bounded SPSC mailbox: MQTT worker owns push(), the main/service loop owns
// pop(). Queue slots may live in PSRAM; only atomic indices remain in internal
// RAM. Overflow and malformed sizes are explicit refusal results.
template<size_t Capacity>
class DownlinkMailbox {
 public:
  struct Slot {
    size_t topicLength=0,payloadLength=0;
    char topic[DownlinkMessage::TopicCapacity]{};
    uint8_t payload[DownlinkMessage::PayloadCapacity]{};
  };
  explicit DownlinkMailbox(Slot (&slots)[Capacity]):slots_(slots){}

  bool push(const char *topic,size_t topicLength,const uint8_t *payload,size_t payloadLength) {
    if(!topic||!payload||topicLength==0||topicLength>=DownlinkMessage::TopicCapacity||
       payloadLength==0||payloadLength>DownlinkMessage::PayloadCapacity||
       topic[topicLength]!='\0')return false;
    const size_t tail=tail_.load(AtomicOrder::Relaxed),head=head_.load(AtomicOrder::Acquire);
    if(tail-head>=Capacity)return false;
    Slot &slot=slots_[tail%Capacity];slot.topicLength=topicLength;slot.payloadLength=payloadLength;
    memcpy(slot.topic,topic,topicLength);slot.topic[topicLength]='\0';
    memcpy(slot.payload,payload,payloadLength);tail_.store(tail+1,AtomicOrder::Release);return true;
  }
  bool pop(DownlinkMessage &out) {
    const size_t head=head_.load(AtomicOrder::Relaxed),tail=tail_.load(AtomicOrder::Acquire);
    if(head==tail)return false;
    Slot &slot=slots_[head%Capacity];DownlinkMessage next;
    next.topicLength=slot.topicLength;next.payloadLength=slot.payloadLength;
    memcpy(next.topic,slot.topic,slot.topicLength+1);memcpy(next.payload,slot.payload,slot.payloadLength);
    memset(&slot,0,sizeof(slot));head_.store(head+1,AtomicOrder::Release);out=next;return true;
  }
  size_t queued()const{return tail_.load(AtomicOrder::Acquire)-head_.load(AtomicOrder::Acquire);}
  void clear() {
    head_.store(0,AtomicOrder::Relaxed);tail_.store(0,AtomicOrder::Relaxed);
    for(size_t i=0;i<Capacity;++i)memset(&slots_[i],0,sizeof(slots_[i]));
  }
 private:
  Slot (&slots_)[Capacity];Atomic<size_t> head_{0},tail_{0};
};
} // namespace mt::mqtt
