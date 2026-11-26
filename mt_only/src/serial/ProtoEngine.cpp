// SPDX-License-Identifier: GPL-3.0-only
#include "ProtoEngine.h"

namespace mt_serial {
bool ProtoEngine::consume(uint8_t byte, ToRadio callback, void *context) {
  switch(state_) {
  case State::Marker0:
    state_ = byte==0x94 ? State::Marker1 : State::Marker0;
    break;
  case State::Marker1:
    if(byte==0xc3) state_=State::LengthHigh;
    else state_=byte==0x94 ? State::Marker1 : State::Marker0;
    break;
  case State::LengthHigh:
    expected_=size_t(byte)<<8;
    state_=State::LengthLow;
    break;
  case State::LengthLow:
    expected_|=byte;
    if(expected_>sizeof(payload_)) {
      state_=byte==0x94 ? State::Marker1 : State::Marker0;
      payloadUsed_=expected_=0;
    } else if(expected_==0) {
      if(callback) callback(context,payload_,0);
      state_=State::Marker0;
      payloadUsed_=expected_=0;
    } else {
      payloadUsed_=0;
      state_=State::Payload;
    }
    break;
  case State::Payload:
    payload_[payloadUsed_++]=byte;
    if(payloadUsed_==expected_) {
      if(callback) callback(context,payload_,payloadUsed_);
      payloadUsed_=expected_=0;
      state_=State::Marker0;
    }
    break;
  }
  return true;
}

bool ProtoEngine::receive(const uint8_t *bytes, size_t size, ToRadio toRadio,
                          void *context) {
  if(size&&!bytes) return false;
  for(size_t i=0;i<size;++i) consume(bytes[i],toRadio,context);
  return true;
}

size_t ProtoEngine::frame(const uint8_t *protobuf, size_t size, uint8_t *out,
                          size_t capacity) {
  if((size&&!protobuf)||!out||size>kMaxPhoneApiPayload||capacity<size+4) return 0;
  out[0]=0x94; out[1]=0xc3;
  out[2]=static_cast<uint8_t>((size>>8)&0xff);
  out[3]=static_cast<uint8_t>(size&0xff);
  for(size_t i=0;i<size;++i) out[i+4]=protobuf[i];
  return size+4;
}

bool ProtoEngine::emit(const uint8_t *protobuf, size_t size, ProtoWrite write,
                       void *context) {
  if(!write||size>kMaxPhoneApiPayload) return false;
  uint8_t framed[kMaxPhoneApiPayload+4];
  const size_t frameSize=frame(protobuf,size,framed,sizeof(framed));
  if(!frameSize) return false;
  write(context,framed,frameSize);
  return true;
}

bool ProtoEngine::poll(FromRadio fromRadio, ProtoWrite write, void *context) {
  return poll(fromRadio,context,write,context);
}

bool ProtoEngine::poll(FromRadio fromRadio, void *fromContext,
                       ProtoWrite write, void *writeContext) {
  if(!fromRadio||!write) return false;
  uint8_t protobuf[kMaxPhoneApiPayload];
  const size_t size=fromRadio(fromContext,protobuf,sizeof(protobuf));
  if(!size) return true;
  return emit(protobuf,size,write,writeContext);
}
} // namespace mt_serial
