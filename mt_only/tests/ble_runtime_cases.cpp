// SPDX-License-Identifier: GPL-3.0-only
// Actual transport callback bodies, with deterministic non-radio stack doubles.
#include "../src/BleTransport.cpp"
#include <cstdio>
using namespace mt::ble;
static const uint8_t framed[]={0x94,0xc3,0,1,0x42};
static void attach(bt_conn&c){mockAdvertising=false;onConnected(&c,0);assert(c.auth);}
static void secure(bt_conn&c,bt_security_t s){c.level=s;onSecurityChanged(&c,s,BT_SECURITY_ERR_SUCCESS);}
static void detach(bt_conn&c){onDisconnected(&c,19);assert(c.refs==1);}
static void reply(const Packet&,void*){assert(send(framed,sizeof(framed),session()));}
static void legacyCases(){
 mt::BluetoothSettings cfg;assert(init(123,cfg,true)==0&&ready()&&mockAdvertising&&!mockBondable);
 bt_conn noPin;attach(noPin);assert(noPin.requested==BT_SECURITY_L2&&!noPin.auth->passkey_display);
 uint8_t out[16]{};assert(readNum(&noPin,nullptr,out,sizeof(out),0)<0);
 assert(!send(framed,sizeof(framed),session()));secure(noPin,BT_SECURITY_L2);
 assert(send(framed,sizeof(framed),session()));poll();assert(mockNotifies==1);
 assert(readFrom(&noPin,nullptr,out,sizeof(out),0)==1&&out[0]==0x42);
 cfg.mode=mt::BluetoothSettings::FixedPin;cfg.fixedPin=0;
 const unsigned beforeStage=mockDisconnects;assert(requestConfiguration(cfg,true));
 assert(mockDisconnects==beforeStage&&!ready()&&!send(framed,sizeof(framed),session()));
 poll();assert(mockDisconnects>beforeStage);detach(noPin);poll();assert(ready());
 bt_conn fixed;attach(fixed);assert(fixed.requested==BT_SECURITY_L3&&fixed.auth->app_passkey(&fixed)==0);
 fixed.auth->passkey_display(&fixed,123456);assert(!pairing().active);
 secure(fixed,BT_SECURITY_L2);assert(readNum(&fixed,nullptr,out,sizeof(out),0)<0);
 secure(fixed,BT_SECURITY_L3);assert(writeTo(&fixed,nullptr,"x",1,0,0)==1);
 Packet packet;assert(receive(packet));assert(dispatch(packet,reply,nullptr));
 const auto stale=packet;detach(fixed);assert(!dispatch(stale,reply,nullptr));poll();
 cfg.mode=mt::BluetoothSettings::RandomPin;assert(requestConfiguration(cfg,true));poll();
 bt_conn random;attach(random);assert(random.requested==BT_SECURITY_L3&&random.auth->app_passkey(&random)==BT_PASSKEY_RAND);
 random.auth->passkey_display(&random,0);assert(pairing().active&&pairing().passkey==0);
 mockAuthInfo->pairing_complete(&random,false);assert(!pairing().active);
 random.auth->passkey_display(&random,999999);assert(pairing().active);
 random.auth->cancel(&random);assert(!pairing().active&&!send(framed,sizeof(framed),session()));poll();detach(random);poll();
 bt_conn failed;mockOverlayError=-EALREADY;attach(failed);assert(!send(framed,sizeof(framed),session()));poll();detach(failed);mockOverlayError=0;poll();
 cfg.enabled=false;assert(requestConfiguration(cfg,true));poll();assert(!ready()&&!mockAdvertising);
 cfg.enabled=true;assert(requestConfiguration(cfg,false));poll();assert(!ready()&&!mockAdvertising);
 cfg.mode=99;assert(!requestConfiguration(cfg,false));poll();assert(!ready()&&!mockAdvertising);
 assert(sessionLockDepth==0);puts("PASS actual BLE transport: L2/L3 admission, transient PIN0/random handoff, policy staging, stale sessions, callback errors and out-of-lock stack boundary");
}

