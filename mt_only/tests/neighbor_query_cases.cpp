// SPDX-License-Identifier: GPL-3.0-only
#ifdef NDEBUG
#error Neighbor encoder qualification requires active assertions
#endif
#include "NeighborQuery.h"
#include "Endpoint.h"
#include <pb_decode.h>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>

static unsigned groups=0;
static void pass(const char *name){++groups;std::printf("PASS %s\n",name);}
static meshtastic_NeighborInfo snapshot(){
  meshtastic_NeighborInfo info{};
  info.node_id=1;info.last_sent_by_id=2;info.node_broadcast_interval_secs=60;
  return info;
}
static meshtastic_Data decode(const uint8_t *wire,size_t size,uint8_t bitfield=0){
  meshtastic_Data data{};auto in=pb_istream_from_buffer(wire,size);
  assert(pb_decode(&in,meshtastic_Data_fields,&data)&&!in.bytes_left);
  assert(data.portnum==meshtastic_PortNum_NEIGHBORINFO_APP&&data.request_id);
  assert(data.has_bitfield&&data.bitfield==bitfield&&!data.want_response);
  assert(!data.dest&&!data.source&&!data.reply_id&&!data.emoji&&!data.xeddsa_signature.size);
  std::array<uint8_t,320> canonical{};auto out=pb_ostream_from_buffer(canonical.data(),canonical.size());
  assert(pb_encode(&out,meshtastic_Data_fields,&data)&&out.bytes_written==size);
  assert(!std::memcmp(wire,canonical.data(),size));
  return data;
}
static meshtastic_NeighborInfo inner(const meshtastic_Data &data){
  meshtastic_NeighborInfo info{};auto in=pb_istream_from_buffer(data.payload.bytes,data.payload.size);
  assert(pb_decode(&in,meshtastic_NeighborInfo_fields,&info)&&!in.bytes_left);
  std::array<uint8_t,meshtastic_NeighborInfo_size> canonical{};
  auto out=pb_ostream_from_buffer(canonical.data(),canonical.size());
  assert(pb_encode(&out,meshtastic_NeighborInfo_fields,&info)&&out.bytes_written==data.payload.size);
  assert(!std::memcmp(data.payload.bytes,canonical.data(),data.payload.size));
  return info;
}
static bench::RouteMetadata metadata(const meshtastic_NeighborInfo &info){
  bench::RouteMetadata m{};m.portnum=71;m.request_id=0x12345678;m.hasBitfield=true;
  auto out=pb_ostream_from_buffer(m.payload,sizeof(m.payload));
  assert(pb_encode(&out,meshtastic_NeighborInfo_fields,&info));m.payloadLength=uint8_t(out.bytes_written);
  return m;
}
struct RecordingEncoder:mt::Endpoint::ControlEncoder {
  unsigned snapshots=0,encodes=0;bool available=true;
  meshtastic_NeighborInfo value=snapshot();
  bool neighborSnapshot(meshtastic_NeighborInfo &out)override{++snapshots;out=value;return available;}
  bool encodeChannelControl(uint32_t,uint32_t,const uint8_t *,size_t,bench::Frame &)override{++encodes;return false;}
};
static bench::Frame header(uint32_t id){
  bench::Frame frame{};frame.len=17;frame.bytes[0]=42;frame.bytes[4]=43;frame.bytes[13]=1;
  for(unsigned i=0;i<4;++i)frame.bytes[8+i]=uint8_t(id>>(8*i));
  return frame; // Synthetic direct Endpoint input; no encoded RF/crypto claim.
}
static int submit(mt::Endpoint &ep,uint8_t owner,const bench::Frame &frame,bool kind=true,bool *created=nullptr){
  return ep.submit(owner,frame.bytes,frame.len,false,0,false,true,true,created,false,false,false,false,false,false,0,kind);
}
int main(){
  std::array<uint8_t,320> wire{};size_t size=999;
  const uint8_t requestBytes[]={8,71,24,1,72,2};
  assert(mt::NeighborQuery::request(wire.data(),wire.size(),size));
  assert(size==sizeof(requestBytes)&&!std::memcmp(wire.data(),requestBytes,size));
  meshtastic_Data requestData{};auto requestIn=pb_istream_from_buffer(wire.data(),size);
  assert(pb_decode(&requestIn,meshtastic_Data_fields,&requestData)&&!requestIn.bytes_left);
  assert(requestData.portnum==meshtastic_PortNum_NEIGHBORINFO_APP&&requestData.want_response&&
    !requestData.payload.size&&!requestData.dest&&!requestData.source&&!requestData.request_id&&
    !requestData.reply_id&&!requestData.emoji&&!requestData.xeddsa_signature.size&&requestData.has_bitfield&&requestData.bitfield==2);
  bench::RouteMetadata requestMeta{};assert(bench::MtCodec::decodeData(wire.data(),size,requestMeta));
  assert(mt::NeighborQuery::requestShape(requestMeta));
  pass("request selector is exact six-byte canonical empty Data with bitfield2");
  for(size_t cap=0;cap<=sizeof(requestBytes);++cap){
    wire.fill(0xa5);size=999;const bool ok=mt::NeighborQuery::request(wire.data()+8,cap,size);
    assert(ok==(cap==sizeof(requestBytes))&&size==(ok?sizeof(requestBytes):0));
    for(size_t i=0;i<8;++i)assert(wire[i]==0xa5);
    for(size_t i=8+(ok?size:0);i<wire.size();++i)assert(wire[i]==0xa5);
  }
  size=999;assert(!mt::NeighborQuery::request(nullptr,320,size)&&size==0);
  pass("request capacity/null guards preserve all output sentinels and zero failed size");
  for(unsigned which=0;which<10;++which){
    auto bad=requestMeta;switch(which){case 0:bad.portnum=67;break;case 1:bad.payloadLength=1;break;
      case 2:bad.wantResponse=false;break;case 3:bad.hasBitfield=false;break;case 4:bad.bitfield=0;break;
      case 5:bad.bitfield=3;break;case 6:bad.request_id=1;break;case 7:bad.reply_id=1;break;
      case 8:bad.emoji=1;break;default:bad.signatureLength=64;break;}
    assert(!mt::NeighborQuery::requestShape(bad));
  }
  pass("request metadata rejects other port/body/missing selector/provenance claims");
  const auto empty=snapshot();
  assert(mt::NeighborQuery::response(0x12345678,empty,wire.data(),wire.size(),size));
  const uint8_t expected[]={0x08,0x47,0x12,0x06,0x08,0x01,0x10,0x02,0x18,0x3c,
                            0x35,0x78,0x56,0x34,0x12,0x48,0x00};
  assert(size==sizeof(expected)&&!std::memcmp(wire.data(),expected,size));
  auto data=decode(wire.data(),size);auto result=inner(data);
  assert(data.request_id==0x12345678&&data.payload.size==6&&result.neighbors_count==0);
  assert(result.node_id==1&&result.last_sent_by_id==2&&result.node_broadcast_interval_secs==60);
  pass("zero-neighbor snapshot retains identity/interval in exact canonical whole Data");

  auto populated=snapshot();populated.neighbors_count=2;
  populated.neighbors[0].node_id=0xdeadbeef;populated.neighbors[0].snr=-7.5f;
  populated.neighbors[0].last_rx_time=0x12345678;populated.neighbors[0].node_broadcast_interval_secs=3600;
  populated.neighbors[1].node_id=7;populated.neighbors[1].snr=4.25f;
  const auto original=populated;
  assert(mt::NeighborQuery::response(UINT32_MAX,populated,wire.data(),wire.size(),size));
  data=decode(wire.data(),size);result=inner(data);
  assert(data.request_id==UINT32_MAX&&result.neighbors_count==2);
  for(unsigned i=0;i<2;++i){
    assert(result.neighbors[i].node_id==original.neighbors[i].node_id);
    assert(result.neighbors[i].snr==original.neighbors[i].snr);
    assert(result.neighbors[i].last_rx_time==original.neighbors[i].last_rx_time);
    assert(result.neighbors[i].node_broadcast_interval_secs==original.neighbors[i].node_broadcast_interval_secs);
  }
  assert(populated.node_id==original.node_id&&populated.neighbors_count==original.neighbors_count);
  assert(populated.neighbors[0].node_id==original.neighbors[0].node_id);
  pass("populated snapshot and maximum request ID round trip without extra envelope claims");

  wire.fill(0xa5);size=999;
  assert(!mt::NeighborQuery::response(0,empty,wire.data(),wire.size(),size)&&size==0);
  for(auto byte:wire)assert(byte==0xa5);
  size=999;assert(!mt::NeighborQuery::response(1,empty,nullptr,320,size)&&size==0);
  size=999;assert(!mt::NeighborQuery::response(1,empty,nullptr,0,size)&&size==0);
  pass("zero request and null outputs fail with zero size and no caller writes");

  assert(mt::NeighborQuery::response(1,populated,wire.data(),wire.size(),size));
  const auto required=size;
  for(size_t capacity=0;capacity<=required;++capacity){
    std::array<uint8_t,336> guarded{};guarded.fill(0xa5);size=999;
    const bool ok=mt::NeighborQuery::response(1,populated,guarded.data()+8,capacity,size);
    assert(ok==(capacity==required)&&size==(ok?required:0));
    for(size_t i=0;i<8;++i)assert(guarded[i]==0xa5);
    for(size_t i=8+capacity;i<guarded.size();++i)assert(guarded[i]==0xa5);
    if(ok)(void)decode(guarded.data()+8,size);
  }
  pass("every insufficient capacity fails and exact capacity succeeds within guard bytes");

  auto large=snapshot();large.node_id=large.last_sent_by_id=large.node_broadcast_interval_secs=UINT32_MAX;
  large.neighbors_count=10;
  for(auto &neighbor:large.neighbors){
    neighbor.node_id=neighbor.last_rx_time=neighbor.node_broadcast_interval_secs=UINT32_MAX;
    neighbor.snr=1.0f;
  }
  size_t innerSize=0;
  assert(pb_get_encoded_size(&innerSize,meshtastic_NeighborInfo_fields,&large));
  assert(innerSize>233&&innerSize<=meshtastic_NeighborInfo_size);
  wire.fill(0xa5);size=999;
  assert(!mt::NeighborQuery::response(1,large,wire.data(),wire.size(),size)&&size==0);
  for(auto byte:wire)assert(byte==0xa5);
  large.neighbors_count=11;size=999;
  assert(!mt::NeighborQuery::response(1,large,wire.data(),wire.size(),size)&&size==0);
  for(auto byte:wire)assert(byte==0xa5);
  pass("oversized inner encoding and invalid repeated count refuse before caller output");

  auto ten=snapshot();ten.neighbors_count=10;
  for(unsigned i=0;i<10;++i)ten.neighbors[i].node_id=i+3;
  assert(mt::NeighborQuery::response(1,ten,wire.data(),wire.size(),size));
  data=decode(wire.data(),size);result=inner(data);
  assert(data.payload.size<=233&&result.neighbors_count==10);
  for(unsigned i=0;i<10;++i)assert(result.neighbors[i].node_id==i+3);
  pass("ten neighbors are accepted when their actual encoded payload fits");

  const auto canonicalMetadata=metadata(empty);
  for(uint8_t flag:{uint8_t(0),uint8_t(1)}){
    auto m=canonicalMetadata;m.bitfield=flag;size=999;
    assert(mt::NeighborQuery::responseShape(m));
    assert(mt::NeighborQuery::response(m,wire.data(),wire.size(),size));
    auto encoded=decode(wire.data(),size,flag);auto observed=inner(encoded);
    assert(encoded.request_id==m.request_id&&encoded.payload.size==m.payloadLength);
    assert(!std::memcmp(encoded.payload.bytes,m.payload,m.payloadLength));
    assert(observed.neighbors_count==0&&observed.node_id==empty.node_id);
    auto golden=std::array<uint8_t,sizeof(expected)>{};std::memcpy(golden.data(),expected,sizeof(expected));golden.back()=flag;
    assert(size==golden.size()&&!std::memcmp(wire.data(),golden.data(),size));
  }
  pass("response metadata rebuild preserves exact whole Data and explicit bitfield0/1");

  for(unsigned variant=0;variant<10;++variant){
    auto m=canonicalMetadata;
    switch(variant){
      case 0:m.portnum=67;break;case 1:m.request_id=0;break;case 2:m.hasBitfield=false;break;
      case 3:m.bitfield=2;break;case 4:m.wantResponse=true;break;case 5:m.reply_id=1;break;
      case 6:m.emoji=1;break;case 7:m.signatureLength=64;break;case 8:m.payloadLength=0;break;
      default:m.payloadLength=234;break;
    }
    wire.fill(0xa5);size=999;assert(!mt::NeighborQuery::responseShape(m));
    assert(!mt::NeighborQuery::response(m,wire.data(),wire.size(),size)&&size==0);
    for(auto byte:wire)assert(byte==0xa5);
  }
  pass("wrong response shape and empty/oversized inner reject without caller writes");

  const uint8_t malformed[][8]={
    {0x08,0x01,0x10,0x02,0x18,0x3c,0x28,0x01}, // unknown NeighborInfo field
    {0x08,0x01,0x10,0x02,0x18,0x3c,0x08,0x01}, // duplicate node_id
    {0x08,0x81,0x00,0x10,0x02,0x18,0x3c},      // nonminimal varint
    {0x10,0x02,0x08,0x01,0x18,0x3c},           // reordered fields
    {0x08,0x80},                              // truncated varint
    {0x0d,0x01,0x00,0x00,0x00}                // wrong wire type
  };
  const uint8_t malformedSizes[]={8,8,7,6,2,5};
  for(unsigned i=0;i<sizeof(malformedSizes)/sizeof(malformedSizes[0]);++i){
    auto m=canonicalMetadata;m.payloadLength=malformedSizes[i];std::memcpy(m.payload,malformed[i],m.payloadLength);
    wire.fill(0xa5);size=999;assert(mt::NeighborQuery::responseShape(m));
    assert(!mt::NeighborQuery::response(m,wire.data(),wire.size(),size)&&size==0);
    for(auto byte:wire)assert(byte==0xa5);
  }
  pass("unknown duplicate reordered nonminimal truncated and mistyped inner fail canonical validation");

  for(float snr:{std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}){
    auto info=snapshot();info.neighbors_count=1;info.neighbors[0].node_id=44;info.neighbors[0].snr=snr;
    const auto m=metadata(info);size=999;wire.fill(0xa5);
    assert(!mt::NeighborQuery::response(m,wire.data(),wire.size(),size)&&size==0);
    for(auto byte:wire)assert(byte==0xa5);
  }
  pass("nonfinite Neighbor SNR refuses before response output");

  assert(mt::NeighborQuery::response(canonicalMetadata,wire.data(),wire.size(),size));
  const auto responseSize=size;
  for(size_t capacity=0;capacity<responseSize;++capacity){
    std::array<uint8_t,336> guarded{};guarded.fill(0xa5);size=999;
    assert(!mt::NeighborQuery::response(canonicalMetadata,guarded.data()+8,capacity,size)&&size==0);
    for(size_t i=0;i<8;++i)assert(guarded[i]==0xa5);
    for(size_t i=8+capacity;i<guarded.size();++i)assert(guarded[i]==0xa5);
  }
  size=999;assert(!mt::NeighborQuery::response(canonicalMetadata,nullptr,320,size)&&size==0);
  pass("canonical response overload reports zero failure size within every capacity boundary");

  RecordingEncoder provider;mt::Endpoint responder(&provider),noProvider;wire.fill(0xa5);
  size=999;assert(!noProvider.neighborReplyData(1,0,wire.data(),wire.size(),size)&&size==0);
  size=999;assert(!responder.neighborReplyData(0,0,wire.data(),wire.size(),size)&&size==0);
  size=999;assert(!responder.neighborReplyData(1,0,nullptr,320,size)&&size==0);
  assert(provider.snapshots==0&&provider.encodes==0);
  provider.available=false;size=999;
  assert(!responder.neighborReplyData(1,0,wire.data(),wire.size(),size)&&size==0);
  provider.available=true;size=999;assert(!responder.neighborReplyData(1,0,wire.data(),0,size)&&size==0);
  assert(responder.neighborReplyAllowed(0));
  assert(responder.neighborReplyData(1,0,wire.data(),wire.size(),size));
  assert(responder.neighborReplyData(2,0,wire.data(),wire.size(),size));
  assert(responder.neighborReplyAllowed(0)&&responder.idle()&&responder.free()==4&&provider.encodes==0);
  pass("shared Endpoint prepare needs provider/request/output and never charges or queues");

  responder.neighborReplyAccepted(0);const auto calls=provider.snapshots;
  assert(!responder.neighborReplyAllowed(0)&&!responder.neighborReplyAllowed(179999));
  size=999;assert(!responder.neighborReplyData(3,179999,wire.data(),wire.size(),size)&&size==0);
  assert(provider.snapshots==calls&&responder.neighborReplyAllowed(180000));
  RecordingEncoder wrapProvider;mt::Endpoint wrap(&wrapProvider);wrap.neighborReplyAccepted(UINT32_MAX-89999u);
  // Modulo32 elapsed is179999 at89999, then exactly180000 at90000.
  assert(!wrap.neighborReplyAllowed(89999)&&wrap.neighborReplyAllowed(90000));
  size=999;assert(!wrap.neighborReplyData(4,89999,wire.data(),wire.size(),size)&&size==0);
  assert(wrapProvider.snapshots==0);
  assert(wrap.neighborReplyData(4,90000,wire.data(),wire.size(),size)&&wrapProvider.snapshots==1);
  pass("acceptedAt0 and exact wrap89999/90000 boundaries block all prepares without provider work");

  mt::Endpoint live;const auto frame=header(0x13579bdf);bool created=true;
  assert(submit(live,0,frame,true,&created)==0&&created);
  auto *job=live.next(0);assert(job&&job->neighborQuery&&job->owners==1&&live.free()==3);
  created=true;assert(submit(live,1,frame,false,&created)==-EINVAL&&!created);
  assert(live.next(0)==job&&job->owners==1&&job->state==mt::Endpoint::State::Queued&&live.free()==3);
  mt::Endpoint::Result notification;assert(!live.take(1,notification));
  created=true;assert(submit(live,1,frame,true,&created)==0&&!created&&job->owners==3&&live.free()==3);
  pass("live same-ID different query kind refuses before owner mutation; identical owners merge");

  live.refused(*job,4);assert(live.free()==4);
  assert(live.take(0,notification)&&notification.id==0x13579bdf);
  assert(live.take(1,notification)&&!live.take(1,notification));
  created=true;assert(submit(live,1,frame,false,&created)==-EINVAL&&!created&&!live.take(1,notification));
  created=true;assert(submit(live,1,frame,true,&created)==0&&!created);
  assert(live.take(1,notification)&&notification.id==0x13579bdf&&notification.kind==mt::Endpoint::Kind::Failed);
  assert(live.idle()&&live.free()==4);
  pass("history rejoin preserves query kind and refuses wrong-kind completion replay");

  mt::Endpoint validity;const auto newFrame=header(0x2468ace0);created=true;
  assert(validity.submit(0,newFrame.bytes,newFrame.len,false,0,false,true,false,&created,false,false,false,false,false,false,0,true)==-EINVAL&&!created);
  created=true;assert(validity.submit(0,newFrame.bytes,newFrame.len,false,0,false,true,true,&created,false,false,true,false,false,false,0,true)==-EINVAL&&!created);
  created=true;assert(submit(validity,2,newFrame,true,&created)==-EINVAL&&!created&&validity.free()==4);
  created=true;assert(submit(validity,0,newFrame,true,&created)==0&&created); // same ID still usable: refusals burned no nonce
  auto *first=validity.next(0);assert(first&&first->neighborQuery);validity.refused(*first,4);
  const auto legacy=header(0x10203040);assert(validity.submit(0,legacy.bytes,legacy.len,false,0)==0);
  auto *oldCall=validity.next(0);assert(oldCall&&!oldCall->neighborQuery&&!oldCall->wantResponse);
  pass("invalid Neighbor flags/owner reserve no nonce and the original submit call remains compatible");
  std::printf("GROUPS %u\n",groups);
}
