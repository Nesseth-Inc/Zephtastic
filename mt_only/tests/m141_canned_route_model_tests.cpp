#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/ui/Model.h"

using namespace mt::ui;

static void testLegacyUnchanged(){
  Model m;m.page=Page::Canned;m.setCannedMessages("one|two",7,true,true,
      Model::CannedBroadcast,0);
  m.input(Input::Select);assert(m.cannedConfirm);
  m.input(Input::Select);PendingCannedSend out{};
  assert(m.takeCannedSend(out));assert(out.index==0&&out.destination==Model::CannedBroadcast&&out.channel==0);
}

static void testDestinationChannelFlow(){
  Model m;m.identity=0x100;m.page=Page::Canned;
  m.node(0x200,"Peer Long Name","PEER",1,-40,9,1);
  m.node(0,"Zero","ZERO",1,-40,9,1);
  m.node(Model::CannedBroadcast,"Broadcast alias","BCAST",1,-40,9,1);
  m.node(m.identity,"Self","SELF",1,-40,9,1);
  m.setCannedContent("first|second",12,true,true);
  m.setCannedRoutes((1u<<1)|(1u<<3),9);
  assert(m.canned.count()==2&&m.cannedViewCount()==3); // two messages plus Back
  char label[48];assert(m.cannedViewLabel(2,label,sizeof(label))&&strcmp(label,"[Back]")==0);
  assert(m.cannedPickerItemCount()==7);
  assert(m.cannedPickerItemLabel(m.canned.count(),label,sizeof(label))&&strcmp(label,"[Back]")==0);
  assert(m.cannedPickerItemLabel(m.canned.count()+1,label,sizeof(label))&&strcmp(label,"Broadcast")==0);
  assert(m.cannedPickerItemLabel(m.canned.count()+2,label,sizeof(label))&&strstr(label,"!00000200"));
  assert(m.cannedPickerItemLabel(m.canned.count()+3,label,sizeof(label))&&strcmp(label,"Channel 1")==0);
  assert(m.cannedPickerItemLabel(6,label,sizeof(label))&&strcmp(label,"Channel 3")==0);
  m.selected=1;m.input(Input::Select);
  assert(m.cannedSubstate==CannedSubstate::Destinations&&m.selected==1);
  assert(m.cannedDestinationCount()==2);
  assert(m.cannedViewLabel(0,label,sizeof(label))&&strcmp(label,"Broadcast")==0);
  assert(m.cannedViewLabel(1,label,sizeof(label))&&strstr(label,"!00000200"));
  m.gesture(Gesture::Previous);assert(m.cannedRouteSelected==2); // wrap to Back row
  m.gesture(Gesture::Next);assert(m.cannedRouteSelected==0);
  m.gesture(Gesture::Next);assert(m.cannedRouteSelected==1);
  m.gesture(Gesture::Hold);
  assert(m.cannedSubstate==CannedSubstate::Channels&&m.cannedDestination==0x200&&m.selected==1);
  assert(m.cannedChannelCount()==2&&m.cannedViewLabel(0,label,sizeof(label))&&strcmp(label,"Channel 1")==0);
  m.gesture(Gesture::Next);assert(m.cannedRouteSelected==1);
  m.input(Input::Select);
  assert(m.cannedSubstate==CannedSubstate::Messages&&m.cannedConfirm&&m.selected==1);
  assert(m.cannedRouteValid());
  m.input(Input::Select);PendingCannedSend out{};
  assert(m.takeCannedSend(out));assert(out.index==1&&out.destination==0x200&&out.channel==3);
}

static void testBackEmptyAndDisabled(){
  Model m;m.page=Page::Canned;m.setCannedContent("a",1,true,true);m.setCannedRoutes(0,1);
  m.input(Input::Select);assert(m.cannedSubstate==CannedSubstate::Destinations);
  m.input(Input::Select);assert(m.cannedSubstate==CannedSubstate::Channels&&m.cannedChannelCount()==0);
  m.input(Input::Select);assert(m.cannedSubstate==CannedSubstate::Destinations);
  m.input(Input::Back);assert(m.cannedSubstate==CannedSubstate::Messages&&!m.cannedConfirm);
  m.cannedReady=false;m.input(Input::Select);assert(m.cannedSubstate==CannedSubstate::Messages&&!m.cannedConfirm);
  PendingCannedSend out{};assert(!m.takeCannedSend(out));
}

static void testInvalidationPreservesMessageAndFailsClosed(){
  Model m;m.identity=1;m.page=Page::Canned;m.node(2,"Two","TWO",1,0,0,1);
  m.setCannedContent("a|b",3,true,true);m.setCannedRoutes((1u<<1)|(1u<<2),4);
  // Select peer 2, then channel 2, returning to the same message cursor.
  m.selected=1;m.input(Input::Select);m.input(Input::Right);m.input(Input::Select);
  m.input(Input::Right);m.input(Input::Select);
  assert(m.cannedConfirm&&m.cannedRouteValid()&&m.selected==1);
  const uint32_t oldGeneration=m.cannedGeneration;
  m.setCannedRoutes(1u<<1,5);
  assert(m.cannedGeneration!=oldGeneration&&!m.cannedConfirm&&!m.cannedPending);
  assert(m.selected==1&&m.cannedRouteNeedsSelection&&!m.cannedRouteValid());
  m.input(Input::Select);PendingCannedSend out{};assert(!m.takeCannedSend(out));

  // Reconfirm a valid route, then evict the selected peer from the bounded
  // 16-node cache. Route removal cancels the pending confirmation.
  m.setCannedRoutes((1u<<1)|(1u<<2),6);m.cannedRouteNeedsSelection=false;
  m.cannedDestination=2;m.cannedChannel=2;m.cannedConfirm=true;
  for(uint32_t id=10;id<26;++id)m.node(id,"Candidate","CAND",1,0,0,id);
  m.node(26,"Evict","EVICT",1,0,0,26);
  assert(!m.cannedConfirm&&!m.cannedRouteValid()&&m.cannedDestination==0);
  assert(m.selected==1);
}

