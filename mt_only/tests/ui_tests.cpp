// SPDX-License-Identifier: GPL-3.0-only
#include "ui/Render.h"
#include <cassert>
#include <fstream>
#include <string>
using namespace mt::ui;
static Canvas c;
static uint64_t frameHash(const Model &m){
 uint64_t hash=1469598103934665603ull;
 for(unsigned first=0;first<Canvas::Height;first+=Canvas::Rows){c.window(first);memset(c.bytes,0,sizeof(c.bytes));c.draw(m);assert(c.clipped==0);for(uint8_t b:c.bytes){hash^=b;hash*=1099511628211ull;}}
 c.window(0);return hash;
}
static void save(const std::string &path,const Model &m){
 std::ofstream o(path,std::ios::binary);o<<"P6\n"<<Canvas::Width<<" "<<Canvas::Height<<"\n255\n";
 for(unsigned first=0;first<Canvas::Height;first+=Canvas::Rows){
  c.window(first);c.draw(m);assert(c.clipped==0);
  for(unsigned y=first;y<Canvas::Height&&y<first+Canvas::Rows;++y)for(unsigned x=0;x<Canvas::Width;++x){unsigned v=c.color(x,y);char rgb[3]={char(((v>>11)&31)*255/31),char(((v>>5)&63)*255/63),char((v&31)*255/31)};o.write(rgb,3);}
 }
 c.window(0);
}

