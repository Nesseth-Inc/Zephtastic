// Real controller/Service/Endpoint; synthetic radio and explicit local clock.
static mt::ModuleSettings controllerSettings(bool telemetry,bool neighbor=false){
 mt::ModuleSettings s;meshtastic_ModuleConfig c{};c.which_payload_variant=meshtastic_ModuleConfig_telemetry_tag;
 c.payload_variant.telemetry.device_telemetry_enabled=telemetry;c.payload_variant.telemetry.device_update_interval=60;assert(s.set(c));
 c={};c.which_payload_variant=meshtastic_ModuleConfig_neighbor_info_tag;c.payload_variant.neighbor_info.enabled=neighbor;c.payload_variant.neighbor_info.transmit_over_lora=true;assert(s.set(c));return s;
}
static void controllerStart(Fixture&f,unsigned role=1){
 f.service.stop();auto c=f.service.config;c.bytes[4]=1;c.setTxEnabled(true);assert(c.setDeviceRole(role,0)&&f.service.apply(c));
 assert(f.service.start(false,0)&&f.service.armEndpoint(true));
}
static void controllerClear(Fixture&f){f.service.stop();assert(f.service.endpointControls()==0);assert(f.service.start(false,0)&&f.service.armEndpoint(true));}
static unsigned controllerPoll(mt::ModuleController&m,Fixture&f,const mt::ModuleSettings&s,uint64_t now,meshtastic_NeighborInfo *last=nullptr){
 if(now!=UINT64_MAX&&now>=f.knownChannelClock.now()){assert(f.knownChannelClock.set(now));}
 unsigned neighbors=0;
 m.poll(f.service,s,true,now,uint32_t(now)^0xabc23456,[&](const uint8_t*p,size_t n,uint32_t id){
  assert(id);meshtastic_Data d{};auto stream=pb_istream_from_buffer(p,n);assert(pb_decode(&stream,meshtastic_Data_fields,&d));
  if(d.portnum==meshtastic_PortNum_NEIGHBORINFO_APP){++neighbors;if(last)assert(mt::NeighborInfoPayload::decode(d.payload.bytes,d.payload.size,*last));}
 });return neighbors;
}
static void controllerRuntimeCases(){
 {
  auto fixture=std::make_unique<Fixture>();mt::ModuleController controller;
  controllerStart(*fixture);fixture->service.stop();
  auto cfg=fixture->service.config;auto request=cfg.radioRequest();
  request.regionCode=29;request.usePreset=true;request.preset=10;request.channel=0;
  assert(cfg.setRadio(request) && fixture->service.apply(cfg));
  uint32_t air=0,health=0;
  assert(controller.sensorInterval(fixture->service,0,1000,true,air)&&air==3600000);
  assert(controller.sensorInterval(fixture->service,0,1000,false,health)&&health==10*air);
  for(uint32_t id=300;id<355;++id) controller.observe(id,Node,0,2000,true,1);
  assert(controller.sensorInterval(fixture->service,0,2000,true,air)&&air>3600000);
  assert(controller.sensorInterval(fixture->service,0,2000,false,health)&&health==10*air);
  for(unsigned role:{2u,11u}) {
    assert(cfg.setDeviceRole(role,0)&&fixture->service.apply(cfg));
    assert(controller.sensorInterval(fixture->service,0,3000,true,air)&&air==43200000);
    assert(controller.sensorInterval(fixture->service,0,3000,false,health)&&health==432000000);
  }
 }

 {
  mt::OnlineObservations table;mt::NodeCache cache;uint32_t count=777;
  assert(table.count(cache,Node,0,count)&&count==1);
  for(uint32_t id:{0u,1u,Node,0xfffffffeu,UINT32_MAX})assert(!table.observe(id,Node,1));
  assert(table.observe(77,Node,10)&&table.observe(77,Node,10)&&table.count(cache,Node,10,count)&&count==2);
  assert(table.count(cache,Node,mt::OnlineObservations::Lifetime+9,count)&&count==2);
  assert(table.count(cache,Node,mt::OnlineObservations::Lifetime+10,count)&&count==1);
  assert(!table.count(cache,Node,UINT64_MAX,count)&&count==1);
  mt::OnlineObservations clocks;assert(clocks.observe(78,Node,1000)&&clocks.count(cache,Node,2000,count)&&count==2);
  count=777;assert(!clocks.count(cache,Node,1999,count)&&count==777&&!clocks.observe(79,Node,1999));
  assert(clocks.count(cache,Node,2000,count)&&count==2);assert(cache.edit(78,Node,mt::NodeCache::Edit::Ignore)>=0);
  assert(clocks.count(cache,Node,2000,count)&&count==1);assert(cache.edit(78,Node,mt::NodeCache::Edit::Unignore)>=0);
  assert(clocks.count(cache,Node,2000,count)&&count==1); // Ignored observations do not resurrect.
  mt::OnlineObservations bounded;for(uint32_t id=100;id<200;++id)assert(bounded.observe(id,Node,id));
  assert(bounded.count(cache,Node,200,count)&&count==65&&bounded.observe(199,Node,200)&&bounded.count(cache,Node,200,count)&&count==65);
  std::cout<<"ONLINE_PHENOTYPE known_self=1 capacity=64 age_ms="<<mt::OnlineObservations::Lifetime<<" unavailable=no_value rollback_retains_highwater=1\n";
  group("observed online64 RAM peers plus self: duplicates, exact2h expiry, ignore, unavailable and rollback preserve honest availability");
 }
 {
  auto f=std::make_unique<Fixture>();controllerStart(*f);mt::ModuleController m;const auto settings=controllerSettings(true);
  controllerPoll(m,*f,settings,0);controllerPoll(m,*f,settings,29999);assert(f->service.endpointControls()==0);
  controllerPoll(m,*f,settings,30000);assert(f->service.endpointControls()==1&&!f->radio.sends);controllerClear(*f);
  controllerPoll(m,*f,settings,89999);assert(!f->service.endpointControls());controllerPoll(m,*f,settings,90000);assert(f->service.endpointControls()==1);
  group("device index0 first check30s then minute cadence uses real Service module queue without RF-delivery claims");
 }
 {
  for(unsigned role:{0u,1u,2u}){auto f=std::make_unique<Fixture>();controllerStart(*f,role);mt::ModuleController m;const auto settings=controllerSettings(true);
   controllerPoll(m,*f,settings,0);assert(f->knownChannelClock.set(30000));f->service.channelMeter().observeRx(18000,true);
   controllerPoll(m,*f,settings,30000);assert(f->service.endpointControls()==unsigned(role==2));
   if(role!=2){controllerPoll(m,*f,settings,90000);assert(f->service.endpointControls()==1);}assert(!f->radio.sends);}
  auto f=std::make_unique<Fixture>();controllerStart(*f);mt::ModuleController m;const auto settings=controllerSettings(true);
  controllerPoll(m,*f,settings,0);f->service.channelMeter().supported(false);controllerPoll(m,*f,settings,30000);assert(!f->service.endpointControls());
  f->service.channelMeter().supported(true);controllerPoll(m,*f,settings,90000);assert(f->service.endpointControls()==1);
  group("client and mute25/router40 channel thresholds and unsupported samples do not consume eligible history");
 }
 {
  auto f=std::make_unique<Fixture>();controllerStart(*f);mt::ModuleController m;auto settings=controllerSettings(true);
  meshtastic_ModuleConfig c{};c.which_payload_variant=meshtastic_ModuleConfig_telemetry_tag;c.payload_variant.telemetry.device_telemetry_enabled=true;c.payload_variant.telemetry.device_update_interval=120;assert(settings.set(c));
  controllerPoll(m,*f,settings,0);controllerPoll(m,*f,settings,30000);assert(f->service.endpointControls()==1);
  controllerPoll(m,*f,settings,150000);assert(f->service.endpointControls()==1); // Pending queue rejects second attempt; branch still recorded.
  controllerClear(*f);controllerPoll(m,*f,settings,210000);assert(!f->service.endpointControls());
  controllerPoll(m,*f,settings,270000);assert(f->service.endpointControls()==1);controllerClear(*f);
  controllerPoll(m,*f,settings,UINT64_MAX);controllerPoll(m,*f,settings,269999);assert(!f->service.endpointControls());
  controllerPoll(m,*f,settings,330000);assert(!f->service.endpointControls());controllerPoll(m,*f,settings,390000);assert(f->service.endpointControls()==1&&!f->radio.sends);
  group("stock eligible branch consumes history even when queue busy; unknown or rolled-back clocks do not reset it");
 }
 {
  auto f=std::make_unique<Fixture>();controllerStart(*f);mt::ModuleController m;auto off=controllerSettings(false),on=controllerSettings(true);
  controllerPoll(m,*f,off,0);for(uint32_t id=100;id<164;++id){m.observe(id,Node,1,1,true,-2);m.observe(id,Node,1,1,true,-2);}
  controllerPoll(m,*f,on,2);controllerPoll(m,*f,on,30002);assert(f->service.endpointControls()==1);controllerClear(*f);
  controllerPoll(m,*f,on,90002);assert(!f->service.endpointControls());
  auto changed=f->service.config;assert(changed.setLoRa(6,20));f->service.stop();assert(f->service.apply(changed));assert(f->service.start(false,100000));assert(f->service.armEndpoint(true));
  assert(f->radio.configuredProfile.spreadingFactor==7&&f->radio.configuredProfile.bandwidthHz==250000);
  controllerPoll(m,*f,on,150002);assert(f->service.endpointControls()==1&&!f->radio.sends&&f->backend.writes==0);
  group("online observation continues with Neighbor disabled; duplicate relayed sources stay bounded and current modem recomputes interval");
 }
 {
  for(unsigned role:{1u,2u}){auto f=std::make_unique<Fixture>();controllerStart(*f,role);mt::ModuleController m;const auto settings=controllerSettings(false,true);
   controllerPoll(m,*f,settings,0);meshtastic_NeighborInfo empty{};assert(m.localNeighbors(f->service,settings,true,0,empty)==0&&!empty.neighbors_count&&empty.node_broadcast_interval_secs==(role==2?43200u:3600u));
   const uint64_t first=role==2?43200000:3600000;assert(!controllerPoll(m,*f,settings,first)); // Available empty does not become periodic packet.
   assert(m.localNeighbors(f->service,settings,true,first-1,empty)==-EACCES&&!empty.node_id);
   assert(m.localNeighbors(f->service,settings,true,first,empty)==0&&empty.node_id==Node&&!empty.neighbors_count);
   m.observe(77,Node,0,first+1,true,-2.5f);meshtastic_NeighborInfo info{};const auto next=first+21600000;
   m.observe(77,Node,0,next,true,-2.5f);assert(controllerPoll(m,*f,settings,next,&info)==1&&info.neighbors_count==1&&info.node_broadcast_interval_secs==(role==2?43200u:3600u));
   assert(!f->service.endpointControls()&&!f->radio.sends&&f->backend.writes==0);}
  group("Neighbor initial role defaults, steady6h and advertised seconds stay distinct; periodic empty suppressed and default key phone-only");
 }
 {
  for(unsigned before:{0u,2u}){auto f=std::make_unique<Fixture>();controllerStart(*f,before);mt::ModuleController m;const auto settings=controllerSettings(false,true);
   controllerPoll(m,*f,settings,0);m.observe(77,Node,0,1,true,-2.5f);
   const unsigned after=before==2?0:2;controllerStart(*f,after);assert(!controllerPoll(m,*f,settings,2));
   meshtastic_NeighborInfo info{};assert(m.localNeighbors(f->service,settings,true,2,info)==0&&info.neighbors_count==1);
   assert(info.node_broadcast_interval_secs==(after==2?43200u:3600u));
   if(after==2)assert(!controllerPoll(m,*f,settings,3600000)); // Retire the old CLIENT deadline.
   const uint64_t deadline=2+(after==2?43200000u:3600000u);
   assert(!controllerPoll(m,*f,settings,deadline-1));
   m.observe(77,Node,0,deadline,true,-2.5f);
   assert(controllerPoll(m,*f,settings,deadline,&info)==1&&info.neighbors_count==1&&info.neighbors[0].node_id==77);
   assert(info.node_broadcast_interval_secs==(after==2?43200u:3600u)&&!f->radio.sends&&f->backend.writes==0);
  }
  group("same controller and enabled settings rebase CLIENT to ROUTER and ROUTER to CLIENT Neighbor deadlines without clearing observations");
 }
 {
  auto f=std::make_unique<Fixture>();controllerStart(*f,0);mt::ModuleController m;auto settings=controllerSettings(true,true);
  meshtastic_ModuleConfig c{};c.which_payload_variant=meshtastic_ModuleConfig_neighbor_info_tag;c.payload_variant.neighbor_info.enabled=true;c.payload_variant.neighbor_info.update_interval=14400;assert(settings.set(c));
  c={};c.which_payload_variant=meshtastic_ModuleConfig_telemetry_tag;c.payload_variant.telemetry.device_telemetry_enabled=true;c.payload_variant.telemetry.device_update_interval=120;assert(settings.set(c));
  controllerPoll(m,*f,settings,0);m.observe(77,Node,0,1,true,-2);controllerStart(*f,2);controllerPoll(m,*f,settings,1000);
  controllerPoll(m,*f,settings,30000);assert(f->service.endpointControls()==1);controllerClear(*f); // Device first check remains30s.
  meshtastic_NeighborInfo info{};assert(m.localNeighbors(f->service,settings,true,30000,info)==0&&info.neighbors_count==1&&info.node_broadcast_interval_secs==14400);
  controllerStart(*f,0);assert(!controllerPoll(m,*f,settings,62000));
  controllerPoll(m,*f,settings,90000);assert(!f->service.endpointControls()); // Retain eligible mesh history at30s.
  controllerPoll(m,*f,settings,150000);assert(f->service.endpointControls()==1&&!f->radio.sends&&f->backend.writes==0);
  assert(!controllerPoll(m,*f,settings,14400000));assert(!controllerPoll(m,*f,settings,14461999));
  assert(controllerPoll(m,*f,settings,14462000,&info)==1&&info.neighbors_count==1&&info.node_broadcast_interval_secs==14400);
  group("configured Neighbor interval overrides both roles while role changes preserve DeviceMetrics cadence and eligible history");
 }
 {
  auto f=std::make_unique<Fixture>();controllerStart(*f,0);mt::ModuleController m;auto settings=controllerSettings(false,true);
  meshtastic_ModuleConfig c{};c.which_payload_variant=meshtastic_ModuleConfig_range_test_tag;c.payload_variant.range_test.enabled=true;c.payload_variant.range_test.sender=10;assert(settings.set(c));
  c={};c.which_payload_variant=meshtastic_ModuleConfig_statusmessage_tag;strcpy(c.payload_variant.statusmessage.node_status,"Retain timers");assert(settings.set(c));
  unsigned ranges=0,statuses=0;std::vector<std::string> sequences;
  auto poll=[&](uint64_t now){assert(f->knownChannelClock.set(now));m.poll(f->service,settings,true,now,uint32_t(now),[&](const uint8_t*p,size_t n,uint32_t){
   meshtastic_Data d{};auto stream=pb_istream_from_buffer(p,n);assert(pb_decode(&stream,meshtastic_Data_fields,&d));
   if(d.portnum==meshtastic_PortNum_NODE_STATUS_APP)++statuses;
   if(d.portnum==meshtastic_PortNum_RANGE_TEST_APP){++ranges;sequences.emplace_back(reinterpret_cast<const char*>(d.payload.bytes),d.payload.size);}
  });};
  poll(0);controllerStart(*f,2);poll(1000);poll(4999);assert(!ranges&&!statuses);poll(5000);assert(ranges==1&&sequences.back()=="seq 1");
  poll(120000);assert(statuses==1&&ranges==2&&sequences.back()=="seq 2");
  controllerStart(*f,0);poll(121000);poll(129999);assert(ranges==2&&statuses==1);poll(130000);
  assert(ranges==3&&sequences.back()=="seq 3"&&statuses==1&&!f->radio.sends&&f->backend.writes==0);
  group("role-only Neighbor reconfiguration preserves Range sequence/start and Status first deadline");
 }
}

