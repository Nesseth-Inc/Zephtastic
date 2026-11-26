// SPDX-License-Identifier: GPL-3.0-only
#include "BleTransport.h"
// BEGIN M104 native Settings admission and per-session durable bond gate.
#include "BleSettingsAdmission.h"
#include "BleBondAcceptance.h"
#if defined(CONFIG_MT_BLE_NATIVE_STORE)
#include <zephyr/settings/settings.h>
extern "C" bool zt_bt_settings_identity_work_pending(void);
#endif
// END M104 native Settings admission.
#include "BluetoothPairingPolicy.h"
#include "BleReplyProgress.h"
#if defined(CONFIG_MT_BLE_DIAGNOSTICS)
#include "BleDiagnostics.h"
#endif
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <stdio.h>
#include <string.h>
// These invariants apply to ordinary PIN builds as well as diagnostic builds.
// Stack-owned bond/CCC persistence needs a separately qualified storage lane.
#if (defined(CONFIG_BT_SETTINGS) || defined(CONFIG_BT_BONDABLE)) && !defined(CONFIG_MT_BLE_NATIVE_STORE)
#error "Transient BLE must not enable unqualified stack settings or bonding"
#endif
#if defined(CONFIG_BT_MAX_CONN) && CONFIG_BT_MAX_CONN != 1
#error "BLE session ownership requires exactly one connection"
#endif
#if defined(CONFIG_BT_USE_DEBUG_KEYS) || defined(CONFIG_BT_SMP_LOG_LEVEL_DBG) || defined(CONFIG_BT_KEYS_LOG_LEVEL_DBG) || defined(CONFIG_BT_LOG_SNIFFER_INFO)
#error "PIN/key-bearing Bluetooth diagnostics are not permitted"
#endif
#if defined(CONFIG_MT_BLE_NATIVE_STORE)
#if !defined(CONFIG_BT_SETTINGS) || !defined(CONFIG_BT_BONDABLE_PER_CONNECTION) || defined(CONFIG_BT_SETTINGS_CCC_LAZY_LOADING) || (defined(CONFIG_LTO) && !defined(CONFIG_MT_LTO_RUNTIME))
#error "Native BLE store requires settings, per-connection bondability, eager CCC and no LTO"
#endif
#if CONFIG_BT_ID_MAX != 1
#error "MT native bond admission currently supports only BT_ID_DEFAULT"
#endif
#endif
namespace mt::ble {
#if defined(CONFIG_BOARD_HELTEC_T096)
// Minimum ATT MTU is 23; Prepare Write consumes five bytes of each PDU.
static_assert(CONFIG_BT_ATT_PREPARE_COUNT * (23 - 5) >= sizeof(Packet::bytes),
              "T096 must accept one complete ToRadio value at the minimum ATT MTU");
#endif
static BleBondAcceptance bondAcceptance;
#if defined(CONFIG_MT_BLE_NATIVE_STORE)
struct DurablePeer {bt_addr_le_t peer{};uint32_t createdEpoch=0;bool known=false;};
static DurablePeer durablePeers[CONFIG_BT_MAX_PAIRED];
static bool durablePeerKnown(const bt_addr_le_t *peer,uint32_t excludeEpoch=0) {
  if(peer)for(const auto &entry:durablePeers)if(entry.known&&(!excludeEpoch||entry.createdEpoch!=excludeEpoch)&&!bt_addr_le_cmp(peer,&entry.peer))return true;
  return false;
}
static bool rememberDurablePeer(const bt_addr_le_t *peer,uint32_t epoch=0) {
  if(durablePeerKnown(peer))return true;
  for(auto &entry:durablePeers)if(!entry.known){entry.peer=*peer;entry.createdEpoch=epoch;entry.known=true;return true;}
  return false;
}
static void bootBond(const bt_bond_info *info,void *) {
  if(!rememberDurablePeer(&info->addr))store::fail(-ENOMEM);
}
#endif
static bool nativePayloadAllowed() {
#if defined(CONFIG_MT_BLE_NATIVE_STORE)
  return store::payloadAllowed();
#else
  return true;
#endif
}
static bool nativeStoreHealthy() {
#if defined(CONFIG_MT_BLE_NATIVE_STORE)
  return store::healthy();
#else
  return true;
#endif
}
static bool resolvePolicy(const mt::BluetoothSettings &settings,bool display,BluetoothPairingPlan &plan) {
#if defined(CONFIG_MT_BLE_NATIVE_STORE)
  // This resolves intent; nativePayloadAllowed separately gates actual readiness.
  return resolve(settings,display,true,plan);
#else
  return resolveTransient(settings,display,plan);
#endif
}
static bt_uuid_128 serviceUuid=BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x6ba1b218,0x15a8,0x461f,0x9fa8,0x5dcae273eafd));
static bt_uuid_128 toUuid=BT_UUID_INIT_128(BT_UUID_128_ENCODE(0xf75c76d2,0x129e,0x4dad,0xa1dd,0x7866124401e7));
static bt_uuid_128 fromUuid=BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x2c55e69e,0x4993,0x11ed,0xb878,0x0242ac120002));
static bt_uuid_128 numUuid=BT_UUID_INIT_128(BT_UUID_128_ENCODE(0xed9da18c,0xa800,0x4f66,0xa670,0xaa7547e34453));
#ifndef MT_BLE_OUT_QUEUE_DEPTH
#if defined(CONFIG_BOARD_HELTEC_T096)
#define MT_BLE_OUT_QUEUE_DEPTH 8
#else
#define MT_BLE_OUT_QUEUE_DEPTH 64
#endif
#endif
static_assert(MT_BLE_OUT_QUEUE_DEPTH>=2&&MT_BLE_OUT_QUEUE_DEPTH<=64,
              "MT_BLE_OUT_QUEUE_DEPTH must be between 2 and 64");
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
// The underlying k_msgq retains dequeued bytes in its backing array. Keep the
// same eight-packet queue and clear each slot while the session mutex is held.
static Packet inputSlots[8]{};
static unsigned inputFirst=0,inputCount=0;
static void wipePacket(Packet &p){
  volatile uint8_t *bytes=reinterpret_cast<volatile uint8_t *>(&p);
  for(size_t i=0;i<sizeof(p);++i)bytes[i]=0;
}
#else
K_MSGQ_DEFINE(toQueue,sizeof(Packet),8,4);
#endif
K_MSGQ_DEFINE(fromQueue,sizeof(Packet),MT_BLE_OUT_QUEUE_DEPTH,4);
K_MUTEX_DEFINE(readLock);
static Packet reading{};
static atomic_t link,ended,changed,counter,restartAdvertising;
static uint32_t counterTicket;
// Called with the session mutex held. Each change, including a CCC refresh
// with the same counter, gets a distinct ticket. Zero means nothing pending.
static void markCounterChanged(){
  if(!++counterTicket)++counterTicket;
  atomic_set(&changed,counterTicket);
}
// All policy/session state is protected by SessionLock. Configuration staging
// never enters the Bluetooth stack or persistent settings subsystem.
static bool available=false,stackReady=false,enableAttempted=false,authRegistered=false;
static int enableResult=0;
static bool policyReady=false,configurationPending=false,stagedReady=false;
static bool sessionAuthorized=false,disconnectRequested=false;
static bool waitingForNoPinEncryption=false;
// Protected by SessionLock. It is intentionally independent of BLE session
// reset so reconnect cannot reopen application ingress implicitly.
static bool ingressOpen=true;
#if defined(CONFIG_MT_SIGNED_BLE_OTA)
static bool otaActive=false;
#endif
static mt::BluetoothSettings configuredSettings,stagedSettings,sessionSettings;
static BluetoothPairingPlan configuredPlan,sessionPlan;
static BlePairingEvent pairingEvent;
static uint32_t generation=0;
static bt_conn *activeConnection=nullptr;
static SecurityStatus securityStatus;
static void (*activityWake)()=nullptr;
static void wakeOwner(){if(activityWake)activityWake();}
// Dedicated result lane: ordinary queues stay closed from acceptance through
// journal verification, bounded reply reads and application of the next policy.
static bool adminBlocked=false,adminCommitting=false,adminNextReady=false,adminDrained=false,adminClockInvalid=false,adminSecurityInvalid=false;
static uint32_t adminToken=0;
static Packet adminReply{};
static mt::BluetoothSettings adminNextSettings;
static BleReplyProgress adminProgress;
static uint64_t adminStart=0,adminDeadline=0,adminDrainedAt=0,adminLastObserved=0;
static constexpr uint32_t AdminReplyMs=3000,AdminDrainGraceMs=100;
#if defined(CONFIG_MT_BLE_DIAGNOSTICS)
struct DiagnosticState {
  uint32_t start=0,rxCount=0,txCount=0,incomplete=0,rxAt=0,txAt=0;
  uint32_t rxMask=0,txMask=0,complete=0,failed=0,security=0,fromNum=0;
  int txResult=0;
  uint8_t rxLast=0,txLast=0,reason=0;bool bonded=false;
  bool seen=false,allowed=false,healthy=false,storeFailed=false,waiting=false,active=false,rejected=false;
  uint8_t connectError=0,disconnectReason=0,level=0,securityError=0;
  int bondingResult=0,overlayResult=0,requestResult=0;
};
static DiagnosticState diagnostic;
static void increment(uint32_t &value){if(value!=UINT32_MAX)++value;}
static uint8_t safeOpcode(uint8_t opcode){return opcode>=1&&opcode<=14?opcode:0;}
#endif
// Zephyr mutexes are recursive: dispatch can synchronously emit a response.
struct SessionLock {
  SessionLock(){k_mutex_lock(&readLock,K_FOREVER);
#if defined(CONFIG_MT_BLE_NATIVE_STORE)
    store::sessionEntered();
#endif
  }
  ~SessionLock(){
#if defined(CONFIG_MT_BLE_NATIVE_STORE)
    store::sessionLeft();
#endif
    k_mutex_unlock(&readLock);
  }
};
void setActivityWake(void (*wake)()){
  SessionLock lock;
  // This is a startup binding, not a replaceable callback owned by a peer.
  if(!activityWake&&wake)activityWake=wake;
}
#if defined(CONFIG_MT_BLE_LOG_RADIO)
static bt_uuid_128 logUuid=BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x5a3d6e49,0x06e6,0x4423,0x9944,0xe9de8cdf9547));
static constexpr size_t LogQueueDepth=4;
static LogRadioValue logValues[LogQueueDepth],logRead;
// Absolute cursors use unsigned wraparound. Producers serialize under
// readLock. Only a matching oldest ticket can be retired by the worker.
static uint32_t logHead=0;
static atomic_t logTail;
static uint32_t logFence=0;
static bool logEnabled=false;
static void clearLogRadio(){
  for(auto &value:logValues)value={};
  logRead={};atomic_set(&logTail,logHead);++logFence;
}
#endif
static void clearSession() {
  // A Read Blob at nonzero offset must never expose the previous connection's
  // retained packet. Serialize clearing with all offset-based reads.
  k_mutex_lock(&readLock,K_FOREVER);
  // Keep already-accepted inbound packets for the owner to account/drain.
  // Their session tokens prevent dispatch into a later BLE connection.
  reading={};k_msgq_purge(&fromQueue);
#if defined(CONFIG_MT_BLE_LOG_RADIO)
  clearLogRadio();
#endif
  atomic_set(&changed,0);pairingEvent.begin(0);
  k_mutex_unlock(&readLock);
}
static bt_security_t requiredSecurity() {
  return sessionPlan.security==Security::Authenticated?BT_SECURITY_L3:BT_SECURITY_L2;
}
static bool secured(bt_conn *conn) {
  // bt_conn_get_security is the nonblocking level accessor (no SMP request).
  // In particular, L2 characteristic permissions alone do not admit PIN-mode
  // application traffic. dispatch can synchronously call send under this lock.
  return nativeStoreHealthy()&&bondAcceptance.allowed()&&conn&&conn==activeConnection&&atomic_get(&link)&&sessionAuthorized&&
    policyReady&&!configurationPending&&sessionPlan.advertise&&
    securityStatus.error==0&&securityStatus.level>=requiredSecurity()&&
    bt_conn_get_security(conn)>=requiredSecurity();
}
static bool permitted(bt_conn *conn) {
#if defined(CONFIG_MT_SIGNED_BLE_OTA)
  if(otaActive)return false;
#endif
  return nativePayloadAllowed()&&!adminBlocked&&secured(conn);
}
#if defined(CONFIG_MT_SIGNED_BLE_OTA)
bool otaPolicyReady(){
  SessionLock lock;
  return stackReady&&available&&policyReady&&!configurationPending&&!adminBlocked&&
    configuredPlan.advertise&&nativePayloadAllowed();
}
bool beginOta(){
  SessionLock lock;
  if(ingressOpen||otaActive||!otaPolicyReady())return false;
  otaActive=true;clearSession();return true;
}
bt_conn *otaDisconnectTarget(uint32_t &epoch){
  SessionLock lock;
  epoch=otaActive&&activeConnection?generation:0;
  return epoch?bt_conn_ref(activeConnection):nullptr;
}
uint32_t otaSession(bt_conn *conn,uint32_t retained){
  SessionLock lock;
  if(!otaActive||!secured(conn))return 0;
  return (nativePayloadAllowed()||(retained&&retained==generation))?generation:0;
}
int otaAccess(uint32_t token,bt_conn **peer){
  SessionLock lock;
  if(peer)*peer=nullptr;
  if(!otaActive||!token||token!=generation||!secured(activeConnection))return -EACCES;
  if(!nativePayloadAllowed())return -EAGAIN;
  if(peer)*peer=bt_conn_ref(activeConnection);
  return 0;
}
#endif
static void clearAdmin() {
  adminBlocked=false;adminCommitting=false;adminNextReady=false;adminDrained=false;
  adminToken=0;adminReply={};adminNextSettings={};adminProgress.reset();
  adminStart=adminDeadline=adminDrainedAt=adminLastObserved=0;adminClockInvalid=false;adminSecurityInvalid=false;
}
static bool adminSecured(bt_conn *conn) {
  if(!conn||conn!=activeConnection||generation!=adminToken||adminSecurityInvalid)return false;
  if(!secured(conn)){adminSecurityInvalid=true;return false;}
  return true;
}
static bool adminTimeValid(uint64_t now) {
  if(adminClockInvalid||now<adminStart||now<adminLastObserved||now>=adminDeadline){adminClockInvalid=true;return false;}
  adminLastObserved=now;return true;
}
static bool adminReadable(bt_conn *conn,uint64_t now) {
  if(!adminBlocked||adminCommitting||!adminReply.length||
     !adminSecured(conn)||!adminTimeValid(now))return false;
  const auto state=adminProgress.poll(now);
  return state==BleReplyProgress::State::Waiting||
    (state==BleReplyProgress::State::Drained&&adminDrained&&now>=adminDrainedAt&&now-adminDrainedAt<AdminDrainGraceMs);
}
static void queueAdminPolicy() {
  stagedSettings=adminNextSettings;stagedReady=adminNextReady;configurationPending=true;
  adminReply={};adminProgress.reset();adminCommitting=false;adminDrained=false;
  clearSession();
}
static ssize_t writeTo(bt_conn *conn,const bt_gatt_attr *,const void *buf,uint16_t len,uint16_t offset,uint8_t flags) {
  SessionLock lock;
  // Check before PREPARE acceptance too. The same mutex serializes this gate
  // with callbacks, so no write can slip across a completed close operation.
  if(!ingressOpen)return BT_GATT_ERR(BT_ATT_ERR_AUTHENTICATION);
  if(!permitted(conn))return BT_GATT_ERR(BT_ATT_ERR_AUTHENTICATION);
  if(size_t(offset)+len>512)return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
  if(flags&BT_GATT_WRITE_FLAG_PREPARE)return 0;
  // Zephyr reassembles prepared long writes before EXECUTE callback.
  if(offset||!len)return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
  Packet p{};p.session=generation;p.length=len;memcpy(p.bytes,buf,len);
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
  p.stockImportAuthenticated=stockImportLink(generation);
  struct ClearPacket {Packet &value;~ClearPacket(){wipePacket(value);}} clearPacket{p};
  if(inputCount==ARRAY_SIZE(inputSlots))return BT_GATT_ERR(BT_ATT_ERR_INSUFFICIENT_RESOURCES);
  inputSlots[(inputFirst+inputCount)%ARRAY_SIZE(inputSlots)]=p;++inputCount;
#else
  if(k_msgq_put(&toQueue,&p,K_NO_WAIT))return BT_GATT_ERR(BT_ATT_ERR_INSUFFICIENT_RESOURCES);
#endif
  wakeOwner();
  return len;
}
static ssize_t readFrom(bt_conn *conn,const bt_gatt_attr *attr,void *buf,uint16_t len,uint16_t offset) {
  SessionLock lock;
  if(adminBlocked) {
    const uint64_t now=uint64_t(k_uptime_get());
    if(!adminReadable(conn,now))return BT_GATT_ERR(BT_ATT_ERR_AUTHENTICATION);
    if(offset>adminReply.length||offset>adminProgress.covered())return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    const size_t count=adminReply.length-offset<len?adminReply.length-offset:len;
    if(count&&adminProgress.state()==BleReplyProgress::State::Waiting) {
      if(!adminProgress.read(adminToken,offset,count,now))return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
      if(adminProgress.state()==BleReplyProgress::State::Drained){adminDrained=true;adminDrainedAt=now;}
    }
    return bt_gatt_attr_read(conn,attr,buf,len,offset,adminReply.bytes,adminReply.length);
  }
  if(!permitted(conn))return BT_GATT_ERR(BT_ATT_ERR_AUTHENTICATION);
  if(!offset) {reading.length=0;(void)k_msgq_get(&fromQueue,&reading,K_NO_WAIT);}
  const auto n=bt_gatt_attr_read(conn,attr,buf,len,offset,reading.bytes,reading.length);
  return n;
}
static ssize_t readNum(bt_conn *conn,const bt_gatt_attr *attr,void *buf,uint16_t len,uint16_t offset) {
  SessionLock lock;
  if(!(adminBlocked?adminReadable(conn,uint64_t(k_uptime_get())):permitted(conn)))return BT_GATT_ERR(BT_ATT_ERR_AUTHENTICATION);
#if defined(CONFIG_MT_BLE_DIAGNOSTICS)
  increment(diagnostic.fromNum);
#endif
  uint8_t n[4];sys_put_le32(atomic_get(&counter),n);return bt_gatt_attr_read(conn,attr,buf,len,offset,n,sizeof(n));
}
#if defined(CONFIG_MT_BLE_LOG_RADIO)
void setLogRadioEnabled(bool enabled) {
  SessionLock lock;
  if(logEnabled!=enabled){clearLogRadio();logEnabled=enabled;}
}
static bool logAllowed(bt_conn *conn){return logEnabled&&ingressOpen&&permitted(conn);}
uint32_t logRadioSession(){
  SessionLock lock;return logAllowed(activeConnection)?generation:0;
}
void publishLogRadio(uint32_t token,const LogRadioValue &value){
  SessionLock lock;
  if(token&&token==generation&&value.size&&value.size<=sizeof(value.bytes)&&logAllowed(activeConnection)){
    const uint32_t oldest=uint32_t(atomic_get(&logTail));
    if(uint32_t(logHead-oldest)==LogQueueDepth)
      (void)atomic_cas(&logTail,oldest,oldest+1);
    logValues[logHead%LogQueueDepth]=value;++logHead;
  }
}
static ssize_t readLog(bt_conn *conn,const bt_gatt_attr *attr,void *buf,uint16_t len,uint16_t offset){
  SessionLock lock;
  if(!ingressOpen||!permitted(conn))return BT_GATT_ERR(BT_ATT_ERR_AUTHENTICATION);
  if(!offset)logRead=logValues[uint32_t(logHead-1)%LogQueueDepth];
  return bt_gatt_attr_read(conn,attr,buf,len,offset,logRead.bytes,logRead.size);
}
#endif
// The stack owns CCC writes. They only request a counter notification; they
// never grant payload access or extend a transaction deadline.
static void ccc(const bt_gatt_attr *,uint16_t) {SessionLock lock;if(permitted(activeConnection)||adminReadable(activeConnection,uint64_t(k_uptime_get())))markCounterChanged();}
BT_GATT_SERVICE_DEFINE(meshService,
 BT_GATT_PRIMARY_SERVICE(&serviceUuid),
 BT_GATT_CHARACTERISTIC(&toUuid.uuid,BT_GATT_CHRC_WRITE,BT_GATT_PERM_WRITE_ENCRYPT|BT_GATT_PERM_PREPARE_WRITE,nullptr,writeTo,nullptr),
 BT_GATT_CHARACTERISTIC(&fromUuid.uuid,BT_GATT_CHRC_READ,BT_GATT_PERM_READ_ENCRYPT,readFrom,nullptr,nullptr),
 BT_GATT_CHARACTERISTIC(&numUuid.uuid,BT_GATT_CHRC_READ|BT_GATT_CHRC_NOTIFY,BT_GATT_PERM_READ_ENCRYPT,readNum,nullptr,nullptr),
 BT_GATT_CCC(ccc,BT_GATT_PERM_READ_ENCRYPT|BT_GATT_PERM_WRITE_ENCRYPT)
#if defined(CONFIG_MT_BLE_LOG_RADIO)
 ,BT_GATT_CHARACTERISTIC(&logUuid.uuid,BT_GATT_CHRC_READ|BT_GATT_CHRC_NOTIFY,BT_GATT_PERM_READ_ENCRYPT,readLog,nullptr,nullptr)
 ,BT_GATT_CCC(nullptr,BT_GATT_PERM_READ_ENCRYPT|BT_GATT_PERM_WRITE_ENCRYPT)
#endif
);
#if defined(CONFIG_MT_NORDIC_DFU)
// Stock nRF52 buttonless service (Adafruit BSD-licensed BLEDfuSecure protocol).
// UUID/opcode compatibility only; no SoftDevice peer-data structure is written
// at a hardcoded RAM address in this Zephyr application.
static bt_uuid_16 dfuUuid=BT_UUID_INIT_16(0xfe59);
static bt_uuid_128 dfuControlUuid=BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x8ec90003,0xf315,0x4f60,0x9fb8,0x838830daea50));
static bool dfuWindow=false,dfuInFlight=false,dfuConfirmed=false;
static uint32_t dfuToken=0;
static uint64_t dfuRequestedAt=0;
static uint8_t dfuReply[]={0x20,0x01,0x01};
static bt_gatt_indicate_params dfuIndication{};
static ssize_t writeDfu(bt_conn *,const bt_gatt_attr *,const void *,uint16_t,uint16_t,uint8_t);
BT_GATT_SERVICE_DEFINE(dfuService,
 BT_GATT_PRIMARY_SERVICE(&dfuUuid),
 BT_GATT_CHARACTERISTIC(&dfuControlUuid.uuid,BT_GATT_CHRC_WRITE|BT_GATT_CHRC_INDICATE,
   BT_GATT_PERM_WRITE_ENCRYPT,nullptr,writeDfu,nullptr),
 BT_GATT_CCC(nullptr,BT_GATT_PERM_READ_ENCRYPT|BT_GATT_PERM_WRITE_ENCRYPT)
);
static void dfuIndicated(bt_conn *conn,bt_gatt_indicate_params *,uint8_t error){
  SessionLock lock;
  // Confirmation is bound to the original live encrypted session. A later
  // normal disconnect can precede Main's poll; a new session cannot inherit it.
  const int64_t now=k_uptime_get();
  dfuConfirmed=!error&&ingressOpen&&dfuWindow&&dfuToken==generation&&permitted(conn)&&
    now>=0&&uint64_t(now)>=dfuRequestedAt&&uint64_t(now)-dfuRequestedAt<2000;
  if(dfuConfirmed)wakeOwner();
}
static void dfuDestroyed(bt_gatt_indicate_params *){SessionLock lock;dfuInFlight=false;}
static ssize_t writeDfu(bt_conn *conn,const bt_gatt_attr *,const void *value,
                        uint16_t length,uint16_t offset,uint8_t flags){
  SessionLock lock;
  if(!ingressOpen||!permitted(conn))return BT_GATT_ERR(BT_ATT_ERR_AUTHENTICATION);
  if(offset)return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
  if(flags||length!=1||!value||*static_cast<const uint8_t *>(value)!=1)
    return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
  if(!dfuWindow||dfuInFlight||dfuConfirmed)return BT_GATT_ERR(BT_ATT_ERR_INSUFFICIENT_RESOURCES);
  if(!bt_gatt_is_subscribed(conn,&dfuService.attrs[2],BT_GATT_CCC_INDICATE))
    return BT_GATT_ERR(BT_ATT_ERR_CCC_IMPROPER_CONF);
  const int64_t now=k_uptime_get();if(now<0)return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
  dfuToken=generation;dfuRequestedAt=uint64_t(now);dfuInFlight=true;dfuConfirmed=false;
  dfuIndication={};dfuIndication.attr=&dfuService.attrs[2];dfuIndication.data=dfuReply;
  dfuIndication.len=sizeof(dfuReply);dfuIndication.func=dfuIndicated;dfuIndication.destroy=dfuDestroyed;
  if(bt_gatt_indicate(conn,&dfuIndication)){dfuInFlight=false;return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);}
  return length;
}
#endif
void setDfuWindow(bool ready){
#if defined(CONFIG_MT_NORDIC_DFU)
  SessionLock lock;dfuWindow=ready;
#else
  (void)ready;
#endif
}
bool takeDfuRequest(){
#if defined(CONFIG_MT_NORDIC_DFU)
  SessionLock lock;
  if(!dfuConfirmed)return false;
  const int64_t now=k_uptime_get();
  const bool allowed=ingressOpen&&dfuWindow&&nativeStoreHealthy()&&policyReady&&!configurationPending&&
    !securityStatus.error&&(!activeConnection||permitted(activeConnection))&&
    dfuToken==generation&&now>=0&&
    uint64_t(now)>=dfuRequestedAt&&uint64_t(now)-dfuRequestedAt<2000;
  dfuConfirmed=false;if(allowed)dfuWindow=false;return allowed;
#else
  return false;
#endif
}
static void onPasskeyDisplay(bt_conn *conn,unsigned int passkey) {
  SessionLock lock;
  if(conn!=activeConnection||!atomic_get(&link)||!sessionAuthorized||
     configurationPending||!policyReady||!sessionPlan.displayPasskey||
     sessionPlan.passkeySource!=PasskeySource::Random)return;
  (void)pairingEvent.publishPasskey(generation,passkey);
}
static uint32_t applicationPasskey(bt_conn *conn) {
  SessionLock lock;
  // A staged policy never substitutes a new PIN in an existing SMP exchange.
  if(conn==activeConnection&&atomic_get(&link)&&sessionAuthorized&&
     sessionPlan.passkeySource==PasskeySource::Fixed)return sessionSettings.fixedPin;
  return BT_PASSKEY_RAND;
}
static void onAuthenticationCancelled(bt_conn *conn) {
  SessionLock lock;
  if(conn!=activeConnection||!atomic_get(&link))return;
  (void)pairingEvent.clear(generation);sessionAuthorized=false;disconnectRequested=true;
}
static const bt_conn_auth_cb noPinCallbacks={.cancel=onAuthenticationCancelled};
static const bt_conn_auth_cb pinCallbacks={
  .passkey_display=onPasskeyDisplay,.cancel=onAuthenticationCancelled,
  .app_passkey=applicationPasskey
};
static void onConnected(bt_conn *conn,uint8_t error) {
  if(error){
#if defined(CONFIG_MT_BLE_DIAGNOSTICS)
    {SessionLock lock;diagnostic={};diagnostic.start=k_uptime_get_32();diagnostic.seen=true;diagnostic.connectError=error;}
#endif
    atomic_set(&restartAdvertising,1);return;
  }
  // Keep the connection alive for later poll operations; references are atomic
  // lifetime operations, not SMP/transport work.
  bt_conn *owned=bt_conn_ref(conn);
  bool loadedBond=false,bondable=false;
#if defined(CONFIG_MT_BLE_NATIVE_STORE)
  {SessionLock lock;loadedBond=durablePeerKnown(bt_conn_get_dst(conn));}
#endif
  uint32_t token=0;bt_security_t required=BT_SECURITY_L2;
  const bt_conn_auth_cb *authentication=&noPinCallbacks;
  bool reject=false;
  {
    SessionLock lock;
    if(activeConnection||adminBlocked)reject=true;
    else {
      clearSession();if(!++generation)++generation;
      activeConnection=owned;owned=nullptr;atomic_set(&link,1);
      wakeOwner();
      token=generation;pairingEvent.begin(token);
      const bool nativeAllowed=nativePayloadAllowed();
      sessionAuthorized=nativeAllowed&&stackReady&&available&&policyReady&&!configurationPending&&configuredPlan.advertise;
      sessionSettings=configuredSettings;sessionPlan=sessionAuthorized?configuredPlan:BluetoothPairingPlan{};
      waitingForNoPinEncryption=sessionAuthorized&&sessionSettings.mode==mt::BluetoothSettings::NoPin;
      reject=!sessionAuthorized;disconnectRequested=reject;
      securityStatus={true,!reject,reject?-EACCES:0,BT_SECURITY_L1,0};
      required=requiredSecurity();bondable=sessionPlan.bondable;
      bondAcceptance.begin(token,bondable,loadedBond);
      if(sessionPlan.passkeySource!=PasskeySource::None)authentication=&pinCallbacks;
#if defined(CONFIG_MT_BLE_DIAGNOSTICS)
      diagnostic={};diagnostic.start=k_uptime_get_32();
      diagnostic.seen=true;diagnostic.allowed=nativeAllowed;diagnostic.healthy=nativeStoreHealthy();diagnostic.rejected=reject;diagnostic.level=BT_SECURITY_L1;
#if defined(CONFIG_MT_BLE_NATIVE_STORE)
      diagnostic.storeFailed=store::failed();diagnostic.waiting=store::waiting();diagnostic.active=store::active();
#endif
#endif
    }
  }
  if(owned)bt_conn_unref(owned);
  if(reject){(void)bt_conn_disconnect(conn,BT_HCI_ERR_REMOTE_USER_TERM_CONN);return;}
  // Install IO capabilities before any security request. Each connection gets
  // its own immutable mode; globally advertising DisplayOnly would turn NoPin
  // into an unintended passkey exchange. No PIN mode falls back from L3 to L2.
  int bonding=0;
#if defined(CONFIG_MT_BLE_NATIVE_STORE)
  bonding=bt_conn_set_bondable(conn,bondable);
  if(bonding==-EALREADY)bonding=0;
#else
  (void)bondable;
#endif
  const int overlaid=bonding?bonding:bt_conn_auth_cb_overlay(conn,authentication);
  const int result=overlaid?overlaid:bt_conn_set_security(conn,required);
  {
    SessionLock lock;
    if(activeConnection==conn&&generation==token){
      securityStatus.request=result;securityStatus.pending=false;
#if defined(CONFIG_MT_BLE_DIAGNOSTICS)
      // Chain results: a prior error can skip a later native API call.
      diagnostic.bondingResult=bonding;diagnostic.overlayResult=overlaid;diagnostic.requestResult=result;
#endif
      if(overlaid||(result&&result!=-EALREADY)){
        sessionAuthorized=false;disconnectRequested=true;(void)pairingEvent.clear(token);
      }
    }
  }
}
static void onDisconnected(bt_conn *conn,uint8_t reason) {
  (void)reason;
  bt_conn *release=nullptr;
  {
    SessionLock lock;
    if(conn!=activeConnection)return;
#if defined(CONFIG_MT_BLE_DIAGNOSTICS)
    diagnostic.disconnectReason=reason;
#endif
    if(adminBlocked)adminProgress.cancel(adminToken);
    atomic_set(&link,0);atomic_set(&ended,1);atomic_set(&restartAdvertising,1);clearSession();
    release=activeConnection;activeConnection=nullptr;sessionAuthorized=false;disconnectRequested=false;
    waitingForNoPinEncryption=false;
    sessionPlan={};sessionSettings={};bondAcceptance.begin(0,false,false);
    securityStatus.connected=false;securityStatus.pending=false;
    wakeOwner();
  }
  bt_conn_unref(release);
}
static void onSecurityChanged(bt_conn *conn,bt_security_t level,bt_security_err error) {
  SessionLock lock;
  if(conn!=activeConnection||!atomic_get(&link))return;
  if(adminBlocked&&generation==adminToken&&(error||level<requiredSecurity()))adminSecurityInvalid=true;
  // NO_PIN stores no bond. A phone may try its previous link key before it
  // pairs afresh on this connection. Keep GATT closed until security succeeds.
  const bool staleKey=waitingForNoPinEncryption&&level==BT_SECURITY_L1&&
      error==BT_SECURITY_ERR_PIN_OR_KEY_MISSING;
  if(!staleKey&&(error||level>=requiredSecurity()))waitingForNoPinEncryption=false;
  securityStatus.level=uint8_t(level);securityStatus.error=uint8_t(error);
#if defined(CONFIG_MT_BLE_LOG_RADIO)
  if(error||level<requiredSecurity())clearLogRadio();
#endif
#if defined(CONFIG_MT_BLE_NATIVE_STORE)
  if(!error&&level>=requiredSecurity()&&durablePeerKnown(bt_conn_get_dst(conn),generation))bondAcceptance.acceptLoaded();
#endif
  if(error&&!staleKey){sessionAuthorized=false;disconnectRequested=true;(void)pairingEvent.clear(generation);}
#if defined(CONFIG_MT_BLE_DIAGNOSTICS)
  increment(diagnostic.security);diagnostic.level=uint8_t(level);diagnostic.securityError=uint8_t(error);
#endif
}
static void onPairingComplete(bt_conn *conn,bool bonded) {
  SessionLock lock;if(conn!=activeConnection||!atomic_get(&link))return;
  bondAcceptance.paired(bonded);
  if(sessionPlan.bondable&&!bonded){sessionAuthorized=false;disconnectRequested=true;}
  (void)pairingEvent.clear(generation);
#if defined(CONFIG_MT_BLE_DIAGNOSTICS)
  increment(diagnostic.complete);diagnostic.bonded=bonded;
#else
  (void)bonded;
#endif
}
static void onPairingFailed(bt_conn *conn,bt_security_err reason) {
  SessionLock lock;if(conn!=activeConnection||!atomic_get(&link))return;
  (void)pairingEvent.clear(generation);sessionAuthorized=false;disconnectRequested=true;
  securityStatus.error=uint8_t(reason);
#if defined(CONFIG_MT_BLE_DIAGNOSTICS)
  increment(diagnostic.failed);diagnostic.reason=uint8_t(reason);
#endif
}
// Completion/failure clears the UI handoff in every build, not only diagnostics.
static bt_conn_auth_info_cb authCallbacks={.pairing_complete=onPairingComplete,.pairing_failed=onPairingFailed};
#if defined(CONFIG_MT_BLE_NATIVE_STORE)
static void onIdentityResolved(bt_conn *conn,const bt_addr_le_t *,const bt_addr_le_t *identity) {
  SessionLock lock;
  if(conn==activeConnection&&connected()&&durablePeerKnown(identity,generation))bondAcceptance.acceptLoaded();
}
#endif
BT_CONN_CB_DEFINE(callbacks)={.connected=onConnected,.disconnected=onDisconnected,
#if defined(CONFIG_MT_BLE_NATIVE_STORE)
 .identity_resolved=onIdentityResolved,
#endif
 .security_changed=onSecurityChanged};
