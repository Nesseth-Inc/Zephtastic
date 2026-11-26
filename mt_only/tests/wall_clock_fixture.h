// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "device_config_fixture.h"
struct ClockFixture:Fixture {
 mt::WallClock clock;uint64_t milliseconds=0;
 explicit ClockFixture(uint8_t model=127):Fixture(model) {usb.clock=&clock;ble.clock=&clock;}
 void tick(uint64_t ms){milliseconds=ms;usb.clockMs=ms;ble.clockMs=ms;}
 void set(uint32_t utc,bool bluetooth=false) {
  meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_set_time_only_tag;a.set_time_only=utc;
  admin(a,bluetooth,uint32_t(milliseconds));
 }
 void connect(bool bluetooth=false){meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;r.want_config_id=71;input(r,bluetooth);}
 void echo(uint32_t claimed,bool bluetooth=false) {
  meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_packet_tag;auto &p=r.packet;
  p.to=Node;p.id=90;p.want_ack=true;p.has_rx_time=true;p.rx_time=claimed;
  p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;p.decoded.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;
  p.decoded.payload.size=1;p.decoded.payload.bytes[0]='x';input(r,bluetooth,uint32_t(milliseconds));
 }
 void receive(uint32_t id,uint32_t port=meshtastic_PortNum_TEXT_MESSAGE_APP) {
  meshtastic_Data d{};d.portnum=static_cast<meshtastic_PortNum>(port);d.has_bitfield=true;d.payload.size=1;d.payload.bytes[0]='x';
  uint8_t data[239],frame[255];size_t length=0;auto stream=pb_ostream_from_buffer(data,sizeof(data));
  assert(pb_encode(&stream,meshtastic_Data_fields,&d));
  assert(service.codec.encodeZeroHopData(0x87654321,0xffffffff,id,data,stream.bytes_written,frame,sizeof(frame),length));
  const uint32_t captured=clock.at(milliseconds);
  c.receive(frame,length,-80,3,uint32_t(milliseconds),captured,mt::Companion::WorkspaceProof{true});
  b.receive(frame,length,-80,3,uint32_t(milliseconds),captured,mt::Companion::WorkspaceProof{true});
 }
};
