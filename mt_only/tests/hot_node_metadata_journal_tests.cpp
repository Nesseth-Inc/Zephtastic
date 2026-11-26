// SPDX-License-Identifier: GPL-3.0-only
#include "mqtt/HotNodeMetadataJournal.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace {
using mt::mqtt::HotNodeMetadataJournal;
using mt::mqtt::OnlineNodeWindow;

struct Stored { char key[20]{}; uint8_t bytes[HotNodeMetadataJournal::RecordSize]{}; size_t size=0; };
struct Backend final : mt::SettingsBackend {
    Stored slots[2]{};
    size_t writes=0, reads=0;
    int writeError=0, readError=0;
    bool tear=false, corruptReadback=false;
    Stored *find(const char *key) {
        for (auto &slot: slots) if (slot.key[0] && !strcmp(slot.key,key)) return &slot;
        return nullptr;
    }
    int read(const char *key,uint8_t *out,size_t capacity) override {
        ++reads;
        if (readError) return readError;
        Stored *slot=find(key); if (!slot) return -ENOENT;
        if (slot->size>capacity) return -E2BIG;
        memcpy(out,slot->bytes,slot->size); return int(slot->size);
    }
    int write(const char *key,const uint8_t *data,size_t length) override {
        ++writes;
        if (writeError) return writeError;
        Stored *slot=find(key);
        if (!slot) for (auto &candidate: slots) if (!candidate.key[0]) {slot=&candidate;strncpy(slot->key,key,sizeof(slot->key)-1);break;}
        assert(slot && length<=sizeof(slot->bytes));
        slot->size=tear?length/2:length;memcpy(slot->bytes,data,slot->size);
        if (corruptReadback&&slot->size>32) slot->bytes[32]^=1;
        return 0;
    }
};