// The opaque payload exercises transport framing and read coverage only. The
// production Companion fixtures separately prove canonical Admin protobufs.
static const uint8_t adminFrame[]={0x94,0xc3,0,6,0xa1,0xb2,0xc3,0xd4,0xe5,0xf6};
static const uint8_t oldFrame[]={0x94,0xc3,0,4,0x11,0x22,0x33,0x44};
static constexpr size_t adminBytes=sizeof(adminFrame)-4;
static void configure(const mt::BluetoothSettings&cfg){
 assert(!connected());assert(requestConfiguration(cfg,true));poll();
 assert(ready()==cfg.enabled);assert(!adminBusy());
}
static mt::BluetoothSettings fixedSettings(){
 auto cfg=mt::BluetoothSettings::defaults();cfg.mode=mt::BluetoothSettings::FixedPin;cfg.fixedPin=123456;return cfg;
}
static void missingKeyRejectionCases(){
 const auto noPin=mt::BluetoothSettings::defaults();
 configure(noPin);
 bt_conn established;attach(established);secure(established,BT_SECURITY_L2);
 onSecurityChanged(&established,BT_SECURITY_L1,BT_SECURITY_ERR_PIN_OR_KEY_MISSING);
 secure(established,BT_SECURITY_L2);
 assert(!send(framed,sizeof(framed),session()));
 detach(established);poll();

 bt_conn failed;attach(failed);
 onSecurityChanged(&failed,BT_SECURITY_L1,BT_SECURITY_ERR_PIN_OR_KEY_MISSING);
 onPairingFailed(&failed,BT_SECURITY_ERR_AUTH_FAIL);
 secure(failed,BT_SECURITY_L2);
 assert(!send(framed,sizeof(framed),session()));
 detach(failed);poll();

 configure(fixedSettings());
 bt_conn fixed;attach(fixed);
 onSecurityChanged(&fixed,BT_SECURITY_L1,BT_SECURITY_ERR_PIN_OR_KEY_MISSING);
 secure(fixed,BT_SECURITY_L3);
 assert(!send(framed,sizeof(framed),session()));
 detach(fixed);poll();
}
static void start(bt_conn&conn,const mt::BluetoothSettings&cfg,int64_t now){
 mockNow=now;configure(cfg);attach(conn);
 secure(conn,cfg.mode==mt::BluetoothSettings::NoPin?BT_SECURITY_L2:BT_SECURITY_L3);
 assert(!adminBusy());
}
static uint32_t readCounter(bt_conn&conn){
 uint8_t bytes[4]{};assert(readNum(&conn,nullptr,bytes,sizeof(bytes),0)==4);
 return uint32_t(bytes[0])|(uint32_t(bytes[1])<<8)|(uint32_t(bytes[2])<<16)|(uint32_t(bytes[3])<<24);
}
static void counted(const Packet&,void*context){++*static_cast<unsigned*>(context);}
struct BeginContext {bool began=false;uint32_t token=0;};
static void beginCallback(const Packet&p,void*context){
 // Actual dispatch owns the recursive SessionLock. All stack mock boundaries
 // assert that no blocking stack operation occurs under that lock.
 assert(sessionLockDepth>0);auto&ctx=*static_cast<BeginContext*>(context);
 const auto calls=mockStackCalls;ctx.token=p.session;ctx.began=beginAdmin(p.session);
 assert(mockStackCalls==calls);
}
static Packet admitted(bt_conn&conn){
 assert(writeTo(&conn,nullptr,"x",1,0,0)==1);Packet p;assert(receive(p));return p;
}
static uint32_t begin(bt_conn&conn){
 const auto p=admitted(conn);BeginContext ctx;
 assert(dispatch(p,beginCallback,&ctx)&&ctx.began&&ctx.token==p.session&&adminBusy());
 assert(!beginAdmin(p.session));return p.session;
}
static void stage(uint32_t token){
 const auto calls=mockStackCalls;assert(stageAdminResult(adminFrame,sizeof(adminFrame),token));assert(mockStackCalls==calls);
}
static void finish(const mt::BluetoothSettings&cfg,uint32_t token,bool allowed=true){
 const auto calls=mockStackCalls;assert(finishAdminConfiguration(cfg,true,token,allowed));assert(mockStackCalls==calls);
}
static void blockedOrdinary(bt_conn&conn,uint32_t token,const char *retained=nullptr,size_t retainedLength=0){
 assert(!send(framed,sizeof(framed),token));
 assert(writeTo(&conn,nullptr,"x",1,0,0)<0);
 assert(writeTo(&conn,nullptr,"x",1,0,BT_GATT_WRITE_FLAG_PREPARE)<0);
 Packet p{};
 if(retained){
  assert(receive(p));assert(p.session==token&&p.length==retainedLength&&memcmp(p.bytes,retained,retainedLength)==0);
  unsigned retainedCalls=0;assert(!dispatch(p,counted,&retainedCalls)&&retainedCalls==0);
  assert(!receive(p));p={};
 } else assert(!receive(p));
 p.session=token;p.length=1;p.bytes[0]='x';
 unsigned calls=0;assert(!dispatch(p,counted,&calls)&&calls==0);
}
static void closedReply(bt_conn&conn,uint32_t token){
 uint8_t bytes[16]{};assert(readFrom(&conn,nullptr,bytes,sizeof(bytes),0)<0);
 assert(readNum(&conn,nullptr,bytes,sizeof(bytes),0)<0);blockedOrdinary(conn,token);
}
static void disconnectedAndApplied(bt_conn&conn){
 detach(conn);poll();assert(!adminBusy());
}
static void checkNewPolicy(const mt::BluetoothSettings&cfg){
 if(!cfg.enabled){assert(!ready()&&!mockAdvertising);return;}
 assert(ready());bt_conn next;attach(next);
 assert(next.requested==(cfg.mode==mt::BluetoothSettings::NoPin?BT_SECURITY_L2:BT_SECURITY_L3));
 if(cfg.mode==mt::BluetoothSettings::FixedPin)assert(next.auth->app_passkey(&next)==cfg.fixedPin);
 secure(next,next.requested);assert(send(framed,sizeof(framed),session()));
 detach(next);poll();
}

