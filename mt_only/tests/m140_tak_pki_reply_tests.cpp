// SPDX-License-Identifier: GPL-3.0-only
// Real-PSA fixture case. Root's frozen campaign runner owns execution.
#include "pki_runtime_fixture.h"
#include "modules/TakPliReplyValidation.h"
#include "HopPolicy.h"

static void configureTak(Fixture &f){
  f.s.stop();auto config=f.s.config;
  assert(config.setDeviceRole(10,0)&&config.setFixedPosition(475123456,-1221234567,true,250));
  config.setPositionSharing(true);assert(config.setPrimaryPositionPrecision(15));
  assert(f.s.apply(config)&&f.s.start(false,0)&&f.s.armEndpoint(true));
  assert(f.s.takConfiguration(uint32_t(meshtastic_Team_Blue),uint32_t(meshtastic_MemberRole_Medic)));
}
static Bytes privatePositionQuery(Fixture &f,uint32_t packetId,bool wantAck=true){
  const uint8_t expected[]={8,3,24,1,72,2};
  uint8_t encrypted[239]{};size_t encryptedSize=0;
  assert(f.crypto.encrypt(f.peer.scalar,f.local.binding.publicKey,f.peer.binding.node,packetId,
    0x9271a4c3,expected,sizeof(expected),encrypted,sizeof(encrypted),encryptedSize));
  Bytes frame(encryptedSize+16);put(frame.data(),f.local.binding.node);put(frame.data()+4,f.peer.binding.node);put(frame.data()+8,packetId);
  frame[12]=uint8_t(4u|(4u<<5)|(wantAck?8u:0u)); // start=4, remaining=4
  frame[15]=uint8_t(f.peer.binding.node);memcpy(frame.data()+16,encrypted,encryptedSize);return frame;
}
static void privatePositionReply(){
  Fixture f(true);configureTak(f);constexpr uint32_t request=0x7193;
  const auto inbound=privatePositionQuery(f,request,true);const unsigned writesBefore=f.lm.writes;
  f.rx(inbound,1500);assert(f.lm.writes>writesBefore);f.poll(1750);
  assert(f.radio.sends==1&&f.radio.frames.size()==1);
  const auto &frame=f.radio.frames[0];assert(frame.size()>=29&&bench::le32(frame.data())==f.peer.binding.node&&
    bench::le32(frame.data()+4)==f.local.binding.node&&bench::le32(frame.data()+8)!=request&&
    (frame[12]&8u)&&((frame[12]&7u)==mt::HopPolicy::response(4,4,f.s.config.hopLimit()))&&
    (((frame[12]>>5)&7u)==mt::HopPolicy::response(4,4,f.s.config.hopLimit()))&&frame[13]==0);
  const uint32_t replyId=bench::le32(frame.data()+8);assert(f.ledger.sent(replyId));
  uint8_t plaintext[227]{};size_t plaintextSize=0;
  assert(f.crypto.decrypt(f.peer.scalar,f.local.binding.publicKey,f.local.binding.node,replyId,
    frame.data()+16,frame.size()-16,plaintext,sizeof(plaintext),plaintextSize));
  Key wrongRecipient{0x52};uint8_t wrongPlaintext[227]{};size_t wrongSize=77;
  assert(!f.crypto.decrypt(wrongRecipient.scalar,f.local.binding.publicKey,f.local.binding.node,replyId,
    frame.data()+16,frame.size()-16,wrongPlaintext,sizeof(wrongPlaintext),wrongSize)&&wrongSize==0);
  meshtastic_Data data{};auto in=pb_istream_from_buffer(plaintext,plaintextSize);
  assert(pb_decode(&in,meshtastic_Data_fields,&data)&&!in.bytes_left);
  assert(data.portnum==meshtastic_PortNum_ATAK_PLUGIN_V2&&data.request_id==request&&
    !data.reply_id&&!data.want_response&&data.has_bitfield&&data.bitfield==0&&
    data.payload.size>1&&data.payload.size<=233&&data.payload.bytes[0]==0xff&&
    mt::tak_pli_reply::canonical(data.payload.bytes,data.payload.size));
  // A replay of the authenticated query cannot cause a second private or public reply.
  f.complete(1900);f.rx(inbound,2100);f.poll(2350);assert(f.radio.sends==1&&f.radio.frames.size()==1);
  std::cout<<"PASS authenticated private port-3 position query yields one durable fresh-ID encrypted port-78 reply with request correlation, ACK/hop, and recipient binding\n";
}
static void failedReplyIdStorageDoesNotFallback(){
  Fixture f(true);configureTak(f);constexpr uint32_t request=0x81a2;
  auto inbound=privatePositionQuery(f,request,true);unsigned writes=0;
  f.lm.onWrite=[&]{assert(f.s.privateStoragePaused()&&!f.radio.healthValue&&!f.s.active());if(++writes==2)f.lm.fault=1;};
  f.rx(inbound,1500);f.poll(1750);
  assert(writes>=2&&f.radio.frames.empty()&&f.radio.sends==0);
  std::cout<<"PASS failed durable reply-ID reservation after authenticated ingress emits neither private nor public fallback\n";
}
static void customChannelReply(){
  Fixture f(true);configureTak(f);f.s.stop();auto config=f.s.config;
  const uint8_t key[16]={0x4a,0x57,0x14,0x92,0x13,0x51,0x16,0xa0,0xb9,0x82,0x26,0x18,0xfa,0x27,0x6b,0x3c};
  mt::ChannelRecord secondary;assert(secondary.set(2,"TAK-test",key,sizeof(key),8)&&config.setSecondaryChannel(1,secondary));
  assert(f.s.apply(config)&&f.s.start(false,0)&&f.s.armEndpoint(true));
  mt::PrimaryChannel::Selection selected;assert(config.channelSelection(1,selected));
  bench::MtCodec custom;assert(custom.setKey(selected.hash,selected.bytes,selected.length));
  bench::Frame query;size_t size=0;constexpr uint32_t request=0x3192;
  assert(custom.encodePositionQuery(f.peer.binding.node,f.local.binding.node,request,query.bytes,sizeof(query.bytes),size));query.len=size;
  const auto beforeWrites=f.lm.writes;f.rx(Bytes(query.bytes,query.bytes+query.len),1500);f.poll(1750);
  assert(f.radio.frames.size()==1&&f.s.selectedChannel()==0&&f.s.receiving()&&!f.s.faulted());
  const auto &frame=f.radio.frames[0];const auto id=bench::le32(frame.data()+8);
  assert(frame[13]==selected.hash&&f.ledger.sent(id)&&f.lm.writes==beforeWrites+1);
  bench::RouteMetadata m;assert(custom.decode(frame.data(),frame.size(),m)&&m.portnum==78&&m.request_id==request);
  uint8_t canonical[239]{};size_t written=0;
  assert(mt::tak_pli_reply::response(m,canonical,sizeof(canonical),written)&&
    custom.validatePositionQueryReply(frame.data(),frame.size(),canonical,written,78)&&
    !f.s.codec.validatePositionQueryReply(frame.data(),frame.size(),canonical,written,78));
  meshtastic_TAKPacketV2 pli{};auto in=pb_istream_from_buffer(m.payload+1,m.payloadLength-1);
  assert(pb_decode(&in,meshtastic_TAKPacketV2_fields,&pli)&&!in.bytes_left);
  meshtastic_Position expected{};assert(f.s.localPosition(expected));mt::PositionPrecision::apply(expected,8);
  assert(pli.latitude_i==expected.latitude_i&&pli.longitude_i==expected.longitude_i);
  mt::PrimaryChannel::clear(selected);
  std::cout<<"PASS real Service custom-channel TAK reply uses received precision/key, one durable ID and restored primary scope with no default-key fallback\n";
}
static void primaryPrivacyForPrivateReply(){
  Fixture f(true);configureTak(f);f.s.stop();auto config=f.s.config;assert(config.setPrimaryPositionPrecision(0));
  const uint8_t key[16]={1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
  mt::ChannelRecord secondary;assert(secondary.set(2,"TAK-test",key,sizeof(key),15)&&config.setSecondaryChannel(1,secondary));
  assert(f.s.apply(config)&&f.s.start(false,0)&&f.s.armEndpoint(true));
  {
    mt::Service::ChannelScope scope(f.s,1);assert(scope.valid());
    f.rx(privatePositionQuery(f,0x7293,false),1500);
  }
  f.poll(1750);assert(f.radio.frames.empty()&&!f.s.faulted()&&f.s.receiving()&&f.s.selectedChannel()==0);
  std::cout<<"PASS PKI reply obeys zero primary position consent even under a temporary share-enabled custom-channel scope\n";
}
int main(){privatePositionReply();failedReplyIdStorageDoesNotFallback();customChannelReply();primaryPrivacyForPrivateReply();}
