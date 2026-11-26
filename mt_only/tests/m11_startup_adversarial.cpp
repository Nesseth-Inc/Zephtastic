// SPDX-License-Identifier: GPL-3.0-only
// Independent assertions using the selected integration's synthetic boot fixture.
#include "startup_review_fixture.h"
static void pass(const char *s){std::cout<<"PASS "<<s<<'\n';}
int main(){
 for(unsigned mode:{2u,3u,4u,5u,10u}){BootFixture f(mode);uint32_t node=0x12345678;assert(!f.startup.prepare(node)&&node==0x12345678);assert(!f.startup.hold(f.service));const auto before=f.memory.values;assert(!f.startup.prepare(node));assert(!f.startup.activate(f.service,true));assert(f.service.apply(f.service.config));assert(!f.service.start(false,0)&&!f.service.armEndpoint(true)&&f.service.faulted());assert(f.memory.values==before&&!f.entropy.calls&&!f.signing.calls&&!f.radio.sends);}
 pass("rejected-stored-state-preserves-caller-id-and-terminal-hold-survives-config-apply");
 for(unsigned mode=0;mode<2;++mode){BootFixture f;assert(f.prepare());if(mode==0)f.radio.healthValue=1;if(mode==1)f.radio.busyValue=true;assert(!f.startup.activate(f.service,true));assert(f.service.faulted()&&!f.service.privateConfigured()&&!f.storage.runtimeIdentity());assert(!f.radio.sends&&f.memory.writes==f.initialWrites);}
 pass("native-rf-health-and-busy-still-refuse-private-attachment");
 {BootFixture f;assert(f.prepare());assert(f.service.nodes.edit(f.peer.binding.node,f.local.binding.node,mt::NodeCache::Edit::Ignore)==1);
  const auto before=f.memory.values;assert(f.startup.activate(f.service,true)&&f.startup.state()==PkiStartup::State::Active);
  assert(f.service.privateConfigured()&&!f.service.privateDormant()&&!f.service.faulted()&&f.storage.runtimeIdentity());
  assert(f.service.start(false,0)&&f.service.armEndpoint(true));const auto plain=data("ignored peer");
  assert(f.service.submitPrivate(0,f.peer.binding.node,900,plain.data(),plain.size(),0,false,nullptr,0,true,100)==-EPERM);
  assert(f.service.receiving()&&f.service.endpointArmed()&&f.service.endpointFree()==4&&
    f.memory.values==before&&!f.entropy.calls&&!f.signing.calls&&!f.backend.encrypts&&!f.radio.sends);}
 pass("ignored boot pin keeps active domain but only selected destination is blocked without nonce or writes");
 for(unsigned mode:{6u,8u}){BootFixture f(mode);const auto before=f.memory.values;assert(f.prepare()&&f.startup.activate(f.service,true));
  assert(f.startup.state()==PkiStartup::State::Dormant&&f.service.privateDormant()&&!f.service.faulted()&&f.storage.runtimeIdentity());
  assert(f.service.apply(f.service.config)&&f.service.start(false,0)&&f.service.armEndpoint(true));const auto plain=data("no peer");
  assert(f.service.submitPrivate(0,f.peer.binding.node,903,plain.data(),plain.size(),0,false,nullptr,0,true,100)<0);
  assert(f.service.privateDormant()&&f.service.receiving()&&f.service.endpointArmed()&&!f.service.faulted()&&
    f.memory.values==before&&!f.entropy.calls&&!f.signing.calls&&!f.backend.encrypts&&
    f.radio.starts==1&&!f.radio.sends);}
 pass("valid zero-active domains allow public readiness while private submission stays closed");
 {BootFixture f;assert(f.prepare()&&f.startup.activate(f.service,true));assert(f.service.start(false,0)&&f.service.armEndpoint(true));auto plain=data("cached");assert(!f.service.submitPrivate(0,f.peer.binding.node,901,plain.data(),plain.size(),0,true,nullptr,0,false,100));const auto writes=f.memory.writes,calls=f.entropy.calls,encrypts=f.backend.encrypts;f.entropy.fail=true;f.signing.fail=true;assert(!f.service.submitPrivate(0,f.peer.binding.node,901,plain.data(),plain.size(),0,true,nullptr,0,false,101));assert(f.memory.writes==writes&&f.entropy.calls==calls&&f.backend.encrypts==encrypts&&!f.service.faulted());f.service.poll(1000,17);assert(f.radio.sends==1);const auto first=f.radio.frames.front();f.radio.done=true;f.service.poll(1200,19);for(uint32_t t=1500;t<=23000&&f.radio.sends<2;t+=500)f.service.poll(t,31);assert(f.radio.sends==2&&f.radio.frames.back()==first&&f.entropy.calls==calls&&f.backend.encrypts==encrypts);}
 pass("post-startup-entropy-starvation-preserves-exact-cached-ciphertext-retry");
 {BootFixture f;assert(f.prepare()&&f.startup.activate(f.service,true)&&f.service.start(false,0));f.entropy.fail=true;f.signing.fail=true;auto plain=data("receive without entropy");uint8_t encrypted[239];size_t n=0;assert(f.crypto.encrypt(f.peer.scalar,f.local.binding.publicKey,f.peer.binding.node,902,1234,plain.data(),plain.size(),encrypted,sizeof(encrypted),n));Bytes frame(n+16);put(frame.data(),f.local.binding.node);put(frame.data()+4,f.peer.binding.node);put(frame.data()+8,902);frame[15]=uint8_t(f.peer.binding.node);memcpy(frame.data()+16,encrypted,n);const auto writes=f.memory.writes;f.service.rx(frame.data(),frame.size(),1,1000,41);PrivateEvent event;assert(f.service.takePrivateEvent(0,frame.data(),frame.size(),event)&&event.size==plain.size()&&!memcmp(event.data,plain.data(),plain.size()));assert(f.memory.writes==writes+1&&!f.entropy.calls&&!f.signing.calls&&!f.service.faulted()&&!f.radio.sends&&!f.service.endpointArmed());f.service.rx(frame.data(),frame.size(),1,1100,43);assert(!f.service.takePrivateEvent(0,frame.data(),frame.size(),event)&&f.memory.writes==writes+1);}
 pass("authenticated-receive-durable-replay-and-disarmed-operation-need-no-fresh-entropy");
}
