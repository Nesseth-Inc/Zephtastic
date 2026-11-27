// SPDX-License-Identifier: GPL-3.0-only
#include "Display.h"
#include "../stock/ui/ZephyrBuzzerFeedback.h"
#include "Render.h"
#if defined(CONFIG_MT_XIAO_EXPANSION_OLED)
#include "../update/FirmwareUpdateController.h"
#endif
#include "RadioPresentation.h"
#include "../Version.h"
#include "meshtastic/mesh.pb.h"
#include <pb_decode.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/regulator.h>
#include <math.h>
#include <stdarg.h>
namespace mt::ui {
// Keep this large display-only cache in zero-init storage. A directly defined
// Model object is emitted as a 9 KiB initialized-data image on T096. The
// wrapper uses the class-local placement-new pattern used by the private
// runtime; no heap, global placement overload, function-local static guard,
// or second initialization image is needed. UI entry points are serialized by
// Controller/serviceLock; init constructs it before display rendering, while
// the accessor also supports existing pre-init settings callbacks.
struct ModelStorage {
  static void *operator new(size_t,void *storage)noexcept{return storage;}
  static void operator delete(void *,void *)noexcept{}
  Model value;
};
static_assert(sizeof(ModelStorage)==sizeof(Model));
// The renderer reads the model on its owner thread. The display driver only
// receives the separate, internal canvas buffer.
#if defined(CONFIG_MT_XIAO_EXPANSION_OLED) && defined(CONFIG_ESP_SPIRAM)
alignas(16) static uint8_t modelStorage[sizeof(ModelStorage)]
    __attribute__((section(".ext_ram.bss.mt_display_model"))){};
#else
alignas(ModelStorage) static uint8_t modelStorage[sizeof(ModelStorage)]{};
#endif
static ModelStorage *modelInstance=nullptr;
static Model &model(){
  if(!modelInstance)modelInstance=new(static_cast<void *>(modelStorage)) ModelStorage;
  return modelInstance->value;
}
#if defined(CONFIG_MT_BASEUI_GAMES)
stock::ui::GamesModule &localGames(){return model().games;}
#endif
static Canvas canvas;
using SystemCommands=stock::ui::SystemCommandsModule;
static int (*runSystemCommand)(SystemCommands::Action,uint32_t)=nullptr;
static bool deviceResetAvailable=false;
void setSystemCommandOwner(int (*run)(SystemCommands::Action,uint32_t),bool deviceReset){
  runSystemCommand=run;deviceResetAvailable=deviceReset;
}
#if defined(CONFIG_MT_XIAO_EXPANSION_OLED)
static uint8_t brightnessLevel=CONFIG_SSD1306_DEFAULT_CONTRAST;
static bool drawingFirmwareUpdate=false;
#else
static uint8_t brightnessLevel=153;
#endif
static Button button;
static bool (*consumeButton)()=nullptr;
void setButtonConsumer(bool (*consume)()){consumeButton=consume;}
static const device *const panel=DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
static const device *const backlight=DEVICE_DT_GET_OR_NULL(DT_NODELABEL(disp_pwr_enable));
static const device *const vext=DEVICE_DT_GET_OR_NULL(DT_NODELABEL(vext_enable));
static const gpio_dt_spec key=GPIO_DT_SPEC_GET(DT_ALIAS(sw0),gpios);
static bool available=false,keyReady=false,blanked=false,wakeRequested=false,splashComplete=false;
static uint32_t rendered=0,boot=0,activity=0,carouselActivity=0;
static uint32_t screenSeconds=15,carouselSeconds=0;
static bool pairingActive=false;
static bool pairingClearPending=false;
static uint32_t pairingPasskey=0;
static void append(char *out,size_t capacity,size_t &used,const char *format,...){
  if(used>=capacity)return;
  va_list args;va_start(args,format);const int n=vsnprintf(out+used,capacity-used,format,args);va_end(args);
  if(n>0){const size_t remaining=capacity-used;used+=(size_t(n)<remaining?size_t(n):remaining-1);}
}
static const char *freshness(PeerTelemetryView::Freshness value){
  switch(value){
  case PeerTelemetryView::Freshness::Fresh:return "FRESH";
  case PeerTelemetryView::Freshness::Stale:return "STALE";
  default:return "UNAVAILABLE";
  }
}
static void airLines(const PeerTelemetryView &view,char (&first)[36],char (&second)[36]){
  first[0]=second[0]=0;size_t used=0,secondUsed=0;bool any=false;
  if(!view.hasAir){snprintf(first,sizeof(first),"AQ %s",freshness(view.airFreshness));return;}
  const auto &m=view.air;
  if(m.has_pm25_standard){append(first,sizeof(first),used,"PM2.5:%lu",static_cast<unsigned long>(m.pm25_standard));any=true;}
  if(m.has_pm100_standard){if(used)append(first,sizeof(first),used," ");append(first,sizeof(first),used,"PM10:%lu",static_cast<unsigned long>(m.pm100_standard));any=true;}
  if(m.has_co2){append(second,sizeof(second),secondUsed,"CO2:%lu",static_cast<unsigned long>(m.co2));any=true;}
  int voc;
  if(m.has_pm_voc_idx && TelemetryView::displayInteger(m.pm_voc_idx,voc)){
    if(second[0])append(second,sizeof(second),secondUsed," ");
    append(second,sizeof(second),secondUsed,"VOC:%d",voc);any=true;
  }
  if(!any)snprintf(first,sizeof(first),"AQ OTHER FIELDS");
}
static void healthLines(const PeerTelemetryView &view,bool fahrenheit,char (&first)[36],char (&second)[36]){
  first[0]=second[0]=0;size_t used=0,secondUsed=0;bool any=false;
  if(!view.hasHealth){snprintf(first,sizeof(first),"HEALTH %s",freshness(view.healthFreshness));return;}
  const auto &m=view.health;
  if(m.has_heart_bpm){append(first,sizeof(first),used,"HR:%u",unsigned(m.heart_bpm));any=true;}
  if(m.has_spO2){if(used)append(first,sizeof(first),used," ");append(first,sizeof(first),used,"O2:%u%%",unsigned(m.spO2));any=true;}
  int degrees;
  if(m.has_temperature && TelemetryView::healthTemperature(m.temperature,fahrenheit,degrees)){
    append(second,sizeof(second),secondUsed,"TEMP:%d%c",degrees,fahrenheit?'F':'C');any=true;
  }
  if(!any)snprintf(first,sizeof(first),"HEALTH FRESH / NO FIELDS");
}
static void peerDetail(Canvas &target,const Model &m){
  if(!m.detail||m.page!=Page::Nodes||m.selected>=m.nodeCount||
     m.nodeDetailSubview==NodeDetailSubview::Info)return;
  const bool air=m.nodeDetailSubview==NodeDetailSubview::PeerAir;
  char first[36]{},second[36]{},source[40]{};
  const auto &node=m.nodes[m.selected];
  // Navigation may change the selected node during this render iteration,
  // before the owner can publish its next cache snapshot. Never label the
  // previous peer's readings with the newly selected peer's identity.
  const PeerTelemetryView absent{};
  const auto &view=m.peerTelemetry.sourceNodeId==node.id?m.peerTelemetry:absent;
  if(air)airLines(view,first,second);else healthLines(view,m.telemetry.fahrenheit,first,second);
  const char *shortName=node.shortName[0]?node.shortName:"Peer";
  snprintf(source,sizeof(source),"%s !%08x",shortName,unsigned(node.id));
  if constexpr(Canvas::Width==240){
    target.wf(2,23,236,91,BG);
    target.wt(9,30,air?"PEER AQ CACHE":"PEER HEALTH CACHE",CYAN,2);
    target.wt(9,51,source,LAVENDER,1,36);
    target.wt(9,69,air?freshness(view.airFreshness):freshness(view.healthFreshness),WHITE,1,30);
    target.wt(9,86,first,WHITE,1,36);
    if(second[0])target.wt(9,103,second,LAVENDER,1,36);
  }else{
    target.rect(2,14,124,39,BG);
    target.text(4,15,air?"PEER AQ CACHE":"PEER HEALTH CACHE",CYAN,20);
    target.text(4,24,source,LAVENDER,20);
    target.text(4,33,air?freshness(view.airFreshness):freshness(view.healthFreshness),WHITE,20);
    target.text(4,42,first,WHITE,20);
    if(second[0])target.text(4,50,second,LAVENDER,20);
  }
}
static char waypointName[30]{},waypointTitle[20]{};
static uint16_t waypointDuration=3000;
static bool waypointVisible=false;
static uint32_t waypointAt=0;
void clearWaypoint(){waypointVisible=false;memset(waypointName,0,sizeof(waypointName));}
void showWaypoint(const char *name,uint32_t now){
  if(!available||pairingActive)return;
  memcpy(waypointTitle,"New Waypoint",13);waypointDuration=3000;
  while(*name && uint8_t(*name)<=32)++name;
  size_t n=strnlen(name,sizeof(waypointName)-1);
  while(n && uint8_t(name[n-1])<=32)--n;
  memcpy(waypointName,name,n);waypointName[n]=0;
  waypointVisible=true;waypointAt=now;wakeRequested=true;activity=carouselActivity=now;rendered=now-200;
}
void showGeofence(uint32_t from,const char *name,bool entered,uint32_t now){
  showWaypoint(name,now);
  if(!waypointVisible)return;
  snprintf(waypointTitle,sizeof(waypointTitle),"!%08x %s",unsigned(from),entered?"IN":"OUT");
  waypointDuration=5000;
}
static int write(bool splash=false){
  for(unsigned row=0;row<Canvas::Height;row+=Canvas::Rows){
    canvas.window(row);
#if defined(CONFIG_MT_XIAO_EXPANSION_OLED)
    if(drawingFirmwareUpdate){
      canvas.wf(0,0,Canvas::Width,Canvas::Height,BG);
      canvas.center(8,"Updating",WHITE);
      canvas.center(30,"Please be patient",WHITE);
      canvas.center(42,"and do not power off.",WHITE);
    }else
#endif
    if(pairingActive)canvas.pairing(pairingPasskey);
    else if(splash)canvas.splash(CONFIG_MT_UI_DEVICE_LABEL,FirmwareVersion);
    else {
      canvas.draw(model());peerDetail(canvas,model());
      if(waypointVisible){
        canvas.rect(0,0,128,waypointName[0]?22:12,BG);
        canvas.text(2,2,waypointTitle,WHITE);
        if(waypointName[0])canvas.text(2,12,waypointName,WHITE,20);
      }
    }
    display_buffer_descriptor d{};d.width=Canvas::Width;d.height=Canvas::Height-row<Canvas::Rows?Canvas::Height-row:Canvas::Rows;d.pitch=Canvas::Width;
    d.buf_size=Canvas::Mono?sizeof(canvas.bytes):unsigned(d.width)*unsigned(d.height)*2;
    const int ret=display_write(panel,0,row,&d,canvas.bytes);if(ret)return ret;
  }
  return 0;
}
static int power(bool on){
  const int displayResult=on?display_blanking_off(panel):display_blanking_on(panel);
  if(on&&displayResult)return displayResult;
  int lightResult=0;
  if constexpr(DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(disp_pwr_enable)))lightResult=on?regulator_enable(backlight):regulator_disable(backlight);
  return displayResult?displayResult:lightResult;
}
#if defined(CONFIG_MT_XIAO_EXPANSION_OLED)
void showFirmwareUpdate(const update::FirmwareUpdateController &owner){
  if(!available||owner.state()!=update::FirmwareUpdateController::State::Prepared||
     (owner.mode()!=update::Mode::EspBleLoader&&owner.mode()!=update::Mode::EspWifiLoader))return;
  // The verified handoff leaves the normal display loop. Present the frame
  // now, replacing any previous contents before waking a blanked panel.
  drawingFirmwareUpdate=true;
  const int result=write();
  drawingFirmwareUpdate=false;
  if(result||(blanked&&power(true))){
    (void)power(false);available=false;return;
  }
  blanked=false;
}
#endif
void init(const Config &config) {
  (void)model();
  screenSeconds=config.displaySeconds();carouselSeconds=config.displayCarouselSeconds();canvas.flipped=config.displayFlip();
  keyReady=gpio_is_ready_dt(&key)&&gpio_pin_configure_dt(&key,GPIO_INPUT)==0;
  if(!device_is_ready(panel))return;
  if constexpr(DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(disp_pwr_enable))){if(!device_is_ready(backlight))return;}
  if constexpr(DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(vext_enable))){if(!device_is_ready(vext)||regulator_enable(vext))return;}
  display_capabilities c{};display_get_capabilities(panel,&c);
  if(c.x_resolution!=Canvas::Width||c.y_resolution!=Canvas::Height)return;
  if constexpr(Canvas::Mono){if(!(c.supported_pixel_formats&PIXEL_FORMAT_MONO01)||!(c.screen_info&SCREEN_INFO_MONO_VTILED)||display_set_pixel_format(panel,PIXEL_FORMAT_MONO01))return;}
  else if(!(c.supported_pixel_formats&(PIXEL_FORMAT_RGB_565|PIXEL_FORMAT_RGB_565X)))return;
  // TFT chosen nodes are raw panels; retain their own MADCTL, init and byte order.
  if(write(true))return;
  if(power(true)){(void)power(false);return;}
  available=true;splashComplete=false;boot=activity=carouselActivity=k_uptime_get_32();
}
bool ready(){return available;}
void pairing(bool active,uint32_t passkey,uint32_t now) {
  active=active&&available&&passkey<=999999u;
  if(pairingActive==active&&(!active||pairingPasskey==passkey))return;
  if(active){auto &m=model();
#if defined(CONFIG_MT_BASEUI_GAMES)
    m.games.visible(false,now);
#endif
    m.commands={};m.cancelCannedAction();m.cannedSubstate=CannedSubstate::Messages;m.cannedRouteSelected=0;button={};}
  if(pairingActive&&!active)pairingClearPending=true;
  pairingActive=active;pairingPasskey=active?passkey:0;
  // Erase the retained output strip immediately when the session is cleared.
  memset(canvas.bytes,0,sizeof(canvas.bytes));
  wakeRequested=true;activity=carouselActivity=now;rendered=now-200;
}
#if defined(CONFIG_MT_TELEMETRY_REPORT_FRAMES)
ReportFrames &telemetryReports(){return model().reports;}
#endif
void telemetry(const TelemetrySnapshot &snapshot,uint64_t nowMs){model().telemetry=snapshot;model().nowMs=nowMs;}
bool selectedPeerForTelemetry(SelectedPeerForTelemetry &out){
  out={};const auto &m=model();
  if(!m.detail||m.page!=Page::Nodes||m.selected>=m.nodeCount||
     m.nodeDetailSubview==NodeDetailSubview::Info)return false;
  const auto &node=m.nodes[m.selected];if(!node.id)return false;
  out.nodeId=node.id;memcpy(out.shortName,node.shortName,sizeof(out.shortName));return true;
}
void peerTelemetry(const PeerTelemetryView &snapshot){
  auto &m=model();
  if(!m.detail||m.page!=Page::Nodes||m.selected>=m.nodeCount||
     m.nodeDetailSubview==NodeDetailSubview::Info||
     !snapshot.sourceNodeId||snapshot.sourceNodeId!=m.nodes[m.selected].id){
    m.peerTelemetry={};return;
  }
  m.peerTelemetry=snapshot;
}
int input(uint32_t eventCode,uint32_t now) {
  if(!available)return -ENODEV;
  if(pairingActive||model().commands.open)return -EBUSY;
  Input in;
  switch(eventCode) {
  case 10:in=Input::Select;break;
  case 11:in=Input::Select;break; // long select: one deliberate select event
  case 12:in=Input::Up;break;
  case 13:in=Input::Down;break;
  case 29:in=Input::Up;break; // stock up/down ALT_PRESS
  case 17:in=Input::Up;break;
  case 18:in=Input::Down;break;
  case 19:in=Input::Left;break;
  case 20:in=Input::Right;break;
  case 24:
#if defined(CONFIG_MT_ONSCREEN_KEYBOARD)
    if(model().keyboard.active){in=Input::Back;break;}
#endif
#if defined(CONFIG_MT_BASEUI_GAMES)
    if(model().page==Page::Games){in=Input::Back;break;}
#endif
    return -EINVAL;
#if defined(CONFIG_MT_BASEUI_GAMES)
  case 30: // ALT_LONG dismisses the stock score-clear overlay.
    if(model().page!=Page::Games||model().games.state!=stock::ui::GamesModule::State::ConfirmClear)return -EINVAL;
    in=Input::Back;break;
#endif
  case 27:in=Input::Back;break;
  case 28:in=Input::UserPress;break;
  default:return -EINVAL;
  }
  activity=carouselActivity=now;
  if(blanked) {
    const int ret=power(true);if(ret){(void)power(false);available=false;return ret;}
    blanked=false;rendered=now-200;return 0;
  }
  auto &displayModel=model();displayModel.now=now;
#if defined(CONFIG_MT_ONSCREEN_KEYBOARD)
  if(displayModel.keyboardInput(eventCode,now)){rendered=now-200;return 0;}
#endif
#if defined(CONFIG_MT_BASEUI_GAMES)
  if(displayModel.page==Page::Games&&displayModel.games.input(eventCode,now)){rendered=now-200;return 0;}
#endif
  displayModel.input(in);
  rendered=now-200;return 0;
}
void setCannedMessages(const char *list,bool ready,bool enabled,uint32_t destination,uint8_t channel) {
  const size_t length=list?strnlen(list,201):0;
  model().setCannedMessages(list,length,ready,enabled,destination,channel);
}
void setCannedRoutes(uint8_t validChannelMask,uint32_t configurationGeneration) {
  model().setCannedRoutes(validChannelMask,configurationGeneration);
}
void setCannedContent(const char *list,bool ready,bool enabled) {
  const size_t length=list?strnlen(list,201):0;
  model().setCannedContent(list,length,ready,enabled);
}
bool takePendingCannedSend(PendingCannedSend &out){return !pairingActive&&model().takeCannedSend(out);}
void reportCannedResult(int result){model().cannedResult=result==0?1:2;}
void reportCannedCompletion(bool relayed,bool success){model().cannedResult=success?(relayed?4:3):5;}
void preview(const Service &s,uint32_t from,uint32_t to,uint32_t id,unsigned port,const uint8_t *p,size_t n,int16_t rssi,int8_t snr,uint32_t now,bool local,const stock::text::Settings &settings) {
  if(!p||n>237)return;
  if(stock::text::wantPacket(port,settings.rangeEnabled)){
    const bool muted=to==s.route.identity()?s.nodes.muted(from):(to==UINT32_MAX && s.channelMuted());
    stock::text::handleReceived(port,p,n,muted,local,settings,[&]{
      return model().text(from,id,p,n,rssi,snr,now,local);
    },[&]{ wakeRequested=true;activity=carouselActivity=now; });
  }
  if(port==meshtastic_PortNum_NODEINFO_APP){meshtastic_User user{};auto in=pb_istream_from_buffer(p,n);if(pb_decode(&in,meshtastic_User_fields,&user))model().node(from,user.long_name,user.short_name,user.hw_model,rssi,snr,now);}
}
#if defined(CONFIG_MT_LOCAL_CLOCK) && defined(CONFIG_MT_XIAO_EXPANSION_OLED)
static uint32_t displayUtc;
void setClockUtc(uint32_t utc){displayUtc=utc;}
#endif
void poll(const Service &s,bool radioReady,bool bleReady,bool bleConnected,bool usbConnected,uint32_t now,bool settingsReady,const GnssView &gnss) {
  if(!available)return;
  auto &displayModel=model();
  if(waypointVisible&&(pairingActive||uint32_t(now-waypointAt)>=waypointDuration)){clearWaypoint();rendered=now-200;}
  displayModel.gnss=gnss;displayModel.now=now;
#if defined(CONFIG_MT_ONSCREEN_KEYBOARD)
  if(displayModel.keyboard.expired(now)){displayModel.keyboard.clear();rendered=now-200;}
#endif
  const bool visualPreferencesChanged=screenSeconds!=s.config.displaySeconds()||canvas.flipped!=s.config.displayFlip()||displayModel.useLongNodeName!=s.config.displayLongNames();
  const bool carouselChanged=carouselSeconds!=s.config.displayCarouselSeconds();
  if(visualPreferencesChanged){displayModel.useLongNodeName=s.config.displayLongNames();screenSeconds=s.config.displaySeconds();canvas.flipped=s.config.displayFlip();wakeRequested=true;activity=carouselActivity=now;}
  if(carouselChanged){carouselSeconds=s.config.displayCarouselSeconds();carouselActivity=now;}
  if(displayModel.commands.open&&(!settingsReady||uint32_t(now-displayModel.commands.lastInput)>=60000)){
    displayModel.commands={};rendered=now-200;
  }
  if(pairingActive)button={};
  if(!pairingActive&&keyReady){int down=gpio_pin_get_dt(&key);Gesture g;if(down>=0&&button.update(down!=0,now,g)){
    activity=carouselActivity=now;const bool consumed=consumeButton&&consumeButton();if(blanked){if(power(true)){(void)power(false);available=false;return;}blanked=false;rendered=now-200;}else if(!consumed){
      stock::ui::buzzer::input(g==Gesture::Hold?10:g==Gesture::Next?28:29,now);
      auto &commands=displayModel.commands;
      if(g==Gesture::Hold&&runSystemCommand&&settingsReady&&uint32_t(button.pressed-boot)>30000&&
         uint32_t(now-button.pressed)>=3900){
        (void)runSystemCommand(SystemCommands::Action::Shutdown,now);commands={};
      }else if(commands.open){
        if(g==Gesture::Hold)commands.select(now,[](SystemCommands::Action action,uint32_t time){
          if(action==SystemCommands::Action::Brighten||action==SystemCommands::Action::Dim){
            const uint8_t value=SystemCommands::brightness(brightnessLevel,action==SystemCommands::Action::Brighten);
#if defined(CONFIG_MT_XIAO_EXPANSION_OLED)
            const int rc=display_set_contrast(panel,value);
#else
            const int rc=display_set_brightness(panel,value);
#endif
            if(!rc)brightnessLevel=value;
            return rc;
          }
          return runSystemCommand?runSystemCommand(action,time):-ENOTSUP;
        });else commands.move(g==Gesture::Next,now);
        rendered=now-200;
      }else if(settingsReady&&runSystemCommand&&displayModel.page==Page::Status&&g==Gesture::Hold){
        commands.begin(now,deviceResetAvailable&&!s.config.isManaged());rendered=now-200;
      }else
#if defined(CONFIG_MT_ONSCREEN_KEYBOARD)
#if defined(CONFIG_MT_BASEUI_EMOTES)
      if(g==Gesture::Hold&&uint32_t(now-button.pressed)>=1600&&displayModel.keyboard.openPicker(now)){rendered=now-200;}
      else
#endif
      if(displayModel.keyboardInput(g==Gesture::Hold?10:g==Gesture::Next?28:29,now)){}
      else
#endif
#if defined(CONFIG_MT_BASEUI_GAMES)
      if(displayModel.page==Page::Games&&displayModel.games.input(
           g==Gesture::Hold?10:(displayModel.games.state==stock::ui::GamesModule::State::Attract?
           (g==Gesture::Next?18:17):(g==Gesture::Next?20:19)),now,
           displayModel.games.selected==0?0xc0:0)){}
      else
#endif
        displayModel.gesture(g);
    }
  }}
  if(!available)return;
  if(wakeRequested){wakeRequested=false;if(blanked){if(power(true)){(void)power(false);available=false;return;}blanked=false;carouselActivity=now;}rendered=now-200;}
  // Pairing may finish inside the startup splash interval. Clear the actual
  // panel now instead of leaving its last PIN visible until ordinary rendering.
  if(pairingClearPending&&!pairingActive){
    if(write(uint32_t(now-boot)<2000)){available=false;power(false);return;}
    pairingClearPending=false;
  }
  // UINT32_MAX is stock's explicit always-on value. Config validation bounds
  // finite values to one day, keeping the millisecond duration below wrap.
  const uint32_t timeout=screenSeconds==UINT32_MAX?0:(screenSeconds?screenSeconds:60)*1000u;
  if(!blanked&&!pairingActive&&screenSeconds!=UINT32_MAX&&uint32_t(now-activity)>=timeout){if(power(false)){available=false;return;}blanked=true;}
  if(visualPreferencesChanged&&!blanked&&uint32_t(now-boot)<2000){if(write(true)){available=false;power(false);}return;}
  if(blanked||(!pairingActive&&uint32_t(now-boot)<2000))return;
  if(!splashComplete){splashComplete=true;carouselActivity=now;}
#if defined(CONFIG_MT_BASEUI_GAMES)
  auto &games=displayModel.games;
  games.node=s.route.identity();games.visible(displayModel.page==Page::Games&&!pairingActive,now);
  const auto before=games.state;
  const auto beforeTick=games.next;
  if(games.poll(now))displayModel.page=Page::Home;
  if(beforeTick!=games.next||before!=games.state)rendered=now-200;
  if(games.onScreen&&games.state==stock::ui::GamesModule::State::Playing){
    activity=carouselActivity=now;
  }
#endif
  // Root-only, one transition per bounded poll. A carousel expiry is not user
  // activity: it neither wakes the display nor extends the screen timeout.
  if(carouselSeconds&&!displayModel.commands.open
#if defined(CONFIG_MT_BASEUI_GAMES)
     &&displayModel.page!=Page::Games
#endif
     &&displayModel.page!=Page::Canned&&!displayModel.detail&&uint32_t(now-carouselActivity)>=carouselSeconds*1000u){
    displayModel.move(true);carouselActivity=now;rendered=now-200;
  }
  if(uint32_t(now-rendered)<200)return;
  rendered=now;
#if defined(CONFIG_MT_LOCAL_CLOCK) && defined(CONFIG_MT_XIAO_EXPANSION_OLED)
  if(displayModel.page==Page::Status)
    clock::LocalTime::label(s.config.localClock(displayUtc),s.config.twelveHourClock(),displayModel.clockLabel);
#endif
  displayModel.identity=s.route.identity();displayModel.now=now;displayModel.radioReady=radioReady;displayModel.receiving=s.receiving();displayModel.txAllowed=s.txAllowed();displayModel.armed=s.endpointArmed();displayModel.fault=s.faulted();
  displayModel.bleReady=bleReady;displayModel.bleConnected=bleConnected;displayModel.usbConnected=usbConnected;displayModel.publicChannel=PrimaryChannel::defaultKey(s.config.node());displayModel.us=s.config.bytes[4];
  displayModel.customChannel=!displayModel.publicChannel&&s.config.channelConfigured();
  displayModel.settingsReady=settingsReady;
  const auto &hardware=s.hardware();
  displayModel.voltageKnown=hardware.cellMilliVolts.has_value();displayModel.cellMilliVolts=displayModel.voltageKnown?*hardware.cellMilliVolts:0;
  displayModel.percentKnown=hardware.cellPercent.has_value();displayModel.cellPercent=displayModel.percentKnown?*hardware.cellPercent:0;
  displayModel.externalPowerKnown=hardware.externalPower.has_value();displayModel.externalPower=displayModel.externalPowerKnown&&*hardware.externalPower;
  if(!radioView(s.config,settingsReady,IS_ENABLED(CONFIG_BOARD_HELTEC_T096),displayModel.radio))displayModel.radio={};
  auto record=s.config.node();clean(displayModel.name,sizeof(displayModel.name),record.bytes+bench::MtNodeRecord::Name,24);
  if(write()){available=false;power(false);}
}
}
