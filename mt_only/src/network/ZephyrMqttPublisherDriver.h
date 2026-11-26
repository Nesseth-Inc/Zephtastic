// SPDX-License-Identifier: MIT
// Zephyr MQTT/socket driver for MqttPublisherTransport. This adapter is
// intentionally inert until connect() is called by the owner after its
// explicit enabled/WiFi-ready checks.
#pragma once

#include "MqttPublisherTransport.h"
#include "MqttDownlinkMailbox.h"

#if defined(CONFIG_MQTT_LIB)
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include "MqttAtomic.h"

namespace mt::mqtt {

class ZephyrMqttPublisherDriver final : public Driver {
 public:
  ZephyrMqttPublisherDriver() = default;
  ~ZephyrMqttPublisherDriver() override { disconnect(); }

  bool tlsTrustAvailable(const TlsTrust &trust) const override;
  int connect(const Settings &settings) override;
  int publish(const char *topic, const uint8_t *payload, size_t size,
              uint8_t qos) override;
  void poll() override;
  void setDownlinkMailbox(DownlinkMailbox<2> *mailbox){mailbox_=mailbox;}
  bool connected() const override { return connected_; }
  bool connecting() const override { return connecting_; }
  void disconnect() override;

  // Nonzero means malformed, unbounded, unrequested or unqueueable broker data
  // was refused and the connection was closed.
  int refusedIncomingPublish() const { return refusedIncomingPublish_.load(AtomicOrder::Acquire); }
  int lastError() const { return lastError_.load(AtomicOrder::Acquire); }

 private:
  static void event(struct mqtt_client *client, const struct mqtt_evt *evt);
  int resolve(const char *host, uint16_t port);
  void fail(int error);
  int subscribeDownlink();

  struct mqtt_client client_{};
  struct net_sockaddr_storage broker_{};
  sec_tag_t tlsTag_ = SEC_TAG_TLS_INVALID;
  struct mqtt_utf8 clientId_{}, username_{}, password_{};
  uint8_t rx_[256]{}, tx_[768]{}; // fits eight maximum bounded channel filters
  // Borrow the transport-owned bounded strings. PublisherTransport keeps its
  // settings buffers stable until disconnect(), so this avoids a second 8x160
  // byte filter copy in internal RAM.
  const char *subscriptionTopics_[Settings::MaxSubscriptionTopics]{};
  uint8_t subscriptionTopicCount_=0;
  DownlinkMailbox<2> *mailbox_=nullptr;
  bool initialized_ = false, connecting_ = false, connected_ = false;
  bool subscribing_=false,subscribed_=false;
  uint16_t subscriptionMessageId_=1;
  int64_t connectDeadlineMs_ = 0;
  Atomic<int> refusedIncomingPublish_{0}, lastError_{0};
};

}  // namespace mt::mqtt
#endif  // CONFIG_MQTT_LIB
