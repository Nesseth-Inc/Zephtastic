// SPDX-License-Identifier: GPL-3.0-only
// Host device stubs and behavioral assertions; production body is inserted by test-display.py.

#include "ui/Render.h"
#include "Version.h"
#include "PrimaryChannel.hpp"
#if defined(CONFIG_MT_XIAO_EXPANSION_OLED)
#include "update/FirmwareUpdateController.h"
#endif
#include "stock/message/TextMessageModule.h"
#include "Observed.h"
#include <cassert>
#include <float.h>
#include <iostream>
#include <math.h>
struct device {int id;};static device panelDev{1},ddisp_pwr_enable{2},dvext_enable{3};
#define DT_CHOSEN(x) 0
#define DT_NODELABEL(x) x
#define DEVICE_DT_GET(x) (&panelDev)
#define DEV_JOIN(x) (&d##x)
#define DEVICE_DT_GET_OR_NULL(x) DEV_JOIN(x)
#define DT_NODE_HAS_STATUS_OKAY(x) 1
#define DT_ALIAS(x) 0
#define GPIO_DT_SPEC_GET(x,y) {}
#define GPIO_INPUT 0
struct gpio_dt_spec {};
static uint32_t clockNow=0;static int keyDown=0,writes=0,on=0,off=0,lightOn=0,lightOff=0,failWrite=0,failBlankOff=0,failBlankOn=0,failLightOn=0;
static bool device_is_ready(const device*){return true;}
static bool gpio_is_ready_dt(const gpio_dt_spec*){return true;}
static int gpio_pin_configure_dt(const gpio_dt_spec*,int){return 0;}
static int gpio_pin_get_dt(const gpio_dt_spec*){return keyDown;}
static uint32_t k_uptime_get_32(){return clockNow;}
constexpr int PIXEL_FORMAT_MONO01=1,PIXEL_FORMAT_RGB_565=2,PIXEL_FORMAT_RGB_565X=4,SCREEN_INFO_MONO_VTILED=8;
struct display_capabilities {unsigned x_resolution=0,y_resolution=0,supported_pixel_formats=0,screen_info=0;};
struct display_buffer_descriptor {unsigned width=0,height=0,pitch=0,buf_size=0;};
static void display_get_capabilities(const device*,display_capabilities*c){c->x_resolution=mt::ui::Canvas::Width;c->y_resolution=mt::ui::Canvas::Height;c->supported_pixel_formats=7;c->screen_info=8;}
[[maybe_unused]] static int display_set_pixel_format(const device*,int){return 0;}
#if defined(CONFIG_MT_XIAO_EXPANSION_OLED)
static int contrastWritten=-1,contrastResult=0;
static uint8_t presented[sizeof(mt::ui::Canvas::bytes)]{};
static int writesAtWake=0;
namespace mt::update {
static bool loaderAvailable=true;
static int loaderResult=0;
bool available(Mode){return loaderAvailable;}
int prepare(Mode,const uint8_t*,const meshtastic_Config_NetworkConfig&){return loaderResult;}
}
static int display_set_contrast(const device*,uint8_t value){contrastWritten=value;return contrastResult;}
#else
static int display_set_brightness(const device*,uint8_t){return 0;}
#endif
static int display_blanking_off(const device*){
#if defined(CONFIG_MT_XIAO_EXPANSION_OLED)
  writesAtWake=writes;
#endif
  ++on;return failBlankOff?-5:0;
}
static int display_blanking_on(const device*){++off;return failBlankOn?-5:0;}
static int regulator_enable(const device*d){if(d->id==2){++lightOn;if(failLightOn)return -6;}return 0;}
static int regulator_disable(const device*d){if(d->id==2)++lightOff;return 0;}
static int display_write(const device*,unsigned x,unsigned y,const display_buffer_descriptor*d,const void*p){
 assert(x==0&&d->width==mt::ui::Canvas::Width&&d->pitch==d->width&&d->height>0);
 assert(y<mt::ui::Canvas::Height&&y+d->height<=mt::ui::Canvas::Height);
 const unsigned bytes=mt::ui::Canvas::Mono?d->width*((d->height+7)/8):d->width*d->height*2;
 assert(d->buf_size==bytes&&bytes<=sizeof(mt::ui::Canvas::bytes));
 volatile uint8_t sink=0;const auto *b=static_cast<const uint8_t*>(p);for(unsigned i=0;i<bytes;++i)sink^=b[i];(void)sink;
#if defined(CONFIG_MT_XIAO_EXPANSION_OLED)
 if(!failWrite)memcpy(presented,p,bytes);
#endif
 ++writes;return failWrite?-5:0;
}
namespace mt {
struct Config {uint8_t bytes[8]{};uint32_t seconds=15,carousel=0;bool flip=false,longNames=false,managed=false;
 bool isManaged()const{return managed;}
 bench::MtNodeRecord node()const{bench::MtNodeRecord record;record.setPublic();return record;}uint32_t displaySeconds()const{return seconds;}bool displayFlip()const{return flip;}
 bool channelConfigured()const{return PrimaryChannel::configured(node(),"LongFast");}
 uint32_t displayCarouselSeconds()const{return carousel;}bool channelMuted()const{return false;}bool displayLongNames()const{return longNames;}};
struct Service {struct Route {uint32_t identity()const{return 0x12345678;}}route;
 struct Nodes {uint32_t mutedPeer=0;bool muted(uint32_t id)const{return id==mutedPeer;}}nodes;
 Config config;
 struct Hardware {zt::Observed<uint16_t>cellMilliVolts;zt::Observed<uint8_t>cellPercent;zt::Observed<bool>externalPower;};
 Hardware hardware()const{return {};}
 bool channelMuted()const{return config.channelMuted();}
 bool receiving()const{return true;}bool txAllowed()const{return false;}bool endpointArmed()const{return false;}bool faulted()const{return false;}};
}
// The injected backend body has modeled platform/service boundaries. Native
// radio presentation is verified separately; this fixture reports unknown.
#define CONFIG_BOARD_HELTEC_T096 1
#define IS_ENABLED(value) (value)
namespace mt::ui {static bool radioView(const mt::Config&,bool,bool,RadioView&out){out={};return false;}}

