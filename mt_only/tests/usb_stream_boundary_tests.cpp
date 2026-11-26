// SPDX-License-Identifier: GPL-3.0-only
// M22 USB logical-stream regression coverage. Memory transports only: the real
// Companion parser, replay table and UsbSessionEpoch run with inert adapters.
#include "node_mute_fixture.h"
#include "UsbSerialJtagTx.h"
#include "admin/PreferenceArchive.h"
#include <iostream>

static unsigned groups=0;
#define PASS(name) do {++groups;std::cout<<"PASS " name "\n";} while(0)

static void serialJtagBulkInCompletion() {
  using namespace mt::usb_serial_jtag;
  // Model a host bulk read which spans two endpoint packets. A short packet
  // or a full read completes it; stopping with just 64 bytes would strand them.
  for (uint32_t length = 0; length <= 2 * 516; ++length) {
    std::vector<uint8_t> source(length), host, inFlight;
    for (uint32_t i = 0; i < length; ++i) source[i] = uint8_t(i);
    uint32_t consumed = 0;
    while (consumed < length) {
      const auto queued = length - consumed;
      const auto write = nextWriteSize(queued);
      assert(write > 0 && write <= PacketSize && write <= queued);
      if (queued > PacketSize) assert(write == PacketSize);
      inFlight.insert(inFlight.end(), source.begin() + consumed, source.begin() + consumed + write);
      consumed += write;
      if (write < PacketSize || inFlight.size() == 2 * PacketSize) {
        host.insert(host.end(), inFlight.begin(), inFlight.end());
        inFlight.clear();
      }
    }
    assert(inFlight.empty() && host == source);
  }
  assert(nextWriteSize(0) == 0);
  PASS("S3 serial/JTAG delivers queue tails at every packet boundary without a subsequent write");
}

static std::vector<uint8_t> hello(uint32_t nonce) {
  meshtastic_ToRadio value{};value.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;
  value.want_config_id=nonce;return MuteFixture::framed(MuteFixture::inputWire(value));
}
static void feed(mt::Companion &companion,const std::vector<uint8_t> &bytes,uint32_t now,uint32_t epoch=0) {
  for(uint8_t byte:bytes)companion.byte(byte,now,epoch,mt::Companion::WorkspaceProof{true});
}
static bool completed(const MuteHost &host,uint32_t nonce) {
  return !host.out.empty()&&host.out.back().which_payload_variant==meshtastic_FromRadio_config_complete_id_tag&&
    host.out.back().config_complete_id==nonce;
}

static void timeoutBoundaries() {
  for(uint32_t delay:{1999u,2000u}) {
    MuteFixture f(81);f.seed();f.usb.unmeasuredHandshakeBoundary=true;
    const uint32_t request=0x22001000u+delay;f.toggle(MutePeer,request,false,1);
    assert(f.service.nodes.muted(MutePeer));const auto writes=f.backend.writes;
    const std::vector<uint8_t> partial{0x94,0xc3,0x00,0x40,0x08,0x01,0x12};feed(f.companion,partial,100);
    const uint32_t nonce=0x18652194u+delay;f.usb.out.clear();feed(f.companion,hello(nonce),100+delay);
    assert(!f.usb.handshakeBoundaries&&!completed(f.usb,nonce));
    // The swallowed request cannot mutate or create a replay entry while the
    // abandoned old frame has not exceeded the strict timeout.
    f.toggle(MutePeer,request,false,101+delay);assert(f.backend.writes==writes&&f.service.nodes.muted(MutePeer));
    f.usb.out.clear();feed(f.companion,hello(nonce),2102+delay);
    assert(f.usb.handshakeBoundaries==1&&completed(f.usb,nonce));
    f.usb.out.clear();f.toggle(MutePeer,request,false,2103+delay);
    assert(f.usb.error(request)==0&&!f.service.nodes.muted(MutePeer)&&f.backend.writes==writes+1);
  }
  PASS("XIAO partial frame rejects handshakes at and below 2000ms; a retry strictly after 2000ms creates one fresh stream");
}

