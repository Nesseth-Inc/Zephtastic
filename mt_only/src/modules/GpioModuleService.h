// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "../Service.h"
#include "GpioModuleRuntime.h"
#include "GpioModuleChannels.h"
#include "GpioModuleWire.h"

namespace mt::modules {
// The same adapter is used by the firmware owner and native integration
// fixtures. GPIO policy never gets a separate radio or a client owner slot.
class GpioModuleService {
public:
  using LocalPacket=void(*)(void *,const uint8_t *,size_t,uint32_t,unsigned);
private:
  Service &service_;
  GpioModuleRuntime &runtime_;
  LocalPacket local_=nullptr;void *localContext_=nullptr;
  uint32_t now_=0,random_=0;
  static bool send(void *context,const GpioModuleRuntime::Outbound &packet) {
    return static_cast<GpioModuleService *>(context)->send(packet);
  }
  bool send(const GpioModuleRuntime::Outbound &packet) {
    uint32_t id=0;
    const int result=service_.submitGpioModule(packet.portnum,packet.payload,packet.payloadSize,
      packet.to,packet.requestId,packet.channel,packet.hopLimit,packet.wantAck,now_,random_++,id);
    if(result)return false;
    if(local_&&packet.to==UINT32_MAX&&!packet.requestId) {
      uint8_t data[64]{};size_t size=0;
      if(GpioModuleWire::outgoing(packet.portnum,packet.payload,packet.payloadSize,0,data,sizeof(data),size))
        local_(localContext_,data,size,id,packet.channel);
    }
    return true;
  }
public:
  GpioModuleService(Service &service,GpioModuleRuntime &runtime,LocalPacket local=nullptr,void *context=nullptr):
    service_(service),runtime_(runtime),local_(local),localContext_(context){}
  bool configure(const GpioModuleSettings &settings,bool ready,uint32_t contextEpoch) {
    uint8_t named=0;
    for(unsigned slot=0;slot<8;++slot)
      if(GpioModuleChannels::gpio(service_.config,slot))named|=uint8_t(1u<<slot);
    return runtime_.configure(settings,ready,named!=0,named,contextEpoch);
  }
  void receive(const uint8_t *frame,size_t size,const bench::RouteMetadata &metadata,
               unsigned slot,uint32_t now,uint32_t random) {
    if(!frame||size<17||size>255||slot>=8)return;
    const auto to=bench::le32(frame),from=bench::le32(frame+4);
    if((to!=UINT32_MAX&&to!=service_.route.identity())||from<=1||from>=0xfffffffeu||
       from==service_.route.identity()||!bench::le32(frame+8)||
       metadata.portnum!=meshtastic_PortNum_REMOTE_HARDWARE_APP)return;
    meshtastic_HardwareMessage message{};
    if(!GpioModuleWire::hardware(metadata.payload,metadata.payloadLength,message))return;
    GpioModuleRuntime::RequestContext context{};
    context.from=from;context.packetId=bench::le32(frame+8);context.channel=uint8_t(slot);
    context.replyHopLimit=uint8_t(Discovery::responseHop(frame,service_.config.hopLimit()));
    context.gpioChannel=GpioModuleChannels::gpio(service_.config,slot);
    context.wantAck=(frame[12]&8)!=0;
    now_=now;random_=random;
    (void)runtime_.handle(message,context,send,this);
  }
  void poll(uint32_t now,uint32_t random) {
    now_=now;random_=random;
    runtime_.poll(now,GpioModuleChannels::stockDefaultPrimary(service_.config),
      uint8_t(service_.config.hopLimit()),send,this);
  }
};
}
