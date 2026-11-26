// SPDX-License-Identifier: GPL-3.0-only
#include "oracle.h"
#include "modules/peripherals/AmbientLightingCore.h"
#include "modules/CannedMessageSettings.h"
#include "stock/peripherals/DetectionSensorModule.h"
#include "stock/peripherals/ExternalNotificationModule.h"
#include "ui/CannedMenu.h"
#include "serial/SerialEngine.h"
#include "stock/peripherals/SerialPosition.h"
#include <vector>

PB_BIND(meshtastic_Position, meshtastic_Position, AUTO)

static void serialCases()
{
    using mt_serial::Mode;
    using namespace oracle;
    const std::vector<std::pair<Mode, meshtastic_ModuleConfig_SerialConfig_Serial_Mode>> modes = {
        {Mode::Simple, meshtastic_ModuleConfig_SerialConfig_Serial_Mode_DEFAULT},
        {Mode::Simple, meshtastic_ModuleConfig_SerialConfig_Serial_Mode_SIMPLE},
        {Mode::TextMessage, meshtastic_ModuleConfig_SerialConfig_Serial_Mode_TEXTMSG},
        {Mode::Proto, meshtastic_ModuleConfig_SerialConfig_Serial_Mode_PROTO}
    };
    meshtastic_NodeInfoLite node{"AB", "Node AB"};
    for (const auto &[mode, wire] : modes) for (bool echo : {false, true})
        for (bool hasNode : {false, true}) for (bool own : {false, true}) {
            moduleConfig.serial.enabled = true;
            moduleConfig.serial.mode = wire;
            moduleConfig.serial.echo = echo;
            nodes.peer = hasNode ? &node : nullptr;
            mt_serial::Engine engine({mode, 0, echo});
            SerialModuleRadio stock;
            std::string actual;
            uart.out.clear();
            auto write = [](void *out, const uint8_t *b, size_t n) {
                static_cast<std::string *>(out)->append(reinterpret_cast<const char *>(b), n);
            };
            meshtastic_MeshPacket p{};
            p.from = own ? 1 : 2;
            const uint8_t binary[] = {'x', 0, 'Y', 255};
            memcpy(p.decoded.payload.bytes, binary, sizeof(binary));
            p.decoded.payload.size = sizeof(binary);
            for (uint32_t id : {0u, 42u, 42u, 43u}) {
                p.id = id;
                stock.handleReceived(p);
                engine.mesh_receive(own, id, hasNode ? node.short_name : nullptr,
                                    binary, sizeof(binary), write, &actual);
                assert(actual == uart.out);
            }
        }
    moduleConfig.serial.enabled = true;
    for (auto mode : {meshtastic_ModuleConfig_SerialConfig_Serial_Mode_NMEA,
                      meshtastic_ModuleConfig_SerialConfig_Serial_Mode_CALTOPO}) {
        assert(mt::stock::serial::positionMode(mode));
        for (bool known : {false, true}) for (bool valid : {false, true}) {
            nodes.peer = known ? &node : nullptr;
            moduleConfig.serial.mode = mode;
            meshtastic_MeshPacket p{};
            p.from = 2; p.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
            p.decoded.portnum = meshtastic_PortNum_POSITION_APP;
            auto &body = p.decoded.payload;
            if (valid) {
                meshtastic_Position fix{};
                fix.has_latitude_i = fix.has_longitude_i = fix.has_altitude = true;
                fix.latitude_i = -100000; fix.longitude_i = 218000; fix.altitude = 17;
                auto out = pb_ostream_from_buffer(body.bytes, sizeof(body.bytes));
                assert(pb_encode(&out, meshtastic_Position_fields, &fix));
                body.size = out.bytes_written;
            } else {
                body.bytes[0] = 0xff; body.size = 1;
            }
            uart.out.clear();
            SerialModuleRadio stock;
            stock.ourPortNum = meshtastic_PortNum_POSITION_APP;
            stock.handleReceived(p);
            bool emitted = mt::stock::serial::handleReceivedPosition(body.bytes, body.size,
                known ? node.long_name : nullptr, [&](const mt_serial::Position &fix, const char *sender) {
                    assert(fix.latitude_i == waypoint.lat && fix.longitude_i == waypoint.lon &&
                           fix.altitude_m == waypoint.alt && waypoint.name == sender && fix.valid);
                    assert(waypoint.caltopo == (mode == meshtastic_ModuleConfig_SerialConfig_Serial_Mode_CALTOPO));
                    return true;
                });
            assert(emitted == valid && emitted == !uart.out.empty());
        }
    }
    puts("PASS Serial receive modes, named/unknown peers, binary bytes, duplicate and zero IDs");
}

