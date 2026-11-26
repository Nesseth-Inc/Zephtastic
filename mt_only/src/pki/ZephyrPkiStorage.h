// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "PkiStorage.h"
#include "PkiAppendHistory.h"
#include "recovery/PkiIdentityRecovery.h"
#include "recovery/PkiIdentityRecoveryIngress.h"
#include "ZephyrAppendStore.h"
#include "LegacyTailTools.h"
#if MT_PKI_LEGACY_TAIL_TOOLS_ENABLED
#include "ZephyrHistoryPageEraser.h"
#endif
#include "PkiPeerAdministration.h"
#include "../ZephyrSettings.h"
#include "../Service.h"
#include <zephyr/kvss/nvs.h>
#include <zephyr/kernel.h>

namespace mt::pki {
// Existing mounted SETTINGS_NVS backend; begin locks only NVS (not settings),
// scans a fixed supported name-ID range, and never invokes a repair/load path.
class NvsBootSettings final:public BootSettings {
public:
  // Typed preference backups add 35 fixed names and remote inventory adds two.
  // Keep scanning bounded beside the existing application and native BT names.
  static constexpr unsigned NameLimit=128,NameBytes=96;
  using ScanProgress = void (*)(void *);
  struct Diagnostic {
    int32_t result=0,nameLength=0,valueLength=0;
    uint32_t valueBytes=0;
    uint16_t lastNameId=0,slot=0,names=0;
    uint8_t stage=0,kind=0;
  };
private:
  // Keep the original eight identity/ledger/counter/contact entries in place;
  // the additive fresh selector and pending intent follow them.
  nvs_fs *fs_=nullptr;uint16_t ids_[10]{};bool held_=false;
  Diagnostic diagnostic_{};
  int scan(ScanProgress, void *);
public:
  NvsBootSettings()=default;
  NvsBootSettings(const NvsBootSettings &)=delete;
  NvsBootSettings &operator=(const NvsBootSettings &)=delete;
  ~NvsBootSettings()override{end();}
  int begin()override;
  int beginWithProgress(ScanProgress, void *);
  void end()override;
  int read(const char *,uint8_t *,size_t)override;
  Diagnostic diagnostic()const{return diagnostic_;}
};
class ZephyrPkiContext final:public PkiStorageContext,public IdentityRecoveryGate {
  k_mutex &mutex_;Radio &radio_;Service *service_=nullptr;unsigned depth_=0;
public:
  ZephyrPkiContext(k_mutex &mutex,Radio &radio):mutex_(mutex),radio_(radio){}
  // Same Service/Radio and actual serviceLock used by every app settings caller.
  // Bind after constructing Service, before publishing clients or starting RX.
  bool bind(Service &);
  bool tryEnter()override;
  void leave()override;
  bool bootQuiescent()const override;
  bool quiescent()const override;
  bool identityRecoveryQuiescent()const override;
};
class ZephyrPkiStorage:public IdentityRecoveryTarget {
  ZephyrPkiContext context_;ZephyrSettings runtime_;NvsBootSettings boot_;
#if defined(CONFIG_MT_PKI_FRESH_DOMAIN_ONLY)
  PkiStorage storage_;
#else
  ZephyrAppendStore appendStore_;
#if MT_PKI_LEGACY_TAIL_TOOLS_ENABLED
  ZephyrHistoryPageEraser eraser_;
#endif
  PkiAppendHistory append_{appendStore_};PkiStorage storage_;
  ZephyrAppendStore recoveryAppendStore_;PkiAppendHistory recoveryAppend_{recoveryAppendStore_};
  IdentityRecoveryBootOverlay recoveryOverlay_{boot_};
  PkiStorage recoveryProof_;
  PkiIdentityRecovery recovery_;
#endif
public:
  // No hardware/storage work in constructors. Never mount/init or create keys.
  ZephyrPkiStorage(k_mutex &mutex,Radio &radio,EntropySource &entropy):context_(mutex,radio),
#if defined(CONFIG_MT_PKI_FRESH_DOMAIN_ONLY)
    storage_(boot_,runtime_,context_,entropy,nullptr){}
#else
    storage_(boot_,runtime_,context_,entropy,&append_),recoveryAppend_(recoveryAppendStore_),
    recoveryOverlay_(boot_),recoveryProof_(recoveryOverlay_,runtime_,context_,entropy,&recoveryAppend_),
    recovery_(runtime_,context_,context_,storage_,recoveryOverlay_,recoveryProof_){}
#endif
  ZephyrPkiStorage(const ZephyrPkiStorage &)=delete;
  ZephyrPkiStorage &operator=(const ZephyrPkiStorage &)=delete;
  PkiStorage &storage(){return storage_;}
  // USB-local restoration of an existing identity scalar. Persists identity
  // journal records only; caller must reboot and run normal startup proof.
  int restoreExistingIdentity(const uint8_t scalar[32])override {
#if defined(CONFIG_MT_PKI_FRESH_DOMAIN_ONLY)
    (void)scalar;
    return -ENOTSUP;
#else
    return recovery_.restore(scalar);
#endif
  }
  NvsBootSettings::Diagnostic bootDiagnostic()const{return boot_.diagnostic();}
#if MT_PKI_LEGACY_TAIL_TOOLS_ENABLED
  int historyBlank(bool &out){return appendStore_.blank(out);}
  int probeLegacyTail(TailProbeResult &out,uint32_t &generation,uint32_t &crc){
    return storage_.probeLegacyTail(appendStore_,out,generation,crc);
  }
  int prepareFormerCode(PkiPeerAdministration &admin,PreparationProgress &progress,
                       const UsbTailPrepareCommand &command,uint32_t epoch,TailPreparationResult &out){
    return admin.prepareTail(appendStore_,eraser_,progress,command,epoch,out);
  }
#endif
  bool bindService(Service &service){return context_.bind(service);}
};
}
