// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "MqttPublisherTransport.h"
#include "MqttDownlinkMailbox.h"
#include "meshtastic/mesh.pb.h"
#include <stddef.h>
#include <stdint.h>

namespace mt::mqtt {

struct RuntimeStatus {
  State state = State::Stopped;
  size_t queued = 0;
  int lastError = 0;
  int refusedIncomingPublish = 0;
  size_t downlinkQueued = 0;
};

// Starts only the direct WiFi/PSRAM backend. Proxy ownership is handled by
// the cross-profile Service/Companion bridge; direct init also requires the
// boot snapshot's stock `wantsLink` demand. Bounded config strings are copied
// before the caller returns. Enqueue calls are producer-side and never do
// socket work; call them from one producer (normally the Service owner).
#if defined(CONFIG_MT_MQTT_GATEWAY)
bool init(const Settings &settings, bool wifiReady);
bool tlsTrustReady(int securityTag);
// Reuse the existing WiFi SNTP sample to establish certificate wall time.
// TLS connection attempts wait until a system realtime update has succeeded.
bool syncCertificateTime(uint32_t seconds);
bool certificateTimeReady();
void setWifiReady(bool ready);
PublishResult enqueueEnvelope(const char *topic, const uint8_t *payload, size_t size);
PublishResult enqueuePacket(const char *topic, const meshtastic_MeshPacket *packet,
                            const char *channelId, size_t channelIdLength,
                            const char *gatewayId, size_t gatewayIdLength);
RuntimeStatus status();
void shutdown();
bool takeDownlink(DownlinkMessage &message);
#else
inline bool init(const Settings &, bool) { return false; }
inline bool tlsTrustReady(int) { return false; }
inline bool syncCertificateTime(uint32_t) { return false; }
inline bool certificateTimeReady() { return false; }
inline void setWifiReady(bool) {}
inline PublishResult enqueueEnvelope(const char *, const uint8_t *, size_t) {
  return PublishResult::Disabled;
}
inline PublishResult enqueuePacket(const char *, const meshtastic_MeshPacket *, const char *, size_t,
                                   const char *, size_t) {
  return PublishResult::Disabled;
}
inline RuntimeStatus status() { return {}; }
inline void shutdown() {}
inline bool takeDownlink(DownlinkMessage &) { return false; }
#endif

}  // namespace mt::mqtt