static void cannedCases()
{
    oracle::CannedMessageModule picker{};
    for (uint32_t to:{UINT32_MAX,42u}) for (bool bell:{false,true}) {
        oracle::meshtastic_NodeInfoLite peer{"N","Name"};peer.num=42;peer.public_key.size=32;
        oracle::nodes.peer=&peer;
        oracle::moduleConfig.canned_message.send_bell=bell;
        picker.sendText(to,3,"message",false);
        uint8_t body[201];const size_t size=mt::stock::canned::copyTextPayload(body,sizeof(body),"message",7,bell);
        bool called=false;
        auto expect=[&](uint32_t dest,const uint8_t *payload,size_t bytes,unsigned channel,bool ack) {
            called=true;
            assert(dest==picker.packet.to && channel==picker.lastChannel && ack==picker.packet.want_ack);
            assert(picker.packet.decoded.payload.size==bytes && !memcmp(picker.packet.decoded.payload.bytes,payload,bytes));
            return 11;
        };
        const auto result=mt::stock::canned::sendText(to,3,body,size,
            [&](const uint8_t *p,size_t n,unsigned c,bool a){assert(to==UINT32_MAX);return expect(to,p,n,c,a);},
            [&](uint32_t d,const uint8_t *p,size_t n,unsigned c,bool a){assert(to==42 && picker.packet.pki_encrypted);return expect(d,p,n,c,a);});
        assert(called && result==11);
        oracle::nodes.peer=nullptr;
    }
    for (int count=1;count<=49;++count) {
        picker.messagesCount=count;
        for (int i=0;i<count;++i) {
            picker.currentMessageIndex=i;
            assert(mt::stock::canned::moveSelection(i,count,true)==unsigned(picker.getNextIndex()));
            assert(mt::stock::canned::moveSelection(i,count,false)==unsigned(picker.getPrevIndex()));
        }
    }
    std::vector<std::string> cases{"only", "|", "A|", "||", "|A||B|", "Hi|Bye|Yes|No|Ok", u8"été|はい|✓"};
    for (int n = 1; n <= 70; ++n) {
        std::string value;
        for (int i = 0; i < n; ++i) value += (i ? "|Z" : "X");
        cases.push_back(value);
    }
    cases.emplace_back(200, '|');
    cases.emplace_back(200, 'x');
    for (const auto &value : cases) {
        oracle::CannedMessageModule stock{};
        std::strcpy(oracle::cannedMessageModuleConfig.messages, value.c_str());
        const auto count = unsigned(stock.splitConfiguredMessages() - 2);
        mt::ui::CannedMenu menu;
        menu.bind(value.data(), value.size());
        assert(count == menu.count());
        for (unsigned i = 0; i < count; ++i) {
            const char *entry; size_t size;
            assert(menu.entry(i, entry, size));
            assert(std::string(entry, size) == stock.messages[i + 1]);
        }
        const char *entry; size_t size;
        assert(menu.entry(count, entry, size) && std::string(entry, size) == "[Back]");
        assert(!menu.entry(count + 1, entry, size));
    }
    mt::modules::CannedMessageSettings stored;
    std::strcpy(oracle::cannedMessageModuleConfig.messages, stored.messages);
    oracle::CannedMessageModule admin;
    for (const auto &value : {std::string(""), std::string("Yes|No|"), std::string("Yes|No|"), std::string(200, 'v')}) {
        const unsigned before = admin.saves;
        admin.handleSetCannedMessageModuleMessages(value.c_str());
        const auto result = stored.assign(value.c_str(), value.size());
        assert((result == mt::stock::canned::AssignResult::Changed) == (before != admin.saves));
        assert(!std::strcmp(stored.messages, oracle::cannedMessageModuleConfig.messages));
    }
    puts("PASS Canned messages against stock selector, sendText addressing, ACK, channel, BEL and payload, menu splitting, UTF-8 and capacity");
}