static void adminAdmissionAndCoverage(){
 auto cfg=mt::BluetoothSettings::defaults();mockNow=1000;configure(cfg);
 assert(!beginAdmin(0));assert(!stageAdminResult(adminFrame,sizeof(adminFrame),0));
 bt_conn conn;attach(conn);const auto token=session();
 assert(!beginAdmin(token));assert(!stageAdminResult(adminFrame,sizeof(adminFrame),token));
 const auto unauthNotifies=mockNotifies;ccc(nullptr,1);poll();assert(mockNotifies==unauthNotifies);
 secure(conn,BT_SECURITY_L2);assert(!beginAdmin(token+1));assert(!adminBusy());
 assert(!stageAdminResult(adminFrame,sizeof(adminFrame),token));
 assert(send(oldFrame,sizeof(oldFrame),token));assert(send(oldFrame,sizeof(oldFrame),token));
 uint8_t bytes[16]{};assert(readFrom(&conn,nullptr,bytes,2,0)==2&&bytes[0]==0x11&&bytes[1]==0x22);
 const auto p=admitted(conn);assert(writeTo(&conn,nullptr,"queued",6,0,0)==6);
 const auto counterBefore=readCounter(conn);
 BeginContext ctx;const auto stackCalls=mockStackCalls;
 assert(dispatch(p,beginCallback,&ctx)&&ctx.began&&ctx.token==token&&adminBusy());
 assert(mockStackCalls==stackCalls);assert(!beginAdmin(token));blockedOrdinary(conn,token,"queued",6);
 // Both retained Read Blob bytes and queued ordinary output are gone.
 assert(readFrom(&conn,nullptr,bytes,sizeof(bytes),2)<=0);
 assert(readFrom(&conn,nullptr,bytes,sizeof(bytes),0)<=0);
 const auto noReplyNotifies=mockNotifies;ccc(nullptr,1);poll();assert(mockNotifies==noReplyNotifies);
 assert(!stageAdminResult(nullptr,sizeof(adminFrame),token));
 const uint8_t empty[]={0x94,0xc3,0,0};assert(!stageAdminResult(empty,sizeof(empty),token));
 const uint8_t malformed[]={0x94,0xc3,0,2,0x42};assert(!stageAdminResult(malformed,sizeof(malformed),token));
 uint8_t oversized[517]{};oversized[0]=0x94;oversized[1]=0xc3;oversized[2]=2;oversized[3]=1;
 assert(!stageAdminResult(oversized,sizeof(oversized),token));
 assert(!stageAdminResult(adminFrame,sizeof(adminFrame),token+1));
 stage(token);
 assert(!stageAdminResult(oldFrame,sizeof(oldFrame),token));
 assert(readFrom(&conn,nullptr,bytes,sizeof(bytes),0)<=0);
 auto invalid=cfg;invalid.mode=99;assert(!finishAdminConfiguration(invalid,true,token,true));
 auto wrong=cfg;wrong.enabled=false;assert(!finishAdminConfiguration(wrong,true,token+1,true));
 const auto next=fixedSettings();finish(next,token);assert(adminBusy());
 assert(readCounter(conn)==counterBefore+1);blockedOrdinary(conn,token);
 bt_conn stranger;assert(readFrom(&stranger,nullptr,bytes,sizeof(bytes),0)<0);
 assert(readNum(&stranger,nullptr,bytes,sizeof(bytes),0)<0);
 const auto beforePoll=mockDisconnects;poll();assert(mockDisconnects==beforePoll);
 assert(mockNotifies==noReplyNotifies+1&&mockLastNotify==&conn&&adminBusy());
 // Notify and FromNum/CCC are signals only, never byte consumption.
 assert(readCounter(conn)==counterBefore+1);ccc(nullptr,1);poll();assert(adminBusy());
 assert(readFrom(&conn,nullptr,bytes,0,0)==0);
 assert(readFrom(&conn,nullptr,bytes,2,0)==2&&memcmp(bytes,adminFrame+4,2)==0);
 assert(readFrom(&conn,nullptr,bytes,2,3)<0); // gap after covered prefix2
 assert(readFrom(&conn,nullptr,bytes,2,adminBytes+1)<0);
 assert(readFrom(&conn,nullptr,bytes,2,0)==2&&memcmp(bytes,adminFrame+4,2)==0);
 assert(readFrom(&conn,nullptr,bytes,3,1)==3&&memcmp(bytes,adminFrame+5,3)==0); // overlap extends to4
 mockNow=1100;assert(readFrom(&conn,nullptr,bytes,2,4)==2&&memcmp(bytes,adminFrame+8,2)==0);
 mockNow=1199;assert(readFrom(&conn,nullptr,bytes,sizeof(bytes),0)==ssize_t(adminBytes));
 assert(memcmp(bytes,adminFrame+4,adminBytes)==0);poll();assert(mockDisconnects==beforePoll);
 // Repeated offset0 must not pop a new packet or restart the first-drain grace.
 mockNow=1200;poll();assert(mockDisconnects>beforePoll&&mockLastDisconnect==&conn);
 closedReply(conn,token);disconnectedAndApplied(conn);checkNewPolicy(next);
 puts("PASS admin barrier: secured dispatch, RAM-only begin/stage/finish, queue isolation, one result, token binding, contiguous/repeated/overlap reads and fixed drain grace");
}

