// SPDX-License-Identifier: GPL-3.0-only
// Synthetic radio with real channel encryption, PKI, replay ledger and Endpoint.
#include "pki_runtime_fixture.h"
#include "modules/peripherals/PaxcounterCore.h"
namespace {
using Pax=mt::modules::peripherals::PaxcounterCore;
struct Counts {
  Pax core;
  uint32_t now=100;
  unsigned reads=0;
  static bool snapshot(void *ctx,meshtastic_Paxcount &out) {
    auto &s=*static_cast<Counts *>(ctx);
    ++s.reads;
    return s.core.reply(s.now,s.now/1000,out);
  }
  void start(Fixture &f) {
    f.s.stop();
    mt::Config cfg=f.s.config;
    mt::ChannelRecord c;
    const uint8_t key[16]={0x75};
    assert(c.set(2,"Sensors",key,sizeof(key))&&cfg.setSecondaryChannel(1,c));
    assert(f.s.apply(cfg));
    assert(f.s.bindPaxSnapshot(snapshot,this));
    assert(!f.s.bindPaxSnapshot(snapshot,this));
    assert(f.s.start(false,0)&&f.s.armEndpoint(true));
    core.configure({true,60,0,0},0);
    const uint8_t addr[6]={1,2,3,4,5,6};
    assert(core.observe(Pax::Kind::Ble,addr,-70,now)==Pax::ObserveResult::Recorded);
  }
};
Bytes request(Fixture &f,bool privateLane,const Bytes &plain,uint32_t packetId,uint32_t from=0) {
  if(!from)from=f.peer.binding.node;
  const uint32_t to=f.local.binding.node;
  Bytes frame(255);put(frame.data(),to);put(frame.data()+4,from);put(frame.data()+8,packetId);
  frame[12]=(3<<5)|2;frame[15]=uint8_t(from);size_t size=plain.size();
  if(privateLane){
    assert(f.crypto.encrypt(f.peer.scalar,f.local.binding.publicKey,from,packetId,0x1978,
        plain.data(),plain.size(),frame.data()+16,239,size));
  }else{
    mt::PrimaryChannel::Selection channel;assert(f.s.config.channelSelection(1,channel));
    frame[13]=channel.hash;
    uint8_t nonce[16];bench::MtCodec::nonce(from,packetId,nonce);
    memcpy(frame.data()+16,plain.data(),size);
    assert(bench::mtAesCtr(nullptr,channel.bytes,channel.length,nonce,frame.data()+16,size));
    mt::PrimaryChannel::clear(channel);
  }
  frame.resize(16+size);return frame;
}
void checkResponse(Fixture &f,bool privateLane,const Bytes &query,unsigned expectedBle=1) {
  assert(f.radio.frames.size()==1);
  const auto &sent=f.radio.frames.front();
  const auto sender=bench::le32(sent.data()+4),packet=bench::le32(sent.data()+8);
  assert(bench::le32(sent.data())==f.peer.binding.node && sender==f.local.binding.node);
  assert(packet && packet!=bench::le32(query.data()+8) && sent[13]==query[13]);
  assert((sent[12]&8)==(query[12]&8) && (sent[12]&7)==mt::HopPolicy::response(3,2,f.s.config.hopLimit()));
  uint8_t plain[239];size_t size=sent.size()-16;
  if(privateLane) {
    assert(f.crypto.decrypt(f.peer.scalar,f.local.binding.publicKey,sender,packet,sent.data()+16,size,plain,sizeof(plain),size));
  } else {
    mt::PrimaryChannel::Selection channel;assert(f.s.config.channelSelection(1,channel));
    uint8_t nonce[16];bench::MtCodec::nonce(sender,packet,nonce);
    memcpy(plain,sent.data()+16,size);
    assert(bench::mtAesCtr(nullptr,channel.bytes,channel.length,nonce,plain,size));
    mt::PrimaryChannel::clear(channel);
  }
  meshtastic_Data data{};auto in=pb_istream_from_buffer(plain,size);
  assert(pb_decode(&in,meshtastic_Data_fields,&data)&&!in.bytes_left);
  assert(data.portnum==meshtastic_PortNum_PAXCOUNTER_APP && data.request_id==101 && !data.want_response);
  meshtastic_Paxcount count{};auto payload=pb_istream_from_buffer(data.payload.bytes,data.payload.size);
  assert(pb_decode(&payload,meshtastic_Paxcount_fields,&count)&&count.ble==expectedBle&&!count.wifi);
}
}
int main(){
  const Bytes selector(std::begin(mt::modules::pax::Request),std::end(mt::modules::pax::Request));
  for(bool privateLane:{false,true}){
    for(int mode=0;mode<10;++mode){
      Fixture f(true);Counts counts;counts.start(f);
      Bytes plain=selector;
      if(mode==1){plain.push_back(0x78);plain.push_back(0);}
      if(mode==8)plain.back()=3; // Stock origin allows MQTT forwarding.
      if(mode==9)plain.back()=7; // Reserved bit.
      auto q=request(f,privateLane,plain,101);
      if(mode==2)counts.now+=Pax::WindowMs;
      if(mode==3)q[12]|=0x10;
      if(mode==4)put(q.data(),UINT32_MAX);
      if(mode==5)counts.core.configure({false,60,0,0},100);
      if(mode==6)q[12]|=8;
      if(mode==7)q[12]=(2<<5)|3;
      f.rx(q,100);f.poll(800);
      if(mode && mode!=2 && mode!=6 && mode!=8){assert(f.radio.frames.empty());continue;}
      checkResponse(f,privateLane,q,mode==2?0:1);f.complete(950);
      f.rx(q,1010);f.poll(1400);
      assert(f.radio.frames.size()==1);
    }
  }
  {
    Fixture f(true);Counts counts;counts.start(f);
    for(unsigned i=0;i<5;++i)
      f.rx(request(f,false,selector,201+i,0x11220077+i),100+i);
    assert(counts.reads==4);
    f.poll(800);assert(f.radio.frames.size()==1);f.complete(901);
    f.rx(request(f,false,selector,206,0x1122007b),920);
    assert(counts.reads==5);
  }
  {
    Fixture f(true);Counts counts;counts.start(f);
    f.rx(request(f,false,selector,301),100);
    assert(counts.reads==1);
    meshtastic_Data audio{};audio.portnum=meshtastic_PortNum_AUDIO_APP;audio.has_bitfield=true;
    const uint8_t body[]={0xc0,0xde,0xc2,9,11,22,33};
    memcpy(audio.payload.bytes,body,sizeof(body));audio.payload.size=sizeof(body);
    uint8_t bytes[40];auto stream=pb_ostream_from_buffer(bytes,sizeof(bytes));
    assert(pb_encode(&stream,meshtastic_Data_fields,&audio));
    uint32_t id=0;
    assert(!f.s.submitModuleAnnouncement(bytes,stream.bytes_written,3,101,3501,id) && id);
    uint32_t refused=0;
    assert(f.s.submitModuleAnnouncement(bytes,stream.bytes_written,3,102,3502,refused)==-EAGAIN && !refused);
    f.poll(800);
    assert(f.radio.frames.size()==1);
    const auto &sent=f.radio.frames[0];
    assert(bench::le32(sent.data()+8)==id && bench::le32(sent.data())==UINT32_MAX && !(sent[12]&8));
  }
  puts("PASS audio broadcast and MAX transmit priority over queued Pax, retained module admission under pressure");
  puts("PASS full response queue skips snapshot and admits new Pax query after a slot frees");
  puts("PASS fresh Pax replies, request ID, selected secondary channel, authenticated PKI, duplicate, stale, malformed, disabled, broadcast and MQTT gates");
}
