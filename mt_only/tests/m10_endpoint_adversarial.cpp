// SPDX-License-Identifier: GPL-3.0-only
// Independent endpoint isolation tests; no radio/provider, synthetic ciphertext.
#include "Endpoint.h"
#include <cassert>
#include <iostream>
#include <memory>
using E=mt::Endpoint;
constexpr uint32_t Local=0x12345678,Peer=0x87654321,Id=0x44556677;
static unsigned failures=0;
void check(bool ok,const char *name){failures+=!ok;std::cout<<(ok?"PASS ":"FAIL ")<<name<<'\n';}
void put(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=uint8_t(n>>(i*8));}
bench::Frame frame(uint32_t from,uint32_t to,uint32_t id){bench::Frame f;f.len=30;put(f.bytes,to);put(f.bytes+4,from);put(f.bytes+8,id);f.bytes[12]=0x6b;f.bytes[15]=uint8_t(from);for(unsigned i=16;i<f.len;++i)f.bytes[i]=uint8_t(i*19);return f;}
E::Job *submit(E &ep,uint8_t owner=0){auto f=frame(Local,Peer,Id);assert(!ep.submit(owner,f.bytes,f.len,true,100,false,true,false,nullptr,true));auto *j=ep.next(100);assert(j&&j->privateMessage);return j;}
int main(){
  {
    auto ep=std::make_unique<E>();auto *j=submit(*ep);ep->started(*j,0,200,100,0);ep->authenticated(Id,Peer,0);
    assert(j->terminal&&j->state==E::State::Active);ep->cancelPrivate();
    assert(j->state==E::State::Active);ep->complete(*j,300,0);E::Result result;
    check(ep->take(0,result)&&result.kind==E::Kind::Failed&&result.code!=0&&!result.peer,
      "revocation_after_early_valid_ack_overrides_terminal_success_without_freeing_active_slot");
  }
  {
    auto ep=std::make_unique<E>();auto *j=submit(*ep);ep->started(*j,0,200,100,0);ep->complete(*j,300,0);
    bench::MtCodec codec(nullptr);bench::RoutingCore routing;auto inbound=frame(Peer,Local,10);bench::RouteMetadata m;
    m.portnum=5;m.routingPresent=m.routingErrorPresent=true;m.request_id=Id;
    for(unsigned error:{0u,5u,32u,39u}){m.routingError=error;ep->receive(codec,routing,Local,inbound.bytes,inbound.len,m,0,500,1,false);assert(ep->pendingPrivate(Id,Peer,true));}
    m.portnum=4;m.nodeInfoValid=true;ep->receive(codec,routing,Local,inbound.bytes,inbound.len,m,0,500,1,false);assert(ep->pendingPrivate(Id,Peer,true));
    m.portnum=1;ep->receive(codec,routing,Local,inbound.bytes,inbound.len,m,0,500,1,false);assert(ep->pendingPrivate(Id,Peer,true));
    auto echo=j->frame;echo.bytes[12]-=1;echo.bytes[15]=uint8_t(Peer);ep->receive(codec,routing,Local,echo.bytes,echo.len,m,0,500,1,false);
    E::Result result;check(ep->pendingPrivate(Id,Peer,true)&&!ep->take(0,result),"public_ack_nak_nodeinfo_app_response_and_own_echo_do_not_complete_private_job");
  }
  {
    auto ep=std::make_unique<E>();auto *j=submit(*ep);auto exact=j->frame;E::Result result;
    ep->authenticated(Id,Peer,0);assert(!ep->take(0,result)&&!j->terminal); // Before any actual send.
    ep->started(*j,0,200,100,0);ep->authenticated(Id+1,Peer,0);ep->authenticated(Id,Peer+1,0);assert(!j->terminal);
    ep->complete(*j,300,0);ep->tick(15601,42);j=ep->next(17000);assert(j&&j->attempts==1&&!memcmp(&exact,&j->frame,sizeof(exact)));
    ep->started(*j,0,17000,100,42);ep->authenticated(Id,Peer,35);assert(j->terminal&&!ep->take(0,result));ep->complete(*j,17200,42);
    check(ep->take(0,result)&&result.kind==E::Kind::Authenticated&&result.code==35&&result.peer==Peer&&result.attempts==2,
      "native_completion_barrier_exact_retry_and_authenticated_nak_code");
  }
  {
    auto ep=std::make_unique<E>();auto *j=submit(*ep);ep->started(*j,0,200,100,0);ep->authenticated(Id,Peer,0);ep->complete(*j,300,0);
    ep->cancelPrivate();E::Result result;assert(ep->take(0,result));
    auto f=frame(Local,Peer,Id);auto code=ep->submit(0,f.bytes,f.len,true,500,false,true,false,nullptr,true);
    check(result.kind==E::Kind::Failed&&code==-EALREADY,"revocation_rewrites_queued_success_and_does_not_restore_old_nonce_from_history");
  }
  {
    auto ep=std::make_unique<E>();auto *j=submit(*ep);auto f=j->frame;
    assert(!ep->submit(1,f.bytes,f.len,true,150,false,true,false,nullptr,true));ep->started(*j,0,200,100,0);
    ep->disconnect(0);ep->authenticated(Id,Peer,0);ep->complete(*j,300,0);E::Result result;
    check(!ep->take(0,result)&&ep->take(1,result)&&result.kind==E::Kind::Authenticated,"two_client_ownership_disconnect_isolated");
  }
  return failures?1:0;
}
