// SPDX-License-Identifier: GPL-3.0-only
// Existing real nanopb Service/Companion fixture; fake private admission is a
// boundary proof only, not new cryptographic or physical-radio evidence.
#include "export_fixture.h"
void deliver(Fixture &f,const Bytes &wire,int8_t legacy,uint8_t raw,bool known,uint32_t now,int16_t rssi=-90){
  f.service.rx(wire.data(),wire.size(),legacy,now,77);
  f.a.receive(wire.data(),wire.size(),rssi,legacy,now,1700000000,known,raw,true,mt::Companion::WorkspaceProof{true});
  f.b.receive(wire.data(),wire.size(),rssi,legacy,now,1700000000,known,raw,true,mt::Companion::WorkspaceProof{true});
}
void checkSnr(const Host &host,uint32_t id,float expected,bool privatePacket=false,int16_t expectedRssi=-90,bool expectedRssiKnown=true){
  unsigned count=0;for(const auto &row:host.out)if(row.which_payload_variant==meshtastic_FromRadio_packet_tag&&row.packet.id==id){
    ++count;if(row.packet.rx_snr!=expected||row.packet.has_rx_rssi!=expectedRssiKnown||row.packet.rx_rssi!=expectedRssi||row.packet.pki_encrypted!=privatePacket)
      std::cerr<<"SNR_EXPORT_MISMATCH id="<<id<<" expected="<<expected<<" actual="<<row.packet.rx_snr<<" expected_rssi_present="<<expectedRssiKnown<<" rssi_present="<<row.packet.has_rx_rssi<<" expected_rssi="<<expectedRssi<<" rssi="<<row.packet.rx_rssi<<" private="<<row.packet.pki_encrypted<<'\n';
    // Host::send decodes actual nanopb bytes. A known zero must remain
    // present; failed/unknown status must omit RSSI and decode to zero.
    assert(row.packet.rx_snr==expected&&row.packet.has_rx_rssi==expectedRssiKnown&&row.packet.rx_rssi==expectedRssi&&row.packet.pki_encrypted==privatePacket);}
  assert(count==1);
}
// The same fixture first exports a known negative or valid zero and then an
// unknown-status zero, proving per-packet presence cannot leak from prior RX.
void checkStatusSequence(Fixture &f,bool privatePacket){
  uint32_t id=2,now=2000;
  for(int16_t rssi:{-90,0}){
    f.clear();auto known=privatePacket?privateFrame(id):text(f,id);
    deliver(f,known,1,5,true,now,rssi);
    checkSnr(f.usb,id,1.25f,privatePacket,rssi,true);checkSnr(f.ble,id,1.25f,privatePacket,rssi,true);
    assert(f.usb.out.size()==1&&f.ble.out.size()==1);
    assert(!f.radio.sends&&!f.usb.saves&&!f.ble.saves&&!f.usb.edits&&!f.ble.edits);
    ++id;now+=10;f.clear();auto unknown=privatePacket?privateFrame(id):text(f,id);
    deliver(f,unknown,-7,255,false,now,0);
    checkSnr(f.usb,id,-7,privatePacket,0,false);checkSnr(f.ble,id,-7,privatePacket,0,false);
    assert(f.usb.out.size()==1&&f.ble.out.size()==1);
    assert(!f.radio.sends&&!f.usb.saves&&!f.ble.saves&&!f.usb.edits&&!f.ble.edits);
    ++id;now+=10;
  }
}
int main(){
  std::cout.setf(std::ios::unitbuf);
  static_assert(mt::packetSnrDb(0)==0&&mt::packetSnrDb(127)==31.75f&&mt::packetSnrDb(128)==-32&&mt::packetSnrDb(255)==-0.25f);
  for(uint8_t model:{81,127})for(unsigned raw=0;raw<256;++raw){
    const int signedRaw=raw<128?int(raw):int(raw)-256;
    assert(mt::packetSnrDb(uint8_t(raw))==float(signedRaw)/4);
    const int8_t legacy=int8_t(signedRaw>>2);Fixture f(model);seed(f);f.clear();
    deliver(f,node(f,2),legacy,uint8_t(raw),true,2000);
    checkSnr(f.usb,2,float(signedRaw)/4);checkSnr(f.ble,2,float(signedRaw)/4);
    assert(f.service.nodes.get(0)->snr==legacy);
    for(const auto *host:{&f.usb,&f.ble})for(const auto &row:host->out)
      if(row.which_payload_variant==meshtastic_FromRadio_node_info_tag)assert(row.node_info.snr==legacy);
    assert(!f.radio.sends&&!f.usb.saves&&!f.ble.saves);
  }
  for(uint8_t model:{81,127})for(int16_t rssi:{-90,0,10}){
    Fixture f(model);seed(f);f.clear();
    deliver(f,text(f,2),1,5,true,2000,rssi);
    checkSnr(f.usb,2,1.25f,false,rssi);checkSnr(f.ble,2,1.25f,false,rssi);
    assert(f.usb.out.size()==1&&f.ble.out.size()==1);
    assert(!f.radio.sends&&!f.usb.saves&&!f.ble.saves&&!f.usb.edits&&!f.ble.edits);
  }
  for(uint8_t model:{81,127}){Fixture f(model);seed(f);checkStatusSequence(f,false);}
  std::cout<<"PASS all256 Qwen conversions and both board public nanopb USB/BLE rx_snr; NodeInfo cache remains integer\n";
  for(unsigned raw=0;raw<256;++raw){AdmittedEvent runtime;Fixture f(81,&runtime);seed(f);f.clear();
    const int signedRaw=raw<128?int(raw):int(raw)-256;
    deliver(f,privateFrame(2),int8_t(signedRaw>>2),uint8_t(raw),true,2000);
    checkSnr(f.usb,2,float(signedRaw)/4,true);checkSnr(f.ble,2,float(signedRaw)/4,true);
    assert(!f.radio.sends&&!f.usb.saves&&!f.ble.saves);
  }
  for(uint8_t model:{81,127})for(int16_t rssi:{-90,0,10}){
    AdmittedEvent runtime;Fixture f(model,&runtime);seed(f);f.clear();
    deliver(f,privateFrame(2),1,5,true,2000,rssi);
    checkSnr(f.usb,2,1.25f,true,rssi);checkSnr(f.ble,2,1.25f,true,rssi);
    assert(f.usb.out.size()==1&&f.ble.out.size()==1);
    assert(!f.radio.sends&&!f.usb.saves&&!f.ble.saves&&!f.usb.edits&&!f.ble.edits);
  }
  for(uint8_t model:{81,127}){AdmittedEvent runtime;Fixture f(model,&runtime);seed(f);checkStatusSequence(f,true);}
  std::cout<<"PASS all256 admitted private-event boundary nanopb exports carry same packet fractional SNR to both transports\n";
  {Fixture f;seed(f);f.clear();deliver(f,text(f,2),-7,255,false,2000);
    checkSnr(f.usb,2,-7,false,0,false);checkSnr(f.ble,2,-7,false,0,false);
    f.clear();const auto legacy=text(f,3);f.service.rx(legacy.data(),legacy.size(),3,3000,77);
    f.a.receive(legacy.data(),legacy.size(),-90,3,3000,1700000000,mt::Companion::WorkspaceProof{true});
    f.b.receive(legacy.data(),legacy.size(),-90,3,3000,1700000000,mt::Companion::WorkspaceProof{true});
    checkSnr(f.usb,3,3,false,0,false);checkSnr(f.ble,3,3,false,0,false);
    f.clear();f.a.receive(text(f,4).data(),1,-90,3,4000,1700000000,true,255,true,mt::Companion::WorkspaceProof{true});assert(f.usb.out.empty());
  }
  std::cout<<"PASS unknown packet metadata and old overload preserve integer output; existing malformed-frame guard remains\n";
}
