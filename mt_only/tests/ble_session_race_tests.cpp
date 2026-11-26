// SPDX-License-Identifier: GPL-3.0-only
// Independent reviewer regression, retained without host-specific paths.
#include "../src/BleTransport.cpp"

#include <thread>
#include <future>
#include <iostream>
using namespace mt::ble;
using namespace std::chrono_literals;
int main() {
 mockSecurityChanged=callbacks.security_changed;
 assert(init(0x12345678)==0);
 for(bool dispatchCase:{false,true}) {
  bt_conn oldConnection,newConnection;callbacks.connected(&oldConnection,0);const auto oldSession=session();
  uint8_t response[]={0x94,0xc3,0,3,'O','L','D'},out[10]{};Packet packet{};
  assert(writeTo(&oldConnection,nullptr,response+4,3,0,0)==3&&receive(packet));
  std::promise<void> entered,release,changing,finished;auto releaseReady=release.get_future();auto finishReady=finished.get_future();
  mockBeforePut=[&](MockQueue *q){if(q==&fromQueue){entered.set_value();releaseReady.wait();}};
  struct Context {const uint8_t *p;uint32_t session;bool *accepted;};bool accepted=false;
  Context context{response,oldSession,&accepted};
  std::thread sender([&]{
   if(dispatchCase)assert(dispatch(packet,[](const Packet &p,void *v){auto &c=*static_cast<Context*>(v);assert(p.session==c.session);*c.accepted=send(c.p,7,c.session);},&context));
   else accepted=send(response,sizeof(response),oldSession);
  });
  entered.get_future().wait();
  std::thread lifecycle([&]{changing.set_value();callbacks.disconnected(&oldConnection,0);callbacks.connected(&newConnection,0);finished.set_value();});
  changing.get_future().wait();assert(finishReady.wait_for(30ms)==std::future_status::timeout);
  release.set_value();sender.join();lifecycle.join();mockBeforePut={};
  assert(accepted&&session()!=oldSession);
  assert(readFrom(&newConnection,nullptr,out,sizeof(out),0)==0);
  assert(!send(response,sizeof(response),oldSession));
  unsigned calls=0;assert(!dispatch(packet,[](const Packet &,void *v){++*static_cast<unsigned*>(v);},&calls)&&!calls);
  assert(readFrom(&newConnection,nullptr,out,1,1)==BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET));
  callbacks.disconnected(&newConnection,0);
  std::cout<<"PASS real-thread BLE "<<(dispatchCase?"dispatch and recursive response":"send")<<" serializes reconnect; queued output purged and stale tokens rejected\n";
 }
}
