// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "MtCodec.h"
#include "MtKeyCommand.h"
#include "RelayCore.h"
#include "meshtastic/mesh.pb.h"
#include <pb_decode.h>
#include <pb_encode.h>
#include "stock/waypoint/WaypointWire.h"
#if defined(CONFIG_MT_HOST_METRICS_BRIDGE)
#include "RemoteTelemetryQuery.h"
#endif
#if defined(CONFIG_MT_GPIO_MODULES)
#include "modules/GpioModuleWire.h"
#endif

namespace mt {
// Opaque application transport only. Allowing a port does not install its
// module, interpret its payload, execute commands or advertise a producer.
class PublicApplication {
  template<class Enum> static uint32_t number(const Enum &value) {
    static_assert(sizeof(Enum)==1||sizeof(Enum)==2||sizeof(Enum)==4);
    if constexpr(sizeof(Enum)==1){uint8_t n;memcpy(&n,&value,1);return n;}
    else if constexpr(sizeof(Enum)==2){uint16_t n;memcpy(&n,&value,2);return n;}
    else {uint32_t n;memcpy(&n,&value,4);return n;}
  }
public:
  static bool allowed(uint32_t port) {
    if(port>=256&&port<=511)return true;
    switch(port) {
      case 2:case 7:case 8:case 9:case 10:case 11:
      case 32:case 33:case 34:case 35:case 36:case 37:case 38:
      case 64:case 65:case 66:case 68:case 69:case 71:case 72:case 73:
      case 75:case 76:case 77:case 112:return true;
      // AtakPluginModule::handleReceived at 8e6a88d is an opaque application pass-through.
      case meshtastic_PortNum_ATAK_PLUGIN_V2:return true;
      default:return false;
    }
  }
  static bool payloadAllowed(uint32_t port,const uint8_t *bytes,size_t size) {
#if defined(CONFIG_MT_HOST_METRICS_BRIDGE)
    if(port==meshtastic_PortNum_TELEMETRY_APP)
      return RemoteTelemetryQuery::responsePayload(bytes,size,meshtastic_Telemetry_host_metrics_tag);
#else
    (void)bytes;(void)size;
#endif
    return allowed(port);
  }
  static bool destination(uint32_t to){return to>1&&to!=0xfffffffeu;}
  static bool client(const meshtastic_MeshPacket &p,bool allowPrivate=false) {
    const auto &d=p.decoded;
    const bool boundedKeyHint=p.public_key.size<=sizeof(p.public_key.bytes);
    // Stock treats public_key as a transport hint only when PKI encryption is
    // explicitly requested. A hint on a non-PKI packet does not change its
    // ordinary shared-channel/public transport semantics.
    const bool envelope=boundedKeyHint&&(!p.pki_encrypted||
      (allowPrivate&&p.to!=0xffffffffu&&(p.public_key.size==0||p.public_key.size==32)));
    return p.which_payload_variant==meshtastic_MeshPacket_decoded_tag&&payloadAllowed(number(d.portnum),d.payload.bytes,d.payload.size)&&
      p.id&&!p.from&&destination(p.to)&&p.channel<8&&p.hop_limit<=7&&!p.hop_start&&
      !p.has_rx_time&&!p.rx_time&&p.rx_snr==0&&!p.has_rx_rssi&&!p.rx_rssi&&
      number(p.priority)<=127&&!number(p.delayed)&&!p.via_mqtt&&envelope&&
      !p.next_hop&&!p.relay_node&&!p.tx_after&&!number(p.transport_mechanism)&&!p.xeddsa_signed&&
      !number(p.ack_proof_status)&&d.payload.size<=233&&!d.want_response&&!d.dest&&!d.source&&
      !d.request_id&&d.bitfield<=1&&(d.has_bitfield||!d.bitfield)&&!d.xeddsa_signature.size;
  }
#if defined(CONFIG_MT_GPIO_MODULES)
  static bool gpioClient(const meshtastic_MeshPacket &p) {
    const auto &d=p.decoded;
    return p.which_payload_variant==meshtastic_MeshPacket_decoded_tag&&
      number(d.portnum)==meshtastic_PortNum_REMOTE_HARDWARE_APP&&p.id&&!p.from&&destination(p.to)&&
      p.channel<8&&p.hop_limit<=7&&!p.hop_start&&!p.has_rx_time&&!p.rx_time&&p.rx_snr==0&&
      !p.has_rx_rssi&&!p.rx_rssi&&number(p.priority)<=127&&!number(p.delayed)&&!p.via_mqtt&&
      !p.next_hop&&!p.relay_node&&!p.tx_after&&!number(p.transport_mechanism)&&!p.xeddsa_signed&&
      !number(p.ack_proof_status)&&d.payload.size<=24&&!d.dest&&!d.source&&!d.request_id&&
      !d.reply_id&&!d.emoji&&d.bitfield<=3&&(d.has_bitfield||!d.bitfield)&&
      !d.xeddsa_signature.size&&modules::GpioModuleWire::command(d.payload.bytes,d.payload.size);
  }
  static bool encodeGpioRequest(const meshtastic_MeshPacket &p,uint8_t *out,size_t capacity,size_t &size) {
    size=0;
    if(!gpioClient(p))return false;
    return modules::GpioModuleWire::encodeRequest(p.decoded.payload.bytes,p.decoded.payload.size,
      p.decoded.want_response,p.decoded.has_bitfield?p.decoded.bitfield:0,out,capacity,size);
  }
  static bool canonicalGpioRequest(const uint8_t *data,size_t size) {
    if(!data||!size||size>239)return false;
    meshtastic_Data d{};auto in=pb_istream_from_buffer(data,size);
    if(!pb_decode(&in,meshtastic_Data_fields,&d)||in.bytes_left||
       number(d.portnum)!=meshtastic_PortNum_REMOTE_HARDWARE_APP||!d.has_bitfield||d.bitfield>3||
       d.request_id||d.reply_id||d.emoji||d.dest||d.source||d.xeddsa_signature.size||
       d.payload.size>24||!modules::GpioModuleWire::command(d.payload.bytes,d.payload.size))return false;
    uint8_t expected[64]{};size_t length=0;
    const bool ok=modules::GpioModuleWire::encodeRequest(d.payload.bytes,d.payload.size,d.want_response,
      d.bitfield,expected,sizeof(expected),length)&&length==size&&!memcmp(expected,data,size);
    bench::wipeKeyBuffer(expected,sizeof(expected));bench::wipeKeyBuffer(&d,sizeof(d));return ok;
  }
#endif
  // Stock Data bit 0 is explicit permission to upload this payload to MQTT.
  // Preserve either caller choice; absent permission always normalizes to 0.
  static bool encode(uint32_t port,const uint8_t *payload,size_t length,uint32_t reply,uint32_t emoji,
                     uint8_t *out,size_t capacity,size_t &size,uint32_t bitfield=0,bool radioOrigin=false) {
    size=0;if(length>233||(length&&!payload)||!out||bitfield>1||!payloadAllowed(port,payload,length))return false;
    meshtastic_Data d{};d.portnum=static_cast<meshtastic_PortNum>(port);d.has_bitfield=true;
    d.bitfield=bitfield;
    d.payload.size=pb_size_t(length);if(length)memcpy(d.payload.bytes,payload,length);
    d.reply_id=reply;d.emoji=emoji;
    if(radioOrigin&&port==meshtastic_PortNum_WAYPOINT_APP)
      d.payload.size=pb_size_t(stock::waypoint::stripLocalPreferences(d.payload.bytes,d.payload.size));
    auto wire=pb_ostream_from_buffer(out,capacity<239?capacity:239);
    if(!pb_encode(&wire,meshtastic_Data_fields,&d))return false;
    size=wire.bytes_written;return true;
  }
  // Exact wire equality rejects unknown/duplicate fields, caller routing
  // metadata and noncanonical encodings before an outgoing ID is reserved.
  static bool canonical(const uint8_t *data,size_t size,uint32_t *port=nullptr) {
    if(!data||!size||size>239)return false;
    meshtastic_Data d{};auto in=pb_istream_from_buffer(data,size);
    if(!pb_decode(&in,meshtastic_Data_fields,&d)||in.bytes_left||!payloadAllowed(number(d.portnum),d.payload.bytes,d.payload.size)||
      !d.has_bitfield||d.bitfield>1||d.want_response||d.dest||d.source||d.request_id||d.xeddsa_signature.size)return false;
    uint8_t expected[239]{};size_t length=0;
    const bool ok=encode(number(d.portnum),d.payload.bytes,d.payload.size,d.reply_id,d.emoji,expected,sizeof(expected),length,uint8_t(d.bitfield))&&
      length==size&&!memcmp(expected,data,size);
    if(ok&&port)*port=number(d.portnum);
    bench::wipeKeyBuffer(expected,sizeof(expected));bench::wipeKeyBuffer(&d,sizeof(d));return ok;
  }
  // Reconstruct the whole canonical plaintext, then compare ciphertext under
  // the received source/ID. Public CTR is not authentication. Header route
  // bits remain outside this application-shape check.
  static bool received(bench::MtCodec &codec,const uint8_t *p,size_t n,const bench::RouteMetadata &m) {
    if(!p||n<17||n>255||p[13]!=codec.channelHash()||!allowed(m.portnum))return false;
#if defined(CONFIG_MT_GPIO_MODULES)
    const bool gpio=m.portnum==meshtastic_PortNum_REMOTE_HARDWARE_APP;
#else
    constexpr bool gpio=false;
#endif
    if(!gpio&&(!m.hasBitfield||m.bitfield>1||m.wantResponse||m.request_id))return false;
#if defined(CONFIG_MT_GPIO_MODULES)
    uint8_t gpioData[239]{};size_t gpioDataSize=0;
    if(gpio&&!modules::GpioModuleWire::receivedData(m,gpioData,sizeof(gpioData),gpioDataSize))return false;
#endif
    if(m.signatureLength){
      // Service owns signature verification and quarantine. Restrict signed
      // module delivery to broadcasts, and reconstruct every covered field
      // plus the signature before comparing the original received ciphertext.
      if(!bench::publicSignedModulePort(m.portnum)||!m.channelDecoded||m.decodeRejected||m.backendFailed||
         !m.signatureVerified||m.signatureRejected||m.signatureQuarantined||m.signatureLength!=64||
         bench::le32(p)!=0xffffffffu||m.payloadLength>sizeof(m.payload))return false;
#if defined(CONFIG_MT_GPIO_MODULES)
      if(gpio) {
        const bool ok=codec.validateSignedModuleData(p,n,gpioData,gpioDataSize);
        bench::wipeKeyBuffer(gpioData,sizeof(gpioData));return ok;
      }
#endif
      meshtastic_Data decoded{};decoded.portnum=static_cast<meshtastic_PortNum>(m.portnum);
      decoded.payload.size=m.payloadLength;if(m.payloadLength)memcpy(decoded.payload.bytes,m.payload,m.payloadLength);
      decoded.reply_id=m.reply_id;decoded.emoji=m.emoji;decoded.has_bitfield=true;decoded.bitfield=m.bitfield;
      decoded.xeddsa_signature.size=64;memcpy(decoded.xeddsa_signature.bytes,m.signature,64);
      uint8_t expected[239]{};auto out=pb_ostream_from_buffer(expected,sizeof(expected));
      const bool ok=pb_encode(&out,meshtastic_Data_fields,&decoded)&&codec.validateSignedModuleData(p,n,expected,out.bytes_written);
      bench::wipeKeyBuffer(expected,sizeof(expected));bench::wipeKeyBuffer(&decoded,sizeof(decoded));return ok;
    }
    uint8_t data[239]{},frame[255]{};size_t size=0,length=0;
#if defined(CONFIG_MT_GPIO_MODULES)
    const bool encoded=gpio? (memcpy(data,gpioData,gpioDataSize),size=gpioDataSize,true):
      encode(m.portnum,m.payload,m.payloadLength,m.reply_id,m.emoji,data,sizeof(data),size,m.bitfield);
    bench::wipeKeyBuffer(gpioData,sizeof(gpioData));
#else
    const bool encoded=encode(m.portnum,m.payload,m.payloadLength,m.reply_id,m.emoji,data,sizeof(data),size,m.bitfield);
#endif
    const bool ok=encoded&&
      codec.encodeZeroHopData(bench::le32(p+4),bench::le32(p),bench::le32(p+8),data,size,frame,sizeof(frame),length)&&
      length==n&&!memcmp(frame+16,p+16,n-16);
    bench::wipeKeyBuffer(data,sizeof(data));bench::wipeKeyBuffer(frame,sizeof(frame));return ok;
  }
};
}
