// SPDX-License-Identifier: GPL-3.0-only
// Exercises the actual Companion ToRadio / FromRadio proxy seams without the
// WiFi-only MQTT_GATEWAY feature macro.
#include "Companion.h"
#include "MqttServiceEnvelopeCodec.h"
#include "known_channel_fixture.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
constexpr uint32_t Self=0x12345678,Peer=0x24680135;
using Bytes=std::vector<uint8_t>;

struct Radio final:mt::Radio {
  int state=0;
  bool ready()override{return true;}
  int start()override{state=1;return 0;}
  int restore()override{state=1;return 0;}
  int stop()override{state=0;return 0;}
  int health()override{return state;}
  bool busy()override{return false;}
  uint32_t airtime(size_t)override{return 100;}
  int send(uint8_t *,size_t)override{return 0;}
  bool completion(int &)override{return false;}
};

struct Host final:mt::Companion::Host {
  mt::Service &service;
  mt::Companion::Host::MqttProxyPolicy policy{};
  uint8_t lastWire[516]{};
  size_t lastWireSize=0;
  unsigned sends=0;
  explicit Host(mt::Service &s):service(s){}
  bool send(const uint8_t *bytes,size_t size)override {
    if(size>sizeof(lastWire))return false;
    std::memcpy(lastWire,bytes,size);lastWireSize=size;++sends;return true;
  }
  int save(const mt::Config &)override{return 0;}
  bool ready()const override{return true;}
  bool mqttProxyPolicy(mt::Companion::Host::MqttProxyPolicy &out)const override {
    out=policy;return true;
  }
  bool takeMqttProxyPublish(meshtastic_MqttClientProxyMessage &out)override {
    return service.takeMqttProxyPublish(out);
  }
};

struct Fixture {
  Radio radio;
  mt::test::KnownChannelFixtureClock channelClock;
  mt::Service service{radio,true,Self,81,true,false};
  Host host{service};
  mt::companion::CompanionWorkspace workspace;mt::Companion companion{service,host,81,workspace};
  Fixture() {
    channelClock.bind(service);
    auto config=service.config;config.bytes[4]=1;config.setTxEnabled(true);
    assert(config.valid()&&config.provisioned()&&service.apply(config));
    assert(service.start(false,0)&&service.armEndpoint(true));
    service.setMqttProxyBridgeEnabled(true);
    host.policy.mqttEnabled=true;host.policy.proxyToClientEnabled=true;
    host.policy.brokerConfigured=true;host.policy.tlsEnabled=true;
    host.policy.encryptionEnabled=true;host.policy.root="lab/root";
    meshtastic_ToRadio hello=meshtastic_ToRadio_init_zero;
    hello.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;hello.want_config_id=7;
    uint8_t wire[16]{};auto out=pb_ostream_from_buffer(wire,sizeof(wire));
    assert(pb_encode(&out,meshtastic_ToRadio_fields,&hello));
    companion.packet(wire,out.bytes_written,1,mt::Companion::WorkspaceProof{true});
    assert(companion.handshakes==1);
  }
  void enableDownlink() {
    service.stop();auto config=service.config;
    assert(config.setPrimaryDownlinkEnabled(true));
    assert(service.apply(config)&&service.start(false,0)&&service.armEndpoint(true));
  }
  Bytes frame(uint32_t id) {
    mt::PrimaryChannel::Selection key{};assert(service.config.channelSelection(0,key));
    assert(service.codec.setKey(key.hash,key.bytes,key.length));
    mt::PrimaryChannel::clear(key);
    const uint8_t value='m';uint8_t data[239]{};size_t dataSize=0;
    assert(mt::PublicApplication::encode(8,&value,1,0,0,data,sizeof(data),dataSize,1));
    Bytes wire(dataSize+16);size_t frameSize=0;
    assert(service.codec.encodeZeroHopData(Peer,0xffffffffu,id,data,dataSize,
        wire.data(),wire.size(),frameSize));
    wire.resize(frameSize);return wire;
  }
  Bytes envelope(bool encrypted,uint32_t id) {
    auto radioFrame=frame(id);meshtastic_MeshPacket packet=meshtastic_MeshPacket_init_zero;
    packet.from=Peer;packet.to=0xffffffffu;packet.id=id;packet.hop_limit=packet.hop_start=3;
    packet.channel=radioFrame[13];packet.which_payload_variant=meshtastic_MeshPacket_encrypted_tag;
    packet.encrypted.size=pb_size_t(radioFrame.size()-16);
    std::memcpy(packet.encrypted.bytes,radioFrame.data()+16,packet.encrypted.size);
    mt::ChannelRecord channel;assert(service.config.channelRecord(0,channel));
    const char *channelName=channel.name()[0]?channel.name():service.config.modemName();
    uint8_t wire[mt::mqtt::ServiceEnvelopeCodec::kMaxEncodedEnvelopeBytes]{};
    size_t size=0;
    if(encrypted) {
      assert(mt::mqtt::ServiceEnvelopeCodec::encode(&packet,channelName,std::strlen(channelName),
          "!abcdef01",9,wire,sizeof(wire),&size));
    } else {
      packet.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
      packet.decoded.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;packet.decoded.has_bitfield=true;
      packet.decoded.bitfield=1;packet.decoded.payload.size=1;packet.decoded.payload.bytes[0]='x';
      assert(mt::mqtt::ServiceEnvelopeCodec::encode(&packet,channelName,std::strlen(channelName),
          "!abcdef01",9,wire,sizeof(wire),&size));
    }
    return Bytes(wire,wire+size);
  }
  void sendProxy(Bytes payload,uint32_t id,bool retained=false) {
    mt::ChannelRecord channel;assert(service.config.channelRecord(0,channel));
    const char *channelName=channel.name()[0]?channel.name():service.config.modemName();
    meshtastic_ToRadio radio=meshtastic_ToRadio_init_zero;
    radio.which_payload_variant=meshtastic_ToRadio_mqttClientProxyMessage_tag;
    auto &proxy=radio.mqttClientProxyMessage;
    std::snprintf(proxy.topic,sizeof(proxy.topic),"lab/root/2/e/%s/!abcdef01",channelName);
    proxy.which_payload_variant=meshtastic_MqttClientProxyMessage_data_tag;
    proxy.payload_variant.data.size=pb_size_t(payload.size());
    std::memcpy(proxy.payload_variant.data.bytes,payload.data(),payload.size());
    proxy.retained=retained;
    uint8_t wire[512]{};auto out=pb_ostream_from_buffer(wire,sizeof(wire));
    assert(pb_encode(&out,meshtastic_ToRadio_fields,&radio));
    companion.packet(wire,out.bytes_written,id,mt::Companion::WorkspaceProof{true});
  }
};

