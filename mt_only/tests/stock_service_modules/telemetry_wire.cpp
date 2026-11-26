// SPDX-License-Identifier: GPL-3.0-only
#include "telemetry/CompletionTelemetry.h"
#include "meshtastic/mesh.pb.h"
#include "ModuleAnnouncement.h"
#include <cassert>
#include <pb_decode.h>
#include <initializer_list>

void testTelemetryWire() {
    using Telemetry = mt::telemetry::CompletionTelemetry;
    Telemetry module;
    meshtastic_ModuleConfig_TelemetryConfig cfg{};
    cfg.health_measurement_enabled=cfg.air_quality_enabled=true;
    assert(module.configure(cfg) && module.poll(1000)==Telemetry::Result::Ok);
    for (auto variant : {meshtastic_Telemetry_air_quality_metrics_tag, meshtastic_Telemetry_health_metrics_tag}) {
        for (uint32_t utc : {0u,1700000000u}) {
            uint8_t buf[239]; size_t len=0;
            assert(module.response(variant,123,1100,utc,buf,sizeof(buf),len)==Telemetry::Result::Ok);
            meshtastic_Data envelope{};
            auto stream=pb_istream_from_buffer(buf,len);
            assert(pb_decode(&stream,meshtastic_Data_fields,&envelope) && envelope.request_id==123);
            meshtastic_Telemetry t{};
            auto body=pb_istream_from_buffer(envelope.payload.bytes,envelope.payload.size);
            assert(pb_decode(&body,meshtastic_Telemetry_fields,&t) && t.time==utc && t.which_variant==variant);
            if(variant==meshtastic_Telemetry_air_quality_metrics_tag) assert(t.variant.air_quality_metrics.co2==432);
            else assert(t.variant.health_metrics.heart_bpm==71);
            envelope.request_id=0;
            auto output=pb_ostream_from_buffer(buf,sizeof(buf));
            assert(pb_encode(&output,meshtastic_Data_fields,&envelope));
            bool isTelemetry=false;
            assert(mt::ModuleAnnouncement::canonical(buf,output.bytes_written,isTelemetry) && isTelemetry);
            assert(module.observePeer(55,envelope.payload.bytes,envelope.payload.size,8000));
            assert(module.peerSnapshot(55,variant,8100,t)==Telemetry::Result::Ok && t.time==utc);
            assert(module.peerSnapshot(55,variant,99000,t)==Telemetry::Result::Stale);
        }
    }
}