static void adminDeadlineAndClock(){
 const auto cfg=mt::BluetoothSettings::defaults();bt_conn conn;
 const int64_t started=(int64_t(1)<<32)+5000;start(conn,cfg,started);
 assert(k_uptime_get()==started&&k_uptime_get_32()==5000);
 const auto token=begin(conn);stage(token);finish(cfg,token);
 const auto disconnects=mockDisconnects;uint8_t bytes[16]{};
 for(int delta: {0,1000,2000}){mockNow=started+delta;ccc(nullptr,1);poll();assert(adminBusy()&&mockDisconnects==disconnects);}
 // First drain is ten milliseconds before the immutable absolute deadline.
 // Its100ms grace cannot extend that deadline, even with repeated reads/CCC.
 mockNow=started+2990;assert(readFrom(&conn,nullptr,bytes,sizeof(bytes),0)==ssize_t(adminBytes));
 mockNow=started+2999;assert(readFrom(&conn,nullptr,bytes,2,0)==2);ccc(nullptr,1);poll();
 assert(mockDisconnects==disconnects);mockNow=started+3000;
 assert(readFrom(&conn,nullptr,bytes,sizeof(bytes),0)<0);poll();assert(mockDisconnects>disconnects);
 closedReply(conn,token);disconnectedAndApplied(conn);checkNewPolicy(cfg);

 bt_conn backwards;start(backwards,cfg,10000);const auto backwardsToken=begin(backwards);
 stage(backwardsToken);finish(cfg,backwardsToken);mockNow=10100;
 assert(readFrom(&backwards,nullptr,bytes,2,0)==2);mockNow=10099;
 assert(readFrom(&backwards,nullptr,bytes,sizeof(bytes),2)<0);
 // Returning the clock forward cannot reopen a session closed by regression.
 mockNow=10101;closedReply(backwards,backwardsToken);poll();disconnectedAndApplied(backwards);checkNewPolicy(cfg);
 puts("PASS admin barrier: 64-bit absolute completion deadline, notification non-drain, deadline-capped grace and latched clock-regression failure");
}

