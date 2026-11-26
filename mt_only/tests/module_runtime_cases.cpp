// SPDX-License-Identifier: GPL-3.0-only
// Actual Service, PSA, nonce journal and control queue. Synthetic keys only.
#include "pki_runtime_fixture.h"
#include "TelemetryAnnouncement.h"
static Bytes metrics(){uint8_t bytes[128]{};size_t size=0;assert(mt::TelemetryAnnouncement::encode(123,{}, {},bytes,sizeof(bytes),size));return {bytes,bytes+size};}
static void custom(Fixture&f,size_t width){
  if(!width)return;
  f.s.stop();auto cfg=f.s.config;auto record=cfg.node();uint8_t key[32]{};key[0]=0x51;
  mt::PrimaryChannel::Selection selected;assert(mt::PrimaryChannel::normalize(key,width,selected));
  record.setKey(selected.hash,selected.bytes,selected.length);mt::PrimaryChannel::clear(selected);cfg.node(record);
  assert(f.s.apply(cfg)&&f.s.start(false,0)&&f.s.armEndpoint(true));
}
int main(){
  for(size_t width:{size_t(0),size_t(16),size_t(32)}){
    Fixture f(true);custom(f,width);const auto data=metrics();uint32_t id=0;
    assert(!f.s.submitModuleAnnouncement(data.data(),data.size(),3,100,991,id)&&id&&f.ledger.sent(id)&&f.signing.calls==1&&f.entropy.calls==1);
    assert(f.s.endpointFree()==4&&f.s.receiving()&&!f.radio.sends);f.poll(1000);assert(f.radio.frames.size()==1);
    const auto&frame=f.radio.frames[0];bench::RouteMetadata m;
    assert(f.s.codec.decode(frame.data(),frame.size(),m)&&m.portnum==67&&m.signatureLength==64&&!m.wantResponse&&!m.request_id&&
      bench::le32(frame.data())==UINT32_MAX&&!(frame[12]&8)&&(frame[12]&7)==3&&(frame[12]>>5)==3);
    bench::MtSignature stockGate;assert(stockGate.trustPublicKey(f.local.binding.node,f.local.binding.publicKey,1000));
    assert(stockGate.apply(frame.data(),frame.size(),m,1000)==bench::MtSignature::Result::Verified);
    f.complete(1200);f.poll(100000);assert(f.radio.frames.size()==1&&f.usb.out.empty()&&f.ble.out.empty());
  }
  std::cout<<"PASS signed module DeviceMetrics uses one durable ID and no ACK retry or client result on all key widths\n";
  for(unsigned port:{36u,66u,71u}){
    Fixture f(true);uint8_t bytes[128]{};size_t size=0;const uint8_t payload[]={8,1};
    assert(mt::PublicApplication::encode(port,payload,sizeof(payload),0,0,bytes,sizeof(bytes),size));uint32_t id=0;
    assert(!f.s.submitModuleAnnouncement(bytes,size,0,100,992,id)&&id&&f.ledger.sent(id)&&f.entropy.calls==1&&f.signing.calls==1&&!f.backend.encrypts);
    f.poll(1000);assert(f.radio.frames.size()==1);const auto&frame=f.radio.frames[0];bench::RouteMetadata m;
    assert(f.s.codec.decode(frame.data(),frame.size(),m)&&m.portnum==port&&m.payloadLength==2&&m.payload[0]==8&&m.payload[1]==1&&m.signatureLength==64&&
      !m.wantResponse&&!m.request_id&&!m.reply_id&&!m.emoji&&!(frame[12]&8)&&!frame[12]);
    bench::MtSignature verifier;assert(verifier.trustPublicKey(f.local.binding.node,f.local.binding.publicKey,1000));
    assert(verifier.apply(frame.data(),frame.size(),m,1000)==bench::MtSignature::Result::Verified);
    f.complete(1200);assert(f.radio.frames.size()==1&&f.signing.calls==1&&f.entropy.calls==1);
  }
  std::cout<<"PASS signed status range and neighbor module broadcasts use one durable ID and verify against local identity\n";
  for(unsigned fault=0;fault<4;++fault){
    Fixture f(true);const auto data=metrics();
    if(fault==0)f.radio.busyValue=true;
    if(fault==1)f.entropy.fail=true;
    if(fault==2)f.signing.fail=true;
    if(fault==3)f.radio.restoreError=-EIO;
    uint32_t id=9;assert(f.s.submitModuleAnnouncement(data.data(),data.size(),0,100,993,id)<0&&!id&&f.s.endpointFree()==4&&!f.radio.sends);
    f.poll(1000);assert(f.radio.frames.empty());
  }
  std::cout<<"PASS busy nonce entropy signing and RX restoration failures never use unsigned fallback\n";
  {Fixture f(true);const auto data=metrics();uint32_t id=0;assert(!f.s.submitModuleAnnouncement(data.data(),data.size(),0,100,994,id));
    f.runtime.freeze();f.poll(1000);assert(f.radio.frames.empty()&&f.usb.out.empty()&&f.ble.out.empty());}
  std::cout<<"PASS provider revocation cancels pending module credentials without client completion claims\n";
}
