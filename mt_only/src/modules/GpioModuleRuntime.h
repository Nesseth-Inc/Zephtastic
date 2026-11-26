// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <meshtastic/remote_hardware.pb.h>
#include <meshtastic/portnums.pb.h>
#include <pb_encode.h>
#include <stdint.h>
#include <string.h>

#include "GpioModuleBackend.h"
#include "GpioModuleSettings.h"
#include "RemoteHardwarePolicy.h"
#include "../stock/modules/RemoteHardwareModule.h"
#include "DetectionSensorPolicy.h"

namespace mt::modules {

// Bounded policy/backend bridge. The caller owns packet parsing, full Data
// validation, exact channel-name resolution, and radio transmission. This
// runtime does not authenticate peers and never derives board pin IDs itself.
class GpioModuleRuntime {
public:
  static constexpr size_t PayloadCapacity = 233;
  static constexpr size_t DuplicateCapacity = 8;
  static constexpr uint32_t RemoteSampleIntervalMs = 100;
  static constexpr uint32_t Broadcast = UINT32_MAX;

  struct RequestContext {
    uint32_t from = 0;
    uint32_t packetId = 0;
    uint8_t channel = 0;
    uint8_t replyHopLimit = 0;
    bool gpioChannel = false;  // caller resolved exact named binding
    bool wantAck = false;
  };
  struct Outbound {
    uint32_t to = Broadcast;
    uint32_t requestId = 0;
    uint8_t portnum = 0;
    uint8_t channel = 0;
    uint8_t hopLimit = 0;
    bool wantAck = false;
    uint8_t payload[PayloadCapacity]{};
    size_t payloadSize = 0;
  };
  using Send = bool (*)(void *context, const Outbound &packet);
  enum class HandleStatus : uint8_t {
    Applied, Queued, Duplicate, Disabled, Unavailable, WrongChannel,
    InvalidContext, InvalidCommand, Denied, Busy, BackendFailure, NoReply
  };

  explicit GpioModuleRuntime(GpioModuleBackend &backend) : backend_(backend) {}

  // gpioBindingValid means at least one exact named-channel binding exists.
  // gpioChannelMask includes every configured channel slot named `gpio`;
  // neither value is a peer-trust or authorization signal.
  bool configure(const GpioModuleSettings &settings, bool ready,
                 bool gpioBindingValid, uint8_t gpioChannelMask,
                 uint32_t channelContextEpoch = 0) {
    if (!settings.valid()) return false;
    const GpioCapabilities caps = backend_.capabilities();
    const bool changed = !configured_ || !settings_.same(settings) || baseReady_ != ready ||
      gpioBindingValid_ != gpioBindingValid || gpioChannelMask_ != gpioChannelMask ||
      channelContextEpoch_ != channelContextEpoch ||
      !sameCapabilities(caps_, caps);
    if (!changed) return true;
    settings_ = settings;
    configured_ = true;
    baseReady_ = ready;
    gpioBindingValid_ = gpioBindingValid;
    gpioChannelMask_ = gpioChannelMask;
    channelContextEpoch_ = channelContextEpoch;
    caps_ = caps;
    ready_ = baseReady_ && caps_.exported != 0 && validateConfig(settings_);
    clearTransient();
    configurePolicies();
    return true;
  }

