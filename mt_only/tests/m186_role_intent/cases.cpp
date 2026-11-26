// SPDX-License-Identifier: GPL-3.0-only
// Host regression of the production RemoteDeviceTransaction and typed journals.
#include "admin/RemoteDeviceTransaction.h"
#include "ModuleJournal.h"
#include "modules/CompletionJournal.h"
#include "notifications/NotificationJournal.h"
#include "network/MqttGatewayJournal.h"
#include "monocypher.h"
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace mt;
using Tx=mt::admin::RemoteDeviceTransaction;
using Bundle=mt::admin::RemoteDeviceBundle;
using Mqtt=mt::network::MqttGatewaySettings;

static unsigned checks=0,groups=0;
#define CHECK(expr) do { ++checks; if(!(expr)){ std::fprintf(stderr,"FAIL line=%d: %s\n",__LINE__,#expr); std::abort(); } } while(0)
#define GROUP(name) do { ++groups; std::printf("GROUP %s\n",name); } while(0)

struct Memory final:SettingsBackend {
  static constexpr size_t MaxValues=24,MaxValue=4096,MaxKey=32,MaxEvents=64;
  struct Value { char key[MaxKey]{};uint8_t bytes[MaxValue]{};size_t length=0;bool present=false; };
  std::array<Value,MaxValues> values{};
  std::array<std::array<char,MaxKey>,MaxEvents> writeLog{};
  unsigned reads=0,writes=0,loggedWrites=0;
  unsigned failWriteAt=0,failReadAt=0,shortReadAt=0;
  bool failAfterCommit=false;
  char failReadKey[MaxKey]{};unsigned failReadKeyNth=0,matchingReads=0;
  int locate(const char *key)const {for(size_t i=0;i<values.size();++i)if(values[i].present&&!std::strcmp(values[i].key,key))return int(i);return -1;}
  int freeSlot()const {for(size_t i=0;i<values.size();++i)if(!values[i].present)return int(i);return -1;}
  int read(const char *key,uint8_t *out,size_t capacity)override {
    ++reads;const int slot=locate(key);
    const bool keyedFault=failReadKey[0]&&!std::strcmp(key,failReadKey)&&++matchingReads==failReadKeyNth;
    if((failReadAt&&reads==failReadAt)||keyedFault)return -EIO;
    if(slot<0)return -ENOENT;
    const auto &value=values[size_t(slot)];if(value.length>capacity)return -E2BIG;
    const size_t count=shortReadAt==reads&&value.length?value.length-1:value.length;
    if(count)std::memcpy(out,value.bytes,count);
    return int(count);
  }
  int write(const char *key,const uint8_t *in,size_t length)override {
    ++writes;if(loggedWrites<writeLog.size()){std::strncpy(writeLog[loggedWrites].data(),key,MaxKey-1);++loggedWrites;}
    if(length>MaxValue)return -E2BIG;
    if(failWriteAt==writes&&!failAfterCommit)return -EIO;
    int slot=locate(key);if(slot<0)slot=freeSlot();if(slot<0)return -ENOSPC;
    auto &value=values[size_t(slot)];std::memset(&value,0,sizeof(value));
    std::strncpy(value.key,key,MaxKey-1);if(length)std::memcpy(value.bytes,in,length);
    value.length=length;value.present=true;
    return failWriteAt==writes&&failAfterCommit?-EIO:0;
  }
  void clearFaults(){failWriteAt=failReadAt=shortReadAt=0;failAfterCommit=false;failReadKey[0]=0;failReadKeyNth=matchingReads=0;}
  void armReadKey(const char *key,unsigned nth=1){std::strncpy(failReadKey,key,MaxKey-1);failReadKeyNth=nth;matchingReads=0;}
  void setRaw(const char *key,const uint8_t *data,size_t length){
    int slot=locate(key);if(slot<0)slot=freeSlot();CHECK(slot>=0&&length<=MaxValue);
    auto &value=values[size_t(slot)];std::memset(&value,0,sizeof(value));
    std::strncpy(value.key,key,MaxKey-1);std::memcpy(value.bytes,data,length);value.length=length;value.present=true;
  }
};

static Tx::Domain domain(uint32_t node=0x10203040) {
  Tx::Domain d{};d.node=node;for(unsigned i=0;i<32;++i)d.publicKey[i]=uint8_t(i+1);return d;
}
static bool digest(void *,const uint8_t *bytes,size_t length,uint8_t out[32]) {
  crypto_blake2b(out,32,bytes,length);return true;
}
static uint32_t crc32(const uint8_t *p,size_t n) {
  uint32_t c=UINT32_MAX;for(size_t i=0;i<n;++i){c^=p[i];for(unsigned b=0;b<8;++b)c=(c>>1)^(0xedb88320u&uint32_t(-int32_t(c&1)));}return ~c;
}