struct meshtastic_User {char long_name[40]{},short_name[5]{};int hw_model=0;};
static void *meshtastic_User_fields=nullptr;
static int pb_istream_from_buffer(const uint8_t*,size_t){return 0;}
static bool pb_decode(int*,void*,meshtastic_User*){return false;}
namespace mt::stock::ui::buzzer { static void input(unsigned,uint32_t){} }
// INCLUDE_ACTUAL_DISPLAY_BODY
int main(){using namespace mt::ui;mt::Service s;
 {
#if defined(CONFIG_MT_TELEMETRY_REPORT_FRAMES)
   Model reportModel;reportModel.page=Page::Telemetry;reportModel.telemetry.showAirQuality=true;
   auto &history=reportModel.reports;
   history.validate(17,1,1000,[](uint32_t){return ReportFrames::Origin{};});
   meshtastic_Telemetry report{};report.which_variant=meshtastic_Telemetry_air_quality_metrics_tag;
   auto &aq=report.variant.air_quality_metrics;
   aq.has_pm10_standard=aq.has_pm25_standard=aq.has_pm100_standard=aq.has_co2=true;aq.co2=424;
   history.observe({17,0,"ME"},true,9,1000,report);
   reportModel.gesture(Gesture::Hold);assert(reportModel.detail&&!reportModel.selected);
   Canvas actualCanvas;actualCanvas.window(0);actualCanvas.draw(reportModel);
   reportModel.gesture(Gesture::Next);assert(reportModel.detail&&reportModel.selected==1);
   assert(strstr(history.page(reportModel.selected,reportModel.enabledReports(),false).rows[0],"424ppm"));
   reportModel.gesture(Gesture::Previous);assert(reportModel.selected==0);
   reportModel.gesture(Gesture::Hold);assert(!reportModel.detail);
#endif
   TelemetrySnapshot sensor{};
   sensor.sampledAtMs=1;
   sensor.showEnvironment=sensor.hasPressure=true;
   sensor.pressureHpa=1013;
   sensor.showHealth=sensor.hasBodyTemperature=true;
   PeerTelemetryView peer{};
   peer.hasHealth=peer.health.has_temperature=true;
   char rows[4][36]{}, first[36]{}, second[36]{}, expected[36]{};
   for (bool fahrenheit : {false,true}) for (float celsius : {-40.f, -1.5f, 0.f, 36.5f, 37.5f, 37.6f}) {
     sensor.bodyTemperatureC=peer.health.temperature=celsius;
     const float display=fahrenheit?(celsius*9)/5+32:celsius;
     // Stock HealthTelemetryModule::drawFrame uses %.0f after converting Celsius.
     snprintf(expected,sizeof(expected),"TEMP:%.0f%c",double(display),fahrenheit?'F':'C');
     healthLines(peer,fahrenheit,first,second);
     assert(!strcmp(expected,second));
     assert(TelemetryView::lines(sensor,2,fahrenheit,rows)==2 && strstr(rows[0],"1013hPa"));
     assert(strstr(rows[1],expected+5));
   }
   peer.health.temperature=INFINITY;
   healthLines(peer,false,first,second);
   assert(!second[0] && strstr(first,"NO FIELDS"));
   peer.hasAir=peer.air.has_pm_voc_idx=true;
   peer.air.pm_voc_idx=100.5f;
   airLines(peer,first,second);
   assert(!strcmp(second,"VOC:100"));
   peer.air.pm_voc_idx=101.5f;
   airLines(peer,first,second);
   assert(!strcmp(second,"VOC:102"));
   for(float invalid : {NAN,INFINITY,FLT_MAX}) {
     peer.air.pm_voc_idx=invalid;
     airLines(peer,first,second);
     assert(!second[0] && strstr(first,"OTHER FIELDS"));
   }
   assert(TelemetryView::lines(sensor,TelemetrySnapshot::FreshnessMs+2,false,rows)==2 && strstr(rows[1],"UNAVAILABLE"));
 }
 const unsigned strips=(Canvas::Height+Canvas::Rows-1)/Canvas::Rows;bool settingsOk=true;
 auto tick=[&](uint32_t n){clockNow=n;poll(s,true,true,true,true,n,settingsOk,{});};
 init(s.config);assert(available&&writes==int(strips)&&on==1&&lightOn==1);
 tick(1999);assert(writes==int(strips));tick(2000);assert(writes==int(2*strips));
 tick(15000);assert(blanked&&off==1&&lightOff==1&&writes==int(2*strips));
 keyDown=1;tick(15010);tick(15041);keyDown=0;tick(15100);tick(15131);tick(15482);
 assert(!blanked&&model().page==Page::Home&&on==2&&lightOn==2&&writes==int(3*strips));
 keyDown=1;tick(16000);tick(16031);keyDown=0;tick(16100);tick(16131);tick(16482);assert(model().page==Page::Messages);
 tick(31482);assert(blanked);
 const uint8_t text[]={'t','e','s','t'};preview(s,2,s.route.identity(),1,1,text,sizeof(text),-70,4,31500,false,{});tick(31500);assert(!blanked&&model().messageCount==1);
 preview(s,2,s.route.identity(),1,1,text,sizeof(text),-70,4,32000,false,{});tick(32000);assert(model().messageCount==1);
 failWrite=1;tick(32500);assert(!available);const auto stopped=writes;tick(35000);assert(writes==stopped);
 assert(lightOn==lightOff);
 auto reset=[&](uint32_t at,int failOn=0,int failLight=0){model()={};canvas={};button={};available=keyReady=blanked=wakeRequested=splashComplete=pairingActive=pairingClearPending=false;pairingPasskey=0;rendered=boot=activity=carouselActivity=0;screenSeconds=15;carouselSeconds=0;clockNow=at;keyDown=writes=on=off=lightOn=lightOff=failWrite=failBlankOff=failBlankOn=failLightOn=0;failBlankOff=failOn;failLightOn=failLight;init(s.config);assert(available==(failOn==0&&failLight==0));};
#if defined(CONFIG_MT_XIAO_EXPANSION_OLED)
 {
   using mt::update::FirmwareUpdateController;
   using mt::update::Mode;
   meshtastic_Config_NetworkConfig network{};
   uint8_t digest[32]={1};
   const auto arm=[&](FirmwareUpdateController &owner,Mode mode){
     assert(owner.schedule(mode,digest,sizeof(digest))==0);
   };
   reset(0);tick(15000);assert(blanked);
   const int originalWrites=writes,originalOn=on;
   FirmwareUpdateController invalid;
   showFirmwareUpdate(invalid);
   assert(invalid.schedule(Mode::EspWifiLoader,digest,31)==-EINVAL);
   mt::update::loaderAvailable=false;
   assert(invalid.schedule(Mode::EspWifiLoader,digest,32)==-ENOTSUP);
   mt::update::loaderAvailable=true;
   showFirmwareUpdate(invalid);
   arm(invalid,Mode::EspWifiLoader);
   showFirmwareUpdate(invalid);
   mt::update::loaderResult=-EIO;
   assert(invalid.execute(true,network)==-EIO);
   mt::update::loaderResult=0;
   showFirmwareUpdate(invalid);
   FirmwareUpdateController nordic;
   assert(nordic.schedule(Mode::NordicBleDfu)==0&&nordic.execute(true,network)==0);
   showFirmwareUpdate(nordic);
   assert(blanked&&writes==originalWrites&&on==originalOn);
   Canvas expected{};
   expected.center(8,"Updating",WHITE);
   expected.center(30,"Please be patient",WHITE);
   expected.center(42,"and do not power off.",WHITE);
   for(auto mode:{Mode::EspWifiLoader,Mode::EspBleLoader}){
     reset(0);tick(15000);assert(blanked);
     FirmwareUpdateController owner;
     arm(owner,mode);assert(owner.execute(true,network)==0);
     const auto before=writes;
     showFirmwareUpdate(owner);
     assert(available&&!blanked&&on==2&&writes==before+1&&writesAtWake==writes);
     assert(!memcmp(presented,expected.bytes,sizeof(presented)));
   }
   reset(0);tick(15000);
   FirmwareUpdateController owner;
   arm(owner,Mode::EspBleLoader);assert(owner.execute(true,network)==0);
   failWrite=1;showFirmwareUpdate(owner);
   assert(!available&&blanked&&on==1&&owner.state()==FirmwareUpdateController::State::Prepared);
   const int attempts=writes;showFirmwareUpdate(owner);assert(writes==attempts);
   reset(0);tick(15000);failBlankOff=1;showFirmwareUpdate(owner);
   assert(!available&&blanked&&off==2&&owner.state()==FirmwareUpdateController::State::Prepared);
 }
 reset(0);tick(2000);
 auto &menu=model().commands;
 menu.begin(2000);
 while (menu.decode(menu.Menu[menu.row].event)!=SystemCommands::Action::Dim)menu.move(true,2000);
 auto hold=[&](uint32_t at){keyDown=1;tick(at);tick(at+31);keyDown=0;tick(at+900);tick(at+931);};
 hold(2100);hold(3100);
 assert(menu.reported && !menu.result && contrastWritten==66 && brightnessLevel==66);
 menu.move(false,4100);
 assert(menu.decode(menu.Menu[menu.row].event)==SystemCommands::Action::Brighten);
 contrastResult=-5;hold(4200);hold(5200);
 assert(menu.result==-5 && contrastWritten==128 && brightnessLevel==66);
 contrastResult=0;hold(6300);hold(7300);
 assert(!menu.result && contrastWritten==128 && brightnessLevel==128);
#endif
 reset(0);failBlankOn=1;tick(15000);assert(!available&&lightOff==1);const auto failedBlankWrites=writes;tick(20000);assert(writes==failedBlankWrites);
 reset(0);tick(15000);assert(blanked);failBlankOff=1;preview(s,2,s.route.identity(),3,1,text,sizeof(text),-70,4,15100,false,{});tick(15100);assert(!available&&lightOn==1&&lightOff==2);const auto failedWakeWrites=writes;tick(20000);assert(writes==failedWakeWrites);
 reset(0);tick(15000);assert(blanked);failLightOn=1;const auto failedLightPage=model().page;
 assert(input(20,15100)==-6&&!available&&model().page==failedLightPage&&on==2&&off==2&&lightOn==2&&lightOff==2);
 const auto failedLightWrites=writes;tick(20000);assert(writes==failedLightWrites&&input(20,20001)==-ENODEV);
 // The stock text handler stores observations independently from its wake decision.
 reset(0);tick(15000);assert(blanked);
 mt::stock::text::Settings settings;
 settings.batteryPercent=9;
 preview(s,2,s.route.identity(),11,meshtastic_PortNum_DETECTION_SENSOR_APP,text,sizeof(text),-70,4,15100,false,settings);
 tick(15100);assert(blanked&&model().messageCount==1);
 settings.batteryPercent=10;settings.externalNotifications=true;
 preview(s,2,s.route.identity(),12,meshtastic_PortNum_ALERT_APP,text,sizeof(text),-70,4,15200,false,settings);
 tick(15200);assert(blanked&&model().messageCount==2);
 settings.externalNotifications=false;
 preview(s,2,s.route.identity(),13,meshtastic_PortNum_RANGE_TEST_APP,text,sizeof(text),-70,4,15300,false,settings);
 assert(blanked&&model().messageCount==2);
 settings.rangeEnabled=true;
 preview(s,2,s.route.identity(),13,meshtastic_PortNum_RANGE_TEST_APP,text,sizeof(text),-70,4,15400,false,settings);
 tick(15400);assert(!blanked&&model().messageCount==3);
 const uint32_t nearWrap=0xffffff00u;reset(nearWrap);tick(nearWrap+1999u);assert(writes==int(strips));tick(nearWrap+2000u);assert(writes==int(2*strips));tick(nearWrap+15000u);assert(blanked);preview(s,2,s.route.identity(),4,1,text,sizeof(text),-70,4,nearWrap+15100u,false,{});tick(nearWrap+15100u);assert(!blanked&&available&&model().messageCount==1);
 reset(0,1);assert(!available&&lightOn==0&&lightOff==1&&off==1);
 reset(0,0,1);assert(!available&&lightOn==1&&lightOff==1&&on==1&&off==1);
 s.config.seconds=0;s.config.flip=true;reset(0);assert(screenSeconds==0&&canvas.flipped);tick(59999);assert(!blanked);tick(60000);assert(blanked);
 s.config.seconds=UINT32_MAX;s.config.flip=false;tick(60001);assert(!blanked&&!canvas.flipped);tick(0xfffffff0u);assert(!blanked);
 s.config.seconds=1;reset(0);tick(999);assert(!blanked);tick(1000);assert(blanked);
 s.config.seconds=86400;reset(nearWrap);tick(nearWrap+86399999u);assert(!blanked);tick(nearWrap+86400000u);assert(blanked);
 s.config.seconds=5;reset(0);tick(200);const int prior=writes;s.config.flip=true;tick(300);assert(canvas.flipped&&writes==prior+int(strips));
 tick(5299);assert(!blanked);tick(5300);assert(blanked);
 reset(0);settingsOk=false;tick(2000);assert(available&&!model().settingsReady&&model().radioReady);
 settingsOk=true;tick(2200);assert(model().settingsReady);
 model().page=Page::Nodes;model().node(42,"Long Test Name","LT",81,-70,4,2200);
 s.config.longNames=true;tick(2400);assert(model().useLongNodeName&&!strcmp(model().nodeLabel(model().nodes[0],"missing"),"Long Test Name"));
 const auto namesWrites=writes;s.config.longNames=false;tick(2401);
 assert(!model().useLongNodeName&&writes==namesWrites+int(strips)&&!strcmp(model().nodeLabel(model().nodes[0],"missing"),"LT"));
 s.config.seconds=15;reset(0);const int splashWrites=writes;
 assert(input(99,500)==-EINVAL&&model().page==Page::Home&&writes==splashWrites);
 assert(input(20,1000)==0&&model().page==Page::Messages&&writes==splashWrites);
 tick(1999);assert(writes==splashWrites);tick(2000);assert(writes==splashWrites+int(strips)&&model().page==Page::Messages);
 tick(15999);assert(!blanked);tick(16000);assert(blanked);const auto asleepPage=model().page;
 assert(input(20,16001)==0&&!blanked&&model().page==asleepPage);tick(16001);assert(model().page==Page::Messages);
 assert(input(20,16002)==0&&model().page==Page::Nodes);tick(16002);assert(model().page==Page::Nodes);
 tick(31002);assert(blanked);failBlankOff=1;assert(input(19,31003)!=0&&!available&&model().page==Page::Nodes);
 assert(input(19,31004)==-ENODEV&&model().page==Page::Nodes);
 auto digest=[&](){uint64_t h=1469598103934665603ull;for(uint8_t b:canvas.bytes){h^=b;h*=1099511628211ull;}return h;};
 s.config.seconds=UINT32_MAX;s.config.flip=false;s.config.carousel=1;reset(0);tick(1999);assert(model().page==Page::Home);
 tick(2000);const auto homeFrame=digest();assert(model().page==Page::Home&&splashComplete);
 tick(2999);assert(model().page==Page::Home);tick(3000);const auto messageFrame=digest();
 assert(model().page==Page::Messages&&messageFrame!=homeFrame);
 tick(9000);assert(model().page==Page::Nodes); // one transition, no catch-up burst
 model().page=Page::Messages;model().detail=true;model().selected=0;model().textPage=0;carouselActivity=9000;
 tick(20000);assert(model().page==Page::Messages&&model().detail&&model().selected==0&&model().textPage==0);
 assert(input(27,20001)==0&&!model().detail);tick(21000);assert(model().page==Page::Messages);tick(21001);assert(model().page==Page::Nodes);
 assert(input(20,21100)==0&&model().page==Page::Channel);tick(22099);assert(model().page==Page::Channel);tick(22100);assert(model().page==Page::Status);
 s.config.seconds=1;s.config.carousel=1;reset(0);tick(1000);assert(blanked&&model().page==Page::Home);
 const auto wakeCount=on;s.config.carousel=2;tick(5000);assert(blanked&&model().page==Page::Home&&on==wakeCount);
 assert(input(20,5001)==0&&!blanked&&model().page==Page::Home);tick(5001);tick(6000);assert(model().page==Page::Home);
 s.config.seconds=UINT32_MAX;s.config.carousel=1;reset(nearWrap);tick(nearWrap+2000u);assert(model().page==Page::Home);
 tick(nearWrap+2999u);assert(model().page==Page::Home);tick(nearWrap+3000u);assert(model().page==Page::Messages);
 reset(0);s.config.seconds=UINT32_MAX;s.config.carousel=1;tick(2000);setCannedContent("A|B",true,true);setCannedRoutes(1,1);model().page=Page::Canned;model().selected=0;carouselActivity=2000;tick(12000);
 assert(model().page==Page::Canned&&model().selected==0&&!model().cannedPending);
 model().cannedSubstate=CannedSubstate::Destinations;model().cannedRouteSelected=1;model().cannedPending=true;model().cannedConfirm=true;
 pairing(true,123456,12001);assert(!model().cannedConfirm&&!model().cannedPending&&model().cannedSubstate==CannedSubstate::Messages&&model().cannedRouteSelected==0);
 keyDown=1;tick(12010);tick(12041);keyDown=0;tick(13000);tick(13031);assert(!model().cannedConfirm&&!model().cannedPending&&model().cannedSubstate==CannedSubstate::Messages);
 pairing(false,0,13032);tick(13032);assert(model().page==Page::Canned&&!model().cannedPending&&model().cannedSubstate==CannedSubstate::Messages);
 const char *backText=nullptr;size_t backLength=0;assert(model().canned.entry(model().canned.count(),backText,backLength)&&backLength==6&&!memcmp(backText,"[Back]",6));
 setCannedMessages("A||B",true,true,0xffffffffu,0);model().page=Page::Canned;model().selected=1;model().gesture(Gesture::Hold);model().gesture(Gesture::Hold);PendingCannedSend pending;assert(!takePendingCannedSend(pending));
 model().selected=0;model().gesture(Gesture::Hold);model().gesture(Gesture::Hold);assert(takePendingCannedSend(pending)&&pending.index==0&&model().cannedResult==1);
 reportCannedCompletion(false,true);assert(model().cannedResult==3);reportCannedCompletion(true,true);assert(model().cannedResult==4);reportCannedCompletion(false,false);assert(model().cannedResult==5);
 reportCannedResult(0);assert(model().cannedResult==1);reportCannedResult(1);assert(model().cannedResult==2);
 std::cout<<"PASS actual Display.cpp body: canned fingerprint, Back item, carousel and pairing input guards; splash and explicit command render,configured blank,stock zero/always-on semantics,admin/button wake without navigation,next action navigation,text wake/dedup,write/blank failures,unavailable refusal,bounded failed-wake/init cleanup,uptime wrap,descriptor bounds,bounded strip buffer reads,boot/live preference application\n";
}