void testPhoneProxyDelegatedTlsAndEncryptedReceive() {
  Fixture f;f.enableDownlink();
  auto envelope=f.envelope(true,700);
  const auto before=f.service.received;
  const auto unsupportedBefore=f.companion.unsupported;
  // Config asks for TLS, but the device deliberately has no TLS-trust proof
  // for a session carried by the authorized local phone proxy.
  f.sendProxy(envelope,20);
  assert(f.service.received==before&&f.companion.unsupported==unsupportedBefore);
  // Broker ingress is not an RF reception. Positive private packet delivery
  // is checked through the real phone input path in the M123 Service fixture.

  f.host.policy.proxyToClientEnabled=false;
  f.sendProxy(envelope,21);
  assert(f.service.received==before&&f.companion.unsupported==unsupportedBefore+1);
}

void testProxyFailsClosedOnRetainedDecodedAndUnconfigured() {
  Fixture f;f.enableDownlink();const auto before=f.service.received;
  f.sendProxy(f.envelope(true,701),22,true);
  assert(f.service.received==before);
  f.sendProxy(f.envelope(false,702),23);
  assert(f.service.received==before);
  f.host.policy.brokerConfigured=false;
  f.sendProxy(f.envelope(true,703),24);
  assert(f.service.received==before);
}

void testCoreBridgeAndFromRadioProxyVariant() {
  Fixture f;
  mt::mqtt::client_proxy::Message rejected{};
  std::strcpy(rejected.topic,"msh/2/e/lab/!12345678");
  rejected.kind=mt::mqtt::client_proxy::PayloadKind::Binary;
  rejected.length=1;rejected.payload[0]=9;
  f.service.setMqttProxyBridgeEnabled(false);
  assert(f.service.enqueueMqttProxyPublish(rejected)==
         mt::mqtt::client_proxy::Queue<1>::PushResult::InvalidMessage);
  f.service.setMqttProxyBridgeEnabled(true);
  mt::mqtt::client_proxy::Message publish{};
  std::strcpy(publish.topic,"msh/2/e/lab/!12345678");
  publish.kind=mt::mqtt::client_proxy::PayloadKind::Binary;
  publish.length=3;publish.payload[0]=1;publish.payload[1]=0;publish.payload[2]=2;
  assert(f.service.enqueueMqttProxyPublish(publish)==
         mt::mqtt::client_proxy::Queue<1>::PushResult::Accepted);
  const auto before=f.host.sends;f.companion.poll(mt::Companion::WorkspaceProof{true});
  assert(f.host.sends==before+1);
  auto input=pb_istream_from_buffer(f.host.lastWire+4,f.host.lastWireSize-4);
  meshtastic_FromRadio from=meshtastic_FromRadio_init_zero;
  assert(pb_decode(&input,meshtastic_FromRadio_fields,&from)&&!input.bytes_left);
  assert(from.which_payload_variant==meshtastic_FromRadio_mqttClientProxyMessage_tag);
  assert(from.mqttClientProxyMessage.which_payload_variant==meshtastic_MqttClientProxyMessage_data_tag);
  assert(from.mqttClientProxyMessage.payload_variant.data.size==3);
  assert(from.mqttClientProxyMessage.payload_variant.data.bytes[1]==0);
  assert(f.service.enqueueMqttProxyPublish(rejected)==
         mt::mqtt::client_proxy::Queue<1>::PushResult::Accepted);
  f.service.setMqttProxyBridgeEnabled(false);
  f.service.setMqttProxyBridgeEnabled(true);
  assert(!f.service.takeMqttProxyPublish(from.mqttClientProxyMessage));
}
}

int main() {
  testPhoneProxyDelegatedTlsAndEncryptedReceive();
  testProxyFailsClosedOnRetainedDecodedAndUnconfigured();
  testCoreBridgeAndFromRadioProxyVariant();
}
