#pragma once
// Bounded broadcast adaptation of Meshtastic flooding/history rules, reviewed
// against v2.7.26 and v2.8.0/v2.8.1. GPL-3.0; see bench/PROVENANCE.md.
#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace bench {
// Header carrier V1.0: confirmed from the user's directly visible photos.
// Sense/B2B control pins are a different profile and must not be used here.
constexpr int MT_CS=5, MT_RESET=3, MT_IRQ=2, MT_BUSY=4, MT_RXEN=6;
constexpr uint32_t SPI_CLOCK_HZ=1000000u;
enum class Protocol : uint8_t { MeshCore, Meshtastic };
inline bool due(uint32_t now, uint32_t deadline) { return int32_t(now - deadline) >= 0; }
struct Counters {
  uint32_t accepted=0, malformed=0, unsupported=0, exhausted=0, duplicates=0;
  uint32_t upgrades=0, retries=0, payloadVariants=0, full=0, expired=0, sent=0, failures=0;
  uint32_t mutableCopies=0,selfEchoes=0;
};
struct Frame { uint8_t bytes[255]{}; uint16_t len=0; uint32_t dueAt=0, expires=0; bool used=false; uint8_t attempts=0,channelContext=0; int8_t rxSnr=0; bool hasRxSnr=false,lateWindow=false; };
inline uint32_t le32(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1])<<8 | uint32_t(p[2])<<16 | uint32_t(p[3])<<24; }
enum class FrameCheck : uint8_t { Valid, Malformed, Unsupported, Exhausted };
inline FrameCheck checkBroadcast(const uint8_t *p,size_t n,uint8_t channel,bool outgoing=false) {
  if(!p || n<17 || n>255 || !le32(p+4) || le32(p+4)==0xffffffffu) return FrameCheck::Malformed;
  const uint8_t hop=p[12]&7,start=p[12]>>5;
  if(start && hop>start) return FrameCheck::Malformed;
  if(le32(p)!=0xffffffffu || !le32(p+8) || p[13]!=channel || (start && p[14]!=0)) return FrameCheck::Unsupported;
  return (hop || outgoing) ? FrameCheck::Valid : FrameCheck::Exhausted;
}
class RelayCore {
  struct Seen { uint32_t from=0,id=0,at=0,lastRelay=0; uint16_t len=0; uint8_t payload[239]{},flags=0,best=0,relays=0; bool used=false; };
  Seen history[64]{};
  Frame queue[8]{};
  uint8_t relayByte=0, channel=0;
  bool ready=false;
  bool continuous=false;
public:
  Counters stats;
  uint16_t lastPayloadDifferences=0;
  uint8_t lastPayloadDiffMask[30]{},lastPayloadFirstDifference=255,lastPayloadLastDifference=255;
  uint16_t lastPayloadBaselineLength=0;
  uint8_t lastPreviousFlags=0;
  bool lastSeen=false;
  void clearObservation() {
    lastPayloadDifferences=0;lastPreviousFlags=0;lastSeen=false;
    memset(lastPayloadDiffMask,0,sizeof(lastPayloadDiffMask));
    lastPayloadFirstDifference=lastPayloadLastDifference=255;lastPayloadBaselineLength=0;
  }
  void configure(uint8_t nodeByte, uint8_t channelHash) { reset(); relayByte=nodeByte; channel=channelHash; ready=nodeByte!=0; }
  // Queue reset preserves live dedup history across an operator stop/enable.
  void configureContinuous(uint8_t nodeByte,uint8_t channelHash) {
    for(auto &q:queue) q=Frame{};
    continuous=true;relayByte=nodeByte;channel=channelHash;ready=nodeByte!=0;
  }
  void clearPending() { for(auto &q:queue) q=Frame{}; }
  bool retry(const Frame &frame,uint32_t now) {
    if(due(now,frame.expires)) return false;
    for(auto &q:queue) if(!q.used) {q=frame;q.used=true;q.dueAt=now+500;return true;}
    ++stats.full;return false;
  }
  void reset() { for(auto &s:history) s=Seen{}; for(auto &q:queue) q=Frame{}; ready=false; }
  size_t pending() const { size_t n=0; for (const auto &q:queue) n+=q.used; return n; }
  void expire(uint32_t now) {
    for (auto &q:queue) if (q.used && due(now,q.expires)) { q.used=false; ++stats.expired; }
    for (auto &s:history) if (s.used && uint32_t(now-s.at)>=(continuous ? 60000u : 600000u)) s.used=false;
  }
  bool ingest(Protocol protocol, const uint8_t *p, size_t n, uint32_t now, uint32_t jitter) {
    expire(now);clearObservation();
    if (protocol!=Protocol::Meshtastic || !ready) { ++stats.unsupported; return false; }
    const auto checked=checkBroadcast(p,n,channel);
    if(checked==FrameCheck::Malformed) { ++stats.malformed;return false; }
    if(checked==FrameCheck::Unsupported) { ++stats.unsupported;return false; }
    if(checked==FrameCheck::Exhausted) { ++stats.exhausted;return false; }
    const uint8_t hop=p[12]&7, start=p[12]>>5;
    // No unicast, PKI, next-hop routing or ACK synthesis in this prototype.
    // Legacy hop_start=0 ignores obsolete next_hop and relay_node per RadioLibInterface.
    uint32_t from=le32(p+4), id=le32(p+8);
    // Ignore our own modern stamp regardless of history occupancy.
    if(continuous && start && p[15]==relayByte) {++stats.selfEchoes;++stats.duplicates;return false;}
    Seen *seen=nullptr, *freeSeen=nullptr;
    for (auto &s:history) { if (s.used && s.from==from && s.id==id) seen=&s; if (!s.used && !freeSeen) freeSeen=&s; }
    Frame *queued=nullptr, *freeQueue=nullptr;
    for (auto &q:queue) { if (q.used && le32(q.bytes+4)==from && le32(q.bytes+8)==id) queued=&q; if (!q.used && !freeQueue) freeQueue=&q; }
    if (seen) {
      lastSeen=true;lastPreviousFlags=seen->flags;
      // Official history identity is sender+ID. Signed originator retries can
      // regenerate a randomized signature/ciphertext; differences are metadata
      // only. Never decrypt, strip a trailer, or treat equality as authentication.
      lastPayloadBaselineLength=seen->len;
      size_t span=seen->len>size_t(n-16) ? seen->len : size_t(n-16);
      for(size_t i=0;i<span;++i) if(i>=seen->len || i>=n-16 || seen->payload[i]!=p[16+i]) {
        ++lastPayloadDifferences;lastPayloadDiffMask[i/8]|=uint8_t(1u<<(i%8));
        if(lastPayloadFirstDifference==255) lastPayloadFirstDifference=i;
        lastPayloadLastDifference=i;
      }
      if(lastPayloadDifferences) ++stats.payloadVariants;
      if((seen->flags&0xf8)!=(p[12]&0xf8)) ++stats.mutableCopies;
      // A modern frame stamped by this relay cannot be an originator retry.
      if(start && p[15]==relayByte) { ++stats.selfEchoes;++stats.duplicates;return false; }
      if (queued) {
        if (hop>seen->best) { ++stats.upgrades; seen->best=hop; }
        else { ++stats.duplicates; return false; }
      } else {
        // Official originator retransmit rule, deliberately bounded to 3 relays/10 min
        // and 1 s cooldown. Improved copies are only upgraded while still pending.
        if (!start || hop!=start || seen->relays>=3 || uint32_t(now-seen->lastRelay)<1000u) { ++stats.duplicates; return false; }
        ++stats.retries;
      }
    }
    if (!queued && !freeQueue) { ++stats.full; return false; }
    if (!seen && !freeSeen) { ++stats.full; return false; } // fail closed; don't evict live history
    if (!seen) { seen=freeSeen; seen->used=true; seen->from=from; seen->id=id; seen->len=n-16;memcpy(seen->payload,p+16,n-16);seen->flags=p[12]; seen->at=now; seen->best=hop; }
    Frame *q=queued ? queued : freeQueue;
    uint32_t oldDue=q->dueAt, oldExpiry=q->expires;
    memcpy(q->bytes,p,n); q->len=n; q->used=true;
    q->bytes[12]=(p[12]&0xf0)|(hop-1); // Pinned Router::send clears want_ack on broadcasts.
    q->bytes[14]=0; q->bytes[15]=relayByte;
    q->dueAt=queued ? oldDue : now+150+jitter%850;
    q->expires=queued ? oldExpiry : now+30000;
    ++stats.accepted;
    return true;
  }
  Frame *next(uint32_t now) { expire(now); for (auto &q:queue) if (q.used && due(now,q.dueAt)) return &q; return nullptr; }
  void defer(Frame &q, uint32_t now, uint32_t jitter) { q.dueAt=now+100+jitter%600; }
  void complete(Frame &q, uint32_t now, bool success) {
    for (auto &s:history) if (s.used && s.from==le32(q.bytes+4) && s.id==le32(q.bytes+8)) { ++s.relays; s.lastRelay=now; }
    q.used=false; if (success) ++stats.sent; else ++stats.failures;
  }
};