static void adminNoReplyAndAuthorityLoss(){
 const auto cfg=mt::BluetoothSettings::defaults();auto disabled=cfg;disabled.enabled=false;
 bt_conn missing;start(missing,cfg,20000);const auto before=readCounter(missing);const auto missingToken=begin(missing);
 finish(disabled,missingToken);
 closedReply(missing,missingToken);assert(uint32_t(atomic_get(&counter))==before);
 poll();disconnectedAndApplied(missing);checkNewPolicy(disabled);

 bt_conn prohibited;start(prohibited,cfg,21000);const auto prohibitedCounter=readCounter(prohibited);const auto prohibitedToken=begin(prohibited);stage(prohibitedToken);
 const auto next=fixedSettings();finish(next,prohibitedToken,false);
 closedReply(prohibited,prohibitedToken);assert(uint32_t(atomic_get(&counter))==prohibitedCounter);
 poll();disconnectedAndApplied(prohibited);checkNewPolicy(next);

 bt_conn lost;start(lost,cfg,22000);const auto lostToken=begin(lost);stage(lostToken);
 auto invalid=cfg;invalid.mode=99;
 const auto calls=mockStackCalls;assert(finishAdminConfiguration(invalid,false,lostToken,true));assert(mockStackCalls==calls);
 // The original secured client may read one captured error result even if the
 // next authoritative settings are unavailable. Only that bounded lane survives.
 uint8_t bytes[16]{};blockedOrdinary(lost,lostToken);
 assert(readFrom(&lost,nullptr,bytes,sizeof(bytes),0)==ssize_t(adminBytes));
 mockNow=22100;poll();closedReply(lost,lostToken);
 disconnectedAndApplied(lost);assert(!ready()&&!mockAdvertising);
 bt_conn denied;onConnected(&denied,0);assert(!denied.auth);
 secure(denied,BT_SECURITY_L3);assert(readFrom(&denied,nullptr,bytes,sizeof(bytes),0)<0);
 assert(!beginAdmin(session()));detach(denied);poll();configure(cfg);
 puts("PASS admin barrier: missing/disallowed reply still hands off authoritative policy; loss of settings authority remains closed");
}

