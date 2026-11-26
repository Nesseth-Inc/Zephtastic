// SPDX-License-Identifier: GPL-3.0-only
#include <meshtastic/channel.pb.h>
#include "network/MqttClientProxy.h"
#include "network/MqttMapReport.h"
#include "stock/mqtt/Subscriptions.h"
#include <array>
#include <cassert>
#include <cstring>
#include <string>
#include <vector>

namespace donor {
using byte = uint8_t;
#define HAS_NETWORKING 1
#define MESHTASTIC_EXCLUDE_PKI 0
#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)
#define optstr(x) x
constexpr char APP_VERSION[] = "fixture-1.0";
constexpr uint32_t default_map_position_precision = 14;
struct { meshtastic_Config_DeviceConfig device{}; meshtastic_Config_LoRaConfig lora{}; } config;
meshtastic_User owner{};
meshtastic_Position localPosition{};
struct NodeDB { unsigned online; unsigned getNumOnlineMeshNodes(bool) { return online; } } db, *nodeDB = &db;
struct Channels {
    std::array<meshtastic_Channel,8> ch{};
    bool hasDefault = false;
    bool hasDefaultChannel() { return hasDefault; }
    size_t getNumChannels() { return ch.size(); }
    auto &getByIndex(size_t i) { return ch[i]; }
    const char *getGlobalId(size_t i) { return ch[i].settings.name; }
} channels;
struct MQTT {
    std::string cryptTopic = "msh/2/e/";
    std::vector<std::string> subscriptions;
    struct PubSub {
        MQTT &mqtt;
        void subscribe(const char *topic, int qos) { assert(qos == 1); mqtt.subscriptions.emplace_back(topic); }
    } pubSub{*this};
    std::vector<byte> seen;
    std::string seenTopic;
    unsigned calls = 0;
    void onReceive(char *topic, byte *payload, size_t length) {
        ++calls;
        seenTopic = topic;
        seen.assign(payload,payload+length);
    }
    void onClientProxyReceive(meshtastic_MqttClientProxyMessage);
    void sendSubscriptions();
};
#include "oracle.inc"
}

static void mapReports() {
    memset(donor::owner.long_name, 'L', sizeof(donor::owner.long_name)-1);
    strcpy(donor::owner.short_name,"ABCD");
    donor::owner.hw_model = meshtastic_HardwareModel_HELTEC_V3;
    donor::config.device.role = meshtastic_Config_DeviceConfig_Role_ROUTER;
    donor::config.lora.region = meshtastic_Config_LoRaConfig_RegionCode_EU_868;
    donor::config.lora.modem_preset = meshtastic_Config_LoRaConfig_ModemPreset_SHORT_FAST;
    mt::mqtt::MqttMapReport::Source s{};
    s.longName=donor::owner.long_name; s.shortName=donor::owner.short_name;
    s.firmwareVersion=donor::APP_VERSION; s.hwModel=donor::owner.hw_model;
    s.role=donor::config.device.role; s.region=donor::config.lora.region;
    s.modemPreset=donor::config.lora.modem_preset;
    for (unsigned p=0; p<=33; ++p) {
        for (int32_t coordinate : {0, 1, -1, -123456789, 123456789, INT32_MAX, INT32_MIN}) {
            donor::localPosition.latitude_i=s.latitudeI=coordinate;
            donor::localPosition.longitude_i=s.longitudeI=int32_t(~uint32_t(coordinate));
            donor::localPosition.altitude=s.altitude=-301;
            donor::db.online=s.numOnlineLocalNodes=13;
            donor::channels.hasDefault=s.hasDefaultChannel=(p%2)==0;
            meshtastic_MapReport actual{};
            mt::stock::MQTT::fillMapReport(actual,s,p);
            const auto expected=donor::fillMapReport(p);
            assert(!strcmp(actual.long_name,expected.long_name) && !strcmp(actual.short_name,expected.short_name));
            assert(!strcmp(actual.firmware_version,expected.firmware_version) && actual.role==expected.role);
            assert(actual.hw_model==expected.hw_model && actual.region==expected.region &&
                   actual.modem_preset==expected.modem_preset && actual.has_default_channel==expected.has_default_channel);
            assert(actual.has_opted_report_location==expected.has_opted_report_location);
            assert(actual.latitude_i==expected.latitude_i && actual.longitude_i==expected.longitude_i);
            assert(actual.altitude==expected.altitude && actual.position_precision==expected.position_precision &&
                   actual.num_online_local_nodes==expected.num_online_local_nodes);
        }
    }
}

static void subscriptions() {
    donor::MQTT original;
    for (unsigned mask=0; mask<256; ++mask) {
        for (unsigned i=0; i<8; ++i) {
            snprintf(donor::channels.ch[i].settings.name,sizeof(donor::channels.ch[i].settings.name),"slot%u",i);
            donor::channels.ch[i].settings.downlink_enabled=(mask&(1u<<i))!=0;
        }
        original.subscriptions.clear();
        original.sendSubscriptions();
        std::vector<std::string> actual;
        assert(mt::stock::MQTT::sendSubscriptions(8,
            [&](size_t i) { return donor::channels.ch[i].settings.downlink_enabled; },
            [&](size_t i) { return donor::channels.getGlobalId(i); },
            [&](const char *name) { actual.push_back(original.cryptTopic+name+"/+"); return true; }));
        assert(actual==original.subscriptions);
    }
}

static void proxy() {
    donor::MQTT original;
    meshtastic_MqttClientProxyMessage msg{};
    strcpy(msg.topic,"msh/2/e/test/fixture");
    for (unsigned tag : {0u, unsigned(meshtastic_MqttClientProxyMessage_text_tag),
                              unsigned(meshtastic_MqttClientProxyMessage_data_tag)}) {
        for (size_t n : {0u,1u,100u,434u,435u}) {
            if (tag==meshtastic_MqttClientProxyMessage_text_tag && n>=sizeof(msg.payload_variant.text)) continue;
            msg.which_payload_variant=tag;
            memset(&msg.payload_variant,0,sizeof(msg.payload_variant));
            if (tag==meshtastic_MqttClientProxyMessage_text_tag) memset(msg.payload_variant.text,'Z',n);
            if (tag==meshtastic_MqttClientProxyMessage_data_tag) {
                msg.payload_variant.data.size=n;
                for (size_t i=0;i<n;++i) msg.payload_variant.data.bytes[i]=uint8_t(i);
            }
            original.calls=0;
            original.onClientProxyReceive(msg);
            unsigned calls=0;
            bool ok=mt::mqtt::client_proxy::dispatchProtobuf(msg,[&](const char *topic,const uint8_t *p,size_t len) {
                ++calls;
                assert(original.seenTopic==topic && std::vector<uint8_t>(p,p+len)==original.seen);
                return true;
            });
            assert(calls==original.calls && ok==bool(calls));
        }
    }
}
void compareMeshPolicies();
void exerciseResponder();
void compareIdentityPolicy();
int main() { compareIdentityPolicy(); mapReports(); subscriptions(); proxy(); compareMeshPolicies(); exerciseResponder(); }