static void rolloverAndPayloadData() {
  MuteFixture f(81);f.seed();f.usb.unmeasuredHandshakeBoundary=true;
  const uint32_t near=0xffffff00u;feed(f.companion,{0x94,0xc3,0x00,0x40,0x08},near);
  const uint32_t nonce=0x18c394c3u;f.usb.out.clear();feed(f.companion,hello(nonce),near+2000u);
  assert(!f.usb.handshakeBoundaries&&!completed(f.usb,nonce));
  feed(f.companion,hello(nonce),near+4001u);assert(f.usb.handshakeBoundaries==1&&completed(f.usb,nonce));

  MuteFixture data(81);data.seed();data.usb.unmeasuredHandshakeBoundary=true;
  meshtastic_ToRadio outer{};outer.which_payload_variant=meshtastic_ToRadio_packet_tag;outer.packet.to=MuteLocal;
  outer.packet.id=0x22002001;outer.packet.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
  outer.packet.decoded.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;
  const uint8_t embedded[]{0x94,0xc3,0x00,0x02,0x18,0x01};
  memcpy(outer.packet.decoded.payload.bytes,embedded,sizeof(embedded));outer.packet.decoded.payload.size=sizeof(embedded);
  feed(data.companion,MuteFixture::framed(MuteFixture::inputWire(outer)),5);
  assert(data.companion.localMessages==1&&!data.usb.handshakeBoundaries);
  const std::vector<uint8_t> counterexample{0x94,0xc3,0x00,0x0d,0x94,0xc3,0x00,0x10,0x94,0xc3,0x00,0x06,0x18,0x94,0xc3,0x80,0x80,0x01};
  feed(data.companion,counterexample,6);assert(!data.usb.handshakeBoundaries);
  PASS("timeout arithmetic survives uint32 rollover and embedded or nested magic remains payload data");
}

static void canonicalityReplayAndBleSeparation() {
  MuteFixture f(81);f.seed();f.seed(MutePeer2,"Second synthetic peer");f.usb.unmeasuredHandshakeBoundary=true;
  const uint32_t request=0x22003001;f.toggle(MutePeer,request,false,1);assert(f.service.nodes.muted(MutePeer));
  const auto writes=f.backend.writes;f.usb.out.clear();f.toggle(MutePeer2,request,false,2);
  assert(f.usb.error(request)==meshtastic_Routing_Error_BAD_REQUEST&&f.backend.writes==writes&&!f.service.nodes.muted(MutePeer2));

  const uint32_t nonce=0x22003002;auto noncanonical=MuteFixture::inputWire([&]{meshtastic_ToRadio v{};v.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;v.want_config_id=nonce;return v;}());
  noncanonical.insert(noncanonical.end(),{0xa0,0x06,0x01});f.usb.out.clear();feed(f.companion,MuteFixture::framed(noncanonical),3);
  assert(!f.usb.handshakeBoundaries&&completed(f.usb,nonce));
  f.usb.out.clear();f.toggle(MutePeer,request,false,4);assert(f.backend.writes==writes&&f.service.nodes.muted(MutePeer));

  f.handshake(true,0x22003003);f.usb.out.clear();f.toggle(MutePeer,request,false,5);
  assert(f.backend.writes==writes&&f.service.nodes.muted(MutePeer));
  feed(f.companion,hello(0x22003004),6);assert(f.usb.handshakeBoundaries==1);
  f.usb.out.clear();f.toggle(MutePeer2,request,false,7);
  assert(f.usb.error(request)==0&&f.service.nodes.muted(MutePeer2)&&f.backend.writes==writes+1);
  PASS("only canonical top-level XIAO handshake resets parser/output/replay; noncanonical and BLE handshakes remain separate");
}

