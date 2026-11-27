// SPDX-License-Identifier: GPL-3.0-only
#include "../device_config_fixture.h"
#include "../relay_fixture_provider.h"
#include <algorithm>
#include <cstring>
#include "donor.h"

namespace {
constexpr uint32_t Peer=0x24680135;
unsigned trafficCases=0, positionCases=0;

struct Recorder : Radio {
    std::vector<std::vector<uint8_t>> frames;
    int send(uint8_t *p, size_t n) override {
        frames.emplace_back(p, p+n);
        return Radio::send(p,n);
    }
};

mt::Config configuration() {
    mt::Config cfg;
    cfg.bytes[4]=1;
    auto node=cfg.node();
    node.setPublic();
    cfg.node(node);
    assert(cfg.setDeviceRole(meshtastic_Config_DeviceConfig_Role_ROUTER,0));
    cfg.setTxEnabled(true);
    return cfg;
}

struct Relay {
    Recorder radio;
    mt::test::KnownChannelFixtureClock clock;
    mt::modules::TrafficManagementWindow window;
    RelayFixtureProvider provider{Node};
    mt::Service service;
    donor::TrafficManagementModule stock;
    uint32_t id=100, arrival=1000;

    Relay(const mt::Config &cfg, const meshtastic_ModuleConfig_TrafficManagementConfig &options,
          bool hasIndexedKey=false) : service(radio,true,Node,127,true,false,hasIndexedKey?&provider:nullptr) {
        std::cerr<<"relay setup: indexed="<<hasIndexedKey<<" precision="<<cfg.primaryPositionPrecision()
                 <<" rate="<<options.rate_limit_max_packets<<" dedup="<<options.position_min_interval_secs<<'\n';
        clock.bind(service);
        assert(service.apply(cfg));
        assert(service.start(false,0));
        assert(service.armEndpoint(true));
        assert(service.relaying());
        mt::modules::TrafficManagementSettings settings;
        settings.set(options);
        window.configure(settings);
        stock.moduleConfig.traffic_management=options;
        donor::config.lora.use_preset=cfg.usePreset();
        donor::config.lora.modem_preset=static_cast<meshtastic_Config_LoRaConfig_ModemPreset>(cfg.modemPreset());
        donor::channels=donor::Channels{};
        for (unsigned i=0;i<8;++i) {
            mt::ChannelRecord raw;
            assert(cfg.channelRecord(i,raw));
            auto &channel=donor::channels.table[i];
            channel.role=static_cast<meshtastic_Channel_Role>(raw.role());
            std::strcpy(channel.settings.name,raw.name());
            // The suite provisions primary via Meshtastic PSK index 1; ZT stores its expansion.
            channel.settings.psk.size=i ? raw.keySize() : 1;
            channel.settings.has_module_settings=true;
            channel.settings.module_settings.position_precision=raw.precision();
        }
    }

