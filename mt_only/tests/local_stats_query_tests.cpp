// SPDX-License-Identifier: GPL-3.0-only
#include "telemetry_query_fixture.h"
#include "../src/pki/PrivateApplication.h"
#include <limits>

static meshtastic_Telemetry maximumLocalStats() {
  meshtastic_Telemetry t{};t.time=UINT32_MAX;t.which_variant=meshtastic_Telemetry_local_stats_tag;
  t.variant.local_stats={UINT32_MAX,100.0f,100.0f,UINT32_MAX,UINT32_MAX,UINT32_MAX,
    UINT16_MAX,UINT16_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT16_MAX,INT32_MIN};
  return t;
}
static std::vector<uint8_t> localStatsRequest() {
  meshtastic_Data d{};d.portnum=meshtastic_PortNum_TELEMETRY_APP;d.want_response=true;
  d.has_bitfield=true;d.bitfield=2;d.payload.size=2;d.payload.bytes[0]=0x32;d.payload.bytes[1]=0;
  uint8_t bytes[227]{};auto out=pb_ostream_from_buffer(bytes,sizeof(bytes));
  assert(pb_encode(&out,meshtastic_Data_fields,&d));return {bytes,bytes+out.bytes_written};
}

static meshtastic_Telemetry producedStatsCheck(const char *name,uint32_t uptime,uint32_t request,
  const zt::Observed<float> &air,const zt::Observed<float> &chan,
  uint32_t expectUptime,float expectAir,float expectChan) {
  uint8_t b[40]{};size_t size=99;
  assert(mt::LocalStatsQuery::uptimeResponse(uptime,request,air,chan,b,sizeof(b),size));
  assert(size>0&&size<=sizeof(b));
  meshtastic_Data d{};auto in=pb_istream_from_buffer(b,size);
  assert(pb_decode(&in,meshtastic_Data_fields,&d)&&!in.bytes_left);
  assert(d.portnum==meshtastic_PortNum_TELEMETRY_APP&&d.request_id==request&&d.has_bitfield&&!d.bitfield&&!d.want_response);
  meshtastic_Telemetry t{};auto p=pb_istream_from_buffer(d.payload.bytes,d.payload.size);
  assert(pb_decode(&p,meshtastic_Telemetry_fields,&t)&&!p.bytes_left);
  assert(t.which_variant==meshtastic_Telemetry_local_stats_tag&&!t.time);
  const auto &ls=t.variant.local_stats;
  assert(ls.uptime_seconds==expectUptime&&ls.channel_utilization==expectChan&&ls.air_util_tx==expectAir);
  // No has_* presence bits: packet/node/heap/noise counters stay scalar defaults.
  assert(!ls.num_packets_tx&&!ls.num_packets_rx&&!ls.num_packets_rx_bad&&!ls.num_online_nodes&&
    !ls.num_total_nodes&&!ls.num_rx_dupe&&!ls.num_tx_relay&&!ls.num_tx_relay_canceled&&
    !ls.heap_total_bytes&&!ls.heap_free_bytes&&!ls.num_tx_dropped&&ls.noise_floor==0);
  group(name);return t;
}
static void runLocalStatsProducerCases(){
 producedStatsCheck("known uptime zero with nonzero request",0,77,zt::Observed<float>(12.5f),zt::Observed<float>(45.0f),0,12.5f,45.0f);
 producedStatsCheck("known uptime UINT32_MAX with nonzero request",UINT32_MAX,42,zt::Observed<float>(1.0f),zt::Observed<float>(2.0f),UINT32_MAX,1.0f,2.0f);
 {uint8_t b[40]{};size_t size=99;
  assert(!mt::LocalStatsQuery::uptimeResponse(100,0,zt::Observed<float>(10.0f),zt::Observed<float>(10.0f),b,sizeof(b),size)&&size==0);
  group("request zero rejected with output size reset to zero");}
 {uint8_t b[40]{};size_t exact=99,size=99;
  assert(mt::LocalStatsQuery::uptimeResponse(UINT32_MAX,UINT32_MAX,100.0f,100.0f,b,sizeof(b),exact));
  assert(exact==29&&exact<sizeof(b));
  assert(!mt::LocalStatsQuery::uptimeResponse(UINT32_MAX,UINT32_MAX,100.0f,100.0f,b,exact-1,size)&&size==0);
  assert(mt::LocalStatsQuery::uptimeResponse(UINT32_MAX,UINT32_MAX,100.0f,100.0f,b,exact,size)&&size==exact);
  size=99;assert(!mt::LocalStatsQuery::uptimeResponse(1,77,{}, {},nullptr,sizeof(b),size)&&size==0);
  group("exact29-byte maximum envelope fits; one-byte-short and null output fail with size reset");}
 {uint8_t a[40]{},b[40]{};size_t sa=99,sb=99;
  assert(mt::LocalStatsQuery::uptimeResponse(0,77,zt::Observed<float>(0.0f),zt::Observed<float>(),a,sizeof(a),sa));
  assert(mt::LocalStatsQuery::uptimeResponse(0,77,zt::Observed<float>(),zt::Observed<float>(),b,sizeof(b),sb));
  // LocalStats scalars have no has_* presence bits: available 0.0 and
  // unavailable both serialize as the default zero, so the wire bytes are
  // identical. No wire-level distinction is claimed.
  assert(sa==sb&&!memcmp(a,b,sa));
  meshtastic_Data d{};auto in=pb_istream_from_buffer(a,sa);assert(pb_decode(&in,meshtastic_Data_fields,&d));
  meshtastic_Telemetry t{};auto p=pb_istream_from_buffer(d.payload.bytes,d.payload.size);assert(pb_decode(&p,meshtastic_Telemetry_fields,&t));
  assert(t.variant.local_stats.air_util_tx==0.0f&&t.variant.local_stats.channel_utilization==0.0f);
  group("available 0.0 and unavailable floats share the default-zero wire form");}
 producedStatsCheck("finite percent boundaries 0 and 100 survive",100,77,zt::Observed<float>(0.0f),zt::Observed<float>(100.0f),100,0.0f,100.0f);
 {const float bad[]={-1.0f,101.0f,std::numeric_limits<float>::quiet_NaN(),
    std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity()};
  for(float v:bad)for(unsigned field=0;field<2;++field){
   uint8_t b[40]{};size_t size=99;
   assert(mt::LocalStatsQuery::uptimeResponse(100,77,field?50.0f:v,field?v:50.0f,b,sizeof(b),size));
   meshtastic_Data d{};auto in=pb_istream_from_buffer(b,size);assert(pb_decode(&in,meshtastic_Data_fields,&d));
   meshtastic_Telemetry t{};auto p=pb_istream_from_buffer(d.payload.bytes,d.payload.size);assert(pb_decode(&p,meshtastic_Telemetry_fields,&t));
   assert(t.variant.local_stats.air_util_tx==(field?50.0f:0.0f)&&t.variant.local_stats.channel_utilization==(field?0.0f:50.0f));
  }
  group("negative, over-100, NaN and +/-infinity are not included; valid sibling survives");}
 producedStatsCheck("only intended uptime and percent values; counters and time stay default",12345,77,zt::Observed<float>(33.0f),zt::Observed<float>(66.0f),12345,33.0f,66.0f);
}

