// SPDX-License-Identifier: GPL-3.0-only
#include "CannedGpioInput.h"
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/atomic.h>

namespace mt::input {
namespace {
constexpr uint32_t LongPressMs=300;
constexpr uint32_t UpDownDebounceMs=50;
constexpr uint32_t SelectDebounceMs=200;
constexpr uint32_t SelectLongCode=11;
constexpr uint32_t UpLongCode=12;
constexpr uint32_t DownLongCode=13;
constexpr uint32_t DownShortCode=28;
constexpr uint32_t UpShortCode=29;
const char RotarySource[]="rotEnc1";
const char UpDownSource[]="upDown1";
}
CannedGpioInput::CannedGpioInput(){
  k_msgq_init(&queue_,queueStorage_,sizeof(Edge),QueueDepth);
  for(unsigned i=0;i<3;++i){pins_[i].owner=this;pins_[i].index=uint8_t(i);}
}
CannedGpioInput::~CannedGpioInput(){stopPins();}
bool CannedGpioInput::validEvent(const meshtastic_ModuleConfig_CannedMessageConfig_InputEventChar &e){
  using U=__underlying_type(meshtastic_ModuleConfig_CannedMessageConfig_InputEventChar);
  U raw{};memcpy(&raw,&e,sizeof(raw));const uint64_t n=uint64_t(raw);
  return n==0||n==10||n==17||n==18||n==19||n==20||n==24||n==27;
}
bool CannedGpioInput::validBool(const bool &value){
  uint8_t raw=0;memcpy(&raw,&value,sizeof(raw));return raw<=1;
}
bool CannedGpioInput::validSource(const char *value,size_t capacity){
  if(!value||!capacity)return false;const size_t n=strnlen(value,capacity);
  if(n==capacity)return false;
  for(size_t i=n+1;i<capacity;++i)if(value[i])return false;
  return true;
}
bool CannedGpioInput::pinsAvailable(){
#if DT_HAS_ALIAS(canned_rotary_a) && DT_HAS_ALIAS(canned_rotary_b) && DT_HAS_ALIAS(canned_rotary_press)
  return true;
#else
  return false;
#endif
}
bool CannedGpioInput::hardwareAvailable(){
  if(!pinsAvailable())return false;
  for(unsigned i=0;i<3;++i){const auto s=specFor(i);if(!s.port||!device_is_ready(s.port))return false;}
  return true;
}
gpio_dt_spec CannedGpioInput::specFor(unsigned pin){
  switch(pin){
#if DT_HAS_ALIAS(canned_rotary_a)
  case A:return GPIO_DT_SPEC_GET(DT_ALIAS(canned_rotary_a),gpios);
#endif
#if DT_HAS_ALIAS(canned_rotary_b)
  case B:return GPIO_DT_SPEC_GET(DT_ALIAS(canned_rotary_b),gpios);
#endif
#if DT_HAS_ALIAS(canned_rotary_press)
  case Press:return GPIO_DT_SPEC_GET(DT_ALIAS(canned_rotary_press),gpios);
#endif
  default:return {};
  }
}
bool CannedGpioInput::pinMatches(unsigned pin,uint32_t configuredPin)const{
  const auto &s=pins_[pin].spec;
  return s.port&&device_is_ready(s.port)&&configuredPin<=255u&&configuredPin==s.pin&&
    (s.dt_flags&GPIO_ACTIVE_LOW)!=0;
}
bool CannedGpioInput::configSupported(const meshtastic_ModuleConfig_CannedMessageConfig &c)const{
  if(!validBool(c.rotary1_enabled)||!validBool(c.updown1_enabled)||!validBool(c.enabled)||!validBool(c.send_bell)||
     !validEvent(c.inputbroker_event_cw)||!validEvent(c.inputbroker_event_ccw)||
     !validEvent(c.inputbroker_event_press)||!validSource(c.allow_input_source,sizeof(c.allow_input_source))||
     (c.rotary1_enabled&&c.updown1_enabled))return false;
  if(!c.rotary1_enabled&&!c.updown1_enabled)return true;
  if(!hardwareAvailable())return false;
  const auto a=specFor(A),b=specFor(B),press=specFor(Press);
  auto match=[](const gpio_dt_spec &s,uint32_t configured){
    return s.port&&configured<=255u&&configured==s.pin&&(s.dt_flags&GPIO_ACTIVE_LOW)!=0;
  };
  if(!match(a,c.inputbroker_pin_a)||!match(b,c.inputbroker_pin_b)||
     !match(press,c.inputbroker_pin_press))return false;
  if(a.port==b.port&&a.pin==b.pin)return false;
  if(a.port==press.port&&a.pin==press.pin)return false;
  if(b.port==press.port&&b.pin==press.pin)return false;
  return true;
}
void CannedGpioInput::gpioHandler(const device *port,gpio_callback *callback,uint32_t changed){
  auto *slot=CONTAINER_OF(callback,PinSlot,callback);
  if(!slot||!slot->owner||slot->spec.port!=port||!(changed&(1u<<slot->spec.pin)))return;
  const int raw=gpio_pin_get_raw(port,slot->spec.pin);
  if(raw<0)return;
  Edge edge{uint32_t(k_uptime_get_32()),slot->index,uint8_t(raw?1:0)};
  (void)slot->owner->queueEdge(edge);
}
bool CannedGpioInput::queueEdge(const Edge &edge){
  if(k_msgq_put(&queue_,&edge,K_NO_WAIT)==0)return true;
  (void)atomic_inc(&dropped_);atomic_set(&overflow_,1);return false;
}
void CannedGpioInput::stopPins(){
  for(auto &p:pins_){
    if(p.registered){
      (void)gpio_pin_interrupt_configure_dt(&p.spec,GPIO_INT_DISABLE);
      (void)gpio_remove_callback(p.spec.port,&p.callback);p.registered=false;
    }
  }
}
void CannedGpioInput::disable(){
  stopPins();configured_=enabledRotary_=enabledUpDown_=false;
  atomic_set(&overflow_,0);rotaryCwSet_=rotaryCcwSet_=false;
  for(unsigned i=0;i<3;++i){down_[i]=false;downAt_[i]=lastShortAt_[i]=lastLongAt_[i]=0;longSent_[i]=false;}
  k_msgq_purge(&queue_);
}
CannedGpioInput::Result CannedGpioInput::initPins(const meshtastic_ModuleConfig_CannedMessageConfig &c){
  if(!pinsAvailable())return Result::HardwareAbsent;
  for(unsigned i=0;i<3;++i)pins_[i].spec=specFor(i);
  if(!pinMatches(A,c.inputbroker_pin_a)||!pinMatches(B,c.inputbroker_pin_b)||
     !pinMatches(Press,c.inputbroker_pin_press))return Result::InvalidConfig;
  for(unsigned i=0;i<3;++i)for(unsigned j=i+1;j<3;++j)
    if(pins_[i].spec.port==pins_[j].spec.port&&pins_[i].spec.pin==pins_[j].spec.pin)return Result::InvalidConfig;
  for(auto &p:pins_){
    if(gpio_pin_configure_dt(&p.spec,GPIO_INPUT|GPIO_PULL_UP)<0)return Result::HardwareError;
    gpio_init_callback(&p.callback,&CannedGpioInput::gpioHandler,1u<<p.spec.pin);
    if(gpio_add_callback(p.spec.port,&p.callback)<0){stopPins();return Result::HardwareError;}
    p.registered=true;
    if(gpio_pin_interrupt_configure_dt(&p.spec,GPIO_INT_EDGE_BOTH)<0){stopPins();return Result::HardwareError;}
    const int raw=gpio_pin_get_raw(p.spec.port,p.spec.pin);
    if(raw<0){stopPins();return Result::HardwareError;}
    raw_[p.index]=uint8_t(raw?1:0);
  }
  return Result::Ok;
}
CannedGpioInput::Result CannedGpioInput::configure(const meshtastic_ModuleConfig_CannedMessageConfig &c){
  disable();
  if(!validBool(c.rotary1_enabled)||!validBool(c.updown1_enabled)||!validBool(c.enabled)||!validBool(c.send_bell)||
     !validEvent(c.inputbroker_event_cw)||!validEvent(c.inputbroker_event_ccw)||
     !validEvent(c.inputbroker_event_press)||!validSource(c.allow_input_source,sizeof(c.allow_input_source)))
    return Result::InvalidConfig;
  if(c.rotary1_enabled&&c.updown1_enabled)return Result::InvalidConfig;
  config_=c;enabledRotary_=c.rotary1_enabled;enabledUpDown_=c.updown1_enabled;
  if(!enabledRotary_&&!enabledUpDown_){configured_=true;return Result::Disabled;}
  const auto result=initPins(c);
  if(result!=Result::Ok){enabledRotary_=enabledUpDown_=false;return result;}
  const bool pressActive=(raw_[Press]==0);
  down_[2]=pressActive;
  if(pressActive)downAt_[2]=uint32_t(k_uptime_get_32());
  configured_=true;return Result::Ok;
}
void CannedGpioInput::emit(EventSink sink,void *context,uint8_t code,const char *source,uint32_t at){
  if(sink&&code)sink(context,Event{code,source,at});
}
void CannedGpioInput::process(const Edge &e,EventSink sink,void *context){
  if(e.pin>Press)return;
  const uint8_t previous=raw_[e.pin];raw_[e.pin]=e.raw;
  if(enabledRotary_){
    const bool rising=previous==0&&e.raw==1;
    const bool falling=previous==1&&e.raw==0;
    if(e.pin==A){
      if(rising&&!raw_[B]){
        if(!rotaryCcwSet_){rotaryCcwSet_=true;if(!down_[2])emit(sink,context,uint8_t(config_.inputbroker_event_ccw),RotarySource,e.at);}
      }else if(falling&&raw_[B])rotaryCcwSet_=false;
    }else if(e.pin==B){
      if(rising&&!raw_[A]){
        if(!rotaryCwSet_){rotaryCwSet_=true;if(!down_[2])emit(sink,context,uint8_t(config_.inputbroker_event_cw),RotarySource,e.at);}
      }else if(falling&&raw_[A])rotaryCwSet_=false;
    }else{
      const bool pressed=e.raw==0;
      if(pressed&&!down_[2]){down_[2]=true;downAt_[2]=e.at;longSent_[2]=false;}
      else if(!pressed&&down_[2]){
        const uint32_t duration=e.at-downAt_[2];
        if(duration<LongPressMs&&uint32_t(e.at-lastShortAt_[2])>=SelectDebounceMs){
          lastShortAt_[2]=e.at;emit(sink,context,uint8_t(config_.inputbroker_event_press),RotarySource,e.at);
        }
        down_[2]=false;longSent_[2]=false;
      }
    }
    return;
  }
  if(!enabledUpDown_)return;
  const unsigned key=e.pin==A?0:e.pin==B?1:2;
  const bool pressed=e.raw==0;
  if(pressed&&!down_[key]){down_[key]=true;downAt_[key]=e.at;longSent_[key]=false;lastLongAt_[key]=0;}
  else if(!pressed&&down_[key]){
    const uint32_t duration=e.at-downAt_[key];
    const uint32_t debounce=key==2?SelectDebounceMs:UpDownDebounceMs;
    if(duration<LongPressMs&&uint32_t(e.at-lastShortAt_[key])>=debounce){
      lastShortAt_[key]=e.at;
      const uint8_t code=key==0?UpShortCode:key==1?DownShortCode:uint8_t(meshtastic_ModuleConfig_CannedMessageConfig_InputEventChar_SELECT);
      emit(sink,context,code,UpDownSource,e.at);
    }
    down_[key]=false;longSent_[key]=false;lastLongAt_[key]=0;
  }
}
void CannedGpioInput::tick(uint32_t now,EventSink sink,void *context){
  if(enabledRotary_&&down_[2]&&!longSent_[2]&&uint32_t(now-downAt_[2])>=LongPressMs){
    emit(sink,context,SelectLongCode,RotarySource,now);longSent_[2]=true;
  }
  if(!enabledUpDown_)return;
  for(unsigned i=0;i<3;++i){
    if(!down_[i]||uint32_t(now-downAt_[i])<LongPressMs)continue;
    const uint32_t repeat=i==2?0:300;
    if(longSent_[i]&&(i==2||uint32_t(now-lastLongAt_[i])<repeat))continue;
    const uint8_t code=i==2?SelectLongCode:i==0?UpLongCode:DownLongCode;
    emit(sink,context,code,UpDownSource,now);longSent_[i]=true;lastLongAt_[i]=now;
    return; // defer another simultaneous hold to the next bounded owner poll
  }
}
size_t CannedGpioInput::poll(uint32_t now,EventSink sink,void *context,size_t maxEvents){
  if(!configured_)return 0;
  if(atomic_cas(&overflow_,1,0)){
    // A lost edge invalidates the quadrature/button edge history. Resynchronize
    // to live levels and suppress gestures already in progress.
    k_msgq_purge(&queue_);rotaryCwSet_=rotaryCcwSet_=false;
    for(unsigned i=0;i<3;++i){
      const int raw=gpio_pin_get_raw(pins_[i].spec.port,pins_[i].spec.pin);
      if(raw>=0)raw_[i]=uint8_t(raw?1:0);
      down_[i]=(raw_[i]==0);downAt_[i]=now;longSent_[i]=true;
    }
  }
  size_t emitted=0;Edge edge{};size_t drained=0;
  while(drained<QueueDepth&&emitted<maxEvents&&k_msgq_get(&queue_,&edge,K_NO_WAIT)==0){
    struct CounterSink {
      EventSink sink;void *context;size_t *count;size_t max;
      static void call(void *v,const Event &e){auto &c=*static_cast<CounterSink*>(v);if(*c.count<c.max){++*c.count;if(c.sink)c.sink(c.context,e);}}
    } counter{sink,context,&emitted,maxEvents};
    process(edge,&CounterSink::call,&counter);++drained;
  }
  struct CounterSink {
    EventSink sink;void *context;size_t *count;size_t max;
    static void call(void *v,const Event &e){auto &c=*static_cast<CounterSink*>(v);if(*c.count<c.max){++*c.count;if(c.sink)c.sink(c.context,e);}}
  } counter{sink,context,&emitted,maxEvents};
  if(emitted<maxEvents)tick(now,&CounterSink::call,&counter);
  return emitted;
}
}
