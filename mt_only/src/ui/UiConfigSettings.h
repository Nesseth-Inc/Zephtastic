// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "meshtastic/device_ui.pb.h"
#include <pb_decode.h>
#include <pb_encode.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace mt::ui {

class UiConfigSensitiveWipe {
  void *value_;
  size_t size_;
public:
  UiConfigSensitiveWipe(void *value,size_t size):value_(value),size_(size){}
  ~UiConfigSensitiveWipe() {
    volatile uint8_t *bytes=static_cast<volatile uint8_t *>(value_);
    for(size_t i=0;i<size_;++i)bytes[i]=0;
  }
};

// Bounded typed view of the stock DeviceUIConfig protobuf. The settings are
// persisted independently from the existing node/config identity records.
class UiConfigSettings {
public:
  static constexpr size_t WireCapacity=meshtastic_DeviceUIConfig_size;

  static meshtastic_DeviceUIConfig defaults() {
    meshtastic_DeviceUIConfig value{};
    // Pinned stock NodeDB.cpp initializes uiconfig with these UI defaults.
    value.screen_brightness=153;
    value.screen_timeout=30;
    return value;
  }

  static bool encode(const meshtastic_DeviceUIConfig &value,uint8_t *wire,
                     size_t capacity,size_t &length) {
    length=0;
    if(!wire)return false;
    if(value.calibration_data.size>sizeof(value.calibration_data.bytes)||
       (value.has_node_filter&&!memchr(value.node_filter.node_name,0,sizeof(value.node_filter.node_name)))||
       (value.has_node_highlight&&!memchr(value.node_highlight.node_name,0,sizeof(value.node_highlight.node_name)))||
       (value.has_map_data&&!memchr(value.map_data.style,0,sizeof(value.map_data.style))))return false;
    auto stream=pb_ostream_from_buffer(wire,capacity);
    if(!pb_encode(&stream,meshtastic_DeviceUIConfig_fields,&value))return false;
    length=stream.bytes_written;
    return length<=WireCapacity;
  }

  static bool decodeCanonical(const uint8_t *wire,size_t length,
                              meshtastic_DeviceUIConfig &out) {
    if((!wire&&length)||length>WireCapacity)return false;
    meshtastic_DeviceUIConfig candidate{};
    UiConfigSensitiveWipe wipeCandidate{&candidate,sizeof(candidate)};
    auto input=pb_istream_from_buffer(wire,length);
    if(!pb_decode(&input,meshtastic_DeviceUIConfig_fields,&candidate)||input.bytes_left)return false;
    uint8_t encoded[WireCapacity]{};
    UiConfigSensitiveWipe wipeEncoded{encoded,sizeof(encoded)};
    size_t encodedLength=0;
    if(!encode(candidate,encoded,sizeof(encoded),encodedLength)||
       encodedLength!=length||(length&&memcmp(encoded,wire,length)))return false;
    out=candidate;
    return true;
  }

  static bool valid(const meshtastic_DeviceUIConfig &value) {
    uint8_t wire[WireCapacity]{};
    UiConfigSensitiveWipe wipeWire{wire,sizeof(wire)};
    size_t length=0;
    return encode(value,wire,sizeof(wire),length);
  }

  static bool same(const meshtastic_DeviceUIConfig &left,
                   const meshtastic_DeviceUIConfig &right) {
    uint8_t a[WireCapacity]{},b[WireCapacity]{};
    UiConfigSensitiveWipe wipeA{a,sizeof(a)},wipeB{b,sizeof(b)};
    size_t aLength=0,bLength=0;
    return encode(left,a,sizeof(a),aLength)&&encode(right,b,sizeof(b),bLength)&&
      aLength==bLength&&(!aLength||!memcmp(a,b,aLength));
  }
};

} // namespace mt::ui