static void environmentControllerCases(){
 auto settings=[](uint32_t seconds){mt::ModuleSettings s;meshtastic_ModuleConfig c{};c.which_payload_variant=meshtastic_ModuleConfig_telemetry_tag;c.payload_variant.telemetry.environment_measurement_enabled=true;c.payload_variant.telemetry.environment_update_interval=seconds;assert(s.set(c));return s;};
 for(unsigned role:{0u,1u,2u}){
  auto f=std::make_unique<Fixture>();controllerStart(*f,role);mt::ModuleController m;auto config=settings(0);unsigned copies=0;
  auto poll=[&](uint64_t now,bool measured=true){assert(f->knownChannelClock.set(now));zt::telemetry::EnvironmentReadings r;if(measured){r.ambientCelsius=0.f;r.humidityPercent=50.f;}f->service.measuredEnvironment(r);
   m.poll(f->service,config,true,now,uint32_t(now)^0x123abcdef,[&](const uint8_t*p,size_t n,uint32_t id){
    assert(id);meshtastic_Data d{};auto in=pb_istream_from_buffer(p,n);assert(pb_decode(&in,meshtastic_Data_fields,&d));
    if(d.portnum!=67)return;meshtastic_Telemetry t{};in=pb_istream_from_buffer(d.payload.bytes,d.payload.size);assert(pb_decode(&in,meshtastic_Telemetry_fields,&t));
    if(t.which_variant==3){++copies;assert(!t.time&&t.variant.environment_metrics.has_temperature&&t.variant.environment_metrics.temperature==0&&!t.variant.environment_metrics.has_barometric_pressure);}
   });
  };
  poll(0);poll(54999);assert(!copies&&!f->service.endpointControls());poll(55000);assert(copies==1&&f->service.endpointControls()==1);controllerClear(*f);
  poll(55001);assert(copies==1&&!f->service.endpointControls());poll(115000);assert(copies==2&&!f->service.endpointControls());
  const uint64_t second=55000+(role==2?43200000:3600000);poll(second-1);assert(!f->service.endpointControls());poll(second+999);assert(f->service.endpointControls()==1);controllerClear(*f);
  config=mt::ModuleSettings{};poll(second+1000);const auto before=copies;poll(second+200000);assert(copies==before&&!f->service.endpointControls()&&!f->radio.sends&&!f->backend.writes);
 }
 group("environment scheduling: stagger55s, phone minute, role1h/12h defaults, no catch-up or implicit enabling, measured zero only");
 {
  auto f=std::make_unique<Fixture>();controllerStart(*f);mt::ModuleController m;const auto config=settings(120);zt::telemetry::EnvironmentReadings r;r.pressureHpa=1013.25f;f->service.measuredEnvironment(r);
  controllerPoll(m,*f,config,0);controllerPoll(m,*f,config,55000);assert(f->service.endpointControls()==1);controllerClear(*f);
  controllerPoll(m,*f,config,175000);assert(f->service.endpointControls()==1);controllerClear(*f);
  controllerPoll(m,*f,config,UINT64_MAX);controllerPoll(m,*f,config,174999);assert(!f->service.endpointControls());
  controllerPoll(m,*f,config,235000);assert(!f->service.endpointControls());controllerPoll(m,*f,config,295000);assert(f->service.endpointControls()==1&&!f->radio.sends);
 }
 group("configured environment interval retains mesh history over unknown/backwards clocks and uses one bounded Service control");
 {
  auto f=std::make_unique<Fixture>();controllerStart(*f);mt::ModuleController m;auto config=settings(120);
  controllerPoll(m,*f,config,0);controllerPoll(m,*f,config,55000);assert(!f->service.endpointControls());
  zt::telemetry::EnvironmentReadings r;r.ambientCelsius=20.f;f->service.measuredEnvironment(r);
  controllerPoll(m,*f,config,56000);assert(!f->service.endpointControls());controllerPoll(m,*f,config,175000);assert(f->service.endpointControls()==1&&!f->radio.sends);
 }
 group("no environment sample never becomes invented payload; eligible failed producer retains stock branch history");
}
static void powerControllerCases(){
 auto settings=[](uint32_t seconds){mt::ModuleSettings s;meshtastic_ModuleConfig c{};c.which_payload_variant=meshtastic_ModuleConfig_telemetry_tag;c.payload_variant.telemetry.power_measurement_enabled=true;c.payload_variant.telemetry.power_update_interval=seconds;assert(s.set(c));return s;};
 for(unsigned role:{0u,1u,2u}){
  auto f=std::make_unique<Fixture>();controllerStart(*f,role);mt::ModuleController m;auto config=settings(0);unsigned copies=0;
  auto poll=[&](uint64_t now,bool measured=true){assert(f->knownChannelClock.set(now));zt::telemetry::PowerReadings r;if(measured){r.voltage[0]=0.f;r.current[0]=-125.f;}f->service.measuredPower(r);
   m.poll(f->service,config,true,now,uint32_t(now)^0x123abcdef,[&](const uint8_t*p,size_t n,uint32_t id){
    assert(id);meshtastic_Data d{};auto in=pb_istream_from_buffer(p,n);assert(pb_decode(&in,meshtastic_Data_fields,&d));
    if(d.portnum!=67)return;meshtastic_Telemetry t{};in=pb_istream_from_buffer(d.payload.bytes,d.payload.size);assert(pb_decode(&in,meshtastic_Telemetry_fields,&t));
    if(t.which_variant==5){++copies;assert(!t.time&&t.variant.power_metrics.has_ch1_voltage&&t.variant.power_metrics.ch1_voltage==0&&t.variant.power_metrics.has_ch1_current&&t.variant.power_metrics.ch1_current==-125&&!t.variant.power_metrics.has_ch2_voltage);}
   });
  };
  poll(0);poll(69999);assert(!copies&&!f->service.endpointControls());poll(70000);assert(copies==1&&f->service.endpointControls()==1);controllerClear(*f);
  poll(70001);assert(copies==1&&!f->service.endpointControls());poll(130000);assert(copies==2&&!f->service.endpointControls());
  const uint64_t second=70000+(role==2?43200000:3600000);poll(second-1);assert(!f->service.endpointControls());poll(second+999);assert(f->service.endpointControls()==1);controllerClear(*f);
  config=mt::ModuleSettings{};poll(second+1000);const auto before=copies;poll(second+200000);assert(copies==before&&!f->service.endpointControls()&&!f->radio.sends&&!f->backend.writes);
 }
 group("power scheduling: stagger70s, phone minute, role1h/12h defaults, no catch-up or implicit enabling, measured zero only");
 {
  auto f=std::make_unique<Fixture>();controllerStart(*f);mt::ModuleController m;const auto config=settings(120);zt::telemetry::PowerReadings r;r.voltage[2]=12.5f;f->service.measuredPower(r);
  controllerPoll(m,*f,config,0);controllerPoll(m,*f,config,70000);assert(f->service.endpointControls()==1);controllerClear(*f);
  controllerPoll(m,*f,config,190000);assert(f->service.endpointControls()==1);controllerClear(*f);
  controllerPoll(m,*f,config,UINT64_MAX);controllerPoll(m,*f,config,189999);assert(!f->service.endpointControls());
  controllerPoll(m,*f,config,250000);assert(!f->service.endpointControls());controllerPoll(m,*f,config,310000);assert(f->service.endpointControls()==1&&!f->radio.sends);
 }
 group("configured power interval retains mesh history over unknown/backwards clocks and uses one bounded Service control");
 {
  auto f=std::make_unique<Fixture>();controllerStart(*f);mt::ModuleController m;auto config=settings(120);
  controllerPoll(m,*f,config,0);controllerPoll(m,*f,config,70000);assert(!f->service.endpointControls());
  zt::telemetry::PowerReadings r;r.voltage[0]=3.3f;f->service.measuredPower(r);
  controllerPoll(m,*f,config,71000);assert(!f->service.endpointControls());controllerPoll(m,*f,config,190000);assert(f->service.endpointControls()==1&&!f->radio.sends);
 }
 group("no power sample never becomes invented payload; eligible failed producer retains stock branch history");
}