static void adminDisconnectAndSecurity(){
 const auto cfg=mt::BluetoothSettings::defaults();const auto next=fixedSettings();
 bt_conn committing;start(committing,cfg,30000);const auto token=begin(committing);
 detach(committing);assert(!stageAdminResult(adminFrame,sizeof(adminFrame),token));
 // The accepted transaction still installs its authoritative result after the
 // original client disconnects; no reply or replacement client is substituted.
 assert(finishAdminConfiguration(next,true,token,true));
 bt_conn early;onConnected(&early,0);assert(!early.auth);uint8_t bytes[16]{};
 secure(early,BT_SECURITY_L3);assert(readFrom(&early,nullptr,bytes,sizeof(bytes),0)<0);
 assert(!beginAdmin(session()));detach(early);poll();checkNewPolicy(next);
 assert(!stageAdminResult(adminFrame,sizeof(adminFrame),token));
 assert(!finishAdminConfiguration(cfg,true,token,true));

 bt_conn waiting;start(waiting,cfg,31000);const auto oldToken=begin(waiting);stage(oldToken);finish(next,oldToken);
 detach(waiting);bt_conn stale;onConnected(&stale,0);assert(!stale.auth);
 secure(stale,BT_SECURITY_L3);assert(!send(framed,sizeof(framed),oldToken));
 assert(readFrom(&stale,nullptr,bytes,sizeof(bytes),0)<0);detach(stale);poll();checkNewPolicy(next);

 bt_conn downgraded;start(downgraded,cfg,32000);const auto downgradeToken=begin(downgraded);stage(downgradeToken);finish(next,downgradeToken);
 secure(downgraded,BT_SECURITY_L1);assert(readFrom(&downgraded,nullptr,bytes,sizeof(bytes),0)<0);
 secure(downgraded,BT_SECURITY_L2);closedReply(downgraded,downgradeToken);
 const auto notifications=mockNotifies;ccc(nullptr,1);poll();assert(mockNotifies==notifications);
 disconnectedAndApplied(downgraded);checkNewPolicy(next);

 bt_conn actual;start(actual,cfg,33000);const auto actualToken=begin(actual);stage(actualToken);finish(next,actualToken);
 // The actual stack level accessor is also checked, independently of callback
 // history. An observed level loss must latch closed on later restoration.
 actual.level=BT_SECURITY_L1;assert(readFrom(&actual,nullptr,bytes,sizeof(bytes),0)<0);
 actual.level=BT_SECURITY_L2;closedReply(actual,actualToken);poll();disconnectedAndApplied(actual);checkNewPolicy(next);

 bt_conn duringCommit;start(duringCommit,cfg,34000);const auto commitToken=begin(duringCommit);
 secure(duringCommit,BT_SECURITY_L1);assert(!stageAdminResult(adminFrame,sizeof(adminFrame),commitToken));
 secure(duringCommit,BT_SECURITY_L2);assert(!stageAdminResult(adminFrame,sizeof(adminFrame),commitToken));
 // A revoked reply epoch still accepts the authoritative transaction outcome;
 // it must hand off the next policy without regaining a reply capability.
 assert(finishAdminConfiguration(next,true,commitToken,true));closedReply(duringCommit,commitToken);
 poll();disconnectedAndApplied(duringCommit);checkNewPolicy(next);

 bt_conn actualDuringCommit;start(actualDuringCommit,cfg,35000);const auto actualCommitToken=begin(actualDuringCommit);
 actualDuringCommit.level=BT_SECURITY_L1;
 assert(!stageAdminResult(adminFrame,sizeof(adminFrame),actualCommitToken));
 actualDuringCommit.level=BT_SECURITY_L2;
 assert(!stageAdminResult(adminFrame,sizeof(adminFrame),actualCommitToken));
 assert(finishAdminConfiguration(next,true,actualCommitToken,true));closedReply(actualDuringCommit,actualCommitToken);
 poll();disconnectedAndApplied(actualDuringCommit);checkNewPolicy(next);
 puts("PASS admin barrier: commit after disconnect, blocked early reconnect, stale tokens, callback and actual-level security loss without reopening");
}