void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=uint8_t(n>>(8*i));}
uint32_t crc32(const uint8_t *p,size_t n){uint32_t c=UINT32_MAX;for(size_t i=0;i<n;++i){c^=p[i];for(unsigned j=0;j<8;++j)c=(c>>1)^(0xedb88320u&uint32_t(-int32_t(c&1)));}return ~c;}
void reseal(Stored &slot){put32(slot.bytes+HotNodeMetadataJournal::RecordSize-4,crc32(slot.bytes,HotNodeMetadataJournal::RecordSize-4));}
constexpr uint32_t Self=0x11223344, PeerA=0x55667788, PeerB=0x99aabbcc;
uint8_t Key[32];
void initKey(){for(unsigned i=0;i<sizeof(Key);++i)Key[i]=uint8_t(i*7u+3u);}
OnlineNodeWindow sample(){OnlineNodeWindow w;w.setSelf(Self);assert(w.create(Self));assert(w.create(PeerA));assert(w.create(PeerB));assert(w.observe(PeerA,true,1700000100u,true));assert(w.setEvictionFlags(PeerA,OnlineNodeWindow::Favorite|OnlineNodeWindow::HasPublicKey));assert(w.setEvictionFlags(PeerB,OnlineNodeWindow::Ignored));return w;}
void emptyAndAlternatingRoundTrip(){
 Backend b;HotNodeMetadataJournal j;auto w=sample();w.recordHeardWhileClockUntrusted(PeerA,50);const size_t before=b.writes;
 assert(j.load(b,w,Self,Key)==0&&j.state()==HotNodeMetadataJournal::State::Empty&&j.ready()&&j.generation()==0&&j.activeSlot()==-1);
 assert(b.writes==before&&w.hotCount()==3&&w.self()==Self);
 assert(HotNodeMetadataJournal::RecordSize==1132&&j.save(b,w,Self,Key)==0&&j.activeSlot()==0&&j.generation()==1);
 assert(!strcmp(b.slots[0].key,"mt_hot/meta0")&&b.slots[0].size==1132);
 auto changed=w;assert(changed.setOrigin(PeerA,false));assert(j.save(b,changed,Self,Key)==0&&j.activeSlot()==1&&j.generation()==2);
 HotNodeMetadataJournal loaded;OnlineNodeWindow out;assert(loaded.load(b,out,Self,Key)==0&&loaded.state()==HotNodeMetadataJournal::State::Loaded&&loaded.activeSlot()==1&&loaded.generation()==2);
 assert(out.hotCount()==3&&out.self()==Self&&out.hotAt(0)->node==Self&&out.hotAt(1)->node==PeerA&&out.hotAt(1)->lastHeard==1700000100u&&!out.hotAt(1)->viaMqtt);
 assert(out.hotAt(1)->evictionFlags==(OnlineNodeWindow::Favorite|OnlineNodeWindow::HasPublicKey));
 uint32_t uptime=0;assert(!out.getHeardAtUptimeSeconds(PeerA,uptime));
 puts("PASS empty is non-mutating; alternating 1132-byte snapshots persist hot rows, not uptime sidecar");
}
void recoverOnlyPreviousValidSlotWithoutRepair(){
 Backend b;HotNodeMetadataJournal j;auto w=sample();assert(j.load(b,w,Self,Key)==0&&j.save(b,w,Self,Key)==0);
 auto next=w;assert(next.setOrigin(PeerA,false));assert(j.save(b,next,Self,Key)==0);
 b.slots[1].bytes[HotNodeMetadataJournal::RecordSize/2]^=0x80;
 HotNodeMetadataJournal reload;OnlineNodeWindow out;const auto writes=b.writes;
 assert(reload.load(b,out,Self,Key)==0&&reload.state()==HotNodeMetadataJournal::State::Recovered&&!reload.ready());
 assert(reload.generation()==1&&reload.activeSlot()==0&&out.hotAt(1)->viaMqtt&&b.writes==writes);
 assert(reload.save(b,out,Self,Key)==-EACCES&&b.writes==writes);
 puts("PASS torn newer slot exposes only prior valid snapshot and prohibits automatic repair/write");
}
void invalidAndWrongIdentityDoNotPublish(){
 Backend b;HotNodeMetadataJournal j;auto w=sample();assert(j.load(b,w,Self,Key)==0&&j.save(b,w,Self,Key)==0);
 uint8_t wrong[32];memcpy(wrong,Key,sizeof(wrong));wrong[0]^=1;
 OnlineNodeWindow untouched;assert(untouched.create(0xabcdef01));untouched.setSelf(0xabcdef01);
 HotNodeMetadataJournal bad;assert(bad.load(b,untouched,Self,wrong)==-EINVAL&&bad.state()==HotNodeMetadataJournal::State::Invalid);
 assert(untouched.hotCount()==1&&untouched.hotAt(0)->node==0xabcdef01&&b.writes==1);
 b.slots[0].bytes[6]=0; b.slots[0].bytes[7]=0;reseal(b.slots[0]);
 OnlineNodeWindow untouched2;assert(untouched2.create(0xabcdef01));untouched2.setSelf(0xabcdef01);
 HotNodeMetadataJournal malformed;assert(malformed.load(b,untouched2,Self,Key)==-EINVAL&&malformed.state()==HotNodeMetadataJournal::State::Invalid);
 assert(untouched2.hotCount()==1&&untouched2.hotAt(0)->node==0xabcdef01);
 puts("PASS wrong identity and malformed zero-row record fail without changing caller state or stored bytes");
}
void strictRowsAndIdentitySelf(){
 Backend b;HotNodeMetadataJournal j;auto w=sample();assert(j.load(b,w,Self,Key)==0&&j.save(b,w,Self,Key)==0);
 auto *r=b.find("mt_hot/meta0");assert(r);
 // Duplicate nonzero node, with a valid outer checksum, is still invalid.
 memcpy(r->bytes+48+2*9,r->bytes+48+1*9,9);reseal(*r);
 OnlineNodeWindow out;HotNodeMetadataJournal reload;assert(reload.load(b,out,Self,Key)==-EINVAL);
 Backend selfMismatch;HotNodeMetadataJournal empty;auto wrongSelf=sample();assert(empty.load(selfMismatch,wrongSelf,Self,Key)==0);
 wrongSelf.setSelf(Self+1);assert(empty.save(selfMismatch,wrongSelf,Self,Key)==-EINVAL&&selfMismatch.writes==0);
 puts("PASS duplicate rows and self-row mismatches are rejected before persistence/publication");
}
void writeReadbackAndGenerationFailures(){
 Backend b;HotNodeMetadataJournal j;auto w=sample();assert(j.load(b,w,Self,Key)==0&&j.save(b,w,Self,Key)==0);
 auto changed=w;assert(changed.setOrigin(PeerA,false));b.writeError=-EIO;
 assert(j.save(b,changed,Self,Key)==-EIO&&j.state()==HotNodeMetadataJournal::State::IoError&&j.generation()==1&&j.activeSlot()==0);
 assert(j.save(b,changed,Self,Key)==-EACCES&&b.writes==2);
 Backend torn;HotNodeMetadataJournal first;assert(first.load(torn,w,Self,Key)==0);torn.tear=true;
 assert(first.save(torn,w,Self,Key)==-EIO&&first.state()==HotNodeMetadataJournal::State::IoError);
 Backend readback;HotNodeMetadataJournal second;assert(second.load(readback,w,Self,Key)==0);readback.corruptReadback=true;
 assert(second.save(readback,w,Self,Key)==-EIO&&second.state()==HotNodeMetadataJournal::State::IoError&&second.generation()==0);
 Backend wrap;HotNodeMetadataJournal seed;assert(seed.load(wrap,w,Self,Key)==0&&seed.save(wrap,w,Self,Key)==0);
 put32(wrap.slots[0].bytes+8,UINT32_MAX);reseal(wrap.slots[0]);HotNodeMetadataJournal maxgen;OnlineNodeWindow loaded;
 assert(maxgen.load(wrap,loaded,Self,Key)==0&&maxgen.generation()==UINT32_MAX);
 const size_t writes=wrap.writes;assert(maxgen.save(wrap,loaded,Self,Key)==-EOVERFLOW&&wrap.writes==writes&&maxgen.ready());
 puts("PASS write/readback uncertainty freezes; generation wrap refuses without a write");
}
void missingSlotIoIsNotEmpty(){
 Backend b;b.readError=-EIO;OnlineNodeWindow w;HotNodeMetadataJournal j;
 assert(j.load(b,w,Self,Key)==-EIO&&j.state()==HotNodeMetadataJournal::State::IoError&&b.writes==0);
 puts("PASS backend read error is not mistaken for an empty journal");
}
void fullHotCapacityRoundTrips(){
 Backend b;OnlineNodeWindow::HotRow rows[OnlineNodeWindow::HotCapacity]{};
 rows[0]={Self,1700000000u,false,0};
 for(size_t i=1;i<OnlineNodeWindow::HotCapacity;++i)rows[i]={uint32_t(0x20000000u+i),uint32_t(1700000000u+i),bool(i&1),uint8_t(i%16)};
 OnlineNodeWindow w;assert(w.load(rows,OnlineNodeWindow::HotCapacity));w.setSelf(Self);
 HotNodeMetadataJournal j;assert(j.load(b,w,Self,Key)==0&&j.state()==HotNodeMetadataJournal::State::Empty);
 assert(j.save(b,w,Self,Key)==0);
 HotNodeMetadataJournal reload;OnlineNodeWindow out;assert(reload.load(b,out,Self,Key)==0&&out.hotCount()==OnlineNodeWindow::HotCapacity);
 assert(out.hotAt(0)->node==Self&&out.hotAt(119)->node==0x20000077u&&out.hotAt(119)->evictionFlags==7);
 puts("PASS all 120 hot rows round-trip at the fixed record cap");
}
void zeroSelfAndInvalidFlagsRefuse(){
 Backend b;OnlineNodeWindow w=sample();HotNodeMetadataJournal j;
 assert(j.load(b,w,0,Key)==-EINVAL&&j.state()==HotNodeMetadataJournal::State::Invalid&&b.writes==0);
 assert(j.load(b,w,Self,Key)==0&&j.save(b,w,Self,Key)==0);
 auto *stored=b.find("mt_hot/meta0");assert(stored);
 stored->bytes[48+8]=0x40;reseal(*stored);
 OnlineNodeWindow untouched;assert(untouched.create(0xabcdef01));untouched.setSelf(0xabcdef01);
 HotNodeMetadataJournal invalid;assert(invalid.load(b,untouched,Self,Key)==-EINVAL&&untouched.hotAt(0)->node==0xabcdef01);
 puts("PASS zero identity and reserved row flags fail closed without caller-state mutation");
}
}

int main(){initKey();emptyAndAlternatingRoundTrip();recoverOnlyPreviousValidSlotWithoutRepair();invalidAndWrongIdentityDoNotPublish();strictRowsAndIdentitySelf();writeReadbackAndGenerationFailures();missingSlotIoIsNotEmpty();fullHotCapacityRoundTrips();zeroSelfAndInvalidFlagsRefuse();return 0;}