static void measuredDtrGeneration() {
  MuteFixture f(127);f.seed();f.usb.enforceSession=true;mt::UsbSessionEpoch epoch;
  auto bind=[&](bool high){const auto state=epoch.bind(true,high,[&]{f.companion.resetStream();f.usb.out.clear();},
      [&]{f.companion.resetStream();f.usb.out.clear();},[&]{f.usb.out.clear();});
    f.usb.sessionMeasured=state.measured;f.usb.sessionConnected=state.connected;f.usb.sessionGeneration=state.generation;return state;};
  auto first=bind(true);assert(first.connected&&first.generation==0);
  feed(f.companion,{0x94,0xc3,0x00},1,first.generation);auto down=bind(false);assert(!down.connected&&down.generation==1);
  // A completed frame tagged with the old extracted generation cannot pass the
  // host's production acceptance boundary after observed DTR loss.
  f.usb.out.clear();feed(f.companion,hello(0x22004001),2,first.generation);assert(f.usb.out.empty());
  auto up=bind(true);assert(up.connected&&up.generation==2);feed(f.companion,hello(0x22004002),3,up.generation);
  assert(completed(f.usb,0x22004002)&&!f.usb.handshakeBoundaries);
  PASS("T096 measured DTR generation drops stale extracted input and accepts only the new connected generation");
}

