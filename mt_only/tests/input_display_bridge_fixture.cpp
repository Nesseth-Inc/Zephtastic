// SPDX-License-Identifier: GPL-3.0-only
// Actual Companion + production Controller + actual Display.cpp body. All
// devices are inert host adapters; no device, USB, BLE or radio API is opened.
#include "device_config_fixture.h"
#include "ui/Controller.h"
#include "ui/Render.h"
#include "ui/RadioPresentation.h"
#include "Version.h"
#include <array>
#include <cassert>
#include <iostream>
#define CONFIG_BOARD_HELTEC_T096 1
#define IS_ENABLED(value) (value)

struct device {int id;};static struct device panelDev{1},ddisp_pwr_enable{2},dvext_enable{3};
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
static uint32_t clockNow=0;static int keyDown=0,writes=0,on=0,off=0,lightOn=0,lightOff=0,failWrite=0;
static unsigned lockDepth=0,maxLockDepth=0,lockCalls=0,unlockCalls=0;static bool requireSerialized=false;
static std::array<uint8_t,mt::ui::Canvas::Width*mt::ui::Canvas::Height*2> framebuffer{};
static bool device_is_ready(const struct device*){return true;}
static bool gpio_is_ready_dt(const gpio_dt_spec*){return true;}
static int gpio_pin_configure_dt(const gpio_dt_spec*,int){return 0;}
static int gpio_pin_get_dt(const gpio_dt_spec*){return keyDown;}
static uint32_t k_uptime_get_32(){return clockNow;}
constexpr int PIXEL_FORMAT_MONO01=1,PIXEL_FORMAT_RGB_565=2,PIXEL_FORMAT_RGB_565X=4,SCREEN_INFO_MONO_VTILED=8;
struct display_capabilities {unsigned x_resolution=0,y_resolution=0,supported_pixel_formats=0,screen_info=0;};
struct display_buffer_descriptor {unsigned width=0,height=0,pitch=0,buf_size=0;};
static void display_get_capabilities(const struct device*,display_capabilities*c){c->x_resolution=mt::ui::Canvas::Width;c->y_resolution=mt::ui::Canvas::Height;c->supported_pixel_formats=7;c->screen_info=8;}
[[maybe_unused]] static int display_set_pixel_format(const struct device*,int){return 0;}
static int display_set_brightness(const struct device*,uint8_t){return 0;}
static int display_blanking_off(const struct device*){assert(!requireSerialized||lockDepth);++on;return 0;}
static int display_blanking_on(const struct device*){assert(!requireSerialized||lockDepth);++off;return 0;}
static int regulator_enable(const struct device*d){assert(!requireSerialized||lockDepth);if(d->id==2)++lightOn;return 0;}
static int regulator_disable(const struct device*d){assert(!requireSerialized||lockDepth);if(d->id==2)++lightOff;return 0;}
static int display_write(const struct device*,unsigned x,unsigned y,const display_buffer_descriptor*d,const void*p){
  assert(!requireSerialized||lockDepth);assert(x==0&&d->width==mt::ui::Canvas::Width&&d->pitch==d->width);
  assert(!mt::ui::Canvas::Mono&&d->buf_size==d->width*d->height*2&&y+d->height<=mt::ui::Canvas::Height);
  memcpy(framebuffer.data()+y*d->width*2,p,d->buf_size);++writes;return failWrite?-5:0;
}
static uint64_t frameHash(){uint64_t h=1469598103934665603ull;for(uint8_t value:framebuffer){h^=value;h*=1099511628211ull;}return h;}
static void lockUi(void*){++lockCalls;++lockDepth;if(lockDepth>maxLockDepth)maxLockDepth=lockDepth;}
static void unlockUi(void*){assert(lockDepth);--lockDepth;++unlockCalls;}

namespace mt::stock::ui::buzzer { static void input(unsigned,uint32_t){} }
// INCLUDE_ACTUAL_DISPLAY_BODY