static void feedProducer(Fixture &f,bool ble,const meshtastic_ToRadio &r) {
  if(ble){input(f.bc,r,0xfffffff0);return;}
  uint8_t wire[512];auto out=pb_ostream_from_buffer(wire,sizeof(wire));
  assert(pb_encode(&out,meshtastic_ToRadio_fields,&r));
  f.c.byte(0x94,123,0,mt::Companion::WorkspaceProof{true});f.c.byte(0xc3,123,0,mt::Companion::WorkspaceProof{true});
  f.c.byte(out.bytes_written>>8,123,0,mt::Companion::WorkspaceProof{true});f.c.byte(out.bytes_written,123,0,mt::Companion::WorkspaceProof{true});
  for(size_t i=0;i<out.bytes_written;++i)f.c.byte(wire[i],123,0,mt::Companion::WorkspaceProof{true});
}
static void runLocalStatsCompanionCases() {
  std::cout.setf(std::ios::unitbuf);
  // h.out/bh.out are local host FromRadio outputs; with the radio stopped and sends==0 they are never RF evidence.
  for(uint8_t model:{81,127})for(bool ble:{false,true}){
    auto f=std::make_unique<Fixture>(false,model);f->s.stop();
    auto &h=ble?f->bh:f->h;auto &other=ble?f->h:f->bh;
    assert(!f->s.receiving()&&!f->s.endpointArmed());
    auto before=f->s.config;
    auto q=query(77,OURS,0);q.packet.channel=0;q.packet.from=PEER;q.packet.want_ack=true;
    q.packet.decoded.payload.size=2;q.packet.decoded.payload.bytes[0]=0x32;q.packet.decoded.payload.bytes[1]=0;
    // Only the selected host's uptime source is valid; the other host's stays invalid.
    h.uptimeValid=true;h.uptime=123456;
    feedProducer(*f,ble,q);
    assert(telemetries(h,77)==1&&results(h,77)==0);
    meshtastic_MeshPacket reply{};
    for(const auto &o:h.out)if(o.which_payload_variant==meshtastic_FromRadio_packet_tag&&
      o.packet.decoded.portnum==meshtastic_PortNum_TELEMETRY_APP&&o.packet.decoded.request_id==77)reply=o.packet;
    assert(reply.from==OURS&&reply.to==OURS&&!reply.want_ack&&!reply.pki_encrypted&&!reply.public_key.size&&!reply.hop_limit&&!reply.decoded.want_response);
    assert(reply.decoded.has_bitfield&&!reply.decoded.bitfield);
    std::vector<uint8_t> body;assert(telemetries(h,77,&body)==1);
    meshtastic_Telemetry t{};auto in=pb_istream_from_buffer(body.data(),body.size());
    assert(pb_decode(&in,meshtastic_Telemetry_fields,&t));
    assert(t.which_variant==meshtastic_Telemetry_local_stats_tag&&t.time==0);
    assert(t.variant.local_stats.uptime_seconds==123456);
    // LocalStats has no has_* fields. The real NodeDB contains self; unmeasured counters stay default omitted.
    assert(!t.variant.local_stats.channel_utilization&&!t.variant.local_stats.air_util_tx);
    assert(!t.variant.local_stats.num_packets_tx&&!t.variant.local_stats.num_packets_rx&&!t.variant.local_stats.num_packets_rx_bad);
    assert(!t.variant.local_stats.num_online_nodes&&t.variant.local_stats.num_total_nodes==1);
    assert(!t.variant.local_stats.num_rx_dupe&&!t.variant.local_stats.num_tx_relay&&!t.variant.local_stats.num_tx_relay_canceled);
    assert(!t.variant.local_stats.heap_total_bytes&&!t.variant.local_stats.heap_free_bytes);
    assert(!t.variant.local_stats.num_tx_dropped&&t.variant.local_stats.noise_floor==0);
    for(unsigned keyLength:{1u,32u}) {
      Fixture hinted(false,model);hinted.s.stop();auto &hintHost=ble?hinted.bh:hinted.h;
      auto &hintOther=ble?hinted.h:hinted.bh;
      hintHost.uptimeValid=true;hintHost.uptime=123456;auto withHint=q;
      withHint.packet.public_key.size=keyLength;memset(withHint.packet.public_key.bytes,0xa5,keyLength);
      feedProducer(hinted,ble,withHint);std::vector<uint8_t> hintedBody;meshtastic_MeshPacket hintedReply{};
      assert(telemetries(hintHost,77,&hintedBody)==1&&results(hintHost,77)==0&&
        telemetries(ble?hinted.h:hinted.bh,77)==0&&hintedBody==body);
      for(const auto &o:hintHost.out)if(o.which_payload_variant==meshtastic_FromRadio_packet_tag&&
          o.packet.decoded.portnum==meshtastic_PortNum_TELEMETRY_APP&&o.packet.decoded.request_id==77)hintedReply=o.packet;
      assert(hintedReply.from==reply.from&&hintedReply.to==reply.to&&
        hintedReply.decoded.portnum==reply.decoded.portnum&&hintedReply.decoded.request_id==reply.decoded.request_id&&
        hintedReply.decoded.has_bitfield&&!hintedReply.decoded.bitfield&&!hintedReply.want_ack&&
        !hintedReply.hop_limit&&!hintedReply.decoded.want_response&&
        hintedReply.decoded.payload.size==reply.decoded.payload.size&&
        !memcmp(hintedReply.decoded.payload.bytes,reply.decoded.payload.bytes,reply.decoded.payload.size)&&
        !hintedReply.pki_encrypted&&!hintedReply.public_key.size&&hintOther.out.empty()&&!hinted.radio.sends&&
        hinted.s.config.same(before));
    }
    assert(other.out.empty()&&f->s.config.same(before)&&!f->radio.sends);
    h.out.clear();
    // Unavailable clock: the selected host's monotonic uptime source is invalid.
    h.uptimeValid=false;
    feedProducer(*f,ble,q);
    assert(results(h,77,meshtastic_Routing_Error_NO_INTERFACE)==1&&results(h,77)==1);
    assert(telemetries(h,77)==0&&other.out.empty()&&f->s.config.same(before)&&!f->radio.sends);
    h.out.clear();
    // Malformed selector: the clock is valid, so only the selector can fail.
    h.uptimeValid=true;h.uptime=123456;
    auto bad=q;bad.packet.decoded.payload.size=1;bad.packet.decoded.payload.bytes[0]=0;
    feedProducer(*f,ble,bad);
    assert(results(h,77,meshtastic_Routing_Error_BAD_REQUEST)==1&&results(h,77)==1);
    assert(telemetries(h,77)==0&&other.out.empty()&&f->s.config.same(before)&&!f->radio.sends);
  }
  group("local USB/BLE self-query: both models and owners answer with same LocalStats wire for absent/1/32-byte key hints; no caller key, unavailable clock and malformed selector rejected, no RF/config change");
}

