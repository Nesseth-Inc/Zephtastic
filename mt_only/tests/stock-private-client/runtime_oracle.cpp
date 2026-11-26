// SPDX-License-Identifier: GPL-3.0-only
// Host-only actual runtime adapter. Shared fixture owns public deterministic TEST
// keys, memory journals, actual PSA crypto and an inert in-memory radio.
#include "pki_runtime_fixture.h"
#include <memory>
static std::string hex(const uint8_t *p,size_t n){const char *d="0123456789abcdef";std::string r;for(size_t i=0;i<n;++i){r+=d[p[i]>>4];r+=d[p[i]&15];}return r;}
static Bytes unhex(const std::string &s){assert(!s.empty()&&s.size()%2==0&&s.size()<=1024);Bytes b;for(size_t i=0;i<s.size();i+=2){unsigned v=0;for(unsigned j=0;j<2;++j){char c=s[i+j];assert((c>='0'&&c<='9')||(c>='a'&&c<='f'));v=v*16+unsigned(c<='9'?c-'0':c-'a'+10);}b.push_back(uint8_t(v));}return b;}
static void records(const std::vector<Bytes> &v){std::cout<<'[';bool first=true;for(auto &b:v){if(!first)std::cout<<',';first=false;std::cout<<'"'<<hex(b.data(),b.size())<<'"';}std::cout<<']';}
static void phase(Fixture &f,const char *name){std::cout<<",\""<<name<<"\":{\"usb\":";records(f.usb.records);std::cout<<",\"ble\":";records(f.ble.records);std::cout<<'}';f.usb.records.clear();f.ble.records.clear();}
static void submit(Fixture &f,bool ble,const Bytes &b){auto &c=ble?f.bc:f.c;if(ble)c.packet(b.data(),b.size(),100,mt::Companion::WorkspaceProof{true});else{for(uint8_t x:{uint8_t(0x94),uint8_t(0xc3),uint8_t(b.size()>>8),uint8_t(b.size())})c.byte(x,100,0,mt::Companion::WorkspaceProof{true});for(uint8_t x:b)c.byte(x,100,0,mt::Companion::WorkspaceProof{true});}}
int main(int argc,char **argv){
  assert(argc==2);
  if(std::string(argv[1])=="info"){Fixture f(true);std::cout<<"{\"local\":"<<f.local.binding.node<<",\"peer\":"<<f.peer.binding.node<<",\"local_public\":\""<<hex(f.local.binding.publicKey,32)<<"\",\"peer_public\":\""<<hex(f.peer.binding.publicKey,32)<<"\"}\n";return 0;}
  bool ble=std::string(argv[1])=="ble";assert(ble||std::string(argv[1])=="usb");std::string line;unsigned count=0;
  while(std::getline(std::cin,line)){
    assert(++count<=128);const auto split=line.find(' ');assert(split!=std::string::npos);const auto mode=line.substr(0,split);auto bytes=unhex(line.substr(split+1));
    auto fp=std::make_unique<Fixture>(true);auto &f=*fp;
    meshtastic_ToRadio request{};auto in=pb_istream_from_buffer(bytes.data(),bytes.size());assert(pb_decode(&in,meshtastic_ToRadio_fields,&request));const auto id=request.packet.id;
    if(mode=="crypto_failure")f.backend.failure=true;
    else if(mode=="entropy_failure")f.entropy.fail=true;
    else if(mode=="unarmed")assert(f.s.armEndpoint(false));
    else assert(mode=="normal"||mode=="authenticated_nak");
    std::cout<<"{\"id\":"<<id;
    // Exercise capability readback from the same concrete provider used below.
    meshtastic_ToRadio hello{};hello.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;hello.want_config_id=0x4d10ffff;input(f.c,hello,2);phase(f,"handshake");
    submit(f,ble,bytes);f.poll();phase(f,"submitted");
    if(f.radio.frames.size()==1){
      const auto sent=f.radio.frames[0];assert(sent.size()>28&&!sent[13]&&bench::le32(sent.data())==f.peer.binding.node&&bench::le32(sent.data()+4)==f.local.binding.node&&bench::le32(sent.data()+8)==id);
      uint8_t plain[227]{};size_t n=0;assert(f.crypto.decrypt(f.peer.scalar,f.local.binding.publicKey,f.local.binding.node,id,sent.data()+16,sent.size()-16,plain,sizeof(plain),n));
      std::cout<<",\"decrypted_data\":\""<<hex(plain,n)<<'"';
      f.complete();phase(f,"completed");f.rx(f.ack(id,false),1500);f.poll(1501);phase(f,"unproven_ack");
      const unsigned code=mode=="authenticated_nak"?33:0;f.rx(f.ack(id,true,code),1600);f.poll(1601);phase(f,"proven_ack");
      // A separately encrypted incoming reply/reaction exercises the complete
      // authenticate -> durable replay -> both companion parsers path.
      meshtastic_Data received{};received.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;received.has_bitfield=true;received.reply_id=id;received.emoji=1;
      const char text[]="M10 authenticated reply";received.payload.size=sizeof(text)-1;memcpy(received.payload.bytes,text,sizeof(text)-1);
      auto enc=pb_ostream_from_buffer(plain,sizeof(plain));assert(pb_encode(&enc,meshtastic_Data_fields,&received));
      std::cout<<",\"incoming_data\":\""<<hex(plain,enc.bytes_written)<<'"';
      constexpr uint32_t incomingId=0x4d400001;uint8_t cipher[239];size_t cn=0;assert(f.crypto.encrypt(f.peer.scalar,f.local.binding.publicKey,f.peer.binding.node,incomingId,0x12345678,plain,enc.bytes_written,cipher,sizeof(cipher),cn));
      Bytes frame(cn+16);put(frame.data(),f.local.binding.node);put(frame.data()+4,f.peer.binding.node);put(frame.data()+8,incomingId);frame[12]=8;frame[15]=uint8_t(f.peer.binding.node);memcpy(frame.data()+16,cipher,cn);
      auto bad=frame;bad[16]^=1;const auto before=f.lm.writes;f.rx(bad,1700);assert(f.lm.writes==before);phase(f,"tampered_receive");
      PkiLedger::Status initial{};assert(f.ledger.status(initial));
      const auto usbBefore=f.delivered(f.usb),bleBefore=f.delivered(f.ble);
      f.rx(frame,1800);phase(f,"valid_receive");const auto after=f.lm.writes;assert(after==before+2);
      PkiLedger::Status accepted{};assert(f.ledger.status(accepted)&&accepted.received==initial.received+1&&accepted.sent==initial.sent+1&&accepted.generation==initial.generation+2);
      const auto usbDelivered=f.delivered(f.usb),bleDelivered=f.delivered(f.ble);assert(usbDelivered==usbBefore+1&&bleDelivered==bleBefore+1);
      f.poll(2300);assert(f.radio.frames.size()==2);f.complete(2500);
      // The one exact duplicate may reserve a fresh proof ACK, but cannot
      // recommit RX or deliver another plaintext copy to either transport.
      f.rx(frame,2600);assert(f.lm.writes==after+1);phase(f,"duplicate_receive");
      PkiLedger::Status repeated{};assert(f.ledger.status(repeated)&&repeated.received==accepted.received&&repeated.sent==accepted.sent+1&&repeated.generation==accepted.generation+1&&repeated.stored==accepted.stored&&repeated.active==accepted.active&&repeated.format==accepted.format);
      assert(f.delivered(f.usb)==usbDelivered&&f.delivered(f.ble)==bleDelivered);
      f.poll(3000);assert(f.radio.frames.size()==3);
      const auto firstAckId=bench::le32(f.radio.frames[1].data()+8),repeatAckId=bench::le32(f.radio.frames[2].data()+8);
      assert(firstAckId&&repeatAckId&&firstAckId!=repeatAckId&&f.ledger.sent(firstAckId)&&f.ledger.sent(repeatAckId));
      for(size_t index:{size_t(1),size_t(2)}){
        bench::RouteMetadata m;const auto &ack=f.radio.frames[index];
        assert(f.s.codec.decode(ack.data(),ack.size(),m)&&m.portnum==meshtastic_PortNum_ROUTING_APP&&m.request_id==incomingId&&m.ackProofLength==8&&!(ack[12]&8));
        assert(bench::le32(ack.data())==f.peer.binding.node&&bench::le32(ack.data()+4)==f.local.binding.node);
        mt::experimental::PkiAck proof(f.crypto);uint8_t route[233]{};size_t rn=0;
        const auto verdict=proof.verify(f.peer.scalar,f.local.binding.publicKey,{true,f.local.binding.node,f.peer.binding.node,incomingId},{true,bench::le32(ack.data()+4),bench::le32(ack.data()),m.request_id},m.payload,m.payloadLength,route,sizeof(route),rn);
        assert(verdict==mt::experimental::AckProofVerdict::ValidProof);meshtastic_Routing routing{};auto stream=pb_istream_from_buffer(route,rn);assert(pb_decode(&stream,meshtastic_Routing_fields,&routing)&&!stream.bytes_left&&routing.error_reason==meshtastic_Routing_Error_NONE);
      }
      f.complete(3200);const auto quotaWrites=f.lm.writes;
      for(uint32_t now:{3300u,3400u,3500u}){
        f.rx(frame,now);PkiLedger::Status quota{};assert(f.ledger.status(quota)&&quota.generation==repeated.generation&&quota.sent==repeated.sent&&quota.received==repeated.received&&quota.stored==repeated.stored&&quota.active==repeated.active&&quota.format==repeated.format);
        assert(f.lm.writes==quotaWrites&&f.delivered(f.usb)==usbDelivered&&f.delivered(f.ble)==bleDelivered);
      }
      phase(f,"quota_duplicate_receive");f.poll(4000);assert(f.radio.frames.size()==3&&f.lm.writes==quotaWrites);
      std::cout<<",\"incoming_ack_verified\":true,\"duplicate_ack_verified\":true,\"duplicate_ack_quota_verified\":true";phase(f,"receive_ack");
    }
    std::cout<<",\"frames\":";records(f.radio.frames);std::cout<<",\"nonce_entropy_calls\":"<<f.entropy.calls<<",\"encrypts\":"<<f.backend.encrypts<<",\"decrypts\":"<<f.backend.decrypts<<",\"parse_errors\":"<<f.c.parseErrors+f.bc.parseErrors<<",\"runtime_ready\":"<<(f.runtime.ready(f.local.binding.node)?"true":"false")<<"}\n";
  }
}
