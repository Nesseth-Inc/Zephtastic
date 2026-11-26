// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
namespace mt {
struct NativeStage {uint32_t calls=0,failedCall=0,stage=0;int32_t result=0;};
struct NativeStages {NativeStage send,config,restart;};
enum class DiagnosticDomain:uint8_t {Unknown=0,Errno=1,Health=2,State=3,Routing=4};
enum class FaultStage:uint16_t {
  Unknown=1,Stop=2,Configure=3,Start=4,StartHealth=5,PrivateStop=6,
  PrivateStopHealth=7,PrivateState=8,PrivateTrust=9,PrivateRestore=10,
  PrivateRestoreHealth=11,PrivateIdleHealth=12,CompletionTimeout=13,
  CompletionStatus=14,Restore=15,RestoreHealth=16,IdleHealth=17,Send=18,
  BusyRestore=19,BusyRestoreHealth=20,ReceiveHealth=21,StorageStop=22,StorageHealth=23
};
struct RadioDiagnostic {
  uint32_t submits=0,rejected=0,faults=0;
  uint16_t rejectStage=0,faultStage=0,rejectFlags=0,faultFlags=0;
  DiagnosticDomain rejectDomain=DiagnosticDomain::Unknown,faultDomain=DiagnosticDomain::Unknown;
  int32_t rejectValue=0,faultValue=0;
  bool nativeKnown=false;NativeStages native;
  static void bump(uint32_t &value){if(value!=UINT32_MAX)++value;}
};
}