static void runLocalStatsProducerEdges() {
  uint8_t absent[40]{},negative[40]{};size_t na=0,nn=0;
  assert(mt::LocalStatsQuery::uptimeResponse(1,77,{}, {},absent,sizeof(absent),na));
  assert(mt::LocalStatsQuery::uptimeResponse(1,77,-0.0f,{},negative,sizeof(negative),nn));
  assert(nn>na);
  meshtastic_Data d{};auto ds=pb_istream_from_buffer(negative,nn);
  assert(pb_decode(&ds,meshtastic_Data_fields,&d)&&!ds.bytes_left);
  meshtastic_Telemetry t{};auto ts=pb_istream_from_buffer(d.payload.bytes,d.payload.size);
  assert(pb_decode(&ts,meshtastic_Telemetry_fields,&t)&&!ts.bytes_left);
  uint32_t bits=0;memcpy(&bits,&t.variant.local_stats.air_util_tx,sizeof(bits));
  assert(bits==0x80000000u&&mt::LocalStatsQuery::responsePayload(d.payload.bytes,d.payload.size));
  group("negative zero preserves canonical nanopb sign bits; positive zero and unavailable remain omitted");
  for(uint8_t model:{81,127})for(bool ble:{false,true}) {
    Fixture f(false,model);f.s.stop();auto &h=ble?f.bh:f.h;auto &other=ble?f.h:f.bh;
    h.uptimeValid=true;h.uptime=UINT32_MAX;const auto before=f.s.config;
    auto q=query(77,OURS,0);q.packet.decoded.payload.size=2;
    q.packet.decoded.payload.bytes[0]=0x32;q.packet.decoded.payload.bytes[1]=0;
    for(unsigned mode:{0u,1u,2u,3u,4u,5u,7u,8u,9u,10u,11u,12u,13u,14u}) {
      auto bad=q;
      switch(mode) {
        case 0:bad.packet.id=0;break;
        case 1:bad.packet.decoded.want_response=false;break;
        case 2:bad.packet.decoded.request_id=9;break;
        case 3:bad.packet.decoded.reply_id=9;break;
        case 4:bad.packet.decoded.has_bitfield=true;break;
        case 5:bad.packet.pki_encrypted=true;break;
        case 7:bad.packet.channel=1;break;
        case 8:bad.packet.hop_start=1;break;
        case 9:bad.packet.decoded.xeddsa_signature.size=1;break;
        case 10:bad.packet.decoded.payload.size=1;break;
        case 11:bad.packet.decoded.payload.size=4;memcpy(bad.packet.decoded.payload.bytes,"\x32\x00\x32\x00",4);break;
        case 12:bad.packet.decoded.payload.size=4;memcpy(bad.packet.decoded.payload.bytes,"\x08\x00\x32\x00",4);break;
        case 13:bad.packet.decoded.payload.size=4;memcpy(bad.packet.decoded.payload.bytes,"\x32\x02\x08\x01",4);break;
        case 14:bad.packet.decoded.emoji=1;break;
      }
      feedProducer(f,ble,bad);
      const auto expected=mode==7?meshtastic_Routing_Error_NO_CHANNEL:
        mode==5||mode==9?meshtastic_Routing_Error_PKI_FAILED:meshtastic_Routing_Error_BAD_REQUEST;
      assert(!telemetries(h,bad.packet.id)&&results(h,bad.packet.id,expected)==1&&results(h,bad.packet.id)==1);
      assert(other.out.empty()&&!f.radio.sends&&f.s.config.same(before));h.out.clear();
    }
    for(uint32_t seconds:{0u,4294968u,UINT32_MAX}) {
      h.uptime=seconds;feedProducer(f,ble,q);std::vector<uint8_t> body;
      assert(telemetries(h,77,&body)==1&&!results(h,77));
      meshtastic_Telemetry x{};auto s=pb_istream_from_buffer(body.data(),body.size());
      assert(pb_decode(&s,meshtastic_Telemetry_fields,&x)&&!s.bytes_left&&x.which_variant==meshtastic_Telemetry_local_stats_tag);
      assert(x.variant.local_stats.uptime_seconds==seconds&&!x.time);h.out.clear();
    }
    // Existing local DeviceMetrics remains independent of the new selector.
    feedProducer(f,ble,query(77,OURS,0));std::vector<uint8_t> body;
    assert(telemetries(h,77,&body)==1&&!results(h,77));
    meshtastic_Telemetry x{};auto s=pb_istream_from_buffer(body.data(),body.size());
    assert(pb_decode(&s,meshtastic_Telemetry_fields,&x)&&x.which_variant==meshtastic_Telemetry_device_metrics_tag);
    assert(x.variant.device_metrics.has_uptime_seconds&&x.variant.device_metrics.uptime_seconds==UINT32_MAX);
    assert(other.out.empty()&&!f.radio.sends&&f.s.config.same(before));
  }
  group("local LocalStats both boards and owners:14 invalid canonical guards, zero/max/64-bit-seconds boundaries, DeviceMetrics isolation and no RF/settings mutation");
}