struct RestoreHost:MuteHost {
  using Archive=mt::admin::PreferenceArchive;
  using Receipt=mt::UsbSessionEpoch::RestoreReceipt;
  enum class Case {Success,Missing,ApplyError,DetachBeforeFence,ReconnectBeforeReply,WrongRequest};
  mt::UsbSessionEpoch epochs;Archive archive;Case test;
  uint8_t setting=11;bool measured,high=true,allowed=true,held=false;
  unsigned receipts=0;bool offered=false;
  RestoreHost(MuteFixture &f,Case t,bool dtr):
    MuteHost(f.service,f.radio,f.backend,f.journal),test(t),measured(dtr){}
  auto line(){return epochs.bind(measured,high,[]{},[]{},[]{});}
  bool consoleAllowed()const override{return allowed;}
  bool ready()const override{return !held&&MuteHost::ready();}
  bool acceptTransportFrame(uint32_t generation)override{
    const auto at=line();return generation==at.generation&&(!at.measured||at.connected);
  }
  int preferences(pb_size_t op,uint32_t,Receipt *receipt)override{
    offered=receipt!=nullptr;
    Archive::SnapshotDomain domain{};domain.node=MuteLocal;domain.publicKey[0]=7;
    const uint8_t section=1;
    Archive::Callbacks cb{};cb.context=this;cb.ready=cb.quiescent=[](void*){return true;};
    cb.capture=[](void *v,uint8_t,size_t offset,uint8_t *out,size_t cap,size_t &total){
      total=1;if(!offset&&cap)out[0]=static_cast<RestoreHost*>(v)->setting;return 0;
    };
    cb.validate=[](void*,uint8_t id,const uint8_t*,size_t size){return id==1&&size==1?0:-EINVAL;};
    cb.apply=[](void *v,uint8_t,const uint8_t *data,size_t){
      auto &host=*static_cast<RestoreHost*>(v);
      if(host.test==Case::ApplyError)return -EIO;
      host.setting=*data;return 0;
    };
    if(op==meshtastic_AdminMessage_backup_preferences_tag)return archive.backup(backend,domain,cb,&section,1);
    assert(op==meshtastic_AdminMessage_restore_preferences_tag);
    const int rc=archive.restore(backend,domain,cb,&section,1);
    Archive::IntentState intent;assert(!archive.inspectRestoreIntent(backend,intent));
    if(rc==-ENOENT){assert(intent==Archive::IntentState::Absent);return rc;}
    assert((rc==0&&intent==Archive::IntentState::Completed)||(rc==-EIO&&intent==Archive::IntentState::Pending));
    if(test==Case::DetachBeforeFence){high=false;(void)line();high=true;}
    const auto before=line();held=true;allowed=false;epochs.fence();out.clear();records.clear();
    if(receipt)receipt->bind(before,epochs.generation());
    return rc;
  }
  bool send(const uint8_t *data,size_t n)override{return allowed&&MuteHost::send(data,n);}
  bool sendRestoreReceipt(Receipt &receipt,uint32_t id,const uint8_t *data,size_t n)override{
    assert(!allowed&&held);
    if(test==Case::ReconnectBeforeReply){high=false;(void)line();high=true;}
    const auto at=line();const bool accepted=receipt.take(test==Case::WrongRequest?id+1:id,at);
    assert(!receipt.take(id,at)); // even an attempted mismatched use consumes it
    if(!accepted)return false;
    ++receipts;return MuteHost::send(data,n);
  }
};
static void restoreReceipt(bool measured,RestoreHost::Case test,unsigned owner=0,bool packetOnly=false){
  using Case=RestoreHost::Case;
  MuteFixture f;RestoreHost host(f,test,measured);
  mt::Companion c(f.service,host,81,f.workspace,owner);
  const uint32_t id=0xc0331234;
  auto input=[&](const std::vector<uint8_t> &framed){
    if(packetOnly||owner)c.packet(framed.data()+4,framed.size()-4,10,mt::Companion::WorkspaceProof{true});
    else feed(c,framed,10,host.line().generation);
  };
  auto admin=[&](pb_size_t op,uint32_t request){
    meshtastic_AdminMessage msg{};msg.which_payload_variant=op;
    const auto payload=MuteFixture::adminWire(msg);
    meshtastic_ToRadio frame{};frame.which_payload_variant=meshtastic_ToRadio_packet_tag;
    auto &p=frame.packet;p.id=request;p.to=MuteLocal;p.want_ack=true;p.pki_encrypted=true;
    p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;p.decoded.want_response=true;
    p.decoded.portnum=meshtastic_PortNum_ADMIN_APP;p.decoded.payload.size=payload.size();
    memcpy(p.decoded.payload.bytes,payload.data(),payload.size());
    input(MuteFixture::framed(MuteFixture::inputWire(frame)));
  };
  input(hello(3456));
  if(test!=Case::Missing){admin(meshtastic_AdminMessage_backup_preferences_tag,id-1);assert(host.error(id-1)==0&&!host.offered);}
  host.setting=29;host.out.clear();
  admin(meshtastic_AdminMessage_restore_preferences_tag,id);
  assert(host.offered==(!owner&&!packetOnly));
  if(test==Case::Missing){
    assert(host.ready()&&host.allowed&&!host.receipts&&host.setting==29&&host.error(id)==meshtastic_Routing_Error_BAD_REQUEST);
    return;
  }
  assert(host.held&&!host.allowed&&!host.ready()&&host.setting==(test==Case::ApplyError?29:11));
  const bool expected=!owner&&!packetOnly&&(test==Case::Success||test==Case::ApplyError);
  assert(host.receipts==unsigned(expected));
  if(expected){
    assert(host.out.size()==1&&host.error(id)==(test==Case::Success?meshtastic_Routing_Error_NONE:meshtastic_Routing_Error_NO_INTERFACE));
    const auto &p=host.out[0].packet;assert(p.from==MuteLocal&&p.to==MuteLocal&&!p.pki_encrypted);
  }else assert(host.out.empty());
  const auto replies=host.out.size();admin(meshtastic_AdminMessage_backup_preferences_tag,id+1);
  assert(host.out.size()==replies);
}
static void restoreReceipts(){
  using Case=RestoreHost::Case;
  for(bool measured:{false,true}){
    restoreReceipt(measured,Case::Success);restoreReceipt(measured,Case::Missing);
    restoreReceipt(measured,Case::ApplyError);restoreReceipt(measured,Case::WrongRequest);
  }
  restoreReceipt(true,Case::DetachBeforeFence);restoreReceipt(true,Case::ReconnectBeforeReply);
  restoreReceipt(false,Case::Success,1);restoreReceipt(false,Case::Success,2);
  restoreReceipt(false,Case::Success,0,true);
  PASS("USB FLASH restore returns its exact committed Routing result across the policy fence; pending/absent archive errors, measured reconnect, request replay and other ingress cannot inherit it");
}

int main(){serialJtagBulkInCompletion();timeoutBoundaries();rolloverAndPayloadData();canonicalityReplayAndBleSeparation();measuredDtrGeneration();restoreReceipts();
  std::cout<<"PASS "<<groups<<" USB stream-boundary groups; memory transports only\n";}