struct Owner {
  Memory &backend;
  SettingsJournal core;
  ModuleJournal modules;
  mt::modules::CompletionJournal completion;
  NotificationJournal notifications;
  mt::network::MqttGatewayJournal mqtt;
  bool pause=true;
  bool ready=false;
  int failSection=-1;
  unsigned saveCalls[Bundle::SectionCount]{};
  Tx observer;
  explicit Owner(Memory &b):backend(b) { load(); }
  void load(){
    CHECK(core.load(backend)==0);CHECK(modules.load(backend)==0);CHECK(completion.load(backend)==0);
    CHECK(notifications.load(backend)==0);CHECK(mqtt.load(backend)==0);ready=true;
  }
  Bundle snapshot()const {
    Bundle b{};b.core=core.config();b.modules=modules.config();b.completion=completion.config();
    b.notifications=notifications.config();b.mqtt=mqtt.config();return b;
  }
  static bool backendReady(void *){return true;}
  static bool journalsReady(void *p){return static_cast<Owner *>(p)->ready;}
  static bool pauseVerified(void *p){return static_cast<Owner *>(p)->pause;}
  static int capture(void *p,uint8_t section,uint8_t *out,size_t capacity,size_t &length){
    auto &o=*static_cast<Owner *>(p);const Bundle b=o.snapshot();size_t size=0;const uint8_t *source=b.section(section,size);
    if(!source||size!=capacity)return -EINVAL;std::memcpy(out,source,size);length=size;return 0;
  }
  static int validate(void *,uint8_t section,const uint8_t *bytes,size_t length){
    switch(section){
    case 0:{
      if(length!=Config::Size)return -EINVAL;Config x;std::memcpy(x.bytes,bytes,length);
      return Bundle::validIntentCore(x)?0:-EINVAL;
    }
    case 1:{if(length!=ModuleSettings::Size)return -EINVAL;ModuleSettings x;std::memcpy(x.bytes,bytes,length);return x.valid()?0:-EINVAL;}
    case 2:{if(length!=mt::modules::CompletionSettings::Size)return -EINVAL;mt::modules::CompletionSettings x;std::memcpy(x.bytes,bytes,length);return x.valid()?0:-EINVAL;}
    case 3:{if(length!=NotificationSettings::Size)return -EINVAL;NotificationSettings x;std::memcpy(x.bytes,bytes,length);return x.valid()?0:-EINVAL;}
    case 4:{if(length!=Mqtt::Size)return -EINVAL;Mqtt x;std::memcpy(x.bytes,bytes,length);return x.valid()?0:-EINVAL;}
    default:return -EINVAL;
    }
  }
  static int save(void *p,uint8_t section,const uint8_t *bytes,size_t length){
    auto &o=*static_cast<Owner *>(p);++o.saveCalls[section];
    Tx::Callbacks cb=o.callbacks();Tx::Outcome observed{};
    CHECK(o.observer.inspect(domain(),cb,observed)==0);
    CHECK(observed.state==Tx::State::Pending&&observed.intentVerified);
    if(o.failSection==int(section))return -EIO;
    switch(section){
    case 0:{
      Config x;std::memcpy(x.bytes,bytes,length);
      if(x.bytes[2]!=Config::Version&&!Config::decode(x.bytes,Config::Size,x))return -EINVAL;
      return o.core.save(o.backend,x);
    }
    case 1:{ModuleSettings x;std::memcpy(x.bytes,bytes,length);return o.modules.save(o.backend,x);}
    case 2:{mt::modules::CompletionSettings x;std::memcpy(x.bytes,bytes,length);return o.completion.save(o.backend,x);}
    case 3:{NotificationSettings x;std::memcpy(x.bytes,bytes,length);return o.notifications.save(o.backend,x);}
    case 4:{Mqtt x;std::memcpy(x.bytes,bytes,length);
      if(!std::memcmp(o.mqtt.config().bytes,x.bytes,Mqtt::ConsentBase))
        return o.mqtt.saveChannelConsentCandidate(o.backend,x);
      return o.mqtt.saveLoRaPolicyCandidate(o.backend,x);}
    default:return -EINVAL;
    }
  }
  static bool hash(void *,const uint8_t *bytes,size_t length,uint8_t out[32]){return digest(nullptr,bytes,length,out);}
  Tx::Callbacks callbacks(){return {&backend,this,&backendReady,&journalsReady,&pauseVerified,&capture,&validate,&save,&hash};}
};

static Bundle changedCandidate(const Bundle &old) {
  Bundle candidate=old;
  CHECK(candidate.core.setNodeInfoSeconds(7200));
  meshtastic_ModuleConfig telemetry{};
  CHECK(candidate.modules.get(telemetry,meshtastic_ModuleConfig_telemetry_tag));
  telemetry.payload_variant.telemetry.device_telemetry_enabled=true;
  telemetry.payload_variant.telemetry.device_update_interval=1800;
  CHECK(candidate.modules.set(telemetry));
  // CompletionSettings owns supplemental telemetry; StatusMessage is owned
  // by ModuleSettings and is intentionally not a CompletionSettings key.
  CHECK(candidate.completion.set(telemetry));
  CHECK(candidate.notifications.set(true,true,true,false,3,1500,30));
  CHECK(candidate.mqtt.setConfigOkToMqtt(true));
  CHECK(candidate.valid());return candidate;
}
static bool sameSection(const Bundle &a,const Bundle &b,unsigned section){
  size_t as=0,bs=0;const uint8_t *ap=a.section(uint8_t(section),as),*bp=b.section(uint8_t(section),bs);
  return ap&&bp&&as==bs&&!std::memcmp(ap,bp,as);
}
static Tx::Outcome apply(Tx &tx,Owner &owner,const Tx::Domain &d,const Bundle &candidate,int &rc){
  Tx::Outcome out{};auto cb=owner.callbacks();rc=tx.beginAndCommit(d,candidate,cb,out);return out;
}
static void recover(Memory &memory,const Tx::Domain &d,const Bundle &target){
  memory.clearFaults();Owner restarted(memory);Tx tx;auto cb=restarted.callbacks();Tx::Outcome out{};
  CHECK(tx.resume(d,cb,out)==0);CHECK(out.state==Tx::State::Completed&&out.completeVerified);
  const Bundle now=restarted.snapshot();for(unsigned i=0;i<Bundle::SectionCount;++i)CHECK(sameSection(now,target,i));
}