int main() {
  runLocalStatsProducerCases();
  runLocalStatsCompanionCases();
  runLocalStatsProducerEdges();
  std::cout.setf(std::ios::unitbuf);
  {Fixture f;beginQuery(f);f.rx(goodFrame(),500);f.clients();
    assert(telemetries(f.h)==1&&results(f.h,77,0)==1);}
  group("existing public DeviceMetrics query and normalized success remain functional");
  const auto request=localStatsRequest();
  assert(mt::pki::PrivateApplication::allowed(request.data(),request.size()));
  group("canonical private LocalStats selector admitted by the private application validator");
  const auto t=maximumLocalStats();const auto payload=encodeTelemetry(t);const auto data=encodeData(payload,UINT32_MAX);
  uint8_t stats[100]{};auto out=pb_ostream_from_buffer(stats,sizeof(stats));
  assert(pb_encode(&out,meshtastic_LocalStats_fields,&t.variant.local_stats));
  assert(out.bytes_written==87&&payload.size()==94&&data.size()==105&&data.size()<=227);
  assert(mt::pki::PrivateApplication::allowed(data.data(),data.size()));
  group("actual nanopb worst-case LocalStats87 Telemetry94 canonical private Data105 including negative int32");
  mt::PeerDeviceMetrics cached;
  assert(!mt::TelemetryQuery::request(payload.data(),payload.size()));
  assert(!mt::TelemetryQuery::cachedMetrics(payload.data(),payload.size(),cached));
  group("LocalStats cannot enter DeviceMetrics request path or peer cache");
  assert(mt::LocalStatsQuery::request(reinterpret_cast<const uint8_t *>("\x32\x00"),2));
  for(const auto &invalid:std::vector<std::vector<uint8_t>>{{},{0x12,0},{0x32},{0x32,0,0x32,0},{0x08,0,0x32,0}})
    assert(!mt::LocalStatsQuery::request(invalid.data(),invalid.size()));
  assert(mt::LocalStatsQuery::responsePayload(payload.data(),payload.size()));
  for(float value:{-1.0f,100.01f,std::numeric_limits<float>::infinity(),
      -std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}) {
    for(unsigned field=0;field<2;++field) {
      auto invalid=t;
      if(field)invalid.variant.local_stats.air_util_tx=value;
      else invalid.variant.local_stats.channel_utilization=value;
      const auto body=encodeTelemetry(invalid),wire=encodeData(body);
      assert(!mt::LocalStatsQuery::responsePayload(body.data(),body.size()));
      assert(!mt::pki::PrivateApplication::allowed(wire.data(),wire.size()));
    }
  }
  group("exact empty selector and finite percentage bounds reject malformed queries and snapshots");
  {auto duplicate=payload;duplicate.insert(duplicate.end(),payload.begin(),payload.end());
    auto unknown=payload;unknown.insert(unknown.end(),{0xf8,1,1});
    auto truncated=payload;truncated.pop_back();
    const std::vector<uint8_t> wide16={0x32,4,0x38,0x80,0x80,4};
    for(const auto &invalid:{duplicate,unknown,truncated,wide16}) {
      const auto wire=encodeData(invalid);
      assert(!mt::LocalStatsQuery::responsePayload(invalid.data(),invalid.size()));
      assert(!mt::pki::PrivateApplication::allowed(wire.data(),wire.size()));
    }
  }
  group("unknown duplicate truncated and uint16-overflow snapshots fail canonical admission");
  {bench::RouteMetadata m;assert(bench::MtCodec::decodeData(data.data(),data.size(),m));
    uint8_t bounded[105]{};size_t n=77;
    assert(!mt::LocalStatsQuery::response(m,bounded,104,n)&&n==0);
    assert(mt::LocalStatsQuery::response(m,bounded,sizeof(bounded),n)&&n==data.size()&&!memcmp(bounded,data.data(),n));
    auto invalid=m;invalid.portnum=1;assert(!mt::LocalStatsQuery::response(invalid,bounded,sizeof(bounded),n));
    invalid=m;invalid.wantResponse=true;assert(!mt::LocalStatsQuery::response(invalid,bounded,sizeof(bounded),n));
    invalid=m;invalid.request_id=0;assert(!mt::LocalStatsQuery::response(invalid,bounded,sizeof(bounded),n));
    invalid=m;invalid.bitfield=2;assert(!mt::LocalStatsQuery::response(invalid,bounded,sizeof(bounded),n));
  }
  group("maximum response has exact105 byte capacity and preserves common canonical response-envelope gates");
  return 0;
}
