// SPDX-License-Identifier: GPL-3.0-only
// Real PSA with deterministic synthetic keys, memory journals and mock radios.
#include "pki_runtime_fixture.h"
#include "pki/PrivateOpaqueApplication.h"
namespace {
void group(const char *name){std::cout<<"PASS "<<name<<'\n';}
meshtastic_Data application(unsigned port=8,unsigned consent=0){
  meshtastic_Data d{};d.portnum=static_cast<meshtastic_PortNum>(port);d.has_bitfield=true;d.bitfield=consent;
  d.reply_id=23;d.emoji=0x1f44d;d.payload.size=4;const uint8_t payload[]={0,0xff,0x80,1};memcpy(d.payload.bytes,payload,4);return d;
}
Bytes encode(const meshtastic_Data &d){uint8_t raw[239]{};auto out=pb_ostream_from_buffer(raw,sizeof(raw));
  assert(pb_encode(&out,meshtastic_Data_fields,&d));return {raw,raw+out.bytes_written};}
meshtastic_ToRadio request(Fixture &f,unsigned port=8,uint32_t id=201){auto r=f.request(id);auto &p=r.packet;
  p.from=0;p.hop_limit=0;p.decoded=application(port);return r;}
bool allowed(const meshtastic_Data &d){const auto raw=encode(d);return mt::pki::PrivateOpaqueApplication::allowed(raw.data(),raw.size());}
Bytes incoming(Fixture &f,const meshtastic_Data &d,uint32_t id=901,const Key *sender=nullptr){
  const auto &peer=sender?*sender:f.peer;const auto plain=encode(d);uint8_t encrypted[239]{};size_t size=0;
  assert(f.crypto.encrypt(peer.scalar,f.local.binding.publicKey,peer.binding.node,id,0x12345678,plain.data(),plain.size(),encrypted,sizeof(encrypted),size));
  Bytes frame(size+16);put(frame.data(),f.local.binding.node);put(frame.data()+4,peer.binding.node);put(frame.data()+8,id);
  frame[15]=uint8_t(peer.binding.node);memcpy(frame.data()+16,encrypted,size);return frame;
}
void delivered(const Fixture &f,const Host &host,const meshtastic_Data &d,uint32_t id){unsigned count=0;
  for(const auto &event:host.out)if(event.which_payload_variant==meshtastic_FromRadio_packet_tag&&event.packet.decoded.portnum==d.portnum){
    const auto &p=event.packet;assert(p.id==id&&p.from==f.peer.binding.node&&p.to==f.local.binding.node&&p.pki_encrypted&&!p.xeddsa_signed&&
      p.public_key.size==32&&!memcmp(p.public_key.bytes,f.peer.binding.publicKey,32)&&encode(p.decoded)==encode(d));++count;
  }
  assert(count==1);
}
unsigned proofs(const Host &host,uint32_t id){unsigned count=0;for(const auto &out:host.out){
  if(out.which_payload_variant==meshtastic_FromRadio_packet_tag&&out.packet.decoded.portnum==5&&out.packet.decoded.request_id==id&&
    out.packet.ack_proof_status==meshtastic_MeshPacket_AckProofStatus_ACK_PROOF_VALID)++count;}
  return count;
}
}
int main(){
  for(unsigned port=0;port<512;++port)for(unsigned consent:{0u,1u}){
    bool expected=port>=256&&port<=511;
    for(unsigned known:{2u,7u,8u,9u,10u,11u,32u,33u,34u,35u,36u,37u,38u,64u,65u,66u,68u,69u,71u,72u,73u,75u,76u,77u,78u,112u})expected|=port==known;
    assert(allowed(application(port,consent))==expected);
  }
  {auto d=application();d.reply_id=d.emoji=0;d.payload.size=220;auto raw=encode(d);assert(raw.size()==227&&allowed(d));
    d.payload.size=221;raw=encode(d);assert(raw.size()==228&&!allowed(d));
    assert(!mt::pki::PrivateOpaqueApplication::allowed(nullptr,0)&&!mt::pki::PrivateOpaqueApplication::allowed(nullptr,1));
    const uint8_t unknownPort[]={8,128,4,72,0};assert(!mt::pki::PrivateOpaqueApplication::allowed(unknownPort,sizeof(unknownPort)));
  }
  for(unsigned mutation=0;mutation<12;++mutation){auto d=application();Bytes raw;
    switch(mutation){case 0:d.has_bitfield=false;break;case 1:d.bitfield=2;break;case 2:d.want_response=true;break;
      case 3:d.request_id=9;break;case 4:d.dest=7;break;case 5:d.source=8;break;case 6:d.xeddsa_signature.size=64;break;default:break;}
    raw=encode(d);
    if(mutation==7)raw.insert(raw.end(),{72,0});
    if(mutation==8)raw.insert(raw.end(),{0x98,6,1});
    if(mutation==9){raw[1]|=0x80;raw.insert(raw.begin()+2,0);}
    if(mutation==10){const uint8_t first[]={raw[0],raw[1]};raw.erase(raw.begin(),raw.begin()+2);raw.insert(raw.end(),first,first+2);}
    if(mutation==11)raw.pop_back();
    assert(!mt::pki::PrivateOpaqueApplication::allowed(raw.data(),raw.size()));
  }
  group("private opaque port policy exact canonical metadata and 227-byte boundary preserve binary payload reply emoji and MQTT consent");
  for(unsigned port:{8u,36u,511u})for(unsigned consent:{0u,1u}){Fixture f;auto r=request(f,port);r.packet.decoded.bitfield=consent;
    input(f.c,r,100);input(f.bc,r,101);assert(f.entropy.calls==1&&f.backend.encrypts==1&&f.ledger.sent(201)&&f.s.endpointFree()==3);
    f.poll(1000);assert(f.radio.frames.size()==1);const auto first=f.radio.frames[0];uint8_t plain[227]{};size_t size=0;
    assert(!first[13]&&bench::le32(first.data())==f.peer.binding.node&&bench::le32(first.data()+4)==f.local.binding.node&&
      f.crypto.decrypt(f.peer.scalar,f.local.binding.publicKey,f.local.binding.node,201,first.data()+16,first.size()-16,plain,sizeof(plain),size));
    const auto expected=encode(r.packet.decoded);assert(size==expected.size()&&!memcmp(plain,expected.data(),size));
    auto changed=r;changed.packet.decoded.payload.bytes[0]^=1;input(f.c,changed,1100);assert(f.entropy.calls==1&&f.backend.encrypts==1);
    f.complete(1200);f.poll(20000);f.poll(22000);assert(f.radio.frames.size()==2&&f.radio.frames[1]==first&&f.entropy.calls==1&&f.backend.encrypts==1);
    f.complete(23000);f.rx(f.ack(201,false),23500);assert(!proofs(f.usb,201)&&!proofs(f.ble,201)&&f.s.endpointFree()==3);
    f.rx(f.ack(201),24000);f.poll(24001);assert(f.result(201,true,0)==1&&proofs(f.usb,201)==1&&proofs(f.ble,201)==1&&f.s.endpointFree()==4);
  }
  group("implicit private generic USB BLE owner merge decrypts exactly at peer and retries unchanged ciphertext until bound proof ACK");
  for(unsigned mode=0;mode<13;++mode){Fixture f;auto r=request(f);const auto writes=f.lm.writes;
    switch(mode){case 0:r.packet.to^=0x100;break;case 1:r.packet.pki_encrypted=true;r.packet.public_key.size=32;memset(r.packet.public_key.bytes,1,32);break;
      case 2:r.packet.to=UINT32_MAX;r.packet.pki_encrypted=true;break;case 3:r.packet.to=UINT32_MAX;r.packet.pki_encrypted=true;r.packet.public_key.size=32;break;
      case 4:r.packet.to=f.local.binding.node;r.packet.pki_encrypted=true;break;case 5:r.packet.ack_proof_status=meshtastic_MeshPacket_AckProofStatus_ACK_PROOF_VALID;break;
      case 6:r.packet.xeddsa_signed=true;break;case 7:r.packet.decoded.want_response=true;break;case 8:r.packet.decoded.request_id=7;break;
      case 9:r.packet.decoded.xeddsa_signature.size=64;break;case 10:f.runtime.freeze();break;
      case 11:r.packet.decoded.payload.size=221;r.packet.decoded.reply_id=r.packet.decoded.emoji=0;break;
      case 12:r.packet.pki_encrypted=true;r.packet.public_key.size=31;break;
}
    input(f.c,r,100);f.poll(1000);assert(!f.entropy.calls&&!f.backend.encrypts&&!f.ledger.sent(201)&&f.lm.writes==writes&&f.radio.frames.empty()&&f.s.endpointFree()==4);
  }
  for(unsigned mode=0;mode<5;++mode){Fixture f;auto r=request(f);
    if(mode<2){r.packet.pki_encrypted=true;if(mode==1){r.packet.public_key.size=32;memcpy(r.packet.public_key.bytes,f.peer.binding.publicKey,32);}}
    else {r.packet.pki_encrypted=false;r.packet.public_key.size=mode==2?32:(mode==3?31:0);
      if(mode==2)memset(r.packet.public_key.bytes,0xa5,32);else if(mode==3)memset(r.packet.public_key.bytes,0x5a,31);}
    input(f.c,r,100);input(f.bc,r,101);
    assert(f.entropy.calls==1&&f.backend.encrypts==1&&f.ledger.sent(201)&&f.s.endpointFree()==3);
    f.poll(1000);assert(f.radio.frames.size()==1);const auto &wire=f.radio.frames[0];uint8_t plain[227]{};size_t size=0;
    assert(!wire[13]&&bench::le32(wire.data())==f.peer.binding.node&&bench::le32(wire.data()+4)==f.local.binding.node&&
      f.crypto.decrypt(f.peer.scalar,f.local.binding.publicKey,f.local.binding.node,201,wire.data()+16,wire.size()-16,plain,sizeof(plain),size));
    const auto expected=encode(r.packet.decoded);assert(size==expected.size()&&!memcmp(plain,expected.data(),size));
    f.complete(1200);f.rx(f.ack(201),1300);f.poll(1301);
    assert(f.result(201,true,0)==1&&proofs(f.usb,201)==1&&proofs(f.ble,201)==1&&f.s.endpointFree()==4);
  }
  group("explicit PKI key mismatches and malformed hints refuse; absent/matching explicit keys and ignored hint-only keys use pinned private transport to both owners");
  for(unsigned mode=0;mode<2;++mode){Fixture f;auto r=request(f);if(mode)f.entropy.fail=true;else f.radio.busyValue=true;
    input(f.c,r,100);f.poll(1000);assert(f.radio.frames.empty()&&f.s.endpointFree()==4);
    if(mode)assert(f.entropy.calls==1&&f.ledger.sent(201));else assert(!f.entropy.calls&&!f.ledger.sent(201));
  }
  group("busy and nonce reservation failure cannot publish unsigned generic fallback");
  {Fixture f(true);auto r=request(f);r.packet.to=UINT32_MAX;input(f.c,r,100);f.poll(1000);
    assert(f.entropy.calls==1&&f.signing.calls==1&&f.ledger.sent(201)&&!f.backend.encrypts&&f.radio.frames.size()==1);
    const auto &frame=f.radio.frames[0];bench::RouteMetadata m;
    assert(frame[13]==f.s.codec.channelHash()&&!(frame[12]&8)&&f.s.codec.decode(frame.data(),frame.size(),m)&&
      m.portnum==8&&m.hasBitfield&&!m.bitfield&&!m.wantResponse&&!m.request_id&&m.signatureLength==64&&
      m.reply_id==23&&m.emoji==0x1f44d);
    bench::MtSignature verifier;assert(verifier.trustPublicKey(f.local.binding.node,f.local.binding.publicKey,1000));
    assert(verifier.apply(frame.data(),frame.size(),m,1000)==bench::MtSignature::Result::Verified);
  }
  for(bool ble:{false,true}){Fixture f;auto r=request(f);r.packet.to=f.local.binding.node;input(ble?f.bc:f.c,r,100);f.poll(1000);unsigned count=0;
    for(const auto &out:(ble?f.ble:f.usb).out)if(out.which_payload_variant==meshtastic_FromRadio_packet_tag&&out.packet.decoded.portnum==8){++count;
      assert(out.packet.from==f.local.binding.node&&out.packet.to==f.local.binding.node&&!out.packet.pki_encrypted&&!out.packet.public_key.size&&encode(out.packet.decoded)==encode(r.packet.decoded));}
    assert(count==1&&!f.entropy.calls&&!f.backend.encrypts&&f.radio.frames.empty()&&f.s.endpointFree()==4&&(ble?f.usb:f.ble).out.empty());
  }
  group("broadcast stays unsigned Public while ordinary self-addressed application is requesting-owner local delivery with zero RF or nonce");
  for(unsigned port:{8u,36u,511u})for(unsigned consent:{0u,1u}){Fixture f;const auto d=application(port,consent);const auto frame=incoming(f,d);
    auto tampered=frame;tampered.back()^=1;const auto writes=f.lm.writes;f.rx(tampered,1000);
    assert(!f.delivered(f.usb)&&!f.delivered(f.ble)&&f.lm.writes==writes);
    f.rx(frame,1100);delivered(f,f.usb,d,901);delivered(f,f.ble,d,901);const auto committed=f.lm.writes;
    f.rx(frame,1200);f.c.receive(frame.data(),frame.size(),-80,3,1200,mt::Companion::WorkspaceProof{true});f.poll(2000);
    delivered(f,f.usb,d,901);delivered(f,f.ble,d,901);assert(f.lm.writes==committed&&!f.entropy.calls&&f.radio.frames.empty());
  }
  group("authenticated canonical generic private ingress reaches each owner once with truthful PKI key and duplicate replay cannot redeliver or reserve ACK without wantACK");
  for(unsigned mode=0;mode<5;++mode){Fixture f;auto d=application();if(mode==3)d.want_response=true;if(mode==4)d.dest=7;
    Key unknown(193);auto frame=incoming(f,d,901,mode==0?&unknown:nullptr);
    if(mode==1)put(frame.data(),UINT32_MAX);
    if(mode==2)frame[13]=f.s.codec.channelHash();
    const auto writes=f.lm.writes;f.rx(frame,1000);f.poll(2000);
    assert(!f.delivered(f.usb)&&!f.delivered(f.ble)&&f.lm.writes==writes&&!f.entropy.calls&&f.radio.frames.empty());
  }
  group("valid unknown-peer ciphertext wrong destination channel provenance and authenticated forbidden Data are refused before durable replay or output");
  for(bool cachedPublic:{false,true}){Fixture f;mt::NodeCache::Entry e{};e.identity(f.peer.binding.node,81,0);
    assert(f.s.nodes.accept(e,f.local.binding.node,0,0)>=0);const int slot=f.s.nodes.find(f.peer.binding.node);assert(slot>=0);
    meshtastic_DeviceMetrics metrics{};metrics.has_uptime_seconds=true;metrics.uptime_seconds=321;mt::PeerDeviceMetrics value;
    assert(value.assign(metrics)&&f.s.nodes.observeDeviceMetrics(f.peer.binding.node,value));
    if(cachedPublic){char text[80]="public";mt::PeerStatus status;assert(status.assign(text)&&f.s.nodes.observeStatus(f.peer.binding.node,status));}
    const auto cfg=f.s.config;const auto before=*f.s.nodes.get(unsigned(slot));auto d=application(36);d.payload.size=9;memcpy(d.payload.bytes,"\x0a\x07private",9);
    const auto frame=incoming(f,d);f.rx(frame,1000);delivered(f,f.usb,d,901);delivered(f,f.ble,d,901);
    const auto &after=*f.s.nodes.get(unsigned(slot));assert(f.s.config.same(cfg)&&!memcmp(before.bytes,after.bytes,sizeof(before.bytes))&&
      !memcmp(&before.deviceMetrics,&after.deviceMetrics,sizeof(value))&&after.status.present()==cachedPublic&&!strcmp(before.status.text(),after.status.text()));
    for(bool ble:{false,true}){auto &host=ble?f.ble:f.usb;auto &companion=ble?f.bc:f.c;host.out.clear();meshtastic_ToRadio config{};
      config.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;config.want_config_id=301;input(companion,config,1200);companion.poll(mt::Companion::WorkspaceProof{true});unsigned replays=0,complete=0;
      for(const auto &out:host.out){if(out.which_payload_variant==meshtastic_FromRadio_config_complete_id_tag)++complete;
        if(out.which_payload_variant==meshtastic_FromRadio_packet_tag&&out.packet.decoded.portnum==36){++replays;assert(complete==1&&!out.packet.pki_encrypted&&!out.packet.public_key.size&&!out.packet.xeddsa_signed&&
          out.packet.decoded.payload.size==8&&!memcmp(out.packet.decoded.payload.bytes,"\x0a\x06public",8));}}
      assert(complete==1&&replays==unsigned(cachedPublic));
    }
  }
  group("private Status cannot populate or overwrite shared Public status replay or DeviceMetrics cache and local config remains unchanged");
}
