#include "network/MqttPublisherTransport.h"
#include <assert.h>
#include <initializer_list>
#include <string.h>

using namespace mt::mqtt;

struct Mock : Driver {
  bool trust = true, live = false, pending = false, failConnect = false, failPublish = false;
  unsigned trustCalls = 0;
  enum class TrustCase { Valid, Missing, ZeroLength, WrongType, BackendError } trustCase = TrustCase::Valid;
  unsigned connects = 0, disconnects = 0, sends = 0, polls = 0;
  bool lastWantsLink=false,lastProxyToClient=false;
  uint8_t subscriptionCount=0;char subscriptions[Settings::MaxSubscriptionTopics][160]{};
  bool tlsTrustAvailable(const TlsTrust &t) const override {
    const_cast<Mock *>(this)->trustCalls++;
    return trust && t.enabled && t.securityTag >= 0 && trustCase == TrustCase::Valid;
  }
  int connect(const Settings &settings) override {
    ++connects;lastWantsLink=settings.wantsLink;lastProxyToClient=settings.proxyToClient;
    subscriptionCount=settings.subscriptionTopicCount;
    for(size_t i=0;i<subscriptionCount;++i)strncpy(subscriptions[i],settings.subscriptionTopics[i],sizeof(subscriptions[i])-1);
    live = !failConnect; pending = !failConnect; return failConnect ? -1 : 0;
  }
  int publish(const char *, const uint8_t *, size_t, uint8_t qos) override {
    assert(qos == 0); ++sends; return failPublish ? -1 : 0;
  }
  void poll() override { ++polls; }
  bool connected() const override { return live; }
  bool connecting() const override { return pending && !live; }
  void disconnect() override { live = pending = false; ++disconnects; }
};

int main() {
  Mock driver;
  PublisherTransport<2, 24, 8>::QueueSlot slots[2]{};
  PublisherTransport<2, 24, 8> pub(driver, slots);
  Settings s{};
  assert(!s.wantsLink&&!s.proxyToClient);
  char host[] = "broker.invalid", id[] = "client", user[] = "u", pass[] = "p";
  s.enabled = true; s.host = host; s.port = 1883; s.clientId = id; s.username = user; s.password = pass;
  assert(!pub.enabled() && pub.state() == State::Stopped);
  s.wantsLink=true;
  Settings proxyOnly=s;proxyOnly.proxyToClient=true;
  assert(proxyOnly.wantsLink&&proxyOnly.proxyToClient);
  assert(!pub.start(proxyOnly,true)&&pub.state()==State::Stopped&&driver.connects==0);
  uint8_t data[] = {1, 2, 3};
  assert(pub.enqueue("t", data, sizeof(data)) == PublishResult::Disabled);
  Settings tls = s; tls.tls = true; tls.trust.enabled = true; tls.trust.securityTag = 7;
  driver.trust = false;
  assert(!pub.start(tls, true) && pub.state() == State::Refused && driver.connects == 0);
  driver.trust = true;
  const unsigned trustChecks = driver.trustCalls;
  tls.trust.securityTag = -1;
  assert(!pub.start(tls, true) && driver.connects == 0);
  tls.trust.securityTag = 7;
  for (Mock::TrustCase invalid : {Mock::TrustCase::Missing, Mock::TrustCase::ZeroLength,
                                  Mock::TrustCase::WrongType, Mock::TrustCase::BackendError}) {
    driver.trustCase = invalid;
    assert(!pub.start(tls, true) && pub.state() == State::Refused && driver.connects == 0);
  }
  assert(driver.trustCalls >= trustChecks + 4);
  driver.trustCase = Mock::TrustCase::Valid;
  assert(pub.start(tls, true));
  pub.stop();
  char channelA[]="root/2/e/Alpha/+",channelB[]="root/2/e/Beta/+";
  s.subscriptionTopicCount=2;s.subscriptionTopics[0]=channelA;s.subscriptionTopics[1]=channelB;
  assert(pub.start(s, false) && pub.state() == State::WaitingForWifi);
  memset(channelA,'x',sizeof(channelA)-1);memset(channelB,'y',sizeof(channelB)-1);
  assert(pub.enqueue("t", data, sizeof(data)) == PublishResult::NotReady);
  pub.setWifiReady(true);
  memset(host, 'x', sizeof(host)-1); host[sizeof(host)-1] = 0;
  memset(id, 'x', sizeof(id)-1); id[sizeof(id)-1] = 0;
  memset(user, 'x', sizeof(user)-1); user[sizeof(user)-1] = 0;
  memset(pass, 'x', sizeof(pass)-1); pass[sizeof(pass)-1] = 0;
  assert(pub.enqueue("one", data, sizeof(data)) == PublishResult::Accepted);
  assert(pub.enqueue("two", data, sizeof(data)) == PublishResult::Accepted);
  assert(pub.enqueue("three", data, sizeof(data)) == PublishResult::QueueFull);
  pub.service();
  assert(driver.connects == 1 && driver.sends == 2 && pub.queued() == 0);
  assert(driver.subscriptionCount==2&&!strcmp(driver.subscriptions[0],"root/2/e/Alpha/+")&&
         !strcmp(driver.subscriptions[1],"root/2/e/Beta/+"));
  assert(driver.lastWantsLink&&!driver.lastProxyToClient); // immutable scalar policy snapshot copied
  pub.service();
  assert(driver.sends == 2); // accepted records publish exactly once
  assert(pub.enqueue("retry", data, sizeof(data)) == PublishResult::Accepted);
  driver.failPublish = true;
  pub.service();
  assert(pub.queued() == 1 && pub.state() == State::Backoff);
  driver.failPublish = false;
  assert(pub.retry());
  pub.service();
  assert(pub.queued() == 0 && driver.sends == 4);
  assert(pub.enqueue("wifi", data, sizeof(data)) == PublishResult::Accepted);
  pub.setWifiReady(false);
  pub.service();
  assert(pub.state() == State::WaitingForWifi && pub.queued() == 1);
  pub.service(); assert(pub.queued() == 1);
  pub.stop(); assert(!pub.enabled() && pub.queued() == 0 && pub.state() == State::Stopped);
  assert(pub.start(s, true)); driver.failConnect = true; pub.service();
  assert(pub.state() == State::Backoff);
  pub.shutdown(); assert(!pub.enabled());
}