static void testRouteSourceUpdatesKeepChosenRoute(){
  Model m;m.identity=1;m.page=Page::Canned;m.node(9,"Nine","NINE",1,0,0,0);
  m.setCannedContent("one|two",7,true,true);m.setCannedRoutes((1u<<0)|(1u<<4),1);
  m.cannedDestination=9;m.cannedChannel=4;m.cannedRouteNeedsSelection=false;m.selected=1;
  m.setCannedContent("new|content",11,true,true);
  assert(m.cannedDestination==9&&m.cannedChannel==4&&m.selected==0);
  m.selected=1;m.cannedConfirm=true;
  m.setCannedRoutes((1u<<0)|(1u<<4),1);
  assert(m.cannedDestination==9&&m.cannedChannel==4&&m.cannedConfirm&&m.selected==1);
  m.setCannedRoutes((1u<<0)|(1u<<4),2);
  assert(m.cannedDestination==9&&m.cannedChannel==4&&!m.cannedConfirm&&m.selected==1);
  m.page=Page::Canned;m.input(Input::Select);
  assert(m.cannedSubstate==CannedSubstate::Destinations&&m.cannedRouteSelected==1);
  m.cannedConfirm=true;m.cannedPending=true;
  m.setCannedContent("new|content",11,false,true);
  assert(!m.cannedConfirm&&!m.cannedPending&&!m.cannedReady);
  assert(m.cannedDestination==9&&m.cannedChannel==4);
}

static void testPickerHighlightTracksPeerIdentity(){
  Model m;m.identity=1;m.page=Page::Canned;
  m.node(10,"Ten","TEN",1,0,0,1);m.node(20,"Twenty","TWN",1,0,0,2);
  m.setCannedContent("message",7,true,true);m.setCannedRoutes(1u,1);
  m.input(Input::Select);assert(m.cannedSubstate==CannedSubstate::Destinations);
  // Candidate order is newest first; choose peer 10, then arrivals/recent
  // updates reorder the cache while the picker remains open.
  m.cannedRouteSelected=m.cannedDestinationIndex(10);
  uint32_t highlighted=0;assert(m.cannedDestinationAt(m.cannedRouteSelected,highlighted)&&highlighted==10);
  m.node(30,"Thirty","THR",1,0,0,3);
  assert(m.cannedDestinationAt(m.cannedRouteSelected,highlighted)&&highlighted==10);
  m.node(20,"Twenty updated","TWN",1,0,0,4);
  assert(m.cannedDestinationAt(m.cannedRouteSelected,highlighted)&&highlighted==10);
  m.node(10,"Ten updated","TEN",1,0,0,5);
  assert(m.cannedDestinationAt(m.cannedRouteSelected,highlighted)&&highlighted==10);
}

static void testDirectSendAdmissionFailsClosed(){
  PendingCannedSend out{};
  Model zeroMask;zeroMask.identity=1;zeroMask.page=Page::Canned;
  zeroMask.setCannedContent("x",1,true,true);zeroMask.setCannedRoutes(0,1);
  zeroMask.selected=0;zeroMask.cannedConfirm=true;zeroMask.requestCannedSend();
  assert(!zeroMask.takeCannedSend(out));

  Model stalePeer;stalePeer.identity=1;stalePeer.page=Page::Canned;
  stalePeer.node(2,"Peer","PEER",1,0,0,1);
  stalePeer.setCannedContent("x",1,true,true);stalePeer.setCannedRoutes(1u,1);
  stalePeer.cannedDestination=2;stalePeer.cannedChannel=0;stalePeer.cannedRouteNeedsSelection=false;
  stalePeer.nodes[0].id=3; // Simulate a stale externally supplied selected route.
  stalePeer.cannedConfirm=true;stalePeer.requestCannedSend();
  assert(!stalePeer.takeCannedSend(out));

  Model staleChannel;staleChannel.page=Page::Canned;
  staleChannel.setCannedContent("x",1,true,true);staleChannel.setCannedRoutes(1u,1);
  staleChannel.cannedDestination=Model::CannedBroadcast;staleChannel.cannedChannel=7;
  staleChannel.cannedRouteNeedsSelection=false;staleChannel.cannedConfirm=true;
  staleChannel.requestCannedSend();assert(!staleChannel.takeCannedSend(out));
}

int main(){
  testLegacyUnchanged();testDestinationChannelFlow();testBackEmptyAndDisabled();
  testInvalidationPreservesMessageAndFailsClosed();testRouteSourceUpdatesKeepChosenRoute();
  testPickerHighlightTracksPeerIdentity();testDirectSendAdmissionFailsClosed();
  printf("PASS canned route model: sizeof(Model)=%zu; picker, back, wrap, invalidation, legacy flow\n",sizeof(Model));
  return 0;
}
