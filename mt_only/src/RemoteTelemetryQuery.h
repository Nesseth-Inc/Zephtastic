// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "TelemetryQuery.h"
#include "LocalStatsQuery.h"
#include <pb_common.h>
namespace mt {
// Canonical bounded transport of peer-reported metrics. No local sensor
// producer, cache write or trust is provided by this helper.
class RemoteTelemetryQuery {
  static bool numeric(const meshtastic_Telemetry &t){
    // In the pinned metric messages, every 32-bit fixed field is an IEEE
    // float. Use nanopb's actual offsets and optional-presence flags for each
    // target ABI. The outer fixed32 timestamp is outside the selected message.
    static_assert(sizeof(float)==4&&FLT_RADIX==2&&FLT_MANT_DIG==24&&FLT_MAX_EXP==128);
#define MT_FLOAT_SCHEMA(row, storage, rule, type, name, tag) \
    static_assert(PB_LTYPE_MAP_##type!=PB_LTYPE_FIXED32 || \
        (__is_same(decltype(row::name),float)&&PB_ATYPE_##storage==PB_ATYPE_STATIC&& \
         (PB_HTYPE_##rule==PB_HTYPE_SINGULAR||PB_HTYPE_##rule==PB_HTYPE_OPTIONAL)));
#define MT_METRIC_SCHEMA(type) meshtastic_##type##_FIELDLIST(MT_FLOAT_SCHEMA,meshtastic_##type)
    MT_METRIC_SCHEMA(DeviceMetrics)
    MT_METRIC_SCHEMA(EnvironmentMetrics)
    MT_METRIC_SCHEMA(AirQualityMetrics)
    MT_METRIC_SCHEMA(PowerMetrics)
    MT_METRIC_SCHEMA(LocalStats)
    MT_METRIC_SCHEMA(HealthMetrics)
    MT_METRIC_SCHEMA(HostMetrics)
    MT_METRIC_SCHEMA(TrafficManagementStats)
    MT_METRIC_SCHEMA(SoilWaterMetrics)
#undef MT_METRIC_SCHEMA
#undef MT_FLOAT_SCHEMA
    pb_field_iter_t field;
    if(!pb_field_iter_begin_const(&field,meshtastic_Telemetry_fields,&t)||
       !pb_field_iter_find(&field,t.which_variant)||!field.submsg_desc||
       !pb_field_iter_begin_const(&field,field.submsg_desc,field.pData))return false;
    do{
      if(PB_LTYPE(field.type)!=PB_LTYPE_FIXED32||
         (field.pSize&&!*static_cast<const bool*>(field.pSize)))continue;
      uint32_t bits;memcpy(&bits,field.pData,sizeof(bits));
      if((bits&0x7f800000u)==0x7f800000u)return false;
    }while(pb_field_iter_next(&field));
    return true;
  }
public:
  static bool supportedVariant(pb_size_t tag){return (tag>=2&&tag<=9)||tag==11;}
  static bool decodeCanonical(const uint8_t *payload,size_t size,pb_size_t expected,meshtastic_Telemetry &out){
    out={};if(!payload||!size||size>233||(expected&&!supportedVariant(expected)))return false;
    auto in=pb_istream_from_buffer(payload,size);
    if(!pb_decode(&in,meshtastic_Telemetry_fields,&out)||in.bytes_left||!supportedVariant(out.which_variant)||
       (expected&&out.which_variant!=expected)||!numeric(out)){out={};return false;}
    uint8_t canonical[233]{};auto encoded=pb_ostream_from_buffer(canonical,sizeof(canonical));
    const bool ok=pb_encode(&encoded,meshtastic_Telemetry_fields,&out)&&encoded.bytes_written==size&&!memcmp(canonical,payload,size);
    if(!ok)out={};
    return ok;
  }
  // A cached snapshot selects its variant; its values never enter RF requests.
  // Preserve the stock cold-cache DeviceMetrics request and strict LocalStats selector.
  static uint8_t requestVariant(const uint8_t *payload,size_t size){
    if(!size)return 2;
    if(TelemetryQuery::request(payload,size))return 2;
    if(LocalStatsQuery::request(payload,size))return 6;
    meshtastic_Telemetry t{};
    return decodeCanonical(payload,size,0,t)&&!t.time&&t.which_variant!=2&&t.which_variant!=6?uint8_t(t.which_variant):0;
  }
  static bool responsePayload(const uint8_t *payload,size_t size,pb_size_t expected=0){
    meshtastic_Telemetry t{};return decodeCanonical(payload,size,expected,t);
  }
  static bool response(const bench::RouteMetadata &m,pb_size_t expected,uint8_t *data,size_t capacity,size_t &size){
    size=0;
    if(!expected){
      meshtastic_Telemetry decoded{};
      if(!decodeCanonical(m.payload,m.payloadLength,0,decoded))return false;
      expected=decoded.which_variant;
    }
    if(expected==2)return TelemetryQuery::response(m,data,capacity,size);
    if(expected==6)return LocalStatsQuery::response(m,data,capacity,size);
    if(!TelemetryQuery::responseShape(m)||!responsePayload(m.payload,m.payloadLength,expected))return false;
    meshtastic_Data d{};d.portnum=meshtastic_PortNum_TELEMETRY_APP;d.request_id=m.request_id;
    d.has_bitfield=true;d.bitfield=m.bitfield;d.payload.size=m.payloadLength;memcpy(d.payload.bytes,m.payload,m.payloadLength);
    auto out=pb_ostream_from_buffer(data,capacity);if(!pb_encode(&out,meshtastic_Data_fields,&d))return false;
    size=out.bytes_written;return true;
  }
};
}