static void success_and_pending_before_sections(){
  GROUP("durable pending precedes all five typed saves");Memory backend;Owner owner(backend);Tx tx;
  const auto old=owner.snapshot(),target=changedCandidate(old);int rc=0;const auto out=apply(tx,owner,domain(),target,rc);
  CHECK(rc==0&&out.state==Tx::State::Completed&&out.completeVerified&&out.intentVerified);
  CHECK(out.verifiedSectionMask==Tx::SectionMask);
  for(unsigned i=0;i<Bundle::SectionCount;++i){CHECK(owner.saveCalls[i]==1);CHECK(sameSection(owner.snapshot(),target,i));}
  CHECK(backend.loggedWrites>=7);CHECK(!std::strcmp(backend.writeLog[0].data(),"mtrole/intent0"));
  CHECK(!std::strcmp(backend.writeLog[1].data(),"mtbase/state0"));
  CHECK(!std::strcmp(backend.writeLog[2].data(),"mtmod/state0"));
  CHECK(!std::strcmp(backend.writeLog[3].data(),"mtcomplete/state0"));
  CHECK(!std::strcmp(backend.writeLog[4].data(),"mtnotify/state0"));
  CHECK(!std::strcmp(backend.writeLog[5].data(),"mtmqtt/state0"));
  CHECK(!std::strcmp(backend.writeLog[6].data(),"mtrole/intent1"));
}

static void write_failure_rollforward_matrix(){
  GROUP("after-commit write uncertainty at intent, every section, and completion");
  for(unsigned boundary=1;boundary<=7;++boundary){
    Memory backend;Owner owner(backend);const auto old=owner.snapshot(),target=changedCandidate(old);
    backend.failWriteAt=backend.writes+boundary;backend.failAfterCommit=true;
    Tx tx;int rc=0;const auto out=apply(tx,owner,domain(),target,rc);CHECK(rc!=0);CHECK(!out.completeVerified);
    backend.clearFaults();Owner restarted(backend);const Bundle before=restarted.snapshot();
    for(unsigned section=0;section<Bundle::SectionCount;++section)
      CHECK(sameSection(before,old,section)||sameSection(before,target,section));
    Tx recovery;auto cb=restarted.callbacks();Tx::Outcome resumed{};CHECK(recovery.resume(domain(),cb,resumed)==0);
    CHECK(resumed.state==Tx::State::Completed&&resumed.completeVerified);
    const auto after=restarted.snapshot();for(unsigned section=0;section<Bundle::SectionCount;++section)CHECK(sameSection(after,target,section));
  }
}

static void readback_failure_matrix(){
  GROUP("typed journal readback uncertainty at every section rolls forward");
  const char *keys[]={"mtbase/state0","mtmod/state0","mtcomplete/state0","mtnotify/state0","mtmqtt/state0"};
  for(unsigned section=0;section<Bundle::SectionCount;++section){
    Memory backend;Owner owner(backend);const auto old=owner.snapshot();const auto target=changedCandidate(old);backend.armReadKey(keys[section]);
    Tx tx;int rc=0;const auto out=apply(tx,owner,domain(),target,rc);CHECK(rc!=0);CHECK(!out.completeVerified);
    backend.clearFaults();Owner restarted(backend);const auto before=restarted.snapshot();
    for(unsigned s=0;s<Bundle::SectionCount;++s)CHECK(sameSection(before,old,s)||sameSection(before,target,s));
    Tx recovery;auto cb=restarted.callbacks();Tx::Outcome resumed{};CHECK(recovery.resume(domain(),cb,resumed)==0);
    CHECK(resumed.completeVerified);const auto after=restarted.snapshot();
    for(unsigned s=0;s<Bundle::SectionCount;++s)CHECK(sameSection(after,target,s));
  }
}

static void pending_intent_readback_failure(){
  GROUP("pending intent commit survives unreadable readback without early section saves");
  Memory backend;Owner owner(backend);const auto target=changedCandidate(owner.snapshot());
  backend.armReadKey("mtrole/intent0",2);Tx tx;int rc=0;const auto out=apply(tx,owner,domain(),target,rc);
  CHECK(rc!=0&&!out.completeVerified);for(unsigned i=0;i<Bundle::SectionCount;++i)CHECK(owner.saveCalls[i]==0);
  backend.clearFaults();recover(backend,domain(),target);
}

