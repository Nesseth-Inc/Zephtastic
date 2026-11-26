// SPDX-License-Identifier: GPL-3.0-only
#include "oracle.h"
#include "stock/modules/RangeTestDistance.h"
#include "stock/mqtt/MQTT.h"
#include "stock/modules/RemoteHardwareModule.h"
#include "stock/telemetry/SensorModules.h"
#include "modules/StoreForwardControl.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>

static void mqttCases()
{
    struct Channel {
        bool anyChannelUplinkEnabled, thisChannelUplinkEnabled, sourceIsLocal;
        bool localUploadConsent, defaultServer, encryptionEnabled;
    };
    donor::MQTT stock;
    for (unsigned flags = 0; flags < 256; ++flags) {
        meshtastic_MeshPacket enc{}, dec{};
        enc.which_payload_variant = meshtastic_MeshPacket_encrypted_tag;
        dec.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
        enc.via_mqtt = flags & 1;
        dec.from = flags & 2 ? 42 : 43;
        dec.decoded.has_bitfield = flags & 4;
        dec.decoded.bitfield = flags & 8 ? 1 : 0;
        donor::channels = {};
        donor::channels.slots[6].settings.uplink_enabled = flags & 16;
        donor::channels.slots[1].settings.uplink_enabled = flags & 32;
        donor::isConfiguredForDefaultServer = flags & 64;
        donor::moduleConfig.mqtt.encryption_enabled = flags & 128;
        Channel ch{bool(flags & 48), bool(flags & 16), dec.from == 42, true,
                   donor::isConfiguredForDefaultServer, donor::moduleConfig.mqtt.encryption_enabled};
        for (const auto port : {meshtastic_PortNum_TEXT_MESSAGE_APP, meshtastic_PortNum_RANGE_TEST_APP,
                                meshtastic_PortNum_DETECTION_SENSOR_APP}) {
            dec.decoded.portnum = port;
            donor::published = nullptr;
            stock.onSend(enc, dec, 6);
            assert(mt::stock::MQTT::onSend(enc, dec, ch) == donor::published);
        }
    }
}

static void gpioCases()
{
    using namespace donor;
    moduleConfig.remote_hardware.enabled = true;
    for (const uint64_t mask : {uint64_t(0), uint64_t(0x24), (UINT64_C(1)<<63)}) {
        RemoteHardwareModule stock;
        meshtastic_HardwareMessage request{}, got{};
        meshtastic_MeshPacket packet{};
        uint64_t localPins = 0, watched = 0;
        auto apply = [&]() {
            stock.handleReceivedProtobuf(packet, &request);
            return mt::stock::RemoteHardwareModule::handleReceivedProtobuf(request,
                [&](uint64_t m, uint64_t value){localPins = (localPins & ~m) | value; return 1;},
                [&](uint64_t m, uint64_t &out){out = m & localPins; return true;},
                [&](uint64_t m){watched = m; return 1;},
                [&](const meshtastic_HardwareMessage &m){got=m; return 1;}, -1, 0);
        };
        pins = 0;
        request.gpio_mask=mask; request.gpio_value=UINT64_MAX;
        request.type=meshtastic_HardwareMessage_Type_WRITE_GPIOS;
        assert(apply()==1 && pins==localPins);
        request.type=meshtastic_HardwareMessage_Type_READ_GPIOS;
        assert(apply()==1 && stock.emitted && stock.result.gpio_mask==got.gpio_mask &&
               stock.result.gpio_value==got.gpio_value && stock.result.type==got.type);
        request.type=meshtastic_HardwareMessage_Type_WATCH_GPIOS;
        assert(apply()==1 && stock.watchGpios==watched);
        uint32_t last=0; uint64_t previous=0; bool initial=true;
        for (uint32_t tick : {29999u,30000u,59999u,60000u,90000u,UINT32_MAX,5u}) {
            now=tick; if(tick==90000u) pins=localPins=0;
            stock.emitted=false; stock.runOnce();
            const bool emitted=mt::stock::RemoteHardwareModule::runOnce(true,watched,tick,localPins,
                                                                        last,previous,initial,got);
            assert(emitted==stock.emitted);
            if (emitted) assert(got.type==stock.result.type && got.gpio_value==stock.result.gpio_value &&
                                got.gpio_mask==stock.result.gpio_mask);
        }
    }
}

static bool stockServerRole(unsigned role, bool requested) {
    struct { struct { unsigned role; } device; } config{{role}};
    struct { struct { bool is_server; } store_forward; } moduleConfig{{requested}};
#include "store_role.inc"
}

