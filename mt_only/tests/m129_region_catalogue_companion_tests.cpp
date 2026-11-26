// SPDX-License-Identifier: GPL-3.0-only
#include "device_config_fixture.h"
#include <cassert>

struct CatalogueHost:Host {
  using Host::Host;
  unsigned failCatalogues=0;
  bool send(const uint8_t *wire,size_t size)override {
    assert(size>=4&&size_t(wire[2]*256+wire[3])==size-4);
    meshtastic_FromRadio message{};auto input=pb_istream_from_buffer(wire+4,size-4);
    assert(pb_decode(&input,meshtastic_FromRadio_fields,&message));
    if(message.which_payload_variant==meshtastic_FromRadio_region_presets_tag&&failCatalogues) {
      --failCatalogues;return false;
    }
    return Host::send(wire,size);
  }
};

struct CatalogueFixture {
  mt::test::KnownChannelFixtureClock knownChannelClock;
  Radio radio;Backend backend;mt::SettingsJournal journal;mt::Service service;
  mt::SettingsEdit edit;CatalogueHost host;mt::companion::CompanionWorkspace workspace;mt::Companion companion;
  explicit CatalogueFixture():service(radio,true,Node,127,true,true),edit(service,journal,backend),
      host(edit,journal),companion(service,host,127,workspace) {
    knownChannelClock.bind(service);assert(journal.load(backend)==0);
    assert(service.apply(journal.config()));
  }
  void input(const meshtastic_ToRadio &request,uint32_t now) {
    host.now=now;uint8_t wire[512];auto stream=pb_ostream_from_buffer(wire,sizeof(wire));
    assert(pb_encode(&stream,meshtastic_ToRadio_fields,&request));
    companion.byte(0x94,now,0,mt::Companion::WorkspaceProof{true});companion.byte(0xc3,now,0,mt::Companion::WorkspaceProof{true});
    companion.byte(uint8_t(stream.bytes_written>>8),now,0,mt::Companion::WorkspaceProof{true});companion.byte(uint8_t(stream.bytes_written),now,0,mt::Companion::WorkspaceProof{true});
    for(size_t i=0;i<stream.bytes_written;++i)companion.byte(wire[i],now,0,mt::Companion::WorkspaceProof{true});
  }
  void handshake(uint32_t nonce,uint32_t now) {
    meshtastic_ToRadio request{};request.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;
    request.want_config_id=nonce;input(request,now);
  }
  void disconnect(uint32_t now) {
    meshtastic_ToRadio request{};request.which_payload_variant=meshtastic_ToRadio_disconnect_tag;
    input(request,now);
  }
  unsigned catalogueCount(size_t start=0)const {
    unsigned count=0;
    for(size_t i=start;i<host.out.size();++i)
      if(host.out[i].which_payload_variant==meshtastic_FromRadio_region_presets_tag)++count;
    return count;
  }
  size_t catalogueIndex(size_t start=0)const {
    for(size_t i=start;i<host.out.size();++i)
      if(host.out[i].which_payload_variant==meshtastic_FromRadio_region_presets_tag)return i;
    return SIZE_MAX;
  }
};

int main() {
  {
    CatalogueFixture f;f.handshake(0x1234,1);
    const size_t index=f.catalogueIndex();assert(index!=SIZE_MAX&&f.catalogueCount()==1);
    assert(index>0&&index+1<f.host.out.size());
    assert(f.host.out[index-1].which_payload_variant==meshtastic_FromRadio_metadata_tag);
    assert(f.host.out[index+1].which_payload_variant==meshtastic_FromRadio_channel_tag);
    const auto &map=f.host.out[index].region_presets;
    assert(map.groups_count==6&&map.region_groups_count==34);
    uint8_t encoded[512]{};auto output=pb_ostream_from_buffer(encoded,sizeof(encoded));
    assert(pb_encode(&output,meshtastic_LoRaRegionPresetMap_fields,&map));
    assert(output.bytes_written<=510);
    assert(f.host.out.back().which_payload_variant==meshtastic_FromRadio_config_complete_id_tag);
    assert(f.host.out.back().config_complete_id==0x1234);
    assert(f.radio.sends==0&&!f.service.endpointArmed());
    const size_t prior=f.host.out.size();f.handshake(0x1234,2);
    assert(f.catalogueCount(prior)==1); // A repeated want_config request starts one fresh stream.
  }
  {
    CatalogueFixture f;f.host.failCatalogues=1;f.handshake(44,1);
    assert(f.catalogueCount()==0);
    for(const auto &message:f.host.out)
      assert(message.which_payload_variant!=meshtastic_FromRadio_channel_tag&&
             message.which_payload_variant!=meshtastic_FromRadio_config_complete_id_tag);
    const size_t beforeDisconnect=f.host.out.size();f.disconnect(2);
    f.companion.poll(mt::Companion::WorkspaceProof{true});assert(f.host.out.size()==beforeDisconnect);
    f.handshake(45,4);
    assert(f.catalogueCount()==1&&f.host.out.back().which_payload_variant==
           meshtastic_FromRadio_config_complete_id_tag&&f.host.out.back().config_complete_id==45);
    assert(!f.companion.encodeErrors&&!f.radio.sends);
  }
  puts("PASS Companion catalogue ordering, bounded encoding, retry/backpressure, disconnect cancellation and no RF");
}
