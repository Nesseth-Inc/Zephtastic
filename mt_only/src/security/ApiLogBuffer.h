// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "meshtastic/mesh.pb.h"

namespace mt::api {

// Bounded owner-context event log. Inputs are allowlisted events and numeric
// values only; arbitrary strings, packets, identifiers, and credentials cannot
// enter this stream. It is a safe subset of stock FromRadio.log_record.
class ApiLogBuffer final {
public:
  static constexpr uint32_t Capacity=16;
  enum class Consumer:uint8_t {Usb=0,Ble=1};
  enum class Event:uint8_t {
    BootReady, RadioReady, RadioRxAcceptedBytes, RadioTxQueuedBytes,
    RadioTxCompleteBytes, RadioTxRejectedErrno, RadioTxFailureErrno,
    StorageHeld, RadioFault, UsbSessionOpened, UsbSessionClosed,
    BleSessionOpened, BleSessionClosed, PowerPolicyState, UpdateOwnerState
  };
private:
  struct Item {uint32_t sequence=0,utcSeconds=0;Event event=Event::BootReady;int32_t value=0;bool utcValid=false;};
  struct Cursor {uint32_t sequence=0,dropped=0;bool active=false;};
  struct Spec {const char *source,*message;meshtastic_LogRecord_Level level;bool hasValue,signedValue;int32_t minimum,maximum;};
  Item items_[Capacity]{};Cursor cursors_[2]{};uint32_t nextSequence_=0,count_=0;bool enabled_=false;
  static bool spec(Event event,Spec &out) {
    switch(event) {
    case Event::BootReady:out={"device","boot ready",meshtastic_LogRecord_Level_INFO,false,false,0,0};return true;
    case Event::RadioReady:out={"radio","radio ready",meshtastic_LogRecord_Level_INFO,false,false,0,0};return true;
    case Event::RadioRxAcceptedBytes:out={"radio","RX admitted bytes=",meshtastic_LogRecord_Level_DEBUG,true,false,0,255};return true;
    case Event::RadioTxQueuedBytes:out={"radio","TX queued bytes=",meshtastic_LogRecord_Level_DEBUG,true,false,0,255};return true;
    case Event::RadioTxCompleteBytes:out={"radio","TX complete bytes=",meshtastic_LogRecord_Level_INFO,true,false,0,255};return true;
    case Event::RadioTxRejectedErrno:out={"radio","TX rejected errno=",meshtastic_LogRecord_Level_WARNING,true,true,-4095,4095};return true;
    case Event::RadioTxFailureErrno:out={"radio","TX failed errno=",meshtastic_LogRecord_Level_WARNING,true,true,-4095,4095};return true;
    case Event::StorageHeld:out={"storage","storage held=",meshtastic_LogRecord_Level_INFO,true,false,0,1};return true;
    case Event::RadioFault:out={"radio","radio fault=",meshtastic_LogRecord_Level_WARNING,true,false,0,1};return true;
    case Event::UsbSessionOpened:out={"usb","USB session opened",meshtastic_LogRecord_Level_INFO,false,false,0,0};return true;
    case Event::UsbSessionClosed:out={"usb","USB session closed",meshtastic_LogRecord_Level_INFO,false,false,0,0};return true;
    case Event::BleSessionOpened:out={"ble","BLE session opened",meshtastic_LogRecord_Level_INFO,false,false,0,0};return true;
    case Event::BleSessionClosed:out={"ble","BLE session closed",meshtastic_LogRecord_Level_INFO,false,false,0,0};return true;
    case Event::PowerPolicyState:out={"power","power policy state=",meshtastic_LogRecord_Level_INFO,true,false,0,5};return true;
    case Event::UpdateOwnerState:out={"update","update owner state=",meshtastic_LogRecord_Level_INFO,true,false,0,4};return true;
    }
    return false;
  }
  static unsigned index(Consumer consumer){return consumer==Consumer::Usb?0u:consumer==Consumer::Ble?1u:2u;}
  static void copyText(char *out,size_t cap,const char *text){if(!out||!cap)return;size_t i=0;if(text)for(;i+1<cap&&text[i];++i)out[i]=text[i];out[i]=0;}
  static void appendUnsigned(char *out,size_t cap,uint32_t value){size_t at=0;while(at<cap&&out[at])++at;if(at>=cap)return;char digits[10];size_t n=0;do{digits[n++]=char('0'+value%10u);value/=10u;}while(value&&n<sizeof(digits));while(n&&at+1<cap)out[at++]=digits[--n];out[at]=0;}
  static void appendSigned(char *out,size_t cap,int32_t value){size_t at=0;while(at<cap&&out[at])++at;if(at>=cap)return;uint32_t magnitude;if(value<0){if(at+1>=cap)return;out[at++]='-';magnitude=uint32_t(-int64_t(value));}else magnitude=uint32_t(value);out[at]=0;appendUnsigned(out+at,cap-at,magnitude);}
  static void render(const Item &item,meshtastic_LogRecord &out){Spec d{};memset(&out,0,sizeof(out));out.level=meshtastic_LogRecord_Level_UNSET;if(!spec(item.event,d))return;copyText(out.source,sizeof(out.source),d.source);copyText(out.message,sizeof(out.message),d.message);if(d.hasValue){if(d.signedValue)appendSigned(out.message,sizeof(out.message),item.value);else appendUnsigned(out.message,sizeof(out.message),uint32_t(item.value));}out.time=item.utcValid?item.utcSeconds:0;out.level=d.level;}
public:
  ApiLogBuffer()=default;ApiLogBuffer(const ApiLogBuffer&)=delete;ApiLogBuffer&operator=(const ApiLogBuffer&)=delete;
  // All calls are serialized by the application owner loop.
  void setEnabled(bool enabled){if(enabled_==enabled)return;enabled_=enabled;if(!enabled_){memset(items_,0,sizeof(items_));count_=0;cursors_[0]=Cursor{};cursors_[1]=Cursor{};}}
  bool enabled()const{return enabled_;}
  bool beginSession(Consumer consumer){unsigned i=index(consumer);if(!enabled_||i>=2)return false;cursors_[i]={nextSequence_,0,true};return true;}
  void endSession(Consumer consumer){unsigned i=index(consumer);if(i<2)cursors_[i]=Cursor{};}
  bool record(Event event,int32_t value,bool utcValid,uint32_t utcSeconds){if(!enabled_)return false;Spec d{};if(!spec(event,d)||(!d.hasValue&&value!=0)||(d.hasValue&&(value<d.minimum||value>d.maximum)))return false;if(count_==Capacity){uint32_t overwritten=uint32_t(nextSequence_-Capacity);for(auto &c:cursors_)if(c.active&&c.sequence==overwritten){++c.sequence;if(c.dropped!=UINT32_MAX)++c.dropped;}}items_[nextSequence_%Capacity]={nextSequence_,utcValid?utcSeconds:0,event,value,utcValid};++nextSequence_;if(count_<Capacity)++count_;return true;}
  // Called synchronously after a successful record while the owner still holds serviceLock.
  bool latest(meshtastic_LogRecord &out)const{
    if(!enabled_||!count_)return false;
    render(items_[(nextSequence_-1)%Capacity],out);return true;
  }
  bool next(Consumer consumer,meshtastic_LogRecord &out,uint32_t &dropped){dropped=0;memset(&out,0,sizeof(out));out.level=meshtastic_LogRecord_Level_UNSET;unsigned i=index(consumer);if(!enabled_||i>=2||!cursors_[i].active)return false;Cursor &c=cursors_[i];dropped=c.dropped;c.dropped=0;if(c.sequence==nextSequence_)return false;const Item &item=items_[c.sequence%Capacity];if(item.sequence!=c.sequence){if(dropped!=UINT32_MAX)++dropped;c.sequence=nextSequence_;return false;}render(item,out);++c.sequence;return true;}
};

} // namespace mt::api