static const uint8_t flags=BT_LE_AD_GENERAL|BT_LE_AD_NO_BREDR;
static const bt_data advertising[]={BT_DATA(BT_DATA_FLAGS,&flags,1),BT_DATA(BT_DATA_UUID128_ALL,serviceUuid.val,16)};
static int advertise() {
  const char *name=bt_get_name();const bt_data scan[]={BT_DATA(BT_DATA_NAME_COMPLETE,name,uint8_t(strlen(name)))};
  return bt_le_adv_start(BT_LE_ADV_CONN_FAST_1,advertising,2,scan,1);
}
bool adminBusy() {SessionLock lock;return adminBlocked;}
bool beginAdmin(uint32_t token) {
  SessionLock lock;
  if(!token||token!=generation||!permitted(activeConnection))return false;
  adminBlocked=true;adminCommitting=true;adminToken=token;
  adminReply={};adminProgress.reset();adminDrained=false;
  clearSession();return true;
}
bool stageAdminResult(const uint8_t *framed,size_t n,uint32_t token) {
  SessionLock lock;
  if(!adminBlocked||!adminCommitting||adminReply.length||token!=adminToken||token!=generation||
     !adminSecured(activeConnection)||!framed||n<=4||n>516||framed[0]!=0x94||framed[1]!=0xc3||
     (size_t(framed[2])<<8|framed[3])!=n-4)return false;
  adminReply.session=token;adminReply.length=n-4;memcpy(adminReply.bytes,framed+4,n-4);return true;
}
bool finishAdminConfiguration(const mt::BluetoothSettings &settings,bool settingsReady,uint32_t token,bool replyAllowed) {
  SessionLock lock;BluetoothPairingPlan plan;
  if(!adminBlocked||!adminCommitting||!token||token!=adminToken||
     (!resolvePolicy(settings,IS_ENABLED(CONFIG_MT_DISPLAY),plan)&&settingsReady))return false;
  adminNextSettings=settings;adminNextReady=settingsReady;adminCommitting=false;
  const uint64_t now=uint64_t(k_uptime_get());
  // A save failure may poison the journal or receive restoration. Even then
  // the original authenticated session can read its one truthful error result;
  // the unready authoritative policy remains closed after this bounded lane.
  if(replyAllowed&&token==generation&&adminSecured(activeConnection)&&adminReply.length&&
     adminProgress.begin(token,adminReply.length,now,AdminReplyMs)) {
    adminStart=adminLastObserved=now;adminDeadline=now+AdminReplyMs;adminClockInvalid=false;
    atomic_inc(&counter);markCounterChanged();
  }else queueAdminPolicy();
  return true;
}
bool requestConfiguration(const mt::BluetoothSettings &settings,bool settingsReady) {
  BluetoothPairingPlan plan;
  if(!resolvePolicy(settings,IS_ENABLED(CONFIG_MT_DISPLAY),plan)){
    if(!settingsReady){
      SessionLock lock;
      // Loss of authoritative settings always closes the gate, even when the
      // supplied desired bytes are corrupt. poll will stop the transport.
      clearAdmin();stagedSettings=settings;stagedReady=false;configurationPending=true;
      clearSession();if(activeConnection)pairingEvent.begin(generation);
    }
    return false;
  }
  SessionLock lock;
  if(!adminBlocked&&configurationPending&&stagedSettings.same(settings)&&stagedReady==settingsReady)return true;
  if(!adminBlocked&&!configurationPending&&policyReady==settingsReady&&configuredSettings.same(settings))return true;
  clearAdmin();
  stagedSettings=settings;stagedReady=settingsReady;configurationPending=true;
  // Close new traffic immediately and clear retained Read Blob bytes. Already
  // accepted inbound packets stay queued for owner accounting/drain. The active
  // session's policy/PIN remains immutable until disconnect.
  clearSession();if(activeConnection)pairingEvent.begin(generation);
  return true;
}
// Called only by init/poll outside serviceLock. A pending setting is committed
// to the runtime only after advertising stopped and the prior link closed.
static int applyConfiguration() {
  bt_conn *disconnect=nullptr;bool pending=false;
  {
    SessionLock lock;
    if(!stackReady)return -EAGAIN;
    if(adminBlocked&&!configurationPending) {
      if(adminCommitting)return 0;
      const uint64_t now=uint64_t(k_uptime_get());
      // A notification or queue operation is not a read. Only contiguous ATT
      // reads can start the short, fixed drain grace. Deadline/security loss
      // always wins, including a backwards clock.
      const auto progress=adminProgress.poll(now);
      const bool waiting=progress==BleReplyProgress::State::Waiting;
      const bool grace=progress==BleReplyProgress::State::Drained&&adminDrained&&
        now>=adminDrainedAt&&now-adminDrainedAt<AdminDrainGraceMs;
      if(adminSecured(activeConnection)&&adminTimeValid(now)&&(waiting||grace))return 0;
      queueAdminPolicy();
    }
    pending=configurationPending;
    if((pending||disconnectRequested)&&activeConnection)disconnect=bt_conn_ref(activeConnection);
  }
  int result=0;
  if(pending)result=bt_le_adv_stop();
  if(disconnect){
    const int ret=bt_conn_disconnect(disconnect,BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    bt_conn_unref(disconnect);
    if(ret&&ret!=-ENOTCONN&&ret!=-EALREADY)return ret;
  }
  if(result&&result!=-EALREADY)return result;
  // A request may have been staged while the disconnect operation ran. This
  // invocation can apply it only if it also stopped the old advertising set.
  if(!pending)return 0;
  {
    SessionLock lock;
    if(!configurationPending||activeConnection)return 0;
    BluetoothPairingPlan plan;
    const bool valid=resolvePolicy(stagedSettings,IS_ENABLED(CONFIG_MT_DISPLAY),plan);
    configuredSettings=stagedSettings;configuredPlan=valid&&stagedReady?plan:BluetoothPairingPlan{};
    policyReady=valid&&stagedReady;configurationPending=false;available=stackReady;
    clearAdmin();
    atomic_set(&restartAdvertising,configuredPlan.advertise?1:0);
  }
  return 0;
}
static int restartIfNeeded() {
  {
    SessionLock lock;
    if(!nativePayloadAllowed()||!stackReady||!policyReady||configurationPending||adminBlocked||activeConnection||!configuredPlan.advertise)return 0;
    if(!atomic_set(&restartAdvertising,0))return 0;
  }
  const int ret=advertise();
  {
    SessionLock lock;
    // If staging raced adv_start, the callback/data gates already reject it;
    // the next poll stops it before installing that new configuration.
    available=ret==0||ret==-EALREADY;
    if(ret&&ret!=-EALREADY)atomic_set(&restartAdvertising,1);
  }
  return ret==-EALREADY?0:ret;
}
static int enableController() {
  if(!enableAttempted){enableAttempted=true;enableResult=bt_enable(nullptr);}
  return enableResult;
}
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
bool prepareHeldImportEntropy() {
  // Called on the main thread during a verified, USB-only continuation.
  // The ESP entropy port needs the BT PHY. Do not load settings, register a
  // pairing policy, mark the transport ready or start advertising in Held.
  {
    SessionLock lock;
    if(ingressOpen||activeConnection||stackReady||policyReady||available)return false;
  }
  return enableController()==0;
}
#endif
static int initializeBoot(uint32_t node,const mt::BluetoothSettings &settings,bool settingsReady,bool activate) {
  const bool valid=requestConfiguration(settings,settingsReady);
  if(!valid){
    SessionLock lock;
    policyReady=false;configuredPlan={};available=false;
    stagedSettings=settings;stagedReady=false;configurationPending=true;
    clearSession();if(activeConnection)pairingEvent.begin(generation);
  }
  if(const int rc=enableController())return rc;
#if defined(CONFIG_MT_BLE_NATIVE_STORE)
  const int loaded=settings_load_subtree("bt");
  store::bootLoadComplete(loaded);if(loaded)return loaded;
  bt_foreach_bond(BT_ID_DEFAULT,bootBond,nullptr);
  if(store::failed())return -EIO;
#endif
  bt_set_bondable(false);
  if(!authRegistered){
    const int ret=bt_conn_auth_info_cb_register(&authCallbacks);if(ret)return ret;
    authRegistered=true;
  }
  char name[28];snprintf(name,sizeof(name),"ZephTastic ZT %04x",unsigned(node&0xffff));
  const int named=bt_set_name(name);if(named)return named;
  {SessionLock lock;stackReady=true;}
  if(!activate)return valid?0:-EINVAL;
  const int applied=applyConfiguration();if(applied)return applied;
  if(!valid)return -EINVAL;
  return restartIfNeeded();
}
int init(uint32_t node) {return init(node,mt::BluetoothSettings::defaults(),true);}
int init(uint32_t node,const mt::BluetoothSettings &settings,bool settingsReady) {
  return initializeBoot(node,settings,settingsReady,true);
}
int prepareBoot(uint32_t node,const mt::BluetoothSettings &settings,bool settingsReady) {
  return initializeBoot(node,settings,settingsReady,false);
}
bool ready(){SessionLock lock;return nativePayloadAllowed()&&available&&policyReady&&!configurationPending&&!adminBlocked&&configuredPlan.advertise;}
#if defined(CONFIG_MT_PAXCOUNTER_WIFI_ESP32)
bool paxRadioIdle(){
  SessionLock lock;
  return stackReady&&policyReady&&!configurationPending&&!adminBlocked&&!activeConnection&&
         !configuredSettings.enabled&&!configuredPlan.advertise;
}
#endif
bool connected(){return atomic_get(&link);}
bool disconnected(){return atomic_set(&ended,0);}
uint32_t session(){SessionLock lock;return connected()?generation:0;}
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
bool stockImportLink(uint32_t token){
  SessionLock lock;
  return token&&token==generation&&configuredSettings.enabled&&sessionSettings.same(configuredSettings)&&
    configuredSettings.mode==BluetoothSettings::FixedPin&&configuredSettings.fixedPin!=0&&configuredSettings.fixedPin!=123456&&
    !securityStatus.pending&&!securityStatus.error&&securityStatus.level>=BT_SECURITY_L3&&
    secured(activeConnection)&&bt_conn_get_security(activeConnection)>=BT_SECURITY_L3;
}
#endif
SecurityStatus security(){SessionLock lock;return securityStatus;}
BlePairingEvent::Snapshot pairing(){
  SessionLock lock;BlePairingEvent::Snapshot snapshot;
  if(!configurationPending&&policyReady&&sessionAuthorized&&atomic_get(&link))
    (void)pairingEvent.snapshot(generation,snapshot);
  return snapshot;
}
#if defined(CONFIG_MT_BLE_DIAGNOSTICS)
size_t diagnostics(char *out,size_t capacity) {
  if(!out||!capacity)return 0;
  SessionLock lock;
  const int n=snprintf(out,capacity,
    "ZT-BLE2 seen=%u live=%u allow=%u health=%u fail=%u wait=%u active=%u reject=%u conn_e=%u bond_r=%d overlay_r=%d request_r=%d disc=%u level=%u sec_e=%u pair_ok=%u bonded=%u pair_fail=%u pair_e=%u rx=%u tx=%u tx_r=%d num=%u age=%u",
    unsigned(diagnostic.seen),unsigned(activeConnection&&atomic_get(&link)),unsigned(diagnostic.allowed),unsigned(diagnostic.healthy),unsigned(diagnostic.storeFailed),unsigned(diagnostic.waiting),unsigned(diagnostic.active),unsigned(diagnostic.rejected),
    unsigned(diagnostic.connectError),diagnostic.bondingResult,diagnostic.overlayResult,diagnostic.requestResult,unsigned(diagnostic.disconnectReason),unsigned(diagnostic.level),unsigned(diagnostic.securityError),
    unsigned(diagnostic.complete),unsigned(diagnostic.bonded),unsigned(diagnostic.failed),unsigned(diagnostic.reason),unsigned(diagnostic.rxCount),unsigned(diagnostic.txCount),diagnostic.txResult,unsigned(diagnostic.fromNum),unsigned(k_uptime_get_32()-diagnostic.start));
  return n>0&&size_t(n)<capacity?size_t(n):0;
}
#endif
bool receive(Packet &p){SessionLock lock;
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
  if(!inputCount)return false;
  p=inputSlots[inputFirst];wipePacket(inputSlots[inputFirst]);inputFirst=(inputFirst+1)%ARRAY_SIZE(inputSlots);--inputCount;return true;
#else
  return k_msgq_get(&toQueue,&p,K_NO_WAIT)==0;
#endif
}
bool dispatch(const Packet &p,void (*callback)(const Packet &,void *),void *context) {
  SessionLock lock;
  if(!callback||!permitted(activeConnection)||!p.session||p.session!=generation)return false;
  callback(p,context);return true;
}
bool send(const uint8_t *framed,size_t n,uint32_t token,bool idleOnly) {
  SessionLock lock;
  if(!permitted(activeConnection)||!token||token!=generation)return false;
  if(!framed||n<=4||n>516||framed[0]!=0x94||framed[1]!=0xc3||
     (size_t(framed[2])<<8|framed[3])!=n-4)return false;
  Packet p{};p.session=generation;p.length=n-4;memcpy(p.bytes,framed+4,p.length);
  if((idleOnly && k_msgq_num_used_get(&fromQueue)) || k_msgq_put(&fromQueue,&p,K_NO_WAIT))return false;
  atomic_inc(&counter);markCounterChanged();return true;
}
void setIngressOpen(bool open){
  SessionLock lock;ingressOpen=open;
#if defined(CONFIG_MT_BLE_LOG_RADIO)
  if(!open)clearLogRadio();
#endif
}
unsigned pendingInput(){SessionLock lock;
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
  return inputCount;
#else
  return unsigned(k_msgq_num_used_get(&toQueue));
#endif
}
// Notification selection only reads session state and pins a connection.
// Never park Zephyr's shared system workqueue behind application dispatch.
struct NotifyLock {
  const bool locked=k_mutex_lock(&readLock,K_NO_WAIT)==0;
  ~NotifyLock(){if(locked)k_mutex_unlock(&readLock);}
};
static void notifyCounter(){
  bt_conn *conn;uint32_t value,ticket;
  {
    NotifyLock lock;
    if(!lock.locked||!(ticket=uint32_t(atomic_get(&changed))))return;
    if(!(adminBlocked?adminReadable(activeConnection,uint64_t(k_uptime_get())):permitted(activeConnection)))return;
    conn=bt_conn_ref(activeConnection);value=atomic_get(&counter);
  }
  uint8_t bytes[4];sys_put_le32(value,bytes);
  const int result=bt_gatt_notify(conn,&meshService.attrs[6],bytes,sizeof(bytes));
  bt_conn_unref(conn);
  if(result!=-ENOMEM&&result!=-EAGAIN)(void)atomic_cas(&changed,ticket,0);
}
#if defined(CONFIG_MT_BLE_LOG_RADIO)
static void notifyLogRadio(){
  // Only the single system work item accesses the backoff fields.
  static uint32_t failureFence,lastFailure;
  static bool backoff;
  bt_conn *conn;uint32_t oldest,fence;LogRadioValue value;
  {
    NotifyLock lock;
    if(!lock.locked||!logAllowed(activeConnection))return;
    oldest=uint32_t(atomic_get(&logTail));if(oldest==logHead)return;
    if(backoff&&failureFence==logFence&&uint32_t(k_uptime_get_32()-lastFailure)<250)return;
    backoff=false;
    value=logValues[oldest%LogQueueDepth];fence=logFence;conn=bt_conn_ref(activeConnection);
  }
  const auto *attr=&meshService.attrs[9];
  int result=0;
  if(bt_gatt_is_subscribed(conn,attr,BT_GATT_CCC_NOTIFY)){
    // A short bearer can still read the complete protobuf via Read Blob.
    const auto mtu=bt_gatt_get_mtu(conn);
    if(mtu>=3&&value.size<=mtu-3){
      bool allowed;
      {
        NotifyLock lock;
        allowed=lock.locked&&logFence==fence&&uint32_t(atomic_get(&logTail))==oldest&&logAllowed(conn);
      }
      if(!allowed){bt_conn_unref(conn);return;}
      result=bt_gatt_notify(conn,attr,value.bytes,value.size);
    }
  }
  bt_conn_unref(conn);
  if(result){failureFence=fence;lastFailure=k_uptime_get_32();backoff=true;}
  if(result!=-ENOMEM&&result!=-EAGAIN)(void)atomic_cas(&logTail,oldest,oldest+1);
}
#endif
static void notifyPending(k_work *){
  // The pinned Zephyr ATT allocator uses K_NO_WAIT in this specific context.
  // Main must remain free to feed the hardware watchdog even if ATT is full.
  notifyCounter();
#if defined(CONFIG_MT_BLE_LOG_RADIO)
  notifyLogRadio();
#endif
}
K_WORK_DEFINE(notificationWork,notifyPending);
void poll() {
  // Main calls poll outside serviceLock: SMP/advertising/disconnect can block.
#if defined(CONFIG_MT_BLE_NATIVE_STORE)
  {SessionLock lock;if(!stackReady)return;}
  if(!store::failed()) {
    bt_addr_le_t local[1];size_t count=1;bt_id_get(local,&count);
    if(count!=1)store::fail(-EIO);
    store::sealBootIfQuiet(zt_bt_settings_identity_work_pending());
  }
  if(store::failed()) {
    SessionLock lock;
    stagedSettings=configuredSettings;stagedReady=false;configurationPending=true;
    policyReady=false;available=false;sessionAuthorized=false;
    disconnectRequested=activeConnection!=nullptr;
  }
#endif
  (void)applyConfiguration();
  (void)restartIfNeeded();
  bool pending=atomic_get(&changed)!=0;
#if defined(CONFIG_MT_BLE_LOG_RADIO)
  {SessionLock lock;pending=pending||logHead!=uint32_t(atomic_get(&logTail));}
#endif
  if(pending)(void)k_work_submit(&notificationWork);
}
}

