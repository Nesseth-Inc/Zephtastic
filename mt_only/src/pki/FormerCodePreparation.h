// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "LegacyTailProbe.h"
#include <cstdint>

namespace mt::pki {
struct HistoryPageEraser {
  virtual ~HistoryPageEraser()=default;
  virtual int validate()const=0;
  virtual int erasePage(unsigned page)=0; // Fixed history pages 0..41 only.
};
struct PreparationProgress {
  virtual ~PreparationProgress()=default;
  virtual int checkpoint()=0; // Current epoch, pause, authority, deadline/feed.
};
struct TailPreparationResult {
  uint8_t attempted=0,verified=0;bool blank=false,complete=false;
};
// Adapted from Qwen C's pure proposal; immutable absolute start/budget.
inline bool preparationDeadline(int64_t start,int64_t now,int64_t budget){
  return start>=0&&now>=start&&budget>0&&budget<=INT64_MAX-start&&now<start+budget;
}
// Internal engine. Its fingerprint argument is a source-owned policy seam for
// deterministic NOR tests. The production PkiStorage API exposes no digest,
// address, range or force parameter and supplies only its baked whitelist.
class FormerCodePreparation {
  static int checked(int rc){return rc>0?-EIO:rc;}
public:
  static constexpr unsigned Pages=42,PageBytes=4096;
  static int run(AppendStore &store,HistoryPageEraser &eraser,PreparationProgress &progress,
                 const uint8_t approved[32],bool &latched,TailPreparationResult &out){
    out={};if(latched)return -EALREADY;
    if(store.capacity()!=LegacyTailProbe::Bytes||!approved)return -EINVAL;
    int rc=checked(progress.checkpoint());if(rc)return rc;
    TailProbeResult probe;
    struct Wipe {TailProbeResult &p;~Wipe(){crypto_wipe(&p,sizeof(p));}} wipe{probe};
    rc=LegacyTailProbe::inspect(store,probe);if(rc)return rc;
    rc=checked(progress.checkpoint());if(rc)return rc;
    if(probe.blank){out.blank=true;out.complete=true;return 0;}
    if(probe.possibleJournal||crypto_verify32(probe.digest,approved))return -EACCES;
    rc=checked(eraser.validate());if(rc)return rc;
    uint8_t bytes[64]{};
    struct WipeBytes {uint8_t *p;~WipeBytes(){crypto_wipe(p,64);}} wipeBytes{bytes};
    for(unsigned page=0;page<Pages;++page){
      rc=checked(progress.checkpoint());if(rc)return rc;
      latched=true;++out.attempted; // Never retry even a driver-reported failure.
      rc=checked(eraser.erasePage(page));if(rc)return rc;
      rc=checked(progress.checkpoint());if(rc)return rc;
      for(unsigned offset=0;offset<PageBytes;offset+=sizeof(bytes)){
        rc=checked(progress.checkpoint());if(rc)return rc;
        rc=checked(store.read(page*PageBytes+offset,bytes,sizeof(bytes)));if(rc)return rc;
        for(uint8_t byte:bytes)if(byte!=0xff)return -EIO;
      }
      ++out.verified;
    }
    rc=checked(progress.checkpoint());if(rc)return rc;
    rc=LegacyTailProbe::inspect(store,probe);if(rc)return rc;
    rc=checked(progress.checkpoint());if(rc)return rc;
    if(!probe.blank)return -EIO;
    out.blank=true;out.complete=true;return 0;
  }
};
}