int main(int argc,char **argv){
 assert(argc==2);std::string dir=argv[1];Model m;
 c.pixel(0,0,PINK);if constexpr(Canvas::Mono)assert(c.color(0,0)==WHITE);else assert(c.bytes[0]==0xf9&&c.bytes[1]==0x73);
 c.pixel(-1,-1,WHITE);c.pixel(Canvas::Width,Canvas::Height,WHITE);assert(c.clipped==2);c.splash("HELTEC V4","2.8.1.ZT-m4u1-tx");assert(!c.clipped);
 // Every strip/backend must display all six digits, including a zero PIN,
 // without clipping. Returning to the model must overwrite all PIN pixels.
 for(const uint32_t pin:{0u,999999u}){
  for(unsigned first=0;first<Canvas::Height;first+=Canvas::Rows){
   c.window(first);c.pairing(pin);assert(!c.clipped);
  }
 }
 c.window(0);const uint64_t beforePin=frameHash(m);
 c.pairing(123456);assert(!c.clipped);assert(frameHash(m)==beforePin);
 for(unsigned p=0;p<6;++p){m.page=Page(p);save(dir+"/empty-"+std::to_string(p)+".ppm",m);}
 m.identity=0x10203040;m.bleReady=m.bleConnected=m.usbConnected=m.radioReady=m.receiving=m.publicChannel=m.us=true;m.now=300000;
 m.node(0x12345678,"Demo Peer","DEMO",127,-83,8,290000);
 m.node(0x87654321,"Hilltop Relay","HILL",81,-101,-3,282000);
 m.node(0x11223344,"Trail companion","TRAL",81,-93,2,296000);
 std::string body="Received on Demo Peer. ZephTastic ZT bench test passed. This is a display-only copy; the companion still receives the complete message.";
 m.text(0x12345678,1,reinterpret_cast<const uint8_t *>(body.data()),body.size(),-83,8,295000,false);
 m.text(m.identity,2,reinterpret_cast<const uint8_t *>("Bluetooth local text"),20,0,0,298000,true);
 m.text(0x11223344,3,reinterpret_cast<const uint8_t *>("Meet at the trailhead"),21,-93,2,299000,false);
 for(unsigned p=0;p<6;++p){m.page=Page(p);save(dir+"/page-"+std::to_string(p)+".ppm",m);}
 m.page=Page::Messages;m.gesture(Gesture::Hold);assert(m.detail);save(dir+"/text-detail.ppm",m);
 m.gesture(Gesture::Next);m.gesture(Gesture::Next);assert(m.selected==2);
 m.gesture(Gesture::Next);assert(m.selected==2&&m.textPage==1);save(dir+"/text-long.ppm",m);
 m.gesture(Gesture::Previous);assert(m.textPage==0);m.gesture(Gesture::Hold);assert(!m.detail);
 m.page=Page::Nodes;m.gesture(Gesture::Hold);assert(m.detail);save(dir+"/node-detail.ppm",m);
 auto viewed=m.nodes[m.selected].id;m.node(0x56789012,"New arrival","NEW",127,-91,2,300000);assert(m.nodes[m.selected].id==viewed);
 m.gesture(Gesture::Next);assert(m.selected==2);m.gesture(Gesture::Hold);
 m.page=Page::Status;m.fault=true;save(dir+"/radio-fault.ppm",m);
 m.fault=false;m.txAllowed=m.armed=true;save(dir+"/tx-armed.ppm",m);m.armed=false;save(dir+"/tx-disarmed.ppm",m);
 // Real status renders must distinguish GNSS states without coordinates or
 // clipping on compact, strip, OLED and wide backends.
 using Gps=zt::gnss::DisplayState;uint64_t gpsHashes[6]{};unsigned gpsIndex=0;
 for(const auto state:{Gps::Absent,Gps::Off,Gps::Starting,Gps::Search,Gps::Fix,Gps::Fault}){
  m.gnss={state,65535};gpsHashes[gpsIndex]=frameHash(m);
  for(unsigned i=0;i<gpsIndex;++i)assert(gpsHashes[i]!=gpsHashes[gpsIndex]);
  save(dir+"/gps-"+std::to_string(gpsIndex++)+".ppm",m);
 }
 char gpsText[32]{};m.gnss={Gps::Fix,12};m.gnss.label(gpsText,sizeof(gpsText));assert(!strcmp(gpsText,"GPS FIX 12 SAT"));
 assert(zt::gnss::classifyDisplayState(false,true,true,true,true)==Gps::Absent);
 assert(zt::gnss::classifyDisplayState(true,false,true,true,true)==Gps::Fault);
 assert(zt::gnss::classifyDisplayState(true,false,true,false,true)==Gps::Off);
 assert(zt::gnss::classifyDisplayState(true,true,false,false,true)==Gps::Starting);
 assert(zt::gnss::classifyDisplayState(true,true,true,false,false)==Gps::Search);
 assert(zt::gnss::classifyDisplayState(true,true,true,false,true)==Gps::Fix);
 m.gnss={};
 m.settingsReady=false;save(dir+"/settings-fault.ppm",m);m.page=Page::Home;save(dir+"/settings-fault-home.ppm",m);m.settingsReady=true;
 const unsigned count=m.messageCount;m.text(0x11223344,3,reinterpret_cast<const uint8_t *>("duplicate"),9,0,0,0,false);assert(m.messageCount==count);
 uint8_t raw[300];memset(raw,0xff,sizeof(raw));for(unsigned i=10;i<50;++i){m.text(i,i,raw,sizeof(raw),-127,-8,i,false);m.node(i,"Overflow node","OVR",127,-127,-8,i);}
 assert(m.messageCount==MessageCapacity&&m.nodeCount==16&&strlen(m.messages[0].text)==237);assert(m.messages[0].text[0]==' ');
 m.page=Page::Messages;m.detail=true;m.selected=MessageCapacity-1;m.textPage=3;m.text(99,99,reinterpret_cast<const uint8_t *>("New"),3,0,0,0,false);assert(m.selected==MessageCapacity-1&&m.textPage==0);
 m.page=Page::Messages;m.detail=false;m.gesture(Gesture::Hold);for(int i=0;i<70;++i){m.gesture(Gesture::Next);c.draw(m);}assert(m.selected<MessageCapacity&&m.textPage<(237+MessagePageChars-1)/MessagePageChars);
 // Explicit stock input events use the same production model as the physical
 // button, while SELECT and BACK are deliberately not a toggle pair.
 Model nav;const auto homeHash=frameHash(nav);nav.input(Input::Up);nav.input(Input::Down);nav.input(Input::Back);nav.input(Input::Select);
 assert(nav.page==Page::Home&&!nav.detail&&frameHash(nav)==homeHash);
 nav.input(Input::Right);assert(nav.page==Page::Messages&&!nav.detail&&frameHash(nav)!=homeHash);
 nav.input(Input::Left);assert(nav.page==Page::Home);nav.input(Input::UserPress);assert(nav.page==Page::Messages);
 nav.input(Input::Select);assert(!nav.detail);nav.text(1,1,reinterpret_cast<const uint8_t *>(body.data()),body.size(),-70,4,1,false);
 const auto listHash=frameHash(nav);nav.input(Input::Select);assert(nav.detail&&nav.selected==0&&nav.textPage==0&&frameHash(nav)!=listHash);
 nav.input(Input::Select);assert(nav.detail);nav.input(Input::Down);assert(nav.textPage==1);
 nav.input(Input::Up);assert(nav.textPage==0);nav.input(Input::Right);assert(nav.textPage==1);nav.input(Input::Left);assert(nav.textPage==0);
 nav.input(Input::Back);assert(!nav.detail&&nav.selected==0&&nav.textPage==0);nav.input(Input::Back);assert(!nav.detail&&nav.page==Page::Messages);
 // Canned page references the setting in place; first hold reviews, second
 // hold creates one request. Existing pages and navigation never do.
 nav.page=Page::Home;PendingCannedSend send;assert(!nav.takeCannedSend(send));
 nav.input(Input::Select);assert(!nav.takeCannedSend(send));nav.input(Input::Right);nav.input(Input::Right);nav.input(Input::Right);nav.input(Input::Right);nav.input(Input::Right);assert(nav.page==Page::Canned);
 const char canned[]="Hi|caf\xc3\xa9|Bye";nav.canned.bind(canned,sizeof(canned)-1);nav.cannedDestination=0xffffffffu;nav.cannedChannel=2;nav.cannedGeneration=9;
 nav.gesture(Gesture::Hold);assert(nav.cannedConfirm&&!nav.takeCannedSend(send));
 nav.gesture(Gesture::Hold);assert(nav.takeCannedSend(send)&&send.index==0&&send.destination==0xffffffffu&&send.channel==2&&send.generation==9);assert(!nav.takeCannedSend(send));
 nav.gesture(Gesture::Next);assert(nav.selected==1&&!nav.cannedConfirm);nav.gesture(Gesture::Hold);nav.gesture(Gesture::Next);assert(!nav.cannedConfirm&&nav.selected==1);nav.input(Input::Back);assert(!nav.cannedConfirm&&!nav.takeCannedSend(send));
 nav.page=Page::Canned;nav.setCannedMessages("Hola|Bye",8,true,true,0xffffffffu,2);assert(nav.cannedGeneration==10&&!nav.takeCannedSend(send));
 nav.gesture(Gesture::Hold);nav.gesture(Gesture::Hold);assert(nav.takeCannedSend(send)&&send.generation==10&&nav.cannedResult==1);
 Model stale;char mutableList[]="A|B";stale.page=Page::Canned;stale.setCannedMessages(mutableList,3,true,true,0xffffffffu,0);stale.gesture(Gesture::Hold);stale.gesture(Gesture::Hold);mutableList[2]='C';stale.setCannedMessages(mutableList,3,true,true,0xffffffffu,0);assert(!stale.takeCannedSend(send)&&stale.cannedGeneration==2);
 stale.selected=stale.canned.count();stale.gesture(Gesture::Hold);assert(stale.page==Page::Home&&!stale.takeCannedSend(send));
 Model emptyCanned;emptyCanned.page=Page::Canned;emptyCanned.setCannedMessages("",0,true,true,0xffffffffu,0);emptyCanned.gesture(Gesture::Hold);assert(emptyCanned.page==Page::Home&&!emptyCanned.takeCannedSend(send));
 CannedMenu parts;const char *part=nullptr;size_t partLength=0;
 parts.bind("A||B",4);assert(parts.count()==3&&parts.entry(1,part,partLength)&&partLength==0&&parts.entry(2,part,partLength)&&partLength==1&&*part=='B');
 parts.bind("|A",2);assert(parts.count()==2&&parts.entry(0,part,partLength)&&partLength==0&&parts.entry(1,part,partLength)&&partLength==1&&*part=='A');
 parts.bind("A|B|",4);assert(parts.count()==2&&parts.entry(1,part,partLength)&&partLength==2&&!memcmp(part,"B|",2));
 char many[100]{};size_t manyLen=0;for(unsigned i=0;i<48;++i){if(i)many[manyLen++]='|';many[manyLen++]='x';}parts.bind(many,manyLen);assert(parts.count()==48&&parts.itemCount()==49&&parts.entry(47,part,partLength)&&partLength==1);
 many[manyLen++]='|';many[manyLen++]='x';parts.bind(many,manyLen);assert(parts.count()==48&&parts.itemCount()==49&&parts.entry(47,part,partLength)&&partLength==1&&part[0]=='x');
 Model emptyEntry;emptyEntry.page=Page::Canned;emptyEntry.setCannedMessages("A||B",4,true,true,0xffffffffu,0);emptyEntry.selected=1;emptyEntry.gesture(Gesture::Hold);emptyEntry.gesture(Gesture::Hold);assert(!emptyEntry.takeCannedSend(send)&&!emptyEntry.cannedConfirm);
 Model noSend;for(unsigned p=0;p<5;++p){noSend.page=Page(p);noSend.input(Input::Select);assert(!noSend.takeCannedSend(send));}
 nav.page=Page::Nodes;nav.input(Input::Select);assert(!nav.detail);nav.node(2,"Node A","NODA",127,-80,5,2);nav.node(3,"Node B","NODB",81,-90,2,3);
 nav.input(Input::Select);assert(nav.detail&&nav.selected==0);nav.input(Input::UserPress);assert(nav.selected==1);nav.input(Input::Up);assert(nav.selected==0);nav.input(Input::Back);assert(!nav.detail);
 // Navigation uses modular uptime arithmetic through wrap and ignores bounce.
 Button b;Gesture g;assert(!b.update(true,0xffffffe0,g));assert(!b.update(true,0,g));assert(!b.update(false,50,g));assert(!b.update(false,81,g));assert(b.update(false,432,g)&&g==Gesture::Next);
 b={};assert(!b.update(true,0,g));assert(!b.update(true,31,g));assert(!b.update(false,100,g));assert(!b.update(false,131,g));assert(!b.update(true,180,g));assert(!b.update(true,211,g));assert(!b.update(false,260,g));assert(b.update(false,291,g)&&g==Gesture::Previous);
 b={};assert(!b.update(true,100,g));assert(!b.update(true,131,g));assert(!b.update(false,1000,g));assert(b.update(false,1031,g)&&g==Gesture::Hold);
 b={};assert(!b.update(true,0,g));assert(!b.update(false,10,g));assert(!b.update(false,50,g));assert(!b.update(false,1000,g));
 m.page=Page::Nodes;m.detail=true;m.selected=0;m.now=0xfffffff0;save(dir+"/node-old.ppm",m);
 for(unsigned p=0;p<6;++p){m.page=Page(p);m.detail=false;save(dir+"/orientation-normal-"+std::to_string(p)+".ppm",m);c.flipped=true;save(dir+"/orientation-flipped-"+std::to_string(p)+".ppm",m);c.flipped=false;}
 puts("PASS ZT UI: six pages, canned review/confirm/cancel and one-shot request, empty/detail/long-message/fault render, SELECT/BACK asymmetry, RGB565 wire bytes, bounded rings, duplicate filtering, selection, paging, button wrap/debounce and orientation fixtures");
}