static void externalCases()
{
    using namespace oracle;
    moduleConfig.external_notification.enabled = true;
    for (unsigned mask = 0; mask < 64; ++mask) for (unsigned state = 0; state < 16; ++state) {
        const mt::stock::notification::Flags flags{bool(mask & 1), bool(mask & 2), bool(mask & 4),
                                                   bool(mask & 8), bool(mask & 16), bool(mask & 32)};
        auto &cfg = moduleConfig.external_notification;
        cfg.alert_bell = flags.alertBell; cfg.alert_message = flags.alertMessage;
        cfg.alert_bell_vibra = flags.alertBellVibra; cfg.alert_message_vibra = flags.alertMessageVibra;
        cfg.alert_bell_buzzer = flags.alertBellBuzzer; cfg.alert_message_buzzer = flags.alertMessageBuzzer;
        cfg.output_ms = 1000; cfg.nag_timeout = mask & 1 ? 3 : 0;
        ExternalNotificationModule stock{};
        muted = state & 1;
        stock.canBuzzValue = state & 2;
        stock.buzzerShouldAlert = state & 4;
        now = UINT32_MAX - 10;
        meshtastic_MeshPacket p{};
        p.from = 2; p.to = 1;
        p.decoded.payload.bytes[0] = 0;
        p.decoded.payload.bytes[1] = state & 8 ? 7 : 'a';
        p.decoded.payload.size = 2;
        const auto expected = mt::stock::notification::handleReceived(flags, p.decoded.payload.bytes, 2, muted,
                                                                       stock.canBuzzValue, stock.buzzerShouldAlert);
        stock.handleReceived(p);
        assert(expected.generic == stock.led && expected.vibra == stock.vibra && expected.buzzer == stock.buzz);
        assert(stock.isNagging == (expected.generic || expected.vibra || expected.buzzer));
        if (stock.isNagging)
            assert(mt::stock::notification::armNagCycle(now, cfg.nag_timeout, cfg.output_ms) == stock.nagCycleCutoff);
    }
    for(bool armed : {false,true}) for(uint32_t delta : {0u,99u,100u,101u}) {
        ExternalNotificationModule donor;donor.isNagging=armed;donor.nagCycleCutoff=20;
        moduleConfig.external_notification.enabled=true;moduleConfig.external_notification.output_ms=100;
        now=uint32_t(20-100)+delta;
        for(auto &last:externalTurnedOn)last=uint32_t(20-100);
        const bool expired=mt::stock::notification::deadlinePassed(now,20);
        assert(donor.runOnce()==((!armed||expired)?INT32_MAX:100));
        donor.isNagging=armed;
        bool actual=armed;
        for(int i=0;i<2;++i)
          assert(bool(donor.handleInputEvent(nullptr))==mt::stock::notification::handleInputEvent(actual,[&]{actual=false;}));
        assert(actual==donor.isNagging);
    }
    puts("PASS External notification, all 64 flag combinations, mute, BEL, latch and cutoff rollover");
}

