// SPDX-License-Identifier: GPL-3.0-only
// Named primary-key normalization. Callers own channel and RF admission.
#ifndef MT_PRIMARY_CHANNEL_HPP
#define MT_PRIMARY_CHANNEL_HPP

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "MtNodeInfo.h"
#include "ChannelName.h"
#include "PskNormalizer.h"

namespace mt {

struct PrimaryChannel {
  struct Selection {
    uint8_t bytes[32]{};
    size_t length = 0;
    uint8_t hash = 0;
    bool publicDefault = false;
    bool licensedPlaintext = false;
    bool aead = false;
  };

  static bool validName(const char *name,bool allowEmpty=true) {
    if(!name)return false;
    for(size_t i=0;i<12;++i) {
      const auto ch=static_cast<unsigned char>(name[i]);
      if(!ch)return i!=0||allowEmpty;
      if(ch<0x20||ch>0x7e)return false;
    }
    return false;
  }
  // Pinned DisplayFormatters long names, US-supported presets only.
  static const char *presetName(unsigned preset) {
    switch(preset) {
    case 0:return "LongFast";case 1:return "LongSlow";
    case 3:return "MediumSlow";case 4:return "MediumFast";
    case 5:return "ShortSlow";case 6:return "ShortFast";
    case 7:return "LongMod";case 8:return "ShortTurbo";
    case 9:return "LongTurbo";case 16:return "MediumTurbo";
    default:return nullptr;
    }
  }
  static bool defaultKey(const bench::MtNodeRecord &record) {
    return record.valid()&&record.bytes[3]==sizeof(bench::MtNodeRecord::PublicKey)&&
      !memcmp(record.bytes+bench::MtNodeRecord::Key,bench::MtNodeRecord::PublicKey,sizeof(bench::MtNodeRecord::PublicKey));
  }
  // Wipes all 32 key bytes and resets length/hash/publicDefault.
  static void clear(Selection &out) {
    bench::wipeKeyBuffer(out.bytes, sizeof(out.bytes));
    out.length = 0;
    out.hash = 0;
    out.publicDefault = false;
    out.licensedPlaintext = false;
    out.aead = false;
  }

  // Normalizes an accepted channel selection into out. Returns false and
  // clears out for invalid input. A computed encrypted-channel hash of zero
  // is valid and remains distinguishable from licensed plaintext by mode.
  static bool normalize(const uint8_t *p, size_t n, Selection &out,const char *name="LongFast",bool aead=false) {
    uint8_t scratch[32]{};
    bool ok = false;
    size_t effLen = 0;
    const uint8_t *eff = nullptr;

    // Expand all nonzero stock aliases and zero-pad short raw keys before
    // deriving the channel hash. Snapshotting also preserves in-place inputs.
    if (PskNormalizer::normalize(p,n,bench::MtNodeRecord::PublicKey,
                                sizeof(bench::MtNodeRecord::PublicKey),
                                scratch,sizeof(scratch),effLen) &&
        (effLen == 16 || effLen == 32)) {
      eff = scratch;
    }

    if (eff != nullptr && validName(name,false)) {
      // Stock XOR over every effective name/key byte; zero is a valid hash.
      uint8_t h = 0;
      const bool named=ChannelName::hash(name,eff,effLen,h);

      if (named) {
        // Hash 0 is preserved without remapping. The snapshot is complete, so
        // clearing out is safe even if p aliased out.bytes.
        ok = true;
        clear(out);
        memcpy(out.bytes, eff, effLen);
        out.length = effLen;
        out.aead=aead;
        out.hash = uint8_t(h^(aead?0xAEu:0u));
        out.publicDefault =
            effLen == sizeof(bench::MtNodeRecord::PublicKey) &&
            memcmp(out.bytes, bench::MtNodeRecord::PublicKey,
                        sizeof(bench::MtNodeRecord::PublicKey)) == 0;
      }
    }

    if (!ok)
      clear(out);
    bench::wipeKeyBuffer(scratch, sizeof(scratch));
    return ok;
  }

  static bool normalizeLicensedPlaintext(const char *name, Selection &out) {
    clear(out);
    uint8_t hash=0;
    if(!ChannelName::licensedPlaintextHash(name,hash))return false;
    out.hash=hash;out.licensedPlaintext=true;
    return true;
  }

  // Decode an explicit node-record mode. Empty key length alone is never an
  // opt-in: only V2 + keyLen 0 selects licensed plaintext.
  static bool fromRecord(const bench::MtNodeRecord &record,const char *name,
                         Selection &out,bool aead=false) {
    clear(out);
    if(!record.valid())return false;
    if(record.licensedPlaintext()) {
      if(aead||!normalizeLicensedPlaintext(name,out)||out.hash!=record.bytes[4]) {
        clear(out);return false;
      }
      return true;
    }
    if(record.bytes[3]!=16&&record.bytes[3]!=32)return false;
    if(!normalize(record.bytes+bench::MtNodeRecord::Key,record.bytes[3],out,name,false)||
       out.hash!=record.bytes[4]) {
      clear(out);return false;
    }
    if(aead)out.hash^=0xAEu;
    out.aead=aead;
    return true;
  }

  // Existing 72-byte node records already carry raw 16/32-byte keys. Require
  // canonical effective-name hash as well as the unchanged record CRC and owner data.
  static bool configured(const bench::MtNodeRecord &record,const char *name="LongFast",bool aead=false) {
    Selection selection;
    const bool accepted=fromRecord(record,name,selection,aead);
    clear(selection);
    return accepted;
  }
};

} // namespace mt

#endif // MT_PRIMARY_CHANNEL_HPP
