// SPDX-License-Identifier: GPL-3.0-only
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cmath>
#include <limits>
#include <cstdio>
#include <string>
#include <vector>
#include "stock/optional/LarkWeather.h"
#include "meshtastic/mesh.pb.h"
#include "meshtastic/telemetry.pb.h"

namespace donor {
enum class ProcessMessage{CONTINUE};
static uint32_t ticks=0,utc=0;static unsigned adc=0;
static uint32_t millis(){return ticks;}
struct Time{static uint32_t getMillis(){return ticks;}static uint32_t skipZero(uint32_t v){return v?v:1;}};
struct Throttle{static bool hasElapsed(uint32_t start,uint32_t interval){return uint32_t(ticks-start)>=interval;}};
struct NodeDB{uint32_t getNodeNum(){return 42;} void *getMeshNode(int){return nullptr;}} nodeInstance,*nodeDB=&nodeInstance;
struct Channels{int getPrimaryIndex(){return 0;}} channels;
static bool isBroadcast(uint32_t id){return id==UINT32_MAX;}
static int8_t getHopsAway(const meshtastic_MeshPacket&,int8_t defaultIfUnknown=-1);
static meshtastic_MeshPacket result{};
static meshtastic_MeshPacket* allocDataPacket(){result={};return &result;}
static std::vector<std::string> logs;
static int sent=0;
struct Service{void sendToMesh(meshtastic_MeshPacket*,int=0){++sent;}} provider,*service=&provider;
static const meshtastic_MeshPacket *currentRequest=nullptr;
static struct {char short_name[5]="BASE";char long_name[25]="Wind station";}owner;
enum {A1,SEC_PER_DAY=86400,RX_SRC_LOCAL=1};
enum class RTCQuality{RTCQualityDevice};
static int analogRead(int){return adc;}
static uint32_t getValidTime(RTCQuality,bool){return utc;}
namespace graphics {void decomposeTime(uint32_t t,int &h,int &m,int &s){h=t%86400/3600;m=t/60%60;s=t%60;}}

struct Sensor{bool present=false;bool hasSensor(){return present;}void getMetrics(meshtastic_Telemetry *t){
 auto &m=t->variant.environment_metrics;m.wind_speed=7.25f;m.wind_direction=157;m.temperature=-7.75f;m.barometric_pressure=1003.5f;}};
struct ReplyBotModule {
 bool isCommand(const char *)const; ProcessMessage handleReceived(const meshtastic_MeshPacket&);void sendDm(const meshtastic_MeshPacket&,const char*);
};
struct GenericThreadModule {bool firstTime=true;int my_interval=10000;int disable(){return -1;}int32_t runOnce();};
struct DropzoneModule {uint32_t startSendConditions=0;Sensor sensor;int32_t runOnce();
 ProcessMessage handleReceived(const meshtastic_MeshPacket&);meshtastic_MeshPacket *sendConditions();};
struct ReplyModule {meshtastic_MeshPacket *allocReply();};
#define LOG_INFO(text,...) logs.emplace_back(text)
#define LOG_DEBUG(...)
#define LOG_ERROR(...)
#include "stock.inc"
}
void endpointCases();
static std::string body(const meshtastic_MeshPacket &q){return {reinterpret_cast<const char*>(q.decoded.payload.bytes),q.decoded.payload.size};}
int main(){
 using namespace mt::stock::optional;
 for(float v:{-1e10f,-273.15f,-40.f,-1.f,0.f,.125f,37.f,1003.5f,1e10f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}){
  auto equal=[](float a,float b){return (std::isnan(a)&&std::isnan(b))||a==b;};
  assert(equal(mt::stock::UnitConversions::CelsiusToFahrenheit(v),donor::UnitConversions::CelsiusToFahrenheit(v)));
  assert(equal(mt::stock::UnitConversions::MetersPerSecondToKnots(v),donor::UnitConversions::MetersPerSecondToKnots(v)));
  assert(equal(mt::stock::UnitConversions::HectoPascalToInchesOfMercury(v),donor::UnitConversions::HectoPascalToInchesOfMercury(v)));
  assert(equal(mt::stock::UnitConversions::displaySafeFloat(v),donor::UnitConversions::displaySafeFloat(v)));
 }
 ReplyBotModule bot;donor::ReplyBotModule reference;
 meshtastic_MeshPacket p{};p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
 std::vector<std::string> words={"/ping","/PING"," /ping","\t/hello world","/testing","/test\n","/pingpong","",
   std::string("/test\0suffix",12),"/ping\rxxx"};
 unsigned count=0;
 for(uint32_t now:{100u,200u,15110u,60120u,UINT32_MAX-12,3u})
 for(uint32_t dest:{42u,UINT32_MAX,912u})for(unsigned slot:{0u,1u})for(const auto &txt:words){
  donor::ticks=now;p.from=3+count%19;p.to=dest;p.channel=slot;p.hop_start=count%5;p.hop_limit=count%3;
  p.decoded.has_bitfield=count&1;p.rx_rssi=count%2?48:-87;p.rx_snr=(int(count%39)-19)*0.25f;
  p.decoded.payload.size=txt.size();memcpy(p.decoded.payload.bytes,txt.data(),txt.size());donor::sent=0;reference.handleReceived(p);
  unsigned calls=0;
  bot.handleReceived({p.from,dest,99,meshtastic_PortNum_TEXT_MESSAGE_APP,slot,p.hop_start,p.hop_limit,false,false,p.decoded.has_bitfield,false,
                      static_cast<int16_t>(p.rx_rssi),p.rx_snr,p.decoded.payload.bytes,txt.size()},42,0,3,now,[&](const Reply &r){
   assert(body(donor::result)==r.text&&r.to==donor::result.to&&r.channel==donor::result.channel&&!r.wantAck&&!r.requestId);++calls;
  });
  assert(calls==unsigned(donor::sent));++count;
 }
 donor::currentRequest=&p;donor::ReplyModule example;assert(body(*example.allocReply())==ExampleAnswer);
 donor::GenericThreadModule generic;GenericThreadModule scheduled;std::vector<std::string> observed;
 for(uint32_t now:{11u,12u,10010u,10011u,20011u}){
  if(now==11||now==10011||now==20011)assert(generic.runOnce()==10000);
  scheduled.poll(now,[&](const char *line){observed.emplace_back(line);});
 }assert(observed==donor::logs);
 for(uint32_t start:{0u,UINT32_MAX-16}){
  DropzoneModule timer;Request q{};q.port=meshtastic_PortNum_TEXT_MESSAGE_APP;
  static constexpr char conditions[]="BASE conditions";
  q.payload=reinterpret_cast<const uint8_t *>(conditions);q.length=sizeof(conditions)-1;
  timer.handleReceived(q,"BASE","Wind station",start);
  assert(!timer.due(start)&&!timer.due(start+4999)&&timer.due(start+5000)&&!timer.due(start+5001));
 }
 for(const char *str:{"BaSe CoNdiTions","Wind station conditions later","BASE condition","/ping","BASE conditions "})
 for(bool weather:{false,true})for(unsigned raw:{0u,99u,100u,1023u}){
  donor::DropzoneModule stock;DropzoneModule module;p.decoded.payload.size=strlen(str);memcpy(p.decoded.payload.bytes,str,strlen(str));
  donor::ticks=100;donor::utc=1712345678;donor::adc=raw;stock.sensor.present=weather;stock.handleReceived(p);
  module.handleReceived({3,42,102,1,0,3,2,false,false,true,false,0,0,p.decoded.payload.bytes,strlen(str)},"BASE","Wind station",100);
  for(uint32_t t:{1099u,5099u,5100u,6100u}){donor::ticks=t;donor::sent=0;assert(stock.runOnce()==1000);
    bool due=module.due(t);assert(due==(donor::sent!=0));if(due){
      Conditions input{raw<100,weather,7.25f,1003.5f,-7.75f,157};char msg[200];module.format(input,donor::utc,msg);assert(msg==body(donor::result));
    }
  }
 }
 LarkWeather station;Conditions measurements;
 const char *keys[]={"Temp","Speed","Pressure","Dir"},*values[]={"-7.75","7.25","1003.5","SSE"};
 unsigned i=0,part=0;uint32_t now=0;
 while(i<4){
    const bool done=station.poll(now++,measurements,[&](const uint8_t *bytes,size_t n){
      assert(!part&&bytes[0]==0&&bytes[1]==strlen(keys[i])&&!bytes[2]&&n==strlen(keys[i])+3&&!memcmp(bytes+3,keys[i],n-3));return true;
    },[&](uint8_t *out,size_t n){
      switch(part++){case 0:assert(n==1);out[0]=0x53;break;case 1:assert(n==1);out[0]=0;break;
      case 2:assert(n==2);out[0]=strlen(values[i]);out[1]=0;break;
      default:assert(n==strlen(values[i]));memcpy(out,values[i++],n);part=0;break;}return true;
    });
    assert(done==(i==4));
 }
 assert(measurements.hasWeather&&measurements.bearing==157&&measurements.celsius==-7.75f&&measurements.pressureHpa==1003.5f);
 printf("PASS %u ReplyBot donor outcomes, generic cadence, example and Dropzone wire/clock/ADC/weather cases\n",count);
 endpointCases();
}
