// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "AppendStore.h"
#include "PkiLedger.h"
#include "PkiCapacity.h"

namespace mt::pki {
class PkiStorage;
// An immutable v2 baseline plus a finite exact append history. The baseline
// reference must remain alive and immutable. CRCs detect faults; they are not
// authentication or protection against restoring an older whole-flash image.
// The caller holds the real Service/storage barrier for every mutation.
//
// Root encoding (little endian, exactly4096 bytes):
//   0 "ZTAH",4 version1,5 header128,6 cell64,7 zero;
//   8 storeBytes,12 rootBytes,16 eventLimit,20 baselineGeneration,
//   24 snapshotBytes1944,28 snapshotCRC,32 localNode,36 localPublic[32];
//   68..127 zero;128..2071 canonical normalized1944B baseline;
//   2072..4087 zero;4088 rootCRC;4092 "CMIT" written separately LAST.
// snapshotCRC is CRC32(snapshot[0..1939]), i.e. its validated embedded seal,
// NOT CRC32 of a snapshot including its own CRC (which has constant residue).
// rootCRC covers bytes0..4087. Anchor.generation is the BASE generation.
// An exhausted legacy journal may activate the separately persisted production
// nonce authority by clearing bit0 of its root commit word, after both new
// records are verified. No identity, baseline, event, CRC or page is erased.
// Older readers reject that commit word and fail closed on firmware rollback.
//
// Event encoding (64 bytes; no separate page headers):
//   0 kind(1 TX,2 RX,3 ADD,4 REVOKE),1 version1,2..3 zero;
//   4 sequence(physical cell index+1),8 immutable lifetime domain,
//   12 node,16 packetID,20 publicKey[32],52 previousCRC,
//   56 CRC32(bytes0..55),60 "CMIT" programmed/read-verified LAST.
// TX:domain0/localNode/nonzeroID/zeroKey. RX:activeDomain/peer/nonzeroID/
// zeroKey. ADD:domain4+sequence/peer/ID0/fullKey. REVOKE:the exact active
// domain/peer/ID0/fullKey. Baseline peer domains are1..4 by stored index.
// Never reuse domains, old nodes, old keys, cells or pages; no erase/GC API.
// open validates all cells and the complete all-FF suffix. Torn/unknown/corrupt
// cells freeze rather than being skipped. Duplicate validation is deliberately
// disk-scanned (worst-case quadratic open); target latency needs qualification.
class PkiAppendHistory {
public:
  static constexpr size_t ActivePeerLimit=ContactLimit;
  static constexpr size_t StoreBytes=172032,RootBytes=4096,CellBytes=64;
  static constexpr size_t EventLimit=(StoreBytes-RootBytes)/CellBytes;
  explicit PkiAppendHistory(AppendStore &store):store_(store){}
  PkiAppendHistory(const PkiAppendHistory &)=delete;
  PkiAppendHistory &operator=(const PkiAppendHistory &)=delete;
  ~PkiAppendHistory(){close();}
  int initialize(const PkiLedger &,bool quiescent,AppendAnchor &out);
  int open(const PkiLedger &,const AppendAnchor &);
  // Cold read-only bootstrap: validate the immutable root against the current
  // identity, then publish its canonical ledger snapshot and matching anchor.
  // This does not load or mutate NVS and does not replay the event region.
  static int reconstructBaseline(AppendStore &,uint32_t localNode,
                                 const uint8_t localPublic[32],
                                 uint8_t snapshot[PkiLedger::RecordSize],
                                 AppendAnchor &anchor);
  bool ready()const;
  int lastError()const{return error_;} // Frozen/storage error; admission rc is returned directly.
  bool close();
  uint16_t remaining()const;
  bool status(PkiLedger::Status &out)const;
  bool lookup(uint32_t node,uint8_t outPublic[32])const;
  bool active(uint32_t node,const uint8_t publicKey[32])const;
  bool revoked(uint32_t node)const;
  bool activePeers(uint32_t *out,size_t capacity,size_t &count)const;
  // After open() has validated the immutable root and replayed every existing
  // event, read the exact embedded baseline in bounded chunks. Each call
  // stages at most 64 bytes and leaves out unchanged on any error. Callers may
  // assemble the 1944-byte value in their own bounded migration buffer; they
  // must discard that buffer unless every chunk succeeds. This is read-only:
  // it neither repairs NVS nor rewrites the root/events/nonce authority.
  int readBaselineChunk(size_t offset,uint8_t *out,size_t size)const;
  bool sent(uint32_t packetId)const;
  // Exact legacy replay lookup without interpreting append capacity. This
  // never admits or commits a new packet; production policy owns that step.
  ReplayCheck recorded(uint32_t node,const uint8_t publicKey[32],uint32_t packetId)const;
  // Exact old-domain lookup including revoked peers. Supplies replay evidence
  // only; the current contact store must independently authorize membership.
  ReplayCheck historicalRecorded(uint32_t node,const uint8_t publicKey[32],uint32_t packetId)const;
  ReplayCheck seen(uint32_t node,const uint8_t publicKey[32],uint32_t packetId)const;
  bool counterMode()const{return ready()&&counterMode_;}
  // Caller verifies identity-bound dual counter records before this one-way
  // selector change, while holding the actual storage/service pause.
  int activateCounterMode(bool quiescent);
  int reserve(uint32_t packetId,bool quiescent);
  int addTrustedPeer(uint32_t node,const uint8_t publicKey[32],bool quiescent);
  int revokeTrustedPeer(uint32_t node,const uint8_t publicKey[32],bool quiescent);
  int commitAuthenticated(uint32_t node,const uint8_t publicKey[32],uint32_t packetId,bool quiescent);
private:
  friend class PkiStorage;
  enum class State:uint8_t {New,Initialized,Ready,Frozen,Closed};
  struct Pin {uint32_t node=0,domain=0;uint8_t publicKey[32]{};};
  struct Base {uint32_t generation=0,snapshotCrc=0,node=0;uint16_t sent=0,received=0,stored=0;uint8_t active=0;};
  AppendStore &store_;const PkiLedger *base_=nullptr;
  mutable State state_=State::New;mutable int error_=0;mutable bool busy_=false;
  bool counterMode_=false;
  AppendAnchor anchor_{};Base initial_{};PkiLedger::Status status_{};
  Pin pins_[ActivePeerLimit]{};uint8_t activeCount_=0;uint16_t events_=0;uint32_t lastCrc_=0;
  int fail(int)const;
  int read(size_t,uint8_t *,size_t)const;
  int blank(size_t,size_t)const;
  int program(size_t,const uint8_t *,size_t);
  int baseInfo(const PkiLedger &,Base &)const;
  int reconstructLedgerBaseline(PkiLedger &,const IdentityBinding &,AppendAnchor &)const;
  bool rootChunk(const PkiLedger &,const Base &,size_t,uint8_t *,size_t)const;
  int validateRoot(const PkiLedger &,const Base &,const AppendAnchor &);
  int baselinePin(unsigned,Pin &,bool &tombstone)const;
  int findActive(uint32_t)const;
  int historical(uint32_t,const uint8_t *,Pin *out=nullptr,size_t before=EventLimit)const;
  int cell(unsigned,uint8_t out[CellBytes])const;
  int hasId(uint32_t domain,uint32_t id,size_t before)const;
  int admissible(uint32_t,const uint8_t *)const;
  int fold(const uint8_t bytes[CellBytes],size_t before);
  int append(uint8_t kind,uint32_t domain,uint32_t node,uint32_t id,const uint8_t *pub);
};
static_assert(PkiAppendHistory::EventLimit==2624,"Fixed reserved-region cell budget");
static_assert(sizeof(PkiAppendHistory)<=1024,"Append history static RAM budget");
}