    void ingress(unsigned slot, uint32_t from, const meshtastic_Data &data, uint32_t uptime, bool allowed) {
        const auto sends=radio.frames.size();
        const auto now=arrival;
        arrival+=10000;
        uint8_t plain[239]{}, frame[255]{};
        auto wire=pb_ostream_from_buffer(plain,sizeof(plain));
        assert(pb_encode(&wire,meshtastic_Data_fields,&data));
        mt::PrimaryChannel::Selection key{};
        assert(service.config.channelSelection(slot,key));
        bench::MtCodec encoder;
        assert(encoder.setKey(key.hash,key.bytes,key.length));
        mt::PrimaryChannel::clear(key);
        size_t size=0;
        assert(encoder.encodeZeroHopData(from,UINT32_MAX,++id,plain,wire.bytes_written,frame,sizeof(frame),size));
        frame[12]=(3u<<5)|3u;
        bool accepted=false;
        service.trafficManagement(&window,uptime);
        std::cerr<<"ingress: port="<<int(data.portnum)<<" slot="<<slot<<" id="<<id
                 <<" uptime="<<uptime<<" expect="<<allowed<<'\n';
        service.rx(frame,size,-5,now,0,true,&accepted);
        assert(accepted==allowed && service.trafficSuppressed()==!allowed);
        assert(service.route.pending()==unsigned(allowed));
        if (allowed) {
            // Poll at each deterministic tick. A decision cannot appear on the radio
            // before the real route frame is due, or leave an extra packet behind.
            bool sent=false;
            for (unsigned elapsed=0; elapsed<=2000 && !sent; ++elapsed) {
                const bool due=service.route.next(now+elapsed)!=nullptr;
                service.poll(now+elapsed,0);
                sent=radio.frames.size()>sends;
                assert(sent==due);
                if (sent) {
                    const auto &actual=radio.frames.back();
                    assert(actual.size()==size && !memcmp(actual.data(),frame,12));
                    assert(actual[13]==frame[13] && (actual[12]&7u)==2);
                    assert(!memcmp(actual.data()+16,frame+16,size-16));
                    radio.done=true;
                    service.poll(now+elapsed+1,0);
                }
            }
            assert(sent);
        }
        service.poll(now+3000,0);
        assert(!service.faulted() && !service.route.pending() && radio.frames.size()==sends+unsigned(allowed));
    }

    void text(uint32_t from, uint32_t uptime, bool expected) {
        donor::TrafficManagementModule::now=uptime;
        const bool allow=!stock.isRateLimited(from,uptime);
        assert(allow==expected);
        meshtastic_Data d{};
        d.has_bitfield=true;
        d.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;
        d.payload.size=1;
        d.payload.bytes[0]='t';
        ingress(0,from,d,uptime,allow);
        ++trafficCases;
    }

