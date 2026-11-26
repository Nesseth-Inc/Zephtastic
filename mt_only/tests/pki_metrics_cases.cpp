// SPDX-License-Identifier: GPL-3.0-only
// Synthetic actual-Service/Companion/PSA integration cases. No device access.
#include "pki_runtime_fixture.h"
#include "known_channel_fixture.h"
#include "../src/pki/PrivateApplication.h"
#include <limits>

namespace {
void pass(const char *name){std::cout<<"PASS "<<name<<'\n';}
Bytes encoded(const meshtastic_Data &d){
  uint8_t b[227]{};auto out=pb_ostream_from_buffer(b,sizeof(b));
  assert(pb_encode(&out,meshtastic_Data_fields,&d));return {b,b+out.bytes_written};
}
Bytes metrics(uint32_t request,uint32_t uptime=123){
  uint8_t b[40]{};size_t n=0;
  assert(mt::TelemetryQuery::uptimeResponse(uptime,request,zt::Observed<float>{0.0f},zt::Observed<float>{0.0f},b,sizeof(b),n));
  return {b,b+n};
}
Bytes localStats(uint32_t request,bool maximum=false){
  meshtastic_Telemetry t{};t.time=UINT32_MAX;t.which_variant=meshtastic_Telemetry_local_stats_tag;
  auto &s=t.variant.local_stats;
  s={maximum?UINT32_MAX:321u,100.0f,0.25f,UINT32_MAX,UINT32_MAX,UINT32_MAX,
    UINT16_MAX,UINT16_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT16_MAX,INT32_MIN};
  meshtastic_Data d{};d.portnum=meshtastic_PortNum_TELEMETRY_APP;d.request_id=request;d.has_bitfield=true;
  auto out=pb_ostream_from_buffer(d.payload.bytes,sizeof(d.payload.bytes));
  assert(pb_encode(&out,meshtastic_Telemetry_fields,&t));d.payload.size=out.bytes_written;
  const auto bytes=encoded(d);assert(bytes.size()==(maximum?105u:102u));return bytes;
}
Bytes remoteTelemetry(uint8_t variant,uint32_t time=0){
  meshtastic_Telemetry t{};t.which_variant=variant;t.time=time;
  switch(variant){
    case 3:t.variant.environment_metrics.has_temperature=true;t.variant.environment_metrics.temperature=21.5f;break;
    case 4:t.variant.air_quality_metrics.has_pm25_standard=true;t.variant.air_quality_metrics.pm25_standard=17;break;
    case 5:t.variant.power_metrics.has_ch1_voltage=true;t.variant.power_metrics.ch1_voltage=12.25f;break;
    case 7:t.variant.health_metrics.has_heart_bpm=true;t.variant.health_metrics.heart_bpm=72;break;
    case 8:t.variant.host_metrics.uptime_seconds=654;t.variant.host_metrics.freemem_bytes=UINT64_C(12345678901);
      t.variant.host_metrics.has_user_string=true;strcpy(t.variant.host_metrics.user_string,"Synthetic peer host metrics");break;
    case 9:t.variant.traffic_management_stats.packets_inspected=321;t.variant.traffic_management_stats.rate_limit_drops=7;break;
    case 11:t.variant.soil_water_metrics.has_soil_ph=true;t.variant.soil_water_metrics.soil_ph=6.75f;break;
    default:assert(false);
  }
  uint8_t bytes[233]{};auto out=pb_ostream_from_buffer(bytes,sizeof(bytes));
  assert(pb_encode(&out,meshtastic_Telemetry_fields,&t));return {bytes,bytes+out.bytes_written};
}
Bytes remoteTelemetryResponse(uint8_t variant,uint32_t request,uint32_t time=1792660000){
  const auto payload=remoteTelemetry(variant,time);meshtastic_Data d{};
  d.portnum=meshtastic_PortNum_TELEMETRY_APP;d.request_id=request;d.has_bitfield=true;
  d.payload.size=payload.size();memcpy(d.payload.bytes,payload.data(),payload.size());return encoded(d);
}
meshtastic_Data decoded(const Bytes &b){meshtastic_Data d{};auto in=pb_istream_from_buffer(b.data(),b.size());assert(pb_decode(&in,meshtastic_Data_fields,&d)&&!in.bytes_left);return d;}
bool sameReplyDiagnostics(const PrivateReplyDiagnostics &a,const PrivateReplyDiagnostics &b){
  return a.attempts==b.attempts&&a.queued==b.queued&&a.no_slot==b.no_slot&&a.producer==b.producer&&a.id==b.id&&a.frame==b.frame&&a.queue==b.queue;
}

// Reuses the real PSA backend, journals, key derivation, radio and Host classes
// from the existing fixture. Only endpoint roles/seeds differ between boards.
struct BorrowedRadio:Radio {
  const uint8_t *borrowed=nullptr;size_t borrowedSize=0;unsigned busyRemaining=0,sendCalls=0;uint32_t air=100;
  int send(uint8_t *p,size_t n)override{++sendCalls;if(busyRemaining){--busyRemaining;return -EBUSY;}borrowed=p;borrowedSize=n;return Radio::send(p,n);}
  uint32_t airtime(size_t)override{return air;}
  bool holds(const Bytes &frame)const{return borrowed&&borrowedSize==frame.size()&&!memcmp(borrowed,frame.data(),frame.size());}
};
struct Side {
  Key local,peer;Memory im,lm;MonocypherIdentityDeriver deriver;IdentityJournal identity;PkiLedger ledger;
  Gate gate;Entropy entropy;SignEntropy signing;Backend backend;mt::experimental::PkiCrypto crypto{backend};
  LedgerNonceAuthority nonce{identity,ledger,lm,entropy,gate};LedgerReplayAuthority replay{identity,ledger,lm,gate};
  mt::test::KnownChannelFixtureClock knownChannelClock;
  PkiRuntime runtime{crypto,backend,identity,nonce,replay,&signing};BorrowedRadio radio;mt::Service s;
  Host usb,ble;mt::companion::CompanionWorkspace workspace;mt::Companion c,bc;uint32_t uptime;
  Side(unsigned localSeed,unsigned peerSeed,uint8_t model,bool known=true,unsigned role=1,unsigned mode=0,bool lateBind=false,bool normalDiscovery=false,unsigned signaturePolicy=0):local(localSeed),peer(peerSeed),
    s(radio,true,local.binding.node,model,true,normalDiscovery,lateBind?nullptr:&runtime),c(s,usb,model,workspace,0),bc(s,ble,model,workspace,1),uptime(500+localSeed){
    knownChannelClock.bind(s);
    assert(!identity.load(im,deriver));IdentityCandidate candidate(local.binding,local.scalar);
    assert(!identity.store(im,deriver,candidate,true));assert(!ledger.load(lm));
    assert(!ledger.initialize(lm,local.binding.publicKey,local.binding.node,true));
    assert(!ledger.addTrustedPeer(lm,peer.binding.node,peer.binding.publicKey,true));assert(runtime.open(local.binding.node,peer.binding.node));gate.service=&s;
    mt::Config cfg;cfg.bytes[4]=1;auto record=cfg.node();record.setPublic();cfg.node(record);assert(cfg.setDeviceRole(role,mode)&&cfg.setPacketSignaturePolicy(signaturePolicy));
    assert(s.apply(cfg));if(lateBind)assert(s.bindPrivateRuntime(runtime));assert(s.start(false,0)&&s.armEndpoint(true));s.measuredUptime(uptime);
    lm.onWrite=[this]{assert(s.privateStoragePaused()&&!radio.healthValue&&!s.active());};
    if(known){mt::NodeCache::Entry e{};e.identity(peer.binding.node,81,0);
      assert(e.text(e.bytes+e.Name,40,"Synthetic admitted peer",24)&&e.text(e.bytes+e.Short,5,"PKI",4));
      assert(s.nodes.accept(e,local.binding.node,1,0)>=0);}
    handshake();clear();
  }
  void handshake(){meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;r.want_config_id=7;input(c,r,1);input(bc,r,1);}
  void clear(){usb.out.clear();ble.out.clear();usb.records.clear();ble.records.clear();}
  meshtastic_ToRadio request(uint32_t id,bool query){
    meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_packet_tag;auto &p=r.packet;
    p.id=id;p.to=peer.binding.node;p.pki_encrypted=true;p.public_key.size=32;memcpy(p.public_key.bytes,peer.binding.publicKey,32);
    p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;p.decoded.portnum=query?meshtastic_PortNum_TELEMETRY_APP:meshtastic_PortNum_TEXT_MESSAGE_APP;
    p.decoded.want_response=query;
    if(query){meshtastic_Telemetry t{};t.which_variant=meshtastic_Telemetry_device_metrics_tag;auto out=pb_ostream_from_buffer(p.decoded.payload.bytes,sizeof(p.decoded.payload.bytes));assert(pb_encode(&out,meshtastic_Telemetry_fields,&t));p.decoded.payload.size=out.bytes_written;}
    else{constexpr char text[]="Synthetic private M68";p.decoded.payload.size=sizeof(text)-1;memcpy(p.decoded.payload.bytes,text,sizeof(text)-1);}
    return r;
  }
  void send(uint32_t id,bool query,bool bothOwners=false){auto r=request(id,query);input(c,r,100);if(bothOwners)input(bc,r,101);}
  void poll(uint32_t now,uint32_t random=711){s.poll(now,random);c.poll(mt::Companion::WorkspaceProof{true});bc.poll(mt::Companion::WorkspaceProof{true});}
  void complete(uint32_t now){radio.done=true;poll(now);}
  void receive(const Bytes &frame,uint32_t now=1500,uint32_t random=901){
    s.rx(frame.data(),frame.size(),3,now,random);
    c.receive(frame.data(),frame.size(),-71,3,now,1792660000u+now/1000,true,12,true,mt::Companion::WorkspaceProof{true});
    bc.receive(frame.data(),frame.size(),-71,3,now,1792660000u+now/1000,true,12,true,mt::Companion::WorkspaceProof{true});
  }
  unsigned applications(const Host &host,unsigned port)const{unsigned count=0;for(const auto &r:host.out)if(r.which_payload_variant==meshtastic_FromRadio_packet_tag&&r.packet.pki_encrypted&&r.packet.decoded.portnum==port)++count;return count;}
  const mt::NodeCache::Entry *entry()const{const int slot=s.nodes.find(peer.binding.node);return slot<0?nullptr:s.nodes.get(unsigned(slot));}
  bool cached()const{const auto *e=entry();return e&&e->deviceMetrics.present();}
  Bytes incoming(const Bytes &plain,uint32_t id,const uint8_t *scalar=nullptr){
    uint8_t cipher[239]{};size_t n=0;
    assert(crypto.encrypt(scalar?scalar:peer.scalar,local.binding.publicKey,peer.binding.node,id,0x19374628,plain.data(),plain.size(),cipher,sizeof(cipher),n));
    Bytes frame(16+n);put(frame.data(),local.binding.node);put(frame.data()+4,peer.binding.node);put(frame.data()+8,id);frame[15]=uint8_t(peer.binding.node);memcpy(frame.data()+16,cipher,n);return frame;
  }
  Bytes ack(uint32_t request,bool proof=true,uint32_t code=0,uint32_t from=0,bool corrupt=false){
    if(!from)from=peer.binding.node;
    uint8_t routing[12]{},raw[2]={24,uint8_t(code)};size_t rn=2;memcpy(routing,raw,2);
    if(proof){mt::experimental::PkiAck a(crypto);assert(a.attach(peer.scalar,local.binding.publicKey,
      {true,from,local.binding.node,request},raw,2,routing,sizeof(routing),rn));}
    if(corrupt){assert(proof&&rn==12);routing[rn-1]^=1;}
    meshtastic_Data d{};d.portnum=meshtastic_PortNum_ROUTING_APP;d.request_id=request;
    d.has_bitfield=true;d.payload.size=rn;memcpy(d.payload.bytes,routing,rn);
    const auto clear=encoded(d);uint8_t frame[255]{};size_t len=0;
    assert(s.codec.encodeZeroHopData(from,local.binding.node,request+1000,clear.data(),clear.size(),frame,sizeof(frame),len));
    return {frame,frame+len};
  }
  Bytes decryptForPeer(const Bytes &frame){uint8_t plain[227]{};size_t n=0;assert(crypto.decrypt(peer.scalar,local.binding.publicKey,local.binding.node,bench::le32(frame.data()+8),frame.data()+16,frame.size()-16,plain,sizeof(plain),n));return {plain,plain+n};}
};

void replyDiagnostics(const Side &f,const PrivateReplyDiagnostics &expected){
  assert(sameReplyDiagnostics(f.runtime.replyDiagnostics(),expected)&&sameReplyDiagnostics(f.s.privateReplyDiagnostics(),expected));
}

// Exercise the real private request -> PkiRuntime::metricsReply -> control queue.
void generatedMetrics(Side &a,Side &b,bool reliable,bool complete=true){
  if(reliable){
    // Keep this direct-Service sender to isolate generated response-control
    // reliability. Separate phase cases below exercise Companion want_ack.
    constexpr uint8_t query[]={8,67,18,2,18,0,24,1,72,2};
    assert(!a.s.submitPrivate(0,a.peer.binding.node,201,query,sizeof(query),0,true,
      a.peer.binding.publicKey,32,true,100));
  }else{auto r=a.request(201,true);input(a.c,r,100);}
  a.poll(1000);assert(a.radio.frames.size()==1);a.complete(1200);
  assert(bool(a.radio.frames[0][12]&8)==reliable);
  b.receive(a.radio.frames[0],1500);b.poll(2000,902);assert(b.radio.frames.size()==1);
  replyDiagnostics(a,{});replyDiagnostics(b,{1,1});
  const auto &wire=b.radio.frames[0];assert(wire.size()>28&&!wire[13]&&bool(wire[12]&8)==reliable);
  const auto d=decoded(b.decryptForPeer(wire));
  assert(d.portnum==meshtastic_PortNum_TELEMETRY_APP&&d.request_id==201&&!d.want_response);
  meshtastic_Telemetry actual{};auto stream=pb_istream_from_buffer(d.payload.bytes,d.payload.size);
  assert(pb_decode(&stream,meshtastic_Telemetry_fields,&actual)&&!stream.bytes_left&&
    actual.which_variant==meshtastic_Telemetry_device_metrics_tag&&
    actual.variant.device_metrics.has_uptime_seconds&&actual.variant.device_metrics.uptime_seconds==b.uptime);
  if(complete)b.complete(2200);
}

Bytes localStatsSelector(){return {8,67,18,2,0x32,0,24,1,72,2};}
void checkGeneratedLocalStats(Side &receiver,const Bytes &wire,uint32_t request,bool ack,uint32_t completedTx=0){
  assert(wire.size()>28&&!wire[13]&&bool(wire[12]&8)==ack&&bench::le32(wire.data()+8)!=request);
  const auto plain=receiver.decryptForPeer(wire);const auto d=decoded(plain);
  assert(d.portnum==meshtastic_PortNum_TELEMETRY_APP&&d.request_id==request&&!d.want_response&&d.has_bitfield&&!d.bitfield);
  meshtastic_Telemetry actual{};auto in=pb_istream_from_buffer(d.payload.bytes,d.payload.size);
  assert(pb_decode(&in,meshtastic_Telemetry_fields,&actual)&&!in.bytes_left&&actual.which_variant==meshtastic_Telemetry_local_stats_tag);
  const auto &stats=actual.variant.local_stats;
  assert(stats.uptime_seconds==receiver.uptime&&!actual.time&&stats.air_util_tx>=0&&stats.air_util_tx<=100&&stats.channel_utilization>=0&&stats.channel_utilization<=100);
  // Snapshot precedes the response's own native completion. Stored ignored
  // placeholders count toward NodeDB total; neither total nor RX means online.
  assert(stats.num_packets_tx==completedTx&&stats.num_total_nodes==1+receiver.s.nodes.count());
  assert(!stats.num_packets_rx&&!stats.num_packets_rx_bad&&!stats.num_online_nodes&&
    !stats.num_rx_dupe&&!stats.num_tx_relay&&!stats.num_tx_relay_canceled&&
    !stats.heap_total_bytes&&!stats.heap_free_bytes&&!stats.num_tx_dropped&&!stats.noise_floor);
  assert(mt::LocalStatsQuery::responsePayload(d.payload.bytes,d.payload.size)&&encoded(d)==plain);
}
void generatedLocalStats(Side &a,Side &b,bool ack,bool complete=true){
  auto query=a.request(201,true);query.packet.want_ack=ack;
  query.packet.decoded.payload.size=2;query.packet.decoded.payload.bytes[0]=0x32;query.packet.decoded.payload.bytes[1]=0;
  input(a.c,query,100);input(a.bc,query,101);a.poll(1000);assert(a.radio.frames.size()==1);a.complete(1200);
  assert(bool(a.radio.frames[0][12]&8)==ack);b.receive(a.radio.frames[0],1500);b.poll(2000,902);
  replyDiagnostics(a,{});replyDiagnostics(b,{1,1});
  assert(b.radio.frames.size()==1);checkGeneratedLocalStats(b,b.radio.frames[0],bench::le32(a.radio.frames[0].data()+8),ack);
  if(complete)b.complete(2200);
}

Bytes positionQuery(){return {8,3,24,1,72,2};}
void configurePrivatePosition(Side &f,bool share=true){
  f.s.stop();auto cfg=f.s.config;
  assert(cfg.setFixedPosition(123456789,-987654321,true,-35));cfg.setPositionSharing(share);
  assert(cfg.positionSharing()==share&&f.s.apply(cfg)&&f.s.start(false,10)&&f.s.armEndpoint(true));
}
Bytes privatePositionRequest(Side &f,uint32_t id,bool ack){
  auto wire=f.incoming(positionQuery(),id);wire[12]=ack?8:0;return wire;
}
void checkPrivatePosition(Side &f,const Bytes &wire,uint32_t request,bool ack){
  assert(wire.size()>28&&!wire[13]&&bool(wire[12]&8)==ack);
  auto d=decoded(f.decryptForPeer(wire));
  assert(d.portnum==meshtastic_PortNum_POSITION_APP&&d.request_id==request&&!d.want_response&&d.has_bitfield&&!d.bitfield);
  meshtastic_Position p{};auto in=pb_istream_from_buffer(d.payload.bytes,d.payload.size);
  assert(pb_decode(&in,meshtastic_Position_fields,&p)&&!in.bytes_left&&p.has_latitude_i&&p.has_longitude_i&&p.has_altitude&&p.altitude==-35);
  assert(p.latitude_i==mt::PositionQuery::publicCoordinate(123456789)&&p.longitude_i==mt::PositionQuery::publicCoordinate(-987654321));
  assert(p.location_source==meshtastic_Position_LocSource_LOC_MANUAL&&p.precision_bits==mt::Config::PublicPositionPrecision&&!p.time);
}
Bytes positionResponse(uint32_t request,int32_t latitude=123456789,int32_t longitude=-987654321){
  meshtastic_Position p{};p.has_latitude_i=p.has_longitude_i=true;p.latitude_i=latitude;p.longitude_i=longitude;
  p.has_altitude=true;p.altitude=-35;p.location_source=meshtastic_Position_LocSource_LOC_MANUAL;p.precision_bits=32;
  p.time=1792660000;
  meshtastic_Data d{};d.portnum=meshtastic_PortNum_POSITION_APP;d.request_id=request;d.has_bitfield=true;
  auto out=pb_ostream_from_buffer(d.payload.bytes,sizeof(d.payload.bytes));assert(pb_encode(&out,meshtastic_Position_fields,&p));
  d.payload.size=out.bytes_written;return encoded(d);
}
meshtastic_ToRadio positionRequest(Side &f,bool ack=true){
  auto r=f.request(201,true);r.packet.want_ack=ack;r.packet.decoded.portnum=meshtastic_PortNum_POSITION_APP;
  r.packet.decoded.payload.size=0;return r;
}
// Stock Companion excludes Position from its client PKI transport. These
// cases exercise the lower-level pinned Service protocol directly instead.
void submitPrivatePosition(Side &f,bool usb=true,bool ble=true,bool ack=true){
  const auto query=positionQuery();
  if(usb)assert(!f.s.submitPrivate(0,f.peer.binding.node,201,query.data(),query.size(),0,ack,
                                  f.peer.binding.publicKey,32,true,100));
  if(ble)assert(!f.s.submitPrivate(1,f.peer.binding.node,201,query.data(),query.size(),0,ack,
                                  f.peer.binding.publicKey,32,true,101));
}
void requestPosition(Side &f,bool ack=true,bool usb=true,bool ble=true,bool complete=true){
  submitPrivatePosition(f,usb,ble,ack);
  assert(f.entropy.calls==1&&f.backend.encrypts==1&&f.ledger.sent(201));
  f.poll(1000);assert(f.radio.frames.size()==1&&bool(f.radio.frames[0][12]&8)==ack&&!f.radio.frames[0][13]);
  assert(f.decryptForPeer(f.radio.frames[0])==positionQuery());
  if(complete)f.complete(1200);
}
void deliveredPosition(const Side &f,const Bytes &plain,uint32_t packet,bool usb=true,bool ble=true){
  const auto expected=decoded(plain);
  for(unsigned owner=0;owner<2;++owner){const auto &host=owner?f.ble:f.usb;const bool wanted=owner?ble:usb;
    assert(f.applications(host,meshtastic_PortNum_POSITION_APP)==unsigned(wanted));
    for(const auto &r:host.out)if(r.which_payload_variant==meshtastic_FromRadio_packet_tag&&r.packet.pki_encrypted&&
      r.packet.decoded.portnum==meshtastic_PortNum_POSITION_APP){
      assert(wanted&&r.packet.from==f.peer.binding.node&&r.packet.to==f.local.binding.node&&r.packet.id==packet&&
        r.packet.public_key.size==32&&!memcmp(r.packet.public_key.bytes,f.peer.binding.publicKey,32)&&
        r.packet.decoded.request_id==expected.request_id&&!r.packet.decoded.want_response&&
        r.packet.decoded.payload.size==expected.payload.size&&!memcmp(r.packet.decoded.payload.bytes,expected.payload.bytes,expected.payload.size));
    }
  }
}
void noPosition(const Side &f){assert(!f.applications(f.usb,meshtastic_PortNum_POSITION_APP)&&!f.applications(f.ble,meshtastic_PortNum_POSITION_APP));}
Bytes positionBytes(const meshtastic_Position &position){
  uint8_t bytes[meshtastic_Position_size]{};auto out=pb_ostream_from_buffer(bytes,sizeof(bytes));
  assert(pb_encode(&out,meshtastic_Position_fields,&position));return {bytes,bytes+out.bytes_written};
}
void cachedPeerPosition(const Side &f,const Bytes &plain){
  assert(f.entry()&&f.entry()->position.present());meshtastic_Position actual{};f.entry()->position.exportTo(actual);
  const auto expected=decoded(plain);assert(positionBytes(actual)==Bytes(expected.payload.bytes,expected.payload.bytes+expected.payload.size));
}
void thinPeer(const Host &host,uint32_t peer){
  unsigned count=0;
  for(const auto &r:host.out)if(r.which_payload_variant==meshtastic_FromRadio_node_info_tag&&r.node_info.num==peer){
    assert(!r.node_info.has_position&&!r.node_info.has_device_metrics);++count;
  }
  assert(count==1);
}

void waiting(Side &s,uint32_t id=101,bool bothOwners=true){
  s.send(id,true,bothOwners);assert(s.entropy.calls==1&&s.backend.encrypts==1&&s.ledger.sent(id));
  s.poll(1000);assert(s.radio.frames.size()==1&&!s.radio.frames[0][13]);s.complete(1200);
}
void cachedUptime(const Side &s,uint32_t expected){assert(s.cached());meshtastic_DeviceMetrics m{};s.entry()->deviceMetrics.exportTo(m);assert(m.has_uptime_seconds&&m.uptime_seconds==expected);}
Bytes metricsWire(const meshtastic_DeviceMetrics &m){uint8_t bytes[100]{};auto out=pb_ostream_from_buffer(bytes,sizeof(bytes));assert(pb_encode(&out,meshtastic_DeviceMetrics_fields,&m));return {bytes,bytes+out.bytes_written};}
void noRoutingProof(const Host &host,uint32_t request){unsigned results=0;for(const auto &r:host.out)if(r.which_payload_variant==meshtastic_FromRadio_packet_tag&&r.packet.decoded.portnum==meshtastic_PortNum_ROUTING_APP&&r.packet.decoded.request_id==request){assert(r.packet.ack_proof_status!=meshtastic_MeshPacket_AckProofStatus_ACK_PROOF_VALID);++results;}assert(results==1);}


// The real Companion selector owns the same one encrypted query for USB/BLE.
void reliableQuery(Side &f,bool complete=true){
  auto r=f.request(201,true);r.packet.want_ack=true;input(f.c,r,100);input(f.bc,r,101);
  assert(f.entropy.calls==1&&f.backend.encrypts==1&&f.ledger.sent(201));
  f.poll(1000);assert(f.radio.frames.size()==1&&f.s.active());
  const auto &wire=f.radio.frames[0];assert(bench::le32(wire.data()+8)==201&&(wire[12]&8)&&!wire[13]);
  const auto d=decoded(f.decryptForPeer(wire));
  assert(d.portnum==meshtastic_PortNum_TELEMETRY_APP&&d.want_response&&!d.request_id);
  if(complete)f.complete(1200);
}
unsigned routingResults(const Host &host,uint32_t request,bool proof,int code=-1){
  unsigned count=0;for(const auto &r:host.out)if(r.which_payload_variant==meshtastic_FromRadio_packet_tag&&
    r.packet.decoded.portnum==meshtastic_PortNum_ROUTING_APP&&r.packet.decoded.request_id==request&&
    (r.packet.ack_proof_status==meshtastic_MeshPacket_AckProofStatus_ACK_PROOF_VALID)==proof){
    meshtastic_Routing rt{};auto in=pb_istream_from_buffer(r.packet.decoded.payload.bytes,r.packet.decoded.payload.size);
    assert(pb_decode(&in,meshtastic_Routing_fields,&rt)&&!in.bytes_left);
    if(code<0||int(rt.error_reason)==code)++count;
  }
  return count;
}
void queryResults(const Side &f,unsigned proofs,unsigned terminal,int terminalCode=0,int proofCode=0){
  for(const auto *host:{&f.usb,&f.ble}){
    assert(routingResults(*host,201,true)==proofs&&routingResults(*host,201,true,proofCode)==proofs);
    assert(routingResults(*host,201,false)==terminal&&routingResults(*host,201,false,terminalCode)==terminal);
  }
}
void companionPositionPkiRefusal(){
  assert(int(meshtastic_Routing_Error_PKI_FAILED)==34);
  for(uint8_t model:{81,127})for(bool ack:{false,true}){
    Side f(11,97,model);const auto request=positionRequest(f,ack);
    const auto identity=f.im.values,ledger=f.lm.values;
    const auto identityWrites=f.im.writes,ledgerWrites=f.lm.writes;
    const auto nonces=f.entropy.calls,encrypts=f.backend.encrypts,decrypts=f.backend.decrypts;
    input(f.c,request,100);input(f.bc,request,101);
    assert(routingResults(f.usb,201,false,meshtastic_Routing_Error_PKI_FAILED)==1&&
           routingResults(f.ble,201,false,meshtastic_Routing_Error_PKI_FAILED)==1);
    assert(f.radio.frames.empty()&&!f.radio.sends&&f.s.endpointFree()==4&&
      f.im.values==identity&&f.lm.values==ledger&&f.im.writes==identityWrites&&f.lm.writes==ledgerWrites&&
      f.entropy.calls==nonces&&f.backend.encrypts==encrypts&&f.backend.decrypts==decrypts);
  }
  pass("Companion explicitly rejects PKI Position from both owners and board models with correlated error 34 and no private crypto, nonce, storage, or RF effects");
}
void noMetrics(const Side &f){assert(!f.cached()&&!f.applications(f.usb,67)&&!f.applications(f.ble,67));}
void localStatsQuery(Side &f,bool usb=true,bool ble=true,uint32_t id=201,bool complete=true){
  auto r=f.request(id,true);r.packet.want_ack=true;
  r.packet.decoded.payload.size=2;r.packet.decoded.payload.bytes[0]=0x32;r.packet.decoded.payload.bytes[1]=0;
  if(usb)input(f.c,r,100);
  if(ble)input(f.bc,r,101);
  f.poll(1000);assert(f.radio.frames.size()==1&&f.s.active());
  const auto d=decoded(f.decryptForPeer(f.radio.frames[0]));
  assert(d.want_response&&d.payload.size==2&&d.payload.bytes[0]==0x32&&!d.payload.bytes[1]);
  if(complete)f.complete(1200);
}
void deliveredLocalStats(const Side &f,const Bytes &plain,uint32_t packet,bool usb=true,bool ble=true){
  const auto expected=decoded(plain);
  for(unsigned owner=0;owner<2;++owner){const auto &host=owner?f.ble:f.usb;const bool wanted=owner?ble:usb;
    assert(f.applications(host,67)==unsigned(wanted));
    for(const auto &r:host.out)if(r.which_payload_variant==meshtastic_FromRadio_packet_tag&&r.packet.pki_encrypted&&r.packet.decoded.portnum==meshtastic_PortNum_TELEMETRY_APP){
      assert(wanted&&r.packet.from==f.peer.binding.node&&r.packet.to==f.local.binding.node&&r.packet.id==packet&&
        r.packet.decoded.request_id==expected.request_id&&r.packet.decoded.payload.size==expected.payload.size&&
        !memcmp(r.packet.decoded.payload.bytes,expected.payload.bytes,expected.payload.size));
    }
  }
}
void deliveredMetrics(const Side &f,uint32_t uptime,uint32_t packet=5001){
  cachedUptime(f,uptime);
  for(const auto *host:{&f.usb,&f.ble}){
    assert(f.applications(*host,67)==1);unsigned matches=0;
    for(const auto &r:host->out)if(r.which_payload_variant==meshtastic_FromRadio_packet_tag&&r.packet.pki_encrypted&&r.packet.decoded.portnum==meshtastic_PortNum_TELEMETRY_APP){
      assert(r.packet.from==f.peer.binding.node&&r.packet.to==f.local.binding.node&&r.packet.id==packet&&
        r.packet.decoded.request_id==201&&!r.packet.decoded.want_response);++matches;
    }assert(matches==1);
  }
}

// Actual signer/channel codec, with only a synthetic sender's private journal.
// The receiver never receives a NodeInfo bootstrap or borrows that sender key.
Bytes signedPublic(const Key &sender,bench::MtCodec &codec,const Bytes &plain,uint32_t id,bool corrupt=false){
  Memory memory;MonocypherIdentityDeriver deriver;IdentityJournal identity;
  assert(!identity.load(memory,deriver));IdentityCandidate candidate(sender.binding,sender.scalar);
  assert(!identity.store(memory,deriver,candidate,true));IdentityBinding binding;KeyRef ref;
  assert(identity.binding(binding,ref));SignEntropy entropy;SignedData signedData;
  auto d=decoded(plain);assert(!d.want_response&&!d.request_id&&d.has_bitfield);
  assert(PkiSigner::sign(identity,ref,binding,entropy,id,0xffffffffu,d,signedData)==SigningResult::Ok);
  if(corrupt){auto damaged=decoded(Bytes(signedData.bytes,signedData.bytes+signedData.size));
    assert(damaged.xeddsa_signature.size==64);damaged.xeddsa_signature.bytes[0]^=1;
    const auto bytes=encoded(damaged);signedData.size=bytes.size();memcpy(signedData.bytes,bytes.data(),bytes.size());}
  uint8_t frame[255]{};size_t size=0;
  assert(codec.encodeSignedApplication(sender.binding.node,id,signedData.bytes,signedData.size,frame,sizeof(frame),size));
  return {frame,frame+size};
}
Bytes publicMetrics(){auto d=decoded(metrics(700));d.request_id=0;return encoded(d);}
unsigned publicApplications(const Host &host,uint32_t peer,unsigned port){
  unsigned count=0;for(const auto &r:host.out)if(r.which_payload_variant==meshtastic_FromRadio_packet_tag&&
    r.packet.from==peer&&r.packet.decoded.portnum==port){
    assert(!r.packet.pki_encrypted&&r.packet.to==0xffffffffu&&!r.packet.decoded.request_id);++count;}
  return count;
}
struct PublicReadOnly {
  const Side &side;const std::map<std::string,Bytes> identity,ledger;
  const unsigned identityWrites,ledgerWrites,nonces,encrypts,decrypts,sends;
  const bool dirty;const Bytes node;
  explicit PublicReadOnly(const Side &s):side(s),identity(s.im.values),ledger(s.lm.values),
    identityWrites(s.im.writes),ledgerWrites(s.lm.writes),nonces(s.entropy.calls),
    encrypts(s.backend.encrypts),decrypts(s.backend.decrypts),sends(s.radio.sends),
    dirty(s.s.nodes.dirty()),node(s.entry()?Bytes(s.entry()->bytes,s.entry()->bytes+mt::NodeCache::Entry::Size):Bytes{}){}
  void unchanged()const{
    assert(side.im.values==identity&&side.lm.values==ledger&&side.im.writes==identityWrites&&side.lm.writes==ledgerWrites);
    assert(side.entropy.calls==nonces&&side.backend.encrypts==encrypts&&side.backend.decrypts==decrypts&&side.radio.sends==sends);
    assert(side.s.nodes.dirty()==dirty);
    if(!node.empty())assert(side.entry()&&!memcmp(node.data(),side.entry()->bytes,node.size()));
  }
  // A successfully verified signed Public packet intentionally commits only
  // the durable anti-downgrade bit in NodeDB. Keep every other observed byte
  // and all identity, ledger, crypto, RF, and dirty-state assertions strict.
  void unchangedExceptVerifiedSigner()const{
    assert(side.im.values==identity&&side.lm.values==ledger&&side.im.writes==identityWrites&&side.lm.writes==ledgerWrites);
    assert(side.entropy.calls==nonces&&side.backend.encrypts==encrypts&&side.backend.decrypts==decrypts&&side.radio.sends==sends);
    assert(side.s.nodes.dirty()==dirty);
    assert(!node.empty());
    {
      const auto *entry=side.entry();assert(entry);
      using Entry=mt::NodeCache::Entry;
      const uint8_t before=node[Entry::LocalFlags],after=entry->bytes[Entry::LocalFlags];
      assert(!(before&Entry::VerifiedSigner)&&(after&Entry::VerifiedSigner));
      assert(((before^after)&uint8_t(~Entry::VerifiedSigner))==0);
      for(size_t i=0;i<node.size();++i)if(i!=Entry::LocalFlags)assert(node[i]==entry->bytes[i]);
    }
  }
};
bench::RouteMetadata publicStatus(Side &side,const Bytes &frame,uint32_t now){
  bench::RouteMetadata m;assert(side.s.codec.decode(frame.data(),frame.size(),m)&&m.signatureLength==64);
  side.s.applyPublicSignature(frame.data(),frame.size(),m,now);return m;
}

void bidirectional(bool reverse,bool query){
  Side a(reverse?97:11,reverse?11:97,reverse?81:127),b(reverse?11:97,reverse?97:11,reverse?127:81);
  a.send(101,query,true);assert(a.entropy.calls==1&&a.backend.encrypts==1);a.poll(1000);assert(a.radio.frames.size()==1);a.complete(1200);
  auto request=a.radio.frames[0];assert(!request[13]);const auto clear=a.decryptForPeer(request);const auto d=decoded(clear);
  assert(d.portnum==(query?meshtastic_PortNum_TELEMETRY_APP:meshtastic_PortNum_TEXT_MESSAGE_APP));
  const auto before=b.lm.writes;b.receive(request,1500);
  assert(b.lm.writes==before+(query?2u:1u)&&b.applications(b.usb,d.portnum)==1&&b.applications(b.ble,d.portnum)==1);
  assert(!b.cached()); // Receiving the request/text never populates DeviceMetrics.
  if(!query){b.receive(request,1700);assert(b.lm.writes==before+1&&b.applications(b.usb,d.portnum)==1&&!b.radio.sends);return;}
  assert(d.want_response&&!d.request_id);b.poll(2000,902);assert(b.radio.frames.size()==1);b.complete(2200);
  const auto response=b.radio.frames[0];const auto reply=decoded(b.decryptForPeer(response));assert(reply.request_id==101&&!reply.want_response&&reply.portnum==meshtastic_PortNum_TELEMETRY_APP);
  const auto durable=a.entry()?Bytes(a.entry()->bytes,a.entry()->bytes+mt::NodeCache::Entry::Size):Bytes{};const bool dirty=a.s.nodes.dirty();
  a.receive(response,2300);a.poll(2301);cachedUptime(a,b.uptime);
  meshtastic_Telemetry actual{};auto input=pb_istream_from_buffer(reply.payload.bytes,reply.payload.size);assert(pb_decode(&input,meshtastic_Telemetry_fields,&actual));meshtastic_DeviceMetrics cache{};a.entry()->deviceMetrics.exportTo(cache);assert(metricsWire(cache)==metricsWire(actual.variant.device_metrics));
  assert(a.applications(a.usb,meshtastic_PortNum_TELEMETRY_APP)==1&&a.applications(a.ble,meshtastic_PortNum_TELEMETRY_APP)==1);
  noRoutingProof(a.usb,101);noRoutingProof(a.ble,101);
  assert(a.s.nodes.dirty()==dirty&&!memcmp(durable.data(),a.entry()->bytes,durable.size()));
  const auto writes=a.lm.writes;a.c.receive(response.data(),response.size(),-71,3,2400,mt::Companion::WorkspaceProof{true});a.bc.receive(response.data(),response.size(),-71,3,2400,mt::Companion::WorkspaceProof{true});
  a.receive(response,2500);assert(a.lm.writes==writes&&a.applications(a.usb,meshtastic_PortNum_TELEMETRY_APP)==1);
  a.clear();a.handshake();cachedUptime(a,b.uptime);thinPeer(a.usb,a.peer.binding.node);thinPeer(a.ble,a.peer.binding.node);
}
}

