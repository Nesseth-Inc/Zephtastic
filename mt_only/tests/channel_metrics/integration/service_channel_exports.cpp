// SPDX-License-Identifier: GPL-3.0-only
// Actual Service/Companion/codec/nanopb; fixture clock and radio metadata only.
// No native callback, USB/BLE connection, hardware, or RF qualification.
#include "endpoint_fixture.h"
#include <cmath>
#include <limits>

#ifdef NDEBUG
#error Tests require assertions.
#endif

struct MetricClock {
  uint64_t now=0;
  static uint64_t read(void *ctx){return static_cast<MetricClock *>(ctx)->now;}
  mt::UptimeClock provider(){return {read,this};}
};
enum class Case {Startup,UnknownOnly,UnknownRetained,LossRetained,SupportPause,AgedZero,
                 Unbound,InvalidClock,Backwards,Overflow,Range};
static const char *label(Case mode) {
  switch(mode) {
    case Case::Startup:return "startup-zero";
    case Case::UnknownOnly:return "unknown-only-zero";
    case Case::UnknownRetained:return "unknown-retains-known";
    case Case::LossRetained:return "loss-retains-known";
    case Case::SupportPause:return "support-pause-retains-known";
    case Case::AgedZero:return "aged-zero";
    case Case::Unbound:return "unbound-absent";
    case Case::InvalidClock:return "invalid-clock-absent";
    case Case::Backwards:return "backwards-clock-absent";
    case Case::Overflow:return "overflow-absent";
    case Case::Range:return "out-of-range-export-absent";
  }
  assert(false);return "invalid";
}
static void check(const meshtastic_Telemetry &telemetry,bool available,uint32_t knownMs) {
  assert(telemetry.which_variant==meshtastic_Telemetry_device_metrics_tag&&!telemetry.time);
  const auto &m=telemetry.variant.device_metrics;
  assert(m.has_uptime_seconds&&m.uptime_seconds==123);
  assert(m.has_air_util_tx&&m.air_util_tx==float(3600)/float(3600000)*100);
  assert(!m.has_battery_level&&!m.has_voltage&&m.has_channel_utilization==available);
  if(available)assert(m.channel_utilization==float(knownMs)/float(60000)*100);
}
static meshtastic_Telemetry telemetry(const uint8_t *bytes,size_t size) {
  meshtastic_Telemetry out{};auto in=pb_istream_from_buffer(bytes,size);
  assert(pb_decode(&in,meshtastic_Telemetry_fields,&out)&&!in.bytes_left);return out;
}
int main() {
  std::cout.setf(std::ios::unitbuf);
  for(uint8_t model:{81,127})for(bool secondOwner:{false,true})
    for(auto mode:{Case::Startup,Case::UnknownOnly,Case::UnknownRetained,Case::LossRetained,
                  Case::SupportPause,Case::AgedZero,Case::Unbound,Case::InvalidClock,
                  Case::Backwards,Case::Overflow,Case::Range}) {
      MetricClock channelClock,airClock;auto f=std::make_unique<Fixture>(true,model,false,false);
      auto &meter=f->s.channelMeter();
      if(mode!=Case::Unbound){assert(meter.bind(channelClock.provider()));meter.supported(true);}
      assert(f->s.airtimeMeter().bind(airClock.provider()));
      f->s.airtimeMeter().noteAcceptedStart(3600);f->s.airtimeMeter().creditPending();
      bool available=true;uint32_t known=0;
      switch(mode) {
        case Case::UnknownRetained:case Case::LossRetained:case Case::SupportPause:
          meter.observeRx(600,true);known=600;break;
        default:break;
      }
      channelClock.now=1000;
      switch(mode) {
        case Case::UnknownOnly:case Case::UnknownRetained:meter.observeRx(500,false);break;
        case Case::LossRetained:meter.uncertain(zt::ChannelEvent::NativeLoss);break;
        case Case::SupportPause:meter.supported(true);meter.supported(false);break;
        case Case::AgedZero:meter.observeRx(600,true);channelClock.now=70000;break;
        case Case::Unbound:available=false;break;
        case Case::InvalidClock:channelClock.now=UINT64_MAX;available=false;break;
        case Case::Backwards:meter.observeRx(600,true);channelClock.now=999;available=false;break;
        case Case::Overflow:meter.observeRx(UINT32_MAX,true);meter.observeRx(1,true);available=false;break;
        case Case::Range:meter.observeRx(60001,true);available=false;break;
        default:break;
      }
      const auto before=f->s.config;auto &host=secondOwner?f->bh:f->h;
      host.uptimeValid=true;host.uptime=123;f->s.measuredUptime(123);
      meshtastic_ToRadio request{};request.which_payload_variant=meshtastic_ToRadio_packet_tag;
      request.packet.id=17;request.packet.to=OURS;request.packet.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
      request.packet.decoded.portnum=meshtastic_PortNum_TELEMETRY_APP;request.packet.decoded.want_response=true;
      input(secondOwner?f->bc:f->c,request,100);
      unsigned replies=0;std::vector<uint8_t> bytes;
      for(const auto &row:host.out)if(row.which_payload_variant==meshtastic_FromRadio_packet_tag&&
          row.packet.decoded.portnum==meshtastic_PortNum_TELEMETRY_APP&&row.packet.decoded.request_id==17) {
        ++replies;bytes.assign(row.packet.decoded.payload.bytes,row.packet.decoded.payload.bytes+row.packet.decoded.payload.size);
      }
      assert(replies==1&&!results(host,17)&&!f->radio.sends);
      check(telemetry(bytes.data(),bytes.size()),available,known);
      assert(f->s.config.same(before));
      // Canonical empty device-metric selector enters the real radio responder.
      uint8_t frame[255];size_t frameSize=0;
      assert(f->s.codec.encodeTelemetryQuery(PEER,OURS,100,frame,sizeof(frame),frameSize));
      f->s.rx(frame,frameSize,3,200,700);f->s.poll(600,0);
      if(available&&mode!=Case::SupportPause) {
        assert(f->radio.sends==1&&f->radio.frames.size()==1);
        const auto &reply=f->radio.frames.front();bench::RouteMetadata decoded;
        assert(f->s.codec.decode(reply.data(),reply.size(),decoded));
        assert(decoded.portnum==meshtastic_PortNum_TELEMETRY_APP&&decoded.request_id==100&&!decoded.wantResponse);
        assert(bench::le32(reply.data())==PEER&&bench::le32(reply.data()+4)==OURS&&!(reply[12]&8));
        check(telemetry(decoded.payload,decoded.payloadLength),available,known);
      } else {
        // Remote telemetry is only emitted over RF when the production
        // normal-admission channel meter is available and in range.
        assert(f->radio.sends==0&&f->radio.frames.empty());
      }
      assert(f->s.config.same(before));
      group((std::string("actual local protobuf and remote admission ")+label(mode)+" model="+std::to_string(model)+
             " owner="+std::to_string(secondOwner)).c_str());
    }
}
