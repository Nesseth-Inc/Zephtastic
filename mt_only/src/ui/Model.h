// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include "RadioView.h"
#include "GnssView.h"
#include "CannedMenu.h"
#include "../stock/ui/OnScreenKeyboardModule.h"
#include "../stock/ui/SystemCommandsModule.h"
#if defined(CONFIG_MT_BASEUI_GAMES)
#include "../stock/ui/GamesModule.h"
#endif
#include "PeerTelemetryView.h"
#if defined(CONFIG_MT_TELEMETRY_REPORT_FRAMES)
#include "ReportFrames.h"
#endif
#ifndef CONFIG_MT_UI_WIDTH
#define CONFIG_MT_UI_WIDTH 160
#endif
#ifndef CONFIG_MT_UI_HEIGHT
#define CONFIG_MT_UI_HEIGHT 80
#endif
#ifndef CONFIG_MT_UI_STRIP_ROWS
#define CONFIG_MT_UI_STRIP_ROWS CONFIG_MT_UI_HEIGHT
#endif
#ifndef CONFIG_MT_UI_MONO
#define CONFIG_MT_UI_MONO 0
#endif
namespace mt::ui {
inline constexpr unsigned MessageCapacity=32;
inline constexpr unsigned MessagePageChars=CONFIG_MT_UI_WIDTH==128?60:72;
// Display-only copies. Reading/changing pages never consumes companion queues.
struct Message {uint32_t from=0,id=0,at=0;int16_t rssi=0;int8_t snr=0;bool local=false;char text[238]{};};
struct Node {uint32_t id=0,at=0;int16_t rssi=0;int8_t snr=0;uint16_t model=0;char name[25]{},shortName[5]{};};
struct PendingCannedSend {uint8_t index=0;uint32_t destination=0xffffffffu;uint8_t channel=0;uint32_t generation=0;
#if defined(CONFIG_MT_ONSCREEN_KEYBOARD)
    char draft[stock::ui::OnScreenKeyboardModule::MaxText+1]{};
#endif
};
// Bounded local-only snapshot. Values are copied from typed sensor adapters;
// `has*` means measured, while `show*` is the saved screen preference.
struct TelemetrySnapshot {
  uint64_t sampledAtMs=0;
  bool showEnvironment=false,showPower=false,showAirQuality=false,showHealth=false,fahrenheit=false;
  bool hasTemperature=false,hasHumidity=false,hasPressure=false;
  int16_t temperatureTenthsC=0;uint16_t humidityTenths=0;uint32_t pressureHpa=0;
  bool hasPowerVoltage[3]{},hasPowerCurrent[3]{};
  uint16_t powerMilliVolts[3]{};int16_t powerMilliAmps[3]{};
  bool hasPm25=false,hasPm10=false,hasCo2=false;
  uint16_t pm25UgM3=0,pm10UgM3=0,co2Ppm=0;
  bool hasHeartRate=false,hasSpO2=false,hasBodyTemperature=false;
  uint8_t heartBpm=0,spO2Percent=0;float bodyTemperatureC=0;
  static constexpr uint64_t FreshnessMs=300000;
  bool enabled()const{return showEnvironment||showPower||showAirQuality||showHealth;}
  bool fresh(uint64_t nowMs)const{return sampledAtMs&&nowMs>=sampledAtMs&&nowMs-sampledAtMs<=FreshnessMs;}
};
enum class Page:uint8_t {Home,Messages,Nodes,Channel,Status,Canned,Telemetry
#if defined(CONFIG_MT_BASEUI_GAMES)
,Games
#endif
};
#if defined(CONFIG_MT_BASEUI_GAMES)
inline constexpr unsigned PageCount=8;
#else
inline constexpr unsigned PageCount=7;
#endif
enum class Gesture:uint8_t {Next,Previous,Hold};
enum class Input:uint8_t {Select,Up,Down,Left,Right,Back,UserPress};
enum class CannedSubstate:uint8_t {Messages,Destinations,Channels};
enum class NodeDetailSubview:uint8_t {Info,PeerAir,PeerHealth};
inline void clean(char *out,size_t cap,const uint8_t *in,size_t n) {
  if(!cap)return;
  size_t k=0;for(;k<n&&k+1<cap&&in[k];++k)out[k]=(in[k]>=32&&in[k]<127)?char(in[k]):' ';
  out[k]=0;
}
struct Model {
  Message messages[MessageCapacity]{};Node nodes[16]{};unsigned messageCount=0,nodeCount=0;
#if defined(CONFIG_MT_BASEUI_GAMES)
  stock::ui::GamesModule games;
#endif
#if defined(CONFIG_MT_LOCAL_CLOCK) && defined(CONFIG_MT_XIAO_EXPANSION_OLED)
  char clockLabel[10]="--:--";
#endif
  Page page=Page::Home;bool detail=false;bool cannedConfirm=false;unsigned selected=0,textPage=0;
  NodeDetailSubview nodeDetailSubview=NodeDetailSubview::Info;
  CannedMenu canned;uint32_t cannedDestination=0xffffffffu;uint8_t cannedChannel=0;
  bool cannedPending=false,cannedReady=true,cannedEnabled=true;uint32_t cannedGeneration=0,cannedFingerprint=0;int cannedResult=0;PendingCannedSend pendingCanned{};
  // Route candidates are projected from the existing node cache and a
  // caller-supplied channel mask; the model owns no duplicate candidate list.
  CannedSubstate cannedSubstate=CannedSubstate::Messages;
  uint8_t cannedValidChannelMask=0;uint32_t cannedRouteSourceGeneration=0;
  unsigned cannedRouteSelected=0;bool cannedRoutesSupplied=false,cannedRouteNeedsSelection=true;
#if defined(CONFIG_MT_ONSCREEN_KEYBOARD)
  stock::ui::OnScreenKeyboardModule keyboard;
  bool composing()const{return selected==canned.count()+1;}
  bool keyboardInput(unsigned code,uint32_t at){
    using Keyboard=stock::ui::OnScreenKeyboardModule;
    if(!keyboard.active)return false;
    const auto result=keyboard.input(code,at);
    if(result==Keyboard::Result::Submit){
      if(cannedRoutesSupplied){
        cannedSubstate=CannedSubstate::Destinations;cannedRouteSelected=cannedDestinationIndex(cannedDestination);
      }else cannedConfirm=true;
    }
    return true;
  }
#endif
  stock::ui::SystemCommandsModule commands;
  static constexpr uint32_t CannedBroadcast=0xffffffffu;
  void cancelCannedAction(){
#if defined(CONFIG_MT_ONSCREEN_KEYBOARD)
    keyboard.clear();memset(pendingCanned.draft,0,sizeof(pendingCanned.draft));
#endif
    cannedConfirm=false;cannedPending=false;cannedResult=0;}
  bool cannedPeerCandidate(uint32_t id)const{return id&&id!=CannedBroadcast&&id!=identity;}
  // One ordered, bounded projection serves count and indexed lookup. A failed
  // lookup leaves the caller's output unchanged; eligible IDs are never zero.
  unsigned cannedPeerScan(unsigned wanted,uint32_t *out)const {
    unsigned count=0;
    for(unsigned i=0;i<nodeCount&&i<16;++i){
      const uint32_t candidate=nodes[i].id;if(!cannedPeerCandidate(candidate))continue;
      bool duplicate=false;for(unsigned j=0;j<i;++j)if(nodes[j].id==candidate){duplicate=true;break;}
      if(duplicate)continue;
      if(out&&count==wanted){*out=candidate;return count+1;}
      ++count;
    }
    return count;
  }
  unsigned cannedPeerCount()const{return cannedPeerScan(0,nullptr);}
  bool cannedPeerAt(unsigned wanted,uint32_t &id)const{return cannedPeerScan(wanted,&id)>wanted;}
  bool cannedHasPeer(uint32_t id)const {
    if(!cannedPeerCandidate(id))return false;
    for(unsigned i=0;i<nodeCount&&i<16;++i)if(nodes[i].id==id)return true;
    return false;
  }
  unsigned cannedDestinationCount()const{return cannedRoutesSupplied?1+cannedPeerCount():0;}
  bool cannedDestinationAt(unsigned index,uint32_t &destination)const {
    if(!cannedRoutesSupplied)return false;
    if(index==0){destination=CannedBroadcast;return true;}
    return cannedPeerAt(index-1,destination);
  }
  unsigned cannedDestinationIndex(uint32_t destination)const {
    for(unsigned i=0;i<cannedDestinationCount();++i){uint32_t candidate=0;if(cannedDestinationAt(i,candidate)&&candidate==destination)return i;}
    return 0;
  }
  unsigned cannedChannelCount()const {
    unsigned count=0;for(unsigned i=0;i<8;++i)if(cannedValidChannelMask&(uint8_t(1u)<<i))++count;
    return count;
  }
  bool cannedChannelAt(unsigned index,uint8_t &channel)const {
    for(unsigned i=0;i<8;++i)if(cannedValidChannelMask&(uint8_t(1u)<<i))
      if(index--==0){channel=uint8_t(i);return true;}
    return false;
  }
  bool cannedRouteValid()const {
    return cannedRoutesSupplied&&!cannedRouteNeedsSelection&&
      (cannedDestination==CannedBroadcast||cannedHasPeer(cannedDestination))&&
      cannedChannel<8&&(cannedValidChannelMask&(uint8_t(1u)<<cannedChannel));
  }
  void setCannedRoutes(uint8_t validChannelMask,uint32_t configurationGeneration){
    const bool changed=!cannedRoutesSupplied||cannedValidChannelMask!=validChannelMask||cannedRouteSourceGeneration!=configurationGeneration;
    cannedRoutesSupplied=true;cannedValidChannelMask=validChannelMask;cannedRouteSourceGeneration=configurationGeneration;
    if(changed){cancelCannedAction();++cannedGeneration;}
    if(cannedDestination!=CannedBroadcast&&!cannedHasPeer(cannedDestination)){
      cannedDestination=0;cannedRouteNeedsSelection=true;cancelCannedAction();
    }
    if(cannedChannel>=8||!(cannedValidChannelMask&(uint8_t(1u)<<cannedChannel))){
      cannedRouteNeedsSelection=true;cancelCannedAction();
    }
  }
  void setCannedContent(const char *list,size_t length,bool ready,bool enabled){
    if(!list)length=0;if(length>200)length=200;
    uint32_t fingerprint=2166136261u;auto mix=[&](uint8_t byte){fingerprint=(fingerprint^byte)*16777619u;};
    for(size_t i=0;i<length;++i)mix(uint8_t(list[i]));
    for(unsigned shift=0;shift<32;shift+=8)mix(uint8_t(length>>shift));
    mix(ready?1:0);mix(enabled?1:0);
    if(fingerprint!=cannedFingerprint){++cannedGeneration;cancelCannedAction();selected=0;}
    cannedFingerprint=fingerprint;canned.bind(list,length);cannedReady=ready;cannedEnabled=enabled;
    if(!ready||!enabled)cancelCannedAction();
  }
  unsigned cannedViewCount()const {
    if(cannedSubstate==CannedSubstate::Messages)return canned.itemCount();
    if(cannedSubstate==CannedSubstate::Destinations)return cannedDestinationCount()+1;
    return cannedChannelCount()+1;
  }
  // Combined row projection keeps the stock message Back row exactly at
  // canned.count(), then appends bounded destination and channel choices.
  unsigned cannedPickerItemCount()const {
    return canned.itemCount()+cannedDestinationCount()+cannedChannelCount();
  }
  bool cannedPickerItemLabel(unsigned row,char *out,size_t capacity)const {
    if(row<canned.itemCount()){
      if(!out||!capacity)return false;
      const char *value=nullptr;size_t length=0;
      if(!canned.entry(row,value,length)||length>=capacity)return false;
      memcpy(out,value,length);out[length]=0;return true;
    }
    row-=canned.itemCount();
    if(row<cannedDestinationCount()){
      uint32_t destination=0;if(!cannedDestinationAt(row,destination))return false;
      if(!out||!capacity)return false;
      if(destination==CannedBroadcast){snprintf(out,capacity,"Broadcast");return true;}
      for(unsigned i=0;i<nodeCount&&i<16;++i)if(nodes[i].id==destination){
        snprintf(out,capacity,"%.24s (!%08x)",nodeLabel(nodes[i],"Peer"),unsigned(destination));return true;
      }
      return false;
    }
    row-=cannedDestinationCount();
    uint8_t channel=0;if(!cannedChannelAt(row,channel)||!out||!capacity)return false;
    snprintf(out,capacity,"Channel %u",unsigned(channel));return true;
  }
  bool cannedViewLabel(unsigned row,char *out,size_t capacity)const {
    if(!out||!capacity)return false;out[0]=0;
    if(cannedSubstate==CannedSubstate::Messages){
      const char *value=nullptr;size_t length=0;
      if(!canned.entry(row,value,length)||length>=capacity)return false;
      memcpy(out,value,length);out[length]=0;return true;
    }
    if(cannedSubstate==CannedSubstate::Destinations){
      if(row==cannedDestinationCount()){snprintf(out,capacity,"[Back]");return true;}
      uint32_t destination=0;if(!cannedDestinationAt(row,destination))return false;
      if(destination==CannedBroadcast){snprintf(out,capacity,"Broadcast");return true;}
      for(unsigned i=0;i<nodeCount&&i<16;++i)if(nodes[i].id==destination){
        snprintf(out,capacity,"%.24s (!%08x)",nodeLabel(nodes[i],"Peer"),unsigned(destination));return true;
      }
      return false;
    }
    if(row==cannedChannelCount()){snprintf(out,capacity,"[Back]");return true;}
    uint8_t channel=0;if(!cannedChannelAt(row,channel))return false;
    snprintf(out,capacity,"Channel %u",unsigned(channel));return true;
  }
  void setCannedMessages(const char *list,size_t length,bool ready,bool enabled,uint32_t destination,uint8_t channel){
    if(!list)length=0;
    if(length>200)length=200;
    uint32_t fingerprint=2166136261u;
    auto mix=[&](uint8_t byte){fingerprint=(fingerprint^byte)*16777619u;};
    for(size_t i=0;i<length;++i)mix(uint8_t(list[i]));
    for(unsigned shift=0;shift<32;shift+=8)mix(uint8_t(length>>shift));
    for(unsigned shift=0;shift<32;shift+=8)mix(uint8_t(destination>>shift));
    mix(channel);mix(ready?1:0);mix(enabled?1:0);
    const bool changed=fingerprint!=cannedFingerprint;
    if(changed){++cannedGeneration;cancelCannedAction();selected=0;}
    cannedFingerprint=fingerprint;
    canned.bind(list,length);cannedReady=ready;cannedEnabled=enabled;cannedDestination=destination;cannedChannel=channel;
  }
  void requestCannedSend(){
    const char *entry=nullptr;size_t length=0;
    if(cannedReady&&cannedEnabled&&(!cannedRoutesSupplied||cannedRouteValid())&&selected<canned.count()&&selected<256&&canned.entry(selected,entry,length)&&length){
      pendingCanned={uint8_t(selected),cannedDestination,cannedChannel,cannedGeneration};cannedPending=true;cannedResult=0;
    }
#if defined(CONFIG_MT_ONSCREEN_KEYBOARD)
    else if(composing()&&keyboard.length&&cannedReady&&cannedEnabled&&(!cannedRoutesSupplied||cannedRouteValid())){
      pendingCanned={UINT8_MAX,cannedDestination,cannedChannel,cannedGeneration};
      memcpy(pendingCanned.draft,keyboard.text,keyboard.length+1);
      cannedPending=true;cannedResult=0;keyboard.clear();
    }
#endif
    cannedConfirm=false;
  }
  bool takeCannedSend(PendingCannedSend &out){if(!cannedPending)return false;out=pendingCanned;pendingCanned={};cannedPending=false;cannedResult=1;return true;}
  uint32_t identity=0,now=0;uint64_t nowMs=0;char name[25]{};
  bool bleReady=false,bleConnected=false,usbConnected=false,radioReady=false,receiving=false,txAllowed=false,armed=false,fault=false,publicChannel=false,us=false;
  bool settingsReady=true,useLongNodeName=false;
  bool customChannel=false;
  RadioView radio;
  GnssView gnss;
  TelemetrySnapshot telemetry;
#if defined(CONFIG_MT_TELEMETRY_REPORT_FRAMES)
  ReportFrames reports;
  unsigned enabledReports() const {
    return unsigned(telemetry.showEnvironment)|(unsigned(telemetry.showPower)<<1)|
           (unsigned(telemetry.showAirQuality)<<2)|(unsigned(telemetry.showHealth)<<3);
  }
#endif
  // One selected-node peer snapshot only; kept separate from local sensors.
  PeerTelemetryView peerTelemetry;
  uint16_t cellMilliVolts=0;
  uint8_t cellPercent=0;
  bool voltageKnown=false,percentKnown=false,externalPowerKnown=false,externalPower=false;
  void powerLabel(char *out,size_t cap)const {
    if(voltageKnown&&percentKnown)
      snprintf(out,cap,"%u.%03uV ~%u%%%s",unsigned(cellMilliVolts/1000),unsigned(cellMilliVolts%1000),unsigned(cellPercent),externalPowerKnown&&externalPower?" USB":"");
    else if(voltageKnown)
      snprintf(out,cap,"%u.%03uV%s",unsigned(cellMilliVolts/1000),unsigned(cellMilliVolts%1000),externalPowerKnown&&externalPower?" USB":"");
    else snprintf(out,cap,"%s",externalPowerKnown&&externalPower?"USB / VBAT N/A":"VBAT UNAVAILABLE");
  }
  const char *channelLabel()const {
    return !radio.loaded?"CHANNEL UNKNOWN":radio.enabled?radio.channel:"CH0 DISABLED";
  }
  void frequencyLabel(char *out,size_t cap)const {
    if(!radio.loaded){snprintf(out,cap,"RADIO UNKNOWN");return;}
    if(!radio.enabled){snprintf(out,cap,"RADIO DISABLED");return;}
    if(!radio.regionUS){snprintf(out,cap,"REGION UNSET");return;}
    snprintf(out,cap,"%u.%03u MHz %ddBm",unsigned(radio.frequencyHz/1000000u),unsigned((radio.frequencyHz%1000000u)/1000u),int(radio.powerDbm));
  }
  void modemLabel(char *out,size_t cap)const {
    if(!radio.loaded||!radio.enabled){snprintf(out,cap,"%s",radio.loaded?"RADIO DISABLED":"MODEM UNKNOWN");return;}
    if(radio.bandwidthHz%1000u)snprintf(out,cap,"BW%u.%u SF%u CR4/%u",unsigned(radio.bandwidthHz/1000u),unsigned((radio.bandwidthHz%1000u)/100u),unsigned(radio.spreadingFactor),unsigned(radio.codingDenominator));
    else snprintf(out,cap,"BW%u SF%u CR4/%u",unsigned(radio.bandwidthHz/1000u),unsigned(radio.spreadingFactor),unsigned(radio.codingDenominator));
  }
  const char *nodeLabel(const Node &n,const char *fallback)const {
    const char *preferred=useLongNodeName?n.name:n.shortName;
    const char *alternate=useLongNodeName?n.shortName:n.name;
    return preferred[0]?preferred:alternate[0]?alternate:fallback;
  }
  bool text(uint32_t from,uint32_t id,const uint8_t *p,size_t n,int16_t rssi,int8_t snr,uint32_t at,bool local) {
    for(unsigned i=0;i<messageCount;++i)if(messages[i].from==from&&messages[i].id==id)return false;
    for(unsigned i=MessageCapacity-1;i>0;--i)messages[i]=messages[i-1];
    auto &m=messages[0];m={};m.from=from;m.id=id;m.at=at;m.rssi=rssi;m.snr=snr;m.local=local;
#if defined(CONFIG_MT_BASEUI_EMOTES)
    size_t used=0;
    for(size_t pos=0;pos<n&&p[pos]&&used+1<sizeof(m.text);){
      if(p[pos]>=128){
        const int found=stock::ui::emotes::match(reinterpret_cast<const char *>(p+pos),n-pos);
        if(found>=0){
          const size_t size=stock::ui::emotes::entries[found].size;
          if(used+size>=sizeof(m.text))break;
          memcpy(m.text+used,p+pos,size);used+=size;pos+=size;continue;
        }
      }
      const char ch=(p[pos]>=32&&p[pos]<127)?char(p[pos]):' ';m.text[used++]=ch;++pos;
    }
    m.text[used]=0;
#else
    clean(m.text,sizeof(m.text),p,n);
#endif
    if(messageCount<MessageCapacity)++messageCount;
    // Keep the same message visible when a new preview arrives.
    if(detail&&page==Page::Messages){if(selected+1<messageCount)++selected;else textPage=0;}
    return true;
  }
  void node(uint32_t id,const char *name,const char *shortName,uint16_t model,int16_t rssi,int8_t snr,uint32_t at) {
    const bool preserveCannedDestination=page==Page::Canned&&cannedSubstate==CannedSubstate::Destinations;
    const unsigned oldDestinationCount=cannedDestinationCount();
    const bool oldBack=preserveCannedDestination&&cannedRouteSelected==oldDestinationCount;
    const bool oldBroadcast=preserveCannedDestination&&cannedRouteSelected==0;
    uint32_t highlightedPeer=0;
    if(preserveCannedDestination&&!oldBack&&!oldBroadcast)
      cannedPeerAt(cannedRouteSelected-1,highlightedPeer);
    unsigned old=0;while(old<nodeCount&&nodes[old].id!=id)++old;
    uint32_t viewed=detail&&page==Page::Nodes&&selected<nodeCount?nodes[selected].id:0;
    if(old==nodeCount&&nodeCount<16)++nodeCount;
    if(old>15)old=15;
    for(unsigned i=old;i>0;--i)nodes[i]=nodes[i-1];
    auto &n=nodes[0];n={};n.id=id;n.at=at;n.model=model;n.rssi=rssi;n.snr=snr;
    clean(n.name,sizeof(n.name),reinterpret_cast<const uint8_t *>(name),24);
    clean(n.shortName,sizeof(n.shortName),reinterpret_cast<const uint8_t *>(shortName),4);
    if(viewed)for(unsigned i=0;i<nodeCount;++i)if(nodes[i].id==viewed){selected=i;break;}
    if(cannedRoutesSupplied&&cannedDestination!=CannedBroadcast&&!cannedHasPeer(cannedDestination)){
      cannedDestination=0;cannedRouteNeedsSelection=true;cancelCannedAction();++cannedGeneration;
    }
    if(preserveCannedDestination){
      if(oldBack)cannedRouteSelected=cannedDestinationCount();
      else if(oldBroadcast)cannedRouteSelected=0;
      else {
        const unsigned next=cannedDestinationIndex(highlightedPeer);
        uint32_t candidate=0;
        if(highlightedPeer&&cannedDestinationAt(next,candidate)&&candidate==highlightedPeer)
          cannedRouteSelected=next;
        else {cannedRouteSelected=0;cancelCannedAction();}
      }
    }
  }
  void move(bool next) {
    if(page==Page::Canned){
      if(cannedSubstate==CannedSubstate::Messages){const unsigned count=canned.itemCount();if(!count){selected=0;cannedConfirm=false;return;}selected=stock::canned::moveSelection(selected,count,next);return;}
      const unsigned count=cannedViewCount();if(!count){cannedRouteSelected=0;cancelCannedAction();return;}
      cannedRouteSelected=(cannedRouteSelected+(next?1:count-1))%count;return;
    }
    if(!detail){
      unsigned candidate=unsigned(page);
      for(unsigned i=0;i<PageCount;++i){candidate=(candidate+(next?1:PageCount-1))%PageCount;if(candidate!=unsigned(Page::Telemetry)||telemetry.enabled())break;}
      page=Page(candidate);selected=0;textPage=0;cannedConfirm=false;return;
    }
#if defined(CONFIG_MT_TELEMETRY_REPORT_FRAMES)
    if(page==Page::Telemetry){
      const unsigned n=reports.count(enabledReports());
      if(!n){detail=false;return;}
      selected=(selected%n+(next?1:n-1))%n;return;
    }
#endif
    const unsigned count=page==Page::Messages?messageCount:nodeCount;
    if(!count){detail=false;return;}
    if(selected>=count)selected=0;
    if(page==Page::Messages) {
      const unsigned pages=(strlen(messages[selected].text)+MessagePageChars-1)/MessagePageChars;
      if(next&&textPage+1<pages){++textPage;return;}
      if(!next&&textPage){--textPage;return;}
    }
    selected=(selected+(next?1:count-1))%count;textPage=0;
  }
  void input(Input in) {
    if(page==Page::Canned){
      if(cannedSubstate==CannedSubstate::Messages){
        if(in==Input::Back){if(cannedConfirm)cannedConfirm=false;else {page=Page::Home;selected=0;}return;}
        if(in==Input::Select){
          if(cannedConfirm){requestCannedSend();return;}
          if(selected==canned.count()){page=Page::Home;selected=0;return;}
#if defined(CONFIG_MT_ONSCREEN_KEYBOARD)
          if(composing()&&cannedReady&&cannedEnabled){keyboard.start(now);return;}
#endif
          if(cannedReady&&cannedEnabled&&selected<canned.count()){
            if(cannedRoutesSupplied){cannedSubstate=CannedSubstate::Destinations;cannedRouteSelected=cannedDestinationIndex(cannedDestination);}
            else cannedConfirm=true;
          }
          return;
        }
        if(in==Input::Left||in==Input::Up){if(cannedConfirm)cannedConfirm=false;else move(false);return;}
        if(in==Input::Right||in==Input::Down||in==Input::UserPress){if(cannedConfirm)cannedConfirm=false;else move(true);return;}
        return;
      }
      if(in==Input::Back){
        if(cannedSubstate==CannedSubstate::Channels)cannedSubstate=CannedSubstate::Destinations;
        else cannedSubstate=CannedSubstate::Messages;
        cannedRouteSelected=0;cancelCannedAction();return;
      }
      if(in==Input::Left||in==Input::Up){move(false);return;}
      if(in==Input::Right||in==Input::Down||in==Input::UserPress){move(true);return;}
      if(in==Input::Select){
        if(!cannedReady||!cannedEnabled)return;
        if(cannedSubstate==CannedSubstate::Destinations){
          const unsigned count=cannedDestinationCount();
          if(cannedRouteSelected==count){cannedSubstate=CannedSubstate::Messages;cannedRouteSelected=0;return;}
          uint32_t destination=0;
          if(cannedDestinationAt(cannedRouteSelected,destination)){
            cannedDestination=destination;cannedRouteNeedsSelection=true;cannedSubstate=CannedSubstate::Channels;
            cannedRouteSelected=0;uint8_t candidate=0;
            for(unsigned i=0;i<cannedChannelCount();++i)if(cannedChannelAt(i,candidate)&&candidate==cannedChannel){cannedRouteSelected=i;break;}
          }
          return;
        }
        const unsigned count=cannedChannelCount();
        if(cannedRouteSelected==count){cannedSubstate=CannedSubstate::Destinations;cannedRouteSelected=0;return;}
        uint8_t channel=0;
        if(cannedChannelAt(cannedRouteSelected,channel)){
          cannedChannel=channel;cannedRouteNeedsSelection=false;cannedSubstate=CannedSubstate::Messages;
          cannedRouteSelected=0;
          if(cannedRouteValid()&&(selected<canned.count()
#if defined(CONFIG_MT_ONSCREEN_KEYBOARD)
             ||(composing()&&keyboard.length)
#endif
            ))cannedConfirm=true;
        }
        return;
      }
      return;
    }
    if(in==Input::Select) {
#if defined(CONFIG_MT_TELEMETRY_REPORT_FRAMES)
      if(page==Page::Telemetry && telemetry.enabled()){if(!detail){detail=true;selected=0;}else move(true);return;}
#endif
      if(!detail&&((page==Page::Messages&&messageCount)||(page==Page::Nodes&&nodeCount)))
        {detail=true;selected=0;textPage=0;nodeDetailSubview=NodeDetailSubview::Info;}
      else if(detail&&page==Page::Nodes) {
        nodeDetailSubview=NodeDetailSubview((unsigned(nodeDetailSubview)+1u)%3u);
      }
      return;
    }
    if(in==Input::Back) {
      if(detail){detail=false;selected=0;textPage=0;nodeDetailSubview=NodeDetailSubview::Info;peerTelemetry={};}
      return;
    }
    if(!detail) {
      if(in==Input::Left)move(false);
      else if(in==Input::Right||in==Input::UserPress)move(true);
      return;
    }
    if(in==Input::Left||in==Input::Up)move(false);
    else if(in==Input::Right||in==Input::Down||in==Input::UserPress)move(true);
  }
  void gesture(Gesture g) {
    if(g==Gesture::Hold){input(detail?Input::Back:Input::Select);return;}
    input(g==Gesture::Next?(detail?Input::Down:Input::Right):(detail?Input::Up:Input::Left));
  }
};
// Polled active-high logical button: debounce, short/double and release-to-hold.
struct Button {
  bool raw=false,stable=false;unsigned taps=0;uint32_t changed=0,pressed=0,released=0;
  bool update(bool down,uint32_t now,Gesture &out) {
    if(down!=raw){raw=down;changed=now;}
    if(raw!=stable&&uint32_t(now-changed)>=30) {
      stable=raw;
      if(stable)pressed=now;
      else if(uint32_t(now-pressed)>=800){taps=0;out=Gesture::Hold;return true;}
      else {released=now;if(++taps==2){taps=0;out=Gesture::Previous;return true;}}
    }
    if(!stable&&!raw&&taps&&uint32_t(now-released)>=350){taps=0;out=Gesture::Next;return true;}
    return false;
  }
};
}
