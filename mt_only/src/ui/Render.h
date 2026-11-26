// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "Model.h"
#include "TelemetryView.h"
#include "font6x8.h"
#include "vaporwave_palette.h"
#include <stdlib.h>
namespace mt::ui {
struct Canvas {
  static constexpr unsigned Width=CONFIG_MT_UI_WIDTH,Height=CONFIG_MT_UI_HEIGHT;
  static constexpr bool Mono=CONFIG_MT_UI_MONO!=0;
  static_assert((Width==128&&Height==64&&Mono)||(Width==160&&Height==80&&!Mono)||(Width==240&&Height==135&&!Mono));
  static constexpr unsigned Rows=CONFIG_MT_UI_STRIP_ROWS;
  static_assert(Rows>=1&&Rows<=Height&&(!Mono||Rows==Height));
  uint8_t bytes[Mono?Width*((Rows+7)/8):Width*Rows*2]{};
  bool flipped=false;
  unsigned firstRow=0;
  void window(unsigned first){firstRow=first;}
  unsigned clipped=0;
  static int px(int v){return Width==160?v*5/4:v;}
  void pixel(int x,int y,uint16_t c){
    if(x<0||y<0||x>=int(Width)||y>=int(Height)){++clipped;return;}
    // Transform before selecting the output strip so 180-degree orientation
    // works for full-frame, monochrome and bounded strip backends alike.
    if(flipped){x=int(Width)-1-x;y=int(Height)-1-y;}
    if(y<int(firstRow)||y>=int(firstRow+Rows))return;
    y-=int(firstRow);
    if constexpr(Mono){const unsigned i=unsigned(y/8)*Width+unsigned(x);const uint8_t bit=1u<<(y%8);if(c!=BG)bytes[i]|=bit;else bytes[i]&=uint8_t(~bit);}
    else {const unsigned i=(unsigned(y)*Width+unsigned(x))*2;bytes[i]=c>>8;bytes[i+1]=c;}
  }
  uint16_t color(unsigned x,unsigned y)const{if(y<firstRow||y>=firstRow+Rows)return BG;y-=firstRow;if constexpr(Mono)return bytes[(y/8)*Width+x]&(1u<<(y%8))?WHITE:0;else {const unsigned i=(y*Width+x)*2;return uint16_t(bytes[i]*256+bytes[i+1]);}}
  void wf(int x,int y,int w,int h,uint16_t c){for(int j=y;j<y+h;++j)for(int i=x;i<x+w;++i)pixel(i,j,c);}
  void wl(int x,int y,int x1,int y1,uint16_t c){int dx=abs(x1-x),sx=x<x1?1:-1,dy=-abs(y1-y),sy=y<y1?1:-1,e=dx+dy;for(;;){pixel(x,y,c);if(x==x1&&y==y1)break;int e2=2*e;if(e2>=dy){e+=dy;x+=sx;}if(e2<=dx){e+=dx;y+=sy;}}}
  void rect(int x,int y,int w,int h,uint16_t c){wf(px(x),px(y),px(w),px(h),c);}
  void line(int x,int y,int x1,int y1,uint16_t c){wl(px(x),px(y),px(x1),px(y1),c);}
  void lettering(int x,int y,const char *s,uint16_t c,unsigned max=25,unsigned scale=1){
    for(unsigned i=0,colAt=0;s[i]&&colAt<max;++i,++colAt){unsigned ch=uint8_t(s[i]);
#if defined(CONFIG_MT_BASEUI_EMOTES)
      if(ch>=128){
        int found=stock::ui::emotes::match(s+i,strlen(s+i));
        if(found>=0){
          emote(x+int(6*colAt*scale),y,unsigned(found),c,8*scale);
          i+=stock::ui::emotes::entries[found].size-1;++colAt;continue;
        }
      }
#endif
      if(ch<32||ch>126)ch='?';
      for(unsigned col=0;col<sizeof(cfb_font_0608[0]);++col)for(unsigned row=0;row<8;++row)if(cfb_font_0608[ch-32][col]&(1u<<row))for(unsigned sy=0;sy<scale;++sy)for(unsigned sx=0;sx<scale;++sx)pixel(x+int((6*colAt+col)*scale+sx),y+int(row*scale+sy),c);}
  }
  void text(int x,int y,const char *s,uint16_t c,unsigned max=25){lettering(px(x),px(y),s,c,max);}
  void right(int x,int y,const char *s,uint16_t c){lettering(px(x)-int(strlen(s))*6,px(y),s,c);}
  void center(int y,const char *s,uint16_t c){lettering((int(Width)-int(strlen(s))*6)/2,px(y),s,c);}
  // Exact MIT procedural artwork from the frozen Rev2 source.
  #include "vaporwave_assets.inc"
  void wt(int x,int y,const char *s,uint16_t c,unsigned scale=1,unsigned max=38){lettering(x,y,s,c,max,scale);}
  void wr(int x,int y,const char *s,uint16_t c){wt(x-int(strlen(s))*6,y,s,c);}
  void cannedLine(const Model &m,int x,int y,unsigned maxChars){
    const char *entry=nullptr;size_t length=0;char shown[64]{};
#if defined(CONFIG_MT_ONSCREEN_KEYBOARD)
    if(m.composing()&&m.cannedConfirm){text(x,y,m.keyboard.text,WHITE,maxChars);return;}
#endif
    if(m.canned.entry(m.selected,entry,length)){if(!length)snprintf(shown,sizeof(shown),"(empty)");else {size_t n=length<maxChars?length:maxChars;if(n>=sizeof(shown))n=sizeof(shown)-1;
      for(size_t i=0;i<n;++i)shown[i]=uint8_t(entry[i])>=32&&uint8_t(entry[i])<127?entry[i]:'?';}}
    else snprintf(shown,sizeof(shown),"NO MESSAGES");
    text(x,y,shown,WHITE,maxChars);
  }
  void cannedRouteChoice(const Model &m,unsigned row,char *out,size_t capacity,unsigned maxName)const {
    if(!capacity)return;
    out[0]=0;
    if(m.cannedSubstate==CannedSubstate::Destinations){
      uint32_t destination=Model::CannedBroadcast;
      if(!m.cannedDestinationAt(row,destination)){snprintf(out,capacity,"-");return;}
      if(destination==Model::CannedBroadcast){snprintf(out,capacity,"Broadcast");return;}
      for(unsigned i=0;i<m.nodeCount&&i<16;++i)if(m.nodes[i].id==destination){
        snprintf(out,capacity,"%.*s !%08x",int(maxName),m.nodeLabel(m.nodes[i],"Peer"),unsigned(destination));return;
      }
      snprintf(out,capacity,"!%08x",unsigned(destination));return;
    }
    uint8_t channel=0;
    if(m.cannedChannelAt(row,channel))snprintf(out,capacity,"Channel %u",unsigned(channel));
    else snprintf(out,capacity,"-");
  }
  void cannedRouteRow(const Model &m,unsigned row,unsigned selected,int y,unsigned maxChars,unsigned maxName){
    char line[64]{};line[0]=row==selected?'>':' ';line[1]=' ';
    const size_t labelCapacity=maxChars>2&&maxChars-1<sizeof(line)-2?maxChars-1:sizeof(line)-2;
    cannedRouteChoice(m,row,line+2,labelCapacity,maxName);
    text(4,y,line,row==selected?PINK:WHITE,maxChars);
  }
  void cannedRouteView(const Model &m){
    const bool destinations=m.cannedSubstate==CannedSubstate::Destinations;
    const unsigned count=destinations?m.cannedDestinationCount():m.cannedChannelCount();
    const unsigned visible=Width==240?6:1;
    const unsigned selected=m.cannedRouteSelected;
    unsigned first=Width==240?(selected?selected-1:0):(selected<count?selected:(count?count-1:0));
    const unsigned last=count>visible?count-visible:0;
    if(first>last)first=last;
    if constexpr(Width==240){
      wt(9,30,destinations?"RECENT PEERS":"CHANNELS",CYAN,2);
      for(unsigned i=0;i<visible&&first+i<count;++i)
        cannedRouteRow(m,first+i,selected,49+int(i*9),38,26);
      wt(9,105,selected==count?"> BACK":"  BACK",selected==count?PINK:LAVENDER);
    }else{
      text(4,17,destinations?"RECENT PEERS":"CHANNELS",CYAN);
      for(unsigned i=0;i<visible&&first+i<count;++i)
        cannedRouteRow(m,first+i,selected,25+int(i*8),20,8);
      text(4,34,selected==count?"> BACK":"  BACK",selected==count?PINK:LAVENDER,20);
      text(4,43,"HOLD PICK",LAVENDER,20);
    }
  }
  void telemetryView(const Model &m,bool wideDisplay){
#if defined(CONFIG_MT_TELEMETRY_REPORT_FRAMES)
    if(m.detail){
      const auto report=m.reports.page(m.selected,m.enabledReports(),m.telemetry.fahrenheit);
      if(wideDisplay){
        wt(9,26,report.title,CYAN,1,30);wt(9,43,report.source,LAVENDER,1,30);
        for(unsigned i=0;i<3;++i)wt(9,63+15*i,report.rows[i],WHITE,1,35);
      }else{
        text(4,15,report.title,CYAN,20);text(4,23,report.source,LAVENDER,20);
        for(unsigned i=0;i<3;++i)text(4,31+7*i,report.rows[i],WHITE,20);
      }
      return;
    }
#endif
    char rows[4][36]{};const unsigned count=TelemetryView::lines(m.telemetry,m.nowMs,m.telemetry.fahrenheit,rows);
    if(wideDisplay){
#if defined(CONFIG_MT_TELEMETRY_REPORT_FRAMES)
      wt(9,31,"LOCAL / HOLD REPORTS",CYAN,1);
#else
      wt(9,31,"LOCAL TELEMETRY",CYAN,2);
#endif
      for(unsigned i=0;i<count&&i<4;++i)wt(9,55+i*15,rows[i],i?LAVENDER:WHITE,1,35);
      if(!count)wt(9,58,"NO TELEMETRY SCREENS ENABLED",LAVENDER,1,34);
    }else{
#if defined(CONFIG_MT_TELEMETRY_REPORT_FRAMES)
      text(4,17,"LOCAL / HOLD REPORTS",CYAN,20);
#else
      text(4,17,"LOCAL TELEMETRY",CYAN,20);
#endif
      for(unsigned i=0;i<count&&i<4;++i)text(4,26+i*7,rows[i],i?LAVENDER:WHITE,20);
      if(!count)text(4,35,"NO SCREENS ENABLED",LAVENDER,20);
    }
  }
  void wide(const Model &m){
    wf(0,0,240,135,BG);wl(1,1,238,1,LAVENDER);wl(1,133,238,133,LAVENDER);wl(1,1,1,133,LAVENDER);wl(238,1,238,133,LAVENDER);
    wl(2,22,237,22,CYAN);wl(2,114,237,114,CYAN);wt(8,4,"ZEPHTASTIC",CYAN,2);
    wr(232,8,m.bleConnected?"BT LINK":m.bleReady?"BT ON":"BT OFF",CYAN);
    if(!m.detail)for(unsigned i=0;i<PageCount;++i)wf(8+i*8,122,i==unsigned(m.page)?5:4,i==unsigned(m.page)?5:4,i==unsigned(m.page)?PINK:CYAN);
    wr(232,121,m.detail?"TAP MORE / HOLD BACK":(m.page==Page::Messages||m.page==Page::Nodes)?"TAP / HOLD VIEW":(m.page==Page::Canned&&m.cannedSubstate!=CannedSubstate::Messages)?"TAP NEXT / 2X PREV / HOLD PICK":"TAP NEXT / 2X PREV",LAVENDER);
    char b[48];
    if(m.detail&&m.page==Page::Messages&&m.selected<m.messageCount){
      const auto &msg=m.messages[m.selected];snprintf(b,sizeof(b),"%u/%u  !%08x",m.selected+1,m.messageCount,unsigned(msg.from));wt(9,29,b,PINK);
      const size_t len=strlen(msg.text),offset=m.textPage*MessagePageChars;
      for(unsigned row=0;row<4;++row){size_t at=offset+row*18;char part[19]{};if(at<len){size_t n=len-at;if(n>18)n=18;memcpy(part,msg.text+at,n);}wt(9,43+row*17,part,WHITE,2);}
      return;
    }
    if(m.detail&&m.page==Page::Nodes&&m.selected<m.nodeCount){
      const auto &n=m.nodes[m.selected];wt(9,30,m.nodeLabel(n,"Unnamed node"),PINK,2,18);
      snprintf(b,sizeof(b),"!%08x  %s",unsigned(n.id),n.shortName);wt(9,51,b,WHITE);
      snprintf(b,sizeof(b),"RSSI %d dBm / SNR %d",n.rssi,n.snr);wt(9,70,b,LAVENDER);
      snprintf(b,sizeof(b),"HW %u / AGE %us",n.model,unsigned((m.now-n.at)/1000));wt(9,89,b,LAVENDER);return;
    }
    switch(m.page){
    case Page::Home:
      wt(9,29,m.name[0]?m.name:"ZT",LAVENDER,2,9);wide_scene();
      wt(125,50,"NODES",LAVENDER);snprintf(b,sizeof(b),"%u",m.nodeCount);wr(228,50,b,WHITE);
      wt(125,65,"TEXT",LAVENDER);snprintf(b,sizeof(b),"%u",m.messageCount);wr(228,65,b,WHITE);
      wt(125,80,m.fault?"RF FAULT":!m.settingsReady?"SETUP BLOCKED":m.receiving?"RF RX ON":"RF OFF",m.fault||!m.settingsReady?PINK:WHITE);
      if(m.usbConnected){wide_usb(126,96);wt(138,98,"USB",CYAN);}
      if(m.bleConnected){wide_ble(178,96);wt(190,98,"BLE",CYAN);}break;
    case Page::Messages:
      if(!m.messageCount)wt(48,58,"NO TEXT YET",LAVENDER,2);
      for(unsigned i=0;i<m.messageCount&&i<4;++i){wt(9,31+i*20,m.messages[i].local?"L":"R",PINK,2);wt(26,35+i*20,m.messages[i].text,i?LAVENDER:WHITE,1,33);}break;
    case Page::Nodes:
      if(!m.nodeCount)wt(24,58,"NO NODEINFO YET",LAVENDER,2);
      for(unsigned i=0;i<m.nodeCount&&i<4;++i){snprintf(b,sizeof(b),"!%08x",unsigned(m.nodes[i].id));wt(9,31+i*20,m.nodeLabel(m.nodes[i],b),i?LAVENDER:PINK,2,18);}break;
    case Page::Channel:
      wt(9,31,m.channelLabel(),CYAN,2);
      m.frequencyLabel(b,sizeof(b));wt(9,56,b,WHITE);
      m.modemLabel(b,sizeof(b));wt(9,81,b,LAVENDER);wt(9,99,m.radio.loaded&&m.radio.enabled?m.radio.modem:"SETUP VIA USB / BLE",LAVENDER);break;
    case Page::Status:
      snprintf(b,sizeof(b),"!%08x  ZT",unsigned(m.identity));wt(9,31,b,CYAN);
      wt(9,51,m.fault?"RADIO FAULT":m.radioReady?"SX1262 READY":"SX1262 NOT READY",m.fault?PINK:WHITE,2);
      m.gnss.label(b,sizeof(b));wt(9,78,b,m.gnss.state==zt::gnss::DisplayState::Fault?PINK:WHITE);
      m.powerLabel(b,sizeof(b));wt(9,98,m.settingsReady?b:"SETTINGS FAULT",m.settingsReady?LAVENDER:PINK);break;
    case Page::Canned:
      if(m.cannedSubstate!=CannedSubstate::Messages){cannedRouteView(m);break;}
      wt(9,31,m.cannedReady?(m.cannedEnabled?"CANNED TEXT":"CANNED OFF"):"SETTINGS LOADING",m.cannedReady?CYAN:PINK,2);
      if(m.cannedDestination==0xffffffffu)snprintf(b,sizeof(b),"TO BROADCAST / CH%u",unsigned(m.cannedChannel));else snprintf(b,sizeof(b),"TO !%08x / CH%u",unsigned(m.cannedDestination),unsigned(m.cannedChannel));
      wt(9,47,b,LAVENDER);if(m.cannedConfirm){wt(9,62,"SEND THIS TEXT?",PINK,2);cannedLine(m,9,83,28);}
      else cannedLine(m,9,66,32);
      if(m.cannedResult==1)wt(9,99,"QUEUED",CYAN);else if(m.cannedResult==2)wt(9,99,"NOT QUEUED",PINK);else if(m.cannedResult==3)wt(9,99,"SENT",CYAN);else if(m.cannedResult==4)wt(9,99,"RELAYED",LAVENDER);else if(m.cannedResult==5)wt(9,99,"SEND FAILED",PINK);
      else if(m.cannedConfirm)wt(9,103,"HOLD SEND / TAP CANCEL",LAVENDER);else wt(9,87,m.selected==m.canned.count()?"HOLD TO GO HOME":m.canned.count()?"HOLD TO REVIEW":"NO MESSAGES",LAVENDER);break;
    #if defined(CONFIG_MT_BASEUI_GAMES)
    case Page::Games:break; // The game owns the whole frame in draw().
#endif
    case Page::Telemetry:telemetryView(m,true);break;
    }
  }
  void splash(const char *label,const char *version){
    clipped=0;
    if constexpr(Width==240){wf(0,0,240,135,BG);wide_scene();wt(24,12,"ZEPHTASTIC",CYAN,3);wt(125,55,label,LAVENDER,1,18);wt(125,75,version,WHITE,1,18);}
    else {rect(0,0,128,64,BG);for(int row=-12;row<=12;row+=3){int rr=144-row*row,half=0;while((half+1)*(half+1)<=rr)++half;rect(64-half,20+row,half*2+1,1,row<-2?PEACH:PINK);}center(17,"ZEPHTASTIC",CYAN);center(39,label,LAVENDER);center(51,version,WHITE);}
  }
  void pairing(uint32_t passkey){
    clipped=0;wf(0,0,Width,Height,BG);
    const char *title="BLUETOOTH PIN";
    lettering((int(Width)-int(strlen(title))*6)/2,int(Height)/6,title,CYAN);
    char digits[7]{};snprintf(digits,sizeof(digits),"%06u",unsigned(passkey%1000000u));
    constexpr unsigned scale=Width>=240?3:2;
    lettering((int(Width)-36*int(scale))/2,int(Height)/2-4*int(scale),digits,WHITE,6,scale);
    const char *hint="Enter on your phone";
    lettering((int(Width)-int(strlen(hint))*6)/2,int(Height)-14,hint,LAVENDER);
    memset(digits,0,sizeof(digits));
  }
#if defined(CONFIG_MT_BASEUI_EMOTES)
  void emote(int x,int y,unsigned index,uint16_t color,int size){
    uint8_t bitmap[32];stock::ui::emotes::bitmap(stock::ui::emotes::entries[index].glyph,bitmap);
    for(int dy=0;dy<size;++dy)for(int dx=0;dx<size;++dx){
      const int sx=dx*16/size,sy=dy*16/size;
      if(bitmap[2*sy+sx/8]&(1u<<(sx%8)))pixel(x+dx,y+dy,color);
    }
  }
#endif
  void drawKeyboard(const stock::ui::OnScreenKeyboardModule &k,const char *title){
      wf(0,0,Width,Height,BG);
#if defined(CONFIG_MT_BASEUI_EMOTES)
      if(k.picker){
        text(2,2,"EMOTES",CYAN);
        const unsigned first=k.emote/12*12;
        for(unsigned i=first;i<first+12&&i<stock::ui::emotes::Count;++i){
          const int x=px(4)+int((i-first)%6)*19,y=px(15)+int((i-first)/6)*20;
          if(i==k.emote)wf(x-1,y-1,18,18,PINK);
          emote(x,y,i,i==k.emote?BG:WHITE,16);
        }
        text(2,54,"HOLD PICK / TAP NEXT",LAVENDER);return;
      }
#endif
      text(2,1,title,CYAN);
      text(2,12,k.text+(k.length>19?k.length-19:0),WHITE,19);
      for(unsigned row=0;row<k.Rows;++row)for(unsigned col=0;col<k.Columns;++col){
        const int x=2+int(col)*11,y=23+int(row)*10;
        char key=k.Keys[row][col];const char label[]={key,0};
        const char *caption=key=='\b'?"<":key=='\n'?"OK":key=='\x1b'?"X":key==' '?"_":label;
        const bool selected=k.cursor==row*k.Columns+col;
        if(selected)rect(x-1,y-1,col==10?14:9,10,PINK);
        text(x,y,caption,selected?BG:WHITE,2);
      }
  }
#if defined(CONFIG_MT_BASEUI_GAMES)
  void drawGame(const stock::ui::GamesModule &g){
    using State=stock::ui::GamesModule::State;
    wf(0,0,Width,Height,BG);
    if(g.state==State::Initials){drawKeyboard(g.initials,"HIGH SCORE INITIALS");return;}
    if(g.state!=State::Playing){
      center(3,g.Names[g.selected],CYAN);char row[24]{};
      switch(g.state){
      case State::Attract:center(22,"UP/DOWN: GAME",WHITE);center(35,"SELECT: PLAY",PINK);break;
      case State::Paused:center(20,"PAUSED",PINK);center(34,"SELECT: RESUME",WHITE);center(45,"BACK: QUIT",LAVENDER);break;
      case State::GameOver:snprintf(row,sizeof(row),"SCORE %lu",(unsigned long)g.lastScore);
        center(22,row,WHITE);center(39,"SELECT: SCORES",PINK);break;
      case State::ConfirmClear:center(22,"CLEAR SCORES?",WHITE);center(39,g.clearYes?"YES":"NO",PINK);break;
      case State::Scores:
        for(unsigned i=0;i<5;++i){const auto &entry=g.scores[g.selected].entries[i];
          snprintf(row,sizeof(row),"%u %.3s %lu",i+1,entry.initials,(unsigned long)entry.value);text(8,14+int(i)*9,row,WHITE);}
        break;
      default:break;
      }
      return;
    }
    char score[20];snprintf(score,sizeof(score),"%lu",(unsigned long)g.score());
    switch(g.selected){
    case 0:{
      text(2,2,score,WHITE);
      for(unsigned i=0;i<g.snake.length();++i){auto c=g.snake.bodyAt(i);rect(c.x*4,c.y*4+13,3,3,CYAN);}
      auto food=g.snake.food();rect(food.x*4,food.y*4+13,3,3,PINK);break;
    }
    case 1:{
      for(unsigned row=0;row<16;++row)for(unsigned col=0;col<10;++col)
        if(g.tetris.board[row][col])rect(2+col*4,row*4,3,3,CYAN);
      const auto &p=g.tetris.current();
      for(unsigned row=0;row<4;++row)for(unsigned col=0;col<4;++col)
        if(TetrisGame::pieceCell(p.type,p.rot,row,col)&&int(row)+p.row>=0)
          rect(2+(int(col)+p.col)*4,(int(row)+p.row)*4,3,3,PINK);
      text(56,12,"SCORE",CYAN);text(56,23,score,WHITE);break;
    }
    case 2:
      text(2,2,score,WHITE);rect(0,62,128,1,LAVENDER);rect(14,g.chirpy.chirpyY(),12,16,CYAN);
      for(unsigned i=0;i<g.chirpy.obstacleSlots();++i)if(g.chirpy.obstacleActive(i))
        rect(g.chirpy.obstacleX(i),62-g.chirpy.obstacleH(i),g.chirpy.obstacleW(i),g.chirpy.obstacleH(i),PINK);
      break;
    case 3:
      text(2,2,score,WHITE);
      for(unsigned row=0;row<5;++row)for(unsigned col=0;col<8;++col)
        if(g.breakout.brickAt(row,col))rect(col*16,12+row*4,15,3,CYAN);
      rect(g.breakout.paddleX(),61,24,2,WHITE);rect(g.breakout.ballX(),g.breakout.ballY(),2,2,PINK);break;
    }
  }
#endif
  void draw(const Model &m){
    clipped=0;
    if(m.commands.open){
      wf(0,0,Width,Height,BG);
      text(3,2,"SYSTEM COMMANDS",CYAN);
      text(3,18,m.commands.Menu[m.commands.row].name,WHITE);
      text(3,33,m.commands.confirm?"HOLD TO CONFIRM":
           m.commands.reported?(m.commands.result==0?"DONE":"UNAVAILABLE"):"HOLD TO SELECT",PINK);
      text(3,49,"TAP: NEXT  2X: PREV",LAVENDER);return;
    }
#if defined(CONFIG_MT_ONSCREEN_KEYBOARD)
    if(m.keyboard.active){drawKeyboard(m.keyboard,"COMPOSE");return;}
#endif
#if defined(CONFIG_MT_BASEUI_GAMES)
    if(m.page==Page::Games){drawGame(m.games);return;}
#endif
    if constexpr(Width==240){wide(m);return;}
    rect(0,0,128,64,BG);line(1,1,126,1,LAVENDER);line(1,62,126,62,LAVENDER);line(1,1,1,62,LAVENDER);line(126,1,126,62,LAVENDER);
    line(2,13,125,13,CYAN);line(2,53,125,53,CYAN);
    text(4,3,"ZEPHTASTIC",CYAN);right(124,3,m.bleConnected?"BT LINK":m.bleReady?"BT ON":"BT OFF",CYAN);
    if(!m.detail)for(unsigned i=0;i<PageCount;++i)rect(4+i*5,58,i==unsigned(m.page)?3:2,i==unsigned(m.page)?3:2,i==unsigned(m.page)?PINK:CYAN);
    right(124,55,m.page==Page::Status?"HOLD SYSTEM MENU":m.detail?"TAP MORE HOLD BACK":(m.page==Page::Messages||m.page==Page::Nodes)?"TAP / HOLD VIEW":(m.page==Page::Canned&&m.cannedConfirm)?"2X PREV CANCEL":"TAP NEXT 2X PREV",LAVENDER);
    char b[48];
    if(m.detail&&m.page==Page::Messages&&m.selected<m.messageCount){
      const auto &msg=m.messages[m.selected];snprintf(b,sizeof(b),"%u/%u  !%08x",m.selected+1,m.messageCount,unsigned(msg.from));text(4,16,b,PINK);
      const size_t len=strlen(msg.text),offset=m.textPage*MessagePageChars;
      for(unsigned row=0;row<3;++row){constexpr unsigned columns=MessagePageChars/3;size_t at=offset+row*columns;char part[25]{};if(at<len){size_t n=len-at;if(n>columns)n=columns;memcpy(part,msg.text+at,n);}text(4,27+row*8,part,WHITE);}
      return;
    }
    if(m.detail&&m.page==Page::Nodes&&m.selected<m.nodeCount){
      const auto &n=m.nodes[m.selected];text(4,16,m.nodeLabel(n,"Unnamed node"),PINK,Width==128?20:24);
      snprintf(b,sizeof(b),"!%08x  %s",unsigned(n.id),n.shortName);text(4,26,b,WHITE);
      snprintf(b,sizeof(b),"RSSI %d  SNR %d",n.rssi,n.snr);text(4,35,b,LAVENDER);
      const unsigned hours=(m.now-n.at)/3600000;snprintf(b,sizeof(b),"HW %u AGE %uh",n.model,hours);text(4,44,b,LAVENDER);return;
    }
    switch(m.page){
    case Page::Home:
      text(4,16,"ZT",PINK);synth();text(59,17,"NODES",LAVENDER);snprintf(b,sizeof(b),"%u",m.nodeCount);right(123,17,b,WHITE);
      text(59,28,"TEXT",LAVENDER);snprintf(b,sizeof(b),"%u",m.messageCount);right(123,28,b,WHITE);
      text(59,Mono?35:37,m.fault?"RF FAULT":!m.settingsReady?"SETUP ERR":m.receiving?"RF RX ON":"RF OFF",m.fault||!m.settingsReady?PINK:WHITE);
      if constexpr(Mono){if(m.usbConnected)text(61,45,"USB",CYAN);if(m.bleConnected)text(105,45,"BT",CYAN);}
      else {if(m.usbConnected)usb_symbol(82,43);if(m.bleConnected)ble_symbol(105,43);}
      break;
    case Page::Messages:
      if(!m.messageCount){center(23,"MESSAGES",CYAN);center(37,"NO TEXT YET",LAVENDER);}
      for(unsigned i=0;i<m.messageCount&&i<3;++i){text(4,17+i*12,m.messages[i].local?"L":"R",PINK);text(13,17+i*12,m.messages[i].text,i?LAVENDER:WHITE,Width==128?18:22);}break;
    case Page::Nodes:
      if(!m.nodeCount){center(23,"NODES",CYAN);center(37,"NO NODEINFO YET",LAVENDER);}
      for(unsigned i=0;i<m.nodeCount&&i<3;++i){snprintf(b,sizeof(b),"!%08x",unsigned(m.nodes[i].id));text(4,17+i*12,m.nodeLabel(m.nodes[i],b),i?LAVENDER:PINK,Width==128?20:24);}break;
    case Page::Channel:
      text(4,17,m.channelLabel(),CYAN);
      m.frequencyLabel(b,sizeof(b));text(4,28,b,WHITE);
      m.modemLabel(b,sizeof(b));text(4,Mono?37:39,b,LAVENDER);text(4,Mono?45:47,m.radio.loaded&&m.radio.enabled?m.radio.modem:"SETUP VIA USB / BLE",LAVENDER);break;
    case Page::Status:
      snprintf(b,sizeof(b),"!%08x  ZT",unsigned(m.identity));text(4,17,b,CYAN);
#if defined(CONFIG_MT_LOCAL_CLOCK) && defined(CONFIG_MT_XIAO_EXPANSION_OLED)
      right(123,17,m.clockLabel,WHITE);
#endif
      text(4,28,m.fault?"RADIO FAULT":m.radioReady?"SX1262 READY":"SX1262 NOT READY",m.fault?PINK:WHITE);
      m.gnss.label(b,sizeof(b));text(4,Mono?37:39,b,m.gnss.state==zt::gnss::DisplayState::Fault?PINK:WHITE);
      m.powerLabel(b,sizeof(b));text(4,Mono?45:47,m.settingsReady?b:"SETTINGS FAULT",m.settingsReady?LAVENDER:PINK);break;
    case Page::Canned:
      if(m.cannedSubstate!=CannedSubstate::Messages){cannedRouteView(m);break;}
      text(4,17,m.cannedReady?(m.cannedEnabled?"CANNED TEXT":"CANNED OFF"):"SETTINGS LOADING",m.cannedReady?CYAN:PINK);
      if(m.cannedDestination==0xffffffffu)snprintf(b,sizeof(b),"TO BROADCAST / CH%u",unsigned(m.cannedChannel));else snprintf(b,sizeof(b),"TO !%08x / CH%u",unsigned(m.cannedDestination),unsigned(m.cannedChannel));
      text(4,26,b,LAVENDER,20);if(m.cannedConfirm){cannedLine(m,4,34,20);text(4,44,"HOLD SEND",PINK,20);}
      else cannedLine(m,4,34,20);
      if(m.cannedResult==1)text(4,47,"QUEUED",CYAN);else if(m.cannedResult==2)text(4,47,"NOT QUEUED",PINK);else if(m.cannedResult==3)text(4,47,"SENT",CYAN);else if(m.cannedResult==4)text(4,47,"RELAYED",LAVENDER);else if(m.cannedResult==5)text(4,47,"SEND FAILED",PINK);
      else if(!m.cannedConfirm)text(4,44,m.selected==m.canned.count()?"HOLD TO GO HOME":m.canned.count()?"HOLD TO REVIEW":"NO MESSAGES",LAVENDER);break;
    #if defined(CONFIG_MT_BASEUI_GAMES)
    case Page::Games:break; // The game owns the whole frame in draw().
#endif
    case Page::Telemetry:telemetryView(m,false);break;
    }
  }
};
}
