// SPDX-License-Identifier: GPL-3.0-only
#include "modules/audio/AudioModule.h"
#include "modules/audio/AudioWire.h"
#include "modules/peripherals/PaxcounterCore.h"
#include "meshtastic/mesh.pb.h"
#include "meshtastic/portnums.pb.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

using Bytes = std::vector<uint8_t>;
static std::vector<Bytes> played;

namespace donor {
struct AudioWorker {
    uint8_t rx_encode_frame[233]{}, tx_header[4]{}, output_buffer[320]{};
    int encode_codec_size = 0, rx_encode_frame_index = 0, adc_buffer_size = 1, codec2 = 0;
};
static int frameBytes;
static std::vector<Bytes> decoded;
static void codec2_decode(int, uint8_t *, const uint8_t *frame) {
    decoded.emplace_back(frame, frame + frameBytes);
}
static void i2s_write(int, void *, size_t, size_t *, int) {}
constexpr int I2S_PORT = 0;
static int pdMS_TO_TICKS(int n) { return n; }

static void receive(AudioWorker *audioModule) {
    size_t bytesOut = 0;
#include "stock_audio_rx.inc"
}
static size_t packetSize(int encode_codec_size) {
    unsigned char tx_header[4]{};
    int encode_frame_num, encode_frame_size;
#include "stock_audio_size.inc"
    return sizeof(tx_header) + encode_frame_size;
}

using NodeNum = uint32_t;
struct Packet {
    uint32_t to = 0;
    struct { bool want_response = true; struct { size_t size{}; uint8_t bytes[233]{}; } payload; } decoded;
    bool want_ack=true;
    meshtastic_MeshPacket_Priority priority = meshtastic_MeshPacket_Priority_UNSET;
};
using meshtastic_MeshPacket = Packet;
constexpr int RX_SRC_LOCAL = 0;
struct Service {
    unsigned calls = 0;
    void sendToMesh(Packet *) { ++calls; }
    void sendToMesh(Packet *, int, bool) { ++calls; }
};
static Service services;
static Service *service = &services;
static unsigned long clockMs;
static unsigned long millis() { return clockMs; }
struct PaxcounterModule {
    bool reportedDataSent = true, allocate = true;
    struct { uint32_t wifi_count = 0, ble_count = 0; } count_from_libpax;
    meshtastic_Paxcount payload{};
    Packet packet;
    Packet *allocDataProtobuf(const meshtastic_Paxcount &pl) {
        payload = pl;
        return allocate ? &packet : nullptr;
    }
    Packet *allocReply();
    bool sendInfo(NodeNum dest);
};
static PaxcounterModule *paxcounterModule;
#define LOG_INFO(...) ((void)0)
#include "stock_pax_send.inc"
#include "stock_pax_reply.inc"
enum class RadioState {rx,tx};
struct UIFrameEvent { enum class Action { REGENERATE_FRAMESET }; Action action{}; };
static bool pressed=false;
static bool digitalRead(unsigned){return pressed;}
constexpr bool HIGH=true;
constexpr unsigned PTT_PIN=1;
static struct { struct { unsigned ptt_pin=0; } audio; } moduleConfig;
struct AudioModule {
    RadioState radio_state=RadioState::rx;
    uint8_t tx_header[4]={0xc0,0xde,0xc2,0};
    uint8_t tx_encode_frame[233]{};
    size_t tx_encode_frame_index=4;
    Packet packet;
    Packet *allocReply(){return &packet;}
    void notifyObservers(UIFrameEvent *){}
    void sendPayload(NodeNum dest=UINT32_MAX,bool wantReplies=false);
    void poll() { UIFrameEvent e;
#include "stock_ptt.inc"
    }
};
#include "stock_audio_send.inc"
#undef LOG_INFO
}