  // Safe to call before saving an Admin candidate. This method is read-only:
  // it does not reconfigure a backend or disturb the active runtime epoch.
  bool validateConfig(const GpioModuleSettings &settings) const {
    if (!settings.valid()) return false;
    const auto &r = settings.remote();
    const auto &d = settings.detection();
    if (d.minimum_broadcast_secs > UINT32_MAX/1000u ||
        d.state_broadcast_secs > UINT32_MAX/1000u) return false;
    const GpioCapabilities caps = backend_.capabilities();
    if ((caps.readable & ~caps.exported) || (caps.writable & ~caps.exported) ||
        (caps.pullup & ~caps.readable)) return false;
    if (r.enabled) {
      if (!caps.exported || r.available_pins_count > 4) return false;
      for (pb_size_t i = 0; i < r.available_pins_count; ++i) {
        const auto &pin = r.available_pins[i];
        if (pin.gpio_pin >= 64 || !(caps.exported & (UINT64_C(1)<<pin.gpio_pin))) return false;
      }
    }
    if (d.enabled) {
      if (d.monitor_pin == 0 || d.monitor_pin >= 64) return false;
      const uint64_t bit=UINT64_C(1)<<d.monitor_pin;
      if (!(caps.exported & caps.readable & bit) ||
          (d.use_pullup && !(caps.pullup & bit))) return false;
    }
    if (r.enabled && d.enabled && !r.allow_undefined_pin_access) {
      const pb_size_t count=r.available_pins_count > 4 ? 4 : r.available_pins_count;
      for(pb_size_t i=0;i<count;++i)
        if(r.available_pins[i].gpio_pin==d.monitor_pin) return false;
    }
    return true;
  }

  // Input must already be a completely decoded/validated HardwareMessage from
  // a caller-verified REMOTE_HARDWARE_APP Data envelope. Shape/source/dest
  // verification is deliberately outside this backend-neutral layer.
  HandleStatus handle(const meshtastic_HardwareMessage &message,
                      const RequestContext &ctx, Send send, void *sendContext) {
    refreshCapabilities();
    flush(send, sendContext);
    if (!ready_ || !caps_.exported) return HandleStatus::Unavailable;
    if (!settings_.remote().enabled) return HandleStatus::Disabled;
    if (!gpioBindingValid_ || !ctx.gpioChannel || ctx.channel >= 8 ||
        !(gpioChannelMask_ & (uint8_t(1) << ctx.channel)))
      return HandleStatus::WrongChannel;
    if (!ctx.from || !ctx.packetId) return HandleStatus::InvalidContext;
    if (isDuplicate(ctx)) return HandleStatus::Duplicate;
    if (pending_.valid && enumValue(message.type) ==
        uint32_t(meshtastic_HardwareMessage_Type_READ_GPIOS)) return HandleStatus::Busy;

    const auto &cfg = settings_.remote();
    RemoteHardwarePolicy::PinDomain domain = remoteDomain(cfg);
    const auto decision = remotePolicy_.evaluate(message, true,
        cfg.allow_undefined_pin_access, domain);
    if (!decision.accepted()) {
      switch (decision.status) {
        case RemoteHardwarePolicy::Status::Disabled: return HandleStatus::Disabled;
        case RemoteHardwarePolicy::Status::WrongChannel: return HandleStatus::WrongChannel;
        case RemoteHardwarePolicy::Status::InvalidCommand: return HandleStatus::InvalidCommand;
        default: return HandleStatus::Denied;
      }
    }
    if (decision.operation == RemoteHardwarePolicy::Operation::Ignore)
      return HandleStatus::NoReply;
    remember(ctx);  // accepted commands have at most one backend effect

    auto write = [&](uint64_t mask, uint64_t value) {
      return backend_.write(mask, value) < 0 ? HandleStatus::BackendFailure : HandleStatus::Applied;
    };
    auto read = [&](uint64_t mask, uint64_t &value) {
      return prepareInputs(mask) && backend_.read(mask, true, value) >= 0;
    };
    auto watch = [&](uint64_t mask) {
      if (!mask || prepareInputs(mask)) return HandleStatus::Applied;
      clearWatch();
      return HandleStatus::BackendFailure;
    };
    auto reply = [&](const meshtastic_HardwareMessage &response) {
      Outbound out{};
      out.to = ctx.from; out.requestId = ctx.packetId;
      out.portnum = meshtastic_PortNum_REMOTE_HARDWARE_APP;
      out.channel = ctx.channel; out.hopLimit = ctx.replyHopLimit;
      out.wantAck = ctx.wantAck;
      if (!encodeHardware(response, out.payload, sizeof(out.payload), out.payloadSize))
        return HandleStatus::BackendFailure;
      pending_.packet = out; pending_.valid = true;
      return flush(send, sendContext) ? HandleStatus::Applied : HandleStatus::Queued;
    };
    return stock::RemoteHardwareModule::handleReceivedProtobuf(message, write, read, watch,
        reply, HandleStatus::BackendFailure, HandleStatus::NoReply);
  }