static void storeClientCases()
{
    using Module = mt::stock::StoreForwardModule;
    for(unsigned role=0;role<=12;++role)for(bool requested:{false,true})
        assert(Module::serverRole(role,requested)==stockServerRole(role,requested));
    Module::ClientInfo info;
    donor::StoreForwardModule stock;
    stock.historyReturnMax=25; stock.historyReturnWindow=240;
    auto compare=[&](const meshtastic_StoreAndForward &p) {
        stock.observe(&p);
        Module::observeRouterInfo(p,info);
        assert(info.records==stock.records && info.requests==stock.requests &&
               info.historyRequests==stock.requests_history && info.heartbeat==stock.heartbeat &&
               info.returnMax==stock.historyReturnMax && info.returnWindowMinutes==stock.historyReturnWindow);
    };
    meshtastic_StoreAndForward p{};
    p.rr=meshtastic_StoreAndForward_RequestResponse_ROUTER_STATS;
    p.which_variant=meshtastic_StoreAndForward_stats_tag;
    p.variant.stats.messages_max=5000; p.variant.stats.requests=123456;
    p.variant.stats.requests_history=23; p.variant.stats.heartbeat=true;
    p.variant.stats.return_max=128; p.variant.stats.return_window=600;
    compare(p);
    p.rr=meshtastic_StoreAndForward_RequestResponse_ROUTER_HISTORY;
    compare(p); // wrong variant must preserve previous information
    p.which_variant=meshtastic_StoreAndForward_history_tag;
    for(uint32_t window : {0u,59999u,60000u,60001u,14400000u,UINT32_MAX}) {
        p.variant.history.window=window;
        compare(p);
    }
    p.rr=meshtastic_StoreAndForward_RequestResponse_ROUTER_STATS; compare(p);
    p.rr=meshtastic_StoreAndForward_RequestResponse_CLIENT_HISTORY; compare(p);
}

static void storeCases()
{
    using namespace mt::modules;
    for (unsigned count=0; count<=3; ++count) {
        for (uint32_t maximum : {1u,2u,5u}) {
            StoreForwardArchive::Record records[3]{};
            StoreForwardArchive::Cursor cursors[2]{};
            StoreForwardArchive archive(records,3,cursors,2);
            uint8_t body='x';
            StoreForwardArchive::MessageView view{};
            view.time=99; view.to=UINT32_MAX; view.from=9; view.payload=&body; view.payloadSize=1;
            for(unsigned i=0;i<count;++i){view.id=i+10; archive.add(view);}
            StoreForwardControl server(archive, {true,true,true,900,maximum,1});
            StoreForwardControl::Context ctx{}; ctx.peer=5;ctx.nowMillis=5000;ctx.nowEpochSeconds=100;
            donor::now=ctx.nowMillis;donor::epoch=ctx.nowEpochSeconds;
            donor::StoreForwardModule stock;
            donor::storeForwardModule=&stock;
            for (unsigned i=0; i<count; ++i) stock.packetHistory[i]={view.time,view.from,view.to};
            stock.records=3;stock.packetHistoryTotalCount=count;stock.historyReturnMax=maximum;
            stock.historyReturnWindow=1;stock.heartbeat=true;stock.requests=1;
            meshtastic_StoreAndForward m{};
            m.rr=meshtastic_StoreAndForward_RequestResponse_CLIENT_STATS;
            auto a=server.receive(m,ctx);
            stock.statsSend(5);
            const auto &x=a.control.variant.stats, &y=stock.reply.variant.stats;
            assert(a.control.rr==stock.reply.rr && a.control.which_variant==stock.reply.which_variant &&
                   x.messages_total==y.messages_total && x.messages_max==y.messages_max &&
                   x.messages_saved==y.messages_saved && x.up_time==y.up_time && x.requests==y.requests &&
                   x.requests_history==y.requests_history && x.heartbeat==y.heartbeat &&
                   x.return_max==y.return_max && x.return_window==y.return_window);
            m.rr=meshtastic_StoreAndForward_RequestResponse_CLIENT_HISTORY;
            a=server.receive(m,ctx);stock.historySend(60,5);
            assert(a.control.rr==stock.reply.rr && a.control.which_variant==stock.reply.which_variant &&
                   a.control.variant.history.history_messages==stock.reply.variant.history.history_messages &&
                   a.control.variant.history.window==stock.reply.variant.history.window &&
                   a.control.variant.history.last_request==stock.reply.variant.history.last_request &&
                   server.active()==stock.busy);
            donor::moduleConfig.store_forward.enabled=donor::air.allowed=true;
            unsigned sent=0;
            while(server.active()) {
                donor::now+=5000;
                donor::air.allowed=false;stock.attempted=false;
                assert(stock.runOnce()==5000 && !stock.attempted &&
                       server.poll(donor::now,false).kind==StoreForwardControl::ActionKind::None);
                donor::air.allowed=stock.available=true;
                stock.runOnce();
                auto replay=server.poll(donor::now,true);
                assert(stock.attempted && replay.kind==StoreForwardControl::ActionKind::Replay &&
                       replay.replay.id==10+sent && replay.destination==stock.busyTo);
                ++sent;
                if(replay.finalReplay) server.finishReplay();
            }
            assert(sent==std::min(count, maximum));
            stock.busy=false;
            for(uint32_t tick : {899999u,900000u,UINT32_MAX,0u}) {
                donor::now=tick;stock.reply={};stock.runOnce();
                auto heartbeat=server.poll(tick,true);
                const bool sent=heartbeat.kind==StoreForwardControl::ActionKind::ControlMessage;
                assert(sent==(stock.reply.rr==meshtastic_StoreAndForward_RequestResponse_ROUTER_HEARTBEAT));
                if(sent) assert(heartbeat.control.variant.heartbeat.period==stock.reply.variant.heartbeat.period);
            }

        }
    }
}

