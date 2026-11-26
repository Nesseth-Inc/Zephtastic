// SPDX-License-Identifier: GPL-3.0-only
#include "endpoint_fixture.h"
int main() {
  {
    // Endpoint phase model: crypto validation belongs to the separate PSA
    // fixture. A successful TX proof must not consume a pending query.
    auto ep=std::make_unique<mt::Endpoint>();auto fixture=std::make_unique<Fixture>();auto &codec=fixture->s.codec;
    bench::Frame frame;size_t size=0;
    assert(fixture->s.encodeTelemetryQuery(PEER,201,frame.bytes,sizeof(frame.bytes),size));
    frame.len=size;frame.bytes[12]=8;
    for(uint8_t owner:{0,1})assert(!ep->submit(owner,frame.bytes,frame.len,true,100,false,true,true,nullptr,true,false,false,true));
    auto *job=ep->next(100);assert(job);ep->started(*job,0,200,100,0);ep->complete(*job,300,0);
    ep->authenticated(201,PEER,0);
    assert(ep->pendingPrivate(201,PEER)&&!ep->idle());
    mt::Endpoint::Result result;
    for(uint8_t owner:{0,1})assert(ep->take(owner,result)&&result.kind==mt::Endpoint::Kind::Authenticated&&!result.code&&result.id==201);
    ep->authenticated(201,PEER,0);for(uint8_t owner:{0,1})assert(!ep->take(owner,result));
    uint8_t data[40]{};size=0;assert(mt::TelemetryQuery::uptimeResponse(456,201,data,sizeof(data),size));
    bench::Frame response;size_t length=0;assert(codec.encodeZeroHopData(PEER,OURS,5001,data,size,response.bytes,sizeof(response.bytes),length));
    bench::RouteMetadata metadata;assert(codec.decode(response.bytes,length,metadata)&&metadata.request_id==201);
    assert(ep->privateTelemetry(OURS,PEER,OURS,5001,metadata,400)&&ep->idle());
    for(uint8_t owner:{0,1})assert(ep->take(owner,result)&&result.kind==mt::Endpoint::Kind::PrivateResponse&&result.id==201);
    ep->authenticated(201,PEER,0);for(uint8_t owner:{0,1})assert(!ep->take(owner,result));
    group("modeled private query retains application phase after one TX proof for both owners");
  }
  {
    auto f=std::make_unique<Fixture>();f->text();f->s.poll(300,0);assert(f->radio.sends==1);
    auto &wire=f->radio.frames[0];assert(wire[12]==(3|3<<5|8));bench::RouteMetadata m;
    assert(f->s.codec.decode(wire.data(),wire.size(),m)&&m.hasBitfield&&m.portnum==1&&m.payloadLength==5);
    f->done();f->clients();assert(!results(f->h,1)); // Native TX_DONE is not a peer ACK.
    f->rx(f->wire(PEER,OURS,100,1));f->clients();assert(results(f->h,1,0,PEER)==1&&f->s.endpointFree()==4);
    group("direct Public wire and matching peer ACK after native completion");
  }
  {
    auto f=std::make_unique<Fixture>();f->text();
    f->rx(f->wire(PEER,OURS,100,1),200);f->clients();assert(!results(f->h,1));
    f->s.poll(300,0);f->rx(f->wire(PEER+1,OURS,101,1));
    f->rx(f->wire(PEER,OURS+1,102,1));f->rx(f->wire(PEER,OURS,103,2));
    f->clients();assert(!results(f->h,1)&&f->s.endpointFree()==3);
    auto echo=f->radio.frames[0];echo[12]=(3<<5)|2|8;echo[15]=0x33;f->rx(echo);f->clients();assert(!results(f->h,1));
    f->rx(f->wire(PEER,OURS,104,1));assert(f->s.endpointFree()==3); // Still active: do not reuse buffer.
    assert(!memcmp(f->radio.pending,f->radio.frames[0].data(),f->radio.frames[0].size()));
    f->done(600);f->clients();assert(results(f->h,1,0,PEER)==1);
    group("early/wrong-peer/misaddressed/unmatched ACK suppression and active buffer ownership");
  }
  {
    auto f=std::make_unique<Fixture>();f->text();f->s.poll(300,0);f->done();
    f->rx(f->wire(PEER,OURS,101,1,0,8),500);f->clients();assert(results(f->h,1,0,PEER)==1);
    f->s.poll(800,0);assert(f->radio.sends==2);bench::RouteMetadata m;auto &a=f->radio.frames.back();
    assert(a[12]==0&&bench::le32(a.data())==PEER&&f->s.codec.decode(a.data(),a.size(),m)&&m.hasBitfield&&m.request_id==101&&m.routingErrorPresent&&!m.routingError);
    f->done(801);f->rx(f->wire(PEER,OURS,102,bench::le32(a.data()+8),0,8),3000);f->s.poll(3300,0);assert(f->radio.sends==2);
    group("terminal zero-hop ACK-of-ACK includes bitfield and does not loop");
  }
  {
    auto f=std::make_unique<Fixture>();f->text();f->s.poll(300,0);f->done();
    for(unsigned retry=1;retry<5;++retry){
      const unsigned now=18000*retry;f->s.poll(now,0);f->s.poll(now+1000,0);f->done(now+1001);
    }
    f->s.poll(90000,0);f->clients();assert(f->radio.sends==5&&results(f->h,1,5)==1&&f->s.endpointFree()==4);
    for(unsigned i=1;i<5;++i)assert(f->radio.frames[0]==f->radio.frames[i]);
    f->s.poll(1000000,0);assert(f->radio.sends==5);
    group("stock acknowledged unicast has five total attempts; same ID/payload; no sixth retry");
  }
  {
    auto f=std::make_unique<Fixture>();f->text();f->s.poll(300,0);f->done();
    f->rx(f->wire(PEER,OURS,100,1,1));f->clients();assert(results(f->h,1,1)==1);f->s.poll(50000,0);assert(f->radio.sends==1);
    group("matched peer NAK terminates retries");
  }
  {
    auto f=std::make_unique<Fixture>();f->text(1,0xffffffff);f->s.poll(300,0);f->done();
    auto a=f->radio.frames[0];assert(a[12]==(3|3<<5));
    f->rx(a);f->clients();assert(!results(f->h,1));
    a[12]=(3<<5)|2;a[15]=0x55;auto changed=a;changed.back()^=1;f->rx(changed);f->clients();assert(!results(f->h,1));
    f->rx(a);f->clients();assert(results(f->h,1,0,OURS)==1);bool honest=false;
    for(auto &r:f->h.out)if(r.which_payload_variant==meshtastic_FromRadio_clientNotification_tag&&strstr(r.clientNotification.message,"relay evidence"))honest=true;
    assert(honest);group("broadcast wire clears want_ack; exact lower-hop echo is relay evidence only");
  }
  {
    auto f=std::make_unique<Fixture>();f->text(1,PEER,false);f->s.poll(300,0);f->done();f->clients();assert(results(f->h,1,0,OURS)==1);
    f->text(2,PEER,false,700);f->s.poll(700,0);f->done(701);f->clients();assert(f->radio.sends==2&&results(f->h,2,0)==1&&f->s.endpointArmed());
    group("normal mode supports successive sends without consuming the legacy quota");
  }
  {
    auto f=std::make_unique<Fixture>();for(unsigned i=1;i<=4;++i)f->text(i);
    assert(f->s.endpointFree()==0);f->text(5);assert(results(f->h,5,38)==1);
    bool full=false;for(auto &r:f->h.out)if(r.which_payload_variant==meshtastic_FromRadio_queueStatus_tag&&r.queueStatus.maxlen==4&&r.queueStatus.free==0)full=true;
    assert(full);f->text(1,PEER,true,200,1);assert(f->s.endpointFree()==0);f->s.poll(300,0);f->done();
    f->rx(f->wire(PEER,OURS,100,1));f->clients();assert(results(f->h,1,0)==1&&results(f->bh,1,0)==1&&f->s.endpointFree()==1);
    f->text(1,PEER,true,600);f->clients();assert(results(f->h,1,0)==2&&f->radio.sends==1);
    f->text(1,PEER,true,600,0,3,"Different");assert(results(f->h,1,32)==1);
    group("shared USB/BLE capacity, duplicate submission and conflicting nonce rejection");
  }
  {
    auto f=std::make_unique<Fixture>();for(unsigned i=1;i<=12;++i){const auto now=1000*i;f->text(i,PEER,false,now);f->s.poll(now,0);f->done(now+1);f->clients();}
    assert(f->radio.sends==12);f->text(1,PEER,false,14000);assert(results(f->h,1,32)==1&&f->s.endpointFree()==4);
    f->s.stop();assert(f->s.apply(f->s.config)&&f->s.start(false,15000)&&f->s.armEndpoint(true));f->text(1,PEER,false,16000);assert(results(f->h,1,32)==2);
    group("evicted IDs remain rejected by boot-lifetime nonce filter across config changes");
  }
  {
    auto f=std::make_unique<Fixture>();f->radio.sendStatus=-EBUSY;f->text();
    for(auto now:{300u,1000u,2000u,3000u,4000u})f->s.poll(now,0);
    f->clients();assert(f->radio.sends==5&&results(f->h,1,5)==1&&!f->s.faulted()&&f->s.endpointFree()==4);
    for(unsigned i=1;i<5;++i)assert(f->radio.frames[0]==f->radio.frames[i]);
    f->s.poll(100000,0);assert(f->radio.sends==5);
    group("recoverable native busy is bounded by five-attempt reliable-unicast policy with immutable retries");
  }
  {
    auto f=std::make_unique<Fixture>();f->radio.occupied=true;f->text();f->s.poll(30100,0);f->clients();assert(f->radio.sends==0&&results(f->h,1,3)==1);
    group("pre-send contention expires without transmission");
  }
  {
    auto f=std::make_unique<Fixture>();f->radio.sendStatus=-EIO;f->text();f->s.poll(300,0);f->clients();assert(f->s.faulted()&&results(f->h,1,4)==1);
    f->s.poll(100000,0);assert(f->radio.sends==1);group("native I/O error faults closed without retry");
  }
  {
    auto f=std::make_unique<Fixture>();f->text();f->s.poll(300,0);f->s.poll(11300,0);f->clients();assert(f->s.faulted()&&f->radio.sends==1&&results(f->h,1,4)==1);
    group("uncertain native completion faults closed");
  }
  {
    auto f=std::make_unique<Fixture>();f->text();f->s.poll(300,0);f->rx(f->wire(PEER,OURS,100,1));f->radio.restoreStatus=-EIO;f->done(600);f->clients();assert(f->s.faulted()&&results(f->h,1,0)==0&&results(f->h,1,4)==1);
    group("RX-restore failure overrides a latched ACK with hardware fault");
  }
  {
    auto f=std::make_unique<Fixture>();f->radio.air=2200;f->text();f->s.poll(300,0);f->done(2600);
    f->s.poll(23000,0);f->s.poll(24000,0);f->done(26300);f->s.poll(47000,0);f->s.poll(48000,0);f->clients();
    assert(f->radio.sends==3&&!f->s.faulted());f->done(49300);
    mt::RadioTiming::Deadline timing;mt::PacketLifetime::Bounds lifetime;
    assert(mt::RadioTiming::resolve(f->radio.air,timing)&&
      mt::PacketLifetime::resolve(timing.hostMs,16000+2*f->radio.air,lifetime));
    const uint32_t absoluteExpiry=100+lifetime.pendingMs;
    f->s.poll(71000,0);f->s.poll(71250,0);assert(f->radio.sends==4&&f->s.active()&&!f->s.faulted());
    f->done(73550);f->s.poll(93000,0);f->s.poll(93250,0);assert(f->radio.sends==5&&f->s.active()&&!f->s.faulted());
    f->done(95450);for(unsigned i=1;i<5;++i)assert(f->radio.frames[0]==f->radio.frames[i]);
    f->s.poll(absoluteExpiry-1,0);f->clients();assert(!results(f->h,1)&&f->radio.sends==5&&!f->s.faulted());
    f->s.poll(absoluteExpiry,0);f->clients();
    assert(results(f->h,1,3)==1&&f->radio.sends==5&&!f->s.faulted()&&f->s.receiving());
    group("long reliable unicast makes five immutable attempts then expires at its airtime-derived absolute deadline");
  }
  {
    auto f=std::make_unique<Fixture>();f->text();f->s.poll(300,0);f->done();
    for(unsigned i=1;i<90;++i)f->rx(f->wire(PEER,0xffffffff,100+i,0,0,0,true),1000*i);
    f->s.poll(90100,0);f->clients();assert(f->radio.sends==1&&results(f->h,1,3)==1);
    group("RX-airtime postpones ACK timers but absolute expiry remains finite");
  }
  {
    auto f=std::make_unique<Fixture>();for(unsigned i=1;i<=4;++i)f->text(i);
    auto incoming=f->wire(PEER,OURS,100,0,0,(3<<5)|1|8,true);f->rx(incoming,1000,700);f->s.poll(1300,0);
    assert(f->radio.sends==1);bench::RouteMetadata m;auto &a=f->radio.frames[0];assert(f->s.codec.decode(a.data(),a.size(),m)&&m.request_id==100&&m.portnum==5&&a[12]==(3|3<<5|8));
    group("separate priority ACK capacity and return-hop policy under full user queue");
  }
  {
    auto f=std::make_unique<Fixture>();auto incoming=f->wire(PEER,OURS,100,0,0,8,true);
    f->rx(incoming,1000,700);f->s.poll(1300,0);f->done(1301);assert(f->radio.sends==1);
    f->rx(incoming,2000,701);f->s.poll(2400,0);assert(f->radio.sends==1);
    for(unsigned i=1;i<=3;++i){auto now=1000+3000*i;f->rx(incoming,now,700+i);f->s.poll(now+300,0);f->done(now+301);}
    assert(f->radio.sends==1); // Duplicates share the existing reliable ACK job.
    f->s.poll(20000,0);f->s.poll(21000,0);f->done(21001);
    f->s.poll(42000,0);f->s.poll(43000,0);f->done(43001);
    f->s.poll(65000,0);f->s.poll(65250,0);assert(f->radio.sends==4);f->done(65251);
    f->s.poll(81000,0);f->s.poll(81250,0);assert(f->radio.sends==5);f->done(81251);
    for(unsigned i=1;i<5;++i)assert(f->radio.frames[0]==f->radio.frames[i]);
    f->s.poll(90999,0);assert(f->radio.sends==5&&f->s.endpointControls()==1);
    f->s.poll(91000,0);assert(f->radio.sends==5&&!f->s.endpointControls());
    f->s.poll(100000,0);assert(f->radio.sends==5);
    group("duplicate direct text shares one ACK with five immutable attempts and absolute expiry");
  }
  {
    auto f=std::make_unique<Fixture>();f->rx(f->wire(PEER,OURS,1,0,0,8,true,false),1000);f->s.poll(2000,0);assert(!f->radio.sends);
    auto bad=f->wire(PEER,OURS,2,0,0,8,true);bad[13]^=1;f->rx(bad,2100);f->s.poll(2500,0);assert(!f->radio.sends);
    f->rx(f->wire(PEER,OURS+1,3,0,0,8,true),2600);f->s.poll(3000,0);assert(!f->radio.sends);
    group("legacy zero-hop/malformed-channel/misaddressed text cannot trigger ACK");
  }
  {
    auto f=std::make_unique<Fixture>();f->text();f->rx(f->wire(PEER,OURS,100,0,0,8,true),200);assert(f->s.armEndpoint(false));
    f->s.poll(1000,0);f->clients();assert(!f->radio.sends&&results(f->h,1,4)==1&&f->s.receiving());
    assert(f->s.armEndpoint(true));f->s.poll(2000,0);assert(!f->radio.sends);
    f->text(2,PEER,true,2100);f->s.poll(2200,0);assert(f->s.active());assert(f->s.armEndpoint(false));f->clients();assert(!f->s.active()&&f->s.endpointFree()==4&&results(f->h,2,4)==1);
    group("disarm clears both queues and cancels active native TX before reuse");
  }
  {
    auto f=std::make_unique<Fixture>();f->text();f->c.resetStream();f->s.poll(300,0);assert(!f->radio.sends&&f->s.endpointFree()==4);
    f->text(2,PEER,true,400,1);f->s.poll(500,0);f->bc.resetStream();assert(f->s.endpointFree()==3);f->done(600);assert(f->s.endpointFree()==4);
    f->s.poll(100000,0);assert(f->radio.sends==1);group("disconnect cancels pending retries and preserves active buffer until completion");
  }
  {
    auto f=std::make_unique<Fixture>(false);assert(!f->s.armEndpoint(true));f->text();assert(results(f->h,1,4)==1);
    f->rx(f->wire(PEER,OURS,100,0,0,8,true),1000);f->s.poll(2000,0);assert(!f->radio.sends);
    group("release TX gate covers originated messages and generated ACKs");
  }
  {
    auto f=std::make_unique<Fixture>();f->text();f->s.poll(300,0);f->done();
    f->rx(f->wire(PEER,OURS,100,1,0,0,true));f->clients();assert(!results(f->h,1));
    for(auto &r:f->h.out)if(r.which_payload_variant==meshtastic_FromRadio_packet_tag&&r.packet.decoded.portnum==1)assert(!r.packet.decoded.request_id);
    group("non-routing request_id cannot bypass peer ACK gate in stock client");
  }
  {
    auto f=std::make_unique<Fixture>();f->text(1,0xffffffff,true,100,0,0);f->s.poll(300,0);
    assert(f->radio.frames[0][12]==(3|3<<5));
    group("stock want_ack plus zero hop normalizes to configured three hops");
  }
  {
    auto f=std::make_unique<Fixture>();f->text();f->s.poll(300,0);f->done();
    f->rx(f->wire(PEER,OURS,100,1),90100);f->clients();assert(results(f->h,1,3)==1&&!results(f->h,1,0));
    group("ACK arriving after absolute expiry cannot revive the request");
  }
  {
    auto f=std::make_unique<Fixture>();uint32_t now=0xfffff000u;f->text(1,PEER,true,now);f->s.poll(now,0); // quiet deadline is already due modulo wrap
    // Establish a wrapped service start deadline, then exercise full retry timing.
    f->s.stop();assert(f->s.start(false,now));assert(f->s.armEndpoint(true));f->text(2,PEER,true,now);
    f->s.poll(now+300,0);assert(f->s.active());f->done(now+400);f->rx(f->wire(PEER,OURS,100,2),1000);f->clients();assert(results(f->h,2,0)==1);
    group("monotonic clock wrap preserves pending ACK matching and deadlines");
  }
  {
    auto f=std::make_unique<Fixture>();uint32_t random=17;
    for(unsigned i=0;i<10000;++i){random=random*1664525+1013904223;std::vector<uint8_t> bytes(random%256);for(auto &b:bytes){random=random*1664525+1013904223;b=random>>24;}f->rx(bytes,i+1000,random);f->s.poll(i+1000,random);}
    assert(!f->radio.sends);group("ten thousand malformed RF frames cause no automatic transmission");
  }
  {
    auto f=std::make_unique<Fixture>(true,127);
    auto configure=[&](int power,uint32_t id) {
      meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_set_config_tag;
      a.set_config.which_payload_variant=meshtastic_Config_lora_tag;auto &l=a.set_config.payload_variant.lora;
      l.region=meshtastic_Config_LoRaConfig_RegionCode_US;l.use_preset=true;l.sx126x_rx_boosted_gain=true;l.hop_limit=3;l.tx_power=power;l.tx_enabled=true;
      meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_packet_tag;auto &p=r.packet;
      p.id=id;p.to=OURS;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;p.decoded.portnum=meshtastic_PortNum_ADMIN_APP;
      auto b=pb_ostream_from_buffer(p.decoded.payload.bytes,sizeof(p.decoded.payload.bytes));assert(pb_encode(&b,meshtastic_AdminMessage_fields,&a));p.decoded.payload.size=b.bytes_written;input(f->c,r,200);
    };
    assert(f->s.armEndpoint(false));configure(1,50);assert(results(f->h,50,0)==1&&f->s.endpointArmed()&&f->s.config.effectivePower(true)==1);
    configure(-8,51);assert(results(f->h,51,0)==1&&f->s.endpointArmed()&&f->s.config.effectivePower(true)==-8);
    configure(-9,52);assert(results(f->h,52,0)==1&&f->s.endpointArmed()&&f->s.config.effectivePower(true)==-9);
    f->text(1,PEER,true,300);f->s.poll(500,0);f->done(501);f->rx(f->wire(PEER,OURS,101,1),600);f->clients();assert(results(f->h,1,0,PEER)==1);
    f->s.stop();assert(f->s.start(false,1000)&&!f->s.endpointArmed());configure(0,53);assert(results(f->h,53,0)==1&&f->s.endpointArmed());
    group("T096 normal TX arm retains M96 normalized power settings and the peer ACK path works");
  }
  {
    auto f=std::make_unique<Fixture>();f->rx(f->wire(PEER,OURS,100,0,0,(3<<5)|1|8,true),1000,700);
    f->s.poll(1300,0);assert(f->radio.sends==1);auto response=f->radio.frames[0];
    const auto ackId=bench::le32(response.data()+8);assert(response[12]==(3|3<<5|8));
    // A forged wrong-peer response cannot finish the active control slot.
    f->rx(f->wire(PEER+1,OURS,101,ackId),1400);f->done(1500);
    f->s.poll(20000,0);f->s.poll(21000,0);assert(f->radio.sends==2);
    f->rx(f->wire(PEER,OURS,102,ackId,0,8),21100);f->done(21200);
    f->s.poll(50000,0);f->clients();assert(f->radio.sends==2&&!results(f->h,ackId)&&!results(f->bh,ackId));
    group("reliable ACK accepts exact peer terminal ACK during native TX with no loops/client result");
  }
  {
    auto f=std::make_unique<Fixture>();f->rx(f->wire(PEER,OURS,100,0,0,(3<<5)|1|8,true),1000,700);
    f->s.poll(1300,0);f->done(1400);auto echoed=f->radio.frames[0];
    echoed[12]=(3<<5)|2|8;echoed[15]=0x99;auto wrong=echoed;wrong.back()^=1;
    f->rx(wrong,1500);f->s.poll(20000,0);f->s.poll(21000,0);assert(f->radio.sends==2);f->done(21100);
    f->rx(echoed,22000);f->s.poll(50000,0);f->s.poll(51000,0);assert(f->radio.sends==2);
    group("exact lower-hop ACK echo cancels control retry; changed ciphertext does not");
  }
  {
    for(uint32_t code:{1u,2u,3u,4u,5u,6u,7u,8u,9u,32u,33u,34u,35u,36u,37u,38u,39u,256u,0xffffffffu}) {
      auto f=std::make_unique<Fixture>();f->text();f->s.poll(300,0);f->done();f->rx(f->wire(PEER,OURS,100,1,code));f->clients();
      assert(results(f->h,1,code<=39?code:2)==1);f->s.poll(50000,0);assert(f->radio.sends==1);
    }
    group("defined peer NAK reasons survive; oversized unknown reasons never truncate to success");
  }
  {
    auto f=std::make_unique<Fixture>();f->radio.sendStatus=-EBUSY;
    f->rx(f->wire(PEER,OURS,100,0,0,8,true),1000,700);
    for(auto t:{1300u,2000u,3000u,4000u,5000u})f->s.poll(t,0);
    assert(f->radio.sends==5&&!f->s.faulted()&&!f->s.endpointControls());
    for(unsigned i=1;i<5;++i)assert(f->radio.frames[0]==f->radio.frames[i]);
    f->s.poll(100000,0);assert(f->radio.sends==5);
    group("native EBUSY for reliable generated ACK consumes five immutable attempts without fault");
  }
  {
    auto f=std::make_unique<Fixture>();f->radio.air=2200;
    f->rx(f->wire(PEER,OURS,100,0,0,8,true),1000,700);f->s.poll(1300,0);f->done(3500);
    f->s.poll(25000,0);f->s.poll(26000,0);f->done(28200);f->s.poll(50000,0);f->s.poll(51000,0);
    assert(f->radio.sends==3&&!f->s.faulted()&&!f->s.throttled);f->done(53500);
    mt::RadioTiming::Deadline timing;mt::PacketLifetime::Bounds lifetime;
    assert(mt::RadioTiming::resolve(f->radio.air,timing)&&
      mt::PacketLifetime::resolve(timing.hostMs,16000+2*f->radio.air,lifetime));
    const uint32_t absoluteExpiry=1000+lifetime.pendingMs;
    f->s.poll(73000,0);f->s.poll(73250,0);assert(f->radio.sends==4&&f->s.active()&&!f->s.faulted());f->done(75450);
    f->s.poll(95000,0);f->s.poll(95250,0);assert(f->radio.sends==5&&f->s.active()&&!f->s.faulted());f->done(97450);
    for(unsigned i=1;i<5;++i)assert(f->radio.frames[0]==f->radio.frames[i]);
    f->s.poll(absoluteExpiry-1,0);assert(f->radio.sends==5&&f->s.endpointControls()==1&&!f->s.faulted());
    f->s.poll(absoluteExpiry,0);assert(f->radio.sends==5&&!f->s.endpointControls()&&!f->s.faulted());
    f->s.poll(100000,0);assert(f->radio.sends==5&&!f->s.faulted());
    group("reliable generated ACK uses five immutable airtime-admitted attempts before absolute expiry");
  }
  std::cout<<"PASS endpoint suite: 34 scenario groups; no hardware or RF\n";
}