static void callback_failure_and_pause_gate(){
  GROUP("section callback failure retains pending intent; failed quiescence writes nothing");
  Memory backend;Owner owner(backend);const auto before=owner.snapshot(),target=changedCandidate(before);
  owner.failSection=3;Tx tx;int rc=0;auto out=apply(tx,owner,domain(),target,rc);
  CHECK(rc==-EIO&&out.state==Tx::State::Held&&out.intentVerified);
  for(unsigned i=0;i<3;++i){CHECK(owner.saveCalls[i]==1);}
  CHECK(owner.saveCalls[3]==1&&owner.saveCalls[4]==0);
  backend.clearFaults();recover(backend,domain(),target);
  Memory gated;Owner blocked(gated);blocked.pause=false;const auto snapshot=blocked.snapshot();Tx noWrite;
  auto cb=blocked.callbacks();Tx::Outcome held{};CHECK(noWrite.beginAndCommit(domain(),changedCandidate(snapshot),cb,held)!=0);
  CHECK(gated.writes==0);CHECK(blocked.snapshot().core.same(snapshot.core));
}

static void stale_and_domain_handling(){
  GROUP("stale section holds; pending domain mismatch holds; completed old domain never replays");
  Memory backend;Owner owner(backend);const auto old=owner.snapshot(),target=changedCandidate(old);
  owner.failSection=0;Tx tx;int rc=0;auto pending=apply(tx,owner,domain(),target,rc);CHECK(rc==-EIO&&pending.intentVerified);
  auto stale=owner.modules.config();meshtastic_ModuleConfig telemetry{};CHECK(stale.get(telemetry,meshtastic_ModuleConfig_telemetry_tag));
  telemetry.payload_variant.telemetry.device_telemetry_enabled=true;telemetry.payload_variant.telemetry.device_update_interval=2600;
  CHECK(stale.set(telemetry));CHECK(owner.modules.save(backend,stale)==0);
  Owner restarted(backend);Tx heldTx;auto cb=restarted.callbacks();Tx::Outcome held{};
  CHECK(heldTx.resume(domain(),cb,held)==-ESTALE);CHECK(held.state==Tx::State::Held);
  CHECK(restarted.modules.config().same(stale));

  Memory domainStore;Owner domainOwner(domainStore);const auto domainTarget=changedCandidate(domainOwner.snapshot());
  domainOwner.failSection=0;Tx pendingTx;int pendingRc=0;(void)apply(pendingTx,domainOwner,domain(),domainTarget,pendingRc);CHECK(pendingRc==-EIO);
  Owner wrongOwner(domainStore);Tx wrongTx;auto wrongCb=wrongOwner.callbacks();Tx::Outcome mismatch{};auto wrong=domain();wrong.publicKey[0]^=0x80;
  CHECK(wrongTx.resume(wrong,wrongCb,mismatch)==-EXDEV);CHECK(mismatch.state==Tx::State::DomainMismatch);

  Memory completeStore;Owner completeOwner(completeStore);const auto completedTarget=changedCandidate(completeOwner.snapshot());
  Tx completeTx;int completeRc=0;auto completeOut=apply(completeTx,completeOwner,domain(),completedTarget,completeRc);
  CHECK(completeRc==0&&completeOut.completeVerified);const unsigned writes=completeStore.writes;
  Owner historicalOwner(completeStore);Tx historicalTx;auto historicalCb=historicalOwner.callbacks();Tx::Outcome historical{};auto newDomain=domain(0x10203041);
  CHECK(historicalTx.resume(newDomain,historicalCb,historical)==0);
  CHECK(historical.state==Tx::State::Completed&&historical.historicalCompleted&&!historical.intentVerified);
  CHECK(completeStore.writes==writes);for(unsigned s=0;s<Bundle::SectionCount;++s)CHECK(sameSection(historicalOwner.snapshot(),completedTarget,s));
}

static void corrupt_oversize_io_and_generation(){
  GROUP("corrupt, oversize, I/O, and exhausted generation remain held");
  Memory corrupt;Owner co(corrupt);co.failSection=0;auto target=changedCandidate(co.snapshot());Tx tx;int rc=0;(void)apply(tx,co,domain(),target,rc);CHECK(rc==-EIO);
  int intent=corrupt.locate("mtrole/intent0");CHECK(intent>=0);corrupt.values[size_t(intent)].bytes[0]^=1;
  Owner corruptReader(corrupt);Tx c;auto ccb=corruptReader.callbacks();Tx::Outcome result{};CHECK(c.inspect(domain(),ccb,result)==-EBADMSG);CHECK(result.state==Tx::State::Corrupt);

  Memory tooLarge;Owner largeOwner(tooLarge);largeOwner.failSection=0;target=changedCandidate(largeOwner.snapshot());Tx t;int largeRc=0;(void)apply(t,largeOwner,domain(),target,largeRc);CHECK(largeRc==-EIO);
  intent=tooLarge.locate("mtrole/intent0");CHECK(intent>=0);tooLarge.values[size_t(intent)].length=Tx::RecordBytes+1;
  Owner largeReader(tooLarge);Tx l;auto lcb=largeReader.callbacks();Tx::Outcome lo{};CHECK(l.inspect(domain(),lcb,lo)==-EBADMSG);CHECK(lo.state==Tx::State::Corrupt);

  Memory io;Owner ioOwner(io);io.armReadKey("mtrole/intent0");Tx i;auto icb=ioOwner.callbacks();Tx::Outcome ioOut{};
  CHECK(i.inspect(domain(),icb,ioOut)==-EIO);CHECK(ioOut.state==Tx::State::IoError);

  Memory exhausted;Owner ex(exhausted);target=changedCandidate(ex.snapshot());Tx done;int doneRc=0;CHECK(apply(done,ex,domain(),target,doneRc).completeVerified&&doneRc==0);
  intent=exhausted.locate("mtrole/intent1");CHECK(intent>=0);auto &record=exhausted.values[size_t(intent)];
  record.bytes[12]=0xff;record.bytes[13]=0xff;record.bytes[14]=0xff;record.bytes[15]=0xff;
  const size_t crcOffset=Tx::CrcOffset;const uint32_t crc=crc32(record.bytes,crcOffset);
  for(unsigned b=0;b<4;++b)record.bytes[crcOffset+b]=uint8_t(crc>>(8*b));
  Owner exReader(exhausted);Tx overflow;auto ocb=exReader.callbacks();Tx::Outcome oo{};
  CHECK(overflow.beginAndCommit(domain(),changedCandidate(exReader.snapshot()),ocb,oo)==-EOVERFLOW);
}

