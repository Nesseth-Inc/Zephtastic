// SPDX-License-Identifier: MIT
#include "ZephyrMqttPublisherDriver.h"
#include "MqttRuntime.h"

#if defined(CONFIG_MQTT_LIB)
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>

namespace mt::mqtt {

bool ZephyrMqttPublisherDriver::tlsTrustAvailable(const TlsTrust &trust) const {
#if defined(CONFIG_MQTT_LIB_TLS)
  // Zephyr's mqtt_sec_config consumes security tags. Probe only the CA
  // credential type with a zero-capacity destination: this SDK's credential
  // stores return -EFBIG and the required positive size without copying bytes.
  // A missing tag/type, zero-length CA, bad tag, or backend error fails closed.
  if (!trust.enabled || trust.securityTag < 0) return false;
  uint8_t probe = 0;
  size_t certificateSize = 0;
  const int rc = tls_credential_get(static_cast<sec_tag_t>(trust.securityTag),
                                   TLS_CREDENTIAL_CA_CERTIFICATE,
                                   &probe, &certificateSize);
  return certificateSize > 0 && (rc == -EFBIG || rc == 0);
#else
  (void)trust;
  return false;
#endif
}

int ZephyrMqttPublisherDriver::resolve(const char *host, uint16_t port) {
  char service[6];
  const int n = snprintf(service, sizeof(service), "%u", port);
  if (n <= 0 || static_cast<size_t>(n) >= sizeof(service)) return -EINVAL;
  struct zsock_addrinfo hints{};
  hints.ai_family = NET_AF_UNSPEC;
  hints.ai_socktype = NET_SOCK_STREAM;
  struct zsock_addrinfo *result = nullptr;
  const int rc = zsock_getaddrinfo(host, service, &hints, &result);
  if (rc != 0 || !result || !result->ai_addr ||
      result->ai_addrlen > sizeof(broker_)) {
    if (result) zsock_freeaddrinfo(result);
    return -EHOSTUNREACH;
  }
  memset(&broker_, 0, sizeof(broker_));
  memcpy(&broker_, result->ai_addr, result->ai_addrlen);
  zsock_freeaddrinfo(result);
  return 0;
}

int ZephyrMqttPublisherDriver::connect(const Settings &settings) {
  if (!settings.enabled || !settings.wantsLink || settings.proxyToClient ||
      !settings.host || !settings.clientId || !settings.port)
    return -EINVAL;
  if (connecting_ || connected_) return 0;
  if (settings.tls && !tlsTrustAvailable(settings.trust)) return -EACCES;
  if (settings.tls && !certificateTimeReady()) {
    lastError_ = -EAGAIN;
    return -EAGAIN;
  }
  if(settings.subscriptionTopicCount>Settings::MaxSubscriptionTopics){lastError_=-EINVAL;return -EINVAL;}
  subscriptionTopicCount_=settings.subscriptionTopicCount;
  for(size_t i=0;i<subscriptionTopicCount_;++i) {
    const char *input=settings.subscriptionTopics[i];size_t n=0;
    if(!input){lastError_=-EINVAL;return -EINVAL;}
    while(n<160&&input[n])++n;
    if(!n||n>=160){lastError_=-EINVAL;return -EINVAL;}
    subscriptionTopics_[i]=input;
  }
  subscribing_=subscribed_=false;subscriptionMessageId_=1;

  int rc = resolve(settings.host, settings.port);
  if (rc) { lastError_ = rc; return rc; }

  mqtt_client_init(&client_);
  initialized_ = true;
  clientId_ = {reinterpret_cast<const uint8_t *>(settings.clientId),
               static_cast<uint32_t>(strlen(settings.clientId))};
  client_.client_id = clientId_;
  client_.broker = &broker_;
  client_.rx_buf = rx_;
  client_.rx_buf_size = sizeof(rx_);
  client_.tx_buf = tx_;
  client_.tx_buf_size = sizeof(tx_);
  client_.keepalive = 60;
  client_.clean_session = 1;
  client_.evt_cb = &ZephyrMqttPublisherDriver::event;
  client_.user_data = this;

  if (settings.username && *settings.username) {
    username_ = {reinterpret_cast<const uint8_t *>(settings.username),
                 static_cast<uint32_t>(strlen(settings.username))};
    client_.user_name = &username_;
    if(settings.password){
      password_ = {reinterpret_cast<const uint8_t *>(settings.password),
                   static_cast<uint32_t>(strlen(settings.password))};
      client_.password = &password_;
    }
  }

  if (settings.tls) {
#if defined(CONFIG_MQTT_LIB_TLS)
    tlsTag_ = static_cast<sec_tag_t>(settings.trust.securityTag);
    client_.transport.type = MQTT_TRANSPORT_SECURE;
    struct mqtt_sec_config &tls = client_.transport.tls.config;
    tls.peer_verify = ZSOCK_TLS_PEER_VERIFY_REQUIRED;
    tls.sec_tag_count = 1;
    tls.sec_tag_list = &tlsTag_;
    tls.hostname = settings.host;
#else
    return -ENOTSUP;
#endif
  } else {
    client_.transport.type = MQTT_TRANSPORT_NON_SECURE;
  }

  // mqtt_connect opens the socket and sends CONNECT. CONNACK is delivered
  // asynchronously by mqtt_input() from poll(); it is not treated as ready yet.
  rc = mqtt_connect(&client_);
  if (rc != 0) {
    lastError_ = rc;
    initialized_ = false;
    return rc;
  }
  connecting_ = true;
  connectDeadlineMs_ = k_uptime_get() + 30000;
  connected_ = false;
  lastError_ = 0;
  refusedIncomingPublish_ = 0;
  return 0;
}

int ZephyrMqttPublisherDriver::publish(const char *topic, const uint8_t *payload,
                                       size_t size, uint8_t qos) {
  if (!connected_ || !topic || !payload || !size || qos != 0 ||
      strlen(topic) > UINT32_MAX || size > UINT32_MAX) return -EINVAL;
  struct mqtt_publish_param param{};
  param.message.topic.topic.utf8 = reinterpret_cast<const uint8_t *>(topic);
  param.message.topic.topic.size = static_cast<uint32_t>(strlen(topic));
  param.message.topic.qos = MQTT_QOS_0_AT_MOST_ONCE;
  param.message.payload.data = const_cast<uint8_t *>(payload);
  param.message.payload.len = static_cast<uint32_t>(size);
  param.message_id = 0;  // Redundant for QoS 0.
  return mqtt_publish(&client_, &param);
}

void ZephyrMqttPublisherDriver::event(struct mqtt_client *client,
                                      const struct mqtt_evt *evt) {
  auto *self = static_cast<ZephyrMqttPublisherDriver *>(client->user_data);
  if (!self || !evt) return;
  switch (evt->type) {
    case MQTT_EVT_CONNACK:
      self->connecting_ = false;
      self->connected_ = evt->result == 0 && evt->param.connack.return_code == 0;
      if (!self->connected_) self->lastError_ = evt->result ? evt->result : -ECONNREFUSED;
      else if(self->subscriptionTopicCount_) {
        const int rc=self->subscribeDownlink();
        if(rc<0)self->refusedIncomingPublish_=rc;
      }
      break;
    case MQTT_EVT_DISCONNECT:
      self->connecting_ = false;
      self->connected_ = false;
      self->subscribing_=self->subscribed_=false;
      if (evt->result) self->lastError_ = evt->result;
      break;
    case MQTT_EVT_SUBACK:
      if(evt->result!=0) {
        self->refusedIncomingPublish_=evt->result<0?evt->result:-EIO;
        break;
      }
      if(!self->subscribing_||evt->param.suback.message_id!=self->subscriptionMessageId_||
         evt->param.suback.return_codes.len!=self->subscriptionTopicCount_||
         !evt->param.suback.return_codes.data) {
        self->refusedIncomingPublish_=-EACCES;
      } else {
        // The subscription requested QoS 1. A broker may grant either QoS 0
        // or QoS 1; a QoS 2 grant or any failure/unknown code is refused.
        for(size_t i=0;i<self->subscriptionTopicCount_;++i) {
          const uint8_t granted=evt->param.suback.return_codes.data[i];
          if(granted!=MQTT_SUBACK_SUCCESS_QoS_0&&
             granted!=MQTT_SUBACK_SUCCESS_QoS_1) {
            self->refusedIncomingPublish_=-EACCES;return;
          }
        }
        self->subscribing_=false;self->subscribed_=true;
      }
      break;
    case MQTT_EVT_PUBLISH:
      // Read the complete payload only after a broker-configured subscription
      // and strict topic/payload bounds are confirmed. Any refusal closes the
      // connection after mqtt_input returns, keeping frame parsing aligned.
      {
        if(evt->result!=0) {
          self->refusedIncomingPublish_=evt->result<0?evt->result:-EIO;
          break;
        }
        const auto &message=evt->param.publish.message;
        const size_t topicSize=message.topic.topic.size;
        const size_t payloadSize=message.payload.len;
        const uint8_t *topicData=message.topic.topic.utf8;
        const auto qos=message.topic.qos;
        // Stop handling additional events in the same mqtt_input() pass once
        // one event has failed closed; poll() disconnects after mqtt_input().
        if(self->refusedIncomingPublish_)break;
        if(!self->subscribed_||!self->mailbox_||!topicData||!topicSize||
           topicSize>=DownlinkMessage::TopicCapacity||!payloadSize||
           payloadSize>DownlinkMessage::PayloadCapacity||
           (qos!=MQTT_QOS_0_AT_MOST_ONCE&&qos!=MQTT_QOS_1_AT_LEAST_ONCE)||
           (qos==MQTT_QOS_1_AT_LEAST_ONCE&&!evt->param.publish.message_id)||
           evt->param.publish.retain_flag) {
          self->refusedIncomingPublish_=-EMSGSIZE;break;
        }
        char topic[DownlinkMessage::TopicCapacity]{};memcpy(topic,topicData,topicSize);
        uint8_t payload[DownlinkMessage::PayloadCapacity]{};
        const int read=mqtt_readall_publish_payload(client,payload,payloadSize);
        if(read<0) {
          self->refusedIncomingPublish_=read;
        } else if(!self->mailbox_->push(topic,topicSize,payload,payloadSize)) {
          // Never acknowledge data that could not enter the existing bounded
          // mailbox. poll() closes the connection after mqtt_input() returns.
          self->refusedIncomingPublish_=-ENOBUFS;
        } else if(qos==MQTT_QOS_1_AT_LEAST_ONCE) {
          struct mqtt_puback_param ack{};
          ack.message_id=evt->param.publish.message_id;
          const int ackResult=mqtt_publish_qos1_ack(client,&ack);
          if(ackResult<0)self->refusedIncomingPublish_=ackResult;
        }
        memset(topic,0,sizeof(topic));memset(payload,0,sizeof(payload));
      }
      break;
    default:
      break;
  }
}

int ZephyrMqttPublisherDriver::subscribeDownlink() {
  if(!connected_||!subscriptionTopicCount_)return -EINVAL;
  if(subscribed_||subscribing_)return 0;
  struct mqtt_topic topics[Settings::MaxSubscriptionTopics]{};
  for(size_t i=0;i<subscriptionTopicCount_;++i) {
    topics[i].topic.utf8=reinterpret_cast<const uint8_t *>(subscriptionTopics_[i]);
    topics[i].topic.size=static_cast<uint32_t>(strlen(subscriptionTopics_[i]));
    topics[i].qos=MQTT_QOS_1_AT_LEAST_ONCE;
  }
  struct mqtt_subscription_list list{};list.list=topics;list.list_count=subscriptionTopicCount_;
  list.message_id=subscriptionMessageId_;
  const int rc=mqtt_subscribe(&client_,&list);
  if(rc<0)return rc;
  subscribing_=true;return 0;
}

void ZephyrMqttPublisherDriver::poll() {
  if (!initialized_ || (!connecting_ && !connected_)) return;
  if (connecting_ && k_uptime_get() >= connectDeadlineMs_) {
    fail(-ETIMEDOUT);
    return;
  }
  int sock = client_.transport.type == MQTT_TRANSPORT_NON_SECURE
                 ? client_.transport.tcp.sock
#if defined(CONFIG_MQTT_LIB_TLS)
                 : client_.transport.tls.sock
#else
                 : -1
#endif
                 ;
  struct zsock_pollfd pfd{};
  pfd.fd = sock;
  pfd.events = ZSOCK_POLLIN;
  const int ready = zsock_poll(&pfd, 1, 0);
  if (ready < 0 || (pfd.revents & (ZSOCK_POLLERR | ZSOCK_POLLHUP | ZSOCK_POLLNVAL))) {
    fail(ready < 0 ? -errno : -ECONNRESET);
    return;
  }
  if (ready > 0 && (pfd.revents & ZSOCK_POLLIN)) {
    const int rc = mqtt_input(&client_);
    if (refusedIncomingPublish_) { fail(refusedIncomingPublish_); return; }
    if (rc < 0 && rc != -EAGAIN && rc != -EWOULDBLOCK) { fail(rc); return; }
  }
  if (connected_) {
    const int rc = mqtt_live(&client_);
    if (rc < 0 && rc != -EAGAIN && rc != -EWOULDBLOCK) fail(rc);
  }
}

void ZephyrMqttPublisherDriver::fail(int error) {
  lastError_ = error;
  if (initialized_) mqtt_abort(&client_);
  initialized_ = connecting_ = connected_ = subscribing_ = subscribed_ = false;
}

void ZephyrMqttPublisherDriver::disconnect() {
  if (initialized_) mqtt_abort(&client_);
  initialized_ = connecting_ = connected_ = subscribing_ = subscribed_ = false;
  memset(subscriptionTopics_,0,sizeof(subscriptionTopics_));subscriptionTopicCount_=0;
}

}  // namespace mt::mqtt
#endif  // CONFIG_MQTT_LIB
