// SPDX-License-Identifier: GPL-3.0-only
// Included after endpoint_fixture.h. Actual Service/Companion, RAM radio only.
namespace radio_diagnostic_test {
struct CounterRadio:Radio {
  unsigned configured=0,started=0,stopped=0,healthReads=0,completionReads=0;
  mutable unsigned snapshots=0;
  int configureStatus=0,startStatus=0,restoredHealth=-1;
  bool nativeKnown=true,changeOnStop=true;
  mt::NativeStages current{{7,6,16,-EIO},{5,4,3,-ENODEV},{3,2,1,-EACCES}};
  bool diagnostic(mt::NativeStages &out)const override{++snapshots;out=nativeKnown?current:mt::NativeStages{};return nativeKnown;}
  int configure(const mt::Config &)override{++configured;return configureStatus;}
  int start()override{++started;return startStatus;}
  int stop()override{++stopped;if(changeOnStop)current={};return Radio::stop();}
  int restore()override{const int ret=Radio::restore();if(!ret&&restoredHealth>=0)healthStatus=restoredHealth;return ret;}
  int health()override{++healthReads;return Radio::health();}
  bool completion(int &out)override{++completionReads;return Radio::completion(out);}
};
struct Rig {
  CounterRadio radio;mt::Service service;uint64_t clock=0;
  static uint64_t readClock(void *ctx){return *static_cast<uint64_t *>(ctx);}
  explicit Rig(bool run=true,bool normal=true):service(radio,true,OURS,81,normal,false){
    assert(service.channel_.bind({readClock,&clock}));service.channel_.supported(true);
    mt::Config config;config.bytes[4]=1;auto node=config.node();node.setPublic();config.node(node);
    assert(service.apply(config));if(run)assert(service.start(false,0)&&service.armEndpoint(true));
  }
  bench::Frame frame(uint32_t id=101){
    meshtastic_Data data{};data.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;data.has_bitfield=true;
    data.payload.size=1;data.payload.bytes[0]='D';uint8_t plain[32]{};auto out=pb_ostream_from_buffer(plain,sizeof(plain));
    assert(pb_encode(&out,meshtastic_Data_fields,&data));bench::Frame wire{};size_t size=0;
    assert(service.codec.encodeZeroHopData(OURS,PEER,id,plain,out.bytes_written,wire.bytes,sizeof(wire.bytes),size));wire.len=size;return wire;
  }
  void queued(){const auto wire=frame();assert(!service.submitMessage(0,wire.bytes,wire.len,false,100));}
};
static bool sameStage(const mt::NativeStage &a,const mt::NativeStage &b){
  return a.calls==b.calls&&a.failedCall==b.failedCall&&a.stage==b.stage&&a.result==b.result;
}
static bool sameNative(const mt::NativeStages &a,const mt::NativeStages &b){
  return sameStage(a.send,b.send)&&sameStage(a.config,b.config)&&sameStage(a.restart,b.restart);
}
static void fault(const Rig &rig,mt::FaultStage stage,mt::DiagnosticDomain domain,int32_t value){
  const auto diag=rig.service.radioDiagnostic();assert(diag.faults==1&&diag.faultStage==uint16_t(stage)&&
    diag.faultDomain==domain&&diag.faultValue==value&&diag.nativeKnown);
  assert(rig.service.faulted()&&!rig.service.receiving()&&!rig.service.active()&&!rig.service.endpointArmed());
}
static void sticky_before_cleanup(){
  Rig rig(false);rig.radio.configureStatus=-EINVAL;const auto expected=rig.radio.current;
  assert(!rig.service.start(false,0));fault(rig,mt::FaultStage::Configure,mt::DiagnosticDomain::Errno,-EINVAL);
  assert(rig.radio.configured==1&&rig.radio.started==0&&rig.radio.healthReads==0&&rig.radio.stopped==1);
  assert(sameNative(rig.service.radioDiagnostic().native,expected)&&!sameNative(rig.radio.current,expected));
  const auto saved=rig.service.radioDiagnostic();const auto reads=rig.radio.snapshots;
  rig.radio.stopStatus=-EIO;rig.service.stop();rig.service.stop();rig.service.poll(300,1);
  assert(!rig.service.start(false,400));const auto after=rig.service.radioDiagnostic();
  assert(rig.radio.snapshots==reads&&after.faults==1&&after.faultStage==saved.faultStage&&
    after.faultDomain==saved.faultDomain&&after.faultValue==saved.faultValue&&after.faultFlags==saved.faultFlags&&sameNative(after.native,expected));
  group("radio diagnostics freeze first native tuple before stop and retain it through repeated cleanup faults and getters");
}
static void start_order_and_health(){
  for(unsigned mode=0;mode<3;++mode){Rig rig(false);
    if(mode==0)rig.radio.configureStatus=-EIO;else if(mode==1)rig.radio.startStatus=-ENODEV;else rig.radio.healthStatus=0;
    assert(!rig.service.start(false,0));assert(rig.radio.configured==1&&rig.radio.started==(mode?1u:0u)&&rig.radio.healthReads==(mode==2?1u:0u));
    fault(rig,mode==0?mt::FaultStage::Configure:mode==1?mt::FaultStage::Start:mt::FaultStage::StartHealth,
      mode==2?mt::DiagnosticDomain::Health:mt::DiagnosticDomain::Errno,mode==0?-EIO:mode==1?-ENODEV:0);
  }
  group("radio configure start health short-circuit in order and expected-one actual-zero health remains an explicit failure");
}
static void send_and_busy_restore(){
  for(unsigned mode=0;mode<4;++mode){Rig rig;rig.queued();rig.radio.sendStatus=mode?-EBUSY:-EIO;
    if(mode==1)rig.radio.restoreStatus=-ENODEV;
    if(mode==2)rig.radio.restoredHealth=0;
    const auto expected=rig.radio.current;rig.service.poll(300,1);assert(rig.radio.sends==1&&!rig.service.active());
    if(mode==3){assert(!rig.service.faulted()&&rig.service.endpointArmed()&&rig.service.endpointFree()==3&&rig.radio.restoreCalls==1);
      assert(!rig.service.radioDiagnostic().faults);rig.service.stop();continue;}
    fault(rig,mode==0?mt::FaultStage::Send:mode==1?mt::FaultStage::BusyRestore:mt::FaultStage::BusyRestoreHealth,
      mode==2?mt::DiagnosticDomain::Health:mt::DiagnosticDomain::Errno,mode==0?-EIO:mode==1?-ENODEV:0);
    assert(rig.radio.restoreCalls==(mode?1u:0u)&&rig.service.endpointFree()==4&&!rig.service.localStats().packetsTx&&sameNative(rig.service.radioDiagnostic().native,expected));
  }
  group("hard send or busy restore failures cancel work with exact cause while healthy CAD-busy restoration retains bounded retry");
}
static void completion_boundaries(){
  for(unsigned mode=0;mode<4;++mode){Rig rig;rig.queued();rig.service.poll(300,1);
    assert(rig.service.active()&&rig.radio.sends==1&&rig.service.endpointFree()==3);
    const auto expected=rig.radio.current;
    if(mode==0){rig.radio.done=true;rig.radio.doneStatus=-EIO;}
    if(mode==1){rig.radio.done=true;rig.radio.restoreStatus=-ENODEV;}
    if(mode==2){rig.radio.done=true;rig.radio.restoredHealth=0;}
    mt::RadioTiming::Deadline timing;assert(mt::RadioTiming::resolve(rig.radio.air,timing));
    if(mode==3){rig.service.poll(300+timing.hostMs-1,1);assert(rig.service.active()&&!rig.service.faulted());}
    rig.service.poll(mode==3?300+timing.hostMs:400,1);
    fault(rig,mode==0?mt::FaultStage::CompletionStatus:mode==1?mt::FaultStage::Restore:mode==2?mt::FaultStage::RestoreHealth:mt::FaultStage::CompletionTimeout,
      mode==2?mt::DiagnosticDomain::Health:mt::DiagnosticDomain::Errno,mode==0?-EIO:mode==1?-ENODEV:mode==2?0:-ETIMEDOUT);
    assert(rig.service.endpointFree()==4&&rig.radio.pending==nullptr&&rig.radio.sends==1&&
      rig.radio.restoreCalls==((mode==1||mode==2)?1u:0u)&&rig.service.localStats().packetsTx==((mode==1||mode==2)?1u:0u));
    assert(sameNative(rig.service.radioDiagnostic().native,expected));rig.service.poll(100000,1);assert(rig.radio.sends==1);
  }
  group("completion errno exact host deadline and restore errors preserve cancellation and credit only known completed TX");
}
static void submit_history_and_saturation(){
  Rig rig(false);auto wire=rig.frame();assert(rig.service.submitMessage(0,wire.bytes,wire.len,false,100)==-EACCES);
  assert(rig.service.start(false,0)&&rig.service.armEndpoint(true));
  assert(rig.service.submitMessage(0,nullptr,0,false,100)==-EINVAL);
  assert(rig.service.submitMessage(2,wire.bytes,wire.len,false,100)==-EINVAL);
  assert(!rig.service.submitMessage(0,wire.bytes,wire.len,false,100)&&!rig.service.submitMessage(1,wire.bytes,wire.len,false,101));
  wire.bytes[wire.len-1]^=1;assert(rig.service.submitMessage(0,wire.bytes,wire.len,false,102)==-EINVAL);
  auto diag=rig.service.radioDiagnostic();assert(diag.submits==6&&diag.rejected==4&&diag.rejectStage==3&&
    diag.rejectDomain==mt::DiagnosticDomain::Errno&&diag.rejectValue==-EINVAL&&!diag.faults&&rig.service.endpointFree()==3);
  rig.service.noteOriginReject(4,mt::DiagnosticDomain::Routing,meshtastic_Routing_Error_NO_INTERFACE);
  rig.service.stop();diag=rig.service.radioDiagnostic();assert(diag.submits==6&&diag.rejected==5&&diag.rejectStage==4&&
    diag.rejectDomain==mt::DiagnosticDomain::Routing&&diag.rejectValue==meshtastic_Routing_Error_NO_INTERFACE&&!diag.faults);
  uint32_t count=UINT32_MAX-1;mt::RadioDiagnostic::bump(count);mt::RadioDiagnostic::bump(count);assert(count==UINT32_MAX);
  count=0;mt::RadioDiagnostic::bump(count);assert(count==1);
  group("submission and last rejection history count failures and duplicate owners without resetting on stop; counter primitive saturates");
}
static void local_log_bounds(){
  Rig rig;const mt::NativeStage maximum{UINT32_MAX,UINT32_MAX,UINT32_MAX,INT32_MIN};
  rig.radio.current={maximum,maximum,maximum};rig.service.noteOriginReject(UINT16_MAX,mt::DiagnosticDomain::Errno,INT32_MIN);
  Host host(rig.service);mt::companion::CompanionWorkspace workspace;mt::Companion client(rig.service,host,81,workspace,0);meshtastic_ToRadio config{};
  assert(!host.safeApiLogEnabled());
  config.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;config.want_config_id=71;input(client,config,1);
  unsigned stages=0,services=0;
  for(const auto &out:host.out)if(out.which_payload_variant==meshtastic_FromRadio_log_record_tag){
    const auto &log=out.log_record;const bool isStage=!strncmp(log.message,"radio_stage ",12),isService=!strncmp(log.message,"radio_service ",14);
    if(!isStage&&!isService)continue;
    assert(memchr(log.message,0,sizeof(log.message))&&strlen(log.message)<sizeof(log.message)&&
      !strcmp(log.source,"ZephTastic")&&log.level==meshtastic_LogRecord_Level_INFO);
    if(isStage)++stages;else ++services;
  }
  assert(stages==0&&services==0&&!rig.radio.sends&&!rig.radio.stopped&&!rig.radio.restoreCalls);
  group("ordinary local handshake emits no legacy extended radio diagnostics without safe-log consent");
}
static void legacy_and_unknown(){
  Radio legacy;mt::NativeStages out{{1,2,3,4},{1,2,3,4},{1,2,3,4}};
  assert(!legacy.diagnostic(out)&&sameNative(out,{}));
  Rig rig(true,false);rig.radio.healthStatus=0;rig.service.poll(300,1);
  fault(rig,mt::FaultStage::Unknown,mt::DiagnosticDomain::Unknown,0);
  group("legacy unclassified fault stays explicitly unknown and unimplemented native metadata defaults unavailable without invented stages");
}
}
static void radioDiagnosticCases(){
  using namespace radio_diagnostic_test;
  sticky_before_cleanup();start_order_and_health();send_and_busy_restore();completion_boundaries();
  submit_history_and_saturation();local_log_bounds();legacy_and_unknown();
}