void testTelemetryWire();
void testSensorSchedule();
void testSensorTransports();

static void telemetryCases()
{
    struct Sample {
        bool hasAir=false,hasHealth=false;
        meshtastic_AirQualityMetrics air{};
        meshtastic_HealthMetrics health{};
    };
    for(unsigned flags=0;flags<16;++flags){
        donor::epoch=flags%2?1700000000u:0u;
        donor::max30102Sensor={bool(flags&1),bool(flags&2),1};
        donor::mlx90614Sensor={bool(flags&4),bool(flags&8),2};
        meshtastic_Telemetry wire{};
        donor::HealthTelemetryModule donorModule;
        const bool valid=donorModule.getHealthTelemetry(&wire);
        auto optical=donor::max30102Sensor, temp=donor::mlx90614Sensor;
        optical.calls=temp.calls=0;
        meshtastic_HealthMetrics m{};
        auto read=[&](donor::TelemetrySensor &s) {
            meshtastic_Telemetry t{};t.variant.health_metrics=m;
            const bool result=s.getMetrics(&t);m=t.variant.health_metrics;return result;
        };
        assert(mt::stock::HealthTelemetryModule::getHealthTelemetry(m,optical.active,
            [&](auto&){return read(optical);},temp.active,[&](auto&){return read(temp);})==valid);
        Sample sample{};sample.hasHealth=valid;sample.health=m;
        meshtastic_Telemetry adapted{};
        assert(mt::stock::telemetry::allocReply(meshtastic_Telemetry_health_metrics_tag,sample,donor::epoch,adapted)==valid);
        if(valid) assert(adapted.time==wire.time && adapted.which_variant==wire.which_variant);
        const auto &expected=wire.variant.health_metrics;
        assert(m.has_temperature==expected.has_temperature && m.temperature==expected.temperature &&
               m.has_heart_bpm==expected.has_heart_bpm && m.heart_bpm==expected.heart_bpm &&
               optical.calls==donor::max30102Sensor.calls && temp.calls==donor::mlx90614Sensor.calls);
    }
    using AirReader=bool(*)(meshtastic_AirQualityMetrics&);
    for(bool ok : {false,true}){
        donor::TelemetrySensor s{true,ok,0};
        donor::AirQualityTelemetryModule module{{&s}};
        meshtastic_Telemetry wire{};
        bool valid=module.getAirQualityTelemetry(&wire);
        AirReader readers[]={ok ? +[](meshtastic_AirQualityMetrics &m){
            m.has_co2=true;m.co2=432;return true;
        } : +[](meshtastic_AirQualityMetrics&){return false;}};
        meshtastic_AirQualityMetrics air{};
        assert(mt::stock::AirQualityTelemetryModule::getAirQualityTelemetry(air, readers)==valid);
        Sample sample{};sample.hasAir=valid;sample.air=air;
        meshtastic_Telemetry adapted{};
        assert(mt::stock::telemetry::allocReply(meshtastic_Telemetry_air_quality_metrics_tag,sample,donor::epoch,adapted)==valid);
        if(valid) assert(adapted.time==wire.time && adapted.which_variant==wire.which_variant);
        assert(air.has_co2==wire.variant.air_quality_metrics.has_co2 && air.co2==wire.variant.air_quality_metrics.co2);
    }
}
int main()
{
    testSensorSchedule();
    testSensorTransports();
    for(double lat : {-89.9, -45.0, 0.0, 42.0, 89.9})
        for(double a : {-179.99,0.0,179.99})
            for(double b : {-179.99,0.0,179.99}) {
                assert(mt::stock::range::latLongToMeter(lat,a,lat,b) ==
                       donor::GeoCoord::latLongToMeter(lat,a,lat,b));
                assert(mt::stock::range::latLongToMeter(lat,a,-lat,b) ==
                       donor::GeoCoord::latLongToMeter(lat,a,-lat,b));
            }

    mqttCases();gpioCases();storeCases();storeClientCases();telemetryCases();testTelemetryWire();
    std::puts("PASS pinned MQTT packet selection, GPIO commands/watch, StoreForward history/stats, AQ/health acquisition");
}
