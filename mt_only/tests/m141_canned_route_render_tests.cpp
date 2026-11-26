// SPDX-License-Identifier: GPL-3.0-only
#include "ui/Render.h"
#include <cassert>
#include <string>

using namespace mt::ui;
static constexpr uint16_t SelectedColor=Canvas::Mono?WHITE:PINK;

static bool glyphsVisible(const Canvas &canvas,int x,int y,const char *text,uint16_t color) {
  for(unsigned i=0;text[i];++i){
    const unsigned ch=uint8_t(text[i]);if(ch<32||ch>126)return false;
    for(unsigned col=0;col<sizeof(cfb_font_0608[0]);++col)for(unsigned row=0;row<8;++row)
      if(cfb_font_0608[ch-32][col]&(1u<<row))
      if(canvas.color(unsigned(x+int(i*6+col)),unsigned(y+int(row)))!=color)return false;
  }
  return true;
}

static uint64_t drawHash(Canvas &canvas,const Model &model) {
  canvas.window(0);memset(canvas.bytes,0,sizeof(canvas.bytes));canvas.draw(model);
  assert(canvas.clipped==0);
  uint64_t hash=1469598103934665603ull;
  for(uint8_t byte:canvas.bytes){hash^=byte;hash*=1099511628211ull;}
  return hash;
}

int main(){
  Model model;Canvas canvas;model.page=Page::Canned;
  model.setCannedContent("Route test|Second",17,true,true);
  model.node(0xA1B2C3D4,"VeryLongRecentPeerName","PEER",127,-80,4,100);
  model.node(0x10203040,"Another peer","ANOT",127,-81,3,99);
  model.setCannedRoutes(0xff,7);
  model.cannedSubstate=CannedSubstate::Destinations;
  model.cannedRouteSelected=2;
  const auto &peer=model.nodes[1];
  assert(peer.id==0xA1B2C3D4);
  char label[64]{};canvas.cannedRouteChoice(model,2,label,sizeof(label),Canvas::Width==240?26:8);
  assert(strstr(label,"!a1b2c3d4")||strstr(label,"!A1B2C3D4"));
  uint64_t destinationHash=drawHash(canvas,model);
  const char *id=strstr(label," !");assert(id);id+=1;
  const int rowY=Canvas::Width==240?67:25;
  const int idX=Canvas::px(4)+(2+int(id-label))*6;
  const int pixelY=Canvas::px(rowY);
  assert(glyphsVisible(canvas,idX,pixelY,id,SelectedColor));

  // Back is a real selected row, visible independently of the scrolling peer window.
  model.cannedRouteSelected=model.cannedDestinationCount();
  uint64_t backHash=drawHash(canvas,model);assert(backHash!=destinationHash);
  const int backY=Canvas::Width==240?105:34;
  assert(glyphsVisible(canvas,Canvas::px(Canvas::Width==240?9:4),Canvas::px(backY),"> BACK",SelectedColor));

  // Channel choices stay numeric, and the explicit Back row is still present.
  model.cannedSubstate=CannedSubstate::Channels;
  model.cannedRouteSelected=7;
  uint64_t channelHash=drawHash(canvas,model);
  assert(channelHash!=backHash);
  char channel[64]{};canvas.cannedRouteChoice(model,7,channel,sizeof(channel),8);
  assert(!strcmp(channel,"Channel 7"));
  const int channelY=Canvas::Width==240?94:25;
  assert(glyphsVisible(canvas,Canvas::px(4),Canvas::px(channelY),"> Channel 7",SelectedColor));

  // Confirmation remains on the message view and visibly includes the exact route.
  model.cannedSubstate=CannedSubstate::Messages;model.cannedConfirm=true;
  model.cannedDestination=0xA1B2C3D4;model.cannedChannel=7;
  uint64_t confirmHash=drawHash(canvas,model);assert(confirmHash!=channelHash);
  assert(glyphsVisible(canvas,Canvas::px(Canvas::Width==240?9:4),Canvas::px(Canvas::Width==240?47:26),
                       "TO !a1b2c3d4 / CH7",Canvas::Mono?WHITE:LAVENDER));
  assert(glyphsVisible(canvas,Canvas::px(Canvas::Width==240?9:4),Canvas::px(Canvas::Width==240?83:34),
                       "Route test",WHITE));
  assert(!canvas.clipped);
}
