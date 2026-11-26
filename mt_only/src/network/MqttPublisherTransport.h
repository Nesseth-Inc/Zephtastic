// SPDX-License-Identifier: MIT
// Bounded MQTT publish state machine, adapted from ZephCore's
// ZephyrMQTTPublisher (source hash recorded in mqtt-publisher-provenance.md).
// Platform networking is deliberately behind Driver; this file has no Zephyr
// dependency and is exercised directly by the native sanitizer fixture.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "MqttAtomic.h"
#include <string.h>

namespace mt::mqtt {

enum class PublishResult : uint8_t { Accepted, Disabled, NotReady, QueueFull, Invalid };
enum class State : uint8_t { Stopped, WaitingForWifi, Connecting, Online, Backoff, Refused };

struct TlsTrust {
  bool enabled = false;
  // Zephyr MQTT accepts trust through its configured credential security tag.
  int securityTag = -1;
};

struct Settings {
  // Eight ordinary channel filters plus the stock PKI wildcard filter.
  static constexpr size_t MaxSubscriptionTopics=9;
  bool enabled = false;
  bool tls = false;
  // Immutable boot snapshot of stock feature demand and proxy ownership.
  // `wantsLink` means MQTT is enabled and a channel/map feature needs it;
  // network readiness is supplied separately by the runtime.
  bool wantsLink = false;
  bool proxyToClient = false;
  const char *host = nullptr;
  uint16_t port = 0;
  const char *clientId = nullptr;
  const char *username = nullptr;
  const char *password = nullptr;
  uint8_t subscriptionTopicCount=0;
  const char *subscriptionTopics[MaxSubscriptionTopics]{}; // ordinary and PKI filters under channel consent
  TlsTrust trust{};
};

struct Driver {
  virtual ~Driver() = default;
  virtual bool tlsTrustAvailable(const TlsTrust &) const = 0;
  virtual int connect(const Settings &) = 0;
  virtual int publish(const char *topic, const uint8_t *payload, size_t size, uint8_t qos) = 0;
  virtual void poll() = 0;
  virtual bool connected() const = 0;
  virtual bool connecting() const = 0;
  virtual void disconnect() = 0;
};

// Caller owns QueueSlot storage. Payload and topic are copied at enqueue time;
// no heap or silently discarded accepted item. A full queue is observable.
template <size_t QueueCapacity, size_t TopicCapacity, size_t PayloadCapacity>
class PublisherTransport {
 public:
  struct QueueSlot {
    size_t topicSize = 0, payloadSize = 0;
    char topic[TopicCapacity]{};
    uint8_t payload[PayloadCapacity]{};
  };

  PublisherTransport(Driver &driver, QueueSlot (&slots)[QueueCapacity])
      : driver_(driver), slots_(slots) {}

  bool start(const Settings &settings, bool wifiReady) {
    stop();
    if (!settings.enabled || settings.proxyToClient || !settings.host || !settings.clientId || !settings.port) {
      state_.store(State::Stopped, AtomicOrder::Release);
      return false;
    }
    if (settings.tls && (!settings.trust.enabled || settings.trust.securityTag < 0 ||
        !driver_.tlsTrustAvailable(settings.trust))) {
      state_.store(State::Refused, AtomicOrder::Release);
      return false;
    }
    if (!copy(settings.host, host_, sizeof(host_)) ||
        !copy(settings.clientId, clientId_, sizeof(clientId_)) ||
        !copy(settings.username ? settings.username : "", username_, sizeof(username_)) ||
        !copy(settings.password ? settings.password : "", password_, sizeof(password_))) {
      state_.store(State::Refused, AtomicOrder::Release);
      return false;
    }
    if(settings.subscriptionTopicCount>Settings::MaxSubscriptionTopics) {
      state_.store(State::Refused,AtomicOrder::Release);return false;
    }
    for(size_t i=0;i<settings.subscriptionTopicCount;++i)
      if(!copy(settings.subscriptionTopics[i],subscriptionTopics_[i],sizeof(subscriptionTopics_[i]))) {
        state_.store(State::Refused,AtomicOrder::Release);return false;
      }
    settings_ = settings;
    settings_.host = host_;
    settings_.clientId = clientId_;
    settings_.username = username_[0] ? username_ : nullptr;
    settings_.password = password_[0] ? password_ : nullptr;
    for(size_t i=0;i<settings_.subscriptionTopicCount;++i)
      settings_.subscriptionTopics[i]=subscriptionTopics_[i];
    wifiReady_.store(wifiReady, AtomicOrder::Relaxed);
    enabled_.store(true, AtomicOrder::Release);
    state_.store(wifiReady ? State::Connecting : State::WaitingForWifi,
                 AtomicOrder::Release);
    return true;
  }

  void setWifiReady(bool ready) {
    // This may be called from the WiFi/main owner. All socket teardown and
    // reconnect work remains on the transport worker in service().
    wifiReady_.store(ready, AtomicOrder::Release);
  }