namespace mt::audio {
// Only the codec library and device I/O are modeled. Production configure,
// PTT, buffer/queue management, packet assembly and playback run unchanged.
Codec2FrameCodec::Codec2FrameCodec() = default;
Codec2FrameCodec::~Codec2FrameCodec() = default;
bool Codec2FrameCodec::open(uint8_t mode) {
    mode_ = mode; bytes_ = AudioWire::frameBytes(mode); samples_ = bytes_;
    codec_ = bytes_ ? this : nullptr;
    return ready();
}
void Codec2FrameCodec::close() { codec_=nullptr; }
bool Codec2FrameCodec::ready() const { return codec_ != nullptr; }
size_t Codec2FrameCodec::samplesPerFrame() const { return samples_; }
size_t Codec2FrameCodec::bytesPerFrame() const { return bytes_; }
uint8_t Codec2FrameCodec::wireMode() const { return mode_; }
bool Codec2FrameCodec::modeAvailable(uint8_t mode) { return AudioWire::frameBytes(mode) != 0; }
bool Codec2FrameCodec::encode(const int16_t *samples,uint8_t *out,size_t cap,size_t &written) {
    assert(ready() && cap >= bytes_); std::copy_n(samples, bytes_, out); written = bytes_; return true;
}
bool Codec2FrameCodec::decode(const uint8_t *bytes,size_t size,int16_t *out,size_t cap,size_t &written) {
    assert(ready() && size == bytes_ && cap >= size); std::copy_n(bytes,size,out); written = size; return true;
}
ZephyrI2sAudio::ZephyrI2sAudio() = default;
ZephyrI2sAudio::~ZephyrI2sAudio() = default;
bool ZephyrI2sAudio::configure(const AudioConfig &,size_t) { return true; }
I2sCapabilities ZephyrI2sAudio::availableCapabilities(const AudioConfig &) { return {true,true,false}; }
I2sCapabilities ZephyrI2sAudio::capabilities() const { return {true,true,true}; }
bool ZephyrI2sAudio::capture(int16_t *,size_t,uint32_t) { return false; }
bool ZephyrI2sAudio::playback(const int16_t *samples,size_t count,uint32_t) {
    played.emplace_back(samples,samples+count); return true;
}
bool ZephyrI2sAudio::pttPressed() const { return donor::pressed; }
void ZephyrI2sAudio::stop() {}
}

static void audioCases() {
    using namespace mt::audio;
    constexpr unsigned modes[] = {0,1,2,3,4,5,8,9};
    constexpr unsigned lengths[] = {8,6,8,7,7,6,4,3};
    unsigned comparisons = 0;
    for (size_t m=0; m < sizeof(modes)/sizeof(modes[0]); ++m) {
        const auto mode = uint8_t(modes[m]); const size_t frameSize = lengths[m];
        ZephyrI2sAudio device; AudioModule module;
        AudioConfig cfg; cfg.enabled = true; cfg.bitrate = decltype(cfg.bitrate)(mode+1);
        assert(module.configure(cfg,device));
        donor::AudioWorker worker;
        uint8_t header[] = {0xc0,0xde,0xc2,mode};
        std::copy_n(header,4,worker.tx_header);
        worker.encode_codec_size = donor::frameBytes = int(frameSize);
        assert(AudioWire::packetSize(frameSize) == donor::packetSize(int(frameSize)));
        for (size_t length=0; length<=233; ++length) {
            Bytes payload(233);
            std::copy_n(header,4,payload.begin());
            for (size_t i=4; i<payload.size(); ++i) payload[i]=uint8_t(i*3);
            std::copy_n(payload.data(),length,worker.rx_encode_frame);
            worker.rx_encode_frame_index=int(length);
            donor::decoded.clear(); played.clear(); donor::receive(&worker);
            bool ok=module.receive(4,1,meshtastic_PortNum_AUDIO_APP,payload.data(),length);
            const bool complete=length>4 && (length-4)%frameSize==0;
            assert(ok==complete);
            module.poll(0);
            if (complete) assert(played==donor::decoded);
            else assert(played.empty()); // ZT admits only complete, nonempty packets.
            ++comparisons;
        }
        uint8_t bad[234]{}; std::copy_n(header,4,bad);
        assert(!module.receive(4,1,meshtastic_PortNum_AUDIO_APP,bad,sizeof(bad)));
        bad[0]=0; assert(!module.receive(4,1,meshtastic_PortNum_AUDIO_APP,bad,4+frameSize));
        donor::AudioModule stock;
        donor::pressed=true;stock.poll();module.poll(2);
        assert(stock.radio_state==donor::RadioState::tx);
        std::array<int16_t,8> pcm{}; for (size_t i=0;i<frameSize;++i) pcm[i]=int16_t(i+23);
        const size_t full=donor::packetSize(int(frameSize));
        const size_t frames=(full-4)/frameSize;
        for (size_t i=0; i<frames+1; ++i) assert(module.pushCaptureSamples(pcm.data(),frameSize));
        stock.tx_header[3]=mode;
        std::copy_n(header,4,stock.tx_encode_frame);
        std::copy_n(pcm.begin(),frameSize,stock.tx_encode_frame+4);
        stock.tx_encode_frame_index=4+frameSize;
        donor::pressed=false;stock.poll();module.poll(3);
        assert(stock.radio_state==donor::RadioState::rx && !stock.packet.want_ack && !stock.packet.decoded.want_response);
        assert(stock.packet.to==UINT32_MAX && stock.packet.priority==meshtastic_MeshPacket_Priority_MAX);
        for (const size_t expected : {full,4+frameSize}) {
            uint8_t bytes[233]{}; size_t length=0; uint32_t port=0;
            assert(!module.sendPayload(bytes,sizeof(bytes),[](uint32_t,const uint8_t *,size_t){return false;}));
            assert(module.sendPayload(bytes,sizeof(bytes),[&](uint32_t p,const uint8_t *,size_t n){port=p;length=n;return true;}));
            if(expected!=full)assert(stock.packet.decoded.payload.size==length && !memcmp(stock.packet.decoded.payload.bytes,bytes,length));
            assert(port==meshtastic_PortNum_AUDIO_APP && length==expected && std::equal(header,header+4,bytes));
            for (size_t i=4;i<length;++i) assert(bytes[i]==pcm[(i-4)%frameSize]);
        }
    }
    assert(AudioModule::regionPermits(1) && !AudioModule::regionPermits(3));
    assert(!AudioModule::regionPermits(0) && !AudioModule::regionPermits(UINT32_MAX));
    printf("%u audio RX length/mode comparisons; eight stock PTT transitions, full and partial sends and retained backpressured frames\n",comparisons);
}