static meshtastic_AdminMessage inputEvent(unsigned code) {
  meshtastic_AdminMessage value{};value.which_payload_variant=meshtastic_AdminMessage_send_input_event_tag;
  value.send_input_event.event_code=uint8_t(code);return value;
}
static meshtastic_AdminMessage channelMute(bool muted) {
  meshtastic_AdminMessage value{};value.which_payload_variant=meshtastic_AdminMessage_set_channel_tag;
  auto &channel=value.set_channel;channel.index=0;channel.role=meshtastic_Channel_Role_PRIMARY;channel.has_settings=true;
  channel.settings.psk.size=1;channel.settings.psk.bytes[0]=1;channel.settings.has_module_settings=true;
  channel.settings.module_settings.is_muted=muted;return value;
}
static std::vector<uint8_t> textFrame(Fixture &fixture,uint32_t from,uint32_t to,uint32_t id,const uint8_t *text,size_t size) {
  meshtastic_Data data{};data.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;data.has_bitfield=true;
  assert(size<=sizeof(data.payload.bytes));data.payload.size=size;memcpy(data.payload.bytes,text,size);
  uint8_t encoded[239]{};auto stream=pb_ostream_from_buffer(encoded,sizeof(encoded));assert(pb_encode(&stream,meshtastic_Data_fields,&data));
  std::vector<uint8_t> wire(255);size_t length=0;assert(fixture.service.codec.encodeZeroHopData(from,to,id,encoded,stream.bytes_written,wire.data(),wire.size(),length));
  wire.resize(length);wire[12]=0;return wire;
}
static bool notice(const Host &host,const char *text) {
  for(const auto &record:host.out)
    if(record.which_payload_variant==meshtastic_FromRadio_clientNotification_tag&&
       record.clientNotification.has_reply_id&&strstr(record.clientNotification.message,text))return true;
  return false;
}
int main(){using namespace mt::ui;Fixture fixture(127);Controller controller(nullptr,lockUi,unlockUi);
 fixture.usb.inputHandler=fixture.ble.inputHandler=[&](uint32_t code,uint32_t now){return controller.input(code,now);};
 fixture.usb.previewHandler=fixture.ble.previewHandler=[&](uint32_t from,uint32_t to,uint32_t id,unsigned port,const uint8_t *payload,size_t n,int16_t rssi,int8_t snr,uint32_t now,bool local){
   controller.preview(fixture.service,from,to,id,port,payload,n,rssi,snr,now,local);
 };
 auto cfg=fixture.service.config;cfg.bytes[4]=1;auto node=cfg.node();node.setPublic();cfg.node(node);cfg.seal();assert(fixture.service.apply(cfg));
 assert(fixture.service.start(false,0));
 auto lockedInput=[&](const meshtastic_ToRadio &value,bool ble,uint32_t now){lockUi(nullptr);fixture.input(value,ble,now);assert(lockDepth==1);unlockUi(nullptr);};
 auto lockedAdmin=[&](const meshtastic_AdminMessage &value,bool ble,uint32_t now){lockUi(nullptr);fixture.admin(value,ble,now);assert(lockDepth==1);unlockUi(nullptr);};
 auto lockedReceive=[&](mt::Companion &client,const std::vector<uint8_t> &wire,int16_t rssi,int8_t snr,uint32_t now){
   lockUi(nullptr);client.receive(wire.data(),wire.size(),rssi,snr,now,mt::Companion::WorkspaceProof{true});assert(lockDepth==1);unlockUi(nullptr);
 };
 meshtastic_ToRadio hello{};hello.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;hello.want_config_id=0x21000001;
 lockedInput(hello,false,1);hello.want_config_id=0x21000002;lockedInput(hello,true,2);fixture.usb.out.clear();fixture.ble.out.clear();
 lockUi(nullptr);init(fixture.service.config);unlockUi(nullptr);assert(available&&writes==1*((CONFIG_MT_UI_HEIGHT+CONFIG_MT_UI_STRIP_ROWS-1)/CONFIG_MT_UI_STRIP_ROWS)&&lockDepth==0);
 requireSerialized=true;
 auto tick=[&](uint32_t now){clockNow=now;lockUi(nullptr);controller.poll(fixture.service,true,true,true,true,now,true);assert(lockDepth==1);unlockUi(nullptr);assert(!lockDepth);};
 tick(2000);const auto homeHash=frameHash();assert(model().page==Page::Home&&writes==2*((CONFIG_MT_UI_HEIGHT+CONFIG_MT_UI_STRIP_ROWS-1)/CONFIG_MT_UI_STRIP_ROWS));

 fixture.usb.out.clear();lockedAdmin(inputEvent(20),false,2100);
 assert(fixture.usb.error()==meshtastic_Routing_Error_NONE&&model().page==Page::Messages&&!model().detail);
 tick(2100);const auto messagesHash=frameHash();assert(messagesHash!=homeHash&&writes==3*((CONFIG_MT_UI_HEIGHT+CONFIG_MT_UI_STRIP_ROWS-1)/CONFIG_MT_UI_STRIP_ROWS));

 const uint8_t longText[]="A deliberately long local display preview that spans more than one bounded text page without draining any companion message queue.";
 auto first=textFrame(fixture,0x12340002,::Node,0x19000001,longText,sizeof(longText)-1);
 lockedReceive(fixture.c,first,-70,4,2200);lockedReceive(fixture.b,first,-70,4,2200);
 tick(2200);const auto populatedHash=frameHash();assert(model().messageCount==1&&populatedHash!=messagesHash);
 fixture.ble.out.clear();lockedAdmin(inputEvent(10),true,2300);
 assert(fixture.ble.error()==meshtastic_Routing_Error_NONE&&model().detail&&model().selected==0);
 tick(2300);const auto detailHash=frameHash();assert(detailHash!=populatedHash);
 fixture.usb.out.clear();lockedAdmin(inputEvent(20),false,2400);assert(fixture.usb.error()==0&&model().textPage==1);
 tick(2400);const auto secondPageHash=frameHash();assert(secondPageHash!=detailHash);
 fixture.ble.out.clear();lockedAdmin(inputEvent(27),true,2500);assert(fixture.ble.error()==0&&!model().detail&&model().page==Page::Messages);
 tick(2500);assert(frameHash()!=secondPageHash);

 // Interleave a physical short press with companion commands through the same
 // exact production Controller. The released short press advances one page.
 keyDown=1;tick(3000);tick(3031);keyDown=0;tick(3100);tick(3131);tick(3482);
 assert(model().page==Page::Nodes);
 fixture.usb.out.clear();lockedAdmin(inputEvent(19),false,3500);assert(fixture.usb.error()==0&&model().page==Page::Messages);

 tick(18500);assert(blanked);const auto sleepingPage=model().page;
 fixture.ble.out.clear();lockedAdmin(inputEvent(20),true,18501);
 assert(fixture.ble.error()==0&&!blanked&&model().page==sleepingPage);
 fixture.usb.out.clear();lockedAdmin(inputEvent(20),false,18502);
 assert(fixture.usb.error()==0&&model().page==Page::Nodes);tick(18502);

 // A persisted local mute policy is consumed only from the shared NodeDB and
 // the real packet destination. Both Companions still fan out and store the DM.
 constexpr uint32_t mutedPeer=0x22334455;mt::NodeCache::Entry peer{};peer.identity(mutedPeer,81,0);
 assert(peer.text(peer.bytes+peer.Name,40,"Muted synthetic peer",21)&&peer.text(peer.bytes+peer.Short,5,"MUT",4));
 assert(fixture.service.nodes.accept(peer,::Node,1,0)>=0);
 assert(fixture.service.nodes.edit(mutedPeer,::Node,mt::NodeCache::Edit::ToggleMute)==1&&fixture.service.nodes.muted(mutedPeer));
 model().page=Page::Messages;model().detail=true;model().selected=0;model().textPage=1;
 const uint32_t viewed=model().messages[model().selected].id;tick(33502);assert(blanked);const int wakeBefore=on;
 const auto activityBefore=activity,carouselBefore=carouselActivity;const unsigned messagesBefore=model().messageCount;
 const uint8_t mutedText[]="muted direct message is stored without waking or moving the selected detail";
 auto muted=textFrame(fixture,mutedPeer,::Node,0x19000002,mutedText,sizeof(mutedText)-1);
 lockedReceive(fixture.c,muted,-75,3,33510);lockedReceive(fixture.b,muted,-75,3,33510);
 assert(model().messageCount==messagesBefore+1&&model().detail&&model().page==Page::Messages&&
   model().messages[model().selected].id==viewed&&model().textPage==1&&!wakeRequested&&activity==activityBefore&&carouselActivity==carouselBefore);
 tick(33510);assert(blanked&&on==wakeBefore&&fixture.c.receivedMessages==2&&fixture.b.receivedMessages==2);

 // Persisted primary-channel mute is consumed by the same production display
 // path. Broadcast still stores/fans out, but does not wake or refresh timers.
 fixture.usb.out.clear();lockedAdmin(channelMute(true),false,33520);
 assert(fixture.usb.error()==meshtastic_Routing_Error_NONE&&fixture.service.config.channelMuted());
 const uint8_t broadcastText[]="public broadcast from muted sender";
 auto broadcast=textFrame(fixture,mutedPeer,0xffffffffu,0x19000003,broadcastText,sizeof(broadcastText)-1);
 lockedReceive(fixture.c,broadcast,-74,2,33600);lockedReceive(fixture.b,broadcast,-74,2,33600);
 assert(!wakeRequested&&activity==activityBefore&&carouselActivity==carouselBefore);
 tick(33600);assert(blanked&&on==wakeBefore&&fixture.c.receivedMessages==3&&fixture.b.receivedMessages==3);

 // Channel mute does not suppress a direct message from an unmuted sender.
 constexpr uint32_t ordinaryPeer=0x22334466;const uint8_t directText[]="ordinary direct still attracts attention";
 auto direct=textFrame(fixture,ordinaryPeer,::Node,0x19000004,directText,sizeof(directText)-1);
 lockedReceive(fixture.c,direct,-73,2,33610);lockedReceive(fixture.b,direct,-73,2,33610);
 assert(wakeRequested);tick(33610);assert(!blanked&&on==wakeBefore+1);

 // Unmute through the other local transport; the next Public broadcast wakes.
 fixture.ble.out.clear();lockedAdmin(channelMute(false),true,33620);
 assert(fixture.ble.error()==meshtastic_Routing_Error_NONE&&!fixture.service.config.channelMuted());
 tick(48620);assert(blanked);const int secondWakeBefore=on;
 auto unmutedBroadcast=textFrame(fixture,mutedPeer,0xffffffffu,0x19000005,broadcastText,sizeof(broadcastText)-1);
 lockedReceive(fixture.c,unmutedBroadcast,-74,2,48621);lockedReceive(fixture.b,unmutedBroadcast,-74,2,48621);
 assert(wakeRequested);tick(48621);assert(!blanked&&on==secondWakeBefore+1);

 // A local echo retains the established attention behavior even if the
 // persisted channel metadata is muted.
 fixture.usb.out.clear();lockedAdmin(channelMute(true),false,48630);assert(fixture.usb.error()==0);
 tick(63630);assert(blanked);const int localWakeBefore=on;
 lockUi(nullptr);controller.preview(fixture.service,::Node,0xffffffffu,0x19000006,
   meshtastic_PortNum_TEXT_MESSAGE_APP,broadcastText,sizeof(broadcastText)-1,0,0,63631,true);unlockUi(nullptr);
 assert(wakeRequested);tick(63631);assert(!blanked&&on==localWakeBefore+1);

 // Keep time monotonic after the new local-echo case above.
 // While awake on the message root, a muted DM can appear on the ordinary
 // render cadence, but it still does not refresh timeout/carousel activity.
 model().detail=false;model().page=Page::Messages;rendered=63600;const auto priorFrame=frameHash();
 const auto awakeActivity=activity,awakeCarousel=carouselActivity;
 const uint8_t awakeMutedText[]="new muted history";
 auto awakeMuted=textFrame(fixture,mutedPeer,::Node,0x19000007,awakeMutedText,sizeof(awakeMutedText)-1);
 lockedReceive(fixture.c,awakeMuted,-73,2,63700);lockedReceive(fixture.b,awakeMuted,-73,2,63700);
 tick(63800);assert(frameHash()!=priorFrame&&activity==awakeActivity&&carouselActivity==awakeCarousel&&!wakeRequested);

 controller.setCannedContent("A|B",true,true);controller.setCannedRoutes(uint8_t((1u<<0)|(1u<<5)),0x140001);
 uint8_t routedChannel=0;assert(model().canned.count()==2&&model().cannedRoutesSupplied&&
   model().cannedChannelCount()==2&&model().cannedChannelAt(1,routedChannel)&&routedChannel==5&&
   model().cannedRouteSourceGeneration==0x140001&&lockDepth==0);

 const auto writesBeforeFailure=writes;failWrite=1;tick(64000);assert(!available&&writes>writesBeforeFailure);
 fixture.ble.out.clear();lockedAdmin(inputEvent(20),true,64001);
 assert(fixture.ble.error()==meshtastic_Routing_Error_NO_INTERFACE&&notice(fixture.ble,"Display input unavailable"));
 assert(!fixture.radio.sends&&fixture.backend.writes==3&&fixture.usb.saves==2&&fixture.ble.saves==1&&fixture.service.endpointFree()==4&&!fixture.service.endpointArmed());
 assert(lockDepth==0&&lockCalls==unlockCalls&&lockCalls>=20&&maxLockDepth==2);
 std::cout<<"PASS actual USB/BLE Companion frames under an outer service lock -> recursive production Controller -> actual Display/Model/Canvas, populated detail paging, physical-button interleave, wake consumption, framebuffer changes and truthful display failure\n";
}
