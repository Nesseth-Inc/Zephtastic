// SPDX-License-Identifier: GPL-3.0-only
// Host-only tests for bounded volatile StoreForward archive policy.
#include "modules/StoreForwardArchive.h"
#include <cassert>
#include <cstdio>
#include <cstring>
using mt::modules::StoreForwardArchive;

static StoreForwardArchive::MessageView message(uint32_t from,uint32_t to,uint32_t time,
                                                 uint32_t id,const uint8_t *payload,size_t size){
  StoreForwardArchive::MessageView m{};m.from=from;m.to=to;m.time=time;m.id=id;
  m.channel=uint8_t(id);m.replyId=id+10;m.emoji=(id&1)!=0;m.hasRxTime=true;
  m.rxRssi=-91;m.hasRxRssi=true;m.rxSnr=7.25f;m.hopStart=3;m.hopLimit=2;
  m.viaMqtt=(id&2)!=0;m.transportMechanism=4;m.payload=payload;m.payloadSize=size;return m;
}
static void selectionAndMetadata(){
  StoreForwardArchive::Record records[12]{};StoreForwardArchive::Cursor cursors[4]{};
  StoreForwardArchive archive(records,12,cursors,4);assert(archive.ready()&&archive.size()==0);
  const uint32_t requester=0x10203040u,other=0x50607080u,third=0x1234u;
  const uint8_t bytesA[]={0x41,0,0x42};const uint8_t bytesB[]={0xff,0x80};
  assert(archive.add(message(requester,StoreForwardArchive::BroadcastNode,101,1,bytesA,sizeof(bytesA)))==StoreForwardArchive::AddResult::Added);
  assert(archive.add(message(other,third,102,2,bytesA,sizeof(bytesA)))==StoreForwardArchive::AddResult::Added);
  assert(archive.add(message(other,StoreForwardArchive::BroadcastNode,100,3,bytesA,sizeof(bytesA)))==StoreForwardArchive::AddResult::Added);
  assert(archive.add(message(other,StoreForwardArchive::BroadcastNode,101,4,bytesB,sizeof(bytesB)))==StoreForwardArchive::AddResult::Added);
  assert(archive.add(message(other,requester,102,5,bytesB,sizeof(bytesB)))==StoreForwardArchive::AddResult::Added);
  assert(archive.add(message(other,third,103,6,bytesA,sizeof(bytesA)))==StoreForwardArchive::AddResult::Added);
  assert(archive.add(message(other,StoreForwardArchive::BroadcastNode,0,7,bytesA,sizeof(bytesA)))==StoreForwardArchive::AddResult::Added);
  assert(archive.add(message(other,StoreForwardArchive::BroadcastNode,104,8,bytesB,sizeof(bytesB)))==StoreForwardArchive::AddResult::Added);

  size_t count=99,last=99;assert(archive.countAvailable(requester,100,count,&last));
  assert(count==3&&last==0); // strict time, own-source/direct-to-other excluded
  StoreForwardArchive::Record selected{};size_t index=99,cursor=99;
  assert(archive.next(requester,100,selected,&index)&&index==3);
  assert(selected.from==other&&selected.to==StoreForwardArchive::BroadcastNode&&selected.id==4&&
         selected.time==101&&selected.channel==4&&selected.replyId==14&&selected.emoji==false);
  assert(selected.hasRxTime&&selected.hasRxRssi&&selected.rxRssi==-91&&selected.rxSnr==7.25f&&
         selected.hopStart==3&&selected.hopLimit==2&&!selected.viaMqtt&&selected.transportMechanism==4);
  assert(selected.payloadSize==sizeof(bytesB)&&!memcmp(selected.payload,bytesB,sizeof(bytesB)));
  assert(archive.cursor(requester,cursor)&&cursor==4);
  assert(archive.countAvailable(requester,100,count,&last)&&last==4&&count==2);
  assert(archive.next(requester,100,selected,&index)&&index==4&&selected.to==requester&&selected.id==5);
  assert(archive.next(requester,100,selected,&index)&&index==7&&selected.id==8);
  assert(!archive.next(requester,100,selected));
  size_t otherCount=0;assert(archive.countAvailable(third,0,otherCount)&&otherCount==6);
}
static void payloadBoundAndValidation(){
  StoreForwardArchive::Record records[2]{};StoreForwardArchive::Cursor cursors[2]{};
  StoreForwardArchive archive(records,2,cursors,2);uint8_t payload[StoreForwardArchive::MaxPayload];
  for(size_t i=0;i<sizeof(payload);++i)payload[i]=uint8_t(i);
  auto max=message(1,StoreForwardArchive::BroadcastNode,3,1,payload,sizeof(payload));
  assert(archive.add(max)==StoreForwardArchive::AddResult::Added&&archive.size()==1);
  StoreForwardArchive::Record selected{};assert(archive.next(9,0,selected));
  assert(selected.payloadSize==233&&selected.payload[0]==0&&selected.payload[232]==uint8_t(232));
  auto tooLong=max;tooLong.payloadSize=234;
  assert(archive.add(tooLong)==StoreForwardArchive::AddResult::InvalidMessage&&archive.size()==1);
  auto nullBytes=max;nullBytes.payload=nullptr;nullBytes.payloadSize=1;
  assert(archive.add(nullBytes)==StoreForwardArchive::AddResult::InvalidMessage&&archive.size()==1);
  StoreForwardArchive::Record zeroRecords[1]{};StoreForwardArchive noCursor(zeroRecords,1,nullptr,0);
  assert(!noCursor.ready()&&noCursor.add(max)==StoreForwardArchive::AddResult::Unavailable);
  StoreForwardArchive noStorage(nullptr,1,cursors,2);assert(!noStorage.ready());

  StoreForwardArchive::Record recordSentinel[1]{};StoreForwardArchive::Cursor cursorSentinel[1]{};
  recordSentinel[0].id=0x12345678u;recordSentinel[0].payload[0]=0xa5;
  cursorSentinel[0]={0x87654321u,7,true};
  const size_t recordOverflow=SIZE_MAX/sizeof(StoreForwardArchive::Record)+1;
  StoreForwardArchive rejectRecordOverflow(recordSentinel,recordOverflow,cursorSentinel,1);
  assert(!rejectRecordOverflow.ready()&&recordSentinel[0].id==0x12345678u&&
         recordSentinel[0].payload[0]==0xa5&&cursorSentinel[0].requester==0x87654321u&&
         cursorSentinel[0].nextIndex==7&&cursorSentinel[0].used);
  const size_t cursorOverflow=SIZE_MAX/sizeof(StoreForwardArchive::Cursor)+1;
  StoreForwardArchive rejectCursorOverflow(recordSentinel,1,cursorSentinel,cursorOverflow);
  assert(!rejectCursorOverflow.ready()&&recordSentinel[0].id==0x12345678u&&
         recordSentinel[0].payload[0]==0xa5&&cursorSentinel[0].requester==0x87654321u&&
         cursorSentinel[0].nextIndex==7&&cursorSentinel[0].used);
}
static void overwriteResetsCursorEpoch(){
  StoreForwardArchive::Record records[3]{};StoreForwardArchive::Cursor cursors[3]{};
  StoreForwardArchive archive(records,3,cursors,3);const uint32_t clientA=101,clientB=202;
  const uint8_t body[]={1};
  for(uint32_t i=1;i<=3;++i)
    assert(archive.add(message(9,StoreForwardArchive::BroadcastNode,i,i,body,sizeof(body)))==StoreForwardArchive::AddResult::Added);
  StoreForwardArchive::Record selected{};assert(archive.next(clientA,0,selected)&&selected.id==1);
  size_t cursor=0;assert(archive.cursor(clientA,cursor)&&cursor==1);
  size_t available=0;assert(archive.countAvailable(clientB,0,available)&&available==3);
  const uint32_t before=archive.epoch();
  assert(archive.add(message(9,StoreForwardArchive::BroadcastNode,4,4,body,sizeof(body)))==StoreForwardArchive::AddResult::OverwroteOldest);
  assert(archive.epoch()==before+1&&archive.size()==1);
  assert(archive.cursor(clientA,cursor)&&cursor==0&&archive.cursor(clientB,cursor)&&cursor==0);
  assert(archive.countAvailable(clientA,0,available)&&available==1);
  assert(archive.next(clientA,0,selected)&&selected.id==4);
  assert(!archive.next(clientA,0,selected));
}
static void boundedCursorEvictionAndReset(){
  StoreForwardArchive::Record records[4]{};StoreForwardArchive::Cursor cursors[2]{};
  StoreForwardArchive archive(records,4,cursors,2);const uint8_t body[]={0x42};
  assert(archive.add(message(7,StoreForwardArchive::BroadcastNode,9,9,body,sizeof(body)))==StoreForwardArchive::AddResult::Added);
  size_t count=0,index=0;assert(archive.countAvailable(10,0,count,&index)&&count==1&&index==0);
  assert(archive.countAvailable(11,0,count)&&count==1);
  StoreForwardArchive::Record selected{};
  assert(archive.next(10,0,selected)&&selected.id==9); // selection advances before delivery
  assert(archive.countAvailable(12,0,count)&&count==1); // bounded eviction replaces requester 10
  assert(!archive.cursor(10,index)&&archive.cursor(11,index)&&archive.cursor(12,index));
  assert(archive.cursor(12,index)&&index==0);
  assert(archive.countAvailable(10,0,count,&index)&&count==1&&index==0);
  assert(!archive.cursor(11,index)&&archive.cursor(10,index)&&index==0);
  const uint32_t epoch=archive.epoch();archive.clear();
  assert(archive.size()==0&&archive.epoch()==epoch+1&&!archive.cursor(10,index)&&!archive.cursor(12,index));
  assert(archive.countAvailable(10,0,count,&index)&&count==0&&index==0);
}
int main(){
  selectionAndMetadata();payloadBoundAndValidation();overwriteResetsCursorEpoch();boundedCursorEvictionAndReset();
  puts("PASS bounded volatile StoreForward archive selection, metadata, payload limits, requester cursors, overwrite reset, and reboot reset");
  return 0;
}