  // Call at the application cadence. GPIO reads are independently rate-limited
  // to 100 ms, so a 5 ms main loop cannot turn into a busy polling loop.
  void poll(uint32_t nowMs, bool stockDefaultPrimary, uint8_t primaryHopLimit,
            Send send, void *sendContext) {
    refreshCapabilities();
    flush(send, sendContext);
    if (!ready_ || !caps_.exported) return;

    if (remotePolicy_.watchMask() && !pending_.valid &&
        (!remoteSampled_ || elapsed(nowMs, lastRemoteSampleMs_) >= RemoteSampleIntervalMs)) {
      const uint64_t mask = remotePolicy_.watchMask() & remoteReadMask();
      if (!mask) {
        clearWatch();
      } else {
        uint64_t sample = 0;
        if (backend_.read(mask, true, sample) < 0) {
          clearWatch();
        } else {
          remoteSampled_ = true; lastRemoteSampleMs_ = nowMs;
          RemoteHardwarePolicy::WatchEvent event{};
          if (remotePolicy_.pollWatchValue(sample, nowMs, event)) {
            Outbound out{};
            out.to = Broadcast;
            out.portnum = meshtastic_PortNum_REMOTE_HARDWARE_APP;
            out.channel = 0; out.hopLimit = primaryHopLimit;
            if (encodeHardware(event.message, out.payload, sizeof(out.payload), out.payloadSize)) {
              pending_.packet = out; pending_.valid = true;
              flush(send, sendContext);
            }
          }
        }
      }
    }

    auto &policy = detectionPolicy_;
    if (!policy.enabledAndReady()) return;
    bool raw = false;
    // The first policy call requests its input mode. Configure once, then wait
    // for the next 100 ms boundary before performing any read.
    if (!detectorInputConfigured_) {
      const auto result = policy.poll(nowMs, false, stockDefaultPrimary,
          [this, send, sendContext, primaryHopLimit](const detection::DetectionSensorPolicy::Event &e) {
            return queueDetection(e, primaryHopLimit, send, sendContext);
          });
      if (result.requestInputMode) {
        if (backend_.configureInput(settings_.detection().monitor_pin,
                                    settings_.detection().use_pullup) < 0) {
          detectorFailed_ = true;
          return;
        }
        detectorInputConfigured_ = true;
        detectorLastReadMs_ = nowMs;
        detectorHaveRead_ = true;
      }
      return;
    }
    if (detectorFailed_ || (detectorHaveRead_ && elapsed(nowMs, detectorLastReadMs_) <
        detection::DetectionSensorPolicy::SampleIntervalMs)) return;
    const uint64_t bit = UINT64_C(1) << settings_.detection().monitor_pin;
    uint64_t value = 0;
    if (backend_.read(bit, settings_.detection().use_pullup, value) < 0) {
      detectorFailed_ = true;
      return;
    }
    raw = (value & bit) != 0;
    detectorLastReadMs_ = nowMs; detectorHaveRead_ = true;
    policy.poll(nowMs, raw, stockDefaultPrimary,
        [this, send, sendContext, primaryHopLimit](const detection::DetectionSensorPolicy::Event &e) {
          return queueDetection(e, primaryHopLimit, send, sendContext);
        });
  }