static Mqtt configuredBroker(){
  Mqtt value;meshtastic_ModuleConfig_MQTTConfig input=meshtastic_ModuleConfig_MQTTConfig_init_zero;
  input.enabled=false;input.tls_enabled=true;input.proxy_to_client_enabled=true;
  std::strcpy(input.address,"broker.example");std::strcpy(input.username,"operator");std::strcpy(input.password,"secret-config");
  std::strcpy(input.root,"mesh/base");input.map_reporting_enabled=true;input.has_map_report_settings=true;
  input.map_report_settings.publish_interval_secs=1234;input.map_report_settings.position_precision=32;
  input.map_report_settings.should_report_location=true;CHECK(value.assign(input));CHECK(value.valid());return value;
}
static void strict_mqtt_deltas_and_credential_preservation(){
  GROUP("strict typed MQTT consent and LoRa-only saves preserve broker credentials");
  Memory backend;mt::network::MqttGatewayJournal journal;CHECK(journal.load(backend)==0);const auto configured=configuredBroker();
  CHECK(journal.save(backend,configured,false,false)==0);const auto baseline=journal.config();
  Config core;CHECK(core.valid());Mqtt consent=baseline;CHECK(consent.setChannelUplink(core,0,true));
  CHECK(journal.saveChannelConsentCandidate(backend,consent)==0);CHECK(journal.config().channelUplinkEnabled(core,0));
  CHECK(!std::memcmp(journal.config().bytes,baseline.bytes,Mqtt::ConsentBase));
  Mqtt unauthorized=journal.config();meshtastic_ModuleConfig_MQTTConfig exportValue{};
  CHECK(unauthorized.exportConfig(exportValue));std::strcpy(exportValue.password,"changed-password");CHECK(unauthorized.assign(exportValue));
  CHECK(journal.saveChannelConsentCandidate(backend,unauthorized)==-EPERM);

  Mqtt policy=journal.config();CHECK(policy.setConfigOkToMqtt(true));
  CHECK(policy.exportConfig(exportValue));std::strcpy(exportValue.root,"mesh/region");CHECK(policy.assign(exportValue));
  CHECK(journal.saveLoRaPolicyCandidate(backend,policy)==0);
  CHECK(journal.config().configOkToMqtt());CHECK(journal.config().channelUplinkEnabled(core,0));
  CHECK(!std::memcmp(journal.config().bytes+Mqtt::Address,baseline.bytes+Mqtt::Address,160));
  CHECK(journal.config().bytes[Mqtt::PasswordLength]==baseline.bytes[Mqtt::PasswordLength]);
  CHECK(!std::memcmp(journal.config().bytes+Mqtt::Password,baseline.bytes+Mqtt::Password,32));

  Mqtt combined=journal.config();CHECK(combined.setConfigOkToMqtt(false));CHECK(combined.setChannelUplink(core,0,false));
  CHECK(journal.saveLoRaPolicyCandidate(backend,combined)==-EPERM);
  CHECK(journal.saveChannelConsentCandidate(backend,combined)==-EPERM);
  Mqtt altered=journal.config();CHECK(altered.exportConfig(exportValue));exportValue.tls_enabled=false;
  CHECK(altered.assign(exportValue));CHECK(journal.saveLoRaPolicyCandidate(backend,altered)==-EPERM);
}