static void adminExplicitOverride(){
 const auto cfg=mt::BluetoothSettings::defaults();const auto next=fixedSettings();
 bt_conn waiting;start(waiting,cfg,40000);const auto token=begin(waiting);stage(token);finish(next,token);
 // Even an override equal to the original active policy aborts the captured
 // reply window and cannot leave the old session operating normally.
 const auto calls=mockStackCalls;assert(requestConfiguration(cfg,true));assert(mockStackCalls==calls);
 closedReply(waiting,token);assert(!stageAdminResult(adminFrame,sizeof(adminFrame),token));
 assert(!finishAdminConfiguration(next,true,token,true));poll();disconnectedAndApplied(waiting);checkNewPolicy(cfg);

 bt_conn committing;start(committing,cfg,41000);const auto committingToken=begin(committing);stage(committingToken);
 auto disabled=cfg;disabled.enabled=false;assert(requestConfiguration(disabled,true));
 closedReply(committing,committingToken);assert(!finishAdminConfiguration(next,true,committingToken,true));
 poll();disconnectedAndApplied(committing);checkNewPolicy(disabled);
 assert(sessionLockDepth==0);
 puts("PASS admin barrier: explicit configuration overrides close both committing and reply-waiting epochs, including same-original-policy override");
}

#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
static void stockImportTransport() {
  for (unsigned scenario = 0; scenario < 3; ++scenario) {
    auto cfg = mt::BluetoothSettings::defaults();
    if (scenario) {
      cfg.mode = mt::BluetoothSettings::FixedPin;
      cfg.fixedPin = scenario == 1 ? 123456 : 654321;  // Synthetic fixture.
    }
    bt_conn link;
    start(link, cfg, mockNow + 1);
    const auto generation = session();
    const bool allowed = scenario == 2;
    assert(stockImportLink(generation) == allowed && !stockImportLink(0));
    uint8_t data = 0x42;
    assert(writeTo(&link, nullptr, &data, 1, 0, 0) == 1);
    Packet packet{};
    assert(receive(packet) && packet.stockImportAuthenticated == allowed);
    assert(packet.session == generation && packet.length == 1 && packet.bytes[0] == data);
    for (const auto &slot : inputSlots) {
      const auto *memory = reinterpret_cast<const uint8_t *>(&slot);
      for (size_t i = 0; i < sizeof(slot); ++i) assert(memory[i] == 0);
    }
    if (allowed) {
      link.level = BT_SECURITY_L2;
      assert(!stockImportLink(generation));
    }
    detach(link);
    poll();
    assert(!stockImportLink(generation));
  }
  puts("PASS import requires authenticated nondefault PIN at write and use; queue releases input bytes");
}
#endif

int main(int argc,char**argv){
 // A named group helps isolate a failing production path without skipping the
 // ordinary transport setup. The receipt-producing runner executes all groups.
 assert(argc<=2);const char*group=argc==2?argv[1]:"all";
 const bool all=strcmp(group,"all")==0;
 const bool admission=all||strcmp(group,"admission")==0;
 const bool clock=all||strcmp(group,"clock")==0;
 const bool authority=all||strcmp(group,"authority")==0;
 const bool security=all||strcmp(group,"security")==0;
 const bool overridePolicy=all||strcmp(group,"override")==0;
 assert(admission||clock||authority||security||overridePolicy);
 legacyCases();if(admission)adminAdmissionAndCoverage();if(clock)adminDeadlineAndClock();
 if(authority)adminNoReplyAndAuthorityLoss();if(security){missingKeyRejectionCases();adminDisconnectAndSecurity();}if(overridePolicy)adminExplicitOverride();
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
 if (all) stockImportTransport();
#endif
 assert(sessionLockDepth==0);puts("PASS selected actual BLE callback and M103 admin-barrier cases (stack doubles only)");
}