static void detectionCases()
{
    using namespace oracle;
    for (unsigned type = 0; type < 6; ++type) for (bool old : {false, true}) for (bool raw : {false, true}) {
        level = raw;
        moduleConfig.detection_sensor.detection_trigger_type =
            static_cast<meshtastic_ModuleConfig_DetectionSensorConfig_TriggerType>(type);
        DetectionSensorModule stock;
        const bool isDetected = stock.hasDetectionEvent();
        assert(mt::stock::detection::hasDetectionEvent(type, raw) == isDetected);
        const auto actual = mt::stock::detection::triggerVerdict(type, old, isDetected);
        const auto upstream = handlers[type](old, isDetected);
        assert((upstream == DetectionSensorVerdictDetected) == (actual == mt::stock::detection::Detected));
        assert((upstream == DetectionSensorVerdictSendState) == (actual == mt::stock::detection::SendState));
        assert((upstream == DetectionSensorVerdictNoop) == (actual == mt::stock::detection::Noop));
    }
    for (const char *name : {"", "Entry", "1234567890123456789"}) {
        std::strcpy(moduleConfig.detection_sensor.name, name);
        for (bool bell : {false, true}) for (bool state : {false, true}) {
            moduleConfig.detection_sensor.send_bell = bell;
            for (auto kind : {mt::stock::detection::Detected, mt::stock::detection::SendState}) {
                DetectionSensorModule stock;
                if (kind == mt::stock::detection::Detected) stock.sendDetectionMessage();
                else stock.sendCurrentStateMessage(state);
                uint8_t bytes[40]{};
                size_t size = mt::stock::detection::composeMessage(bytes, sizeof(bytes), name, kind, state, bell);
                const auto &sent = sink.sent->decoded.payload;
                assert(!sink.sent->want_ack && size == sent.size && !memcmp(bytes, sent.bytes, size));
            }
        }
    }
    for(unsigned trigger=0;trigger<6;++trigger) for(bool prev : {false,true}) for(bool input : {false,true})
      for(unsigned elapsed : {999u,1000u,1999u,2000u,3000u}) for(unsigned heartbeat : {0u,1u,2u}){
        auto &cfg=moduleConfig.detection_sensor;
        cfg.enabled=true;cfg.minimum_broadcast_secs=2;cfg.state_broadcast_secs=heartbeat;
        cfg.detection_trigger_type=static_cast<decltype(cfg.detection_trigger_type)>(trigger);
        DetectionSensorModule donor;donor.wasDetected=prev;donor.lastSentToMesh=UINT32_MAX-10;
        now=donor.lastSentToMesh+elapsed;level=input;sink.sent=nullptr;
        const bool active=mt::stock::detection::hasDetectionEvent(trigger,input);
        bool previous=prev;
        const auto sample=mt::stock::detection::runOnce(trigger,previous,active,elapsed>=2000,
                                                         heartbeat && elapsed>=heartbeat*1000,false);
        const auto interval=donor.runOnce();
        assert(previous==donor.wasDetected && bool(sink.sent)==(sample.verdict!=mt::stock::detection::Noop));
        assert(interval==(sink.sent?DELAYED_INTERVAL:GPIO_POLLING_INTERVAL));
        if(sink.sent){
          uint8_t msg[40]{};
          const auto n=mt::stock::detection::composeMessage(msg,sizeof(msg),cfg.name,sample.verdict,active,cfg.send_bell);
          assert(n==sink.sent->decoded.payload.size&&!memcmp(msg,sink.sent->decoded.payload.bytes,n));
        }
      }
    puts("PASS Detection sensor, all trigger types, raw polarities and previous states");
}

static void ambientCases()
{
    using Core = mt::modules::peripherals::AmbientLightingCore;
    auto &cfg = oracle::moduleConfig.ambient_lighting;
    struct State { uint8_t rgb[3]; uint8_t current; bool failed; int off; };
    auto apply = [](void *v, uint8_t current, uint8_t r, uint8_t g, uint8_t b) {
        auto &s = *static_cast<State *>(v);
        s = {{r, g, b}, current, s.failed, s.off};
        return s.failed ? -1 : 0;
    };
    auto off = [](void *v) { ++static_cast<State *>(v)->off; return 0; };
    for (uint8_t value : {0, 1, 35, 255}) {
        cfg.led_state = true; cfg.red = value; cfg.green = 1; cfg.blue = 240; cfg.current = 25;
        oracle::LightProbe probe;
        State s{};
        Core core;
        core.configure({true, 25, value, 1, 240});
        const uint32_t now = UINT32_MAX - 1000;
        assert(core.poll(now, true, &s, apply, off) == Core::Result::Applied);
        const auto delay = probe.runOnce();
        for (unsigned i = 0; i < 3; ++i) assert(s.rgb[i] == oracle::rgb[i]);
        assert(s.current == 25 && delay == Core::RefreshMs);
        assert(core.poll(now + delay - 1, true, &s, apply, off) == Core::Result::NotDue);
        s.failed = true;
        assert(core.poll(now + delay, true, &s, apply, off) == Core::Result::Failed);
        s.failed = false;
        assert(core.poll(now + delay, true, &s, apply, off) == Core::Result::Applied);
        core.configure({}); assert(core.poll(now + delay, true, &s, apply, off) == Core::Result::Disabled);
        assert(s.off == 1);
        core.configure({true, 1, 2, 3, 4});
        assert(core.poll(0, false, &s, apply, off) == Core::Result::Unavailable);
    }
    puts("PASS Ambient lighting against entire stock RGB header, refresh, disabled, backend refusal");
}

void nmeaCases();
void weatherCases();
void notificationCases();
int main()
{
    nmeaCases();
    weatherCases();
    notificationCases();
    serialCases();
    cannedCases();
    externalCases();
    detectionCases();
    ambientCases();
}
