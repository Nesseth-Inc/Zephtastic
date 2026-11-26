// SPDX-License-Identifier: GPL-3.0-only
#include "MqttRuntime.h"

#if defined(CONFIG_MT_MQTT_GATEWAY)
#include "MqttServiceEnvelopeCodec.h"
#include "MqttReconnectScheduler.h"
#include "ZephyrMqttPublisherDriver.h"
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include "MqttAtomic.h"
#include "MqttTlsClock.h"
#include <zephyr/sys/clock.h>
#if defined(CONFIG_MT_WIFI_STATION)
#include "ZephyrWiFiStation.h"
#endif
#include <errno.h>
#include <string.h>

namespace mt::mqtt {

static constexpr size_t kQueueCapacity = 8;
static constexpr size_t kTopicCapacity = 160;
static constexpr size_t kPayloadCapacity = ServiceEnvelopeCodec::kMaxEncodedEnvelopeBytes;
using RuntimeTransport = PublisherTransport<kQueueCapacity, kTopicCapacity, kPayloadCapacity>;
using RuntimeSlot = RuntimeTransport::QueueSlot;

BUILD_ASSERT(IS_ENABLED(CONFIG_ESP_SPIRAM), "MQTT publish queue requires PSRAM");
BUILD_ASSERT(DT_NODE_EXISTS(DT_NODELABEL(psram0)), "MQTT is supported only on the PSRAM WiFi board");
BUILD_ASSERT(DT_PROP(DT_NODELABEL(psram0), size) >= 8 * 1024 * 1024,
             "MQTT queue requires the qualified 8 MiB PSRAM profile");

// Queue payload/topic storage is external only in this explicitly selected
// WiFi profile. Socket buffers and control state remain in internal RAM.
static RuntimeSlot queueSlots[kQueueCapacity]
    __attribute__((section(".ext_ram.bss.mt_mqtt_tx")));
using RuntimeDownlinkMailbox=DownlinkMailbox<2>;
static RuntimeDownlinkMailbox::Slot downlinkSlots[2]
    __attribute__((section(".ext_ram.bss.mt_mqtt_rx")));
static RuntimeDownlinkMailbox downlinkMailbox(downlinkSlots);
static ZephyrMqttPublisherDriver driver;
static RuntimeTransport transport(driver, queueSlots);
K_KERNEL_STACK_DEFINE(workerStack, 4096);
static struct k_thread workerThread;
K_SEM_DEFINE(wakeWorker, 0, 1);
static Atomic<bool> started{false};
static Atomic<bool> stopping{false};
static Atomic<bool> networkReadySnapshot{false};
static CertificateClock certificateClock;
static MqttReconnectScheduler reconnectScheduler;
static bool tlsEnabledSnapshot=false;
static bool wantsLinkSnapshot=false;
static bool trustReadySnapshot=false;

static int setCertificateRealtime(uint32_t seconds) {
  struct timespec value{};
  value.tv_sec = seconds;
  if (static_cast<uint64_t>(value.tv_sec) != static_cast<uint64_t>(seconds))
    return -ERANGE;
  return sys_clock_settime(SYS_CLOCK_REALTIME, &value);
}

bool syncCertificateTime(uint32_t seconds) {
  const bool synced=certificateClock.sync(seconds, setCertificateRealtime);
  if(synced&&started.load(AtomicOrder::Acquire))k_sem_give(&wakeWorker);
  return synced;
}

bool certificateTimeReady() { return certificateClock.ready(); }

static void worker(void *, void *, void *) {
  bool previousWifiReady=networkReadySnapshot.load(AtomicOrder::Acquire);
  while (!stopping.load(AtomicOrder::Acquire)) {
    const uint32_t now=static_cast<uint32_t>(k_uptime_get_32());
    const bool wifiReady=networkReadySnapshot.load(AtomicOrder::Acquire);
    if(wifiReady!=previousWifiReady) {
      reconnectScheduler.networkChanged();
      previousWifiReady=wifiReady;
    }
    if (stopping.load(AtomicOrder::Acquire)) break;
    MqttReconnectScheduler::Inputs input{};
    input.nowMs=now;input.mqttEnabled=true;input.wantsLink=wantsLinkSnapshot;
    input.networkReady=wifiReady;input.brokerConnected=driver.connected();
    input.tlsEnabled=tlsEnabledSnapshot;input.tlsClockReady=certificateClock.ready();
    input.tlsTrustReady=trustReadySnapshot;
#if defined(CONFIG_MT_WIFI_STATION) && !defined(ARCH_PORTDUINO)
    input.link=MqttReconnectScheduler::Link::Wifi;
    input.stockWifiFailureCounter=true;
    input.wifiReconnectAvailable=true;
#endif
    const auto decision=reconnectScheduler.next(input);
    uint32_t waitMs=decision.waitMs;
    switch(decision.action) {
      case MqttReconnectScheduler::Action::AttemptBrokerConnect:
        if(transport.state()==State::Backoff)(void)transport.retry();
        else transport.service();
        waitMs=20;
        break;
      case MqttReconnectScheduler::Action::WaitForConnectResult:
      case MqttReconnectScheduler::Action::PollConnectedBroker:
      case MqttReconnectScheduler::Action::WaitForNetwork:
        transport.service();
        if(decision.action!=MqttReconnectScheduler::Action::WaitForNetwork)waitMs=20;
        break;
      case MqttReconnectScheduler::Action::RequestWifiReconnect:
#if defined(CONFIG_MT_WIFI_STATION) && !defined(ARCH_PORTDUINO)
        zc_wifi_station_reconnect();
#endif
        break;
      case MqttReconnectScheduler::Action::DisconnectUnwantedBroker:
        driver.disconnect();
        break;
      case MqttReconnectScheduler::Action::WaitForTlsClock:
      case MqttReconnectScheduler::Action::WaitForTlsTrust:
        // Clock readiness can arrive asynchronously through the existing SNTP
        // callback. A bounded fallback recheck handles a changed CA store.
        waitMs=1000;
        break;
      case MqttReconnectScheduler::Action::WaitForBrokerBackoff:
      case MqttReconnectScheduler::Action::NotWanted:
      case MqttReconnectScheduler::Action::ProxyOwnedElsewhere:
      case MqttReconnectScheduler::Action::Disabled:
        break;
    }
    const State after=transport.state();
    if(reconnectScheduler.attemptInFlight()) {
      if(after==State::Online)reconnectScheduler.connectSucceeded();
      else if(after==State::Backoff)reconnectScheduler.connectFailed(now);
      else if(after==State::WaitingForWifi)reconnectScheduler.connectAborted();
    }
    // New queued work, Wi-Fi state, and SNTP events wake this wait. The
    // policy still caps connected polling at stock's 20 ms cadence.
    if(!waitMs)waitMs=1;
    (void)k_sem_take(&wakeWorker,K_MSEC(waitMs));
  }
  transport.shutdown();
}

bool init(const Settings &settings, bool wifiReady) {
  // Proxy sessions are owned by the cross-profile Service/Companion bridge;
  // this Zephyr socket runtime only owns direct broker connections.
  if (started.load(AtomicOrder::Acquire) || !settings.enabled ||
      !settings.wantsLink || settings.proxyToClient) return false;
  downlinkMailbox.clear();driver.setDownlinkMailbox(&downlinkMailbox);
  if (!transport.start(settings, wifiReady)) return false;
  tlsEnabledSnapshot=settings.tls;
  trustReadySnapshot=!settings.tls||driver.tlsTrustAvailable(settings.trust);
  wantsLinkSnapshot=settings.wantsLink;
  networkReadySnapshot.store(wifiReady,AtomicOrder::Release);
  reconnectScheduler.reset();
  stopping.store(false, AtomicOrder::Release);
  started.store(true, AtomicOrder::Release);
  k_thread_create(&workerThread, workerStack, K_KERNEL_STACK_SIZEOF(workerStack),
                  worker, nullptr, nullptr, nullptr, K_PRIO_PREEMPT(8), 0, K_NO_WAIT);
  k_thread_name_set(&workerThread, "mt_mqtt_tx");
  return true;
}

void setWifiReady(bool ready) {
  if (!started.load(AtomicOrder::Acquire)) return;
  networkReadySnapshot.store(ready,AtomicOrder::Release);
  transport.setWifiReady(ready);
  k_sem_give(&wakeWorker);
}

bool tlsTrustReady(int securityTag) {
  return driver.tlsTrustAvailable(TlsTrust{true,securityTag});
}

PublishResult enqueueEnvelope(const char *topic, const uint8_t *payload, size_t size) {
  if (!started.load(AtomicOrder::Acquire) || stopping.load(AtomicOrder::Acquire))
    return PublishResult::Disabled;
  const PublishResult result = transport.enqueue(topic, payload, size);
  if (result == PublishResult::Accepted) k_sem_give(&wakeWorker);
  return result;
}

PublishResult enqueuePacket(const char *topic, const meshtastic_MeshPacket *packet,
                            const char *channelId, size_t channelIdLength,
                            const char *gatewayId, size_t gatewayIdLength) {
  if (!started.load(AtomicOrder::Acquire) || stopping.load(AtomicOrder::Acquire))
    return PublishResult::Disabled;
  uint8_t encoded[ServiceEnvelopeCodec::kMaxEncodedEnvelopeBytes];
  size_t encodedSize = 0;
  if (!ServiceEnvelopeCodec::encode(packet, channelId, channelIdLength,
                                   gatewayId, gatewayIdLength, encoded,
                                   sizeof(encoded), &encodedSize))
    return PublishResult::Invalid;
  return enqueueEnvelope(topic, encoded, encodedSize);
}

RuntimeStatus status() {
  RuntimeStatus result;
  result.state = transport.state();
  result.queued = transport.queued();
  result.lastError = driver.lastError();
  result.refusedIncomingPublish = driver.refusedIncomingPublish();
  result.downlinkQueued=downlinkMailbox.queued();
  return result;
}

bool takeDownlink(DownlinkMessage &message) {
  return started.load(AtomicOrder::Acquire)&&downlinkMailbox.pop(message);
}

void shutdown() {
  if (!started.load(AtomicOrder::Acquire)) return;
  stopping.store(true, AtomicOrder::Release);
  k_sem_give(&wakeWorker);
  (void)k_thread_join(&workerThread, K_FOREVER);
  started.store(false, AtomicOrder::Release);
  stopping.store(false, AtomicOrder::Release);
  reconnectScheduler.reset();
  networkReadySnapshot.store(false,AtomicOrder::Release);
}

}  // namespace mt::mqtt
#endif