  bool available() const { return ready_ && caps_.exported != 0; }
  bool pendingOutput() const { return pending_.valid; }

private:
  struct Duplicate { uint32_t from=0, id=0; uint8_t channel=0; bool valid=false; };
  struct Pending { Outbound packet{}; bool valid=false; };
  GpioModuleBackend &backend_;
  GpioModuleSettings settings_{};
  GpioCapabilities caps_{};
  RemoteHardwarePolicy remotePolicy_{};
  detection::DetectionSensorPolicy detectionPolicy_{};
  Duplicate duplicates_[DuplicateCapacity]{};
  Pending pending_{};
  size_t nextDuplicate_=0;
  bool configured_=false, ready_=false, baseReady_=false, gpioBindingValid_=false;
  uint8_t gpioChannelMask_=0;
  uint32_t channelContextEpoch_=0;
  bool remoteSampled_=false, detectorInputConfigured_=false, detectorFailed_=false;
  bool detectorHaveRead_=false;
  uint32_t lastRemoteSampleMs_=0, detectorLastReadMs_=0;

  static uint32_t elapsed(uint32_t now, uint32_t then) { return uint32_t(now-then); }
  template<class E> static uint32_t enumValue(const E &value) {
    using U = __underlying_type(E); U raw{}; memcpy(&raw,&value,sizeof(raw));
    return static_cast<uint32_t>(raw);
  }
  static bool sameCapabilities(const GpioCapabilities &a,const GpioCapabilities &b) {
    return a.exported==b.exported && a.readable==b.readable &&
      a.writable==b.writable && a.pullup==b.pullup;
  }
  void clearTransient() {
    memset(duplicates_,0,sizeof(duplicates_)); nextDuplicate_=0; pending_=Pending{};
    remoteSampled_=false; detectorInputConfigured_=false; detectorFailed_=false;
    detectorHaveRead_=false; lastRemoteSampleMs_=detectorLastReadMs_=0;
    remotePolicy_.clearWatch();
    detectionPolicy_=detection::DetectionSensorPolicy{};
  }
  void configurePolicies() {
    const auto &r=settings_.remote();
    remotePolicy_.setEnabled(ready_ && gpioBindingValid_ && r.enabled);
    const auto &d=settings_.detection();
    detection::DetectionSensorPolicy::Config config{};
    config.enabled=d.enabled; config.monitorPin=d.monitor_pin;
    config.usePullup=d.use_pullup; config.minimumBroadcastSecs=d.minimum_broadcast_secs;
    config.stateBroadcastSecs=d.state_broadcast_secs; config.sendBell=d.send_bell;
    config.trigger=enumValue(d.detection_trigger_type);
    memcpy(config.name,d.name,sizeof(config.name));
    const bool pinResolved = ready_ && d.monitor_pin > 0 && d.monitor_pin < 64 &&
      (caps_.exported & caps_.readable & (UINT64_C(1)<<d.monitor_pin)) &&
      (!d.use_pullup || (caps_.pullup & (UINT64_C(1)<<d.monitor_pin)));
    detectionPolicy_.configure(config,pinResolved);
  }
  void refreshCapabilities() {
    if (!configured_) return;
    const auto current=backend_.capabilities();
    if (!sameCapabilities(current,caps_)) {
      const auto old=settings_; const bool oldBaseReady=baseReady_;
      const bool oldBinding=gpioBindingValid_; const auto oldMask=gpioChannelMask_;
      settings_=old; baseReady_=oldBaseReady; gpioBindingValid_=oldBinding;
      gpioChannelMask_=oldMask; caps_=current;
      ready_=baseReady_ && caps_.exported!=0 && validateConfig(settings_);
      clearTransient(); configurePolicies();
    }
  }
  RemoteHardwarePolicy::PinDomain remoteDomain(
      const meshtastic_ModuleConfig_RemoteHardwareConfig &cfg) const {
    uint64_t configured=0;
    if (!cfg.allow_undefined_pin_access) {
      const pb_size_t count=cfg.available_pins_count > 4 ? 4 : cfg.available_pins_count;
      for (pb_size_t i=0;i<count;++i) {
        const auto &pin=cfg.available_pins[i];
        if (pin.gpio_pin >= 64) continue;
        // Stock exposes every explicitly listed pin. Electrical read/write
        // permissions still come only from the reviewed backend capability map.
        configured |= UINT64_C(1)<<pin.gpio_pin;
      }
    } else configured=caps_.exported;
    // A detector owns its monitor pin while enabled; do not let remote
    // commands reconfigure or drive the same physical input.
    if (settings_.detection().enabled && settings_.detection().monitor_pin < 64)
      configured &= ~(UINT64_C(1)<<settings_.detection().monitor_pin);
    const uint64_t exported=caps_.exported;
    uint64_t writable = caps_.writable & exported;
    if (settings_.detection().enabled && settings_.detection().monitor_pin < 64)
      writable &= ~(UINT64_C(1)<<settings_.detection().monitor_pin);
    return {exported, remoteReadMask(), writable, configured & exported};
  }
  uint64_t remoteReadMask() const {
    uint64_t mask=caps_.readable & caps_.pullup & caps_.exported;
    if (settings_.detection().enabled && settings_.detection().monitor_pin < 64)
      mask &= ~(UINT64_C(1)<<settings_.detection().monitor_pin);
    return mask;
  }
  bool prepareInputs(uint64_t mask) {
    if (!mask || (mask & ~remoteReadMask())) return false;
    for (unsigned pin=0;pin<64;++pin) if (mask & (UINT64_C(1)<<pin))
      if (backend_.configureInput(pin,true)<0) return false;
    return true;
  }
  void clearWatch() {
    meshtastic_HardwareMessage clear=meshtastic_HardwareMessage_init_default;
    clear.type=meshtastic_HardwareMessage_Type_WATCH_GPIOS;
    RemoteHardwarePolicy::PinDomain domain=remoteDomain(settings_.remote());
    remotePolicy_.evaluate(clear,true,settings_.remote().allow_undefined_pin_access,domain);
  }
  bool isDuplicate(const RequestContext &ctx) const {
    for (const auto &entry:duplicates_) if(entry.valid&&entry.from==ctx.from&&
        entry.id==ctx.packetId&&entry.channel==ctx.channel) return true;
    return false;
  }
  void remember(const RequestContext &ctx) {
    duplicates_[nextDuplicate_]={ctx.from,ctx.packetId,ctx.channel,true};
    nextDuplicate_=(nextDuplicate_+1)%DuplicateCapacity;
  }
  static bool encodeHardware(const meshtastic_HardwareMessage &message,
                             uint8_t *out,size_t capacity,size_t &size) {
    auto stream=pb_ostream_from_buffer(out,capacity);
    if(!pb_encode(&stream,meshtastic_HardwareMessage_fields,&message)) return false;
    size=stream.bytes_written;return size<=capacity;
  }
  bool flush(Send send,void *ctx) {
    if(!pending_.valid||!send) return false;
    if(!send(ctx,pending_.packet)) return false;
    pending_=Pending{};return true;
  }
  bool queueDetection(const detection::DetectionSensorPolicy::Event &event,
                      uint8_t hop,Send send,void *ctx) {
    if(pending_.valid || !send) return false;
    Outbound out{};out.to=Broadcast;out.portnum=meshtastic_PortNum_DETECTION_SENSOR_APP;
    out.channel=0;out.hopLimit=hop;out.wantAck=event.wantAck;
    if(event.payloadSize>sizeof(out.payload)) return false;
    memcpy(out.payload,event.payload,event.payloadSize);out.payloadSize=event.payloadSize;
    // Detection policy owns its bounded pending/deferred actions. Sending
    // directly avoids retaining a second copy when the policy retries.
    return send(ctx,out);
  }
};

} // namespace mt::modules