  PublishResult enqueue(const char *topic, const uint8_t *payload, size_t size) {
    if (!enabled_.load(AtomicOrder::Acquire)) return PublishResult::Disabled;
    if (!wifiReady_.load(AtomicOrder::Acquire)) return PublishResult::NotReady;
    size_t topicLength = 0;
    if (!topic || !payload || !size || size > PayloadCapacity ||
        !boundedLength(topic, TopicCapacity, topicLength) || topicLength == 0)
      return PublishResult::Invalid;
    const size_t tail = tail_.load(AtomicOrder::Relaxed);
    const size_t head = head_.load(AtomicOrder::Acquire);
    if (tail - head == QueueCapacity) return PublishResult::QueueFull;
    QueueSlot &slot = slots_[tail % QueueCapacity];
    slot.topicSize = topicLength;
    memcpy(slot.topic, topic, slot.topicSize + 1);
    memcpy(slot.payload, payload, size);
    slot.payloadSize = size;
    tail_.store(tail + 1, AtomicOrder::Release);
    return PublishResult::Accepted;
  }

  // One bounded cooperative service step. A dequeued item is removed only
  // after the driver accepts it; failed sends remain queued for reconnect.
  void service() {
    if (!enabled_.load(AtomicOrder::Acquire)) return;
    if (!wifiReady_.load(AtomicOrder::Acquire)) {
      if (state_.load(AtomicOrder::Acquire) == State::Online ||
          state_.load(AtomicOrder::Acquire) == State::Connecting)
        driver_.disconnect();
      state_.store(State::WaitingForWifi, AtomicOrder::Release);
      return;
    }
    if (state_.load(AtomicOrder::Acquire) == State::WaitingForWifi)
      state_.store(State::Connecting, AtomicOrder::Release);
    if (state_.load(AtomicOrder::Acquire) == State::Connecting) {
      if (!driver_.connecting() && driver_.connect(settings_) != 0) {
        state_.store(State::Backoff, AtomicOrder::Release);
        return;
      }
    }
    driver_.poll();
    if (!driver_.connected()) {
      if (!driver_.connecting()) state_.store(State::Backoff, AtomicOrder::Release);
      return;
    }
    state_.store(State::Online, AtomicOrder::Release);
    for (;;) {
      size_t head = head_.load(AtomicOrder::Relaxed);
      if (head == tail_.load(AtomicOrder::Acquire)) break;
      QueueSlot &slot = slots_[head % QueueCapacity];
      if (driver_.publish(slot.topic, slot.payload, slot.payloadSize, 0) != 0) {
        driver_.disconnect();
        state_.store(State::Backoff, AtomicOrder::Release);
        return;
      }
      head_.store(head + 1, AtomicOrder::Release);
    }
  }

  // Caller invokes retry when its scheduler's bounded backoff expires.
  bool retry() {
    if (!enabled_.load(AtomicOrder::Acquire) || !wifiReady_.load(AtomicOrder::Acquire) ||
        state_.load(AtomicOrder::Acquire) != State::Backoff) return false;
    state_.store(State::Connecting, AtomicOrder::Release);
    if (driver_.connect(settings_) != 0) { state_.store(State::Backoff, AtomicOrder::Release); return false; }
    return true;
  }

  void stop() {
    if (state_.load(AtomicOrder::Acquire) == State::Online ||
        state_.load(AtomicOrder::Acquire) == State::Connecting) driver_.disconnect();
    enabled_.store(false, AtomicOrder::Release);
    head_.store(0, AtomicOrder::Relaxed);
    tail_.store(0, AtomicOrder::Relaxed);
    wifiReady_.store(false, AtomicOrder::Relaxed);
    state_.store(State::Stopped, AtomicOrder::Release);
    for (size_t i = 0; i < QueueCapacity; ++i) {
      memset(&slots_[i], 0, sizeof(slots_[i]));
    }
    memset(host_, 0, sizeof(host_));
    memset(clientId_, 0, sizeof(clientId_));
    memset(username_, 0, sizeof(username_));
    memset(password_, 0, sizeof(password_));
    memset(subscriptionTopics_,0,sizeof(subscriptionTopics_));
    settings_ = {};
  }
  void shutdown() { stop(); }
  State state() const { return state_.load(AtomicOrder::Acquire); }
  size_t queued() const {
    return tail_.load(AtomicOrder::Acquire) - head_.load(AtomicOrder::Acquire);
  }
  bool enabled() const { return enabled_.load(AtomicOrder::Acquire); }

 private:
  static bool copy(const char *from, char *to, size_t capacity) {
    if (!from) return false;
    size_t size = 0;
    while (size < capacity && from[size]) ++size;
    if (size == capacity) return false;
    memcpy(to, from, size);
    to[size] = '\0';
    return true;
  }
  static bool boundedLength(const char *text, size_t capacity, size_t &length) {
    if (!text) return false;
    length = 0;
    while (length < capacity && text[length]) ++length;
    return length < capacity;
  }
  Driver &driver_;
  QueueSlot (&slots_)[QueueCapacity];
  Settings settings_{};
  char host_[128]{}, clientId_[64]{}, username_[96]{}, password_[96]{};
  char subscriptionTopics_[Settings::MaxSubscriptionTopics][160]{};
  Atomic<size_t> head_{0}, tail_{0};
  Atomic<bool> enabled_{false}, wifiReady_{false};
  Atomic<State> state_{State::Stopped};
};

}  // namespace mt::mqtt