static void transaction_mqtt_consent_validation(){
  GROUP("transaction accepts one core-bound consent delta and rejects mixed policy delta");
  Memory backend;Owner owner(backend);const auto old=owner.snapshot();Bundle consent=old;
  CHECK(consent.mqtt.setChannelUplink(consent.core,0,true));CHECK(consent.valid());Tx tx;auto cb=owner.callbacks();Tx::Outcome out{};
  CHECK(tx.beginAndCommit(domain(),consent,cb,out)==0);CHECK(out.completeVerified);CHECK(owner.mqtt.config().channelUplinkEnabled(consent.core,0));

  Memory mixedBackend;Owner mixedOwner(mixedBackend);Bundle mixed=mixedOwner.snapshot();
  CHECK(mixed.mqtt.setConfigOkToMqtt(true));CHECK(mixed.mqtt.setChannelUplink(mixed.core,0,true));
  Tx mixedTx;auto mixedCb=mixedOwner.callbacks();Tx::Outcome rejected{};
  const unsigned writes=mixedBackend.writes;CHECK(mixedTx.beginAndCommit(domain(),mixed,mixedCb,rejected)==-EPERM);
  CHECK(mixedBackend.writes==writes);CHECK(mixedOwner.mqtt.config().valid());

  Memory credentialBackend;mt::network::MqttGatewayJournal initial;CHECK(initial.load(credentialBackend)==0);
  const Mqtt broker=configuredBroker();CHECK(initial.save(credentialBackend,broker,false,false)==0);
  Owner credentialOwner(credentialBackend);const Bundle credentialOld=credentialOwner.snapshot();
  const Bundle credentialTarget=changedCandidate(credentialOld);Tx credentialTx;auto credentialCb=credentialOwner.callbacks();
  Tx::Outcome credentialOut{};CHECK(credentialTx.beginAndCommit(domain(),credentialTarget,credentialCb,credentialOut)==0);
  CHECK(credentialOut.completeVerified&&sameSection(credentialOwner.snapshot(),credentialTarget,4));
  const Mqtt &saved=credentialOwner.mqtt.config();
  CHECK(!std::memcmp(saved.bytes+Mqtt::Address,broker.bytes+Mqtt::Address,Mqtt::Root-Mqtt::Address));
  CHECK(saved.bytes[Mqtt::Flags]&Mqtt::Tls);CHECK(saved.bytes[Mqtt::PasswordLength]==broker.bytes[Mqtt::PasswordLength]);
}

