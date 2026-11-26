// SPDX-License-Identifier: GPL-3.0-only
// Opaque transport fixtures. No application module, radio or hardware action.
#include "endpoint_fixture.h"
namespace {
using Bytes=std::vector<uint8_t>;
meshtastic_ToRadio request(unsigned port=8,uint32_t id=201,uint32_t to=PEER,bool ack=false){
  meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_packet_tag;auto &p=r.packet;
  p.id=id;p.to=to;p.want_ack=ack;p.hop_limit=1;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
  p.decoded.portnum=static_cast<meshtastic_PortNum>(port);p.decoded.payload.size=4;
  p.decoded.payload.bytes[0]=0;p.decoded.payload.bytes[1]=0xff;p.decoded.payload.bytes[2]=0x80;p.decoded.payload.bytes[3]=1;
  p.decoded.reply_id=123;p.decoded.emoji=0x1f44d;return r;
}
Bytes encoded(const meshtastic_Data &data){uint8_t bytes[239]{};auto out=pb_ostream_from_buffer(bytes,sizeof(bytes));assert(pb_encode(&out,meshtastic_Data_fields,&data));return {bytes,bytes+out.bytes_written};}
Bytes incoming(Fixture &f,unsigned port=8,uint32_t id=701,uint32_t to=OURS,bool ack=false,bool invalid=false,uint8_t bitfield=0){
  const auto r=request(port,id,to);auto d=r.packet.decoded;d.has_bitfield=true;d.bitfield=bitfield;
  auto bytes=encoded(d);if(invalid)bytes.insert(bytes.end(),{0xa0,1,1});
  uint8_t frame[255]{};size_t n=0;assert(f.s.codec.encodeZeroHopData(PEER,to,id,bytes.data(),bytes.size(),frame,sizeof(frame),n));
  frame[12]=ack?8:0;return {frame,frame+n};
}
unsigned applications(const Host &host,unsigned port){
  unsigned n=0;
  for(const auto &f:host.out){
    if(f.which_payload_variant==meshtastic_FromRadio_packet_tag&&unsigned(f.packet.decoded.portnum)==port)++n;
  }
  return n;
}
}
int main(){
  for(bool consent:{false,true})for(unsigned port=0;port<=600;++port){const bool expected=(port>=256&&port<=511)||
    port==2||port==7||port==8||port==9||port==10||port==11||(port>=32&&port<=38)||
    (port>=64&&port<=66)||port==68||port==69||(port>=71&&port<=73)||(port>=75&&port<=78)||port==112;
    assert(mt::PublicApplication::allowed(port)==expected);
    if(!expected)continue;
    auto f=std::make_unique<Fixture>();auto r=request(port);r.packet.decoded.has_bitfield=consent;r.packet.decoded.bitfield=consent?1:0;
    input(f->c,r,100);input(f->bc,r,101);
    assert(f->s.endpointFree()==3);f->s.poll(300,0);assert(f->radio.frames.size()==1);f->done(400);f->clients();
    bench::RouteMetadata m;const auto &wire=f->radio.frames[0];assert(f->s.codec.decode(wire.data(),wire.size(),m));
    assert(bench::le32(wire.data()+4)==OURS&&m.portnum==port&&m.payloadLength==4&&!memcmp(m.payload,r.packet.decoded.payload.bytes,4)&&
      m.reply_id==123&&m.emoji==0x1f44d&&m.hasBitfield&&m.bitfield==unsigned(consent)&&!m.wantResponse&&!m.signatureLength);
    assert(results(f->h,201,0)==1&&results(f->bh,201,0)==1);
  }
  group("exact application port allowlist including every private port and shared USB/BLE opaque origination with MQTT consent preserved or absent normalized to zero");
  for(unsigned mode=0;mode<3;++mode){auto f=std::make_unique<Fixture>();auto r=request(256);
    r.packet.decoded.has_bitfield=mode!=0;r.packet.decoded.bitfield=mode==2?1:0;input(f->c,r,100);f->s.poll(300,0);f->done(400);
    bench::RouteMetadata m;const auto &wire=f->radio.frames[0];assert(f->s.codec.decode(wire.data(),wire.size(),m)&&m.hasBitfield&&m.bitfield==(mode==2?1:0));
    f->rx(incoming(*f,256,701,OURS,false,false,1),1000);f->rx(incoming(*f,256,702,OURS,true,false,2),1500);f->s.poll(2000,0);
    assert(f->radio.frames.size()==1&&applications(f->h,256)==1&&applications(f->bh,256)==1);
    for(const auto *host:{&f->h,&f->bh})for(const auto &out:host->out)if(out.which_payload_variant==meshtastic_FromRadio_packet_tag&&out.packet.decoded.portnum==256)
      assert(out.packet.decoded.has_bitfield&&out.packet.decoded.bitfield==1);
  }
  group("absent explicit-zero and explicit-one consent stay distinct at client admission and canonical incoming one forwards while bit two cannot deliver or ACK");
  for(bool ble:{false,true}){auto f=std::make_unique<Fixture>(false);auto r=request(256,201,OURS,true);
    input(ble?f->bc:f->c,r,100);f->s.poll(1000,0);
    assert(!f->radio.sends&&applications(ble?f->bh:f->h,256)==1&&applications(ble?f->h:f->bh,256)==0&&results(ble?f->bh:f->h,201,0)==1);
    const Host &host=ble?f->bh:f->h;for(const auto &out:host.out)if(out.which_payload_variant==meshtastic_FromRadio_packet_tag&&out.packet.decoded.portnum==256)
      assert(out.packet.from==OURS&&out.packet.to==OURS&&!out.packet.want_ack&&!out.packet.hop_limit&&!out.packet.pki_encrypted&&!out.packet.xeddsa_signed);
  }
  group("local opaque delivery stays on requesting USB or BLE session with zero RF even RX-only");
  for(size_t hint:{size_t(1),size_t(31),size_t(32)})for(bool ble:{false,true})for(uint32_t to:{PEER,UINT32_MAX,OURS}){
    struct Observation {Bytes rf,localData;uint32_t localFrom=0,localTo=0;bool localPki=false,localSigned=false;};
    auto observe=[&](size_t hintLength){
      auto f=std::make_unique<Fixture>();auto r=request(256,801,to);
      if(hintLength){r.packet.public_key.size=hintLength;memset(r.packet.public_key.bytes,0x5a,hintLength);}
      input(ble?f->bc:f->c,r,100);f->s.poll(300,0);Observation result;
      if(to==OURS){
        assert(!f->radio.sends&&f->radio.frames.empty()&&f->s.endpointFree()==4);
        const Host &owner=ble?f->bh:f->h;const Host &other=ble?f->h:f->bh;
        assert(applications(owner,256)==1&&applications(other,256)==0);
        for(const auto &out:owner.out)if(out.which_payload_variant==meshtastic_FromRadio_packet_tag&&out.packet.decoded.portnum==256){
          assert(out.packet.from==OURS&&out.packet.to==OURS&&!out.packet.want_ack&&!out.packet.hop_limit&&
            !out.packet.pki_encrypted&&!out.packet.xeddsa_signed&&!out.packet.public_key.size&&out.packet.decoded.payload.size==4&&
            !memcmp(out.packet.decoded.payload.bytes,r.packet.decoded.payload.bytes,4)&&
            out.packet.decoded.reply_id==123&&out.packet.decoded.emoji==0x1f44d);
          result.localData=encoded(out.packet.decoded);result.localFrom=out.packet.from;result.localTo=out.packet.to;
          result.localPki=out.packet.pki_encrypted;result.localSigned=out.packet.xeddsa_signed;
        }
        assert(!result.localData.empty());
      } else {
        assert(f->radio.frames.size()==1&&f->radio.sends==1&&f->s.endpointFree()==3);
        bench::RouteMetadata metadata{};const auto &wire=f->radio.frames[0];
        assert(f->s.codec.decode(wire.data(),wire.size(),metadata)&&bench::le32(wire.data())==to&&
          bench::le32(wire.data()+4)==OURS&&metadata.portnum==256&&metadata.payloadLength==4&&
          !memcmp(metadata.payload,r.packet.decoded.payload.bytes,4)&&metadata.reply_id==123&&metadata.emoji==0x1f44d&&
          metadata.hasBitfield&&!metadata.bitfield&&!metadata.wantResponse&&!metadata.signatureLength);
        result.rf=wire;
      }
      return result;
    };
    const auto noHint=observe(0),hintOnly=observe(hint);
    if(to==OURS)assert(noHint.localData==hintOnly.localData&&noHint.localFrom==hintOnly.localFrom&&
      noHint.localTo==hintOnly.localTo&&noHint.localPki==hintOnly.localPki&&noHint.localSigned==hintOnly.localSigned);
    else assert(noHint.rf==hintOnly.rf);
  }
  group("bounded hint-only lengths match no-hint USB/BLE unicast, broadcast and local echo; RF stays canonical and local echo stays owner-only");
  for(size_t hintLength:{size_t(1),size_t(31),size_t(32)})for(bool ble:{false,true}){
    auto f=std::make_unique<Fixture>();auto r=request(1,803,OURS,true);
    r.packet.decoded.payload.size=5;memcpy(r.packet.decoded.payload.bytes,"local",5);
    r.packet.public_key.size=hintLength;memset(r.packet.public_key.bytes,0x5a,hintLength);
    input(ble?f->bc:f->c,r,100);f->s.poll(1000,0);
    const auto &owner=ble?f->bh:f->h;const auto &other=ble?f->h:f->bh;
    assert(!f->radio.sends&&f->radio.frames.empty()&&applications(owner,1)==1&&applications(other,1)==0&&results(owner,803,0)==1);
    for(const auto &out:owner.out)if(out.which_payload_variant==meshtastic_FromRadio_packet_tag&&out.packet.decoded.portnum==1){
      assert(out.packet.from==OURS&&out.packet.to==OURS&&!out.packet.want_ack&&!out.packet.pki_encrypted&&
        !out.packet.public_key.size&&out.packet.decoded.payload.size==5&&!memcmp(out.packet.decoded.payload.bytes,"local",5));
    }
  }
  group("hint-only local text echo clears caller key metadata, stays on requesting USB/BLE owner and uses no RF");
  for(bool ble:{false,true}){
    auto send=[&](bool cached,size_t hintLength,bool wrongHint){
      auto f=std::make_unique<Fixture>();int slot=-1;mt::NodeCache::Entry saved{};
      if(cached){
        mt::NodeCache::Entry advertised{};advertised.identity(PEER,127,1);
        advertised.bytes[mt::NodeCache::Entry::KeyLength]=32;
        memset(advertised.bytes+mt::NodeCache::Entry::Key,0x42,32);
        slot=f->s.nodes.accept(advertised,OURS,-3,1);assert(slot>=0&&!f->s.privateConfigured());
        assert(f->s.privateDestinationDisposition(PEER)==mt::pki::DestinationDisposition::KnownUnpinned);
        saved=*f->s.nodes.get(unsigned(slot));assert(!saved.verifiedSigner()&&!saved.manuallyVerified());
      }
      auto r=request(256,802,PEER);if(hintLength){r.packet.public_key.size=hintLength;
        memset(r.packet.public_key.bytes,wrongHint?0x99:0x42,hintLength);}
      input(ble?f->bc:f->c,r,100);f->s.poll(300,0);
      assert(f->radio.frames.size()==1&&f->radio.sends==1&&f->s.endpointFree()==3);
      bench::RouteMetadata metadata{};const auto &wire=f->radio.frames[0];
      assert(f->s.codec.decode(wire.data(),wire.size(),metadata)&&bench::le32(wire.data())==PEER&&
        bench::le32(wire.data()+4)==OURS&&metadata.portnum==256&&metadata.payloadLength==4&&
        !memcmp(metadata.payload,r.packet.decoded.payload.bytes,4)&&metadata.reply_id==123&&metadata.emoji==0x1f44d&&
        metadata.hasBitfield&&!metadata.bitfield&&!metadata.wantResponse&&!metadata.signatureLength);
      if(cached){const auto *after=f->s.nodes.get(unsigned(slot));assert(after&&
          !memcmp(saved.bytes,after->bytes,sizeof(saved.bytes))&&!after->verifiedSigner()&&!after->manuallyVerified());}
      return wire;
    };
    const auto noKeyControl=send(false,0,false);
    assert(send(true,0,false)==noKeyControl);
    assert(send(true,1,false)==noKeyControl);
    assert(send(true,32,true)==noKeyControl);
  }
  group("without local PKI, cached unpinned destination key and absent/short/wrong hints preserve canonical USB/BLE shared-channel application output without trust promotion");
  for(unsigned mode=0;mode<24;++mode){auto f=std::make_unique<Fixture>();auto r=request();auto &p=r.packet;
    switch(mode){case 0:p.from=PEER;break;case 1:p.hop_start=1;break;case 2:p.pki_encrypted=true;break;
      case 3:p.pki_encrypted=true;p.public_key.size=1;break;case 4:p.xeddsa_signed=true;break;case 5:p.ack_proof_status=meshtastic_MeshPacket_AckProofStatus_ACK_PROOF_VALID;break;
      case 6:p.decoded.xeddsa_signature.size=1;break;case 7:p.decoded.want_response=true;break;case 8:p.decoded.request_id=123;break;
      case 9:p.decoded.dest=123;break;case 10:p.decoded.source=123;break;case 11:p.decoded.has_bitfield=true;p.decoded.bitfield=2;break;
      case 12:p.has_rx_time=true;p.rx_time=1;break;case 13:p.rx_snr=1;break;case 14:p.has_rx_rssi=true;break;
      case 15:p.via_mqtt=true;break;case 16:p.next_hop=1;break;case 17:p.relay_node=1;break;case 18:p.tx_after=1;break;
      case 19:p.hop_limit=8;break;case 20:p.to=1;break;case 21:p.id=0;break;case 22:p.channel=1;break;
      case 23:p.decoded.portnum=meshtastic_PortNum_LORA_OTA_APP;break;}
    input(f->c,r,100);f->s.poll(1000,0);assert(!f->radio.sends&&results(f->h,p.id)>0);
  }
  group("generic client rejects cryptographic routing response receive-metadata reserved-port and envelope claims before RF");
  {auto oversized=request();oversized.packet.public_key.size=sizeof(oversized.packet.public_key.bytes)+1;
    assert(!mt::PublicApplication::client(oversized.packet,true));}
  {auto f=std::make_unique<Fixture>();auto r=request();uint8_t bytes[512]{};auto out=pb_ostream_from_buffer(bytes,sizeof(bytes));assert(pb_encode(&out,meshtastic_ToRadio_fields,&r));
    Bytes duplicate(bytes,bytes+out.bytes_written);duplicate.insert(duplicate.end(),bytes,bytes+out.bytes_written);f->c.packet(duplicate.data(),duplicate.size(),100,mt::Companion::WorkspaceProof{true});
    Bytes unknown(bytes,bytes+out.bytes_written);unknown.insert(unknown.end(),{0xa0,1,1});f->c.packet(unknown.data(),unknown.size(),200,mt::Companion::WorkspaceProof{true});
    f->s.poll(1000,0);assert(!f->radio.sends);
    uint8_t payload[233]{},data[239]{};size_t n=0;
    assert(mt::PublicApplication::encode(8,payload,232,0,0,data,sizeof(data),n)&&n==239&&mt::PublicApplication::canonical(data,n));
    assert(!mt::PublicApplication::encode(8,payload,233,0,0,data,sizeof(data),n)&&!n);
    assert(mt::PublicApplication::encode(511,payload,231,0,0,data,sizeof(data),n)&&n==239);
    assert(!mt::PublicApplication::encode(511,payload,232,0,0,data,sizeof(data),n)&&!n);
    assert(!mt::PublicApplication::encode(8,payload,4,0,0,data,sizeof(data),n,2)&&!n);
    assert(!mt::PublicApplication::encode(8,payload,4,0,0,data,sizeof(data),n,256)&&!n);
  }
  group("duplicate and unknown client fields fail canonical check and exact 239-byte Data boundary includes port overhead");
  {auto f=std::make_unique<Fixture>();auto r=request(8,201,PEER,true);input(f->c,r,100);f->s.poll(300,0);f->done(400);f->clients();
    f->rx(f->wire(PEER+1,OURS,701,201));f->rx(f->wire(PEER,OURS,702,202));f->clients();assert(!results(f->h,201));
    f->rx(incoming(*f,8,703),600);f->clients();assert(!results(f->h,201));
    f->rx(f->wire(PEER,OURS,704,201),700);f->clients();assert(results(f->h,201,0)==1);
    for(const auto &out:f->h.out)if(out.which_payload_variant==meshtastic_FromRadio_packet_tag&&out.packet.decoded.portnum==5)
      assert(out.packet.ack_proof_status==meshtastic_MeshPacket_AckProofStatus_ACK_PROOF_ABSENT&&!out.packet.pki_encrypted&&!out.packet.xeddsa_signed);
  }
  group("generic reliable message consumes only matching Public Routing ACK without claiming authenticated delivery");
  {auto f=std::make_unique<Fixture>();const auto wire=incoming(*f,8,701,OURS,true);f->rx(wire,1000,700);f->s.poll(1300,0);
    assert(f->radio.frames.size()==1&&applications(f->h,8)==1&&applications(f->bh,8)==1);f->done(1400);
    f->rx(wire,1500,701);f->s.poll(1800,0);assert(f->radio.frames.size()==1&&applications(f->h,8)==1&&applications(f->bh,8)==1);
    f->s.poll(20000,0);f->s.poll(21000,0);f->done(21200);f->s.poll(42000,0);f->s.poll(43000,0);f->done(43200);f->s.poll(65000,0);
    assert(f->radio.frames.size()==3&&f->radio.frames[0]==f->radio.frames[1]&&f->radio.frames[1]==f->radio.frames[2]);
  }
  group("incoming generic ACK and duplicate share one bounded three-attempt control while application fanout occurs once");
  {auto f=std::make_unique<Fixture>();f->rx(incoming(*f,8,701,0xffffffffu,true),1000);f->rx(incoming(*f,8,702,OURS,true,true),1100);
    for(unsigned port:{6,12,13,74,79})f->rx(incoming(*f,port,800+port,OURS,true),1200+port);
    f->s.poll(2000,0);assert(!f->radio.sends&&applications(f->h,8)==1&&applications(f->bh,8)==1);
  }
  group("broadcast malformed generic Data and excluded management ports cannot amplify ACKs or dispatch remote admin");
 {for(uint32_t expiry:{0u,1u,1792660000u}){
  meshtastic_Waypoint waypoint{};waypoint.id=23;waypoint.has_latitude_i=waypoint.has_longitude_i=true;
  waypoint.latitude_i=0;waypoint.longitude_i=-123456789;waypoint.expire=expiry;strcpy(waypoint.name,"Lab waypoint fixture");
  auto f=std::make_unique<Fixture>();auto r=request(8);auto body=pb_ostream_from_buffer(r.packet.decoded.payload.bytes,sizeof(r.packet.decoded.payload.bytes));
  assert(pb_encode(&body,meshtastic_Waypoint_fields,&waypoint));r.packet.decoded.payload.size=body.bytes_written;
  input(f->c,r,100);f->s.poll(300,0);assert(f->radio.frames.size()==1);bench::RouteMetadata m{};
  assert(f->s.codec.decode(f->radio.frames[0].data(),f->radio.frames[0].size(),m)&&m.payloadLength==body.bytes_written&&
    !memcmp(m.payload,r.packet.decoded.payload.bytes,m.payloadLength));
  // Malformed app payload is still opaque carrier data, not an accepted waypoint.
  auto invalid=request(8,202);invalid.packet.decoded.payload.size=1;invalid.packet.decoded.payload.bytes[0]=0xff;
  f->done(400);input(f->c,invalid,500);f->s.poll(800,0);assert(f->radio.frames.size()==2);
 }}
 group("Qwen waypoint vectors: permanent/deleted/timed and malformed app bytes preserve transport without claiming app parsing or expiry semantics");

}
