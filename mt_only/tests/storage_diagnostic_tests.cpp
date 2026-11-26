// SPDX-License-Identifier: GPL-3.0-only
// Actual Companion + SettingsEdit + journal, synthetic radio only.
#include "device_config_fixture.h"
#include <cstdio>
static void assertNoLegacyStorageDiagnostic(const Host &host) {
  for(const auto &m:host.out)if(m.which_payload_variant==meshtastic_FromRadio_log_record_tag&&
      std::string(m.log_record.message).find("storage_diag ")==0) {
    assert(false&&"ordinary handshake must not emit legacy storage diagnostics");
  }
}
static void ordinaryHandshake(Fixture &f,bool ble=false,uint32_t nonce=901) {
  auto &host=ble?f.ble:f.usb;host.out.clear();
  meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;r.want_config_id=nonce;
  f.input(r,ble,1000);assertNoLegacyStorageDiagnostic(host);
}
static void packet(mt::Companion &c,const meshtastic_ToRadio &r,uint32_t now) {
  uint8_t bytes[512];auto out=pb_ostream_from_buffer(bytes,sizeof(bytes));
  assert(pb_encode(&out,meshtastic_ToRadio_fields,&r));c.packet(bytes,out.bytes_written,now,mt::Companion::WorkspaceProof{true});
}
struct NoPauseHost final:Host {
  using Host::Host;
  int save(const mt::Config &)override{return -EBUSY;}
};
static meshtastic_AdminMessage lora(bool tx,uint8_t model) {
  meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_set_config_tag;
  a.set_config.which_payload_variant=meshtastic_Config_lora_tag;auto &l=a.set_config.payload_variant.lora;
  l.region=meshtastic_Config_LoRaConfig_RegionCode_US;l.use_preset=true;l.modem_preset=meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST;
  l.channel_num=20;l.tx_power=model==127?-9:2;l.sx126x_rx_boosted_gain=true;l.hop_limit=3;l.tx_enabled=tx;return a;
}
static void ready(Fixture &f) {auto cfg=f.service.config;cfg.bytes[4]=1;cfg.setTxEnabled(false);assert(f.edit.save(cfg,0)==0);assert(f.service.start(false,0));assert(!f.service.endpointArmed());}
int main() {
  unsigned groups=0;
  for(uint8_t model:{uint8_t(81),uint8_t(127)}) {
    {auto f=std::make_unique<Fixture>(model,false);ready(*f);
     const auto before=f->service.storagePauseDiagnostic();ordinaryHandshake(*f);
     const auto after=f->service.storagePauseDiagnostic();
     assert(after.sequence==before.sequence&&after.reason==before.reason&&after.mask==before.mask&&after.result==before.result&&!f->radio.sends);
     ++groups;std::cout<<"PASS ordinary handshake emits no legacy storage log or receipt mutation model="<<unsigned(model)<<"\n";}
    {auto f=std::make_unique<Fixture>(model,false);ready(*f);f->radio.busyFlag=true;
     auto writes=f->backend.writes;auto before=f->service.config;f->admin(lora(true,model));
     assert(f->usb.error()==meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED);
     assert(f->backend.writes==writes&&f->service.config.same(before)&&f->service.receiving()&&!f->service.faulted()&&!f->radio.sends);
     const auto refused=f->service.storagePauseDiagnostic();
     assert(refused.sequence&&refused.reason==2&&refused.mask==0&&refused.result==-EBUSY);
     f->admin(lora(false,model));assert(f->usb.error()==0);ordinaryHandshake(*f,false,902);
     f->radio.busyFlag=false;assert(f->service.pauseForStorage()==0);assert(f->service.start(false,2000));
     const auto background=f->service.storagePauseDiagnostic();
     assert(background.sequence==refused.sequence+1&&background.reason==0&&background.mask==0&&background.result==0);
     ordinaryHandshake(*f,false,903);
     ++groups;std::cout<<"PASS busy refusal and current service receipt model="<<unsigned(model)<<"\n";}
    {auto f=std::make_unique<Fixture>(model,false);ready(*f);f->radio.busyFlag=true;f->radio.busyDetail=11;
     f->admin(lora(true,model));assert(f->usb.error()==meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED);
     const auto d=f->service.storagePauseDiagnostic();assert(d.sequence&&d.reason==2&&d.mask==11&&d.result==-EBUSY&&f->radio.busyDetailReads==1);
     f->radio.busyDetail=14;f->admin(lora(false,model));ordinaryHandshake(*f);
     assert(f->radio.busyDetailReads==1&&!f->radio.sends);
     ++groups;std::cout<<"PASS exact native busy detail without extra metadata or cleanup I/O model="<<unsigned(model)<<"\n";}
    {auto f=std::make_unique<Fixture>(model,false);ready(*f);f->radio.pauseResult=-EAGAIN;
     auto writes=f->backend.writes;f->admin(lora(true,model));assert(f->usb.error()==meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED);
     const auto d=f->service.storagePauseDiagnostic();assert(d.sequence&&d.reason==4&&d.mask==0&&d.result==-EBUSY);
     ordinaryHandshake(*f);
     assert(f->backend.writes==writes&&!f->service.config.txEnabled()&&f->service.receiving()&&!f->service.faulted()&&!f->radio.sends);
     ++groups;std::cout<<"PASS clean native deferral distinguished model="<<unsigned(model)<<"\n";}
    {auto f=std::make_unique<Fixture>(model,false);ready(*f);assert(f->service.armEndpoint(true));text(*f,77,500);
     const auto free=f->service.endpointFree(),writes=f->backend.writes,stops=f->radio.stops;
     f->admin(lora(true,model));assert(f->usb.error()==meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED);
     const auto d=f->service.storagePauseDiagnostic();assert(d.sequence&&d.reason==1&&(d.mask&128)&&d.result==-EBUSY);
     ordinaryHandshake(*f);
     assert(f->service.endpointFree()==free&&f->backend.writes==writes&&f->radio.stops==stops&&!f->radio.sends);
     ++groups;std::cout<<"PASS accepted queued work preserved under refusal model="<<unsigned(model)<<"\n";}
    {auto f=std::make_unique<Fixture>(model,false);ready(*f);f->backend.fail=true;f->admin(lora(true,model));
     assert(f->usb.error()==meshtastic_Routing_Error_NO_INTERFACE);ordinaryHandshake(*f);
     const auto d=f->service.storagePauseDiagnostic();
     assert(d.sequence&&d.reason==0&&d.mask==0&&d.result==0&&!f->radio.sends);
     ++groups;std::cout<<"PASS backend failure not misclassified as busy guard model="<<unsigned(model)<<"\n";}
    {auto f=std::make_unique<Fixture>(model,false);ready(*f);f->radio.busyFlag=true;f->admin(lora(true,model),true);
     assert(f->ble.error()==meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED&&f->usb.out.empty());
     ordinaryHandshake(*f,true);ordinaryHandshake(*f,false);
     const auto d=f->service.storagePauseDiagnostic();assert(d.sequence&&d.reason==2&&d.result==-EBUSY&&!f->radio.sends);
     ++groups;std::cout<<"PASS USB/BLE error and handshake output ownership model="<<unsigned(model)<<"\n";}
  }
  {auto f=std::make_unique<Fixture>(127,false);ready(*f);f->radio.busyFlag=true;
   assert(f->service.pauseForStorage()==-EBUSY);f->radio.busyFlag=false;
   auto host=std::make_unique<NoPauseHost>(f->edit,f->journal);mt::companion::CompanionWorkspace workspace;auto c=std::make_unique<mt::Companion>(f->service,*host,127,workspace);
   meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_packet_tag;auto &p=r.packet;
   p.to=Node;p.id=88;p.want_ack=true;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
   p.decoded.portnum=meshtastic_PortNum_ADMIN_APP;auto a=lora(true,127);
   auto out=pb_ostream_from_buffer(p.decoded.payload.bytes,sizeof(p.decoded.payload.bytes));
   assert(pb_encode(&out,meshtastic_AdminMessage_fields,&a));p.decoded.payload.size=out.bytes_written;
   const auto before=f->service.storagePauseDiagnostic();
   packet(*c,r,1500);assert(host->error()==meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED);
   host->out.clear();r={};r.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;r.want_config_id=904;packet(*c,r,1600);
   assertNoLegacyStorageDiagnostic(*host);
   const auto after=f->service.storagePauseDiagnostic();
   assert(after.sequence==before.sequence&&after.reason==before.reason&&after.mask==before.mask&&after.result==before.result&&!f->radio.sends);
   ++groups;std::cout<<"PASS failure before Service pause preserves current service receipt\n";}
  std::cout<<"PASS storage diagnostic groups="<<groups<<"; no hardware or RF\n";
}