// Shared by both protocols; all callers run serially on the firmware's main loop thread.
class TxArbiter {
  uint8_t owner=0;
  uint32_t quietUntil=0, windowAt=0, usedMs=0;
public:
  uint32_t denied=0;
  bool ready=false, armed=false;
  bool available(uint32_t now) const { return ready && armed && !owner && due(now,quietUntil); }
  bool acquire(uint8_t who, uint32_t now, uint32_t airtime) {
    if (uint32_t(now-windowAt)>=600000) { windowAt=now; usedMs=0; }
    if (!who || !available(now) || !airtime || airtime>2000 || usedMs+airtime>6000) { ++denied; return false; }
    owner=who; usedMs+=airtime; return true;
  }
  void release(uint8_t who,uint32_t now) { if (owner==who) { owner=0; quietUntil=now+250; } }
  bool busy() const { return owner!=0; }
  uint32_t budgetUsed() const { return usedMs; }
};

inline bool validPins(int cs,int rst,int irq,int busy,int rxen) {
  // Photo-confirmed header V1.0 profile. GPIO4 is a BUSY input, never CS.
  // I2C must remain disabled: GPIO5/6 are NSS/RF_SW.
  // Restrict to this profile so configuration cannot drive an IRQ/BUSY output,
  // a B2B control, shared SPI line, or UART pin as chip-select/reset/RXEN.
  return cs==MT_CS && rst==MT_RESET && irq==MT_IRQ && busy==MT_BUSY && rxen==MT_RXEN;
}
inline bool validModem(float freq,float bw,int sf,int cr,int channel) {
  return freq>=862 && freq<=930 && (bw==31.25f || bw==62.5f || bw==125 || bw==250 || bw==500)
      && sf>=7 && sf<=12 && cr>=5 && cr<=8 && channel>=0 && channel<=255;
}
}