#if defined(CONFIG_MT_BLE_DIAGNOSTICS)
extern "C" uint32_t mt_ble_trace_token(bt_conn *conn) {
  using namespace mt::ble;SessionLock lock;
  return conn==activeConnection&&connected()?generation:0;
}
extern "C" void mt_ble_trace_rx(bt_conn *conn,uint8_t opcode,bool complete) {
  using namespace mt::ble;SessionLock lock;
  if(conn!=activeConnection||!connected())return;
  if(!complete){increment(diagnostic.incomplete);return;}
  opcode=safeOpcode(opcode);increment(diagnostic.rxCount);
  diagnostic.rxMask|=1u<<opcode;diagnostic.rxLast=opcode;diagnostic.rxAt=k_uptime_get_32()-diagnostic.start;
}
extern "C" void mt_ble_trace_tx(bt_conn *conn,uint32_t token,uint8_t opcode,int result) {
  using namespace mt::ble;SessionLock lock;
  if(conn!=activeConnection||!connected()||!token||token!=generation)return;
  opcode=safeOpcode(opcode);increment(diagnostic.txCount);
  diagnostic.txMask|=1u<<opcode;diagnostic.txLast=opcode;diagnostic.txAt=k_uptime_get_32()-diagnostic.start;diagnostic.txResult=result;
}
#endif

