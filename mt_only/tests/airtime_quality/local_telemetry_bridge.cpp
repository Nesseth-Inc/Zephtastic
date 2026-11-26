// SPDX-License-Identifier: GPL-3.0-only
// Actual local Companion generation. No injected producer measurements.
#include "telemetry_query_fixture.h"
#include "observation_adapter.h"
#include "known_channel_fixture.h"
#ifdef NDEBUG
#error Acceptance requires enabled assertions
#endif
static std::vector<uint8_t> unhex(const char *p){std::string s=p;assert(s.size()%2==0);std::vector<uint8_t>b;for(size_t i=0;i<s.size();i+=2)b.push_back(uint8_t(std::stoul(s.substr(i,2),nullptr,16)));return b;}
static std::string hex(const uint8_t *p,size_t n){constexpr char h[]="0123456789abcdef";std::string s;for(size_t i=0;i<n;++i){s+=h[p[i]>>4];s+=h[p[i]&15];}return s;}
static void records(const Host &host,const char *name){for(const auto &o:host.out){uint8_t b[512]{};auto out=pb_ostream_from_buffer(b,sizeof(b));assert(pb_encode(&out,meshtastic_FromRadio_fields,&o));std::cout<<name<<" "<<hex(b,out.bytes_written)<<"\n";}}
int main(int argc,char **argv){
 (void)&group;(void)&results;(void)&telemetries;(void)&beginQuery;(void)&goodFrame;
 if(argc!=5)return 64;
 const auto request=unhex(argv[1]);const uint8_t model=uint8_t(std::stoi(argv[2]));
 const bool ble=std::string(argv[3])=="ble";const std::string mode=argv[4];
 const bool primed=mode=="fraction"||mode=="expired"||mode=="backward"||mode=="lost";
 assert(primed||mode=="unknown"||mode=="idle"||mode=="invalid");
 qualify::Clock clock;Radio radio;mt::test::KnownChannelFixtureClock channelClock;
 mt::Service service(radio,primed,OURS,model,true,false);channelClock.bind(service);
 mt::companion::CompanionWorkspace workspace;Host usb(service),bluetooth(service);mt::Companion uc(service,usb,model,workspace,0),bc(service,bluetooth,model,workspace,1);
 if(mode!="unknown")qualify::bindObservation(service,clock);
 mt::Config cfg;cfg.bytes[4]=1;auto node=cfg.node();node.setPublic();cfg.node(node);
 assert(service.apply(cfg)&&service.start(false,0)&&service.armEndpoint(primed));
 usb.uptimeValid=bluetooth.uptimeValid=true;usb.uptime=bluetooth.uptime=321;
 meshtastic_ToRadio setup{};setup.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;setup.want_config_id=1;
 clock.ms=1;input(uc,setup,1);input(bc,setup,1);usb.out.clear();bluetooth.out.clear();
 if(primed) {
  meshtastic_ToRadio message{};message.which_payload_variant=meshtastic_ToRadio_packet_tag;auto &p=message.packet;
  p.to=PEER;p.id=0x59001234;p.hop_limit=0;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
  p.decoded.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;p.decoded.payload.size=5;memcpy(p.decoded.payload.bytes,"probe",5);
  radio.air=600;clock.ms=100;input(uc,message,100);clock.ms=300;service.poll(300,0);assert(radio.sends==1);
  clock.ms=301;clock.valid=mode!="lost";radio.done=true;service.poll(301,0);assert(service.sent==1);uc.poll(mt::Companion::WorkspaceProof{true});bc.poll(mt::Companion::WorkspaceProof{true});
  // Inactive disarm must preserve known samples and existing uncertainty.
  assert(service.armEndpoint(false));usb.out.clear();bluetooth.out.clear();
 }
 const uint32_t at=mode=="expired"?3600600:primed?1600:600;
 clock.ms=mode=="backward"?300:at;clock.valid=mode!="invalid";
 auto &client=ble?bc:uc;
 if(ble)client.packet(request.data(),request.size(),at,mt::Companion::WorkspaceProof{true});
 else {client.byte(0x94,at,0,mt::Companion::WorkspaceProof{true});client.byte(0xc3,at,0,mt::Companion::WorkspaceProof{true});client.byte(uint8_t(request.size()>>8),at,0,mt::Companion::WorkspaceProof{true});client.byte(uint8_t(request.size()),at,0,mt::Companion::WorkspaceProof{true});for(auto b:request)client.byte(b,at,0,mt::Companion::WorkspaceProof{true});}
 uc.poll(mt::Companion::WorkspaceProof{true});bc.poll(mt::Companion::WorkspaceProof{true});assert(radio.sends==int(primed)&&!service.endpointArmed());
 records(usb,"USB");records(bluetooth,"BLE");std::cout<<"COUNTS 0 "<<radio.sends<<"\n";
}