    void position(unsigned slot, const meshtastic_Position &pos, bool known, unsigned precision,
                  bool privacyAllows, uint32_t uptime, bool expected) {
        donor::TrafficManagementModule::now=uptime;
        meshtastic_MeshPacket packet{};
        packet.from=Peer;
        packet.channel=slot;
        assert(donor::channels.isWellKnownChannel(slot)==known);
        if (known) assert(donor::getPositionPrecisionForChannel(slot)==precision);
        const bool duplicate=privacyAllows && known && stock.shouldDropPosition(&packet,&pos,uptime);
        const bool rateLimited=!duplicate && stock.isRateLimited(Peer,uptime);
        const bool allow=privacyAllows && !duplicate && !rateLimited;
        assert(allow==expected);
        meshtastic_Data d{};
        d.has_bitfield=true;
        d.portnum=meshtastic_PortNum_POSITION_APP;
        auto stream=pb_ostream_from_buffer(d.payload.bytes,sizeof(d.payload.bytes));
        assert(pb_encode(&stream,meshtastic_Position_fields,&pos));
        d.payload.size=stream.bytes_written;
        ingress(slot,Peer,d,uptime,allow);
        ++positionCases;
    }
};

meshtastic_Position position(int32_t lat, int32_t lon, unsigned bits) {
    meshtastic_Position p{};
    p.has_latitude_i=p.has_longitude_i=true;
    p.latitude_i=lat;
    p.longitude_i=lon;
    donor::applyPositionPrecision(p,bits);
    return p;
}

void relayCases() {
    auto cfg=configuration();
    meshtastic_ModuleConfig_TrafficManagementConfig options{};
    options.rate_limit_window_secs=600;
    options.rate_limit_max_packets=2;
    {
        Relay r(cfg,options);
        r.text(Peer,0,true);
        r.text(Peer,0,true);
        r.text(Peer,0,false);
        r.text(Peer+1,0,true);
        r.text(Peer,599999,false);
        r.text(Peer,600000,true);
    }
    options={};
    options.position_min_interval_secs=720;
    {
        // Even with no local position source or configured grid, stock dedups on
        // the 19-bit fallback. A suppressed duplicate cannot slide the window.
        Relay r(cfg,options);
        auto p=position(123456789,-23456789,32);
        r.position(0,p,true,0,true,0,true);
        r.position(0,p,true,0,true,359999,false);
        r.position(0,p,true,0,true,719999,false);
        r.position(0,p,true,0,true,720000,true);
    }
    {
        options.rate_limit_window_secs=300;
        options.rate_limit_max_packets=2;
        Relay r(cfg,options);
        const auto p=position(123456789,-23456789,32);
        r.position(0,p,true,0,true,0,true);
        r.position(0,p,true,0,true,0,false); // Dupes do not spend the next rate token.
        r.text(Peer,0,true);
        r.text(Peer,0,false);
        options.rate_limit_window_secs=0;
        options.rate_limit_max_packets=0;
    }
    auto gnss=cfg.gnssSettings();
    gnss.enabled=true;
    assert(cfg.setGnssSettings(gnss) && cfg.setPrimaryPositionPrecision(24));
    mt::ChannelRecord secondary;
    assert(secondary.set(2,"Local",nullptr,0,12) && cfg.setSecondaryChannel(1,secondary));
    const uint8_t alias=2;
    assert(secondary.set(2,"MediumFast",&alias,1,12) && cfg.setSecondaryChannel(2,secondary));
    {
        Relay r(cfg,options,true);
        auto precise=position(123456789,-23456789,32);
        auto compliant=position(precise.latitude_i,precise.longitude_i,15);
        r.position(0,precise,true,15,false,0,false);
        r.position(0,compliant,true,15,true,0,true);
        r.position(0,compliant,true,15,true,0,false);
        auto next=position(precise.latitude_i+(1<<17),precise.longitude_i,15);
        r.position(0,next,true,15,true,0,true);
    }
    {
        Relay r(cfg,options,true);
        auto p=position(123456789,-23456789,32);
        r.position(1,p,false,12,true,0,true);
        r.position(1,p,false,12,true,0,true); // Named channel using the default key.
        auto aliasPosition=position(123456789,-23456789,12);
        r.position(2,aliasPosition,true,12,true,0,true);
        r.position(2,aliasPosition,true,12,true,0,false); // Preset name, public key index 2.
    }
}

void localCases() {
    for (unsigned configured : {0u,1u,12u,15u,24u,32u}) {
        auto cfg=configuration();
        assert(cfg.setFixedPosition(123456789,-23456789,false,0));
        assert(cfg.setPrimaryPositionPrecision(configured));
        Relay r(cfg,{});
        auto before=position(123456789,-23456789,32);
        donor::applyPositionPrecision(before,donor::getPositionPrecisionForChannel(0));
        const int result=r.service.announcePosition(1000,0x15700123u);
        // At one bit the stock midpoint is outside the geographic latitude range.
        const bool transmits=configured && before.latitude_i>=-900000000 && before.latitude_i<=900000000;
        assert(transmits ? result==0 : result==-EACCES);
        assert(r.service.endpointControls()==unsigned(transmits) && r.radio.frames.empty());
        r.service.poll(1500,0);
        assert(r.radio.frames.size()==unsigned(transmits));
        if (transmits) {
            const auto &wire=r.radio.frames.back();
            bench::RouteMetadata m{};
            assert(r.service.codec.decode(wire.data(),wire.size(),m) && m.channelDecoded);
            assert(m.portnum==meshtastic_PortNum_POSITION_APP && !m.wantResponse);
            meshtastic_Position emitted{};
            auto stream=pb_istream_from_buffer(m.payload,m.payloadLength);
            assert(pb_decode(&stream,meshtastic_Position_fields,&emitted) && !stream.bytes_left);
            assert(emitted.has_latitude_i && emitted.has_longitude_i && emitted.precision_bits==before.precision_bits);
            assert(emitted.latitude_i==before.latitude_i && emitted.longitude_i==before.longitude_i);
            r.radio.done=true;
            r.service.poll(1501,0);
        }
        ++positionCases;
    }
}
}

int main() {
    relayCases();
    localCases();
    std::cout<<"{\"traffic_management\":"<<trafficCases<<",\"position\":"<<positionCases<<"}\n";
}