#if defined(CONFIG_MT_BLE_NATIVE_STORE)
extern "C" void zt_bt_key_delete_result(uint8_t id,const bt_addr_le_t *peer,int result) {
  using namespace mt::ble;
  if(result)store::fail(result);
  if(id!=BT_ID_DEFAULT||!peer)return;
  SessionLock lock;
  // Native SMP also clears temporary keys while pairing. That is not a
  // revocation of application bond authority. NoPin never depends on a bond.
  const bool wasDurable=durablePeerKnown(peer);
  for(auto &entry:durablePeers)if(entry.known&&!bt_addr_le_cmp(peer,&entry.peer))entry={};
  if(wasDurable&&sessionPlan.bondable&&activeConnection&&connected()&&!bt_addr_le_cmp(peer,bt_conn_get_dst(activeConnection))) {
    bondAcceptance.revoke();sessionAuthorized=false;disconnectRequested=true;
  }
}
extern "C" uint32_t zt_bt_key_store_begin(uint8_t id,const bt_addr_le_t *peer) {
  using namespace mt::ble;SessionLock lock;
  if(id!=BT_ID_DEFAULT||!peer||!activeConnection||!connected()||
     bt_addr_le_cmp(peer,bt_conn_get_dst(activeConnection)))return 0;
  return bondAcceptance.storeBegin();
}
extern "C" void zt_bt_key_store_end(uint8_t id,const bt_addr_le_t *peer,uint32_t token,int result) {
  using namespace mt::ble;
  if(result)store::fail(result);
  SessionLock lock;
  if(id!=BT_ID_DEFAULT||!peer||!activeConnection||!connected()||!token||token!=generation||
     bt_addr_le_cmp(peer,bt_conn_get_dst(activeConnection)))return;
  if(!result&&!rememberDurablePeer(peer,token)){store::fail(-ENOMEM);result=-ENOMEM;}
  bondAcceptance.storeEnd(token,result);
}
#endif