int main(){
  for(uint8_t model:{81,127})for(bool ack:{false,true}){
    Side a(11,97,model),b(97,11,model==81?127:81);
    zt::telemetry::EnvironmentReadings value;value.ambientCelsius=0.f;value.humidityPercent=50.f;value.pressureHpa=1013.25f;b.s.measuredEnvironment(value);
    auto r=a.request(201,true);r.packet.want_ack=ack;r.packet.decoded.payload.size=2;r.packet.decoded.payload.bytes[0]=0x1a;r.packet.decoded.payload.bytes[1]=0;
    input(a.c,r,100);a.poll(1000);assert(a.radio.frames.size()==1);a.complete(1200);
    b.receive(a.radio.frames[0],1500);b.poll(2000,902);assert(b.radio.frames.size()==1);replyDiagnostics(b,{1,1});
    const auto wire=b.radio.frames[0];const auto d=decoded(b.decryptForPeer(wire));assert(d.portnum==67&&d.request_id==201&&bool(wire[12]&8)==ack);
    meshtastic_Telemetry t{};auto in=pb_istream_from_buffer(d.payload.bytes,d.payload.size);assert(pb_decode(&in,meshtastic_Telemetry_fields,&t)&&!in.bytes_left&&t.which_variant==3&&!t.time);
    const auto&m=t.variant.environment_metrics;assert(m.has_temperature&&m.temperature==0&&m.has_relative_humidity&&m.relative_humidity==50&&m.has_barometric_pressure&&m.barometric_pressure==1013.25f&&!m.has_voltage&&!m.has_current);
    b.complete(2200);a.receive(wire,2500);a.poll(2600);assert(a.applications(a.usb,67)==1&&!a.applications(a.ble,67));
    a.receive(wire,2700);assert(a.applications(a.usb,67)==1);
  }
  pass("private environment: both target models, real PSA, measured zero/units, request correlation, ACK policy and owner-only delivery");
  {Side a(11,97,127),b(97,11,81);auto r=a.request(201,true);r.packet.decoded.payload.size=2;r.packet.decoded.payload.bytes[0]=0x1a;r.packet.decoded.payload.bytes[1]=0;
    input(a.c,r,100);a.poll(1000);assert(a.radio.frames.size()==1);a.complete(1200);b.receive(a.radio.frames[0],1500);b.poll(2000,902);
    assert(b.radio.frames.empty());replyDiagnostics(b,{1,0,0,1});}
  pass("private environment unavailable producer returns no invented measurement or RF response");
  for(uint8_t model:{81,127})for(bool ack:{false,true}){
    Side a(11,97,model),b(97,11,model==81?127:81);
    zt::telemetry::PowerReadings value;value.voltage[0]=0.f;value.current[0]=-125.f;value.voltage[2]=12.5f;b.s.measuredPower(value);
    auto r=a.request(201,true);r.packet.want_ack=ack;r.packet.decoded.payload.size=2;r.packet.decoded.payload.bytes[0]=0x2a;r.packet.decoded.payload.bytes[1]=0;
    input(a.c,r,100);a.poll(1000);assert(a.radio.frames.size()==1);a.complete(1200);
    b.receive(a.radio.frames[0],1500);b.poll(2000,902);assert(b.radio.frames.size()==1);replyDiagnostics(b,{1,1});
    const auto wire=b.radio.frames[0];const auto d=decoded(b.decryptForPeer(wire));assert(d.portnum==67&&d.request_id==201&&bool(wire[12]&8)==ack);
    meshtastic_Telemetry t{};auto in=pb_istream_from_buffer(d.payload.bytes,d.payload.size);assert(pb_decode(&in,meshtastic_Telemetry_fields,&t)&&!in.bytes_left&&t.which_variant==5&&!t.time);
    const auto&m=t.variant.power_metrics;assert(m.has_ch1_voltage&&m.ch1_voltage==0&&m.has_ch1_current&&m.ch1_current==-125&&m.has_ch3_voltage&&m.ch3_voltage==12.5f&&!m.has_ch2_voltage&&!m.has_ch2_current&&!m.has_ch3_current);
    b.complete(2200);a.receive(wire,2500);a.poll(2600);assert(a.applications(a.usb,67)==1&&!a.applications(a.ble,67));
    a.receive(wire,2700);assert(a.applications(a.usb,67)==1);
  }
  pass("private power: both target models, real PSA, measured zero/units, request correlation, ACK policy and owner-only delivery");
  {Side a(11,97,127),b(97,11,81);auto r=a.request(201,true);r.packet.decoded.payload.size=2;r.packet.decoded.payload.bytes[0]=0x2a;r.packet.decoded.payload.bytes[1]=0;
    input(a.c,r,100);a.poll(1000);assert(a.radio.frames.size()==1);a.complete(1200);b.receive(a.radio.frames[0],1500);b.poll(2000,902);
    assert(b.radio.frames.empty());replyDiagnostics(b,{1,0,0,1});}
  pass("private power unavailable producer returns no invented measurement or RF response");
  {Side a(11,97,127),b(97,11,81,false,0,3);auto request=a.request(801,true);
    // MeshPacket.decoded is already the outer Data message. Its payload here
    // is only the nested Telemetry LocalStats selector, not a second Data.
    constexpr uint8_t selector[]={0x32,0};request.packet.decoded.payload.size=sizeof(selector);
    memcpy(request.packet.decoded.payload.bytes,selector,sizeof(selector));
    input(a.c,request,100);input(a.bc,request,101);a.poll(1000);assert(a.radio.frames.size()==1);a.complete(1200);
    PkiLedger::Status before,after;assert(b.ledger.status(before)&&b.s.nodes.count()==0);
    const auto writes=b.lm.writes,decrypts=b.backend.decrypts,encrypts=b.backend.encrypts;
    b.receive(a.radio.frames[0],1500);b.poll(2000,902);
    assert(b.s.nodes.count()==0&&b.backend.decrypts==decrypts+1&&b.backend.encrypts==encrypts+1&&b.radio.frames.size()==1);
    assert(b.ledger.status(after)&&after.received==before.received+1&&after.sent==before.sent+1&&b.lm.writes>writes);
    replyDiagnostics(b,{1,1});
    const auto response=b.radio.frames[0];assert(!response[13]&&bench::le32(response.data())==a.local.binding.node&&
      bench::le32(response.data()+4)==b.local.binding.node);
    checkGeneratedLocalStats(b,response,801,false);b.complete(2200);
    a.receive(response,2500);assert(a.applications(a.usb,67)==1&&a.applications(a.ble,67)==1);
    a.receive(response,2600);assert(a.applications(a.usb,67)==1&&a.applications(a.ble,67)==1);
    pass("KNOWN_ONLY-active-pin-without-NodeDB-user-authenticates-localstats-and-returns-private-response-to-both-owners");}
  {Side f(97,11,81,false,0,3);Key unknown(193);const auto clear=localStatsSelector();uint8_t cipher[239]{};size_t size=0;
    assert(f.crypto.encrypt(unknown.scalar,f.local.binding.publicKey,unknown.binding.node,802,0x19374628,
      clear.data(),clear.size(),cipher,sizeof(cipher),size));
    Bytes frame(16+size);put(frame.data(),f.local.binding.node);put(frame.data()+4,unknown.binding.node);
    put(frame.data()+8,802);frame[15]=uint8_t(unknown.binding.node);memcpy(frame.data()+16,cipher,size);
    const auto writes=f.lm.writes,decrypts=f.backend.decrypts,entropy=f.entropy.calls;
    f.receive(frame,1000);f.poll(1500);
    assert(f.lm.writes==writes&&f.backend.decrypts==decrypts&&f.entropy.calls==entropy&&f.radio.frames.empty()&&
      !f.applications(f.usb,67)&&!f.applications(f.ble,67));replyDiagnostics(f,{});
    pass("KNOWN_ONLY-unpinned-source-refuses-before-decrypt-replay-response-or-owner-event");}
  {Side f(97,11,81,true,0,5,true);meshtastic_Data d{};d.portnum=static_cast<meshtastic_PortNum>(256);d.has_bitfield=true;d.payload.size=1;d.payload.bytes[0]=0x7f;
    const auto frame=f.incoming(encoded(d),802);const auto writes=f.lm.writes;f.receive(frame,1000);f.poll(1500);
    assert(f.lm.writes==writes&&f.radio.frames.empty()&&!f.applications(f.usb,256)&&f.runtime.ready(f.local.binding.node));
    f.receive(f.incoming(data("core text"),803),2000);assert(f.applications(f.usb,1)==1&&f.applications(f.ble,1)==1);
    pass("CORE_PORTNUMS_ONLY-private-noncore-payload-policy-blocks-before-replay-publication-without-key-fallback");}
  {
    for (unsigned policy : {0u, 1u}) {
      for (unsigned role : {0u, 1u, 2u}) {
        Side f(97,11,81,true,role,0,true,true,policy);
        assert(!f.s.submitNodeInfo(0,804+role,0xffffffffu,0,false,false,100));
        f.poll(1000);
        assert(f.radio.frames.size()==1);
        const auto &frame=f.radio.frames[0];
        bench::RouteMetadata metadata;
        assert(f.s.codec.decode(frame.data(),frame.size(),metadata));
        assert(metadata.signatureLength==(policy?64:0));
        meshtastic_User user{};
        auto in=pb_istream_from_buffer(metadata.payload,metadata.payloadLength);
        assert(pb_decode(&in,meshtastic_User_fields,&user)&&unsigned(user.role)==role);
        bench::MtSignature gate;
        const auto expected=policy?bench::MtSignature::Result::Verified:bench::MtSignature::Result::Unsigned;
        assert(gate.apply(frame.data(),frame.size(),metadata,1100)==expected);
      }
    }
    pass("current-role-in-compatible-unsigned-and-balanced-verified-NodeInfo");
  }

  {Side a(11,97,127),relay(193,11,81,true,0),c(97,11,81);
    auto request=a.request(401,false);request.packet.hop_limit=2;input(a.c,request,100);a.poll(1000);a.complete(1200);
    assert(a.radio.frames.size()==1);const auto original=a.radio.frames[0];
    const auto writes=relay.lm.writes,enc=relay.backend.encrypts,dec=relay.backend.decrypts;
    relay.receive(original,1500);assert(relay.s.route.pending()==1&&!relay.applications(relay.usb,1));
    relay.poll(10000);assert(relay.radio.frames.size()==1&&relay.s.active());const auto forwarded=relay.radio.frames[0];
    assert(forwarded.size()==original.size()&&!memcmp(forwarded.data(),original.data(),12));
    assert((forwarded[12]&7)==1&&(forwarded[12]>>5)==2&&!forwarded[13]&&forwarded[15]==uint8_t(relay.local.binding.node));
    assert(!memcmp(forwarded.data()+16,original.data()+16,original.size()-16)&&relay.radio.holds(forwarded));
    c.receive(forwarded,10200);assert(c.applications(c.usb,1)==1&&c.applications(c.ble,1)==1);
    relay.complete(11000);assert(relay.s.sent==1&&!relay.s.active()&&!relay.s.route.pending());
    assert(relay.lm.writes==writes&&relay.backend.encrypts==enc&&relay.backend.decrypts==dec);
    replyDiagnostics(relay,{});pass("three-actual-Services-PSA-endpoints-opaque-private-relay-has-no-recipient-key-or-crypto-storage-phone-effects");}

  {Side f(97,11,81);f.radio.busyRemaining=2;f.receive(f.incoming(localStatsSelector(),201),1000);
    const auto writes=f.lm.writes,encrypts=f.backend.encrypts;replyDiagnostics(f,{1,1});
    f.poll(1500);assert(f.radio.frames.empty()&&f.s.endpointControls()==1&&!f.s.faulted());
    f.poll(3000);assert(f.radio.frames.empty()&&f.s.endpointControls()==1&&!f.s.faulted());
    f.poll(5000);assert(f.radio.sendCalls==3&&f.radio.frames.size()==1&&f.s.active());
    const auto wire=f.radio.frames[0];assert(!wire[13]&&!(wire[12]&8)&&f.radio.holds(wire));
    const auto d=decoded(f.decryptForPeer(wire));assert(d.portnum==67&&d.request_id==201);
    f.complete(5200);assert(!f.s.active()&&!f.s.endpointControls()&&f.s.sent==1&&f.lm.writes==writes&&f.backend.encrypts==encrypts);
    replyDiagnostics(f,{1,1});pass("native-CAD-busy-private-LocalStats-control-reuses-one-reservation-and-completes-third-attempt");}
  {Side f(97,11,81);f.radio.busyRemaining=3;f.receive(f.incoming(localStatsSelector(),201),1000);
    const auto writes=f.lm.writes,encrypts=f.backend.encrypts;
    for(uint32_t now:{1500u,3000u,5000u,10000u})f.poll(now);
    assert(f.radio.sendCalls==3&&f.radio.frames.empty()&&!f.s.endpointControls()&&!f.s.active()&&!f.s.faulted());
    assert(f.lm.writes==writes&&f.backend.encrypts==encrypts);replyDiagnostics(f,{1,1});
    pass("native-CAD-busy-generated-control-stops-after-three-attempts-without-reencoding-or-extra-RF");}

  {PrivateReplyDiagnostics zero;assert(sameReplyDiagnostics(zero,{}));uint32_t n=UINT32_MAX-1;
    PrivateReplyDiagnostics ordinary{1,2,3,4,5,6,7};ordinary.add({7,6,5,4,3,2,1});ordinary.add(zero);
    assert(sameReplyDiagnostics(ordinary,{8,8,8,8,8,8,8}));
    PrivateReplyDiagnostics::bump(n);PrivateReplyDiagnostics::bump(n);assert(n==UINT32_MAX);
    assert(PrivateReplyDiagnostics::sum(3,4)==7&&PrivateReplyDiagnostics::sum(UINT32_MAX-1,2)==UINT32_MAX);
    PrivateReplyDiagnostics a{UINT32_MAX-1,UINT32_MAX-1,UINT32_MAX-1,UINT32_MAX-1,UINT32_MAX-1,UINT32_MAX-1,UINT32_MAX-1};
    a.add({2,2,2,2,2,2,2});a.add(zero);
    assert(sameReplyDiagnostics(a,{UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX}));
    pass("private-reply-diagnostics-zero-bump-sum-and-all-field-add-saturate-without-wrap");}
  {Side f(97,11,81);replyDiagnostics(f,{});const auto query=f.incoming(localStatsSelector(),201);f.entropy.fail=true;
    f.receive(query,1000);f.poll(1500);assert(f.radio.frames.empty()&&f.entropy.calls==1);replyDiagnostics(f,{1,0,0,0,0,1});
    const PublicReadOnly snapshot(f);f.clear();f.handshake();replyDiagnostics(f,{1,0,0,0,0,1});snapshot.unchanged();
    for(const auto *host:{&f.usb,&f.ble}){unsigned legacyPrivateLogs=0;for(const auto &out:host->out){
      if(out.which_payload_variant!=meshtastic_FromRadio_log_record_tag||strncmp(out.log_record.message,"private_reply_diag ",19))continue;
      ++legacyPrivateLogs;}
      assert(legacyPrivateLogs==0);}
    f.runtime.freeze();replyDiagnostics(f,{1,0,0,0,0,1});
    pass("private-reply-diagnostics-remain-typed-and-passive-with-no-legacy-handshake-string-on-either-owner");}
  {Side f(97,11,81);for(uint32_t id=10;id<14;++id)f.send(id,true);assert(!f.s.endpointFree());
    f.receive(f.incoming(localStatsSelector(),201),1000);replyDiagnostics(f,{1,0,1});
    assert(f.entropy.calls==4&&f.radio.frames.empty());
    pass("private-reply-diagnostics-full-real-endpoint-counts-no-slot-without-response-nonce");}
  {for(const uint8_t variant:{3,4,5,7,8,9,11})for(bool reverse:{false,true}){
    Side f(reverse?97:11,reverse?11:97,reverse?81:127);auto r=f.request(201,true);r.packet.want_ack=true;
    const auto selector=remoteTelemetry(variant);r.packet.decoded.payload.size=selector.size();memcpy(r.packet.decoded.payload.bytes,selector.data(),selector.size());
    input(f.c,r,100);input(f.bc,r,101);f.poll(1000);assert(f.radio.frames.size()==1);f.complete(1200);
    const auto request=decoded(f.decryptForPeer(f.radio.frames[0]));
    assert(request.portnum==meshtastic_PortNum_TELEMETRY_APP&&request.want_response&&!request.request_id&&
      request.payload.size==2&&request.payload.bytes[0]==uint8_t((variant<<3)|2)&&request.payload.bytes[1]==0);
    assert(!f.cached()&&!f.s.storageStopSafe());
    f.receive(f.ack(201,false,meshtastic_Routing_Error_NO_RESPONSE),1250);f.poll(1251);
    queryResults(f,0,0);noMetrics(f);assert(!f.s.storageStopSafe());
    const uint8_t other=variant==3?4:3;
    f.receive(f.incoming(remoteTelemetryResponse(other,201),5401),1300);f.poll(1301);
    noMetrics(f);queryResults(f,0,0);assert(!f.s.storageStopSafe());
    const auto plain=remoteTelemetryResponse(variant,201),wire=f.incoming(plain,5402);
    f.receive(wire,1500);f.poll(1501);deliveredLocalStats(f,plain,5402);queryResults(f,0,1);assert(!f.cached()&&f.s.storageStopSafe());
    const auto writes=f.lm.writes;f.receive(wire,1600);f.poll(1601);deliveredLocalStats(f,plain,5402);
    assert(f.lm.writes==writes&&!f.cached()&&f.radio.frames.size()==1);
    for(const auto *host:{&f.usb,&f.ble})for(const auto &message:host->out)if(message.which_payload_variant==meshtastic_FromRadio_packet_tag&&
      message.packet.pki_encrypted&&message.packet.decoded.portnum==meshtastic_PortNum_TELEMETRY_APP){
      meshtastic_Telemetry value{};auto in=pb_istream_from_buffer(message.packet.decoded.payload.bytes,message.packet.decoded.payload.size);
      assert(pb_decode(&in,meshtastic_Telemetry_fields,&value)&&!in.bytes_left&&value.which_variant==variant&&value.time==1792660000);}}
    pass("remote-seven-telemetry-variants-real-PSA-normalize-selector-reject-unproved-NAK-match-requested-variant-fanout-once-and-preserve-DeviceMetrics-cache-absence");}
  {for(const uint8_t variant:{3,4,5,7,8,9,11}){Side f(11,97,127);auto r=f.request(201,true);
    const auto selector=remoteTelemetry(variant,1792660000);r.packet.decoded.payload.size=selector.size();memcpy(r.packet.decoded.payload.bytes,selector.data(),selector.size());
    const auto writes=f.lm.writes;input(f.c,r,100);f.poll(1000);
    assert(f.radio.frames.empty()&&!f.entropy.calls&&!f.backend.encrypts&&f.lm.writes==writes&&!f.cached()&&f.s.storageStopSafe());}
    pass("remote-seven-telemetry-selectors-reject-caller-time-before-nonce-encryption-or-radio-send");}
  {for(bool reverse:{false,true}){Side f(reverse?97:11,reverse?11:97,reverse?81:127);
    localStatsQuery(f);f.receive(f.ack(201),1300);f.poll(1301);queryResults(f,1,0);noMetrics(f);
    const auto plain=localStats(201,true),wire=f.incoming(plain,5101);
    f.receive(wire,1500);f.poll(1501);deliveredLocalStats(f,plain,5101);assert(!f.cached()&&f.s.storageStopSafe());
    f.receive(wire,1600);f.receive(f.ack(201),1700);f.poll(1701);
    deliveredLocalStats(f,plain,5101);queryResults(f,1,1);assert(f.radio.frames.size()==1);}
    pass("LocalStats-real-PSA-max105-distinct-RF-ID-request-correlation-both-roles-and-shared-owners");}
  {Side f(11,97,127);localStatsQuery(f);
    f.receive(f.incoming(metrics(201,777),5101),1300);f.poll(1301);noMetrics(f);assert(!f.s.storageStopSafe());
    f.receive(f.incoming(localStats(202),5102),1400);f.poll(1401);noMetrics(f);assert(!f.s.storageStopSafe());
    const auto plain=localStats(201);f.receive(f.incoming(plain,5103),1500);f.poll(1501);
    deliveredLocalStats(f,plain,5103);assert(!f.cached()&&f.s.storageStopSafe());
    pass("LocalStats-pending-request-rejects-DeviceMetrics-and-wrong-Data-request-ID-before-valid-response");}
  {Side f(11,97,127);reliableQuery(f);
    f.receive(f.incoming(localStats(201),5101),1300);f.poll(1301);noMetrics(f);assert(!f.s.storageStopSafe());
    f.receive(f.incoming(metrics(201,777),5102),1500);f.poll(1501);deliveredMetrics(f,777,5102);
    pass("DeviceMetrics-pending-request-rejects-LocalStats-without-consuming-request-or-polluting-cache");}
  {Side f(11,97,127);mt::PeerDeviceMetrics cached;meshtastic_DeviceMetrics prior{};
    prior.has_uptime_seconds=true;prior.uptime_seconds=55;assert(cached.assign(prior));
    assert(f.s.nodes.observeDeviceMetrics(f.peer.binding.node,cached));
    localStatsQuery(f);const auto plain=localStats(201);f.receive(f.incoming(plain,5101),1500);f.poll(1501);
    deliveredLocalStats(f,plain,5101);cachedUptime(f,55);
    pass("LocalStats-authenticated-delivery-preserves-existing-DeviceMetrics-cache");}
  {for(unsigned mode=0;mode<3;++mode){Side f(11,97,127);localStatsQuery(f,mode!=1,mode!=0);
    if(mode==2)f.c.resetStream();
    const auto plain=localStats(201);f.receive(f.incoming(plain,5101),1500);f.poll(1501);
    deliveredLocalStats(f,plain,5101,mode==0,mode!=0);assert(!f.cached());}
    pass("LocalStats-only-requesting-USB-BLE-or-remaining-shared-owner-delivers");}
  {Side f(11,97,127);localStatsQuery(f);
    auto broken=f.incoming(localStats(201),5101);broken.back()^=1;f.receive(broken,1300);f.poll(1301);noMetrics(f);
    auto malformed=decoded(localStats(201));malformed.payload.bytes[malformed.payload.size++]=0;
    f.receive(f.incoming(encoded(malformed),5102),1400);f.poll(1401);noMetrics(f);
    f.receive(f.ack(201),1500);f.poll(90101);queryResults(f,1,1,3);assert(f.s.storageStopSafe());
    f.receive(f.incoming(localStats(201),5103),90200);f.poll(90201);noMetrics(f);
    pass("LocalStats-corrupt-and-malformed-replies-fail-before-delivery-and-expired-query-cannot-reopen");}
  {for(bool reverse:{false,true})for(bool ack:{false,true})for(uint32_t uptime:{0u,123u,UINT32_MAX}){
    Side a(reverse?97:11,reverse?11:97,reverse?81:127),b(reverse?11:97,reverse?97:11,reverse?127:81);
    b.uptime=uptime;b.s.measuredUptime(uptime);generatedLocalStats(a,b,ack);assert(!b.cached());
    if(ack){b.receive(b.ack(bench::le32(b.radio.frames[0].data()+8)),2300);b.poll(2301);}
    assert(b.s.storageStopSafe());}
    pass("LocalStats-private-producer-canonical-zero-max-uptime-both-roles-and-ACK-modes-without-invented-counters");}
  {for(bool reverse:{false,true}){Side a(reverse?97:11,reverse?11:97,reverse?81:127),b(reverse?11:97,reverse?97:11,reverse?127:81);
    generatedLocalStats(a,b,true);const auto plain=b.decryptForPeer(b.radio.frames[0]);
    a.receive(b.radio.frames[0],2300);a.poll(2301);deliveredLocalStats(a,plain,bench::le32(b.radio.frames[0].data()+8));
    assert(!a.cached()&&!b.cached());a.poll(3000);assert(a.radio.frames.size()==2);a.complete(3200);
    b.receive(a.radio.frames.back(),3300);b.poll(3301);assert(a.s.storageStopSafe()&&b.s.storageStopSafe());}
    pass("LocalStats-private-producer-real-PSA-request-response-delivers-to-both-requesting-owners-only");}
  {mt::Endpoint e;uint8_t out[40]{};size_t n=123;
    assert(!e.privateTelemetryReply(123,UINT32_MAX,1000,out,sizeof(out),n,true)&&!n);
    e.measuredUptime(UINT32_MAX);e.measuredAirUtilTx(zt::Observed<float>{100.0f});e.measuredChannelUtilization(zt::Observed<float>{100.0f});
    assert(!e.privateTelemetryReply(123,UINT32_MAX,1000,out,28,n,true)&&!n);
    assert(e.privateTelemetryReply(123,UINT32_MAX,1000,out,sizeof(out),n,true)&&n==29);
    auto d=decoded(Bytes(out,out+n));meshtastic_Telemetry t{};auto in=pb_istream_from_buffer(d.payload.bytes,d.payload.size);
    assert(pb_decode(&in,meshtastic_Telemetry_fields,&t)&&t.which_variant==meshtastic_Telemetry_local_stats_tag&&
      t.variant.local_stats.uptime_seconds==UINT32_MAX&&t.variant.local_stats.air_util_tx==100&&t.variant.local_stats.channel_utilization==100);
    assert(!e.privateTelemetryReply(123,UINT32_MAX-1,2000,out,sizeof(out),n));
    assert(e.privateTelemetryReply(123,UINT32_MAX-2,7000,out,sizeof(out),n));d=decoded(Bytes(out,out+n));t={};in=pb_istream_from_buffer(d.payload.bytes,d.payload.size);
    assert(pb_decode(&in,meshtastic_Telemetry_fields,&t)&&t.which_variant==meshtastic_Telemetry_device_metrics_tag);
    pass("LocalStats-private-producer-capacity-failure-does-not-consume-rate-record-and-default-API-retains-DeviceMetrics");}
  {for(bool disarmed:{false,true}){Side f(97,11,81);if(disarmed)assert(f.s.armEndpoint(false));else f.s.measuredUptime(uint64_t(UINT32_MAX)+1);
    const auto q=f.incoming(localStatsSelector(),201);const auto nonces=f.entropy.calls,encrypts=f.backend.encrypts;
    f.receive(q,1000);f.poll(1500);assert(f.radio.frames.empty()&&f.entropy.calls==nonces&&f.backend.encrypts==encrypts);
    replyDiagnostics(f,disarmed?PrivateReplyDiagnostics{}:PrivateReplyDiagnostics{1,0,0,1});
    if(disarmed)assert(f.s.armEndpoint(true));else f.s.measuredUptime(123);
    f.receive(q,7000);f.poll(7500);assert(f.radio.frames.empty()&&f.entropy.calls==nonces&&f.backend.encrypts==encrypts);
    replyDiagnostics(f,disarmed?PrivateReplyDiagnostics{}:PrivateReplyDiagnostics{1,0,0,1});}
    pass("LocalStats-private-producer-unknown-uptime-or-disarm-never-generates-and-seen-query-cannot-reopen");}
  {Side f(97,11,81);const auto selector=localStatsSelector();const auto before=f.lm.values;
    auto malformed=decoded(selector);malformed.payload.bytes[malformed.payload.size++]=0;
    const auto m=f.incoming(encoded(malformed),201),foreign=f.incoming(selector,201,Key(193).scalar);auto bad=f.incoming(selector,201);bad.back()^=1;const auto encrypts=f.backend.encrypts;
    const Bytes *cases[]={&m,&foreign,&bad};for(const auto *q:cases){f.receive(*q,1000);f.poll(1500);assert(f.radio.frames.empty()&&f.lm.values==before&&!f.entropy.calls&&f.backend.encrypts==encrypts);replyDiagnostics(f,{});}
    f.receive(f.incoming(selector,201),2000);f.poll(2500,902);assert(f.radio.frames.size()==1);checkGeneratedLocalStats(f,f.radio.frames[0],201,false);f.complete(2700);
    pass("LocalStats-private-producer-malformed-selector-foreign-key-and-bad-tag-cannot-poison-valid-request");}
  {Side f(97,11,81);const auto first=f.incoming(localStatsSelector(),201);
    constexpr uint8_t metricBytes[]={8,67,18,2,18,0,24,1,72,2};const Bytes metricQuery(metricBytes,metricBytes+sizeof(metricBytes));
    const auto second=f.incoming(localStatsSelector(),202),metric=f.incoming(metricQuery,203),later=f.incoming(localStatsSelector(),204);
    f.receive(first,1000);f.poll(1500,902);assert(f.radio.frames.size()==1);f.complete(1700);
    replyDiagnostics(f,{1,1});
    const auto nonces=f.entropy.calls,encrypts=f.backend.encrypts;
    f.receive(first,1800);f.receive(second,2500);f.receive(metric,3000);f.poll(3500);
    assert(f.radio.frames.size()==1&&f.entropy.calls==nonces&&f.backend.encrypts==encrypts);
    replyDiagnostics(f,{3,1,0,2});
    f.receive(later,7000);f.poll(7500,903);assert(f.radio.frames.size()==2);checkGeneratedLocalStats(f,f.radio.frames[1],204,false,1);f.complete(7700);
    f.receive(first,8000);f.poll(8500);assert(f.radio.frames.size()==2);
    replyDiagnostics(f,{4,2,0,2});
    pass("LocalStats-private-producer-replay-and-shared-DeviceMetrics-peer-cooldown-prevent-response-amplification");}
  {Side f(97,11,81);f.receive(f.incoming(localStatsSelector(),201),1000);f.poll(1500,902);
    assert(f.radio.frames.size()==1);checkGeneratedLocalStats(f,f.radio.frames[0],201,false,0);f.complete(1700);
    const Key ignored(193);assert(f.s.nodes.edit(ignored.binding.node,f.local.binding.node,mt::NodeCache::Edit::Ignore)==1);
    assert(f.s.nodes.count()==2&&f.s.nodes.ignored(ignored.binding.node));
    f.receive(f.incoming(localStatsSelector(),202),7000);f.poll(7500,903);
    assert(f.radio.frames.size()==2);checkGeneratedLocalStats(f,f.radio.frames[1],202,false,1);
    const auto d=decoded(f.decryptForPeer(f.radio.frames[1]));meshtastic_Telemetry t{};auto in=pb_istream_from_buffer(d.payload.bytes,d.payload.size);
    assert(pb_decode(&in,meshtastic_Telemetry_fields,&t)&&t.variant.local_stats.num_packets_tx==1&&t.variant.local_stats.num_total_nodes==3);
    f.complete(7700);assert(f.s.storageStopSafe());
    pass("LocalStats-private-producer-counts-only-completed-TX-and-local-plus-stored-peers-including-ignored-placeholder");}
  {Side a(11,97,127),b(97,11,81);b.radio.air=2200;generatedLocalStats(a,b,true);const auto wire=b.radio.frames[0];
    const auto nonces=b.entropy.calls,encrypts=b.backend.encrypts,writes=b.lm.writes;
    // Completion at 2200 sets the ACK deadline to 22311 (15 s + 2*airtime
    // + the fixture's 711 ms random term). Each retry tick then applies its
    // own 250 ms queue backoff; use random=0 and poll that exact due boundary.
    b.poll(23000,0);b.poll(23250,0);assert(b.radio.frames.size()==2&&b.radio.frames[1]==wire);b.complete(25450);
    b.poll(46000,0);b.poll(46250,0);assert(b.radio.frames.size()==3&&b.radio.frames[2]==wire);b.complete(48450);
    b.poll(69000,0);b.poll(69250,0);assert(b.radio.frames.size()==4&&b.radio.frames[3]==wire);b.complete(71450);
    b.poll(92000,0);b.poll(92250,0);assert(b.radio.frames.size()==5&&b.radio.frames[4]==wire);b.complete(94450);
    mt::RadioTiming::Deadline timing;mt::PacketLifetime::Bounds lifetime;
    assert(mt::RadioTiming::resolve(b.radio.air,timing)&&
      mt::PacketLifetime::resolve(timing.hostMs,16000+2*b.radio.air,lifetime));
    const uint32_t absoluteExpiry=1500+lifetime.pendingMs;
    b.poll(absoluteExpiry-1);assert(b.radio.frames.size()==5&&b.radio.sendCalls==5&&b.s.endpointControls()==1&&!b.s.faulted());
    b.poll(absoluteExpiry);assert(b.radio.frames.size()==5&&b.radio.sendCalls==5&&b.s.storageStopSafe()&&!b.s.faulted());
    assert(b.entropy.calls==nonces&&b.backend.encrypts==encrypts&&b.lm.writes==writes);
    replyDiagnostics(b,{1,1});
    pass("LocalStats-private-producer-five reliable attempts reuse ciphertext and one durable nonce until airtime-derived absolute expiry");}
  {for(bool freeze:{false,true}){Side a(11,97,127),b(97,11,81);generatedLocalStats(a,b,true,false);const auto wire=b.radio.frames[0];
    assert(b.radio.holds(wire));b.receive(b.ack(bench::le32(wire.data()+8)),2100);assert(!b.s.storageStopSafe()&&b.radio.holds(wire));
    if(freeze){b.runtime.freeze();}b.complete(2200);assert(b.s.storageStopSafe());b.poll(50000);assert(b.radio.frames.size()==1);}
    pass("LocalStats-private-producer-early-proof-and-freeze-retain-active-native-frame-until-completion");}
  {for(bool reverse:{false,true}){Side f(reverse?97:11,reverse?11:97,reverse?81:127);reliableQuery(f);
    noMetrics(f);queryResults(f,0,0);assert(!f.s.storageStopSafe());
    const auto writes=f.lm.writes,nonces=f.entropy.calls,encrypts=f.backend.encrypts;
    const auto proof=f.ack(201);assert(bench::le32(proof.data()+8)==1201);
    f.receive(proof,1300);f.poll(1301);queryResults(f,1,0);noMetrics(f);assert(!f.s.storageStopSafe());
    f.receive(proof,1350);f.poll(1351);queryResults(f,1,0);
    // Rejoining the same pending ciphertext also proves its session survived.
    auto repeat=f.request(201,true);repeat.packet.want_ack=true;input(f.c,repeat,1400);input(f.bc,repeat,1401);
    assert(f.lm.writes==writes&&f.entropy.calls==nonces&&f.backend.encrypts==encrypts&&f.radio.frames.size()==1);
    const auto response=f.incoming(metrics(201,456),5001);assert(bench::le32(response.data()+8)==5001);
    f.receive(response,1500);f.poll(1501);deliveredMetrics(f,456);queryResults(f,1,1);assert(f.s.storageStopSafe());
    f.receive(response,1600);f.receive(proof,1700);f.poll(1701);
    deliveredMetrics(f,456);queryResults(f,1,1);assert(f.radio.frames.size()==1&&f.s.storageStopSafe());}
    pass("Companion-private-query-proof-then-distinct-RF-response-retains-session-and-delivers-once-both-owners-and-roles");}
  {Side f(11,97,127);reliableQuery(f);const auto response=f.incoming(metrics(201,457),5001),proof=f.ack(201);
    f.receive(response,1300);f.poll(1301);deliveredMetrics(f,457);queryResults(f,0,1);assert(f.s.storageStopSafe());
    f.receive(proof,1400);f.receive(response,1500);f.receive(proof,1600);f.poll(1601);
    deliveredMetrics(f,457);queryResults(f,0,1);assert(f.s.storageStopSafe()&&f.radio.frames.size()==1);
    pass("Companion-private-query-response-before-proof-does-not-reopen-or-emit-late-proof-results");}
  {Side f(11,97,127);reliableQuery(f,false);const auto frame=f.radio.frames[0],proof=f.ack(201);
    assert(f.radio.holds(frame));f.receive(proof,1300);f.poll(1301);queryResults(f,0,0);noMetrics(f);
    assert(f.s.active()&&!f.s.storageStopSafe()&&f.radio.holds(frame));
    f.receive(proof,1350);f.poll(1351);queryResults(f,0,0);assert(f.radio.holds(frame));
    f.complete(1400);queryResults(f,1,0);assert(!f.s.active()&&!f.s.storageStopSafe());
    f.receive(f.incoming(metrics(201,458),5001),1500);f.poll(1501);
    deliveredMetrics(f,458);queryResults(f,1,1);assert(f.s.storageStopSafe()&&f.radio.frames.size()==1);
    pass("Companion-private-query-active-proof-defers-owner-notification-retains-native-frame-and-allows-later-response");}
  {Side f(11,97,127);reliableQuery(f);
    const Bytes invalid[]={f.ack(201,false),f.ack(202),f.ack(201,true,0,Key(193).binding.node),f.ack(201,true,0,0,true)};
    uint32_t now=1300;for(const auto &proof:invalid){f.receive(proof,now);f.poll(now+1);now+=100;
      queryResults(f,0,0);noMetrics(f);assert(!f.s.storageStopSafe());}
    f.receive(f.ack(201),1800);f.poll(1801);queryResults(f,1,0);assert(!f.s.storageStopSafe());
    // A peer-authenticated envelope is not enough: application request_id wins.
    f.receive(f.incoming(metrics(5001,999),5000),1900);f.poll(1901);noMetrics(f);assert(!f.s.storageStopSafe());
    f.receive(f.incoming(metrics(201,459),5001),2000);f.poll(2001);
    deliveredMetrics(f,459);queryResults(f,1,1);assert(f.s.storageStopSafe()&&f.radio.frames.size()==1);
    pass("Companion-private-query-unsigned-wrong-peer-ID-corrupt-proofs-and-wrong-application-request-cannot-complete");}
  {Side f(11,97,127);reliableQuery(f);f.receive(f.ack(201,true,meshtastic_Routing_Error_BAD_REQUEST),1300);f.poll(1301);
    queryResults(f,1,0,0,meshtastic_Routing_Error_BAD_REQUEST);noMetrics(f);assert(f.s.storageStopSafe());
    f.receive(f.incoming(metrics(201,460),5001),1400);f.receive(f.ack(201),1500);f.poll(1501);
    queryResults(f,1,0,0,meshtastic_Routing_Error_BAD_REQUEST);noMetrics(f);assert(f.s.storageStopSafe()&&f.radio.frames.size()==1);
    pass("Companion-private-query-authenticated-negative-Routing-reason-is-terminal-without-application-delivery");}
  {for(bool earlyProof:{false,true}){Side f(11,97,127);reliableQuery(f);
    if(earlyProof){f.receive(f.ack(201),1300);f.poll(1301);}queryResults(f,earlyProof?1:0,0);
    f.receive(f.ack(201),90100);f.poll(90101);queryResults(f,earlyProof?1:0,1,3);
    noMetrics(f);assert(f.s.storageStopSafe());
    f.receive(f.incoming(metrics(201,461),5001),90102);f.receive(f.ack(201),90200);f.poll(90201);
    noMetrics(f);queryResults(f,earlyProof?1:0,1,3);assert(f.s.storageStopSafe()&&f.radio.frames.size()==1);}
    pass("Companion-private-query-waiting-absolute-expiry-rejects-boundary-proof-and-late-response-with-or-without-earlier-proof");}
  {for(bool earlyProof:{false,true}){Side f(11,97,127);reliableQuery(f,false);const auto frame=f.radio.frames[0];
    if(earlyProof){f.receive(f.ack(201),1300);f.poll(1301);}queryResults(f,0,0);
    // RX ticks the real endpoint while the native send still borrows its frame.
    f.receive(f.ack(201),90100);queryResults(f,0,0);noMetrics(f);
    assert(f.s.active()&&!f.s.storageStopSafe()&&f.radio.holds(frame));
    f.receive(f.incoming(metrics(201,462),5001),90101);noMetrics(f);assert(f.radio.holds(frame));
    f.complete(90102);queryResults(f,0,1,3);assert(!f.s.active()&&f.s.storageStopSafe()&&!f.s.faulted());
    f.receive(f.incoming(metrics(201,462),5002),90103);f.poll(90104);
    noMetrics(f);queryResults(f,0,1,3);assert(f.radio.frames.size()==1&&f.s.storageStopSafe());}
    pass("Companion-private-query-active-absolute-expiry-retains-native-buffer-and-suppresses-proof-success-until-failed-completion");}
  {for(bool active:{false,true}){Side f(11,97,127);reliableQuery(f,!active);const auto frame=f.radio.frames[0];
    f.receive(f.ack(201),1300);f.poll(1301);queryResults(f,active?0:1,0);
    const auto writes=f.lm.writes;assert(!f.s.revokePrivatePeer(f.peer.binding.node)&&f.lm.writes==writes);
    f.runtime.freeze();f.poll(1350);noMetrics(f);
    if(active){assert(f.s.active()&&!f.s.storageStopSafe()&&f.radio.holds(frame));f.complete(1400);}
    assert(f.s.storageStopSafe());queryResults(f,active?0:1,1,4);
    f.receive(f.incoming(metrics(201,463),5001),1500);f.receive(f.ack(201),1600);f.poll(1601);
    noMetrics(f);queryResults(f,active?0:1,1,4);assert(f.s.storageStopSafe()&&f.radio.frames.size()==1);}
    Side done(11,97,127);reliableQuery(done);done.receive(done.incoming(metrics(201,464),5001),1300);done.poll(1301);
    deliveredMetrics(done,464);assert(done.s.storageStopSafe()&&done.s.revokePrivatePeer(done.peer.binding.node));
    done.receive(done.incoming(metrics(201,999),5002),1400);done.receive(done.ack(201),1500);done.poll(1501);
    deliveredMetrics(done,464);queryResults(done,0,1);assert(!done.runtime.ready(done.local.binding.node)&&done.radio.frames.size()==1);
    pass("Companion-private-query-freeze-cancels-waiting-or-active-phase-and-Service-revoke-requires-real-quiescence");}
  companionPositionPkiRefusal();
  {auto q=positionQuery();assert(PrivateApplication::allowed(q.data(),q.size()));
    for(size_t i=0;i<q.size();++i){auto bad=q;bad[i]^=1;assert(!PrivateApplication::allowed(bad.data(),bad.size()));}
    auto shortQuery=q;shortQuery.pop_back();assert(!PrivateApplication::allowed(shortQuery.data(),shortQuery.size()));
    q.push_back(0);assert(!PrivateApplication::allowed(q.data(),q.size()));
    Side f(11,97,127);configurePrivatePosition(f);uint8_t out[40]{};size_t n=0;
    assert(mt::PositionQuery::fixedResponse(f.s.config,201,out,sizeof(out),n)&&n<=37);
    assert(PrivateApplication::allowed(out,n));
    auto response=decoded(Bytes(out,out+n));response.request_id=0;auto bad=encoded(response);assert(!PrivateApplication::allowed(bad.data(),bad.size()));
    response=decoded(Bytes(out,out+n));response.dest=99;bad=encoded(response);assert(!PrivateApplication::allowed(bad.data(),bad.size()));
    bad=Bytes(out,out+n);bad.insert(bad.end(),{8,3});assert(!PrivateApplication::allowed(bad.data(),bad.size()));
    bad=positionResponse(201,900000001);assert(!PrivateApplication::allowed(bad.data(),bad.size()));
    const auto text=data("unchanged private text"),m=metrics(201);
    assert(PrivateApplication::allowed(text.data(),text.size())&&PrivateApplication::allowed(m.data(),m.size()));
    pass("private-Position-admission-exact-query-or-canonical-response-preserves-text-metrics-and-rejects-extra-metadata");}
  {for(bool reverse:{false,true})for(bool ack:{false,true}){
    Side f(reverse?97:11,reverse?11:97,reverse?81:127);const auto config=f.s.config;const auto identity=f.im.values;
    requestPosition(f,ack);assert(!f.s.storageStopSafe());
    if(ack){f.receive(f.ack(201),1300);f.poll(1301);queryResults(f,1,0);assert(!f.s.storageStopSafe());}
    const auto plain=positionResponse(201),wire=f.incoming(plain,5201);
    f.receive(wire,1500);f.poll(1501);deliveredPosition(f,plain,5201);queryResults(f,ack?1:0,1);
    f.receive(wire,1600);f.receive(f.ack(201),1700);f.poll(1701);deliveredPosition(f,plain,5201);
    assert(f.s.storageStopSafe()&&f.radio.frames.size()==1&&f.im.values==identity&&!memcmp(config.bytes,f.s.config.bytes,sizeof(config.bytes)));}
    pass("private-Position-requester-exact-empty-query-ACK-phase-response-once-both-roles-and-no-local-position-mutation");}
  {for(bool reverse:{false,true}){Side f(reverse?97:11,reverse?11:97,reverse?81:127);
    assert(f.entry()&&!f.entry()->position.present());const auto config=f.s.config;const bool dirty=f.s.nodes.dirty();
    const Bytes durable(f.entry()->bytes,f.entry()->bytes+mt::NodeCache::Entry::Size);const auto identity=f.im.values;
    requestPosition(f);const auto plain=positionResponse(201),wire=f.incoming(plain,5301);
    f.receive(wire,1500);f.poll(1501);deliveredPosition(f,plain,5301);cachedPeerPosition(f,plain);
    const auto replayWrites=f.lm.writes;
    f.c.receive(wire.data(),wire.size(),-71,3,1550,mt::Companion::WorkspaceProof{true});f.bc.receive(wire.data(),wire.size(),-71,3,1550,mt::Companion::WorkspaceProof{true});
    f.receive(wire,1600);f.poll(1601);deliveredPosition(f,plain,5301);cachedPeerPosition(f,plain);
    assert(f.lm.writes==replayWrites&&f.s.nodes.count()==1&&f.s.nodes.dirty()==dirty&&
      !memcmp(durable.data(),f.entry()->bytes,durable.size())&&!memcmp(config.bytes,f.s.config.bytes,sizeof(config.bytes))&&f.im.values==identity);
    f.clear();f.handshake();cachedPeerPosition(f,plain);thinPeer(f.usb,f.peer.binding.node);thinPeer(f.ble,f.peer.binding.node);
    assert(!memcmp(config.bytes,f.s.config.bytes,sizeof(config.bytes)));}
    pass("private-Position-known-peer-cache-retains-full-position-and-handshake-emits-thin-peer");}
  {for(unsigned mode=0;mode<4;++mode){Side f(11,97,127);const auto config=f.s.config;
    assert(f.entry()&&!f.entry()->position.present());if(mode)requestPosition(f);
    auto plain=positionResponse(mode==1?202:201);auto wire=f.incoming(plain,5301);
    if(mode==2)put(wire.data()+4,Key(193).binding.node);
    if(mode==3){auto d=decoded(plain);d.payload.bytes[d.payload.size++]=0;wire=f.incoming(encoded(d),5301);}
    f.receive(wire,1500);f.poll(1501);noPosition(f);assert(!f.entry()->position.present()&&!memcmp(config.bytes,f.s.config.bytes,sizeof(config.bytes)));
    f.handshake();for(const auto *host:{&f.usb,&f.ble}){bool found=false;
      for(const auto &r:host->out)if(r.which_payload_variant==meshtastic_FromRadio_node_info_tag&&r.node_info.num==f.peer.binding.node){assert(!r.node_info.has_position);found=true;}
      assert(found);}}
    pass("private-Position-unsolicited-wrong-request-foreign-peer-and-malformed-replies-cannot-populate-cache-or-handshake-position");}
  {Side f(11,97,127,false);requestPosition(f);const auto plain=positionResponse(201);
    f.receive(f.incoming(plain,5301),1500);f.poll(1501);deliveredPosition(f,plain,5301);
    assert(!f.entry()&&!f.s.nodes.count());f.handshake();for(const auto *host:{&f.usb,&f.ble})
      for(const auto &r:host->out)assert(r.which_payload_variant!=meshtastic_FromRadio_node_info_tag||r.node_info.num!=f.peer.binding.node);
    pass("private-Position-correlated-unknown-peer-delivers-without-allocating-NodeDB-cache-entry");}
  {for(unsigned mode=0;mode<3;++mode){Side f(11,97,127);requestPosition(f,true,mode!=1,mode!=0);
    if(mode==2)f.c.resetStream();
    const auto plain=positionResponse(201);
    f.receive(f.incoming(plain,5201),1500);f.poll(1501);deliveredPosition(f,plain,5201,mode==0,mode!=0);assert(f.s.storageStopSafe());}
    pass("private-Position-response-delivers-only-requesting-or-remaining-USB-BLE-owner");}
  {Side f(11,97,127);requestPosition(f);const auto plain=positionResponse(201);
    f.receive(f.ack(201,false,meshtastic_Routing_Error_NO_RESPONSE),1300);f.poll(1301);queryResults(f,0,0);assert(!f.s.storageStopSafe());
    f.receive(f.incoming(metrics(201),5201),1400);f.poll(1401);noPosition(f);queryResults(f,0,0);
    f.receive(f.incoming(positionResponse(202),5202),1500);f.poll(1501);noPosition(f);queryResults(f,0,0);
    f.receive(f.incoming(plain,5202),1600);f.poll(1601);noPosition(f); // Wrong-request authenticated RF ID is durably burned.
    f.receive(f.incoming(plain,5203),1700);f.poll(1701);deliveredPosition(f,plain,5203);queryResults(f,0,1);
    pass("private-Position-rejects-unproved-public-NAK-wrong-application-request-and-reused-RF-ID-without-consuming-valid-query");}
  {Side f(11,97,127);requestPosition(f);const auto plain=positionResponse(201);auto broken=f.incoming(plain,5201);broken.back()^=1;
    const auto writes=f.lm.writes;f.receive(broken,1300);f.receive(f.incoming(plain,5202,Key(193).scalar),1400);
    auto wrong=f.incoming(plain,5203);put(wrong.data()+4,Key(193).binding.node);f.receive(wrong,1500);
    auto malformed=decoded(plain);malformed.payload.bytes[malformed.payload.size++]=0;f.receive(f.incoming(encoded(malformed),5204),1600);
    f.poll(1601);noPosition(f);queryResults(f,0,0);assert(f.lm.writes==writes&&!f.s.storageStopSafe());
    f.receive(f.incoming(plain,5201),1700);f.poll(1701);deliveredPosition(f,plain,5201);
    pass("private-Position-bad-AEAD-foreign-key-source-and-malformed-payload-do-not-commit-replay-or-hide-valid-response");}
  {Side f(11,97,127);const auto plain=positionResponse(201);f.receive(f.incoming(plain,5201),90);noPosition(f);
    submitPrivatePosition(f,true,true,true);
    f.receive(f.incoming(plain,5202),150);noPosition(f);f.poll(1000);f.complete(1200);
    f.receive(f.incoming(plain,5202),1300);noPosition(f); // Pre-transmit reply is seen, never re-opened.
    f.receive(f.incoming(plain,5203),1400);f.poll(1401);deliveredPosition(f,plain,5203);
    pass("private-Position-unsolicited-and-pre-transmit-replies-never-gain-later-delivery-approval");}
  {Side f(11,97,127);requestPosition(f,true,true,true,false);const auto frame=f.radio.frames[0];
    f.receive(f.ack(201),1300);f.poll(1301);queryResults(f,0,0);assert(f.radio.holds(frame)&&!f.s.storageStopSafe());
    f.complete(1400);queryResults(f,1,0);assert(!f.s.storageStopSafe());
    const auto plain=positionResponse(201);f.receive(f.incoming(plain,5201),1500);f.poll(1501);deliveredPosition(f,plain,5201);queryResults(f,1,1);
    pass("private-Position-early-ACK-proof-retains-native-buffer-and-session-until-later-application-response");}
  {for(bool active:{false,true})for(bool proof:{false,true}){Side f(11,97,127);requestPosition(f,true,true,true,!active);
    if(proof){f.receive(f.ack(201),1300);f.poll(1301);}queryResults(f,!active&&proof?1:0,0);
    const auto frame=f.radio.frames[0];f.receive(f.incoming(positionResponse(201),5201),90100);
    if(active){assert(f.radio.holds(frame)&&!f.s.storageStopSafe());f.complete(90101);}else f.poll(90101);
    noPosition(f);queryResults(f,!active&&proof?1:0,1,3);assert(f.s.storageStopSafe());
    f.receive(f.incoming(positionResponse(201),5202),90200);f.poll(90201);noPosition(f);}
    pass("private-Position-active-or-waiting-absolute-expiry-rejects-boundary-and-late-responses-with-or-without-proof");}
  {for(bool active:{false,true}){Side f(11,97,127);requestPosition(f,true,true,true,!active);const auto frame=f.radio.frames[0];
    f.receive(f.ack(201),1300);f.poll(1301);f.runtime.freeze();f.poll(1350);
    if(active){assert(f.radio.holds(frame)&&!f.s.storageStopSafe());f.complete(1400);}
    noPosition(f);queryResults(f,active?0:1,1,4);assert(f.s.storageStopSafe());
    f.receive(f.incoming(positionResponse(201),5201),1500);f.poll(1501);noPosition(f);}
    pass("private-Position-authority-freeze-cancels-application-phase-with-native-lifetime-preserved");}
  {Side f(11,97,127);requestPosition(f);f.receive(f.ack(201,true,meshtastic_Routing_Error_BAD_REQUEST),1300);f.poll(1301);
    queryResults(f,1,0,0,meshtastic_Routing_Error_BAD_REQUEST);assert(f.s.storageStopSafe());
    f.receive(f.incoming(positionResponse(201),5201),1400);f.poll(1401);noPosition(f);
    pass("private-Position-authenticated-negative-proof-is-terminal-without-response-delivery");}
  {for(bool reverse:{false,true})for(bool ack:{false,true}){
    Side a(reverse?97:11,reverse?11:97,reverse?81:127),b(reverse?11:97,reverse?97:11,reverse?127:81);
    configurePrivatePosition(b);const auto cfg=a.s.config;requestPosition(a,ack);
    b.receive(a.radio.frames[0],1500);b.poll(2000,902);assert(b.radio.frames.size()==1);
    const auto response=b.radio.frames[0],plain=b.decryptForPeer(response);checkPrivatePosition(b,response,201,ack);b.complete(2200);
    a.receive(response,2300);a.poll(2301);deliveredPosition(a,plain,bench::le32(response.data()+8));
    assert(!memcmp(cfg.bytes,a.s.config.bytes,sizeof(cfg.bytes)));
    if(ack){a.poll(3000);assert(a.radio.frames.size()==2);a.complete(3200);b.receive(a.radio.frames.back(),3300);b.poll(3301);}
    assert(a.s.storageStopSafe()&&b.s.storageStopSafe());}
    pass("private-Position-actual-two-Service-PSA-query-consented-responder-and-requester-complete-both-roles-and-ACK-modes");}
  {for(bool fixed:{false,true})for(bool ack:{false,true}){Side f(97,11,81);if(fixed)configurePrivatePosition(f,false);
    const auto wire=privatePositionRequest(f,201,ack);const auto nonces=f.entropy.calls,encrypts=f.backend.encrypts;
    f.receive(wire,1000);f.poll(1500);f.receive(wire,1700);f.poll(2000);
    assert(f.radio.frames.empty()&&f.entropy.calls==nonces&&f.backend.encrypts==encrypts&&f.s.storageStopSafe());}
    pass("authenticated-private-Position-query-discloses-nothing-without-explicit-fixed-location-consent");}
  {for(bool reverse:{false,true})for(bool ack:{false,true}){Side f(reverse?97:11,reverse?11:97,reverse?81:127);
    configurePrivatePosition(f);f.receive(privatePositionRequest(f,201,ack),1000);f.poll(1500,902);
    assert(f.radio.frames.size()==1);const auto wire=f.radio.frames[0];checkPrivatePosition(f,wire,201,ack);
    f.complete(1700);assert(f.s.storageStopSafe()==!ack);
    if(ack){f.receive(f.ack(bench::le32(wire.data()+8)),1800);f.poll(1801);assert(f.s.storageStopSafe());}
    f.poll(40000);assert(f.radio.frames.size()==1);}
    pass("shared-private-Position-response-is-encrypted-correlated-quantized-and-inherits-RF-ACK-both-roles");}
  {Side f(97,11,81);configurePrivatePosition(f);const auto first=privatePositionRequest(f,201,false);
    f.receive(first,1000);f.poll(1500,902);assert(f.radio.frames.size()==1);f.complete(1700);
    f.receive(first,1800);f.poll(2200);assert(f.radio.frames.size()==1);
    f.receive(privatePositionRequest(f,202,false),2500);f.poll(3000);assert(f.radio.frames.size()==1);
    f.receive(privatePositionRequest(f,203,false),7000);f.poll(7500,903);assert(f.radio.frames.size()==1);
    f.receive(privatePositionRequest(f,204,false),180999);f.poll(180999,904);assert(f.radio.frames.size()==1);
    f.receive(privatePositionRequest(f,205,false),181000);f.poll(181500,905);assert(f.radio.frames.size()==2);
    checkPrivatePosition(f,f.radio.frames[1],205,false);f.complete(181700);
    f.receive(first,182000);f.poll(182500);assert(f.radio.frames.size()==2);
    pass("private Position rejects ciphertext replay and enforces the global three-minute reply boundary");}
  {Side f(97,11,81);configurePrivatePosition(f);mt::Endpoint e;e.positionConfig(f.s.config);
    uint8_t out[40]{};size_t n=0;assert(e.privatePositionReply(f.peer.binding.node,201,1000,out,sizeof(out),n));
    bench::Frame wire{};wire.len=29;put(wire.bytes,f.peer.binding.node);put(wire.bytes+4,f.local.binding.node);put(wire.bytes+8,9001);wire.bytes[12]=8;
    assert(!e.submitPrivateControl(wire,201,1000,true));auto cfg=f.s.config;cfg.setPositionSharing(false);e.positionConfig(cfg);
    assert(e.idle()&&!e.next(1500));cfg.setPositionSharing(true);e.positionConfig(cfg);
    assert(!e.privatePositionReply(f.peer.binding.node,201,2000,out,sizeof(out),n));
    pass("private-position-queue-consent-revocation-cancels-unsent-and-retains-rate-replay-history");}
  {Side f(97,11,81);configurePrivatePosition(f);f.receive(privatePositionRequest(f,201,true),1000);f.poll(1500,902);
    assert(f.radio.frames.size()==1);const auto actual=f.radio.frames[0];checkPrivatePosition(f,actual,201,true);
    mt::Endpoint e;e.positionConfig(f.s.config);bench::Frame frame{};frame.len=actual.size();memcpy(frame.bytes,actual.data(),actual.size());
    assert(!e.submitPrivateControl(frame,201,1000,true));auto *j=e.next(1500);assert(j);e.started(*j,0,1500,100,0);
    const auto retained=j->frame;auto cfg=f.s.config;cfg.setPositionSharing(false);e.positionConfig(cfg);
    assert(!e.idle()&&j->frame.len==retained.len&&!memcmp(j->frame.bytes,retained.bytes,retained.len));
    e.complete(*j,1700,0);assert(e.idle());e.tick(30000,0);assert(!e.next(30300));
    f.complete(1700);f.receive(f.ack(bench::le32(actual.data()+8)),1800);assert(f.s.storageStopSafe());
    pass("private-position-typed-control-retains-active-ciphertext-until-native-completion-and-suppresses-retry-after-consent-revoke");}
  {for(bool reverse:{false,true})for(bool reliable:{false,true}){
    Side a(reverse?97:11,reverse?11:97,reverse?81:127),b(reverse?11:97,reverse?97:11,reverse?127:81);
    generatedMetrics(a,b,reliable);assert(b.s.storageStopSafe()==!reliable);
    if(reliable){b.receive(b.ack(bench::le32(b.radio.frames[0].data()+8)),2300);b.poll(2301);assert(b.s.storageStopSafe());}
    b.poll(50000);assert(b.radio.frames.size()==1);}
    pass("generated-private-metrics-inherits-only-RF-ACK-bit-and-valid-proof-completes-both-board-roles");}
  {Side a(11,97,127),b(97,11,81);b.radio.air=2200;generatedMetrics(a,b,true);const auto wire=b.radio.frames[0];
    const auto nonce=b.entropy.calls,writes=b.lm.writes,encrypts=b.backend.encrypts;
    // Match Endpoint's completion deadline and retry queue jitter exactly as
    // in the LocalStats producer case above; only the generated payload differs.
    b.poll(23000,0);b.poll(23250,0);assert(b.radio.frames.size()==2&&b.radio.frames[1]==wire);b.complete(25450);
    b.poll(46000,0);b.poll(46250,0);assert(b.radio.frames.size()==3&&b.radio.frames[2]==wire);b.complete(48450);
    b.poll(69000,0);b.poll(69250,0);assert(b.radio.frames.size()==4&&b.radio.frames[3]==wire);b.complete(71450);
    b.poll(92000,0);b.poll(92250,0);assert(b.radio.frames.size()==5&&b.radio.frames[4]==wire);b.complete(94450);
    mt::RadioTiming::Deadline timing;mt::PacketLifetime::Bounds lifetime;
    assert(mt::RadioTiming::resolve(b.radio.air,timing)&&
      mt::PacketLifetime::resolve(timing.hostMs,16000+2*b.radio.air,lifetime));
    const uint32_t absoluteExpiry=1500+lifetime.pendingMs;
    b.poll(absoluteExpiry-1);assert(b.radio.frames.size()==5&&b.radio.sendCalls==5&&b.s.endpointControls()==1&&!b.s.faulted());
    b.poll(absoluteExpiry);assert(b.radio.frames.size()==5&&b.radio.sendCalls==5&&b.s.storageStopSafe()&&!b.s.faulted());
    assert(b.entropy.calls==nonce&&b.lm.writes==writes&&b.backend.encrypts==encrypts);
    pass("lost-private-reply-proof retries five reliable attempts with exact ciphertext and one nonce until airtime-derived absolute expiry");}
  {Side a(11,97,127),b(97,11,81);generatedMetrics(a,b,true);const auto id=bench::le32(b.radio.frames[0].data()+8);
    b.receive(b.ack(id,false),2300);assert(!b.s.storageStopSafe());
    b.receive(b.ack(id+1),2400);assert(!b.s.storageStopSafe());
    b.receive(b.ack(id,true,0,Key(193).binding.node),2500);assert(!b.s.storageStopSafe());
    b.receive(b.ack(id,true,0,0,true),2600);assert(!b.s.storageStopSafe());
    b.receive(b.ack(id),2700);b.poll(2701);assert(b.s.storageStopSafe());
    b.poll(50000);assert(b.radio.frames.size()==1);
    pass("unsigned-wrong-id-foreign-and-corrupt-proof-cannot-finish-reliable-private-metrics");}
  {Side a(11,97,127),b(97,11,81);generatedMetrics(a,b,true,false);const auto id=bench::le32(b.radio.frames[0].data()+8);
    b.receive(b.ack(id),2100);assert(!b.s.storageStopSafe());b.complete(2200);assert(b.s.storageStopSafe());
    b.poll(50000);assert(b.radio.frames.size()==1);
    pass("early-private-proof-retains-native-active-buffer-until-completion-without-retry");}
  {for(bool active:{false,true}){Side a(11,97,127),b(97,11,81);generatedMetrics(a,b,true,!active);
    const auto wire=b.radio.frames[0];b.runtime.freeze();b.poll(2300);
    if(active){assert(!b.s.storageStopSafe()&&b.radio.frames[0]==wire);b.complete(2400);}
    assert(b.s.storageStopSafe());b.poll(50000);assert(b.radio.frames.size()==1);}
    pass("private-authority-freeze-cancels-waiting-and-active-generated-retries-after-native-completion");}
  for(bool reverse:{false,true}){bidirectional(reverse,false);pass(reverse?"reverse-private-text-real-PSA-both-owners":"forward-private-text-real-PSA-both-owners");bidirectional(reverse,true);pass(reverse?"reverse-private-metrics-request-response-cache-export":"forward-private-metrics-request-response-cache-export");}
  {Side f(11,97,127);waiting(f);auto invalid=f.incoming(metrics(102),5001);const auto before=f.lm.writes;f.receive(invalid);
    assert(f.lm.writes==before+1&&!f.cached()&&!f.applications(f.usb,67));
    f.receive(f.incoming(metrics(101),5001),1600);assert(!f.cached()); // Authenticated wrong-request ID was burned, not reusable.
    f.receive(f.incoming(metrics(101),5002),1700);cachedUptime(f,123);assert(f.applications(f.usb,67)==1&&f.applications(f.ble,67)==1);
    meshtastic_DeviceMetrics cached{};f.entry()->deviceMetrics.exportTo(cached);assert(cached.has_air_util_tx&&cached.air_util_tx==0&&cached.has_channel_utilization&&cached.channel_utilization==0);
    pass("wrong-request-authenticated-replay-burn-cannot-poison-cache-or-hide-valid-new-response");}
  {Side f(11,97,127);waiting(f);const auto before=f.lm.writes;auto good=f.incoming(metrics(101),5001);auto bad=good;bad[16]^=1;f.receive(bad);
    Key foreign(73);f.receive(f.incoming(metrics(101),5002,foreign.scalar),1600);bad=good;put(bad.data()+4,foreign.binding.node);f.receive(bad,1700);
    assert(f.lm.writes==before&&!f.cached()&&!f.applications(f.usb,67)&&f.runtime.ready(f.local.binding.node));
    f.receive(good,1800);cachedUptime(f,123);pass("tampered-AEAD-foreign-key-and-source-do-not-write-replay-deliver-or-cache");}
  {Side f(11,97,127);waiting(f);auto plain=metrics(101);auto d=decoded(plain);meshtastic_Telemetry t{};t.which_variant=meshtastic_Telemetry_device_metrics_tag;t.variant.device_metrics.has_channel_utilization=true;t.variant.device_metrics.channel_utilization=std::numeric_limits<float>::quiet_NaN();
    auto out=pb_ostream_from_buffer(d.payload.bytes,sizeof(d.payload.bytes));assert(pb_encode(&out,meshtastic_Telemetry_fields,&t));d.payload.size=out.bytes_written;
    f.receive(f.incoming(encoded(d),5001));assert(!f.cached()&&!f.applications(f.usb,67));pass("authenticated-malformed-metric-values-never-populate-cache");}
  {Side f(11,97,127,false);waiting(f);f.receive(f.incoming(metrics(101),5001));assert(!f.entry()&&!f.s.nodes.count()&&f.applications(f.usb,67)==1);pass("matched-private-response-does-not-allocate-unknown-peer-cache");}
  {Side f(11,97,127);waiting(f);f.receive(f.incoming(metrics(101),5001),100001);assert(!f.cached()&&!f.applications(f.usb,67));f.poll(100002);assert(f.radio.frames.size()==1);pass("expired-private-query-cannot-be-revived-and-has-no-RF-retry");}
  {Side f(11,97,127);waiting(f);const auto plain=metrics(101);uint8_t frame[255]{};size_t size=0;
    assert(f.s.codec.encodeZeroHopData(f.peer.binding.node,f.local.binding.node,5001,plain.data(),plain.size(),frame,sizeof(frame),size));
    const auto writes=f.lm.writes;f.receive(Bytes(frame,frame+size));assert(f.lm.writes==writes&&!f.cached()&&!f.applications(f.usb,67));
    f.receive(f.incoming(plain,5002),1700);cachedUptime(f,123);pass("Public-response-claim-cannot-consume-private-response-approval");}
  {Side f(11,97,127);uint8_t query[26]{};size_t size=0;assert(f.s.codec.encodeTelemetryQuery(f.local.binding.node,f.peer.binding.node,101,query,sizeof(query),size));
    assert(!f.s.submitTelemetryQuery(0,query,size,100));f.poll(1000);assert(f.radio.frames.size()==1);f.complete(1200);
    f.receive(f.incoming(metrics(101),5001));assert(!f.cached()&&!f.applications(f.usb,67));pass("private-response-cannot-consume-a-Public-query-approval");}
  {Side f(11,97,127);for(uint32_t id=10;id<14;++id)f.send(id,true);assert(!f.s.endpointFree()&&f.entropy.calls==4);const auto writes=f.lm.writes;f.send(14,true);assert(f.lm.writes==writes&&f.entropy.calls==4&&!f.ledger.sent(14)&&!f.radio.sends);pass("four-job-capacity-refuses-fifth-before-extra-nonce-or-air");}
  {Side f(11,97,127);auto r=f.request(10,true);r.packet.public_key.bytes[7]^=1;input(f.c,r);assert(!f.entropy.calls&&!f.radio.sends&&!f.ledger.sent(10));assert(f.s.armEndpoint(false));f.send(11,true);assert(!f.entropy.calls&&!f.ledger.sent(11));pass("wrong-key-and-disarmed-query-refused-without-durable-nonce");}
  {Side f(11,97,127);assert(f.identity.close());const auto writes=f.lm.writes;f.send(10,true);f.receive(f.incoming(metrics(10),5001));assert(!f.entropy.calls&&f.lm.writes==writes&&!f.radio.sends&&!f.cached()&&!f.applications(f.usb,67));pass("unready-identity-refuses-private-query-and-receive-without-fallback-or-cache");}
  {Side f(11,97,127);waiting(f);assert(f.s.nodes.edit(f.peer.binding.node,f.local.binding.node,mt::NodeCache::Edit::Ignore)==1);const auto writes=f.lm.writes;f.receive(f.incoming(metrics(101),5001));assert(f.lm.writes==writes&&!f.cached()&&!f.applications(f.usb,67));pass("ignored-pinned-peer-cannot-complete-query-write-replay-or-cache");}
  {Side f(11,97,127);f.backend.failure=true;f.send(10,true);assert(f.ledger.sent(10)&&f.entropy.calls==1&&!f.radio.sends&&f.s.faulted()&&!f.runtime.ready(f.local.binding.node));pass("crypto-failure-burns-durable-query-ID-and-never-downgrades-to-Public");}
  {Side f(11,97,81,true,1,0,false,true,1);assert(!f.s.submitNodeInfo(0,300,0xffffffffu,0,false,false,100));f.poll(1000);assert(f.radio.frames.size()==1);f.complete(1200);
    bench::MtSignature gate;bench::RouteMetadata first;auto &bootstrap=f.radio.frames[0];assert(f.s.codec.decode(bootstrap.data(),bootstrap.size(),first)&&gate.apply(bootstrap.data(),bootstrap.size(),first,1300)==bench::MtSignature::Result::Verified);
    for(bool telemetry:{false,true}){const auto plain=telemetry?metrics(700):data("signed Public regression");auto d=decoded(plain);if(telemetry)d.request_id=0;const auto bytes=encoded(d);uint8_t wire[255]{};size_t n=0;const uint32_t id=telemetry?302:301;
      assert(f.s.encodePublicMessage(0xffffffffu,id,bytes.data(),bytes.size(),wire,sizeof(wire),n)&&f.ledger.sent(id));bench::RouteMetadata m;
      assert(f.s.codec.decode(wire,n,m)&&m.signatureLength==64&&gate.apply(wire,n,m,1400+id)==bench::MtSignature::Result::Verified);
      m.signature[0]^=1;assert(gate.apply(wire,n,m,1500+id)==bench::MtSignature::Result::Rejected);}
    pass("signed-Public-text-and-telemetry-verify-after-exact-NodeInfo-bootstrap-no-unsigned-fallback");}
  {for(bool telemetry:{false,true}){Side f(11,97,127);const auto plain=telemetry?publicMetrics():data("pinned Public without NodeInfo");
      const unsigned port=telemetry?meshtastic_PortNum_TELEMETRY_APP:meshtastic_PortNum_TEXT_MESSAGE_APP;
      const auto frame=signedPublic(f.peer,f.s.codec,plain,8001);assert(!f.s.signature.keyCount(1500));
      // Reproduce the old verifier's empty-cache quarantine with real signature
      // bytes, then exercise the actual Service/Companion wrapper and fanout.
      bench::MtSignature empty;bench::RouteMetadata m;assert(f.s.codec.decode(frame.data(),frame.size(),m));
      assert(empty.apply(frame.data(),frame.size(),m,1500)==bench::MtSignature::Result::Quarantined);
      const PublicReadOnly before(f);f.receive(frame);assert(f.s.signature.keyCount(1500)==1);
      assert(publicApplications(f.usb,f.peer.binding.node,port)==1&&publicApplications(f.ble,f.peer.binding.node,port)==1);
      for(const auto *host:{&f.usb,&f.ble})for(const auto &r:host->out)if(r.which_payload_variant==meshtastic_FromRadio_packet_tag){
        const auto expected=decoded(plain);assert(r.packet.id==8001&&r.packet.decoded.payload.size==expected.payload.size&&
          !memcmp(r.packet.decoded.payload.bytes,expected.payload.bytes,expected.payload.size));}
      f.receive(frame,1600);assert(publicApplications(f.usb,f.peer.binding.node,port)==1&&publicApplications(f.ble,f.peer.binding.node,port)==1);
      assert(f.cached()==telemetry);before.unchangedExceptVerifiedSigner();}
    pass("pinned-signed-Public-text-and-broadcast-metrics-without-NodeInfo-fanout-once-with-only-signer-marker-and-no-private-journal-writes");}
  {Side f(11,97,127);const auto plain=data("pin survives opportunistic-cache expiry");
    const auto first=signedPublic(f.peer,f.s.codec,plain,8010),expired=signedPublic(f.peer,f.s.codec,plain,8011),cleared=signedPublic(f.peer,f.s.codec,plain,8012);
    const PublicReadOnly before(f);f.receive(first,1500);const uint32_t later=1501+bench::MtSignature::KeyTtlMs;
    assert(!f.s.signature.keyCount(later));f.receive(expired,later);assert(f.s.signature.keyCount(later)==1);
    f.s.signature.clear();assert(!f.s.signature.keyCount(later+1));f.receive(cleared,later+1);
    assert(f.s.signature.keyCount(later+1)==1&&publicApplications(f.usb,f.peer.binding.node,1)==3&&publicApplications(f.ble,f.peer.binding.node,1)==3);
    before.unchangedExceptVerifiedSigner();pass("durable-pin-revalidates-signed-Public-after-full-TTL-and-explicit-signature-cache-clear");}
  {Side f(11,97,127);Key unknown(193);const auto frame=signedPublic(unknown,f.s.codec,data("unknown signed peer"),8020);
    const PublicReadOnly before(f);const auto status=publicStatus(f,frame,1500);assert(status.signatureQuarantined&&!status.signatureVerified&&!status.signatureRejected);
    f.receive(frame);assert(f.usb.out.empty()&&f.ble.out.empty()&&!f.s.signature.keyCount(1500)&&f.s.nodes.count()==1);
    before.unchanged();pass("unknown-signed-Public-peer-stays-quarantined-without-opportunistic-NodeDB-trust");}
  {Side f(11,97,127);const auto frame=signedPublic(f.peer,f.s.codec,data("corrupt only the signature"),8030,true);
    const PublicReadOnly before(f);const auto status=publicStatus(f,frame,1500);assert(status.signatureRejected&&!status.signatureVerified&&!status.signatureQuarantined);
    f.receive(frame);assert(f.usb.out.empty()&&f.ble.out.empty()&&!f.cached());before.unchanged();
    pass("active-durable-pin-does-not-substitute-for-actual-Public-signature-verification");}
  {Side f(11,97,127);Key other(193);
    // Explicit legacy cache pinning permits a different valid curve key under
    // this node ID. This is a conflict boundary, not a fabricated CRC collision.
    assert(f.s.signature.trustPublicKey(f.peer.binding.node,other.binding.publicKey,1000));
    const auto frame=signedPublic(f.peer,f.s.codec,data("conflicting live authority"),8040);const PublicReadOnly before(f);
    const auto status=publicStatus(f,frame,1500);assert(status.signatureRejected&&!status.signatureVerified&&!status.signatureQuarantined);
    f.receive(frame);assert(f.usb.out.empty()&&f.ble.out.empty()&&f.s.signature.keyCount(1500)==1);
    assert(f.s.signature.trustPublicKey(f.peer.binding.node,other.binding.publicKey,1500)&&
      !f.s.signature.trustPublicKey(f.peer.binding.node,f.peer.binding.publicKey,1500));before.unchanged();
    pass("durable-pin-refresh-cannot-replace-a-conflicting-live-signature-authority");}
  {Side f(11,97,127);for(unsigned i=0;i<bench::MtSignature::Capacity;++i){assert(100+i!=f.peer.binding.node);assert(f.s.signature.trustPublicKey(100+i,f.peer.binding.publicKey,1000));}
    const auto frame=signedPublic(f.peer,f.s.codec,data("full authority cache"),8050);const PublicReadOnly before(f);
    const auto status=publicStatus(f,frame,1500);assert(status.signatureRejected&&!status.signatureVerified&&!status.signatureQuarantined);
    f.receive(frame);assert(f.usb.out.empty()&&f.ble.out.empty()&&f.s.signature.keyCount(1500)==bench::MtSignature::Capacity);
    before.unchanged();
    f.s.signature.forgetPublicKey(100);f.receive(frame,1600);
    assert(publicApplications(f.usb,f.peer.binding.node,1)==1&&publicApplications(f.ble,f.peer.binding.node,1)==1&&f.s.signature.keyCount(1600)==bench::MtSignature::Capacity);
    before.unchangedExceptVerifiedSigner();pass("full32-live-signature-cache-fails-closed-without-eviction-and-recovers-only-after-explicit-slot-removal");}
  {Side f(11,97,127);const auto frame=signedPublic(f.peer,f.s.codec,data("ignored durable peer"),8060);
    assert(f.s.nodes.edit(f.peer.binding.node,f.local.binding.node,mt::NodeCache::Edit::Ignore)==1);const PublicReadOnly before(f);
    const auto status=publicStatus(f,frame,1500);assert(status.signatureRejected&&!status.signatureVerified);
    f.receive(frame);assert(f.usb.out.empty()&&f.ble.out.empty()&&!f.s.signature.keyCount(1500));before.unchanged();
    pass("ignored-durable-peer-cannot-refresh-signature-cache-or-reach-either-local-owner");}
  {for(unsigned mode=0;mode<3;++mode){Side f(11,97,127);const auto frame=signedPublic(f.peer,f.s.codec,data("inactive durable peer"),8070+mode);
      if(mode==0)f.runtime.freeze();else if(mode==1)assert(f.identity.close());else assert(f.s.revokePrivatePeer(f.peer.binding.node));
      const PublicReadOnly before(f);uint8_t key[32];memset(key,0x5a,sizeof(key));assert(!f.runtime.peerPublicIdentity(f.peer.binding.node,key));for(auto v:key){assert(v==0x5a);}
      const auto status=publicStatus(f,frame,1500);assert(status.signatureRejected&&!status.signatureVerified&&!status.signatureQuarantined);
      f.receive(frame);assert(f.usb.out.empty()&&f.ble.out.empty()&&!f.s.signature.keyCount(1500));before.unchanged();}
    pass("frozen-closed-and-Service-revoked-private-authorities-refuse-Public-pin-refresh-without-extra-writes");}
}
