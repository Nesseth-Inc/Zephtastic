// SPDX-License-Identifier: GPL-3.0-only
#include "position_query_fixture.h"
#include "stock/optional/ReplyWire.h"
void endpointCases(){
  for(auto port:{meshtastic_PortNum_REPLY_APP,meshtastic_PortNum_TEXT_MESSAGE_APP}){
    auto f=std::make_unique<Fixture>();meshtastic_Data d{};
    d.portnum=port;d.want_response=port==meshtastic_PortNum_REPLY_APP;d.has_bitfield=true;d.bitfield=d.want_response?2:0;
    static constexpr char ping[]="/ping";d.payload.size=5;memcpy(d.payload.bytes,ping,5);
    uint8_t encoded[64];auto pb=pb_ostream_from_buffer(encoded,sizeof(encoded));assert(pb_encode(&pb,meshtastic_Data_fields,&d));
    auto packet=rawFrame({encoded,encoded+pb.bytes_written},PEER,OURS,721,uint8_t(3u<<5|1u));
    auto recv=[&](uint32_t now){f->s.rx(packet.data(),packet.size(),-2,now,133,true,nullptr,false,0,0,-82,-2);
      // No companion receives the frame. A standalone radio must still answer.
      f->s.flushStockReply(now,4519);};
    auto viaMqtt=packet;viaMqtt[12]|=0x10u;
    f->s.rx(viaMqtt.data(),viaMqtt.size(),-2,25,121,true,nullptr,false,0,0,-82,-2);
    f->s.rx(packet.data(),packet.size(),-2,30,122,false,nullptr,false,0,0,-82,-2);
    f->s.flushStockReply(31,432);f->s.poll(50,133);assert(f->radio.frames.empty());
    recv(100);f->s.poll(500,134);assert(f->radio.sends==1);
    const auto &frame=f->radio.frames.back();assert(bench::le32(frame.data())==PEER);
    bench::RouteMetadata reply{};assert(f->s.codec.decode(frame.data(),frame.size(),reply));
    assert(reply.portnum==uint32_t(port)&&!reply.wantResponse&&!(frame[12]&8));
    std::string body(reinterpret_cast<char*>(reply.payload),reply.payloadLength);
    if(d.want_response){assert(reply.request_id==721&&body=="Message Received");}
    else {assert(!reply.request_id&&body=="🎙️ Mic Check : 2 Hops away | RSSI -82 | SNR -2.0");}
    f->done(510);recv(700);f->s.poll(1000,132);assert(f->radio.sends==1);
  }
  auto f=std::make_unique<Fixture>();
  const auto node=f->s.config.node();
  std::string question(reinterpret_cast<const char*>(node.bytes+bench::MtNodeRecord::Short),strnlen(reinterpret_cast<const char*>(node.bytes+bench::MtNodeRecord::Short),4));
  question+=" conditions";
  auto packet=rawFrame(encodeData({question.begin(),question.end()},0,1),PEER,UINT32_MAX,725);
  f->s.rx(packet.data(),packet.size(),-2,600,125);
  // Dropzone is likewise independent of phone attachment or console permissions.
  assert(!f->s.takeDropzoneRequest(5599)&&f->s.takeDropzoneRequest(5600));
  char report[200];mt::stock::optional::Conditions weather{};weather.open=true;
  mt::stock::optional::DropzoneModule::format(weather,3723,report);
  uint32_t id=0;
  assert(!f->s.submitLocalInput(meshtastic_PortNum_TEXT_MESSAGE_APP,reinterpret_cast<const uint8_t*>(report),strlen(report),0,false,5700,812,id)&&id);
  f->s.poll(6100,125);assert(f->radio.frames.size()==1);
  const auto &sent=f->radio.frames[0];bench::RouteMetadata result{};
  assert(bench::le32(sent.data())==UINT32_MAX&&f->s.codec.decode(sent.data(),sent.size(),result)&&result.portnum==1);
  assert(std::string(reinterpret_cast<char*>(result.payload),result.payloadLength)=="OPEN @ 01:02:03\nNo sensor found");
  puts("PASS optional RF endpoint replies with real channel bytes, metadata and single dispatch across USB/BLE");
}