static Config asLegacy(const Config &current,unsigned version=22){
  CHECK(current.valid()&&(version>=24||!current.ignoredSenderCount())&&(version>=23||!current.primaryAead()));
  Config legacy=current;legacy.bytes[2]=version;legacy.seal();
  CHECK(legacy.validBody(version));return legacy;
}
static void put32(uint8_t *p,uint32_t value){for(unsigned i=0;i<4;++i)p[i]=uint8_t(value>>(8*i));}
static std::array<uint8_t,Tx::RecordBytes> makeLegacyPending(const Tx::Domain &d,const Bundle &base,
                                                            const Bundle &target,uint32_t generation=7){
  CHECK(base.validIntent()&&target.validIntent());
  std::array<uint8_t,Tx::RecordBytes> record{};
  record[0]='Z';record[1]='T';record[2]='R';record[3]='I';record[4]=1;record[5]=1;
  put32(record.data()+8,uint32_t(Tx::RecordBytes));put32(record.data()+12,generation);
  put32(record.data()+16,d.node);std::memcpy(record.data()+20,d.publicKey,32);
  for(uint8_t section=0;section<Bundle::SectionCount;++section){
    size_t length=0;const uint8_t *bytes=base.section(section,length);
    CHECK(bytes&&length==Bundle::sectionSize(section));
    crypto_blake2b(record.data()+Tx::DigestsOffset+size_t(section)*Tx::DigestBytes,32,bytes,length);
  }
  std::memcpy(record.data()+Tx::BundleOffset,&target,sizeof(target));
  put32(record.data()+Tx::CrcOffset,crc32(record.data(),Tx::CrcOffset));return record;
}
static std::array<uint8_t,Tx::RecordBytes> makeLegacyCompleted(const Tx::Domain &d,uint32_t generation){
  std::array<uint8_t,Tx::RecordBytes> record{};
  record[0]='Z';record[1]='T';record[2]='R';record[3]='I';record[4]=1;record[5]=2;
  put32(record.data()+8,uint32_t(Tx::RecordBytes));put32(record.data()+12,generation);
  put32(record.data()+16,d.node);std::memcpy(record.data()+20,d.publicKey,32);
  put32(record.data()+Tx::CrcOffset,crc32(record.data(),Tx::CrcOffset));return record;
}
static bool sameLegacyPending(const Memory &backend,const std::array<uint8_t,Tx::RecordBytes> &expected){
  const int slot=backend.locate("mtrole/intent0");
  return slot>=0&&backend.values[size_t(slot)].length==expected.size()&&
    !std::memcmp(backend.values[size_t(slot)].bytes,expected.data(),expected.size());
}
static bool allZero(const uint8_t *bytes,size_t length){
  for(size_t i=0;i<length;++i)if(bytes[i])return false;
  return true;
}
#if defined(CONFIG_MT_LOCAL_CLOCK)
static void v24_clock_boundary(){
  for(bool different:{false,true}){
    Memory backend;Owner owner(backend);Config core=owner.core.config();
    uint32_t ignored=0xaabbccdd;CHECK(core.setIgnoredSenders(&ignored,1));CHECK(owner.core.save(backend,core)==0);
    Bundle base=owner.snapshot(),target=changedCandidate(owner.snapshot());
    base.core=asLegacy(base.core,24);target.core=asLegacy(target.core,24);
    const auto pending=makeLegacyPending(domain(),base,target,41);
    backend.setRaw("mtrole/intent0",pending.data(),pending.size());
    if(different){char zone[65]="CST6CDT,M3.2.0,M11.1.0";CHECK(core.setTimezone(zone)&&owner.core.save(backend,core)==0);}
    auto cb=owner.callbacks();Tx tx;Tx::Outcome out{};
    const unsigned writes=backend.writes;
    if(different){CHECK(tx.resume(domain(),cb,out)==-EBADMSG&&backend.writes==writes&&sameLegacyPending(backend,pending));}
    else{CHECK(tx.resume(domain(),cb,out)==0&&out.completeVerified&&owner.core.config().ignoresIncoming(ignored));}
  }
}
#endif
static void legacy_rollforward(){
  GROUP("V22 and V23 intents preserve byte/digest authority across interrupted core writes");
  for(unsigned version:{22u,23u})for(unsigned afterCoreWrite=0;afterCoreWrite<2;++afterCoreWrite){
    Memory backend;Owner owner(backend);Config policy=owner.core.config();
    CHECK(policy.setSecurityOptions(true,true));
    if(version==23){
      CHECK(policy.setPrimaryAead(true));ChannelRecord child;
      const uint8_t key[16]={0x62};
      CHECK(child.set(2,"Child",key,sizeof(key),0,false,false,false,true)&&policy.setSecondaryChannel(1,child));
    }
    CHECK(owner.core.save(backend,policy)==0);
    const Bundle currentBase=owner.snapshot();
    Bundle base=currentBase;base.core=asLegacy(base.core,version);
    Bundle target=changedCandidate(currentBase);target.core=asLegacy(target.core,version);
    const auto pending=makeLegacyPending(domain(),base,target,41);backend.setRaw("mtrole/intent0",pending.data(),pending.size());
    const unsigned coreGeneration=owner.core.generation();owner.failSection=afterCoreWrite?1:0;
    Tx first;auto cb=owner.callbacks();Tx::Outcome interrupted{};
    CHECK(first.resume(domain(),cb,interrupted)==-EIO);CHECK(interrupted.intentVerified&&!interrupted.completeVerified);
    CHECK(sameLegacyPending(backend,pending));
    if(afterCoreWrite){
      Config migratedTarget;CHECK(Config::decode(target.core.bytes,Config::Size,migratedTarget));
      CHECK(owner.core.config().same(migratedTarget));CHECK(owner.core.config().bytes[2]==24); // No clock preference in these historical images.
      CHECK(owner.core.generation()==coreGeneration+1);
    }else{
      CHECK(owner.core.config().same(currentBase.core));CHECK(owner.core.generation()==coreGeneration);
    }
    backend.clearFaults();Owner restarted(backend);Tx recovery;auto restartCb=restarted.callbacks();Tx::Outcome resumed{};
    CHECK(recovery.resume(domain(),restartCb,resumed)==0);CHECK(resumed.completeVerified&&resumed.generation==42);
    CHECK(sameLegacyPending(backend,pending));
    const Bundle actual=restarted.snapshot();Config migratedTarget;
    CHECK(Config::decode(target.core.bytes,Config::Size,migratedTarget));CHECK(actual.core.same(migratedTarget));
    CHECK(actual.core.debugLogApiEnabled()&&actual.core.adminChannelEnabled());
    for(unsigned section=1;section<Bundle::SectionCount;++section)CHECK(sameSection(actual,target,section));
    const int completedSlot=backend.locate("mtrole/intent1");CHECK(completedSlot>=0);
    const auto &completed=backend.values[size_t(completedSlot)];
    CHECK(completed.bytes[5]==2&&completed.length==Tx::RecordBytes);
    CHECK(allZero(completed.bytes+Tx::DigestsOffset,Tx::CrcOffset-Tx::DigestsOffset));
    Tx historical;auto historicalCb=restarted.callbacks();Tx::Outcome historicalOut{};auto other=domain(0x10203041);
    CHECK(historical.inspect(other,historicalCb,historicalOut)==0);
    CHECK(historicalOut.state==Tx::State::Completed&&historicalOut.historicalCompleted&&!historicalOut.intentVerified);
    Bundle next=changedCandidate(actual);Tx fresh;auto freshCb=restarted.callbacks();Tx::Outcome freshOut{};
    CHECK(fresh.beginAndCommit(domain(),next,freshCb,freshOut)==0);
    CHECK(freshOut.completeVerified&&freshOut.generation==44);
  }
}
static void pending_filter_conflicts(){
  GROUP("V24 sender preferences and divergent authenticated V23 bytes cannot be lost during recovery");
  for(unsigned version:{22u,23u})for(bool newFilter:{false,true}){
    Memory backend;Owner owner(backend);const Bundle current=owner.snapshot();
    Bundle base=current;base.core=asLegacy(base.core,version);
    Bundle target=changedCandidate(current);target.core=asLegacy(target.core,version);
    auto pending=makeLegacyPending(domain(),base,target,9);
    if(!newFilter){pending[Tx::DigestsOffset]^=1;put32(pending.data()+Tx::CrcOffset,crc32(pending.data(),Tx::CrcOffset));}
    backend.setRaw("mtrole/intent0",pending.data(),pending.size());
    if(newFilter){Config c=current.core;uint32_t zero=0;CHECK(c.setIgnoredSenders(&zero,1)&&owner.core.save(backend,c)==0);}
    auto writes=backend.writes;Tx tx;auto cb=owner.callbacks();Tx::Outcome out{};
    CHECK(tx.resume(domain(),cb,out)==(newFilter?-EBADMSG:-ESTALE));
    CHECK(tx.state()==Tx::State::Held&&backend.writes==writes&&sameLegacyPending(backend,pending));
  }
}
static void legacy_v22_rejects_aead_and_corruption(){
  GROUP("V22 migration cannot erase V23 AEAD and malformed historical intent remains corrupt");
  for(unsigned secondary=0;secondary<2;++secondary){
    Memory backend;Owner owner(backend);Bundle base=owner.snapshot();base.core=asLegacy(base.core);
    Bundle target=changedCandidate(owner.snapshot());target.core=asLegacy(target.core);
    auto pending=makeLegacyPending(domain(),base,target,9);backend.setRaw("mtrole/intent0",pending.data(),pending.size());
    Config flagged=owner.core.config();
    if(secondary){ChannelRecord record;const uint8_t key[16]={0x63};
      CHECK(record.set(2,"AeadChild",key,sizeof(key),0,false,false,false,true));
      CHECK(flagged.setSecondaryChannel(1,record));
    }else CHECK(flagged.setPrimaryAead(true));
    CHECK(flagged.valid()&&owner.core.save(backend,flagged)==0);
    const unsigned writes=backend.writes;Tx held;auto cb=owner.callbacks();Tx::Outcome out{};
    CHECK(held.resume(domain(),cb,out)!=0);CHECK(out.state==Tx::State::Held||held.state()==Tx::State::Held);
    CHECK(backend.writes==writes&&sameLegacyPending(backend,pending));
  }
  {
    Memory backend;Owner owner(backend);Bundle base=owner.snapshot();base.core=asLegacy(base.core);
    Bundle target=changedCandidate(owner.snapshot());target.core=asLegacy(target.core);
    auto pending=makeLegacyPending(domain(),base,target,3);pending[6]=1;
    put32(pending.data()+Tx::CrcOffset,crc32(pending.data(),Tx::CrcOffset));
    backend.setRaw("mtrole/intent0",pending.data(),pending.size());Tx reader;auto cb=owner.callbacks();Tx::Outcome out{};
    CHECK(reader.inspect(domain(),cb,out)==-EBADMSG&&out.state==Tx::State::Corrupt);
  }
  {
    Memory backend;Owner owner(backend);Bundle base=owner.snapshot();base.core=asLegacy(base.core);
    Bundle target=changedCandidate(owner.snapshot());target.core=asLegacy(target.core);
    auto pending=makeLegacyPending(domain(),base,target,4);pending[Tx::BundleOffset+604]=1;
    put32(pending.data()+Tx::CrcOffset,crc32(pending.data(),Tx::CrcOffset));
    backend.setRaw("mtrole/intent0",pending.data(),pending.size());Tx reader;auto cb=owner.callbacks();Tx::Outcome out{};
    CHECK(reader.inspect(domain(),cb,out)==-EBADMSG&&out.state==Tx::State::Corrupt);
  }
  {
    Memory backend;Owner owner(backend);Bundle base=owner.snapshot();base.core=asLegacy(base.core);
    Bundle target=changedCandidate(owner.snapshot());target.core=asLegacy(target.core);
    auto pending=makeLegacyPending(domain(),base,target,5);pending[Tx::BundleOffset+Config::Size-2]^=1;
    put32(pending.data()+Tx::CrcOffset,crc32(pending.data(),Tx::CrcOffset));
    backend.setRaw("mtrole/intent0",pending.data(),pending.size());Tx reader;auto cb=owner.callbacks();Tx::Outcome out{};
    CHECK(reader.inspect(domain(),cb,out)==-EBADMSG&&out.state==Tx::State::Corrupt);
  }
  {
    Memory backend;Owner owner(backend);Bundle base=owner.snapshot();base.core=asLegacy(base.core);
    Bundle target=changedCandidate(owner.snapshot());target.core=asLegacy(target.core);
    auto pending=makeLegacyPending(domain(),base,target,6);pending[Tx::CrcOffset]^=1;
    backend.setRaw("mtrole/intent0",pending.data(),pending.size());Tx reader;auto cb=owner.callbacks();Tx::Outcome out{};
    CHECK(reader.inspect(domain(),cb,out)==-EBADMSG&&out.state==Tx::State::Corrupt);
  }
  {
    Memory backend;Owner owner(backend);const auto completed=makeLegacyCompleted(domain(),17);
    backend.setRaw("mtrole/intent0",completed.data(),completed.size());Tx reader;auto cb=owner.callbacks();Tx::Outcome out{};
    CHECK(reader.inspect(domain(),cb,out)==0&&out.state==Tx::State::Completed&&out.generation==17);
    CHECK(out.completeVerified&&out.intentVerified);
    Bundle next=changedCandidate(owner.snapshot());Tx fresh;auto freshCb=owner.callbacks();Tx::Outcome freshOut{};
    CHECK(fresh.beginAndCommit(domain(),next,freshCb,freshOut)==0);
    CHECK(freshOut.completeVerified&&freshOut.generation==19);
  }
}

int main(){
  success_and_pending_before_sections();
  write_failure_rollforward_matrix();
  readback_failure_matrix();
  pending_intent_readback_failure();
  callback_failure_and_pause_gate();
  stale_and_domain_handling();
  corrupt_oversize_io_and_generation();
  strict_mqtt_deltas_and_credential_preservation();
  transaction_mqtt_consent_validation();
#if defined(CONFIG_MT_LOCAL_CLOCK)
  v24_clock_boundary();
#endif
  legacy_rollforward();pending_filter_conflicts();
  legacy_v22_rejects_aead_and_corruption();
  std::printf("SUMMARY groups=%u checks=%u failures=0\n",groups,checks);
  return 0;
}
