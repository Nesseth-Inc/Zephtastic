// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "BluetoothSettings.h"
#include "BlePairingEvent.h"
#if defined(CONFIG_MT_SIGNED_BLE_OTA)
struct bt_conn;
#endif
namespace mt::ble {
#if defined(CONFIG_MT_SIGNED_BLE_OTA)
bool otaPolicyReady();
bool beginOta();
bt_conn *otaDisconnectTarget(uint32_t &epoch);
// A retained epoch may buffer its next write in RAM during a healthy native
// Settings lease. A new peer must always enter through an idle store.
uint32_t otaSession(bt_conn *conn,uint32_t retained=0);
// Grant processing only for an authorized epoch and an idle store. A caller
// performing a stack operation may request a referenced peer.
// -EAGAIN identifies the same authorized peer during a temporary store lease.
int otaAccess(uint32_t session,bt_conn **peer=nullptr);
#endif
struct Packet {uint32_t session;uint16_t length;
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
  bool stockImportAuthenticated=false;
#endif
  uint8_t bytes[512];};
struct SecurityStatus {bool connected=false,pending=false;int request=0;uint8_t level=0,error=0;};
int init(uint32_t node);
int init(uint32_t node,const mt::BluetoothSettings &settings,bool settingsReady);
// Boot-only stack/settings preparation, with no advertising or payload policy
// activation. This enables the ESP controller entropy source before PKI opens.
// Main publishes final application readiness through requestConfiguration().
int prepareBoot(uint32_t node,const mt::BluetoothSettings &settings,bool settingsReady);
// RAM-only staging; safe while the application service mutex is held. poll()
// applies changes outside that mutex and closes the previous session first.
bool requestConfiguration(const mt::BluetoothSettings &settings,bool settingsReady);
// Single local-BLE Admin transaction. begin runs inside validated dispatch;
// journal work runs only after dispatch returns. All APIs below are RAM-only.
bool beginAdmin(uint32_t token);
bool adminBusy();
bool stageAdminResult(const uint8_t *framed,size_t n,uint32_t token);
// True means next policy was accepted, not that the peer received the result.
bool finishAdminConfiguration(const mt::BluetoothSettings &settings,bool settingsReady,
                              uint32_t token,bool replyAllowed);
bool ready();
#if defined(CONFIG_MT_PAXCOUNTER_WIFI_ESP32)
bool paxRadioIdle();
#endif
bool connected();
bool disconnected();
uint32_t session();
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
bool stockImportLink(uint32_t token);
bool prepareHeldImportEntropy();
#endif
// Ephemeral last-attempt diagnostics, containing no peer identity or key.
SecurityStatus security();
BlePairingEvent::Snapshot pairing();
#if defined(CONFIG_MT_BLE_DIAGNOSTICS)
// Optional local-only, RAM-only pairing-stage report.
size_t diagnostics(char *out,size_t capacity);
#endif
bool receive(Packet &p);
// The callback runs under the session mutex. Call only with the service mutex
// already held; connection callbacks must never acquire the service mutex.
bool dispatch(const Packet &p,void (*callback)(const Packet &,void *),void *context);
bool send(const uint8_t *framed,size_t n,uint32_t session,bool idleOnly=false);
// Main-owned ingress admission. Closing blocks new ToRadio and DFU writes but
// preserves packets already accepted into the owner queue. The gate survives
// BLE session reset/reconnect; only an explicit reopen changes it back.
void setIngressOpen(bool open);
unsigned pendingInput();
#if defined(CONFIG_MT_BLE_LOG_RADIO)
struct LogRadioValue {uint8_t size=0;uint8_t bytes[64]{};};
void setLogRadioEnabled(bool enabled);
uint32_t logRadioSession();
void publishLogRadio(uint32_t token,const LogRadioValue &value);
#endif
void poll();
// Buttonless Nordic DFU only. Main publishes a quiescence/admission window;
// the GATT callback merely records an encrypted, indicated session request.
// takeDfuRequest runs under the normal Service -> session lock order and never
// performs a reset or storage operation. Unsupported builds always return false.
void setDfuWindow(bool ready);
bool takeDfuRequest();
// Install once at startup. The callback must be ISR-safe/nonblocking and must
// never take Service or Settings locks (for example k_sem_give only).
void setActivityWake(void (*wake)());
}
