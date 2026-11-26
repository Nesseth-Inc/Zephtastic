// SPDX-License-Identifier: GPL-3.0-only
#include "Companion.h"
#include <cassert>
#include <vector>
#include <iostream>
#include <chrono>
#include <unistd.h>
struct Radio:mt::Radio {
 int starts=0,sends=0,sendResult=-EPERM;bool done=false,busyNow=false;std::vector<uint8_t> last;bool ready()override{return true;}int start()override{++starts;return 0;}
 int restore()override{return 0;}int stop()override{return 0;}int health()override{return 1;}
 bool busy()override{return busyNow;}uint32_t airtime(size_t)override{return 100;}
 int send(uint8_t *p,size_t n)override{++sends;last.assign(p,p+n);return sendResult;}bool completion(int &r)override{r=0;return done;}
};
struct Host:mt::Companion::Host {
 mt::Service &s;bool stream=false,storage=true;unsigned previews=0,pings=0;std::vector<meshtastic_FromRadio> received;
 explicit Host(mt::Service &v):s(v){}
 bool send(const uint8_t *p,size_t n)override {
  if(stream){while(n){ssize_t w=write(1,p,n);if(w<=0)return false;p+=w;n-=w;}return true;}
  assert(n>=4&&p[0]==0x94&&p[1]==0xc3&&size_t(p[2]*256+p[3])==n-4);
  meshtastic_FromRadio r{};auto b=pb_istream_from_buffer(p+4,n-4);assert(pb_decode(&b,meshtastic_FromRadio_fields,&r));received.push_back(r);return true;
 }
 bool ready()const override{return storage;}
 void nodeInfoPing(uint32_t now)override{++pings;assert(s.phoneNodeInfoPing(now,42)==-EACCES);}
 void preview(uint32_t,uint32_t,uint32_t,unsigned,const uint8_t *,size_t,int16_t,int8_t,uint32_t,bool)override{++previews;}
 int save(const mt::Config &c)override{if(!storage)return -EIO;bool rx=s.receiving();s.stop();if(!s.apply(c))return -EINVAL;return rx&&c.provisioned()&&!s.start(false,1)?-EIO:0;}
};
static void send(mt::Companion &c,const meshtastic_ToRadio &r,uint32_t now=1) {
 uint8_t bytes[512];auto s=pb_ostream_from_buffer(bytes,sizeof(bytes));assert(pb_encode(&s,meshtastic_ToRadio_fields,&r));
 c.byte(0x94,now,0,mt::Companion::WorkspaceProof{true});c.byte(0xc3,now,0,mt::Companion::WorkspaceProof{true});c.byte(s.bytes_written>>8,now,0,mt::Companion::WorkspaceProof{true});c.byte(s.bytes_written,now,0,mt::Companion::WorkspaceProof{true});
 for(size_t i=0;i<s.bytes_written;++i)c.byte(bytes[i],now,0,mt::Companion::WorkspaceProof{true});
}
static void admin(mt::Companion &c,const meshtastic_AdminMessage &a,uint32_t id=99) {
 meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_packet_tag;auto &p=r.packet;
 p.to=0x12345678;p.id=id;p.pki_encrypted=true;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
 p.decoded.portnum=meshtastic_PortNum_ADMIN_APP;
 auto s=pb_ostream_from_buffer(p.decoded.payload.bytes,sizeof(p.decoded.payload.bytes));assert(pb_encode(&s,meshtastic_AdminMessage_fields,&a));
 p.decoded.payload.size=s.bytes_written;send(c,r);
}
static meshtastic_Routing_Error error(const Host &h) {
 auto &p=h.received.back().packet;assert(p.decoded.portnum==meshtastic_PortNum_ROUTING_APP);
 meshtastic_Routing r{};auto s=pb_istream_from_buffer(p.decoded.payload.bytes,p.decoded.payload.size);assert(pb_decode(&s,meshtastic_Routing_fields,&r));return r.error_reason;
}
static std::vector<uint8_t> textFrame(uint32_t to,uint32_t id,uint32_t from=0x87654321) {
 std::vector<uint8_t> frame(255);auto put=[&](int n,uint32_t v){for(int i=0;i<4;++i)frame[n+i]=v>>(8*i);};
 put(0,to);put(4,from);put(8,id);frame[13]=bench::MtNodeRecord::publicHash();
 meshtastic_Data d{};d.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;d.payload.size=5;memcpy(d.payload.bytes,"Hello",5);d.has_bitfield=true;
 auto stream=pb_ostream_from_buffer(frame.data()+16,239);assert(pb_encode(&stream,meshtastic_Data_fields,&d));frame.resize(16+stream.bytes_written);
 uint8_t nonce[16];bench::MtCodec::nonce(from,id,nonce);
 assert(bench::mtAesCtr(nullptr,bench::MtNodeRecord::PublicKey,16,nonce,frame.data()+16,stream.bytes_written));return frame;
}
int main(int argc,char **) {
 static Radio radio;static mt::Service service(radio,false,0x12345678,81,argc>1);static Host host(service);static mt::companion::CompanionWorkspace workspace;static mt::Companion c(service,host,81,workspace);
 if(argc>1){host.stream=true;uint8_t b;auto start=std::chrono::steady_clock::now();while(read(0,&b,1)==1)c.byte(b,std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count(),0,mt::Companion::WorkspaceProof{true});return 0;}
 meshtastic_ToRadio request{};request.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;request.want_config_id=123;send(c,request);
 assert(c.handshakes==1&&host.received.front().my_info.my_node_num==0x12345678&&host.received.back().config_complete_id==123);
 assert(radio.starts==0&&radio.sends==0);assert(!service.receiving());
 meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_set_owner_tag;strcpy(a.set_owner.long_name,"MT Companion");strcpy(a.set_owner.short_name,"MTC");admin(c,a);assert(error(host)==meshtastic_Routing_Error_NONE);
 assert(!memcmp(service.config.node().bytes+bench::MtNodeRecord::Name,"MT Companion",12));
 a={};a.which_payload_variant=meshtastic_AdminMessage_set_channel_tag;a.set_channel.index=0;a.set_channel.role=meshtastic_Channel_Role_PRIMARY;a.set_channel.has_settings=true;a.set_channel.settings.psk.size=1;a.set_channel.settings.psk.bytes[0]=1;
 admin(c,a);assert(error(host)==meshtastic_Routing_Error_NONE);assert(service.config.node().isPublic());
 a.set_channel.settings.psk.bytes[0]=2;admin(c,a);assert(error(host)==meshtastic_Routing_Error_BAD_REQUEST&&service.config.node().isPublic());
 a={};a.which_payload_variant=meshtastic_AdminMessage_set_config_tag;a.set_config.which_payload_variant=meshtastic_Config_lora_tag;
 auto &l=a.set_config.payload_variant.lora;l.region=meshtastic_Config_LoRaConfig_RegionCode_US;l.use_preset=true;l.channel_num=20;l.sx126x_rx_boosted_gain=true;l.hop_limit=3;
 l.tx_enabled=true;admin(c,a);assert(error(host)==meshtastic_Routing_Error_BAD_REQUEST&&radio.starts==0);
 l.tx_enabled=false;l.sx126x_rx_boosted_gain=false;admin(c,a);
 assert(error(host)==meshtastic_Routing_Error_BAD_REQUEST&&!service.receiving()&&radio.starts==0);
 l.sx126x_rx_boosted_gain=true;admin(c,a);assert(error(host)==meshtastic_Routing_Error_NONE&&service.receiving());
 auto frame=textFrame(0xffffffff,100);size_t before=host.received.size();c.receive(frame.data(),frame.size(),-90,4,200,mt::Companion::WorkspaceProof{true});assert(host.received.size()==before+1&&c.receivedMessages==1);
 assert(host.received.back().packet.decoded.portnum==meshtastic_PortNum_TEXT_MESSAGE_APP);assert(!memcmp(host.received.back().packet.decoded.payload.bytes,"Hello",5));
 c.receive(frame.data(),frame.size(),-90,4,201,mt::Companion::WorkspaceProof{true});assert(host.received.size()==before+1);
 for(uint32_t from:{0u,1u,0xfffffffeu,0xffffffffu}) {
  auto invalid=textFrame(0xffffffff,777,from);c.receive(invalid.data(),invalid.size(),-90,4,202,mt::Companion::WorkspaceProof{true});
  assert(host.received.size()==before+1);
 }
 auto zeroId=textFrame(0xffffffff,0);c.receive(zeroId.data(),zeroId.size(),-90,4,202,mt::Companion::WorkspaceProof{true});assert(host.received.size()==before+1);
 frame=textFrame(0x77777777,101);c.receive(frame.data(),frame.size(),-90,4,202,mt::Companion::WorkspaceProof{true});assert(host.received.size()==before+1);
 frame=textFrame(0x12345678,102);c.receive(frame.data(),frame.size(),-90,4,203,mt::Companion::WorkspaceProof{true});assert(c.receivedMessages==2);
 // Owner updates preserve explicit receive intent; invalid headers do not enter client/cache.
 a={};a.which_payload_variant=meshtastic_AdminMessage_set_owner_tag;strcpy(a.set_owner.short_name,"TEST");admin(c,a);assert(service.receiving());
 before=host.received.size();frame=textFrame(0xffffffff,103);frame[12]=(1<<5)|2;c.receive(frame.data(),frame.size(),-90,4,204,mt::Companion::WorkspaceProof{true});assert(host.received.size()==before);
 frame=textFrame(0xffffffff,104);for(unsigned i=0;i<4;++i)frame[4+i]=0x12345678>>(8*i);c.receive(frame.data(),frame.size(),-90,4,205,mt::Companion::WorkspaceProof{true});assert(host.received.size()==before);
 request={};request.which_payload_variant=meshtastic_ToRadio_packet_tag;request.packet.id=1;request.packet.to=0xffffffff;request.packet.which_payload_variant=meshtastic_MeshPacket_decoded_tag;request.packet.decoded.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;
 send(c,request);assert(error(host)==meshtastic_Routing_Error_NO_INTERFACE&&radio.sends==0);
 request.packet.to=0x12345678;send(c,request);assert(error(host)==meshtastic_Routing_Error_NONE&&c.localMessages==1&&radio.sends==0);
 request={};request.which_payload_variant=meshtastic_ToRadio_heartbeat_tag;for(uint32_t nonce:{0u,2u,UINT32_MAX}){
   request.heartbeat.nonce=nonce;before=host.received.size();send(c,request);
   assert(host.received.size()==before+1&&host.received.back().which_payload_variant==meshtastic_FromRadio_queueStatus_tag);
   assert(host.received.back().queueStatus.mesh_packet_id==0&&host.pings==0);
 }
 before=host.received.size();request.heartbeat.nonce=1;send(c,request);
 assert(host.pings==1&&host.received.size()==before&&radio.sends==0&&service.nodeDiscovery.queued==0);
 host.storage=false;send(c,request);host.storage=true;assert(host.pings==1);
 // A cached NodeInfo key cannot be changed/removed by an unsigned update,
 // through either node_info or raw packet delivery.
 meshtastic_User u{};strcpy(u.long_name,"Known public node");u.public_key.size=32;memset(u.public_key.bytes,42,32);
 uint8_t payload[127],rf[255];size_t rfSize=0;auto stream=pb_ostream_from_buffer(payload,sizeof(payload));
 assert(pb_encode(&stream,meshtastic_User_fields,&u));
 assert(service.codec.encodeZeroHopNodeInfo(0x87654322,200,payload,stream.bytes_written,rf,sizeof(rf),rfSize));
 c.receive(rf,rfSize,-90,4,400,mt::Companion::WorkspaceProof{true});before=host.received.size();
 assert(host.received[before-2].node_info.user.public_key.size==32);
 memset(u.public_key.bytes,41,32);stream=pb_ostream_from_buffer(payload,sizeof(payload));assert(pb_encode(&stream,meshtastic_User_fields,&u));
 assert(service.codec.encodeZeroHopNodeInfo(0x87654322,201,payload,stream.bytes_written,rf,sizeof(rf),rfSize));
 c.receive(rf,rfSize,-90,4,401,mt::Companion::WorkspaceProof{true});assert(host.received.size()==before);
 u.public_key.size=0;stream=pb_ostream_from_buffer(payload,sizeof(payload));assert(pb_encode(&stream,meshtastic_User_fields,&u));
 assert(service.codec.encodeZeroHopNodeInfo(0x87654322,202,payload,stream.bytes_written,rf,sizeof(rf),rfSize));
 c.receive(rf,rfSize,-90,4,402,mt::Companion::WorkspaceProof{true});assert(host.received.size()==before);
 // Validly encrypted self identity must not enter the client node list.
 strcpy(u.long_name,"Forged self");stream=pb_ostream_from_buffer(payload,sizeof(payload));assert(pb_encode(&stream,meshtastic_User_fields,&u));
 assert(service.codec.encodeZeroHopNodeInfo(service.route.identity(),203,payload,stream.bytes_written,rf,sizeof(rf),rfSize));
 c.receive(rf,rfSize,-90,4,403,mt::Companion::WorkspaceProof{true});assert(host.received.size()==before);
 // Invalid/empty or licensed User must not bypass validation via raw delivery.
 auto invalidUser=[&](uint32_t id,bool licensed) {
   auto f=textFrame(0xffffffff,id);meshtastic_Data data{};data.portnum=meshtastic_PortNum_NODEINFO_APP;
   data.has_bitfield=true;if(licensed){data.payload.size=2;data.payload.bytes[0]=0x30;data.payload.bytes[1]=1;}
   f.resize(255);auto st=pb_ostream_from_buffer(f.data()+16,239);assert(pb_encode(&st,meshtastic_Data_fields,&data));f.resize(st.bytes_written+16);
   uint8_t iv[16];bench::MtCodec::nonce(0x87654321,id,iv);assert(bench::mtAesCtr(nullptr,bench::MtNodeRecord::PublicKey,16,iv,f.data()+16,st.bytes_written));
   c.receive(f.data(),f.size(),-90,4,404,mt::Companion::WorkspaceProof{true});
 };
 invalidUser(204,false);invalidUser(205,true);assert(host.received.size()==before);
 // Damaged and oversized frames recover at the next header; partial-frame timeout.
 c.byte(0x94,10,0,mt::Companion::WorkspaceProof{true});c.byte(0xc3,10,0,mt::Companion::WorkspaceProof{true});c.byte(0xff,10,0,mt::Companion::WorkspaceProof{true});c.byte(0xff,10,0,mt::Companion::WorkspaceProof{true});
 c.byte(0x94,20,0,mt::Companion::WorkspaceProof{true});c.byte(0xc3,20,0,mt::Companion::WorkspaceProof{true});c.byte(0,20,0,mt::Companion::WorkspaceProof{true});c.byte(100,20,0,mt::Companion::WorkspaceProof{true});
 request={};request.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;request.want_config_id=234;send(c,request,3000);assert(host.received.back().config_complete_id==234);
 // Bounded random serial input under ASAN/UBSAN, then reconnect.
 host.received.clear();uint32_t random=19;
 for(unsigned i=0;i<100000;++i){random=random*1664525+1013904223;c.byte(random>>24,5000+i,0,mt::Companion::WorkspaceProof{true});}
 send(c,request,200000);assert(host.received.back().config_complete_id==234);
 assert(radio.sends==0&&c.encodeErrors==0);
 // T096 reports its real model; no BLE capability claim.
 static mt::Service s2(radio,false,0x13572468,127);static Host h2(s2);static mt::companion::CompanionWorkspace workspace2;static mt::Companion c2(s2,h2,127,workspace2);send(c2,request);
 bool model=false;for(auto &m:h2.received)if(m.which_payload_variant==meshtastic_FromRadio_metadata_tag){model=m.metadata.hw_model==127&&!m.metadata.hasBluetooth;}
 assert(model);
 // Actual companion -> codec -> service -> Radio path, not a parallel encoder.
 static Radio tr;tr.sendResult=0;static mt::Service ts(tr,true,0x12345678,81);static Host th(ts);static mt::companion::CompanionWorkspace workspace3;static mt::Companion tc(ts,th,81,workspace3,1);
 mt::Config cfg;cfg.bytes[4]=1;auto record=cfg.node();record.setPublic();cfg.node(record);assert(ts.apply(cfg)&&ts.start(false,0));
 assert(ts.armEndpoint(true));
 meshtastic_ToRadio tx{};tx.which_payload_variant=meshtastic_ToRadio_packet_tag;auto &tp=tx.packet;
 tp.to=0xffffffff;tp.id=777;tp.which_payload_variant=meshtastic_MeshPacket_decoded_tag;tp.decoded.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;tp.decoded.payload.size=5;memcpy(tp.decoded.payload.bytes,"Hello",5);
 send(tc,tx,100);assert(tr.sends==0&&ts.endpointCount()==1&&!ts.endpointArmed());
 ts.poll(300,1);assert(tr.sends==1&&tr.last[12]==0&&bench::le32(tr.last.data())==0xffffffff);
 bench::RouteMetadata decoded;assert(ts.codec.decode(tr.last.data(),tr.last.size(),decoded));assert(decoded.portnum==1&&!decoded.wantResponse&&decoded.hasBitfield&&decoded.payloadLength==5);
 tr.done=true;ts.poll(301,1);tc.poll(mt::Companion::WorkspaceProof{true});assert(error(th)==meshtastic_Routing_Error_NONE&&ts.sent==1&&ts.receiving());
 assert(!ts.armEndpoint(true));send(tc,tx,302);assert(error(th)==meshtastic_Routing_Error_NO_INTERFACE&&tr.sends==1);
 ts.stop();assert(ts.apply(cfg)&&ts.start(false,400));assert(!ts.armEndpoint(true));
 // Radio/CAD refusal never retries or refunds the one-message quota.
 static Radio br;br.sendResult=-EBUSY;static mt::Service bs(br,true,0x12345678,81);
 assert(bs.apply(cfg)&&bs.start(false,0)&&bs.armEndpoint(true));
 assert(bs.submitEndpoint(0,777,tr.last.data(),tr.last.size(),100)==0);bs.poll(300,1);
 for(unsigned i=301;i<1000;++i)bs.poll(i,1);
 uint32_t resultId;int result;assert(br.sends==1&&bs.takeEndpointResult(0,resultId,result)&&result==-EBUSY&&!bs.armEndpoint(true));
 // Uncompleted native TX times out, faults closed, and is never retransmitted.
 static Radio dr;dr.sendResult=0;static mt::Service ds(dr,true,0x12345678,81);
 assert(ds.apply(cfg)&&ds.start(false,0)&&ds.armEndpoint(true));assert(ds.submitEndpoint(0,777,tr.last.data(),tr.last.size(),100)==0);
 mt::RadioTiming::Deadline timeout;assert(mt::RadioTiming::resolve(dr.airtime(tr.last.size()),timeout)&&timeout.driverMs==10000&&timeout.hostMs==11000);
 ds.poll(300,1);ds.poll(300+timeout.hostMs-1,1);assert(dr.sends==1&&!ds.faulted()&&!ds.takeEndpointResult(0,resultId,result));
 ds.poll(300+timeout.hostMs,1);assert(dr.sends==1&&ds.faulted()&&ds.takeEndpointResult(0,resultId,result)&&result==-ETIMEDOUT);
 // Display previews receive accepted packets with no client attached. They do
 // not consume output, and duplicate/misaddressed RF cannot create previews.
 static Radio ur;static mt::Service us(ur,false,0x12345678,127);static Host uh(us);static mt::companion::CompanionWorkspace workspace4;static mt::Companion uc(us,uh,127,workspace4);
 assert(us.apply(cfg)&&us.start(false,0));auto uf=textFrame(0xffffffff,801);uc.receive(uf.data(),uf.size(),-90,4,10,mt::Companion::WorkspaceProof{true});
 assert(uh.previews==1&&uh.received.empty());uc.receive(uf.data(),uf.size(),-90,4,11,mt::Companion::WorkspaceProof{true});assert(uh.previews==1);
 uf=textFrame(0x77777777,802);uc.receive(uf.data(),uf.size(),-90,4,12,mt::Companion::WorkspaceProof{true});assert(uh.previews==1);
 send(uc,request);const size_t ub=uh.received.size();uf=textFrame(0xffffffff,803);uc.receive(uf.data(),uf.size(),-90,4,13,mt::Companion::WorkspaceProof{true});
 assert(uh.previews==2&&uh.received.size()==ub+1&&uh.received.back().packet.decoded.portnum==1);
 std::cout<<"PASS companion handshake/admin/receive/dedup/TX-denial/framing/fuzz/T096/display-preview independence (24 groups)\n";
}
