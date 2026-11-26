#include "Fixture.h"
#include "vendor/RoutingCore.h"
#include <iostream>

namespace {
constexpr NodeNum A=0xaabbcc11,B=0xaabbcc22,R=0xaabbcc33,OURS=0x1234007c;
unsigned compared=0,assertions=0,boundedCases=0,timerComparisons=0,delayComparisons=0;
unsigned authCases=0,authAssertions=0,signatureCases=0,signatureQuarantineCases=0;
unsigned authHeaderComparisons=0;
bool authPhase=false;
void check(bool condition,const char *why) {
    ++assertions;
    if(authPhase)++authAssertions;
    if(!condition) {std::cerr<<"FAILED: "<<why<<'\n';std::abort();}
}
void authCheck(bool condition,const char *why) {check(condition,why);}
meshtastic_MeshPacket packet(NodeNum from=A,NodeNum to=B,PacketId id=1,
                            uint8_t hops=3,uint8_t start=3,uint8_t next=0) {
    meshtastic_MeshPacket p{};
    p.from=from;p.to=to;p.id=id;p.hop_limit=hops;p.hop_start=start;
    p.next_hop=next;p.relay_node=static_cast<uint8_t>(from);
    p.channel=8;p.want_ack=true;p.which_payload_variant=meshtastic_MeshPacket_encrypted_tag;
    p.encrypted.size=16;p.transport_mechanism=meshtastic_MeshPacket_TransportMechanism_TRANSPORT_LORA;
    for(unsigned i=0;i<p.encrypted.size;++i)p.encrypted.bytes[i]=static_cast<uint8_t>(i+20);
    return p;
}
void write32(uint8_t *p,uint32_t n) {for(unsigned i=0;i<4;++i)p[i]=static_cast<uint8_t>(n>>(8*i));}
std::vector<uint8_t> wire(const meshtastic_MeshPacket &p) {
    std::vector<uint8_t> bytes(16+p.encrypted.size);
    write32(bytes.data(),p.to);write32(bytes.data()+4,p.from);write32(bytes.data()+8,p.id);
    bytes[12]=static_cast<uint8_t>(p.hop_limit|(p.want_ack?8:0)|(p.hop_start<<5));
    bytes[13]=p.channel;bytes[14]=p.next_hop;bytes[15]=p.relay_node;
    std::memcpy(bytes.data()+16,p.encrypted.bytes,p.encrypted.size);
    return bytes;
}
bench::RouteMetadata decoded(uint32_t port=1) {
    bench::RouteMetadata m;m.channelDecoded=true;m.portnum=port;m.encodedDataSize=16;return m;
}
void decodedPacket(meshtastic_MeshPacket &p,const bench::RouteMetadata &m) {
    p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
    p.decoded={};p.decoded.portnum=static_cast<meshtastic_PortNum>(m.portnum);
    p.decoded.request_id=m.request_id;p.decoded.reply_id=m.reply_id;
    p.decoded.xeddsa_signature.size=m.signatureLength;
    std::memcpy(p.decoded.xeddsa_signature.bytes,m.signature,m.signatureLength);
    p.xeddsa_signed=m.signatureVerified;
    p.decoded.payload.size=m.payloadLength;
    std::memcpy(p.decoded.payload.bytes,m.payload,m.payloadLength);
}
void compareHeader(const bench::Frame &sent,const meshtastic_MeshPacket &expected,const char *why) {
    check(bench::le32(sent.bytes)==expected.to&&bench::le32(sent.bytes+4)==expected.from&&
          bench::le32(sent.bytes+8)==expected.id&&(sent.bytes[12]&7)==expected.hop_limit&&
          (sent.bytes[12]>>5)==expected.hop_start&&bool(sent.bytes[12]&8)==expected.want_ack&&
          sent.bytes[14]==expected.next_hop&&sent.bytes[15]==expected.relay_node,why);
    if(authPhase)++authHeaderComparisons;else ++compared;
}
uint32_t airtime(size_t n,void *) {return fixtureRadio.getPacketTime(static_cast<uint32_t>(n));}
struct Pair {
    ReliableRouter upstream;bench::RoutingCore candidate;
    std::vector<uint8_t> lastWire;uint32_t lastIngress=0;
    explicit Pair(bool alwaysRelay=true) {
        config.device.role=alwaysRelay?meshtastic_Config_DeviceConfig_Role_ROUTER:meshtastic_Config_DeviceConfig_Role_CLIENT;
        candidate.configureContinuous(0x7c,8);candidate.configureIdentity(OURS);
        candidate.setOtherOpaqueChannels(true); // Matches r1 native adapter's opaque relay policy.
        candidate.setAlwaysRelay(alwaysRelay);candidate.setAirtimeProvider(airtime);
        candidate.configureTiming(fixtureRadio.computeSlotTimeMsec(),0);
    }
    bool ingest(const meshtastic_MeshPacket &incoming,const bench::RouteMetadata &m,uint32_t now,bool requireSame=true) {
        Time::clock=now;lastIngress=now;lastWire=wire(incoming);auto p=incoming;
        // RadioLibInterface's legacy normalization is an explicit fixture boundary.
        if(!p.hop_start){p.next_hop=0;p.relay_node=0;}
        size_t before=fixtureQueue.size();bool baseline=false;
        if(!m.channelDecoded)baseline=upstream.relayOpaquePacket(&p);
        else {
            decodedPacket(p,m);
            const bool filtered=isBroadcast(p.to)?upstream.FloodingRouter::shouldFilterReceived(&p):upstream.shouldFilterReceived(&p);
            if(!filtered) {
                meshtastic_Routing routing{};routing.error_reason=static_cast<meshtastic_Routing_Error>(m.routingError);
                isBroadcast(p.to)?upstream.FloodingRouter::sniffReceived(&p,m.routingPresent?&routing:nullptr):
                                  upstream.sniffReceived(&p,m.routingPresent?&routing:nullptr);
            }
            baseline=fixtureQueue.size()>before;
            if(m.nodeInfoValid)fixtureDB.add(p.from,p.hop_start>0&&p.hop_start==p.hop_limit);
        }
        const bool result=candidate.ingestWithMetadata(bench::Protocol::Meshtastic,lastWire.data(),lastWire.size(),m,now,0);
        if(requireSame)check(result==baseline,"candidate/upstream admission");
        return result;
    }
    void transmit(uint32_t now,const char *why="candidate/upstream routing header") {
        Time::clock=now;auto *q=candidate.next(now);
        check(q!=nullptr&&!fixtureQueue.empty(),"both peers have outbound frame");
        compareHeader(*q,fixtureQueue.front(),why);
        if(lastWire.size()==q->len&&bench::le32(lastWire.data()+8)==bench::le32(q->bytes+8))
            check(std::memcmp(q->bytes+16,lastWire.data()+16,q->len-16)==0,"candidate preserves encrypted payload");
        candidate.noteTransmitStart(*q);
        ++q->attempts; // Native TX adapter counts admission attempts before complete().
        candidate.complete(*q,now,true);fixtureQueue.erase(fixtureQueue.begin());
    }
    void nodeInfo(NodeNum from,uint32_t now) {
        auto p=packet(from,NODENUM_BROADCAST,from^now,1,1);p.want_ack=false;
        auto m=decoded(4);m.nodeInfoValid=true;
        check(ingest(p,m,now),"NodeInfo admission");transmit(now+200);
    }
    void learn(uint32_t now=10000) {
        nodeInfo(A,now-9000);nodeInfo(B,now-8000);nodeInfo(R,now-7000);
        auto dm=packet(A,B,100);check(ingest(dm,decoded(),now),"first managed flood DM");transmit(now+200);
        auto echo=dm;echo.hop_limit=1;echo.relay_node=0x33;
        check(!ingest(echo,decoded(),now+1000),"duplicate relayer observation");
        auto ack=packet(B,A,101,2,3);ack.relay_node=0x33;
        auto m=decoded(5);m.request_id=dm.id;m.routingPresent=m.routingErrorPresent=true;
        check(ingest(ack,m,now+2000),"returning ACK transit");
        check(candidate.nextHop(B,now+2000)==0x33&&upstream.getNextHop(B,0x7c)==0x33,"two-way provenance learns same route");
        transmit(now+2200);
    }
};
void opaqueMatrix() {
    unsigned cases=0;
    for(uint8_t hop=0;hop<8;++hop)for(uint8_t next:{uint8_t(0),uint8_t(0x7c),uint8_t(0x33)})
    for(bool wanted:{false,true})for(bool broadcast:{false,true})for(bool legacy:{false,true})
    for(uint8_t hash:{uint8_t(0),uint8_t(8),uint8_t(255)}) {
        fixtureReset();
        {Pair pair;auto p=packet(A,broadcast?NODENUM_BROADCAST:B,1000+cases,hop,legacy?0:hop,next);
         p.want_ack=wanted;p.channel=hash;
         if(pair.ingest(p,{},1000))pair.transmit(1200);
         check(pair.candidate.historyCount()==0&&pair.upstream.pending.empty(),"opaque stays outside trusted state");
         check(!pair.upstream.wasSeenRecently(&p,false),"opaque stays outside official PacketHistory");}
        check(packetPool.active()==0,"opaque pool ownership");++cases;
    }
    fixtureReset();
    {Pair pair;auto p=packet();p.channel=0;
     check(pair.ingest(p,{},1000),"opaque first send");pair.transmit(1200);
     p.hop_limit=2;check(!pair.ingest(p,{},2000),"opaque duplicate suppression");p.hop_limit=p.hop_start;
     check(pair.ingest(p,{},3000),"opaque originator retry");pair.transmit(3200);
     check(pair.ingest(p,{},5000),"opaque third originator transmission");pair.transmit(5200);
     check(pair.ingest(p,{},7000),"opaque further originator transmission matches stock");pair.transmit(7200);
     check(pair.candidate.historyCount()==0&&pair.candidate.knownCount()==0&&
           !pair.candidate.retransmitting()&&pair.upstream.pending.empty(),
           "opaque originator repeats never create trusted history, routes or retransmission state");++cases;}
    std::cout<<"opaque header/admission matrix: "<<cases<<" cases PASS\n";
}
void historyAndRouting() {
    fixtureReset();
    {Pair pair;pair.learn();auto flood=packet(A,B,200);
     check(pair.ingest(flood,decoded(),15000),"known route does not retarget managed flooding");pair.transmit(15200);
     check(pair.upstream.pending.empty()&&pair.candidate.retransmitting()==0,"flooding transit invents no ARQ");
     auto directed=packet(A,B,201,3,3,0x7c);
     check(pair.ingest(directed,decoded(),20000),"directed-to-us packet");pair.transmit(20200);
     check(pair.upstream.pending.size()==1&&pair.candidate.retransmitting()==1,"directed ARQ starts");
     auto *reference=pair.upstream.findPendingPacket(A,directed.id);check(reference,"upstream ARQ record");
     const uint32_t delay=fixtureRadio.getRetransmissionMsec(reference->packet);
     check(pair.candidate.retransmissionDeadline(A,directed.id)-20200==delay,"exact upstream retry formula at TX completion");
     for(unsigned attempt=2;attempt<=3;++attempt) {
         Time::clock=reference->nextTxMsec;pair.upstream.doRetransmissions();
         pair.transmit(pair.candidate.retransmissionDeadline(A,directed.id));
         check(pair.candidate.retransmitting()==(attempt==2?1u:0u),"bounded ARQ lifecycle");
         reference=pair.upstream.findPendingPacket(A,directed.id);
     }
     check(pair.candidate.routingStats.fallbacks==1&&fixtureDB.getMeshNode(B)->next_hop==0,"final retry floods and clears route");
     Time::clock=reference->nextTxMsec;pair.upstream.doRetransmissions();
     check(pair.upstream.pending.empty()&&fixtureQueue.empty()&&fixtureAcks.empty(),"relay exhaustion invents no endpoint NAK");}
    check(packetPool.active()==0,"ARQ pool ownership");fixtureReset();
    {Pair pair;pair.learn();auto directed=packet(A,B,301,3,3,0x7c);
     check(pair.ingest(directed,decoded(),20000),"implicit ACK send");pair.transmit(20200);
     auto echo=directed;echo.hop_limit=1;echo.next_hop=0;echo.relay_node=0x33;
     check(!pair.ingest(echo,decoded(),22000),"decoded echo filtered");
     check(pair.upstream.pending.empty()&&pair.candidate.retransmitting()==0,"decoded LoRa echo cancels intermediate ARQ");
     directed.id=302;check(pair.ingest(directed,decoded(),23000),"opaque isolation send");pair.transmit(23200);echo.id=302;
     echo.channel=0;
     check(pair.ingest(echo,{},24000),"PKI-shaped opaque copy admitted separately");pair.transmit(24200);
     check(pair.upstream.pending.size()==1&&pair.candidate.retransmitting()==1,"opaque echo cannot cancel trusted ARQ");
     check(pair.candidate.routeCount()==1&&pair.candidate.opaqueCount()==1,"opaque echo never learns route");
     const auto before=pair.candidate.retransmissionDeadline(A,directed.id);
     pair.candidate.noteBlockedAirtime(1000);
     check(pair.candidate.retransmissionDeadline(A,directed.id)==before+1000,"co-site activity extends candidate ACK wait");
     auto *pending=pair.upstream.findPendingPacket(A,directed.id);const auto referenceBefore=pending->nextTxMsec;
     auto rx=packet(0xbbccdd44,0xbbccdd55,999);decodedPacket(rx,decoded());
     pair.upstream.shouldFilterReceived(&rx);
     check(pair.upstream.findPendingPacket(A,directed.id)->nextTxMsec==referenceBefore+1000,"actual ReliableRouter extends RX ACK wait");
     Time::clock=pair.upstream.findPendingPacket(A,directed.id)->nextTxMsec;pair.upstream.doRetransmissions();
     pair.transmit(pair.candidate.retransmissionDeadline(A,directed.id));
     check(pair.candidate.retransmitting()==1,"opaque same-ID TX did not consume trusted retry budget");}
    std::cout<<"route provenance, flooding, directed ARQ/fallback, implicit ACK and opaque isolation: PASS\n";
}
void upgradeAndCancellation() {
    fixtureReset();
    {Pair pair;auto p=packet(A,NODENUM_BROADCAST,401,1,3);
     check(pair.ingest(p,decoded(),1000),"lower-hop pending flood");p.hop_limit=2;
     check(pair.ingest(p,decoded(),1100,false),"better-hop queued replacement");
     check(pair.candidate.pending()==1&&fixtureQueue.size()==1,"single replacement");pair.transmit(1200);
     check(pair.candidate.stats.upgrades==1,"upgrade counted");}
    fixtureReset();
    {Pair pair(false);auto p=packet(A,NODENUM_BROADCAST,402,2,3);
     check(pair.ingest(p,decoded(),1000),"client queued flood");p.hop_limit=1;p.relay_node=0x33;
     check(!pair.ingest(p,decoded(),1100),"duplicate flood filtered");
     check(fixtureQueue.empty()&&pair.candidate.pending()==0,"client duplicate cancels waiting relay");}
    fixtureReset();
    {NextHopRouter reference;auto p=packet(A,B,403,3,3,0x55);decodedPacket(p,decoded());
     reference.wasSeenRecently(&p);p.hop_limit=2;p.relay_node=0x7c;reference.wasSeenRecently(&p);
     p.hop_limit=1;p.relay_node=0x33;reference.wasSeenRecently(&p);
     // Public history mutation makes the literal fallback predicate reachable. Ordinary ingress alone is not claimed to do so.
     reference.removeRelayer(0x7c,p.id,p.from);p.next_hop=0;bool fallback=false,wasNext=false,upgrade=false;
     check(reference.wasSeenRecently(&p,true,&fallback,&wasNext,&upgrade)&&fallback,"actual history fallback after relayer removal");}
    std::cout<<"better-hop replacement, literal fallback predicate and role-sensitive duplicate cancellation: PASS\n";
}
void transitControlResponses() {
    for(unsigned mode=0;mode<6;++mode) {
        fixtureReset();
        {Pair pair;pair.learn();auto original=packet(A,B,700+mode,3,3,0x7c);
         check(pair.ingest(original,decoded(),20000),"control response original admission");
         if(mode!=5)pair.transmit(20200);
         auto response=packet(mode==1||mode==2?R:B,A,800+mode,0,0);
         response.want_ack=false;auto m=decoded(5);m.routingPresent=m.routingErrorPresent=true;
         m.request_id=original.id;
         if(mode==2)m.routingError=meshtastic_Routing_Error_MAX_RETRANSMIT;
         if(mode==3){m.request_id=0;m.reply_id=original.id;}
         if(mode==4)m.request_id=99999;
         check(!pair.ingest(response,m,mode==5?20100:21000),"zero-hop control traffic is never forwarded");
         const bool cancels=mode!=3&&mode!=4;
         check(pair.upstream.pending.empty()==cancels&&pair.candidate.retransmitting()==(cancels?0u:1u),
               "destination ACK, named intermediate ACK/NAK, reply-only and wrong-ID cancellation");
         if(mode==5)check(fixtureQueue.empty()&&pair.candidate.pending()==0,"known recipient response cancels before first TX");
         check(pair.candidate.routeCount()==1,"zero-hop control does not invent a route");
         check(fixtureAcks.empty(),"transit fixture generates no endpoint ACK or NAK");}
        check(packetPool.active()==0,"control response pool ownership");
    }
    fixtureReset();
    {Pair pair;pair.learn();auto original=packet(A,B,710,3,3,0x7c);
     check(pair.ingest(original,decoded(),20000),"unknown responder fixture");pair.transmit(20200);
     auto response=packet(0xeeeeee44,A,810,0,0);auto m=decoded(5);m.routingPresent=m.routingErrorPresent=true;m.request_id=original.id;
     check(!pair.ingest(response,m,21000),"unknown zero-hop response remains off air");
     check(pair.upstream.pending.empty()&&pair.candidate.retransmitting()==0,
           "unknown responder cancels exact pending request on both routers without route learning");++boundedCases;}
    fixtureReset();
    {Pair pair;pair.learn();auto original=packet(A,B,711,3,3,0x7c);
     check(pair.ingest(original,decoded(),20000),"opaque response fixture");pair.transmit(20200);
     auto response=packet(B,A,811,0,0);response.channel=0;
     check(!pair.ingest(response,{},21000),"opaque zero-hop response remains off air");
     check(pair.upstream.pending.size()==1&&pair.candidate.retransmitting()==1,"opaque control cannot influence trusted ACK state");}
    std::cout<<"destination/intermediate ACK/NAK, before-first-TX cancellation, reply-only, wrong-ID and opaque control: PASS\n";
}
void collisionsAndAging() {
    fixtureReset();
    {uint8_t saved=0;
     auto bind=[&](bench::RoutingCore &core){core.setRouteHintStore(&saved,
        [](void *ctx,uint32_t to){return to==B?*static_cast<uint8_t *>(ctx):uint8_t(0);},
        [](void *ctx,uint32_t to,uint8_t hop){if(to==B)*static_cast<uint8_t *>(ctx)=hop;});};
     {Pair before;bind(before.candidate);before.learn();check(saved==0x33,"stock acknowledgement saves a NodeDB route hint");}
     Pair restarted;bind(restarted.candidate);
     check(restarted.candidate.nextHop(B,13000)==0,"restored hint waits for a fresh direct neighbor");
     restarted.nodeInfo(R,14000);
     check(restarted.candidate.nextHop(B,14500)==0x33&&restarted.upstream.getNextHop(B,0x7c)==0x33,
           "rebooted health cache can use the persisted hint after fresh neighbor evidence");
     check(!restarted.candidate.nextHop(B,14500,0x33)&&!restarted.upstream.getNextHop(B,0x33),
           "stored byte equal to the ingress relayer is excluded");
     restarted.nodeInfo(0xeeeeee33,15000);
     check(!restarted.candidate.nextHop(B,15200)&&!restarted.upstream.getNextHop(B,0x7c),
           "restored route with ambiguous fresh neighbors falls back");}

    fixtureReset();
    {Pair pair;pair.learn();pair.nodeInfo(0xeeeeee33,14000);
     check(pair.candidate.nextHop(B,14500)==0&&!pair.upstream.getNextHop(B,0x7c),"ambiguous relay byte falls back");
     check(pair.candidate.routingStats.collisions>0,"collision counted");}
    fixtureReset();
    {Pair pair;pair.learn();Time::clock=12000+NextHopRouter::ROUTE_TTL_MSEC;
     check(pair.candidate.nextHop(B,Time::clock)==0&&!pair.upstream.getNextHop(B,0x7c),"same 30 minute route TTL");}
    fixtureReset();
    {NextHopRouter reference;
     for(unsigned i=0;i<10000;++i){Time::clock=0xfffffff0u+i;auto p=packet(A+(i%7),B,10000+i);
         check(!reference.wasSeenRecently(&p),"new history identity under load");
         check(reference.wasSeenRecently(&p,false),"history lookup under load");}
     check(reference.initOk(),"actual full history survives clock-wrap load");}
    std::cout<<"last-byte collision, route TTL and actual PacketHistory 10000 wrap/load insertions: PASS\n";
}
void timingAndEndpointBoundary() {
    fixtureReset();
    {RadioInterface reference;reference.sf=11;reference.bw=250;
     check(reference.computeSlotTimeMsec()==28,"actual long-fast CAD slot");reference.sf=7;reference.bw=62.5;
     check(reference.computeSlotTimeMsec()==12,"actual alternate CAD slot");reference.slotTimeMsec=28;reference.lengthAirtime=true;
     for(size_t n=1;n<=239;++n)for(unsigned util:{0u,20u,40u,60u,80u,100u}){
         auto p=packet();p.encrypted.size=static_cast<pb_size_t>(n);fixtureAirTime.utilization=static_cast<float>(util);
         const uint32_t delay=reference.getRetransmissionMsec(&p);
         check(reference.lastLength==n+32,"encrypted timing includes both header lengths");
         check(delay>=4500,"exact processing floor");}
     fixtureAirTime.utilization=0;fixtureCanonicalDataSize=16;auto p=packet();decodedPacket(p,decoded());
     reference.getRetransmissionMsec(&p);check(reference.lastLength==32,"decoded canonical size plus one header");}
    fixtureReset();
    {ReliableRouter reference;auto dm=packet(A,OURS);decodedPacket(dm,decoded());
     check(reference.shouldSuccessAckWithWantAck(&dm),"actual endpoint DM ACK requests reliability");
     dm.to=NODENUM_BROADCAST;check(!reference.shouldSuccessAckWithWantAck(&dm),"broadcast ACK excluded");
     dm.to=OURS;dm.want_ack=false;check(!reference.shouldSuccessAckWithWantAck(&dm),"unrequested ACK excluded");
     dm=packet(OURS,B,500);reference.startRetransmission(packetPool.allocCopy(dm),5);
     auto overheard=dm;overheard.relay_node=0x33;reference.perhapsGenerateImplicitAckForOwnOverheard(&overheard);
     check(reference.pending.empty()&&fixtureAcks.size()==1&&fixtureAcks.front().hops==0,"endpoint internal implicit ACK excluded from relay");}
    check(packetPool.active()==0,"endpoint fixture pool ownership");
    std::cout<<"actual radio timing and excluded endpoint ACK-with-ACK/implicit-receipt boundaries: PASS\n";
}
void candidateTimingMatrix() {
    for(size_t size=1;size<=239;++size)for(uint8_t util:{uint8_t(0),uint8_t(20),uint8_t(40),uint8_t(60),uint8_t(80),uint8_t(100)}) {
        fixtureReset();fixtureRadio.lengthAirtime=true;fixtureAirTime.utilization=util;
        fixtureCanonicalDataSize=size;
        {Pair pair;pair.learn();pair.candidate.configureTiming(fixtureRadio.computeSlotTimeMsec(),util);
         auto p=packet(A,B,100000+static_cast<uint32_t>(size)*100+util,3,3,0x7c);
         p.encrypted.size=static_cast<pb_size_t>(size);
         for(size_t i=0;i<size;++i)p.encrypted.bytes[i]=static_cast<uint8_t>(i*3+size);
         auto m=decoded();m.encodedDataSize=static_cast<uint16_t>(size);
         check(pair.ingest(p,m,20000),"timer matrix directed input");pair.transmit(20200);
         auto *reference=pair.upstream.findPendingPacket(A,p.id);check(reference,"timer matrix actual pending");
         const uint32_t expected=fixtureRadio.getRetransmissionMsec(reference->packet);
         if(pair.candidate.retransmissionDeadline(A,p.id)-20200!=expected)
             std::cerr<<"timer size="<<size<<" util="<<unsigned(util)<<" expected="<<expected
                      <<" actual="<<pair.candidate.retransmissionDeadline(A,p.id)-20200<<" length="<<fixtureRadio.lastLength<<'\n';
         check(pair.candidate.retransmissionDeadline(A,p.id)-20200==expected,"candidate/exact retry timer matrix");
         ++timerComparisons;}
        check(packetPool.active()==0,"timer matrix pool ownership");
    }
    for(bool always:{false,true})for(int8_t snr=-20;snr<=10;++snr)for(uint32_t jitter:{0u,1u,7u,15u,31u,63u,127u,255u,1023u,0xffffffffu}) {
        fixtureReset();fixtureRandom=jitter;
        {Pair pair(always);pair.candidate.setRxSnr(snr);auto p=packet();p.rx_snr=snr;
         const uint32_t expected=fixtureRadio.getTxDelayMsecWeighted(&p);
         const auto frame=wire(p);
         check(pair.candidate.ingest(bench::Protocol::Meshtastic,frame.data(),frame.size(),1000,jitter),"weighted delay input");
         if(expected)check(pair.candidate.next(1000+expected-1)==nullptr,"weighted delay cannot TX early");
         auto *q=pair.candidate.next(1000+expected);
         check(q&&q->dueAt==1000+expected,"candidate/exact SNR contention delay");++delayComparisons;}
    }
    std::cout<<"candidate/exact timing: "<<timerComparisons<<" retransmission and "<<delayComparisons<<" SNR contention cases PASS\n";
}
void malformedAndBounds() {
    fixtureReset();bench::RoutingCore candidate;candidate.configureContinuous(0x7c,8);candidate.configureIdentity(OURS);
    candidate.setOtherOpaqueChannels(true);
    auto p=packet();auto bytes=wire(p);
    check(!candidate.ingest(bench::Protocol::MeshCore,bytes.data(),bytes.size(),1000,0),"MeshCore excluded from MT");
    uint32_t random=0xdec0de;
    for(unsigned i=0;i<100000;++i){std::array<uint8_t,256> input{};
        random^=random<<13;random^=random>>17;random^=random<<5;size_t n=random%input.size();
        for(size_t j=0;j<n;++j){random^=random<<13;random^=random>>17;random^=random<<5;input[j]=random;}
        candidate.ingest(bench::Protocol::Meshtastic,input.data(),n,1000+i,random);candidate.expire(1000+i);
        check(candidate.pending()<=8&&candidate.opaqueCount()<=64&&candidate.historyCount()<=64&&candidate.retransmitting()<=8,"load remains bounded");}
    candidate.clearRoutingState();
    for(unsigned i=0;i<8;++i){p.id=600+i;bytes=wire(p);check(candidate.ingest(bench::Protocol::Meshtastic,bytes.data(),bytes.size(),200000,0),"queue accepts capacity");}
    p.id=608;bytes=wire(p);check(!candidate.ingest(bench::Protocol::Meshtastic,bytes.data(),bytes.size(),200000,0),"ninth frame fails closed");
    check(candidate.pending()==8,"full queue preserves existing frames");candidate.expire(230000);check(candidate.pending()==0,"queue TTL cleanup");
    bench::RouteMetadata m;m.decodeRejected=true;
    check(candidate.ingestWithMetadata(bench::Protocol::Meshtastic,bytes.data(),bytes.size(),m,300000,0)&&
          candidate.historyCount()==0&&candidate.routeCount()==0,
          "decode failure stays outside trusted routing but remains opaque transit");++boundedCases;
    std::cout<<"candidate 100000 malformed/load frames, protocol isolation, queue and TTL bounds: PASS\n";
}

using SignaturePolicy=meshtastic_Config_SecurityConfig_PacketSignaturePolicy;
constexpr SignaturePolicy COMPATIBLE=meshtastic_Config_SecurityConfig_PacketSignaturePolicy_PACKET_SIGNATURE_POLICY_COMPATIBLE;
constexpr SignaturePolicy BALANCED=meshtastic_Config_SecurityConfig_PacketSignaturePolicy_PACKET_SIGNATURE_POLICY_BALANCED;
constexpr SignaturePolicy STRICT=meshtastic_Config_SecurityConfig_PacketSignaturePolicy_PACKET_SIGNATURE_POLICY_STRICT;

// Only the upstream admission decisions are actual implementation. Decryption,
// authoritative-key lookup, protobuf sizing/first-contact decoding, CRC and the
// XEdDSA cryptographic verdict below are controlled fixtures, not proof of crypto.
void candidateQuarantine(const meshtastic_MeshPacket &wirePacket,const bench::RouteMetadata &m,bool accepted) {
    bench::RoutingCore c;c.configureContinuous(0x7c,8);c.configureIdentity(OURS);c.setOtherOpaqueChannels(true);
    const auto bytes=wire(wirePacket);
    authCheck(c.ingestWithMetadata(bench::Protocol::Meshtastic,bytes.data(),bytes.size(),m,1000,0)==accepted,
              "candidate auth-boundary admission");
    authCheck(c.historyCount()==0&&c.knownCount()==0&&c.routeCount()==0&&c.retransmitting()==0,
              "untrusted signature/decode verdict cannot populate trusted state");
    authCheck(c.pending()==unsigned(accepted)&&c.opaqueCount()==unsigned(accepted),"candidate isolated opaque queue/history");
    if(accepted) {
        auto *q=c.next(2000);authCheck(q&&q->len==bytes.size(),"candidate quarantined frame available");
        authCheck(q->bytes[13]==bytes[13]&&std::memcmp(q->bytes+16,bytes.data()+16,bytes.size()-16)==0,
                  "opaque quarantine preserves channel and encrypted bytes");
        c.noteTransmitStart(*q);++q->attempts;c.complete(*q,2000,true);
        authCheck(c.historyCount()==0&&c.knownCount()==0&&c.routeCount()==0&&c.retransmitting()==0,
                  "quarantined TX completion cannot promote trust");
    }
}
void admissionDecodeAndCache() {
    for(auto state:{DECODE_SUCCESS,DECODE_FAILURE,DECODE_OPAQUE,DECODE_FATAL,DECODE_POLICY_REJECT})
    for(unsigned endpoint=0;endpoint<3;++endpoint) {
        fixtureReset();auto p=packet(endpoint==2?OURS:A,endpoint==1?OURS:B,200000+authCases);
        const auto original=p;p.ack_proof_status=meshtastic_MeshPacket_AckProofStatus_ACK_PROOF_VALID;
        fixtureDecodeOverride=true;fixtureDecodeState=state;fixtureDecodedData.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;
        const auto expected=state==DECODE_SUCCESS?RoutingAuthVerdict::ACCEPT:
            state==DECODE_OPAQUE||(state==DECODE_FAILURE&&endpoint==0)?RoutingAuthVerdict::OPAQUE_RELAY_ONLY:
            RoutingAuthVerdict::REJECT;
        authCheck(passesRoutingAuthGate(&p)==expected,"actual decoder-state and transit/local gate matrix");
        authCheck(p.ack_proof_status==meshtastic_MeshPacket_AckProofStatus_ACK_PROOF_ABSENT,"actual gate clears inbound ACK authentication flag");
        authCheck(p.which_payload_variant==original.which_payload_variant&&p.encrypted.size==original.encrypted.size&&
                  std::memcmp(p.encrypted.bytes,original.encrypted.bytes,p.encrypted.size)==0,
                  "actual gate authenticates copy and preserves wire representation");
        authCheck(fixtureAuthEvaluationCount()==1&&fixtureAuthCacheValid()==(state==DECODE_SUCCESS),"only ACCEPT populates exact upstream auth cache");
        if(endpoint==0&&(state==DECODE_FAILURE||state==DECODE_OPAQUE||state==DECODE_FATAL)) {
            bench::RouteMetadata m;m.decodeRejected=state==DECODE_FAILURE;m.backendFailed=state==DECODE_FATAL;
            candidateQuarantine(original,m,state!=DECODE_FATAL);
        }
        ++authCases;
    }
    fixtureReset();auto p=packet();fixtureDecodeOverride=true;fixtureDecodeState=DECODE_SUCCESS;
    fixtureDecodedData.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;
    authCheck(passesRoutingAuthGate(&p)==RoutingAuthVerdict::ACCEPT,"cache first authentication");
    fixtureDecodeState=DECODE_FATAL;p.ack_proof_status=meshtastic_MeshPacket_AckProofStatus_ACK_PROOF_INVALID;
    authCheck(passesRoutingAuthGate(&p)==RoutingAuthVerdict::ACCEPT&&fixtureAuthEvaluationCount()==1,
              "untrusted ACK flag variation cannot force cache re-authentication");
    auto applied=p;authCheck(fixtureApplyAuthCache(&applied)&&!fixtureAuthCacheValid(),"exact cache consumes authenticated snapshot once");
    authCheck(applied.which_payload_variant==meshtastic_MeshPacket_decoded_tag&&
              applied.ack_proof_status==meshtastic_MeshPacket_AckProofStatus_ACK_PROOF_ABSENT,"cached snapshot has decoded data and no forged ACK proof status");
    authCheck(!fixtureApplyAuthCache(&applied),"consumed cache cannot apply twice");++authCases;
    for(unsigned mutation=0;mutation<3;++mutation) {
        fixtureReset();p=packet();fixtureDecodeOverride=true;fixtureDecodeState=DECODE_SUCCESS;
        authCheck(passesRoutingAuthGate(&p)==RoutingAuthVerdict::ACCEPT,"cache mutation setup");
        fixtureDecodeState=DECODE_FATAL;
        if(mutation==0)++p.encrypted.bytes[0];else if(mutation==1)++p.id;else config.security.packet_signature_policy=STRICT;
        authCheck(passesRoutingAuthGate(&p)==RoutingAuthVerdict::REJECT&&fixtureAuthEvaluationCount()==2&&!fixtureAuthCacheValid(),
                  "wire bytes, packet ID and policy change invalidate exact auth cache");++authCases;
    }
    fixtureReset();p=packet();decodedPacket(p,decoded());p.pki_encrypted=true;p.public_key.size=32;p.xeddsa_signed=true;
    p.ack_proof_status=meshtastic_MeshPacket_AckProofStatus_ACK_PROOF_VALID;
    authCheck(passesRoutingAuthGate(&p)==RoutingAuthVerdict::ACCEPT&&!p.xeddsa_signed,"decoded ingress cannot supply verified signed flag");
    authCheck(fixtureApplyAuthCache(&p)&&!p.pki_encrypted&&!p.public_key.size&&!p.xeddsa_signed&&
              p.ack_proof_status==meshtastic_MeshPacket_AckProofStatus_ACK_PROOF_ABSENT,"decoded ingress clears serialized local authentication metadata");
    authCheck(fixtureCryptLock.acquisitions==1,"already-decoded admission reaches actual signature policy under lock fixture");++authCases;
}
void signaturePolicyGuards() {
    for(auto policy:{COMPATIBLE,BALANCED,STRICT})for(unsigned length=1;length<64;++length) {
        fixtureReset();config.security.packet_signature_policy=policy;
        const auto original=packet(A,NODENUM_BROADCAST,210000+signatureCases);
        auto p=original;auto m=decoded();m.signatureLength=static_cast<uint8_t>(length);decodedPacket(p,m);
        p.decoded.xeddsa_signature.size=static_cast<pb_size_t>(length);p.xeddsa_signed=true;
        authCheck(passesRoutingAuthGate(&p)==RoutingAuthVerdict::REJECT,"actual all-policy partial-signature guard");
        authCheck(!fixtureAuthCacheValid()&&fixtureCrypto.calls==0&&fixtureDB.nodes.empty(),"partial signature never becomes verified or cached");
        candidateQuarantine(original,m,false);++signatureCases;++authCases;
    }
    // All combinations explicitly compare decisions, including candidate's
    // documented inability to enforce Strict or reject cryptographically invalid
    // full signatures: every full signature is isolated opaque transit instead.
    for(auto policy:{COMPATIBLE,BALANCED,STRICT})for(unsigned key=0;key<4;++key)for(bool verifies:{false,true}) {
        fixtureReset();config.security.packet_signature_policy=policy;
        const auto original=packet(A,B,220000+signatureQuarantineCases);
        auto p=original;auto m=decoded();m.signatureLength=64;decodedPacket(p,m);p.decoded.xeddsa_signature.size=64;
        if(key==1||key==2) {fixtureDB.add(A);auto *node=fixtureDB.getMeshNode(A);
            node->public_key.size=32;node->public_key.bytes[0]=0x42;node->authoritative=key==2;}
        if(key==3) {meshtastic_NodeInfoLite_public_key_t k{};k.size=32;k.bytes[0]=0x43;fixtureDB.authoritativeWarmKeys[A]=k;}
        fixtureCrypto.verifyResult=verifies;const bool hasAuthoritative=key>=2;
        const bool accepted=hasAuthoritative?verifies:policy!=STRICT;
        authCheck(passesRoutingAuthGate(&p)==(accepted?RoutingAuthVerdict::ACCEPT:RoutingAuthVerdict::REJECT),
                  "actual full-signature authoritative/unknown/opportunistic/warm-key policy matrix");
        authCheck(fixtureCrypto.calls==unsigned(hasAuthoritative),"opportunistic cache key cannot supply authentication");
        authCheck(p.xeddsa_signed==(hasAuthoritative&&verifies),"only authoritative fixture verification marks actual packet signed");
        if(hasAuthoritative&&verifies) {
            authCheck(fixtureDB.isKnownXeddsaSigner(A),"verified authoritative signer persists downgrade evidence");
            authCheck(fixtureCrypto.lastFrom==A&&fixtureCrypto.lastTo==B&&fixtureCrypto.lastId==p.id,"actual policy binds crypto fixture call to full header identity");
        }
        candidateQuarantine(original,m,true);++signatureQuarantineCases;++authCases;
    }
    fixtureReset();auto p=packet();decodedPacket(p,decoded());p.decoded.xeddsa_signature.size=64;
    meshtastic_NodeInfoLite_public_key_t key{};key.size=32;fixtureDB.authoritativeWarmKeys[A]=key;fixtureDB.failCreate=true;
    authCheck(passesRoutingAuthGate(&p)==RoutingAuthVerdict::REJECT&&fixtureCrypto.calls==1,
              "verified warm key cannot bypass failed signer re-admission");++authCases;
    for(unsigned fail=0;fail<6;++fail) {
        fixtureReset();config.security.packet_signature_policy=STRICT;
        p=packet();decodedPacket(p,decoded(4));p.decoded.xeddsa_signature.size=64;
        fixtureInnerDecodeSuccess=fail!=1;fixtureBootstrapUser.public_key.size=fail==2?31:32;
        fixtureBootstrapCrc=fail==3?B:A;fixtureCrypto.verifyResult=fail!=4;fixtureDB.failCreate=fail==5;
        authCheck(passesRoutingAuthGate(&p)==(fail==0?RoutingAuthVerdict::ACCEPT:RoutingAuthVerdict::REJECT),
                  "actual first-contact NodeInfo decode/key-size/CRC/verification/node-capacity decisions");
        authCheck(fixtureAuthCacheValid()==(fail==0)&&fixtureDB.isKnownXeddsaSigner(A)==(fail==0),"first-contact failure cannot establish signer or cache");
        if(!fail)authCheck(p.xeddsa_signed&&fixtureDB.getMeshNode(A)->public_key.size==32,"first-contact verified fixture marks signer and copies key");
        ++authCases;
    }
    for(auto policy:{COMPATIBLE,BALANCED,STRICT})for(unsigned kind=0;kind<7;++kind) {
        fixtureReset();config.security.packet_signature_policy=policy;fixtureDB.add(A);
        nodeInfoLiteSetBit(fixtureDB.getMeshNode(A),NODEINFO_BITFIELD_HAS_XEDDSA_SIGNED_MASK,true);
        p=packet(A,kind==0||kind==3||kind==4||kind==5?NODENUM_BROADCAST:B);
        decodedPacket(p,decoded(kind==5?3:1));p.decoded.payload.size=120;p.xeddsa_signed=true;
        owner.is_licensed=kind==2;fixtureCanonicalDataSize=kind==3?174:173;fixtureSizingSuccess=kind!=4;
        fixtureInnerDecodeSuccess=kind==5;p.pki_encrypted=kind==6;
        if(kind==5){fixtureDataSizeTracksPayload=true;fixtureCanonicalDataSize=165;}
        const bool downgrade=kind==0||kind==2||kind==5;
        const bool accepted=kind==6||policy==COMPATIBLE||(policy==BALANCED&&!downgrade);
        // Direct policy call exercises legitimate already-authenticated PKI;
        // the separate gate test above clears forged PKI on decoded ingress.
        authCheck(checkXeddsaReceivePolicy(&p)==accepted,"actual unsigned downgrade/strict/PKI/canonical-sizing boundary");
        authCheck(!p.xeddsa_signed&&p.decoded.payload.size==120,"unsigned policy clears signed flag and restores canonical payload size");++authCases;
    }
    fixtureReset();config.security.packet_signature_policy=STRICT;p=packet();decodedPacket(p,decoded());p.pki_encrypted=true;
    authCheck(passesRoutingAuthGate(&p)==RoutingAuthVerdict::REJECT,"forged decoded PKI cannot bypass Strict at admission gate");++authCases;
}
void verifiedSignedRoutingCompatibility() {
    fixtureReset();config.security.packet_signature_policy=COMPATIBLE;
    {Pair pair;
     auto signedIngress=[&](const meshtastic_MeshPacket &wirePacket,bench::RouteMetadata metadata,uint32_t now) {
         metadata.signatureLength=64;metadata.signatureVerified=true;
         auto authenticated=wirePacket;decodedPacket(authenticated,metadata);
         // Verification verdict is injected. The actual policy/key/bootstrap
         // decisions execute unchanged, but no real signature is computed here.
         fixtureAuthReset();fixtureCrypto.verifyResult=true;
         if(metadata.nodeInfoValid) {
             fixtureInnerDecodeSuccess=true;fixtureBootstrapUser={};fixtureBootstrapUser.public_key.size=32;
             fixtureBootstrapUser.public_key.bytes[0]=static_cast<uint8_t>(wirePacket.from);
             fixtureBootstrapCrc=wirePacket.from;
         } else {
             auto *node=fixtureDB.getOrCreateMeshNode(wirePacket.from);
             node->public_key.size=32;node->authoritative=true;
         }
         authCheck(passesRoutingAuthGate(&authenticated)==RoutingAuthVerdict::ACCEPT&&authenticated.xeddsa_signed,
                   "actual Compatible signature fixture verifies routing ingress");
         const bool result=pair.ingest(wirePacket,metadata,now);
         // Pair's NodeInfoModule substitute reinitializes known-node bookkeeping.
         // Restore the proven key to the controlled authoritative lookup fixture.
         if(metadata.nodeInfoValid) {
             auto *node=fixtureDB.getMeshNode(wirePacket.from);node->public_key.size=32;node->authoritative=true;
             nodeInfoLiteSetBit(node,NODEINFO_BITFIELD_HAS_XEDDSA_SIGNED_MASK,true);
         }
         ++authCases;return result;
     };
     for(NodeNum n:{A,B,R}) {
         auto p=packet(n,NODENUM_BROADCAST,n^1000,1,1);auto m=decoded(4);m.nodeInfoValid=true;
         m.payloadLength=32;m.publicKeyLength=32;
         authCheck(signedIngress(p,m,1000),"verified first-contact NodeInfo admission");pair.transmit(1200);
     }
     authCheck(pair.candidate.knownCount()==3&&pair.candidate.routingStats.signatureVerified==3,
               "verified signed NodeInfos provide candidate known-node evidence");
     auto original=packet(A,B,260000);authCheck(signedIngress(original,decoded(),10000),"verified signed managed flood");pair.transmit(10200);
     auto echo=original;echo.hop_limit=1;echo.relay_node=0x33;
     authCheck(!signedIngress(echo,decoded(),11000),"verified signed echo updates decoded relayer history");
     auto receipt=packet(B,A,260001,2,3);receipt.relay_node=0x33;
     auto m=decoded(5);m.request_id=original.id;m.routingPresent=m.routingErrorPresent=true;
     authCheck(signedIngress(receipt,m,12000),"verified signed returning receipt transits");
     authCheck(pair.candidate.nextHop(B,12000)==0x33&&pair.upstream.getNextHop(B,0x7c)==0x33,
               "verified signed first-contact nodes and returning receipt learn identical next hop");pair.transmit(12200);
     original=packet(A,B,260002,3,3,0x7c);
     authCheck(signedIngress(original,decoded(),20000),"verified signed directed relay");pair.transmit(20200);
     authCheck(pair.candidate.retransmitting()==1&&pair.upstream.pending.size()==1,"verified signed directed relay starts intermediate ARQ");
     auto ack=packet(R,A,260003,0,0);ack.want_ack=false;m=decoded(5);m.request_id=original.id;m.routingPresent=m.routingErrorPresent=true;
     authCheck(!signedIngress(ack,m,21000),"verified signed named intermediate ACK is zero-hop");
     authCheck(pair.candidate.retransmitting()==0&&pair.upstream.pending.empty(),"verified signed intermediate ACK cancels both pending retransmissions");
     original.id=260004;authCheck(signedIngress(original,decoded(),22000),"verified signed second directed relay");pair.transmit(22200);
     ack=packet(B,A,260005,0,0);ack.want_ack=false;m.request_id=original.id;
     authCheck(!signedIngress(ack,m,23000),"verified signed recipient ACK is zero-hop");
     authCheck(pair.candidate.retransmitting()==0&&pair.upstream.pending.empty(),"verified signed recipient ACK cancels both pending retransmissions");
     authCheck(pair.candidate.opaqueCount()==0&&fixtureAcks.empty(),"verified signed flows stay decoded and generate no endpoint receipts");}
    authCheck(packetPool.active()==0,"signed-compatibility actual packet pool released");
    for(bool decodeAlsoFailed:{false,true}) {
        fixtureReset();const auto original=packet(A,NODENUM_BROADCAST,270000+authCases);
        auto p=original;auto m=decoded(4);m.nodeInfoValid=true;m.signatureLength=64;m.signatureRejected=true;
        m.decodeRejected=decodeAlsoFailed;decodedPacket(p,m);fixtureDB.add(A);
        auto *node=fixtureDB.getMeshNode(A);node->public_key.size=32;node->authoritative=true;fixtureCrypto.verifyResult=false;
        authCheck(passesRoutingAuthGate(&p)==RoutingAuthVerdict::REJECT,"actual known-invalid full signature rejection");
        candidateQuarantine(original,m,false);++authCases;
    }
}
void candidateAuthStateIsolation() {
    for(unsigned mode=0;mode<3;++mode) {
        fixtureReset();bench::RoutingCore c;c.configureContinuous(0x7c,8);c.configureIdentity(OURS);c.setAirtimeProvider(airtime);
        auto m=decoded(4);m.nodeInfoValid=true;
        for(NodeNum n:{A,B,R}) {auto p=packet(n,NODENUM_BROADCAST,n^1000,1,1);auto b=wire(p);
            authCheck(c.ingestWithMetadata(bench::Protocol::Meshtastic,b.data(),b.size(),m,1000,0),"auth-isolation known-node setup");
            auto *q=c.next(2000);authCheck(q,"auth-isolation node TX");c.noteTransmitStart(*q);++q->attempts;c.complete(*q,2000,true);}
        auto original=packet(A,B,230000+mode,3,3,0x7c);auto b=wire(original);m=decoded();
        authCheck(c.ingestWithMetadata(bench::Protocol::Meshtastic,b.data(),b.size(),m,3000,0),"auth-isolation directed ARQ setup");
        auto *q=c.next(4000);authCheck(q,"auth-isolation directed TX");c.noteTransmitStart(*q);++q->attempts;c.complete(*q,4000,true);
        const auto pending=c.retransmitting(),history=c.historyCount(),known=c.knownCount(),routes=c.routeCount();
        const auto deadline=c.retransmissionDeadline(A,original.id);
        auto response=packet(B,A,240000+mode,0,0);m=decoded(5);m.routingPresent=m.routingErrorPresent=true;m.request_id=original.id;
        if(mode==0)m.signatureLength=64;
        if(mode==1){m=bench::RouteMetadata{};m.decodeRejected=true;}
        if(mode==2)m.backendFailed=true;
        b=wire(response);authCheck(!c.ingestWithMetadata(bench::Protocol::Meshtastic,b.data(),b.size(),m,4500,0),"untrusted zero-hop response stays off air");
        authCheck(c.retransmitting()==pending&&c.historyCount()==history&&c.knownCount()==known&&c.routeCount()==routes&&
                  c.retransmissionDeadline(A,original.id)==deadline,"signed/decode-failed/backend-failed ACK cannot cancel trusted ARQ");
        auto echo=original;echo.hop_limit=1;echo.next_hop=0;echo.relay_node=0x33;b=wire(echo);
        m=decoded();m.signatureLength=64;
        authCheck(c.ingestWithMetadata(bench::Protocol::Meshtastic,b.data(),b.size(),m,4600,0),"signed same-ID echo admitted only as opaque");
        q=c.next(5600);authCheck(q,"signed opaque echo TX");c.noteTransmitStart(*q);++q->attempts;c.complete(*q,5600,true);
        authCheck(c.retransmitting()==pending&&c.historyCount()==history&&c.knownCount()==known&&c.routeCount()==routes&&
                  c.retransmissionDeadline(A,original.id)==deadline,"signed opaque echo TX cannot consume/cancel trusted ARQ");++authCases;
    }
    fixtureReset();auto p=packet(0xabcd0044,NODENUM_BROADCAST,250000,2,2);auto m=decoded(4);m.nodeInfoValid=true;m.signatureLength=64;
    candidateQuarantine(p,m,true);++authCases;
    std::cout<<"actual routing admission/cache and signature policy with controlled crypto/key/decode fixtures: "<<authCases
             <<" cases ("<<signatureCases<<" malformed signature, "<<signatureQuarantineCases<<" full-signature quarantine), "
             <<authAssertions<<" assertions PASS\n";
}
}
int main() {
    opaqueMatrix();historyAndRouting();upgradeAndCancellation();transitControlResponses();collisionsAndAging();timingAndEndpointBoundary();candidateTimingMatrix();malformedAndBounds();
    const unsigned existingMatrixAssertions=assertions+1; // Includes final pool-ownership assertion retained below.
    authPhase=true;
    admissionDecodeAndCache();signaturePolicyGuards();verifiedSignedRoutingCompatibility();candidateAuthStateIsolation();
    authPhase=false;
    check(packetPool.active()==0,"all actual upstream pool allocations released");
    std::cout<<"RESULT {\"compared\":"<<compared<<",\"assertions\":"<<assertions
             <<",\"timer_comparisons\":"<<timerComparisons<<",\"contention_delay_comparisons\":"<<delayComparisons
             <<",\"bounded_policy_cases\":"<<boundedCases<<",\"malformed_load_frames\":100000"
             <<",\"existing_matrix_assertions\":"<<existingMatrixAssertions<<",\"auth_cases\":"<<authCases
             <<",\"auth_assertions\":"<<authAssertions<<",\"malformed_signature_cases\":"<<signatureCases
             <<",\"auth_header_comparisons\":"<<authHeaderComparisons
             <<",\"full_signature_quarantine_cases\":"<<signatureQuarantineCases<<"}\n";
}