static void paxCases() {
    donor::services.calls=0;
    using Pax = mt::modules::peripherals::PaxcounterCore;
    donor::PaxcounterModule stock; donor::paxcounterModule=&stock;
    Pax candidate; candidate.configure({true,1,0,0},0);
    assert(candidate.config().bleThreshold==-80 && candidate.config().wifiThreshold==-80);
    const uint8_t addr[6]={2,1,2,3,4,5};
    assert(candidate.observe(Pax::Kind::Wifi,addr,-81,100)==Pax::ObserveResult::WeakSignal);
    assert(candidate.observe(Pax::Kind::Wifi,addr,-80,100)==Pax::ObserveResult::Recorded);
    assert(candidate.observe(Pax::Kind::Wifi,addr,-80,101)==Pax::ObserveResult::Duplicate);
    assert(candidate.observe(Pax::Kind::Ble,addr,-80,100)==Pax::ObserveResult::Recorded);
    stock.count_from_libpax={1,1};stock.reportedDataSent=false;
    donor::clockMs=100;assert(stock.allocReply());
    meshtastic_Paxcount reply{};assert(candidate.reply(100,0,reply));
    assert(reply.wifi==stock.payload.wifi && reply.ble==stock.payload.ble && reply.uptime==stock.payload.uptime);
    for (const uint32_t now : {1000u,2000u,3000u,4000u}) {
        if (now==4000) {
            const uint8_t another[6]={2,1,2,3,4,6};
            assert(candidate.observe(Pax::Kind::Ble,another,-20,3999)==Pax::ObserveResult::Recorded);
            stock.count_from_libpax.ble_count=2;stock.reportedDataSent=false;
        }
        donor::clockMs=now;stock.allocate=now!=1000;
        const bool sent=stock.sendInfo(UINT32_MAX);
        bool called=false;
        auto r=candidate.sendInfo(now,now/1000,[&](const meshtastic_Paxcount &pl){
            called=true;
            assert(pl.wifi==stock.payload.wifi && pl.ble==stock.payload.ble && pl.uptime==stock.payload.uptime);
            return stock.allocate;
        });
        assert((r==Pax::PollResult::Ready)==sent);
        assert(called==(now!=3000));
        if(now==1000) assert(r==Pax::PollResult::NotSubmitted);
        if(now==3000) assert(r==Pax::PollResult::NoFreshData);
    }
    assert(donor::services.calls==2);
    candidate.configure({false,1,0,0},5000);
    assert(candidate.sendInfo(9000,9,[](auto &){assert(false);return false;})==Pax::PollResult::Disabled);
    assert(!candidate.reply(100000,100,reply)); // All observations expired.
    candidate.configure({false,1,0,0},100001);assert(!candidate.reply(100001,100,reply));
    puts("Paxcounter solicited allocation, freshness, disabled and stock scheduled-send refusal/retry matched");
}

int main() { audioCases(); paxCases(); }
